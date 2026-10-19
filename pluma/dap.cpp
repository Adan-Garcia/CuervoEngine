#include "pluma/dap.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>

#include "pluma/bytecode.hpp"
#include "pluma/source.hpp"
#include "pluma/vm.hpp"

namespace pluma::dap {

namespace fs = std::filesystem;
using JsonValue = json::Value;

namespace {

constexpr std::chrono::milliseconds idle_interval{20};
constexpr std::int64_t thread_id = 1; // one VM thread; coroutines run on it

JsonValue str(std::string_view s) { return JsonValue::string(s); }
JsonValue num(std::int64_t i) { return JsonValue::integer(i); }

std::string_view command_of(const JsonValue& request) { return request.str_or("command"); }
const JsonValue& arguments_of(const JsonValue& request) {
    static const JsonValue none = JsonValue::object();
    const JsonValue* a = request.get("arguments");
    return a != nullptr && a->is_object() ? *a : none;
}

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

// `a.b[2].c` -> {"a", "b", "[2]", "c"}; empty on a syntax error.
std::vector<std::string> split_path(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    auto skip_space = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    };
    skip_space();
    bool want_name = true;
    while (i < text.size()) {
        if (want_name) {
            if (!is_ident_start(text[i])) return {};
            const std::size_t begin = i;
            while (i < text.size() && is_ident_char(text[i])) ++i;
            parts.emplace_back(text.substr(begin, i - begin));
            want_name = false;
        } else if (text[i] == '.') {
            ++i;
            want_name = true;
        } else if (text[i] == '[') {
            const std::size_t close = text.find(']', i);
            if (close == std::string_view::npos) return {};
            std::size_t index = 0;
            const auto r = std::from_chars(text.data() + i + 1, text.data() + close, index);
            if (r.ec != std::errc{} || r.ptr != text.data() + close) return {};
            parts.push_back(std::format("[{}]", index));
            i = close + 1;
        } else {
            return {};
        }
        skip_space();
    }
    if (want_name) return {};
    return parts;
}

} // namespace

Debugger::~Debugger() { detach(); }

// ------------------------------------------------------------ messages

void Debugger::send_response(const JsonValue& request, JsonValue body) {
    transport_.send(json::dump(JsonValue::object({
        {"seq", num(seq_++)},
        {"type", str("response")},
        {"request_seq", num(request.int_or("seq"))},
        {"success", JsonValue::boolean(true)},
        {"command", str(command_of(request))},
        {"body", std::move(body)},
    })));
}

void Debugger::send_error(const JsonValue& request, std::string_view message) {
    transport_.send(json::dump(JsonValue::object({
        {"seq", num(seq_++)},
        {"type", str("response")},
        {"request_seq", num(request.int_or("seq"))},
        {"success", JsonValue::boolean(false)},
        {"command", str(command_of(request))},
        {"message", str(message)},
        {"body", JsonValue::object({{"error", JsonValue::object({
                                                  {"id", num(1)},
                                                  {"format", str(message)},
                                                  {"showUser", JsonValue::boolean(false)},
                                              })}})},
    })));
}

void Debugger::send_event(std::string_view event, JsonValue body) {
    transport_.send(json::dump(JsonValue::object({
        {"seq", num(seq_++)},
        {"type", str("event")},
        {"event", str(event)},
        {"body", std::move(body)},
    })));
}

void Debugger::output(std::string_view text, std::string_view category) {
    send_event("output", JsonValue::object({{"category", str(category)}, {"output", str(text)}}));
}

void Debugger::exited(int exit_code) {
    send_event("exited", JsonValue::object({{"exitCode", num(exit_code)}}));
    send_event("terminated");
}

// ------------------------------------------------------------ lifecycle

std::optional<JsonValue> Debugger::wait_for_start(const std::function<std::string(const JsonValue&)>& validate) {
    phase_ = Phase::Starting;
    while (!configured_ && !terminate_) {
        if (transport_.closed()) return std::nullopt;
        std::optional<std::string> text = transport_.receive(idle_interval * 5);
        if (!text) continue;
        std::optional<JsonValue> msg = json::Parser(*text).parse();
        if (msg && command_of(*msg) == "launch" && !launch_args_) {
            const JsonValue& args = arguments_of(*msg);
            std::string error = validate ? validate(args) : std::string();
            if (!error.empty()) {
                send_error(*msg, error);
                return std::nullopt;
            }
            launch_args_ = args;
            stop_on_entry_ = args.bool_or("stopOnEntry", false);
            send_response(*msg);
            continue;
        }
        handle(*text);
    }
    if (terminate_ || !launch_args_) return std::nullopt;
    phase_ = Phase::Running;
    return launch_args_;
}

void Debugger::attach(VM& vm) {
    vm_ = &vm;
    sources_.clear(); // the old VM's SourceFiles are gone after a reload
    refs_.clear();
    vm.set_debug_hooks(this);
    if (!vm.protos.empty()) reverify(vm); // else on_code_loaded does it
}

void Debugger::on_code_loaded(VM& vm) {
    // Maybe a hot reload's candidate VM, not yet active: its code is what
    // runs next either way.
    reverify(vm);
}

void Debugger::reverify(const VM& vm) {
    // The client learns of moved or (un)verified breakpoints through
    // `breakpoint` events.
    for (auto& [file, bps] : breakpoints_) {
        verify(file, bps, &vm);
        for (const Breakpoint& bp : bps) {
            send_event("breakpoint", JsonValue::object({
                                         {"reason", str("changed")},
                                         {"breakpoint", JsonValue::object({
                                                            {"id", num(bp.id)},
                                                            {"verified", JsonValue::boolean(bp.verified)},
                                                            {"line", num(bp.line)},
                                                        })},
                                     }));
        }
    }
    rebuild_lines();
}

void Debugger::detach() {
    if (vm_ != nullptr && vm_->debug_hooks() == this) vm_->set_debug_hooks(nullptr);
    vm_ = nullptr;
    refs_.clear();
    sources_.clear();
}

void Debugger::poll() {
    while (std::optional<std::string> text = transport_.receive(std::chrono::milliseconds(0))) handle(*text);
    if (transport_.closed()) terminate_ = true;
}

// ------------------------------------------------------------ hooks

void Debugger::on_line(VM& vm) {
    if (terminate_) return;
    const bool entry = stop_on_entry_;
    const bool pause = pause_.exchange(false);
    if (!entry && !pause && step_ == Step::None && lines_.empty()) return; // the common case: run on
    const std::size_t depth = vm.frame_depth();
    if (entry || pause) {
        stop_on_entry_ = false;
        stop(vm, entry ? "entry" : "pause", {}, {});
        return;
    }
    switch (step_) {
        case Step::In: stop(vm, "step", {}, {}); return;
        case Step::Over:
            if (depth <= step_depth_) {
                stop(vm, "step", {}, {});
                return;
            }
            break;
        case Step::Out:
            if (depth < step_depth_) {
                stop(vm, "step", {}, {});
                return;
            }
            break;
        case Step::None: break;
    }
    const StackFrameInfo top = vm.top_frame();
    auto it = lines_.find(top.file);
    if (it == lines_.end() || !std::binary_search(it->second.begin(), it->second.end(), top.line)) return;
    std::vector<std::int64_t> hit;
    for (const Breakpoint& bp : breakpoints_.find(top.file)->second) {
        if (bp.verified && bp.line == top.line) hit.push_back(bp.id);
    }
    stop(vm, "breakpoint", {}, hit);
}

void Debugger::on_panic(VM& vm, std::string_view message) {
    if (terminate_ || !stop_on_panic_) return;
    stop(vm, "exception", message, {});
    step_ = Step::None; // a panic can't be stepped through: it unwinds now
}

void Debugger::stop(VM& vm, std::string_view reason, std::string_view text, const std::vector<std::int64_t>& hit) {
    step_ = Step::None;
    phase_ = Phase::Stopped;
    stopped_vm_ = &vm; // maybe not the active VM: a hot reload's candidate
    ++stops_;
    refs_.clear();
    JsonValue body = JsonValue::object({
        {"reason", str(reason)},
        {"threadId", num(thread_id)},
        {"allThreadsStopped", JsonValue::boolean(true)},
    });
    if (!text.empty()) {
        body.o["description"] = str(std::format("Panic: {}", text));
        body.o["text"] = str(text);
    }
    if (!hit.empty()) {
        json::Array ids;
        for (std::int64_t id : hit) ids.push_back(num(id));
        body.o["hitBreakpointIds"] = JsonValue::array(std::move(ids));
    }
    send_event("stopped", std::move(body));
    while (phase_ == Phase::Stopped) {
        if (transport_.closed()) {
            terminate_ = true;
            break;
        }
        if (std::optional<std::string> msg = transport_.receive(idle_interval)) {
            handle(*msg);
        } else if (idle_) {
            idle_();
        }
    }
    resume();
}

void Debugger::resume() {
    phase_ = Phase::Running;
    stopped_vm_ = nullptr;
    refs_.clear();
    pause_.store(false);
}

// ------------------------------------------------------------ requests

bool Debugger::handle(const std::string& text) {
    std::optional<JsonValue> parsed = json::Parser(text).parse();
    if (!parsed || !parsed->is_object() || parsed->str_or("type") != "request") return false;
    const JsonValue& request = *parsed;
    const std::string_view command = command_of(request);
    const JsonValue& args = arguments_of(request);
    const bool stopped = phase_ == Phase::Stopped;

    if (command == "initialize") {
        json::Array filters;
        filters.push_back(JsonValue::object({
            {"filter", str("panic")},
            {"label", str("Panics")},
            {"description", str("Stop when a script panics, before the stack unwinds")},
            {"default", JsonValue::boolean(true)},
        }));
        send_response(request, JsonValue::object({
                                   {"supportsConfigurationDoneRequest", JsonValue::boolean(true)},
                                   {"supportsEvaluateForHovers", JsonValue::boolean(true)},
                                   {"supportsTerminateRequest", JsonValue::boolean(true)},
                                   {"supportsDelayedStackTraceLoading", JsonValue::boolean(true)},
                                   {"exceptionBreakpointFilters", JsonValue::array(std::move(filters))},
                               }));
        send_event("initialized");
    } else if (command == "launch") {
        send_error(request, "already launched");
    } else if (command == "attach") {
        send_error(request, "attach is not supported: start the game with a `launch` configuration");
    } else if (command == "configurationDone") {
        configured_ = true;
        send_response(request);
    } else if (command == "setBreakpoints") {
        on_set_breakpoints(request);
    } else if (command == "setExceptionBreakpoints") {
        stop_on_panic_ = false;
        if (const JsonValue* f = args.get("filters"); f != nullptr && f->is_array()) {
            for (const JsonValue& v : f->a) stop_on_panic_ = stop_on_panic_ || (v.is_string() && v.s == "panic");
        }
        send_response(request);
    } else if (command == "setFunctionBreakpoints") {
        send_response(request, JsonValue::object({{"breakpoints", JsonValue::array()}}));
    } else if (command == "threads") {
        json::Array threads;
        threads.push_back(JsonValue::object({{"id", num(thread_id)}, {"name", str("main")}}));
        send_response(request, JsonValue::object({{"threads", JsonValue::array(std::move(threads))}}));
    } else if (command == "pause") {
        pause_.store(true);
        send_response(request);
    } else if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut") {
        if (!stopped) {
            send_error(request, "not stopped");
            return false;
        }
        const std::size_t depth = stopped_vm_ != nullptr ? stopped_vm_->frame_depth() : 0;
        if (command == "continue") step_ = Step::None;
        if (command == "next") step_ = Step::Over;
        if (command == "stepIn") step_ = Step::In;
        if (command == "stepOut") step_ = Step::Out;
        step_depth_ = depth;
        send_response(request, command == "continue" ? JsonValue::object({{"allThreadsContinued", JsonValue::boolean(true)}})
                                                     : JsonValue::object());
        phase_ = Phase::Running; // stop()'s loop ends; step_ applies from the next line
        return true;
    } else if (command == "stackTrace") {
        on_stack_trace(request);
    } else if (command == "scopes") {
        on_scopes(request);
    } else if (command == "variables") {
        on_variables(request);
    } else if (command == "evaluate") {
        on_evaluate(request);
    } else if (command == "source") {
        on_source(request);
    } else if (command == "disconnect" || command == "terminate") {
        terminate_ = true;
        send_response(request);
        if (stopped) phase_ = Phase::Running;
        return stopped;
    } else {
        send_error(request, std::format("unsupported request '{}'", command));
    }
    return false;
}

// ------------------------------------------------------------ breakpoints

std::string Debugger::relative_path(const JsonValue* source) const {
    if (source == nullptr) return {};
    // A source the debugger served by reference (the engine prelude).
    if (const std::int64_t ref = source->int_or("sourceReference", 0); ref > 0) {
        if (static_cast<std::size_t>(ref) <= sources_.size()) return sources_[static_cast<std::size_t>(ref) - 1].path;
        return std::string(source->str_or("name"));
    }
    const std::string_view path = source->str_or("path");
    if (path.empty()) return std::string(source->str_or("name"));
    return relativize(path);
}

std::string Debugger::relativize(std::string_view path) const {
    const fs::path p(path);
    if (!root_.empty()) {
        fs::path rel = p.lexically_relative(root_);
        if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
    }
    return p.generic_string();
}

void Debugger::set_source_root(fs::path root) {
    root_ = std::move(root);
    // Breakpoints set during the handshake, before the root was known, were
    // keyed by the client's absolute paths.
    std::map<std::string, std::vector<Breakpoint>, std::less<>> rekeyed;
    for (auto& [file, bps] : breakpoints_) {
        auto& list = rekeyed[relativize(file)];
        list.insert(list.end(), bps.begin(), bps.end());
    }
    breakpoints_ = std::move(rekeyed);
    rebuild_lines();
}

void Debugger::verify(const std::string& file, std::vector<Breakpoint>& bps, const VM* vm) const {
    std::vector<std::uint32_t> code;
    if (vm != nullptr) code = vm->code_lines(file);
    for (Breakpoint& bp : bps) {
        if (vm == nullptr) {
            // Before the program starts: accepted as asked, checked on attach.
            bp.line = bp.requested;
            bp.verified = true;
            continue;
        }
        // A line without code (a comment, a brace) stops at the next one with code.
        auto it = std::lower_bound(code.begin(), code.end(), bp.requested);
        bp.verified = it != code.end();
        bp.line = bp.verified ? *it : bp.requested;
    }
}

void Debugger::rebuild_lines() {
    lines_.clear();
    for (const auto& [file, bps] : breakpoints_) {
        std::vector<std::uint32_t> stops;
        for (const Breakpoint& bp : bps) {
            if (bp.verified) stops.push_back(bp.line);
        }
        if (stops.empty()) continue;
        std::sort(stops.begin(), stops.end());
        lines_.emplace(file, std::move(stops));
    }
}

void Debugger::on_set_breakpoints(const JsonValue& request) {
    const JsonValue& args = arguments_of(request);
    const std::string file = relative_path(args.get("source"));
    std::vector<Breakpoint> bps;
    if (const JsonValue* list = args.get("breakpoints"); list != nullptr && list->is_array()) {
        for (const JsonValue& b : list->a) {
            const std::int64_t line = b.int_or("line", 0);
            if (line <= 0) continue;
            bps.push_back(Breakpoint{next_breakpoint_id_++, static_cast<std::uint32_t>(line), 0, false});
        }
    }
    verify(file, bps, vm_);
    json::Array out;
    for (const Breakpoint& bp : bps) {
        JsonValue b = JsonValue::object({
            {"id", num(bp.id)},
            {"verified", JsonValue::boolean(bp.verified)},
            {"line", num(bp.line)},
        });
        if (!bp.verified) b.o["message"] = str("no code at or after this line");
        out.push_back(std::move(b));
    }
    if (bps.empty()) {
        breakpoints_.erase(file);
    } else {
        breakpoints_[file] = std::move(bps);
    }
    rebuild_lines();
    send_response(request, JsonValue::object({{"breakpoints", JsonValue::array(std::move(out))}}));
}

// ------------------------------------------------------------ inspection

JsonValue Debugger::source_json(const SourceFile* file, std::string_view path) {
    const fs::path on_disk = root_.empty() ? fs::path(path) : root_ / fs::path(path);
    std::error_code ec;
    if (fs::is_regular_file(on_disk, ec)) {
        return JsonValue::object({
            {"name", str(fs::path(path).filename().string())},
            {"path", str(on_disk.string())},
        });
    }
    // Not a file of the project (the engine prelude): served by `source`.
    std::size_t index = 0;
    while (index < sources_.size() && sources_[index].path != path) ++index;
    if (index == sources_.size()) sources_.push_back({file, std::string(path)});
    return JsonValue::object({
        {"name", str(path)},
        {"sourceReference", num(static_cast<std::int64_t>(index + 1))},
        {"presentationHint", str("deemphasize")},
    });
}

void Debugger::on_stack_trace(const JsonValue& request) {
    if (phase_ != Phase::Stopped || stopped_vm_ == nullptr) {
        send_error(request, "not stopped");
        return;
    }
    const JsonValue& args = arguments_of(request);
    const std::vector<StackFrameInfo> frames = stopped_vm_->stack_frames();
    const auto start = static_cast<std::size_t>(std::max<std::int64_t>(0, args.int_or("startFrame", 0)));
    std::int64_t levels = args.int_or("levels", 0);
    if (levels <= 0) levels = static_cast<std::int64_t>(frames.size());
    json::Array out;
    for (std::size_t i = start; i < frames.size() && static_cast<std::int64_t>(out.size()) < levels; ++i) {
        const StackFrameInfo& f = frames[i];
        JsonValue frame = JsonValue::object({
            {"id", num(static_cast<std::int64_t>(i + 1))},
            {"name", str(f.function)},
            {"line", num(f.line)},
            {"column", num(f.column)},
        });
        if (!f.file.empty()) frame.o["source"] = source_json(f.fn != nullptr ? f.fn->source : nullptr, f.file);
        out.push_back(std::move(frame));
    }
    send_response(request, JsonValue::object({
                               {"stackFrames", JsonValue::array(std::move(out))},
                               {"totalFrames", num(static_cast<std::int64_t>(frames.size()))},
                           }));
}

std::int64_t Debugger::add_ref(Ref ref) {
    refs_.push_back(ref);
    return static_cast<std::int64_t>(refs_.size());
}

void Debugger::on_scopes(const JsonValue& request) {
    const std::int64_t frame_id = arguments_of(request).int_or("frameId", 0);
    if (phase_ != Phase::Stopped || stopped_vm_ == nullptr || frame_id < 1 ||
        static_cast<std::size_t>(frame_id) > stopped_vm_->frame_depth()) {
        send_error(request, "no such frame");
        return;
    }
    const auto frame = static_cast<std::size_t>(frame_id - 1);
    json::Array scopes;
    scopes.push_back(JsonValue::object({
        {"name", str("Locals")},
        {"presentationHint", str("locals")},
        {"variablesReference", num(add_ref({Ref::Kind::Locals, frame, {}}))},
        {"expensive", JsonValue::boolean(false)},
    }));
    scopes.push_back(JsonValue::object({
        {"name", str("Globals")},
        {"variablesReference", num(add_ref({Ref::Kind::Globals, 0, {}}))},
        {"expensive", JsonValue::boolean(false)},
    }));
    send_response(request, JsonValue::object({{"scopes", JsonValue::array(std::move(scopes))}}));
}

JsonValue Debugger::variable_json(std::string name, const Value& value) {
    JsonValue v = JsonValue::object({
        {"name", str(name)},
        {"value", str(stopped_vm_->describe(value))},
        {"variablesReference", num(0)},
    });
    if (!stopped_vm_->children(value).empty()) v.o["variablesReference"] = num(add_ref({Ref::Kind::Children, 0, value}));
    return v;
}

void Debugger::on_variables(const JsonValue& request) {
    const std::int64_t id = arguments_of(request).int_or("variablesReference", 0);
    if (phase_ != Phase::Stopped || stopped_vm_ == nullptr || id < 1 || static_cast<std::size_t>(id) > refs_.size()) {
        send_error(request, "no such variables reference");
        return;
    }
    const Ref ref = refs_[static_cast<std::size_t>(id) - 1]; // a copy: add_ref may grow refs_
    json::Array out;
    switch (ref.kind) {
        case Ref::Kind::Locals:
            for (const LocalInfo& l : stopped_vm_->frame_locals(ref.frame)) {
                JsonValue v = variable_json(std::string(l.name), l.value);
                v.o["type"] = str(l.type);
                out.push_back(std::move(v));
            }
            break;
        case Ref::Kind::Globals:
            for (std::size_t i = 0; i < stopped_vm_->globals.size() && i < stopped_vm_->global_names.size(); ++i) {
                JsonValue v = variable_json(stopped_vm_->global_names[i], stopped_vm_->globals[i]);
                if (i < stopped_vm_->global_types.size()) v.o["type"] = str(stopped_vm_->global_types[i]);
                out.push_back(std::move(v));
            }
            break;
        case Ref::Kind::Children:
            for (const ChildInfo& c : stopped_vm_->children(ref.value)) out.push_back(variable_json(c.name, c.value));
            break;
    }
    send_response(request, JsonValue::object({{"variables", JsonValue::array(std::move(out))}}));
}

std::optional<Value> Debugger::evaluate_path(std::string_view expression, std::size_t frame) const {
    const std::vector<std::string> parts = split_path(expression);
    if (parts.empty() || stopped_vm_ == nullptr) return std::nullopt;
    std::optional<Value> current;
    std::size_t next = 1;
    for (const LocalInfo& l : stopped_vm_->frame_locals(frame)) {
        if (l.name == parts[0]) current = l.value;
    }
    if (!current) {
        // Globals are qualified ("Game.main"): the longest dotted prefix wins.
        std::string name;
        for (std::size_t n = 0; n < parts.size() && !parts[n].starts_with('['); ++n) {
            name += (n > 0 ? "." : "") + parts[n];
            if (std::optional<std::uint32_t> g = stopped_vm_->global_index(name)) {
                current = stopped_vm_->globals[*g];
                next = n + 1;
            }
        }
    }
    if (!current) return std::nullopt;
    for (std::size_t i = next; i < parts.size(); ++i) {
        bool found = false;
        for (const ChildInfo& c : stopped_vm_->children(*current)) {
            if (c.name == parts[i]) {
                current = c.value;
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    return current;
}

void Debugger::on_evaluate(const JsonValue& request) {
    const JsonValue& args = arguments_of(request);
    if (phase_ != Phase::Stopped || stopped_vm_ == nullptr) {
        send_error(request, "not stopped");
        return;
    }
    const std::int64_t frame_id = args.int_or("frameId", 1);
    const std::size_t frame = frame_id >= 1 ? static_cast<std::size_t>(frame_id - 1) : 0;
    const std::string_view expression = args.str_or("expression");
    const std::optional<Value> v = evaluate_path(expression, frame);
    if (!v) {
        // Only variable paths are evaluated: running script code while
        // stopped could change the simulation.
        send_error(request, std::format("cannot evaluate '{}' (variables, fields and [index] only)", expression));
        return;
    }
    JsonValue var = variable_json(std::string(expression), *v);
    send_response(request, JsonValue::object({
                               {"result", var.o["value"]},
                               {"variablesReference", var.o["variablesReference"]},
                           }));
}

void Debugger::on_source(const JsonValue& request) {
    const JsonValue& args = arguments_of(request);
    std::int64_t ref = args.int_or("sourceReference", 0);
    if (ref <= 0) {
        if (const JsonValue* s = args.get("source")) ref = s->int_or("sourceReference", 0);
    }
    if (ref <= 0 || static_cast<std::size_t>(ref) > sources_.size()) {
        send_error(request, "unknown source");
        return;
    }
    const SourceRef& s = sources_[static_cast<std::size_t>(ref) - 1];
    const std::string_view text = s.file != nullptr ? s.file->part_text(s.path) : std::string_view();
    send_response(request, JsonValue::object({{"content", str(text)}, {"mimeType", str("text/x-pluma")}}));
}

} // namespace pluma::dap

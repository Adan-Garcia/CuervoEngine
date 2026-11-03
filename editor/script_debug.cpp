#include "editor/script_debug.hpp"

#include <algorithm>
#include <format>

namespace fs = std::filesystem;
using pluma::json::Value;

namespace cuervo::editor {

namespace {

std::string quote(std::string_view s) { return pluma::json::dump(Value::string(s)); }

std::string bp_args(const fs::path& file, const std::set<int>& lines) {
    std::string out = std::format("{{\"source\":{{\"path\":{}}},\"breakpoints\":[", quote(file.string()));
    bool first = true;
    for (const int l : lines) {
        out += std::format("{}{{\"line\":{}}}", first ? "" : ",", l);
        first = false;
    }
    return out + "]}";
}

} // namespace

DebugSession::DebugSession() : dbg_(transport_) {}

bool DebugSession::start(const fs::path& project, const Breakpoints& bps) {
    root_ = project;
    dbg_.set_source_root(project);
    transport_.client_request("initialize", R"({"adapterID":"pluma"})");
    transport_.client_request("launch", std::format("{{\"project\":{}}}", quote(project.string())));
    for (const auto& [rel, lines] : bps) transport_.client_request("setBreakpoints", bp_args(project / rel, lines));
    transport_.client_request("configurationDone");
    return dbg_.wait_for_start({}).has_value();
}

void DebugSession::request(std::string_view command, std::string args, std::function<void(const Value&)> done) {
    const std::int64_t seq = transport_.client_request(command, args);
    if (done) pending_[seq] = std::move(done);
}

void DebugSession::set_breakpoints(const std::string& rel, const std::set<int>& lines) {
    request("setBreakpoints", bp_args(root_ / rel, lines));
}

void DebugSession::pump() {
    while (auto m = transport_.client_receive(std::chrono::milliseconds(0))) {
        if (auto v = pluma::json::Parser(*m).parse()) on_message(*v);
    }
}

void DebugSession::on_message(const Value& v) {
    const std::string_view type = v.str_or("type");
    if (type == "response") {
        const auto it = pending_.find(v.int_or("request_seq"));
        if (it == pending_.end()) return;
        auto done = std::move(it->second);
        pending_.erase(it);
        if (v.bool_or("success", true)) done(v.get("body") != nullptr ? *v.get("body") : Value::object());
        else done(Value::null());
        return;
    }
    if (type != "event") return;
    const std::string_view ev = v.str_or("event");
    const Value* body = v.get("body");
    if (ev == "stopped") {
        stopped_ = true;
        ++stop_serial_;
        reason_ = body != nullptr ? std::string(body->str_or("reason")) : std::string();
        frames_.clear();
        vars_.clear();
        frame_ = 0;
        fetch_frame_state();
    } else if (ev == "continued") {
        leave_stop();
    } else if (ev == "terminated" || ev == "exited") {
        terminated_ = true;
        leave_stop();
    } else if (ev == "output" && body != nullptr) {
        output_.emplace_back(body->str_or("output"));
        if (output_.size() > 500) output_.erase(output_.begin(), output_.begin() + 100);
    }
}

void DebugSession::leave_stop() {
    stopped_ = false;
    frames_.clear();
    vars_.clear();
    for (WatchItem& w : watches_) { w.pending = false; }
}

void DebugSession::fetch_frame_state() {
    const std::uint64_t serial = stop_serial_;
    request("stackTrace", R"({"threadId":1})", [this, serial](const Value& body) {
        if (serial != stop_serial_ || !stopped_) return;
        frames_.clear();
        if (const Value* fr = body.get("stackFrames")) {
            for (const Value& f : fr->a) {
                DebugFrame d;
                d.id = static_cast<int>(f.int_or("id"));
                d.name = std::string(f.str_or("name"));
                d.line = static_cast<int>(f.int_or("line"));
                if (const Value* src = f.get("source")) {
                    std::error_code ec;
                    const std::string path(src->str_or("path"));
                    if (!path.empty()) {
                        const fs::path rel = fs::relative(path, root_, ec);
                        d.rel = ec ? path : rel.generic_string();
                    }
                }
                frames_.push_back(std::move(d));
            }
        }
        if (!frames_.empty()) select_frame(std::clamp(frame_, 0, static_cast<int>(frames_.size()) - 1));
    });
}

void DebugSession::select_frame(int index) {
    if (index < 0 || index >= static_cast<int>(frames_.size())) return;
    frame_ = index;
    vars_.clear();
    const std::uint64_t serial = stop_serial_;
    const int fid = frames_[static_cast<std::size_t>(index)].id;
    request("scopes", std::format("{{\"frameId\":{}}}", fid), [this, serial, index](const Value& body) {
        if (serial != stop_serial_ || index != frame_ || !stopped_) return;
        const Value* sc = body.get("scopes");
        if (sc == nullptr || sc->a.empty()) return;
        const int ref = static_cast<int>(sc->a[0].int_or("variablesReference"));
        request("variables", std::format("{{\"variablesReference\":{}}}", ref), [this, serial, index](const Value& vb) {
            if (serial != stop_serial_ || index != frame_ || !stopped_) return;
            vars_.clear();
            if (const Value* vs = vb.get("variables")) {
                for (const Value& v : vs->a) {
                    DebugVar d;
                    d.name = std::string(v.str_or("name"));
                    d.value = std::string(v.str_or("value"));
                    d.type = std::string(v.str_or("type"));
                    d.ref = static_cast<int>(v.int_or("variablesReference"));
                    vars_.push_back(std::move(d));
                }
            }
        });
    });
    for (std::size_t i = 0; i < watches_.size(); ++i) eval_watch(i);
}

void DebugSession::toggle_var(int index) {
    if (index < 0 || index >= static_cast<int>(vars_.size())) return;
    DebugVar& v = vars_[static_cast<std::size_t>(index)];
    if (v.ref <= 0) return;
    if (v.open) {
        // Drop the children (deeper rows right after it).
        const int depth = v.depth;
        v.open = false;
        std::size_t e = static_cast<std::size_t>(index) + 1;
        while (e < vars_.size() && vars_[e].depth > depth) ++e;
        vars_.erase(vars_.begin() + index + 1, vars_.begin() + static_cast<std::ptrdiff_t>(e));
        return;
    }
    v.open = true;
    fetch_children(index);
}

void DebugSession::fetch_children(int index) {
    const std::uint64_t serial = stop_serial_;
    const int ref = vars_[static_cast<std::size_t>(index)].ref;
    const std::string name = vars_[static_cast<std::size_t>(index)].name;
    request("variables", std::format("{{\"variablesReference\":{}}}", ref), [this, serial, name, ref](const Value& body) {
        if (serial != stop_serial_ || !stopped_) return;
        std::size_t at = vars_.size();
        for (std::size_t i = 0; i < vars_.size(); ++i) if (vars_[i].ref == ref && vars_[i].open) { at = i; break; }
        if (at == vars_.size()) return;
        const int depth = vars_[at].depth + 1;
        std::vector<DebugVar> kids;
        if (const Value* vs = body.get("variables")) {
            for (const Value& v : vs->a) {
                DebugVar d;
                d.name = std::string(v.str_or("name"));
                d.value = std::string(v.str_or("value"));
                d.type = std::string(v.str_or("type"));
                d.ref = static_cast<int>(v.int_or("variablesReference"));
                d.depth = depth;
                kids.push_back(std::move(d));
            }
        }
        vars_.insert(vars_.begin() + static_cast<std::ptrdiff_t>(at) + 1, kids.begin(), kids.end());
    });
}

void DebugSession::add_watch(const std::string& expr) {
    if (expr.empty()) return;
    watches_.push_back({expr, {}, false, false});
    if (stopped_) eval_watch(watches_.size() - 1);
}

void DebugSession::remove_watch(int index) {
    if (index >= 0 && index < static_cast<int>(watches_.size())) watches_.erase(watches_.begin() + index);
}

void DebugSession::eval_watch(std::size_t i) {
    if (!stopped_ || frames_.empty() || i >= watches_.size()) return;
    const std::string expr = watches_[i].expr;
    watches_[i].pending = true;
    const std::uint64_t serial = stop_serial_;
    request("evaluate", std::format("{{\"expression\":{},\"frameId\":{},\"context\":\"watch\"}}", quote(expr), frames_[static_cast<std::size_t>(frame_)].id),
            [this, serial, expr](const Value& body) {
                if (serial != stop_serial_) return;
                for (WatchItem& w : watches_) {
                    if (w.expr != expr) continue;
                    w.pending = false;
                    w.ok = body.get("result") != nullptr;
                    w.result = w.ok ? std::string(body.str_or("result")) : std::string("not available");
                }
            });
}

void DebugSession::resume() { if (stopped_) { request("continue", R"({"threadId":1})"); leave_stop(); } }
void DebugSession::step_over() { if (stopped_) { request("next", R"({"threadId":1})"); leave_stop(); } }
void DebugSession::step_in() { if (stopped_) { request("stepIn", R"({"threadId":1})"); leave_stop(); } }
void DebugSession::step_out() { if (stopped_) { request("stepOut", R"({"threadId":1})"); leave_stop(); } }
void DebugSession::pause() { dbg_.request_pause(); }
void DebugSession::disconnect() { request("disconnect", "{}"); transport_.close(); leave_stop(); }

} // namespace cuervo::editor

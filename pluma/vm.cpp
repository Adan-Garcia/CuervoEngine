#include "pluma/vm.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <format>
#include <iostream>

#include "pluma/builtins.hpp"
#include "pluma/collections.hpp"
#include "pluma/numeric.hpp"
#include "pluma/tir.hpp"

namespace pluma {

namespace {

// Thrown by VM::panic; caught by VM::protect, which builds the report.
struct PanicSignal {
    std::string message;
};

std::string_view arith_symbol(Op op) {
    switch (op) {
        case Op::Add: return "+";
        case Op::Sub: return "-";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Rem: return "%";
        case Op::Neg: return "-";
        case Op::Shl: return "<<";
        case Op::Shr: return ">>";
        default: return "?";
    }
}

BinaryOp arith_binop(Op op) {
    switch (op) {
        case Op::Add: return BinaryOp::Add;
        case Op::Sub: return BinaryOp::Sub;
        case Op::Mul: return BinaryOp::Mul;
        case Op::Div: return BinaryOp::Div;
        case Op::Rem: return BinaryOp::Rem;
        case Op::BAnd: return BinaryOp::BitAnd;
        case Op::BOr: return BinaryOp::BitOr;
        case Op::BXor: return BinaryOp::BitXor;
        case Op::Shl: return BinaryOp::Shl;
        case Op::Shr: return BinaryOp::Shr;
        default: return BinaryOp::Add;
    }
}

std::string_view method_for(Op op) {
    switch (op) {
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Rem: return "rem";
        case Op::Neg: return "neg";
        default: return "";
    }
}

template <class F>
void format_float(std::string& out, F x) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), x);
    std::string_view s(buf, static_cast<std::size_t>(res.ptr - buf));
    out += s;
    if (s.find_first_of(".eEni") == std::string_view::npos) out += ".0";
}

void quote(std::string& out, std::string_view s) {
    out += '"';
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\0': out += "\\0"; break;
            case '{': out += "{{"; break;
            case '}': out += "}}"; break;
            default: out += ch;
        }
    }
    out += '"';
}

StrObj* as_str(const Value& v) { return static_cast<StrObj*>(v.o); }

// Largest vector size (checker.cpp, max_vec_dim): vector temporaries live on
// the C++ stack.
constexpr std::uint32_t max_vec = 64;

float scalar_op(Op op, float p, float q) {
    switch (op) {
        case Op::Add: return p + q;
        case Op::Sub: return p - q;
        case Op::Mul: return p * q;
        case Op::Div: return p / q;
        default: return std::fmod(p, q);
    }
}

// Componentwise vector arithmetic; `kind` is opk::Vec, VecS (vector op
// scalar) or SVec (scalar op vector).
Value vec_arith(Op op, std::uint8_t kind, const Value& x, const Value& y) {
    float out[max_vec];
    if (kind == opk::VecS) {
        std::uint32_t n = vec_size(x);
        const float* a = vec_data(x);
        for (std::uint32_t i = 0; i < n; ++i) out[i] = scalar_op(op, a[i], y.f);
        return vec_value(out, n);
    }
    if (kind == opk::SVec) {
        std::uint32_t n = vec_size(y);
        const float* b = vec_data(y);
        for (std::uint32_t i = 0; i < n; ++i) out[i] = scalar_op(op, x.f, b[i]);
        return vec_value(out, n);
    }
    std::uint32_t n = vec_size(x);
    const float* a = vec_data(x);
    const float* b = vec_data(y);
    for (std::uint32_t i = 0; i < n; ++i) out[i] = scalar_op(op, a[i], b[i]);
    return vec_value(out, n);
}

bool vec_equal(const Value& x, const Value& y) {
    std::uint32_t n = vec_size(x);
    if (vec_size(y) != n) return false;
    const float* a = vec_data(x);
    const float* b = vec_data(y);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

float vec_dot(const Value& x, const Value& y) {
    std::uint32_t n = vec_size(x);
    const float* a = vec_data(x);
    const float* b = vec_data(y);
    float s = 0.0f;
    for (std::uint32_t i = 0; i < n; ++i) s += a[i] * b[i];
    return s;
}

// The await kinds, numbered like tir::AwaitKind (the compiler stores them in k).
using AK = tir::AwaitKind;

} // namespace

std::string PanicInfo::render() const {
    std::string out = std::format("{}:{}:{}: panic: {}\n", file, line, column, message);
    for (const std::string& t : trace) out += "    at " + t + "\n";
    return out;
}

std::string PanicInfo::render_with_locals() const {
    std::string out = std::format("{}:{}:{}: panic: {}\n", file, line, column, message);
    for (std::size_t i = 0; i < trace.size(); ++i) {
        out += "    at " + trace[i] + "\n";
        if (i >= frames.size()) continue;
        for (const PanicLocal& l : frames[i].locals) out += std::format("        {}: {} = {}\n", l.name, l.type, l.value);
    }
    return out;
}

VM::VM(VmOptions options) : options_(std::move(options)) {
    stack_.resize(1024);
    rng_state_ = options_.random_seed;
    intern(""); // id 0: the default name
}

VM::~VM() {
    for (Value& v : stack_) release(v);
    for (Value& v : globals) release(v);
    std::vector<CoroObj*> coros;
    coros.swap(coros_);
    for (CoroObj* c : coros) release(Value::object(c));
    // Manual objects die with their VM; then garbage cycles are collected
    // while this VM's types (finalizers) still exist.
    reset_arena(ArenaKind::Frame);
    reset_arena(ArenaKind::Scene);
    reset_arena(ArenaKind::Alloc);
    collect_all_cycles(false);
}

// ------------------------------------------------------------ memory

void VM::register_manual(Object* o) {
    ManualList& l = manual_[static_cast<std::size_t>(arena_of(o))];
    if (l.items.size() >= l.compact_at) {
        std::erase_if(l.items, [](const auto& e) { return e.first->gen != e.second; });
        l.compact_at = std::max<std::size_t>(64, l.items.size() * 2);
    }
    l.items.emplace_back(o, o->gen);
}

void VM::free_manual(Object* o, const FreeSite& site) {
    // Its coroutines stop, like those of a node that leaves the tree (5.9).
    if ((o->flags & objflag::Promised) != 0) cancel_owned(o);
    destroy_manual(o, site);
}

void VM::reset_arena(ArenaKind arena) {
    std::vector<std::pair<Object*, std::uint16_t>> items;
    items.swap(manual_[static_cast<std::size_t>(arena)].items);
    FreeSite site;
    site.reset = arena;
    site.tick = tick_;
    // Newest first, like defers: deterministic finalization order.
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->first->gen == it->second) free_manual(it->first, site);
    }
}

void VM::reset_frame_arena() { reset_arena(ArenaKind::Frame); }
void VM::reset_scene_arena() { reset_arena(ArenaKind::Scene); }

std::size_t VM::arena_objects(ArenaKind arena) const {
    std::size_t n = 0;
    for (const auto& e : manual_[static_cast<std::size_t>(arena)].items) n += e.first->gen == e.second ? 1 : 0;
    return n;
}

std::string VM::free_text(const Object* o) const {
    const FreeSite* s = free_site(o);
    if (s == nullptr) return "it was freed";
    if (s->proto != nullptr) {
        const auto* p = static_cast<const Proto*>(s->proto);
        std::string_view file;
        std::uint32_t line = 0;
        std::uint32_t col = 0;
        if (p->resolve_pc(s->pc, file, line, col)) {
            return std::format("it was freed at {}:{}:{}", file, line, col);
        }
        return "it was freed";
    }
    switch (s->reset) {
        case ArenaKind::Frame: return std::format("it was freed by the frame arena at the end of tick {}", s->tick);
        case ArenaKind::Scene: return "it was freed by the scene arena when the scene unloaded";
        default: return "it was freed when its VM was destroyed";
    }
}

void VM::stale_panic(const Value& v, std::string_view what) {
    panic(std::format("use after free: {}; {}", what, free_text(v.o)));
}

RunResult VM::collect_cycles(std::uint64_t budget, CollectResult* stats) {
    CollectResult r = pluma::collect_cycles(budget, gc_credit_, options_.check_acyclic);
    if (stats != nullptr) *stats = r;
    RunResult out;
    if (!r.acyclic_violation.empty()) {
        out.ok = false;
        out.panic = PanicInfo{};
        out.panic->message = std::format("an instance of '@acyclic' class '{}' was part of a reference cycle "
                                         "(the cycle was collected; remove '@acyclic' or break the cycle)",
                                         r.acyclic_violation);
    }
    return out;
}

Value VM::new_manual_list(ArenaKind arena, bool untraced) {
    ListObj* l = manual_list(arena);
    (void)untraced; // manual objects are never traced
    register_manual(l);
    return manual_ref(l);
}

std::uint32_t VM::intern(std::string_view name) {
    auto it = name_ids_.find(name);
    if (it != name_ids_.end()) return it->second;
    auto id = static_cast<std::uint32_t>(names_.size());
    names_.emplace_back(name);
    name_ids_.emplace(std::string(name), id);
    return id;
}

void VM::write(std::string_view text) {
    if (options_.out) {
        options_.out(text);
    } else {
        std::cout << text << std::flush;
    }
}

void VM::panic(std::string message) { throw PanicSignal{std::move(message)}; }

// ------------------------------------------------------------ host functions

std::uint32_t VM::register_native(std::string name, NativeFn fn) {
    auto it = native_ids_.find(name);
    if (it != native_ids_.end()) {
        natives_[it->second].fn = std::move(fn);
        return it->second;
    }
    auto id = static_cast<std::uint32_t>(natives_.size());
    native_ids_.emplace(name, id);
    natives_.push_back(Native{std::move(name), std::move(fn)});
    return id;
}

std::uint32_t VM::native_index(std::string_view name) const {
    auto it = native_ids_.find(name);
    return it != native_ids_.end() ? it->second : no_native;
}

void VM::set_native_finalizer(std::string class_name, void (*finalizer)(void*)) {
    finalizers_[std::move(class_name)] = finalizer;
}

void (*VM::native_finalizer(std::string_view class_name) const)(void*) {
    auto it = finalizers_.find(class_name);
    return it != finalizers_.end() ? it->second : nullptr;
}

const RecordRT* VM::find_class(std::string_view name) const {
    for (const auto& c : classes) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

const RecordRT* VM::find_struct(std::string_view name) const {
    for (const auto& s : structs) {
        if (s->name == name) return s.get();
    }
    return nullptr;
}

std::optional<std::uint32_t> VM::global_index(std::string_view qualified) const {
    for (std::size_t i = 0; i < global_names.size(); ++i) {
        if (global_names[i] == qualified) return static_cast<std::uint32_t>(i);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> VM::proto_by_key(std::string_view key) const {
    for (const auto& p : protos) {
        if (p->key == key) return p->index;
    }
    return std::nullopt;
}

// ------------------------------------------------------------ frames

std::string VM::location(const Frame& f, std::uint32_t pc) const {
    std::string_view file;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    if (!f.fn->resolve_pc(pc, file, line, col)) return "<unknown>";
    return std::format("{}:{}:{}", file, line, col);
}

std::uint32_t VM::stack_top() const {
    if (frames_.empty()) return 0;
    const Frame& f = frames_.back();
    return f.base + f.fn->reg_count;
}

void VM::ensure_stack(std::size_t size) {
    if (stack_.size() >= size) return;
    std::size_t n = stack_.size();
    while (n < size) n *= 2;
    stack_.resize(n);
}

void VM::push_frame(const Proto* fn, const ClosureObj* closure, const Value* args, std::uint32_t argc,
                    std::uint32_t ret, bool discard) {
    if (frames_.size() >= options_.max_frames) panic("stack overflow (recursion too deep)");
    std::uint32_t base = stack_top();
    // Arguments may live in the stack, which can move when it grows.
    const Value* data = stack_.data();
    bool inside = args >= data && args < data + stack_.size();
    std::size_t offset = inside ? static_cast<std::size_t>(args - data) : 0;
    ensure_stack(static_cast<std::size_t>(base) + fn->reg_count + 1);
    if (inside) args = stack_.data() + offset;
    for (std::uint32_t i = 0; i < argc; ++i) assign(stack_[base + i], args[i]);
    if (closure != nullptr) ++const_cast<ClosureObj*>(closure)->rc;
    frames_.push_back(Frame{fn, closure, base, 0, ret, discard, nullptr});
    enter_hook(frames_.back());
}

Value VM::pop_frame(const Value& result) {
    Frame f = frames_.back();
    leave_hook(f);
    Value r = result;
    retain(r); // the result may live in a register cleared below
    for (std::uint32_t i = 0; i < f.fn->reg_count; ++i) assign_owned(stack_[f.base + i], Value{});
    frames_.pop_back();
    if (f.closure != nullptr) release(Value::object(const_cast<ClosureObj*>(f.closure)));
    if (f.discard) return r; // owned by the native caller
    assign_owned(stack_[f.ret], r);
    return Value{};
}

Value VM::invoke(std::uint32_t proto_index, const Value* args, std::uint32_t argc) {
    std::size_t depth = frames_.size();
    push_frame(protos[proto_index].get(), nullptr, args, argc, 0, true);
    return execute(depth);
}

Value VM::invoke_closure(const ClosureObj* closure, const Value* args, std::uint32_t argc) {
    std::size_t depth = frames_.size();
    push_frame(closure->fn, closure, args, argc, 0, true);
    return execute(depth);
}

PanicInfo VM::panic_info(const std::string& message, std::size_t depth) const {
    PanicInfo info;
    info.message = message;
    for (std::size_t i = frames_.size(); i > depth; --i) {
        const Frame& f = frames_[i - 1];
        std::uint32_t pc = f.pc > 0 ? f.pc - 1 : 0;
        std::string_view file;
        std::uint32_t line = 0;
        std::uint32_t col = 0;
        bool has_loc = f.fn->resolve_pc(pc, file, line, col);
        if (i == frames_.size() && has_loc) {
            info.file = std::string(file);
            info.line = line;
            info.column = col;
        }
        info.trace.push_back(std::format("{} ({})", f.fn->name, location(f, pc)));
        // The frame and its live locals, for a debugger (DESIGN.md 5.8). The
        // registers are still intact: unwinding happens after this.
        PanicFrame frame;
        frame.function = f.fn->name;
        if (has_loc) {
            frame.file = std::string(file);
            frame.line = line;
            frame.column = col;
        }
        for (const LocalDebug& l : f.fn->locals) {
            if (pc < l.begin || pc >= l.end || f.base + l.reg >= stack_.size()) continue;
            const Value& v = stack_[f.base + l.reg];
            frame.locals.push_back({l.name, l.type, const_cast<VM*>(this)->describe(v)});
        }
        info.frames.push_back(std::move(frame));
    }
    return info;
}

// ------------------------------------------------------------ debugging

void VM::set_debug_hooks(DebugHooks* hooks) {
    hooks_ = hooks;
    const std::uint32_t events = hooks != nullptr ? hooks->events() : 0;
    hook_lines_ = (events & DebugHooks::lines) != 0;
    hook_calls_ = (events & DebugHooks::calls) != 0;
    hook_panics_ = (events & DebugHooks::panics) != 0;
    // Running frames start over: their next instruction reports its line.
    for (Frame& f : frames_) f.hook_line = 0;
}

std::uint32_t VM::line_of(const Proto& fn, std::uint32_t pc) const {
    if (fn.debug_lines.size() != fn.code.size()) {
        fn.debug_lines.assign(fn.code.size(), 0);
        for (std::uint32_t i = 0; i < fn.code.size(); ++i) {
            std::string_view file;
            std::uint32_t line = 0;
            std::uint32_t column = 0;
            // An empty span marks compiler glue (cleanup, implicit return): no line.
            const bool glue = i < fn.spans.size() && fn.spans[i].begin == fn.spans[i].end;
            if (!glue && fn.resolve_pc(i, file, line, column)) fn.debug_lines[i] = line;
        }
    }
    return pc < fn.debug_lines.size() ? fn.debug_lines[pc] : 0;
}

void VM::line_hook(Frame& f) {
    const std::uint32_t pc = f.pc;
    const std::uint32_t line = line_of(*f.fn, pc);
    // A new line, or the same line again after a backward jump (one pass of
    // a loop written on one line). Line 0 is compiler glue without a span.
    const bool event = line != 0 && (line != f.hook_line || pc < f.hook_pc);
    f.hook_pc = pc;
    if (!event) return;
    f.hook_line = line;
    hooks_->on_line(*this);
}

std::vector<StackFrameInfo> VM::stack_frames() const {
    std::vector<StackFrameInfo> out;
    out.reserve(frames_.size());
    for (std::size_t i = frames_.size(); i > 0; --i) {
        const Frame& f = frames_[i - 1];
        const std::uint32_t pc = inspect_pc(f, i == frames_.size());
        StackFrameInfo info;
        info.fn = f.fn;
        info.function = f.fn->name;
        f.fn->resolve_pc(pc, info.file, info.line, info.column);
        out.push_back(info);
    }
    return out;
}

StackFrameInfo VM::top_frame() const {
    StackFrameInfo info;
    if (frames_.empty()) return info;
    const Frame& f = frames_.back();
    info.fn = f.fn;
    info.function = f.fn->name;
    f.fn->resolve_pc(inspect_pc(f, true), info.file, info.line, info.column);
    return info;
}

std::vector<LocalInfo> VM::frame_locals(std::size_t index) const {
    std::vector<LocalInfo> out;
    if (index >= frames_.size()) return out;
    const Frame& f = frames_[frames_.size() - 1 - index];
    const std::uint32_t pc = inspect_pc(f, index == 0);
    for (const LocalDebug& l : f.fn->locals) {
        if (pc < l.begin || pc >= l.end || f.base + l.reg >= stack_.size()) continue;
        out.push_back({l.name, l.type, stack_[f.base + l.reg]});
    }
    return out;
}

std::vector<ChildInfo> VM::children(const Value& v) const {
    std::vector<ChildInfo> out;
    if (is_vec(v)) {
        static constexpr const char* axes[] = {"x", "y", "z", "w"};
        const std::uint32_t n = vec_size(v);
        const float* d = vec_data(v);
        for (std::uint32_t i = 0; i < n; ++i) out.push_back({i < 4 ? axes[i] : std::to_string(i), Value::f32(d[i])});
        return out;
    }
    if (!v.is_obj() || is_stale(v)) return out;
    switch (v.o->kind) {
        case ObjKind::List:
        case ObjKind::Array: {
            const auto* l = static_cast<const ListObj*>(v.o);
            for (std::size_t i = 0; i < l->items.size(); ++i) out.push_back({std::format("[{}]", i), l->items[i]});
            break;
        }
        case ObjKind::Map:
        case ObjKind::Set: {
            const auto* h = static_cast<const HashObj*>(v.o);
            std::size_t n = 0;
            for (std::size_t i = 0; i < h->keys.size(); ++i) {
                if (h->keys[i].tag == Tag::Nil) continue;
                if (v.o->kind == ObjKind::Map) {
                    out.push_back({"[" + const_cast<VM*>(this)->describe(h->keys[i], 60) + "]", h->vals[i]});
                } else {
                    out.push_back({std::format("[{}]", n), h->keys[i]});
                }
                ++n;
            }
            break;
        }
        case ObjKind::Deque: {
            const auto* d = static_cast<const DequeObj*>(v.o);
            for (std::size_t i = 0; i < d->count; ++i) out.push_back({std::format("[{}]", i), d->at(i)});
            break;
        }
        case ObjKind::PQueue: {
            const auto* q = static_cast<const PQueueObj*>(v.o);
            for (std::size_t i = 0; i < q->items.size(); ++i) out.push_back({std::format("[{}]", i), q->items[i]});
            break;
        }
        case ObjKind::Instance: {
            const auto* inst = static_cast<const InstanceObj*>(v.o);
            for (std::size_t i = 0; i < inst->fields.size(); ++i) {
                const bool named = i < inst->type->field_names.size();
                out.push_back({named ? inst->type->field_names[i] : std::to_string(i), inst->fields[i]});
            }
            break;
        }
        case ObjKind::Enum: {
            const auto* e = static_cast<const EnumObj*>(v.o);
            const VariantRT& var = e->type->variants[e->variant];
            for (std::size_t i = 0; i < e->fields.size(); ++i) {
                out.push_back({i < var.field_names.size() ? var.field_names[i] : std::to_string(i), e->fields[i]});
            }
            break;
        }
        case ObjKind::Cell: out.push_back({"value", static_cast<const CellObj*>(v.o)->value}); break;
        case ObjKind::Weak: {
            const Object* t = static_cast<const WeakObj*>(v.o)->target;
            out.push_back({"target", t != nullptr ? Value::object(const_cast<Object*>(t)) : Value::null()});
            break;
        }
        case ObjKind::Closure: {
            const auto* c = static_cast<const ClosureObj*>(v.o);
            for (std::size_t i = 0; i < c->captures.size(); ++i) {
                out.push_back({std::format("capture {}", i), c->captures[i]});
            }
            break;
        }
        default: break;
    }
    return out;
}

std::vector<std::uint32_t> VM::code_lines(std::string_view file) const {
    std::vector<std::uint32_t> lines;
    for (const auto& p : protos) {
        if (p->code.empty()) continue;
        std::string_view f;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        // A function's code lives in one source part. Glue (an empty span at
        // offset 0) would name the first part of a combined source instead.
        bool found = false;
        for (std::uint32_t pc = 0; pc < p->code.size() && !found; ++pc) {
            const bool glue = pc < p->spans.size() && p->spans[pc].begin == p->spans[pc].end;
            found = !glue && p->resolve_pc(pc, f, line, column);
        }
        if (!found || f != file) continue;
        for (std::uint32_t pc = 0; pc < p->code.size(); ++pc) {
            if (const std::uint32_t l = line_of(*p, pc); l != 0) lines.push_back(l);
        }
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

void VM::unwind_to(std::size_t depth) {
    while (frames_.size() > depth) {
        Frame f = frames_.back();
        leave_hook(f);
        for (std::uint32_t i = 0; i < f.fn->reg_count; ++i) assign_owned(stack_[f.base + i], Value{});
        frames_.pop_back();
        if (f.closure != nullptr) release(Value::object(const_cast<ClosureObj*>(f.closure)));
    }
    // Coroutines that were running in the unwound frames are stopped.
    while (!running_.empty() && running_.back().depth >= depth) {
        CoroObj* c = running_.back().coro;
        running_.pop_back();
        c->state = CoroObj::State::Cancelled;
        drop_saved(c);
        release(Value::object(c));
    }
}

RunResult VM::protect(const std::function<void()>& body) {
    std::size_t depth = frames_.size();
    RunResult result;
    try {
        body();
    } catch (const PanicSignal& p) {
        if (hook_panics_) {
            // Frames are intact until unwind_to: a debugger stops here.
            in_panic_ = true;
            hooks_->on_panic(*this, p.message);
            in_panic_ = false;
        }
        result.ok = false;
        result.panic = panic_info(p.message, depth);
        unwind_to(depth);
        suspended_ = false;
    }
    return result;
}

RunResult VM::run(std::uint32_t proto_index) {
    return protect([&] { release(invoke(proto_index, nullptr, 0)); });
}

RunResult VM::call(std::uint32_t proto_index, std::span<const Value> args, Value* out) {
    return protect([&] {
        Value r = invoke(proto_index, args.data(), static_cast<std::uint32_t>(args.size()));
        if (out != nullptr) {
            *out = r;
        } else {
            release(r);
        }
    });
}

namespace {

// Hosts may hold manual objects as plain references; script code gets them
// back as checked ones.
Value checked(const Value& v) {
    if (v.tag == Tag::Obj && v.ref == 0 && is_manual(v.o)) return manual_ref(v.o);
    return v;
}

} // namespace

RunResult VM::call_method(const Value& self, std::string_view name, std::span<const Value> args, Value* out) {
    const std::uint32_t* m = find_method(self, name);
    if (m == nullptr) {
        RunResult r;
        r.ok = false;
        r.panic = PanicInfo{};
        r.panic->message = std::format("value has no method '{}'", name);
        return r;
    }
    std::vector<Value> all;
    all.reserve(args.size() + 1);
    all.push_back(checked(self));
    for (const Value& a : args) all.push_back(checked(a));
    return call(*m, all, out);
}

RunResult VM::instantiate(const RecordRT* cls, Value* out, ArenaKind arena) {
    Value v;
    if (arena == ArenaKind::None) {
        v = Value::object(new_instance(cls, cls->field_names.size()));
    } else {
        InstanceObj* o = manual_instance(cls, cls->field_names.size(), arena);
        register_manual(o);
        v = manual_ref(o);
    }
    retain(v);
    RunResult r = protect([&] { release(invoke(cls->field_init, &v, 1)); });
    if (!r.ok) {
        if (v.ref != 0 && !is_stale(v)) free_manual(v.o, FreeSite{});
        release(v);
        v = Value::null();
    }
    *out = v;
    return r;
}

// ------------------------------------------------------------ coroutines

CoroObj* VM::start_coroutine(const Proto* fn, const Value* args, Object* owner) {
    CoroObj* c = new_coro();
    c->start_proto = fn->index;
    c->start_args.assign(args, args + fn->param_count);
    for (const Value& v : c->start_args) retain(v);
    // The owner is counted: a suspended coroutine keeps it alive (5.9).
    c->owner = owner;
    if (owner != nullptr) {
        ++owner->rc;
        if (is_manual(owner)) owner->flags |= objflag::Promised; // free() cancels its coroutines
    }
    c->rc = 2; // the scheduler's reference and the caller's (the Task value)
    coros_.push_back(c);
    resume(c);
    return c;
}

CoroObj* VM::create_coroutine(std::uint32_t fn, std::vector<Value> args, Object* owner) {
    CoroObj* c = new_coro();
    c->start_proto = fn;
    c->start_args = std::move(args);
    c->owner = owner;
    if (owner != nullptr) ++owner->rc;
    c->rc = 1; // the scheduler's
    coros_.push_back(c);
    return c;
}

RunResult VM::run_fresh() {
    RunResult first;
    std::size_t n = coros_.size();
    for (std::size_t i = 0; i < n; ++i) {
        CoroObj* c = coros_[i];
        if (c->state != CoroObj::State::Fresh) continue;
        RunResult r = protect([&] { resume(c); });
        if (!r.ok && first.ok) first = std::move(r);
    }
    return first;
}

void VM::resume(CoroObj* c) {
    std::size_t depth = frames_.size();
    if (c->state == CoroObj::State::Fresh) {
        const Proto* p = protos[c->start_proto].get();
        push_frame(p, nullptr, c->start_args.data(), p->param_count, 0, true);
        frames_.back().coro = c;
    } else {
        // Copy the heap frames back onto the stack (DESIGN.md 5.9).
        std::uint32_t base0 = stack_top();
        ensure_stack(static_cast<std::size_t>(base0) + c->regs.size() + 1);
        for (std::size_t i = 0; i < c->regs.size(); ++i) stack_[base0 + i] = c->regs[i]; // moved
        c->regs.clear();
        for (std::size_t k = 0; k < c->frames.size(); ++k) {
            const CoroObj::SavedFrame& sf = c->frames[k];
            frames_.push_back(Frame{sf.fn, sf.closure, sf.base + base0, sf.pc, sf.ret + base0, k == 0,
                                    k == 0 ? c : nullptr});
            enter_hook(frames_.back());
        }
        c->frames.clear();
        const Frame& top = frames_.back();
        assign_owned(stack_[top.base + c->await_dst], c->resume_value);
        c->resume_value = Value{};
        release(c->wait_value);
        c->wait_value = Value{};
        c->wait = CoroObj::Wait::None;
        c->signaled = false;
    }
    c->state = CoroObj::State::Running;
    ++c->rc;
    running_.push_back(Running{c, depth});
    Value r = execute(depth);
    running_.pop_back();
    if (suspended_) {
        suspended_ = false;
    } else {
        finish(c, r);
    }
    release(Value::object(c));
}

void VM::suspend(CoroObj::Wait wait, std::uint64_t wake_tick, Value wait_value, std::uint16_t dst) {
    // Find the running coroutine's base frame; only script frames may lie
    // between it and the `await`.
    std::size_t bi = frames_.size();
    CoroObj* c = nullptr;
    while (bi > 0) {
        --bi;
        if (frames_[bi].coro != nullptr) {
            c = frames_[bi].coro;
            break;
        }
        if (frames_[bi].discard) panic("cannot suspend a coroutine inside a native callback");
    }
    if (c == nullptr) panic("'await' outside of a running coroutine");
    retain(wait_value);
    std::uint32_t base0 = frames_[bi].base;
    std::uint32_t top = stack_top();
    if (c->cancel_requested) {
        unwind_to(bi + 1);
        Frame f = frames_.back();
        leave_hook(f);
        for (std::uint32_t i = 0; i < f.fn->reg_count; ++i) assign_owned(stack_[f.base + i], Value{});
        frames_.pop_back();
        release(wait_value);
        c->state = CoroObj::State::Cancelled;
        drop_saved(c);
        suspended_ = true;
        return;
    }
    c->regs.assign(stack_.begin() + base0, stack_.begin() + top);
    for (std::uint32_t i = base0; i < top; ++i) stack_[i] = Value{}; // moved into the coroutine
    c->frames.clear();
    for (std::size_t j = bi; j < frames_.size(); ++j) {
        const Frame& f = frames_[j];
        c->frames.push_back(CoroObj::SavedFrame{f.fn, f.closure, f.base - base0, f.pc, j == bi ? 0 : f.ret - base0});
    }
    for (std::size_t j = frames_.size(); j > bi; --j) leave_hook(frames_[j - 1]); // innermost first
    frames_.resize(bi);
    c->state = CoroObj::State::Suspended;
    c->wait = wait;
    c->wake_tick = wake_tick;
    assign_owned(c->wait_value, wait_value);
    c->await_dst = dst;
    suspended_ = true;
}

void VM::release_owner(CoroObj* c) {
    Object* o = c->owner;
    c->owner = nullptr;
    if (o != nullptr) release(Value::object(o));
}

void VM::drop_saved(CoroObj* c) {
    for (Value& v : c->regs) release(v);
    c->regs.clear();
    for (const CoroObj::SavedFrame& f : c->frames) {
        if (f.closure != nullptr) release(Value::object(const_cast<ClosureObj*>(f.closure)));
    }
    c->frames.clear();
    assign_owned(c->wait_value, Value{});
    assign_owned(c->resume_value, Value{});
    for (Value& v : c->start_args) release(v);
    c->start_args.clear();
    release_owner(c);
}

void VM::finish(CoroObj* c, const Value& result) {
    c->state = CoroObj::State::Done;
    assign_owned(c->result, result);
    for (Value& v : c->start_args) release(v);
    c->start_args.clear();
    release_owner(c);
}

void VM::cancel(CoroObj* c) {
    if (c->finished()) return;
    if (c->state == CoroObj::State::Running) {
        c->cancel_requested = true; // stops at its next `await`
        return;
    }
    c->state = CoroObj::State::Cancelled;
    drop_saved(c);
}

void VM::cancel_owned(const Object* owner) {
    if (owner == nullptr) return;
    for (CoroObj* c : coros_) {
        if (c->owner == owner) cancel(c);
    }
}

std::vector<CoroObj*> VM::coroutines() const {
    std::vector<CoroObj*> out;
    for (CoroObj* c : coros_) {
        if (!c->finished()) out.push_back(c);
    }
    return out;
}

RunResult VM::tick_coroutines(const FrozenFn& frozen) {
    ++tick_;
    RunResult first;
    std::size_t n = coros_.size();
    for (std::size_t i = 0; i < n; ++i) {
        CoroObj* c = coros_[i];
        if (c->state != CoroObj::State::Suspended && c->state != CoroObj::State::Fresh) continue;
        if (frozen && frozen(c->owner)) {
            if (c->wait == CoroObj::Wait::Ticks) ++c->wake_tick;
            continue;
        }
        RunResult r = protect([&] {
            bool ready = false;
            switch (c->wait) {
                case CoroObj::Wait::None: ready = true; break;
                case CoroObj::Wait::Ticks: ready = tick_ >= c->wake_tick; break;
                case CoroObj::Wait::Task: ready = static_cast<CoroObj*>(c->wait_value.o)->finished(); break;
                case CoroObj::Wait::Signal: ready = c->signaled; break;
                case CoroObj::Wait::Until: {
                    Value cond = c->wait_value;
                    retain(cond);
                    Value v = invoke_closure(static_cast<ClosureObj*>(cond.o), nullptr, 0);
                    release(cond);
                    ready = v.b;
                    break;
                }
            }
            if (ready && (c->state == CoroObj::State::Suspended || c->state == CoroObj::State::Fresh)) resume(c);
        });
        if (!r.ok) {
            cancel(c); // e.g. its `until` condition panicked
            if (first.ok) first = std::move(r);
        }
    }
    std::size_t w = 0;
    for (CoroObj* c : coros_) {
        if (c->finished()) {
            release(Value::object(c));
        } else {
            coros_[w++] = c;
        }
    }
    coros_.resize(w);
    return first;
}

void VM::emit_signal(SignalObj* signal, const Value* args, std::uint32_t argc) {
    // Copies: handlers may (dis)connect, and the stack may move under `args`.
    std::vector<Value> a(args, args + argc);
    for (const Value& v : a) retain(v);
    std::vector<Value> handlers = signal->handlers;
    for (const Value& h : handlers) retain(h);
    for (const Value& h : handlers) {
        Value r = invoke_closure(static_cast<ClosureObj*>(h.o), a.data(), argc);
        release(r);
    }
    for (const Value& h : handlers) release(h);
    // Coroutines awaiting the signal resume in the next scheduler pass.
    for (CoroObj* c : coros_) {
        if (c->state == CoroObj::State::Suspended && c->wait == CoroObj::Wait::Signal && !c->signaled &&
            c->wait_value.o == signal) {
            c->signaled = true;
            assign(c->resume_value, argc >= 1 ? a[0] : Value{});
        }
    }
    for (const Value& v : a) release(v);
}

// ------------------------------------------------------------ math hooks

float VM::m_sin(float x) const { return options_.math.sin != nullptr ? options_.math.sin(x) : std::sin(x); }
float VM::m_cos(float x) const { return options_.math.cos != nullptr ? options_.math.cos(x) : std::cos(x); }
float VM::m_tan(float x) const { return options_.math.tan != nullptr ? options_.math.tan(x) : std::tan(x); }
float VM::m_asin(float x) const { return options_.math.asin != nullptr ? options_.math.asin(x) : std::asin(x); }
float VM::m_acos(float x) const { return options_.math.acos != nullptr ? options_.math.acos(x) : std::acos(x); }
float VM::m_atan2(float y, float x) const {
    return options_.math.atan2 != nullptr ? options_.math.atan2(y, x) : std::atan2(y, x);
}
float VM::m_exp(float x) const { return options_.math.exp != nullptr ? options_.math.exp(x) : std::exp(x); }
float VM::m_log(float x) const { return options_.math.log != nullptr ? options_.math.log(x) : std::log(x); }
float VM::m_pow(float b, float e) const { return options_.math.pow != nullptr ? options_.math.pow(b, e) : std::pow(b, e); }

// ------------------------------------------------------------ generic ops

namespace {

Value num_result(NumKind k, std::int64_t i) { return Value::integer(k, i); }

} // namespace

Value VM::dyn_arith(Op op, const Value& a, const Value& b) {
    if (is_num_tag(a.tag) && a.tag == b.tag) {
        NumKind k = tag_num(a.tag);
        if (k == NumKind::F32) {
            float x = a.f;
            float y = b.f;
            switch (op) {
                case Op::Add: return Value::f32(x + y);
                case Op::Sub: return Value::f32(x - y);
                case Op::Mul: return Value::f32(x * y);
                case Op::Div: return Value::f32(x / y);
                default: return Value::f32(std::fmod(x, y));
            }
        }
        if (k == NumKind::F64) {
            double x = a.d;
            double y = b.d;
            switch (op) {
                case Op::Add: return Value::f64(x + y);
                case Op::Sub: return Value::f64(x - y);
                case Op::Mul: return Value::f64(x * y);
                case Op::Div: return Value::f64(x / y);
                default: return Value::f64(std::fmod(x, y));
            }
        }
        std::int64_t out = 0;
        num::Status st = num::int_arith(arith_binop(op), k, a.i, b.i, out);
        if (st == num::Status::DivByZero) panic("division by zero");
        if (st == num::Status::Overflow && !options_.wrap_overflow) {
            panic(std::format("integer overflow in '{}' ({})", arith_symbol(op), to_string(k)));
        }
        return num_result(k, out);
    }
    if (is_vec(a) && is_vec(b)) {
        Value v = vec_arith(op, opk::Vec, a, b);
        retain(v);
        return v;
    }
    if (op == Op::Add && a.is_obj() && b.is_obj() && a.o->kind == ObjKind::Str && b.o->kind == ObjKind::Str) {
        Value v = str_value(as_str(a)->s + as_str(b)->s);
        retain(v);
        return v;
    }
    if (a.is_obj()) {
        if (const std::uint32_t* m = find_method(a, method_for(op))) {
            Value args[2] = {a, b};
            retain(args[0]);
            retain(args[1]);
            Value r = invoke(*m, args, 2);
            release(args[0]);
            release(args[1]);
            return r;
        }
    }
    if (a.is_null()) panic("null reference");
    panic(std::format("operator '{}' is not supported for this value", arith_symbol(op)));
}

bool VM::dyn_equal(const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return a.is_null() && b.is_null();
    if (is_vec(a) || is_vec(b)) return is_vec(a) && is_vec(b) && vec_equal(a, b);
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case Tag::Bool: return a.b == b.b;
        case Tag::F32: return a.f == b.f;
        case Tag::F64: return a.d == b.d;
        case Tag::Obj: break;
        default: return a.i == b.i && a.aux == b.aux; // ints, names, plain enums
    }
    if (a.o == b.o) return true;
    if (a.o->kind != b.o->kind) return false;
    switch (a.o->kind) {
        case ObjKind::Str: return as_str(a)->s == as_str(b)->s;
        case ObjKind::Instance: {
            auto* x = static_cast<InstanceObj*>(a.o);
            auto* y = static_cast<InstanceObj*>(b.o);
            if (const std::uint32_t* m = find_method(a, "eq")) {
                Value args[2] = {a, b};
                retain(args[0]);
                retain(args[1]);
                Value r = invoke(*m, args, 2);
                release(args[0]);
                release(args[1]);
                return r.b;
            }
            if (!x->type->is_struct || x->type != y->type) return false; // classes: identity
            for (std::size_t i = 0; i < x->fields.size(); ++i) {
                if (!dyn_equal(x->fields[i], y->fields[i])) return false;
            }
            return true;
        }
        case ObjKind::Enum: {
            auto* x = static_cast<EnumObj*>(a.o);
            auto* y = static_cast<EnumObj*>(b.o);
            if (x->type != y->type || x->variant != y->variant) return false;
            for (std::size_t i = 0; i < x->fields.size(); ++i) {
                if (!dyn_equal(x->fields[i], y->fields[i])) return false;
            }
            return true;
        }
        case ObjKind::Array: {
            auto* x = static_cast<ListObj*>(a.o);
            auto* y = static_cast<ListObj*>(b.o);
            if (x->items.size() != y->items.size()) return false;
            for (std::size_t i = 0; i < x->items.size(); ++i) {
                if (!dyn_equal(x->items[i], y->items[i])) return false;
            }
            return true;
        }
        default: return false; // lists, maps, closures, tasks, signals: identity
    }
}

bool VM::dyn_less(const Value& a, const Value& b, bool or_equal) {
    if (is_num_tag(a.tag) && a.tag == b.tag) {
        NumKind k = tag_num(a.tag);
        if (k == NumKind::F32) return or_equal ? a.f <= b.f : a.f < b.f;
        if (k == NumKind::F64) return or_equal ? a.d <= b.d : a.d < b.d;
        return or_equal ? !num::int_less(k, b.i, a.i) : num::int_less(k, a.i, b.i);
    }
    if (a.is_obj() && b.is_obj() && a.o->kind == ObjKind::Str && b.o->kind == ObjKind::Str) {
        int c = as_str(a)->s.compare(as_str(b)->s);
        return or_equal ? c <= 0 : c < 0;
    }
    if (a.is_obj()) {
        if (const std::uint32_t* m = find_method(a, "cmp")) {
            Value args[2] = {a, b};
            retain(args[0]);
            retain(args[1]);
            Value r = invoke(*m, args, 2);
            release(args[0]);
            release(args[1]);
            return or_equal ? r.i <= 0 : r.i < 0;
        }
    }
    if (a.is_null() || b.is_null()) panic("null reference");
    panic("values of this type cannot be ordered");
}

Value VM::copy_value(const Value& v) {
    if (v.is_obj() && v.o->kind == ObjKind::Array) {
        // [T; N] is a value (LANGUAGE.md 9.16): copied deeply, like structs.
        auto* src = static_cast<ListObj*>(v.o);
        ListObj* dst = new_array((v.o->flags & objflag::Untraced) != 0);
        dst->items.resize(src->items.size());
        for (std::size_t i = 0; i < src->items.size(); ++i) assign(dst->items[i], copy_value(src->items[i]));
        return Value::object(dst);
    }
    if (!v.is_obj() || v.o->kind != ObjKind::Instance) return v;
    auto* src = static_cast<InstanceObj*>(v.o);
    if (!src->type->is_struct) return v;
    InstanceObj* dst = new_instance(src->type, src->fields.size());
    for (std::size_t i = 0; i < src->fields.size(); ++i) assign(dst->fields[i], copy_value(src->fields[i]));
    return Value::object(dst);
}

const std::uint32_t* VM::find_method(const Value& receiver, std::string_view name) {
    if (!receiver.is_obj() || receiver.o->kind != ObjKind::Instance) return nullptr;
    const RecordRT* t = static_cast<InstanceObj*>(receiver.o)->type;
    auto it = t->methods.find(name);
    return it != t->methods.end() ? &it->second : nullptr;
}

std::string VM::format(const Value& v, bool repr) {
    std::string out;
    format_into(out, v, repr, 0);
    return out;
}

std::string VM::describe(const Value& v, std::size_t max_length) {
    std::string out;
    const bool scripts = format_scripts_;
    const std::size_t limit = format_limit_;
    format_scripts_ = false;
    format_limit_ = max_length;
    format_into(out, v, true, 0);
    format_scripts_ = scripts;
    format_limit_ = limit;
    if (out.size() > max_length) {
        // Cut on a UTF-8 boundary.
        std::size_t cut = max_length;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) --cut;
        out.resize(cut);
        out += "...";
    }
    return out;
}

void VM::format_into(std::string& out, const Value& v, bool repr, int depth) {
    if (out.size() > format_limit_) return; // describe(): enough text
    switch (v.tag) {
        case Tag::Nil: out += "<void>"; return;
        case Tag::Null: out += "null"; return;
        case Tag::Bool: out += v.b ? "true" : "false"; return;
        case Tag::U64: out += std::to_string(static_cast<std::uint64_t>(v.i)); return;
        case Tag::F32: format_float(out, v.f); return;
        case Tag::F64: format_float(out, v.d); return;
        case Tag::Name: out += "#" + names_[static_cast<std::size_t>(v.i)]; return;
        case Tag::Enum: {
            const EnumRT* e = enums[v.aux].get();
            for (const VariantRT& var : e->variants) {
                if (var.discriminant == v.i) {
                    out += e->name + "." + var.name;
                    return;
                }
            }
            out += e->name + "(" + std::to_string(v.i) + ")";
            return;
        }
        case Tag::Vec:
        case Tag::Obj: break;
        default: out += std::to_string(v.i); return; // other integers
    }
    if (is_vec(v)) {
        std::uint32_t n = vec_size(v);
        const float* d = vec_data(v);
        out += "vec" + std::to_string(n) + "(";
        for (std::uint32_t i = 0; i < n; ++i) {
            if (i > 0) out += ", ";
            format_float(out, d[i]);
        }
        out += ")";
        return;
    }
    if (is_stale(v)) {
        out += "<freed>";
        return;
    }
    if (depth > 8) {
        out += "...";
        return;
    }
    switch (v.o->kind) {
        case ObjKind::Str:
            if (repr) {
                quote(out, as_str(v)->s);
            } else {
                out += as_str(v)->s;
            }
            return;
        case ObjKind::List:
        case ObjKind::Array: {
            auto* l = static_cast<ListObj*>(v.o);
            out += "[";
            for (std::size_t i = 0; i < l->items.size() && out.size() <= format_limit_; ++i) {
                if (i > 0) out += ", ";
                format_into(out, l->items[i], true, depth + 1);
            }
            out += "]";
            return;
        }
        case ObjKind::Map:
        case ObjKind::Set: {
            // {k: v, ...} and Set[a, b], in insertion order.
            auto* h = static_cast<HashObj*>(v.o);
            const bool map = v.o->kind == ObjKind::Map;
            out += map ? "{" : "Set[";
            bool first = true;
            for (std::size_t i = 0; i < h->keys.size() && out.size() <= format_limit_; ++i) {
                if (h->keys[i].tag == Tag::Nil) continue;
                if (!first) out += ", ";
                first = false;
                format_into(out, h->keys[i], true, depth + 1);
                if (map) {
                    out += ": ";
                    format_into(out, h->vals[i], true, depth + 1);
                }
            }
            out += map ? "}" : "]";
            return;
        }
        case ObjKind::Deque: {
            auto* d = static_cast<DequeObj*>(v.o);
            out += "Deque[";
            for (std::size_t i = 0; i < d->count && out.size() <= format_limit_; ++i) {
                if (i > 0) out += ", ";
                format_into(out, d->at(i), true, depth + 1);
            }
            out += "]";
            return;
        }
        case ObjKind::PQueue:
            out += "PriorityQueue(len = " + std::to_string(static_cast<PQueueObj*>(v.o)->items.size()) + ")";
            return;
        case ObjKind::StrBuilder:
            out += "StrBuilder(";
            quote(out, static_cast<StrBuilderObj*>(v.o)->s);
            out += ")";
            return;
        case ObjKind::Instance: {
            const std::uint32_t* m = format_scripts_ ? find_method(v, "to_str") : nullptr;
            if (m != nullptr) {
                Value self = v;
                retain(self);
                Value r = invoke(*m, &self, 1);
                release(self);
                if (r.is_obj() && r.o->kind == ObjKind::Str) out += as_str(r)->s;
                release(r);
                return;
            }
            auto* inst = static_cast<InstanceObj*>(v.o);
            if (!format_scripts_ && depth > 0) {
                // describe(): nested objects stay short (node trees link to
                // their parents, so expanding them repeats the whole tree).
                out += inst->type->name + (inst->fields.empty() ? " {}" : " {...}");
                return;
            }
            out += inst->type->name;
            if (inst->fields.empty()) {
                out += " {}";
                return;
            }
            out += " { ";
            for (std::size_t i = 0; i < inst->fields.size(); ++i) {
                if (i > 0) out += ", ";
                out += inst->type->field_names[i] + " = ";
                format_into(out, inst->fields[i], true, depth + 1);
            }
            out += " }";
            return;
        }
        case ObjKind::Enum: {
            auto* e = static_cast<EnumObj*>(v.o);
            const VariantRT& var = e->type->variants[e->variant];
            if (e->type == result_rt()) {
                out += var.name + "(";
                format_into(out, e->fields[0], true, depth + 1);
                out += ")";
                return;
            }
            out += e->type->name + "." + var.name;
            if (e->fields.empty()) return;
            out += "(";
            for (std::size_t i = 0; i < e->fields.size(); ++i) {
                if (i > 0) out += ", ";
                out += var.field_names[i] + " = ";
                format_into(out, e->fields[i], true, depth + 1);
            }
            out += ")";
            return;
        }
        case ObjKind::Closure: out += "<fn " + static_cast<ClosureObj*>(v.o)->fn->name + ">"; return;
        case ObjKind::Cell:
            if (!format_scripts_) {
                // describe(): a captured local shows its value.
                format_into(out, static_cast<CellObj*>(v.o)->value, true, depth + 1);
                return;
            }
            out += "<cell>";
            return;
        case ObjKind::Vec: return; // handled above
        case ObjKind::Coro: out += "<task " + protos[static_cast<CoroObj*>(v.o)->start_proto]->name + ">"; return;
        case ObjKind::Signal: out += "<signal>"; return;
        case ObjKind::Weak: out += "<weak>"; return;
    }
}

// ------------------------------------------------------------ natives

Value VM::call_native(std::uint16_t id, Value* args, std::uint32_t argc) {
    if (id >= static_cast<std::uint16_t>(Builtin::ParseInt) && id < static_cast<std::uint16_t>(Builtin::Echo)) {
        return call_lib(id, args, argc);
    }
    auto list_of = [&](const Value& v) -> ListObj* {
        if (!v.is_obj()) panic("null reference: the list is null");
        if (is_stale(v)) stale_panic(v, "the list");
        return static_cast<ListObj*>(v.o);
    };
    auto index_of = [&](const Value& v, std::size_t size, bool allow_end) -> std::size_t {
        std::int64_t i = v.i;
        if (i < 0 || static_cast<std::size_t>(i) > size || (!allow_end && static_cast<std::size_t>(i) == size)) {
            panic(std::format("index {} is out of bounds for a list of length {}", i, size));
        }
        return static_cast<std::size_t>(i);
    };
    auto owned = [](Value v) {
        retain(v);
        return v;
    };
    auto num_less = [](const Value& a, const Value& b) {
        switch (a.tag) {
            case Tag::F32: return a.f < b.f;
            case Tag::F64: return a.d < b.d;
            default: return num::int_less(tag_num(a.tag), a.i, b.i);
        }
    };
    auto float_fn = [&](const Value& x, float (*f32)(float), double (*f64)(double)) {
        if (x.tag == Tag::F32) return Value::f32(f32(x.f));
        return Value::f64(f64(x.d));
    };
    // Vector helpers: results are owned.
    auto vec_out = [&](const float* d, std::uint32_t n) { return owned(vec_value(d, n)); };
    auto coro_of = [&](const Value& v) -> CoroObj* {
        if (!v.is_obj()) panic("null reference: the task is null");
        return static_cast<CoroObj*>(v.o);
    };
    auto signal_of = [&](const Value& v) -> SignalObj* {
        if (!v.is_obj()) panic("null reference: the signal's object is null");
        return static_cast<SignalObj*>(v.o);
    };
    switch (static_cast<Builtin>(id)) {
        case Builtin::Print: write(as_str(args[0])->s + "\n"); return {};
        case Builtin::Echo: {
            Value v = args[0];
            retain(v);
            std::string s = format(v, true);
            release(v);
            write(s + "\n");
            return {};
        }
        case Builtin::Assert:
            if (!args[0].b) {
                const std::string& msg = as_str(args[1])->s;
                panic(msg.empty() ? std::string("assertion failed") : "assertion failed: " + msg);
            }
            return {};
        case Builtin::Panic: panic(as_str(args[0])->s);
        case Builtin::Min: return num_less(args[1], args[0]) ? args[1] : args[0];
        case Builtin::Max: return num_less(args[0], args[1]) ? args[1] : args[0];
        case Builtin::Clamp: {
            Value x = args[0];
            if (num_less(x, args[1])) x = args[1];
            if (num_less(args[2], x)) x = args[2];
            return x;
        }
        case Builtin::Abs: {
            Value x = args[0];
            if (x.tag == Tag::F32) return Value::f32(std::fabs(x.f));
            if (x.tag == Tag::F64) return Value::f64(std::fabs(x.d));
            if (x.i >= 0) return x;
            std::int64_t out = 0;
            NumKind k = tag_num(x.tag);
            if (num::int_neg(k, x.i, out) != num::Status::Ok && !options_.wrap_overflow) {
                panic(std::format("integer overflow in 'abs' ({})", to_string(k)));
            }
            return Value::integer(k, out);
        }
        case Builtin::Sign: {
            Value x = args[0];
            if (x.tag == Tag::F32) return Value::f32(x.f > 0.0f ? 1.0f : (x.f < 0.0f ? -1.0f : 0.0f));
            if (x.tag == Tag::F64) return Value::f64(x.d > 0.0 ? 1.0 : (x.d < 0.0 ? -1.0 : 0.0));
            NumKind k = tag_num(x.tag);
            return Value::integer(k, x.i > 0 ? 1 : (x.i < 0 ? -1 : 0));
        }
        case Builtin::Sqrt:
            // IEEE 754 square root is correctly rounded: deterministic.
            if (args[0].tag == Tag::F32) return Value::f32(std::sqrt(args[0].f));
            return Value::f64(std::sqrt(args[0].d));
        // floor/ceil/round are exact operations: deterministic everywhere.
        case Builtin::Floor:
            return float_fn(args[0], [](float x) { return std::floor(x); }, [](double x) { return std::floor(x); });
        case Builtin::Ceil:
            return float_fn(args[0], [](float x) { return std::ceil(x); }, [](double x) { return std::ceil(x); });
        case Builtin::Round:
            return float_fn(args[0], [](float x) { return std::round(x); }, [](double x) { return std::round(x); });
        case Builtin::Sin: return Value::f32(m_sin(args[0].f));
        case Builtin::Cos: return Value::f32(m_cos(args[0].f));
        case Builtin::Tan: return Value::f32(m_tan(args[0].f));
        case Builtin::Asin: return Value::f32(m_asin(args[0].f));
        case Builtin::Acos: return Value::f32(m_acos(args[0].f));
        case Builtin::Atan: return Value::f32(m_atan2(args[0].f, 1.0f));
        case Builtin::Atan2: return Value::f32(m_atan2(args[0].f, args[1].f));
        case Builtin::Exp: return Value::f32(m_exp(args[0].f));
        case Builtin::Log: return Value::f32(m_log(args[0].f));
        case Builtin::Log2: return Value::f32(m_log(args[0].f) * 1.44269504088896340736f);
        case Builtin::Log10: return Value::f32(m_log(args[0].f) * 0.43429448190325182765f);
        case Builtin::Pow: return Value::f32(m_pow(args[0].f, args[1].f));
        case Builtin::Lerp: return Value::f32(args[0].f + (args[1].f - args[0].f) * args[2].f);
        case Builtin::InverseLerp: {
            float a = args[0].f, b = args[1].f, v = args[2].f;
            return Value::f32(b == a ? 0.0f : (v - a) / (b - a));
        }
        case Builtin::Remap: {
            float in_min = args[0].f, in_max = args[1].f, out_min = args[2].f, out_max = args[3].f, v = args[4].f;
            float t = in_max == in_min ? 0.0f : (v - in_min) / (in_max - in_min);
            return Value::f32(out_min + (out_max - out_min) * t);
        }
        case Builtin::Smoothstep: {
            float e0 = args[0].f, e1 = args[1].f, x = args[2].f;
            if (e0 == e1) return Value::f32(x < e0 ? 0.0f : 1.0f);
            float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            return Value::f32(t * t * (3.0f - 2.0f * t));
        }
        case Builtin::Step:
            return Value::f32(args[1].f < args[0].f ? 0.0f : 1.0f);
        case Builtin::MoveToward: {
            float from = args[0].f, to = args[1].f, delta = args[2].f;
            if (std::fabs(to - from) <= delta) return Value::f32(to);
            return Value::f32(from + (to > from ? delta : -delta));
        }
        case Builtin::RadToDeg: return Value::f32(args[0].f * (180.0f / 3.14159265358979323846f));
        case Builtin::DegToRad: return Value::f32(args[0].f * (3.14159265358979323846f / 180.0f));
        case Builtin::Wrap: {
            float val = args[0].f, mn = args[1].f, mx = args[2].f;
            float range = mx - mn;
            return Value::f32(range == 0.0f ? mn : val - range * std::floor((val - mn) / range));
        }
        case Builtin::WrapAngle: {
            float angle = args[0].f;
            constexpr float pi = 3.14159265358979323846f;
            constexpr float tau = 6.28318530717958647692f;
            return Value::f32((angle + pi) - tau * std::floor((angle + pi) / tau) - pi);
        }
        case Builtin::FromAngle: {
            const float d[2] = {m_cos(args[0].f) * args[1].f, m_sin(args[0].f) * args[1].f};
            return vec_out(d, 2);
        }
        case Builtin::VecLength: return Value::f32(std::sqrt(vec_dot(args[0], args[0])));
        case Builtin::VecLengthSq: return Value::f32(vec_dot(args[0], args[0]));
        case Builtin::VecDot: return Value::f32(vec_dot(args[0], args[1]));
        case Builtin::VecNormalized: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            float len = std::sqrt(vec_dot(args[0], args[0]));
            float out[max_vec];
            // The zero vector stays zero (no NaNs from 0 / 0).
            for (std::uint32_t i = 0; i < n; ++i) out[i] = len > 0.0f ? a[i] / len : 0.0f;
            return vec_out(out, n);
        }
        case Builtin::VecDistance:
        case Builtin::VecDistanceSq: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            const float* b = vec_data(args[1]);
            float s = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) s += (a[i] - b[i]) * (a[i] - b[i]);
            return Value::f32(static_cast<Builtin>(id) == Builtin::VecDistance ? std::sqrt(s) : s);
        }
        case Builtin::VecLerp: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            const float* b = vec_data(args[1]);
            float t = args[2].f;
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
            return vec_out(out, n);
        }
        case Builtin::VecAbs:
        case Builtin::VecMin:
        case Builtin::VecMax:
        case Builtin::VecClamp: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) {
                float x = a[i];
                switch (static_cast<Builtin>(id)) {
                    case Builtin::VecAbs: x = std::fabs(x); break;
                    case Builtin::VecMin: x = std::min(x, vec_data(args[1])[i]); break;
                    case Builtin::VecMax: x = std::max(x, vec_data(args[1])[i]); break;
                    default: x = std::min(std::max(x, vec_data(args[1])[i]), vec_data(args[2])[i]); break;
                }
                out[i] = x;
            }
            return vec_out(out, n);
        }
        case Builtin::VecReflect: {
            std::uint32_t n = vec_size(args[0]);
            const float* v = vec_data(args[0]);
            const float* norm = vec_data(args[1]);
            float d = vec_dot(args[0], args[1]);
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) out[i] = v[i] - 2.0f * d * norm[i];
            return vec_out(out, n);
        }
        case Builtin::VecProject: {
            std::uint32_t n = vec_size(args[0]);
            const float* onto = vec_data(args[1]);
            float onto_len_sq = vec_dot(args[1], args[1]);
            float scale = onto_len_sq > 0.0f ? vec_dot(args[0], args[1]) / onto_len_sq : 0.0f;
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) out[i] = scale * onto[i];
            return vec_out(out, n);
        }
        case Builtin::VecSlide: {
            std::uint32_t n = vec_size(args[0]);
            const float* v = vec_data(args[0]);
            const float* norm = vec_data(args[1]);
            float norm_len_sq = vec_dot(args[1], args[1]);
            float scale = norm_len_sq > 0.0f ? vec_dot(args[0], args[1]) / norm_len_sq : 0.0f;
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) out[i] = v[i] - scale * norm[i];
            return vec_out(out, n);
        }
        case Builtin::VecClampLength: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            float len = std::sqrt(vec_dot(args[0], args[0]));
            float min_len = args[1].f;
            float max_len = args[2].f;
            float target_len = std::min(std::max(len, min_len), max_len);
            float scale = len > 0.0f ? target_len / len : 0.0f;
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) out[i] = a[i] * scale;
            return vec_out(out, n);
        }
        case Builtin::VecRotated: {
            const float* a = vec_data(args[0]);
            float c = m_cos(args[1].f);
            float s = m_sin(args[1].f);
            // Y-down: a positive angle turns clockwise on screen (DESIGN.md 7.1).
            const float d[2] = {a[0] * c - a[1] * s, a[0] * s + a[1] * c};
            return vec_out(d, 2);
        }
        case Builtin::VecAngle: return Value::f32(m_atan2(vec_data(args[0])[1], vec_data(args[0])[0]));
        case Builtin::VecPerpendicular: {
            const float* a = vec_data(args[0]);
            const float d[2] = {-a[1], a[0]};
            return vec_out(d, 2);
        }
        case Builtin::VecAngleTo: {
            const float* a = vec_data(args[0]);
            const float* b = vec_data(args[1]);
            float cross2 = a[0] * b[1] - a[1] * b[0];
            float dot2 = a[0] * b[0] + a[1] * b[1];
            return Value::f32(m_atan2(cross2, dot2));
        }
        case Builtin::VecSlerp: {
            const float* a = vec_data(args[0]);
            const float* b = vec_data(args[1]);
            float t = args[2].f;
            float cross2 = a[0] * b[1] - a[1] * b[0];
            float dot2 = a[0] * b[0] + a[1] * b[1];
            float theta = m_atan2(cross2, dot2);
            float len_a = std::sqrt(a[0] * a[0] + a[1] * a[1]);
            float len_b = std::sqrt(b[0] * b[0] + b[1] * b[1]);
            float len = len_a + (len_b - len_a) * t;
            float ang = m_atan2(a[1], a[0]) + theta * t;
            const float d[2] = {m_cos(ang) * len, m_sin(ang) * len};
            return vec_out(d, 2);
        }
        case Builtin::VecCross2: {
            const float* a = vec_data(args[0]);
            const float* b = vec_data(args[1]);
            return Value::f32(a[0] * b[1] - a[1] * b[0]);
        }
        case Builtin::VecCross3: {
            const float* a = vec_data(args[0]);
            const float* b = vec_data(args[1]);
            const float d[3] = {
                a[1] * b[2] - a[2] * b[1],
                a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0]
            };
            return vec_out(d, 3);
        }
        // ---- Milestone 9.2b integer arithmetic (LANGUAGE.md 9.20). One integer
        // kind; int_arith always yields the wrapped two's-complement result.
        case Builtin::WrappingAdd:
        case Builtin::WrappingSub:
        case Builtin::WrappingMul:
        case Builtin::CheckedAdd:
        case Builtin::CheckedSub:
        case Builtin::CheckedMul:
        case Builtin::SaturatingAdd:
        case Builtin::SaturatingSub:
        case Builtin::SaturatingMul: {
            int rel = static_cast<int>(id) - static_cast<int>(Builtin::WrappingAdd);
            int which = rel / 3; // 0 wrapping, 1 checked, 2 saturating
            int opi = rel % 3;   // 0 add, 1 sub, 2 mul
            BinaryOp op = opi == 0 ? BinaryOp::Add : (opi == 1 ? BinaryOp::Sub : BinaryOp::Mul);
            NumKind k = tag_num(args[0].tag);
            std::int64_t out = 0;
            bool overflow = num::int_arith(op, k, args[0].i, args[1].i, out) == num::Status::Overflow;
            if (!overflow || which == 0) return Value::integer(k, out);
            if (which == 1) return Value::null();
            std::int64_t lo = num::min_of(k);
            std::int64_t hi = num::wrap(k, num::max_of(k));
            bool up;
            if (!is_signed(k)) {
                up = op != BinaryOp::Sub;
            } else if (op == BinaryOp::Add) {
                up = args[1].i > 0;
            } else if (op == BinaryOp::Sub) {
                up = args[1].i < 0;
            } else {
                up = (args[0].i < 0) == (args[1].i < 0);
            }
            return Value::integer(k, up ? hi : lo);
        }
        // ---- Milestone 9.2b vector methods. Only IEEE-exact operations.
        case Builtin::VecFloor:
        case Builtin::VecCeil:
        case Builtin::VecRound:
        case Builtin::VecFract:
        case Builtin::VecSign: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) {
                float x = a[i];
                switch (static_cast<Builtin>(id)) {
                    case Builtin::VecFloor: x = std::floor(x); break;
                    case Builtin::VecCeil: x = std::ceil(x); break;
                    case Builtin::VecRound: x = std::round(x); break;
                    case Builtin::VecFract: x = x - std::floor(x); break;
                    default: x = x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); break;
                }
                out[i] = x;
            }
            return vec_out(out, n);
        }
        case Builtin::VecSnapped: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            float step = args[1].f;
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) out[i] = step == 0.0f ? a[i] : std::floor(a[i] / step + 0.5f) * step;
            return vec_out(out, n);
        }
        case Builtin::VecBounce: { // the reflection's opposite: a ball hitting a wall
            std::uint32_t n = vec_size(args[0]);
            const float* v = vec_data(args[0]);
            const float* norm = vec_data(args[1]);
            float d = vec_dot(args[0], args[1]);
            float out[max_vec];
            for (std::uint32_t i = 0; i < n; ++i) out[i] = -(v[i] - 2.0f * d * norm[i]);
            return vec_out(out, n);
        }
        case Builtin::VecMoveToward: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            const float* b = vec_data(args[1]);
            float delta = args[2].f;
            float diff[max_vec];
            float sq = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                diff[i] = b[i] - a[i];
                sq += diff[i] * diff[i];
            }
            float len = std::sqrt(sq);
            float out[max_vec];
            if (len <= delta || len == 0.0f) {
                for (std::uint32_t i = 0; i < n; ++i) out[i] = b[i];
            } else {
                for (std::uint32_t i = 0; i < n; ++i) out[i] = a[i] + diff[i] / len * delta;
            }
            return vec_out(out, n);
        }
        case Builtin::VecDirectionTo: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            const float* b = vec_data(args[1]);
            float diff[max_vec];
            float sq = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                diff[i] = b[i] - a[i];
                sq += diff[i] * diff[i];
            }
            float len = std::sqrt(sq);
            for (std::uint32_t i = 0; i < n; ++i) diff[i] = len > 0.0f ? diff[i] / len : 0.0f;
            return vec_out(diff, n);
        }
        case Builtin::VecLimitLength: {
            std::uint32_t n = vec_size(args[0]);
            const float* a = vec_data(args[0]);
            float len = std::sqrt(vec_dot(args[0], args[0]));
            float max_len = args[1].f;
            float out[max_vec];
            bool shrink = len > 0.0f && len > max_len;
            for (std::uint32_t i = 0; i < n; ++i) out[i] = shrink ? a[i] / len * max_len : a[i];
            return vec_out(out, n);
        }
        case Builtin::VecSetAt: {
            std::uint32_t n = vec_size(args[0]);
            std::int64_t i = args[1].i;
            if (i < 0 || i >= static_cast<std::int64_t>(n)) panic(std::format("index {} is out of bounds for a vec{}", i, n));
            float out[max_vec];
            const float* a = vec_data(args[0]);
            for (std::uint32_t j = 0; j < n; ++j) out[j] = a[j];
            out[i] = args[2].f;
            return vec_out(out, n);
        }
        case Builtin::TaskCancel: cancel(coro_of(args[0])); return {};
        case Builtin::TaskIsDone: return Value::boolean(coro_of(args[0])->finished());
        case Builtin::SignalConnect: {
            SignalObj* s = signal_of(args[0]);
            if (!args[1].is_obj()) panic("null reference: cannot connect a null function");
            s->handlers.emplace_back();
            assign(s->handlers.back(), args[1]);
            return {};
        }
        case Builtin::SignalDisconnect: {
            SignalObj* s = signal_of(args[0]);
            for (auto it = s->handlers.begin(); it != s->handlers.end(); ++it) {
                if (it->is_obj() && args[1].is_obj() && it->o == args[1].o) {
                    Value old = *it;
                    s->handlers.erase(it);
                    release(old);
                    break;
                }
            }
            return {};
        }
        case Builtin::SignalEmit: emit_signal(signal_of(args[0]), args + 1, argc - 1); return {};
        case Builtin::ListNew: return owned(Value::object(new_list()));
        case Builtin::ListLen:
            return Value::integer(NumKind::I32, static_cast<std::int64_t>(list_of(args[0])->items.size()));
        case Builtin::ListPush: {
            ListObj* l = list_of(args[0]);
            l->items.emplace_back();
            assign(l->items.back(), args[1]);
            return {};
        }
        case Builtin::ListPop: {
            ListObj* l = list_of(args[0]);
            if (l->items.empty()) panic("pop() on an empty list");
            Value v = l->items.back(); // the list's reference moves to the caller
            l->items.pop_back();
            return v;
        }
        case Builtin::ListClear: {
            ListObj* l = list_of(args[0]);
            std::vector<Value> items;
            items.swap(l->items);
            for (Value& v : items) release(v);
            return {};
        }
        case Builtin::ListInsert: {
            ListObj* l = list_of(args[0]);
            std::size_t i = index_of(args[1], l->items.size(), true);
            l->items.insert(l->items.begin() + static_cast<std::ptrdiff_t>(i), Value{});
            assign(l->items[i], args[2]);
            return {};
        }
        case Builtin::ListRemove: {
            ListObj* l = list_of(args[0]);
            std::size_t i = index_of(args[1], l->items.size(), false);
            Value v = l->items[i];
            l->items.erase(l->items.begin() + static_cast<std::ptrdiff_t>(i));
            return v;
        }
        case Builtin::ListIsEmpty: return Value::boolean(list_of(args[0])->items.empty());
        case Builtin::ListContains: {
            ListObj* l = list_of(args[0]);
            Value x = args[1];
            retain(x);
            bool found = false;
            for (std::size_t i = 0; i < l->items.size() && !found; ++i) found = dyn_equal(l->items[i], x);
            release(x);
            return Value::boolean(found);
        }
        case Builtin::StrLen: return Value::integer(NumKind::I32, static_cast<std::int64_t>(as_str(args[0])->s.size()));
        case Builtin::StrContains:
            return Value::boolean(as_str(args[0])->s.find(as_str(args[1])->s) != std::string::npos);
        case Builtin::StrStartsWith: return Value::boolean(as_str(args[0])->s.starts_with(as_str(args[1])->s));
        case Builtin::StrEndsWith: return Value::boolean(as_str(args[0])->s.ends_with(as_str(args[1])->s));
        case Builtin::Count: break;
        default: break; // the 9.2a library: call_lib
    }
    (void)argc;
    panic("unknown native function");
}

// ------------------------------------------------------------ interpreter

Value VM::execute(std::size_t stop_depth) {
    return hook_lines_ ? execute_loop<true>(stop_depth) : execute_loop<false>(stop_depth);
}

template <bool LineHooks>
Value VM::execute_loop(std::size_t stop_depth) {
    if (!frames_.empty() && frames_.back().fn->aot_fn != nullptr) {
        Value out{};
        while (frames_.size() > stop_depth) {
            Frame* top = &frames_.back();
            if (top->fn->aot_fn != nullptr) {
                if (!top->fn->aot_fn(*this, &out)) return Value{};
                if (frames_.size() == stop_depth) return out;
                continue;
            }
            break;
        }
        if (frames_.size() <= stop_depth) return out;
    }
    Frame* f = &frames_.back();
    Value* R = stack_.data() + f->base;
    const Instr* code = f->fn->code.data();
    const Value* K = f->fn->consts.data();
    auto reload = [&]() {
        f = &frames_.back();
        R = stack_.data() + f->base;
        code = f->fn->code.data();
        K = f->fn->consts.data();
    };
    auto span_text = [&]() -> std::string {
        return std::string(f->fn->slice_pc(f->pc - 1));
    };
    auto null_panic = [&]() {
        std::string text = span_text();
        if (text.empty()) panic("null reference");
        panic(std::format("null reference: '{}' is null", text));
    };
    // Dereferences: a checked (manual) reference must still be live unless
    // the instruction is in `unsafe { }` (DESIGN.md 5.10).
    auto stale = [&](const Value& v) {
        std::string text = span_text();
        stale_panic(v, text.empty() ? std::string("the object") : std::format("'{}'", text));
    };
    auto instance = [&](const Value& v, std::uint8_t k = 0) -> InstanceObj* {
        if (!v.is_obj()) null_panic();
        if (v.ref != 0 && (k & opk::Unchecked) == 0 && v.o->gen != v.aux) [[unlikely]] stale(v);
        return static_cast<InstanceObj*>(v.o);
    };
    auto list = [&](const Value& v, std::uint8_t k = 0) -> ListObj* {
        if (!v.is_obj()) null_panic();
        if (v.ref != 0 && (k & opk::Unchecked) == 0 && v.o->gen != v.aux) [[unlikely]] stale(v);
        return static_cast<ListObj*>(v.o);
    };
    auto cell = [](const Value& v) { return static_cast<CellObj*>(v.o); };
    auto overflow = [&](Op op, NumKind k) {
        panic(std::format("integer overflow in '{}' ({})", arith_symbol(op), to_string(k)));
    };
    auto call = [&](const Proto* fn, const ClosureObj* closure, std::uint16_t a, std::uint16_t args) {
        push_frame(fn, closure, R + args, fn->param_count, f->base + a, false);
        reload();
    };
    // `start(f(...))`: Call/CallV with k = 1 (no owner) or 2 (owner just
    // below the arguments).
    auto start = [&](const Proto* fn, const Instr& in) {
        Object* owner = nullptr;
        if (in.k == 2 && R[in.c - 1].is_obj()) owner = R[in.c - 1].o;
        CoroObj* c = start_coroutine(fn, R + in.c, owner);
        reload();
        assign_owned(R[in.a], Value::object(c));
    };

    for (;;) {
        if constexpr (LineHooks) {
            if (hook_lines_) line_hook(*f); // hooks removed while running: stop reporting
        }
        const Instr in = code[f->pc++];
        switch (in.op) {
            case Op::Nop: break;
            case Op::Mov: assign(R[in.a], R[in.b]); break;
            case Op::LoadK: assign(R[in.a], K[in.b]); break;
            case Op::LoadI:
                assign_owned(R[in.a], Value::integer(static_cast<NumKind>(in.k), static_cast<std::int16_t>(in.b)));
                break;
            case Op::LoadNull: assign_owned(R[in.a], Value::null()); break;
            case Op::LoadBool: assign_owned(R[in.a], Value::boolean(in.b != 0)); break;
            case Op::Clear: assign_owned(R[in.a], Value{}); break;
            case Op::GetG: assign(R[in.a], globals[in.b]); break;
            case Op::SetG: assign(globals[in.a], R[in.b]); break;
            case Op::GetUp: assign(R[in.a], f->closure->captures[in.b]); break;
            case Op::GetUpC: assign(R[in.a], cell(f->closure->captures[in.b])->value); break;
            case Op::SetUpC: assign(cell(f->closure->captures[in.a])->value, R[in.b]); break;
            case Op::NewCell: {
                CellObj* c = new_cell(in.k != 0);
                assign(c->value, R[in.b]);
                assign(R[in.a], Value::object(c));
                break;
            }
            case Op::GetCell: assign(R[in.a], cell(R[in.b])->value); break;
            case Op::SetCell: assign(cell(R[in.a])->value, R[in.b]); break;
            case Op::Closure: {
                const Proto* p = protos[in.b].get();
                ClosureObj* c = new_closure(p, p->capture_count);
                for (std::uint16_t i = 0; i < p->capture_count; ++i) assign(c->captures[i], R[in.c + i]);
                assign(R[in.a], Value::object(c));
                break;
            }
            case Op::GetF: {
                InstanceObj* o = instance(R[in.b], in.k);
                if ((in.k & opk::WeakField) != 0) {
                    assign(R[in.a], weak_load(o->fields[in.c]));
                } else {
                    assign(R[in.a], o->fields[in.c]);
                }
                break;
            }
            case Op::SetF: {
                InstanceObj* o = instance(R[in.a], in.k);
                if ((in.k & opk::WeakField) != 0) {
                    assign_owned(o->fields[in.b], weak_store(R[in.c]));
                } else {
                    assign(o->fields[in.b], R[in.c]);
                }
                break;
            }
            case Op::GetI: {
                ListObj* l = list(R[in.b], in.k);
                std::int64_t i = R[in.c].i;
                if (i < 0 || static_cast<std::size_t>(i) >= l->items.size()) {
                    panic(std::format("index {} is out of bounds for a list of length {}", i, l->items.size()));
                }
                assign(R[in.a], l->items[static_cast<std::size_t>(i)]);
                break;
            }
            case Op::SetI: {
                ListObj* l = list(R[in.a], in.k);
                std::int64_t i = R[in.b].i;
                if (i < 0 || static_cast<std::size_t>(i) >= l->items.size()) {
                    panic(std::format("index {} is out of bounds for a list of length {}", i, l->items.size()));
                }
                assign(l->items[static_cast<std::size_t>(i)], R[in.c]);
                break;
            }
            case Op::NewList: {
                auto arena = static_cast<ArenaKind>(in.k & 3);
                ListObj* l = arena == ArenaKind::None ? new_list((in.k & opk::UntracedList) != 0) : manual_list(arena);
                if ((in.k & opk::ArrayList) != 0) l->kind = ObjKind::Array;
                l->items.resize(in.c);
                for (std::uint16_t i = 0; i < in.c; ++i) assign(l->items[i], R[in.b + i]);
                if (arena == ArenaKind::None) {
                    assign(R[in.a], Value::object(l));
                } else {
                    register_manual(l);
                    assign(R[in.a], manual_ref(l));
                }
                break;
            }
            case Op::NewObj: {
                const RecordRT* t = classes[in.b].get();
                if (in.k == 0) {
                    assign(R[in.a], Value::object(new_instance(t, t->field_names.size())));
                } else {
                    InstanceObj* o = manual_instance(t, t->field_names.size(), static_cast<ArenaKind>(in.k));
                    register_manual(o);
                    assign(R[in.a], manual_ref(o));
                }
                break;
            }
            case Op::NewStruct: {
                const RecordRT* t = structs[in.b].get();
                InstanceObj* o = new_instance(t, t->field_names.size());
                for (std::size_t i = 0; i < o->fields.size(); ++i) assign(o->fields[i], R[in.c + i]);
                assign(R[in.a], Value::object(o));
                break;
            }
            case Op::NewEnum: {
                const EnumRT* t = enums[in.b].get();
                const VariantRT& var = t->variants[in.k];
                EnumObj* o = new_enum(t, in.k, var.field_names.size());
                for (std::size_t i = 0; i < o->fields.size(); ++i) assign(o->fields[i], R[in.c + i]);
                assign(R[in.a], Value::object(o));
                break;
            }
            case Op::Copy: assign(R[in.a], copy_value(R[in.b])); break;
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Rem: {
                const Value& x = R[in.b];
                const Value& y = R[in.c];
                if (in.k < 8) {
                    auto k = static_cast<NumKind>(in.k);
                    std::int64_t out = 0;
                    num::Status st = num::int_arith(arith_binop(in.op), k, x.i, y.i, out);
                    if (st != num::Status::Ok) {
                        if (st == num::Status::DivByZero) panic("division by zero");
                        if (!options_.wrap_overflow) overflow(in.op, k);
                    }
                    assign_owned(R[in.a], Value::integer(k, out));
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F32)) {
                    assign_owned(R[in.a], Value::f32(scalar_op(in.op, x.f, y.f)));
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F64)) {
                    double p = x.d;
                    double q = y.d;
                    double r = in.op == Op::Add   ? p + q
                               : in.op == Op::Sub ? p - q
                               : in.op == Op::Mul ? p * q
                               : in.op == Op::Div ? p / q
                                                  : std::fmod(p, q);
                    assign_owned(R[in.a], Value::f64(r));
                } else if (in.k == opk::Vec || in.k == opk::VecS || in.k == opk::SVec) {
                    assign(R[in.a], vec_arith(in.op, in.k, x, y));
                } else {
                    Value p = x;
                    Value q = y;
                    retain(p);
                    retain(q);
                    Value r = dyn_arith(in.op, p, q);
                    release(p);
                    release(q);
                    reload();
                    assign_owned(R[in.a], r);
                }
                break;
            }
            case Op::Neg: {
                const Value& x = R[in.b];
                if (in.k < 8) {
                    auto k = static_cast<NumKind>(in.k);
                    std::int64_t out = 0;
                    if (num::int_neg(k, x.i, out) != num::Status::Ok && !options_.wrap_overflow) overflow(in.op, k);
                    assign_owned(R[in.a], Value::integer(k, out));
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F32)) {
                    assign_owned(R[in.a], Value::f32(-x.f));
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F64)) {
                    assign_owned(R[in.a], Value::f64(-x.d));
                } else if (in.k == opk::Vec) {
                    float out[max_vec];
                    std::uint32_t n = vec_size(x);
                    for (std::uint32_t i = 0; i < n; ++i) out[i] = -vec_data(x)[i];
                    assign(R[in.a], vec_value(out, n));
                } else {
                    Value p = x;
                    retain(p);
                    Value r{};
                    if (is_num_tag(p.tag)) {
                        Value zero = p;
                        zero.i = 0;
                        if (p.tag == Tag::F32) zero.f = 0.0f;
                        if (p.tag == Tag::F64) zero.d = 0.0;
                        r = dyn_arith(Op::Sub, zero, p);
                    } else if (is_vec(p)) {
                        float out[max_vec];
                        std::uint32_t n = vec_size(p);
                        for (std::uint32_t i = 0; i < n; ++i) out[i] = -vec_data(p)[i];
                        r = vec_value(out, n);
                        retain(r);
                    } else if (const std::uint32_t* m = find_method(p, "neg")) {
                        r = invoke(*m, &p, 1);
                    } else {
                        panic("operator '-' is not supported for this value");
                    }
                    release(p);
                    reload();
                    assign_owned(R[in.a], r);
                }
                break;
            }
            case Op::BAnd:
            case Op::BOr:
            case Op::BXor:
            case Op::Shl:
            case Op::Shr: {
                const Value& x = R[in.b];
                const Value& y = R[in.c];
                if (in.k == opk::Bool) {
                    bool r = in.op == Op::BAnd ? (x.b && y.b) : in.op == Op::BOr ? (x.b || y.b) : (x.b != y.b);
                    assign_owned(R[in.a], Value::boolean(r));
                    break;
                }
                auto k = static_cast<NumKind>(in.k);
                std::int64_t out = 0;
                if (num::int_bits(arith_binop(in.op), k, x.i, y.i, out) != num::Status::Ok && !options_.wrap_overflow) {
                    panic(std::format("shift amount {} is out of range for '{}'", y.i, to_string(k)));
                }
                assign_owned(R[in.a], Value::integer(k, out));
                break;
            }
            case Op::BNot: {
                auto k = static_cast<NumKind>(in.k);
                assign_owned(R[in.a], Value::integer(k, num::wrap(k, ~static_cast<std::uint64_t>(R[in.b].i))));
                break;
            }
            case Op::Not: assign_owned(R[in.a], Value::boolean(!R[in.b].b)); break;
            case Op::Eq:
            case Op::Ne:
            case Op::Lt:
            case Op::Le: {
                const Value& x = R[in.b];
                const Value& y = R[in.c];
                bool r = false;
                bool is_eq = in.op == Op::Eq || in.op == Op::Ne;
                if (in.k < 8) {
                    auto k = static_cast<NumKind>(in.k);
                    r = is_eq ? x.i == y.i : in.op == Op::Lt ? num::int_less(k, x.i, y.i) : !num::int_less(k, y.i, x.i);
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F32)) {
                    r = is_eq ? x.f == y.f : in.op == Op::Lt ? x.f < y.f : x.f <= y.f;
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F64)) {
                    r = is_eq ? x.d == y.d : in.op == Op::Lt ? x.d < y.d : x.d <= y.d;
                } else if (in.k == opk::Bool) {
                    r = x.b == y.b;
                } else if (in.k == opk::Name || in.k == opk::Enum) {
                    r = x.i == y.i;
                } else if (in.k == opk::Ref) {
                    // Identity; a stale reference differs from its slot's new occupant.
                    r = (x.is_obj() ? x.o : nullptr) == (y.is_obj() ? y.o : nullptr) &&
                        !(x.ref != 0 && y.ref != 0 && x.aux != y.aux);
                } else if (in.k == opk::Str) {
                    int c = as_str(x)->s.compare(as_str(y)->s);
                    r = is_eq ? c == 0 : in.op == Op::Lt ? c < 0 : c <= 0;
                } else if (in.k == opk::Vec) {
                    r = vec_equal(x, y);
                } else {
                    Value p = x;
                    Value q = y;
                    retain(p);
                    retain(q);
                    r = is_eq ? dyn_equal(p, q) : dyn_less(p, q, in.op == Op::Le);
                    release(p);
                    release(q);
                    reload();
                }
                if (in.op == Op::Ne) r = !r;
                assign_owned(R[in.a], Value::boolean(r));
                break;
            }
            case Op::Cast: {
                const Value& x = R[in.b];
                std::uint8_t from = static_cast<std::uint8_t>(in.c);
                std::uint8_t to = in.k;
                Value out;
                bool from_float = from == static_cast<std::uint8_t>(NumKind::F32) ||
                                  from == static_cast<std::uint8_t>(NumKind::F64);
                double fv = from == static_cast<std::uint8_t>(NumKind::F32) ? static_cast<double>(x.f) : x.d;
                std::int64_t iv = from == opk::Bool ? (x.b ? 1 : 0) : x.i;
                NumKind fk = from < 8 ? static_cast<NumKind>(from) : NumKind::I64;
                if (to == static_cast<std::uint8_t>(NumKind::F32)) {
                    out = Value::f32(from_float ? static_cast<float>(fv) : static_cast<float>(num::int_to_double(fk, iv)));
                } else if (to == static_cast<std::uint8_t>(NumKind::F64)) {
                    out = Value::f64(from_float ? fv : num::int_to_double(fk, iv));
                } else {
                    auto tk = static_cast<NumKind>(to);
                    out = Value::integer(tk, from_float ? num::float_to_int(tk, fv)
                                                        : num::wrap(tk, static_cast<std::uint64_t>(iv)));
                }
                assign_owned(R[in.a], out);
                break;
            }
            case Op::Downcast: {
                const Value& x = R[in.b];
                if (x.is_obj()) {
                    const RecordRT* target = classes[in.c].get();
                    const RecordRT* t = x.o->kind == ObjKind::Instance ? static_cast<InstanceObj*>(x.o)->type : nullptr;
                    if (t == nullptr || !t->is_subclass_of(target)) {
                        panic(std::format("cannot cast {} to '{}'", t != nullptr ? "'" + t->name + "'" : "this value",
                                          target->name));
                    }
                }
                assign(R[in.a], x);
                break;
            }
            case Op::InstOf: {
                const Value& x = R[in.b];
                bool r = x.is_obj() && x.o->kind == ObjKind::Instance &&
                         static_cast<InstanceObj*>(x.o)->type->is_subclass_of(classes[in.c].get());
                assign_owned(R[in.a], Value::boolean(r));
                break;
            }
            case Op::Variant: {
                const Value& x = R[in.b];
                if (!x.is_obj()) null_panic();
                assign_owned(R[in.a], Value::integer(NumKind::I32, static_cast<EnumObj*>(x.o)->variant));
                break;
            }
            case Op::GetPay: assign(R[in.a], static_cast<EnumObj*>(R[in.b].o)->fields[in.c]); break;
            case Op::Jmp: f->pc = in.target(); break;
            case Op::JmpF:
                if (!R[in.a].b) f->pc = in.target();
                break;
            case Op::JmpT:
                if (R[in.a].b) f->pc = in.target();
                break;
            case Op::JmpNull:
                if (R[in.a].is_null()) f->pc = in.target();
                break;
            case Op::JmpNN:
                if (!R[in.a].is_null()) f->pc = in.target();
                break;
            case Op::ToStr: {
                Value x = R[in.b];
                retain(x);
                std::string s = format(x, false);
                release(x);
                reload();
                assign(R[in.a], str_value(std::move(s)));
                break;
            }
            case Op::Concat: {
                std::string s;
                for (std::uint16_t i = 0; i < in.c; ++i) s += as_str(R[in.b + i])->s;
                assign(R[in.a], str_value(std::move(s)));
                break;
            }
            case Op::Call: {
                const Proto* p = protos[in.b].get();
                if (in.k != 0) {
                    start(p, in);
                } else {
                    call(p, nullptr, in.a, in.c);
                }
                break;
            }
            case Op::CallV: {
                InstanceObj* o = instance(R[in.c], in.k);
                const Proto* p = protos[o->type->vtable[in.b]].get();
                if ((in.k & 0x0f) != 0) {
                    start(p, in);
                } else {
                    call(p, nullptr, in.a, in.c);
                }
                break;
            }
            case Op::CallC: {
                const Value& cv = R[in.b];
                if (!cv.is_obj()) null_panic();
                auto* c = static_cast<ClosureObj*>(cv.o);
                call(c->fn, c, in.a, in.c);
                break;
            }
            case Op::CallN: {
                Value r = call_native(in.b, R + in.c, in.k);
                reload();
                assign_owned(R[in.a], r);
                break;
            }
            case Op::CallX: {
                // The arguments are copied: the host may call back into the
                // VM, which can move the stack. The frame keeps them alive.
                std::array<Value, 16> small;
                std::vector<Value> big;
                Value* args = small.data();
                if (in.k > small.size()) {
                    big.assign(R + in.c, R + in.c + in.k);
                    args = big.data();
                } else {
                    std::copy(R + in.c, R + in.c + in.k, small.begin());
                }
                for (std::uint8_t i = 0; i < in.k; ++i) {
                    if (is_stale(args[i])) stale(args[i]);
                }
                NativeCall nc{*this, args, in.k};
                Value r = natives_[in.b].fn(nc);
                reload();
                assign_owned(R[in.a], checked(r));
                break;
            }
            case Op::CallD: {
                const Value& recv = R[in.c];
                if (!recv.is_obj()) null_panic();
                if (is_stale(recv)) stale(recv);
                std::string_view name = as_str(K[in.b])->s;
                const std::uint32_t* m = find_method(recv, name);
                if (m == nullptr) panic(std::format("value has no method '{}'", name));
                call(protos[*m].get(), nullptr, in.a, in.c);
                break;
            }
            case Op::Ret:
            case Op::Ret0: {
                Value result = in.op == Op::Ret ? R[in.a] : Value{};
                Value out = pop_frame(result);
                if (frames_.size() == stop_depth) return out;
                reload();
                break;
            }
            case Op::ForPrep:
            case Op::ForLoop: {
                auto k = static_cast<NumKind>(in.k & 0x7f);
                bool inclusive = (in.k & opk::Inclusive) != 0;
                Value& i = R[in.a];
                std::int64_t hi = R[in.a + 1].i;
                std::int64_t step = R[in.a + 2].i;
                bool up = is_signed(k) ? step > 0 : true;
                auto in_range = [&](std::int64_t v) {
                    if (up) return inclusive ? !num::int_less(k, hi, v) : num::int_less(k, v, hi);
                    return inclusive ? !num::int_less(k, v, hi) : num::int_less(k, hi, v);
                };
                if (in.op == Op::ForPrep) {
                    if (step == 0) panic("a range step cannot be zero");
                    if (!in_range(i.i)) f->pc = in.target();
                } else {
                    std::int64_t next = 0;
                    if (num::int_arith(BinaryOp::Add, k, i.i, step, next) == num::Status::Ok && in_range(next)) {
                        i.i = next;
                        f->pc = in.target();
                    }
                }
                break;
            }
            case Op::ListNext: {
                ListObj* l = list(R[in.b], in.k);
                auto idx = static_cast<std::size_t>(R[in.c].i);
                if (idx < l->items.size()) {
                    assign(R[in.a], l->items[idx]);
                    ++f->pc;
                }
                break;
            }
            case Op::Inc: R[in.a].i += 1; break;
            case Op::IterNext: {
                Value coll = R[in.b];
                retain(coll);
                bool found = false;
                try {
                    found = iter_next(R + in.a, coll, R[in.c], in.k);
                } catch (...) {
                    release(coll);
                    throw;
                }
                release(coll);
                if (found) ++f->pc;
                break;
            }
            case Op::Await: {
                // Suspends the running coroutine until the awaited thing is
                // ready (DESIGN.md 5.9); `in.a` receives the result on resume.
                switch (static_cast<AK>(in.k)) {
                    case AK::Ticks:
                    case AK::Seconds: {
                        std::int64_t n = R[in.b].i;
                        if (static_cast<AK>(in.k) == AK::Seconds) {
                            // Time is counted in ticks: round(seconds * rate), at least one.
                            float t = std::floor(R[in.b].f * options_.tick_rate + 0.5f);
                            n = t >= 1.0f ? static_cast<std::int64_t>(std::min(t, 1e15f)) : 1;
                        }
                        if (n <= 0) { // wait_ticks(0): no suspension
                            assign_owned(R[in.a], Value{});
                            break;
                        }
                        suspend(CoroObj::Wait::Ticks, tick_ + static_cast<std::uint64_t>(n), Value{}, in.a);
                        return Value{};
                    }
                    case AK::Until: {
                        Value cond = R[in.b];
                        if (!cond.is_obj()) null_panic();
                        retain(cond);
                        Value r = invoke_closure(static_cast<ClosureObj*>(cond.o), nullptr, 0);
                        reload();
                        if (r.b) { // already true: no suspension
                            release(cond);
                            assign_owned(R[in.a], Value{});
                            break;
                        }
                        suspend(CoroObj::Wait::Until, 0, cond, in.a);
                        release(cond);
                        return Value{};
                    }
                    case AK::Task: {
                        const Value& t = R[in.b];
                        if (!t.is_obj()) null_panic();
                        auto* tc = static_cast<CoroObj*>(t.o);
                        if (tc->finished()) {
                            assign_owned(R[in.a], Value{});
                            break;
                        }
                        if (tc->state == CoroObj::State::Running) panic("a coroutine cannot await itself");
                        suspend(CoroObj::Wait::Task, 0, t, in.a);
                        return Value{};
                    }
                    case AK::Signal: {
                        if (!R[in.b].is_obj()) null_panic();
                        suspend(CoroObj::Wait::Signal, 0, R[in.b], in.a);
                        return Value{};
                    }
                    case AK::Call: break; // compiled as a plain call
                }
                break;
            }
            case Op::VecNew: {
                float out[max_vec];
                std::uint32_t n = in.k;
                if (in.c == 1 && R[in.b].tag == Tag::F32 && n > 1) {
                    for (std::uint32_t i = 0; i < n; ++i) out[i] = R[in.b].f; // splat
                } else {
                    std::uint32_t at = 0;
                    for (std::uint16_t p = 0; p < in.c; ++p) {
                        const Value& part = R[in.b + p];
                        if (part.tag == Tag::F32) {
                            out[at++] = part.f;
                        } else {
                            std::uint32_t m = vec_size(part);
                            const float* d = vec_data(part);
                            for (std::uint32_t i = 0; i < m; ++i) out[at++] = d[i];
                        }
                    }
                }
                assign(R[in.a], vec_value(out, n));
                break;
            }
            case Op::Swz: {
                const float* d = vec_data(R[in.b]);
                if (in.c == 1) {
                    assign_owned(R[in.a], Value::f32(d[in.k & 3]));
                    break;
                }
                float out[4];
                for (std::uint16_t i = 0; i < in.c; ++i) out[i] = d[(in.k >> (2 * i)) & 3];
                assign(R[in.a], vec_value(out, in.c));
                break;
            }
            case Op::VecWith: {
                float out[max_vec];
                std::uint32_t n = vec_size(R[in.b]);
                std::copy(vec_data(R[in.b]), vec_data(R[in.b]) + n, out);
                const Value& v = R[in.c];
                if (v.tag == Tag::F32) {
                    out[in.k & 3] = v.f;
                } else {
                    std::uint32_t m = vec_size(v);
                    for (std::uint32_t i = 0; i < m; ++i) out[(in.k >> (2 * i)) & 3] = vec_data(v)[i];
                }
                assign(R[in.a], vec_value(out, n));
                break;
            }
            case Op::VecIdx: {
                std::uint32_t n = vec_size(R[in.b]);
                std::int64_t i = R[in.c].i;
                if (i < 0 || static_cast<std::uint64_t>(i) >= n) {
                    panic(std::format("index {} is out of bounds for a vec{}", i, n));
                }
                assign_owned(R[in.a], Value::f32(vec_data(R[in.b])[i]));
                break;
            }
            case Op::NewSig: assign(R[in.a], Value::object(new_signal())); break;
            case Op::Free: {
                // DESIGN.md 5.10: frees a manual object; `free(null)` does nothing.
                const Value v = R[in.a];
                if (v.tag != Tag::Obj) break;
                if (!is_manual(v.o)) {
                    panic(std::format("free: '{}' is not a manual object (only objects from 'alloc', 'new(frame)', "
                                      "'new(scene)' or a manual class can be freed)",
                                      span_text()));
                }
                if (v.ref != 0 ? v.o->gen != v.aux : (v.o->flags & objflag::Dead) != 0) {
                    panic(std::format("double free: '{}' was already freed; {}", span_text(), free_text(v.o)));
                }
                FreeSite site;
                site.proto = f->fn;
                site.pc = f->pc - 1;
                site.tick = tick_;
                free_manual(v.o, site);
                break;
            }
        }
    }
}

// ------------------------------------------------------------ disassembly

std::string_view to_string(Op op) {
    switch (op) {
#define PLUMA_OP_NAME(name) \
    case Op::name: return #name;
        PLUMA_OPCODES(PLUMA_OP_NAME)
#undef PLUMA_OP_NAME
    }
    return "?";
}

std::string disassemble(const Proto& p) {
    std::string out = std::format("fn {} (params {}, regs {}, captures {})\n", p.name, p.param_count, p.reg_count,
                                  p.capture_count);
    for (std::size_t i = 0; i < p.code.size(); ++i) {
        const Instr& in = p.code[i];
        out += std::format("  {:4} {:<10} a={} b={} c={} k={}\n", i, to_string(in.op), in.a, in.b, in.c, in.k);
    }
    return out;
}

} // namespace pluma

#pragma once

// The register VM (DESIGN.md 5.12): executes bytecode from the compiler.
//
// Script-to-script calls push a Frame onto an explicit frame stack; registers
// live in one value stack addressed by frame base offsets. Native code that
// needs to call back into script (operator dispatch, `to_str` on generic
// values, signal handlers, host calls) runs a nested dispatch loop with
// `invoke`.
//
// Coroutines (DESIGN.md 5.9) run on the same stack: resuming one copies its
// saved frames onto the top of the stack and runs a nested dispatch loop;
// `await` copies them back into the CoroObj ("heap frames") and returns from
// that loop. Frames between a coroutine's base and the `await` must all be
// script frames: suspending inside a native callback panics (the checker
// already rejects the ways to get there). The scheduler (`tick_coroutines`)
// resumes waiting coroutines once per simulation tick, in start order, so
// coroutine timing is deterministic.
//
// Memory (DESIGN.md 5.10, pluma/heap.hpp): refcounting plus the cycle
// collector, which the host runs at a fixed point of its tick with
// `collect_cycles(budget)`; manual objects live in the VM's arenas (frame,
// scene) or its `alloc` registry, and references to them are checked
// (pointer + generation) by every instruction that dereferences them, except
// inside `unsafe { }`.
// TODO(perf): structs are boxed and copied on read (value semantics); the AOT
// path and a later VM revision should store them inline in registers/fields.
//
// Panics (DESIGN.md 5.8) unwind to the host entry point (`run`, `call_*`,
// `tick_coroutines`) and are reported with the source location and a stack
// trace; integer overflow panics unless `wrap_overflow` (release semantics)
// is set.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/bytecode.hpp"
#include "pluma/debug.hpp"
#include "pluma/heap.hpp"
#include "pluma/value.hpp"

namespace pluma {

class VM;

// Transcendental functions for `sin`, `cos`, `atan2`, `from_angle`,
// `rotated` and `angle`. The language library has no deterministic math of
// its own (it must not depend on the engine); the engine installs
// cuervo/math/dmath here so simulation scripts are deterministic
// (DESIGN.md 4.1). Unset entries fall back to <cmath> (REPL, tests).
struct MathHooks {
    float (*sin)(float) = nullptr;
    float (*cos)(float) = nullptr;
    float (*tan)(float) = nullptr;
    float (*asin)(float) = nullptr;
    float (*acos)(float) = nullptr;
    float (*atan2)(float, float) = nullptr;
    float (*exp)(float) = nullptr;
    float (*log)(float) = nullptr;
    float (*pow)(float, float) = nullptr;
};

struct VmOptions {
    // Release builds wrap integer overflow; development builds panic.
    bool wrap_overflow = false;
    // Where `print` writes (default: stdout).
    std::function<void(std::string_view)> out;
    std::size_t max_frames = 4096;
    // Simulation ticks per second: `await wait(seconds)` waits
    // round(seconds * tick_rate) ticks (at least one).
    float tick_rate = 60.0f;
    MathHooks math;
    // Development: `@acyclic` classes the compiler can't prove acyclic are
    // traced anyway, and the collector reports one found in a garbage cycle
    // (pluma/acyclic.hpp). Release builds trust the promise.
    bool check_acyclic = true;
    // `List.shuffle()` (LANGUAGE.md 9.17): the host's seeded stream, a
    // uniform integer in [0, bound). Unset: the VM's own PCG32 stream seeded
    // with `random_seed`, so plain Pluma is deterministic too.
    std::function<std::uint32_t(std::uint32_t bound)> random_below;
    std::uint64_t random_seed = 0x853c49e6748fea9bull;
};

// One local of a frame at the moment of a panic. `value` is display text made
// without running script code (no `to_str`), bounded in length.
struct PanicLocal {
    std::string name;
    std::string type;
    std::string value;
};

// One script frame at the moment of a panic (DESIGN.md 5.8: the editor shows
// the stack trace and locals). Natives have no frame of their own: a panic
// inside one is reported at the script call that reached it.
struct PanicFrame {
    std::string function;
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::vector<PanicLocal> locals; // in declaration order, parameters first
};

struct PanicInfo {
    std::string message;
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::vector<std::string> trace; // innermost first: "at Boss.hurt (boss.plm:12:9)"
    std::vector<PanicFrame> frames; // innermost first, one per trace line
    std::string render() const;
    // render() plus each frame's locals ("name: type = value").
    std::string render_with_locals() const;
};

struct RunResult {
    bool ok = true;
    std::optional<PanicInfo> panic;
};

// A host function bound to an `@native` declaration (pluma/bindings.hpp).
// `args` are the declaration's parameters (self first for methods). The
// result is owned by the caller (retain objects before returning them);
// return Value{} for nothing. Report errors with vm.panic().
struct NativeCall {
    VM& vm;
    Value* args;
    std::uint32_t count;
};
using NativeFn = std::function<Value(NativeCall&)>;

class VM {
public:
    explicit VM(VmOptions options = {});
    ~VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    // ---- program image, filled in by the compiler
    std::vector<std::unique_ptr<Proto>> protos;
    std::vector<std::unique_ptr<RecordRT>> classes;
    std::vector<std::unique_ptr<RecordRT>> structs;
    std::vector<std::unique_ptr<EnumRT>> enums;
    std::vector<Value> globals;
    std::vector<std::string> global_names; // qualified ("Boss.count" for statics)
    std::vector<std::string> global_types; // type names, for hot reload

    std::uint32_t intern(std::string_view name);
    const std::string& name_text(std::uint32_t id) const { return names_[id]; }

    // ---- host functions (register before compiling the code that uses them)
    static constexpr std::uint32_t no_native = 0xffffffffu;
    std::uint32_t register_native(std::string name, NativeFn fn);
    std::uint32_t native_index(std::string_view name) const;
    // native_index, also binding an FFI key (pluma/ffi.hpp) on first use;
    // `error` explains a key that can't be bound.
    std::uint32_t resolve_native(std::string_view name, std::string* error = nullptr);
    void set_native_finalizer(std::string class_name, void (*finalizer)(void*));
    void (*native_finalizer(std::string_view class_name) const)(void*);

    // ---- lookups by name (host code, hot reload)
    const RecordRT* find_class(std::string_view name) const;
    const RecordRT* find_struct(std::string_view name) const;
    std::optional<std::uint32_t> global_index(std::string_view qualified) const;
    std::optional<std::uint32_t> proto_by_key(std::string_view key) const;

    // ---- entry points from the host. Each catches panics, unwinds to where
    // it was called and reports them in the result.

    // Calls a function that takes no arguments (a chunk or `main`).
    RunResult run(std::uint32_t proto_index);
    // Calls proto `index` with `args`; the result (owned) goes to `out`.
    RunResult call(std::uint32_t proto_index, std::span<const Value> args, Value* out = nullptr);
    // Calls the method `name` of class instance `self` (virtual dispatch).
    RunResult call_method(const Value& self, std::string_view name, std::span<const Value> args = {},
                          Value* out = nullptr);
    // Creates an instance of a class and runs its field initializers (not
    // `init`). The result is owned by the caller. With an `arena` it is a
    // manual object (a checked reference) registered in this VM.
    RunResult instantiate(const RecordRT* cls, Value* out, ArenaKind arena = ArenaKind::None);
    // A new, empty manual list in `arena` (a checked reference).
    Value new_manual_list(ArenaKind arena, bool untraced);

    // ---- memory (DESIGN.md 5.10)

    // One incremental step of the cycle collector, `budget` work units
    // (pluma/heap.hpp). Call it at a fixed point of the tick, when no
    // script runs. A broken `@acyclic` promise comes back as a panic.
    RunResult collect_cycles(std::uint64_t budget, CollectResult* stats = nullptr);
    // Frees every object of the frame arena (end of tick) or the scene arena
    // (scene unload), newest first.
    void reset_frame_arena();
    void reset_scene_arena();
    // Live manual objects registered in `arena` (Alloc: `alloc` and manual
    // classes).
    std::size_t arena_objects(ArenaKind arena) const;
    // Runs `body` with panic protection (for host code that calls natives
    // or script in several steps).
    RunResult protect(const std::function<void()>& body);

    // ---- coroutines (DESIGN.md 5.9)

    // Advances the simulation tick and resumes every coroutine that is due,
    // in start order. Coroutines started during the pass first run on their
    // own `start`. Returns the first panic, if any; a panicking coroutine is
    // stopped and the others keep running.
    //
    // `frozen(owner)` (optional) pauses coroutines by owner (the engine's
    // scene pause): a frozen coroutine is not resumed and its `until`
    // condition is not evaluated, and a `wait`/`wait_ticks` deadline moves
    // one tick later, so paused time doesn't count. Signals emitted meanwhile
    // are kept and resume it once it thaws.
    using FrozenFn = std::function<bool(const Object* owner)>;
    RunResult tick_coroutines(const FrozenFn& frozen = nullptr);
    std::uint64_t tick() const { return tick_; }
    // Cancels every coroutine owned by `owner` (when it leaves the scene).
    void cancel_owned(const Object* owner);
    void cancel(CoroObj* c);
    // Live (not finished) coroutines, in start order.
    std::vector<CoroObj*> coroutines() const;
    // Creates a coroutine of proto `fn` that has not run yet (hot reload
    // restarts); `run_fresh` runs every such coroutine to its first suspension.
    CoroObj* create_coroutine(std::uint32_t fn, std::vector<Value> args, Object* owner);
    RunResult run_fresh();

    // Display (`repr` = false) or debug/REPL (`repr` = true) text of a value.
    // May call `to_str` methods.
    std::string format(const Value& v, bool repr);
    // Debugger text of a value (repr style): never runs script code (no
    // `to_str`), so it is safe during a panic or from a host's inspector,
    // and stops after about `max_length` bytes ("...").
    std::string describe(const Value& v, std::size_t max_length = 160);

    const VmOptions& options() const { return options_; }
    std::size_t frame_depth() const { return frames_.size(); }

    // ---- debugging and profiling (pluma/debug.hpp, DESIGN.md 5.14)

    // Installs (or, with nullptr, removes) the hooks; `hooks` must outlive
    // the VM or be removed first. Install them between calls into the VM:
    // an interpreter loop already running keeps its line-hook setting until
    // it returns, and only frames entered afterwards report calls/returns.
    void set_debug_hooks(DebugHooks* hooks);
    DebugHooks* debug_hooks() const { return hooks_; }
    // Script frames on the stack (also those under a native call), innermost first.
    std::vector<StackFrameInfo> stack_frames() const;
    // stack_frames()[0] without building the rest (a per-line debugger check).
    StackFrameInfo top_frame() const;
    // Live locals of stack frame `index` (0 = innermost), parameters first.
    std::vector<LocalInfo> frame_locals(std::size_t index) const;
    // Fields of an instance or enum payload, list elements, vector
    // components, a captured cell's value, a weak reference's target.
    std::vector<ChildInfo> children(const Value& v) const;
    // Source line of instruction `pc` of `fn` (0 = unknown).
    std::uint32_t line_of(const Proto& fn, std::uint32_t pc) const;
    // Lines of source part `file` that have code: where a breakpoint can stop.
    std::vector<std::uint32_t> code_lines(std::string_view file) const;

    // Used by natives.
    [[noreturn]] void panic(std::string message);
    void write(std::string_view text);
    Value invoke(std::uint32_t proto_index, const Value* args, std::uint32_t argc);
    Value invoke_closure(const ClosureObj* closure, const Value* args, std::uint32_t argc);
    // Emits a signal: calls its handlers, then wakes coroutines awaiting it.
    void emit_signal(SignalObj* signal, const Value* args, std::uint32_t argc);

private:
    struct Frame {
        const Proto* fn = nullptr;
        const ClosureObj* closure = nullptr;
        std::uint32_t base = 0;
        std::uint32_t pc = 0;
        std::uint32_t ret = 0;    // absolute stack slot for the result
        bool discard = false;     // result goes back to native code
        CoroObj* coro = nullptr;  // set on the bottom frame of a running coroutine
        // Debug hooks: line and pc of the last line event (0 = none yet).
        std::uint32_t hook_line = 0;
        std::uint32_t hook_pc = 0;
    };
    void line_hook(Frame& f);
    // pc of the instruction frame `f` is at, for inspection: the innermost
    // frame's next one (it stopped before it), or the one it's inside (a call;
    // the innermost frame during a panic).
    std::uint32_t inspect_pc(const Frame& f, bool innermost) const {
        if (innermost && !in_panic_) return f.pc;
        return f.pc > 0 ? f.pc - 1 : 0;
    }
    void enter_hook(const Frame& f) {
        if (hook_calls_) hooks_->on_call(*this, *f.fn);
    }
    void leave_hook(const Frame& f) {
        if (hook_calls_) hooks_->on_return(*this, *f.fn);
    }

    Value execute(std::size_t stop_depth);
    // The interpreter loop, compiled with and without the line hook check so
    // a VM without a debugger pays nothing for it (DebugHooks::lines).
    template <bool LineHooks>
    Value execute_loop(std::size_t stop_depth);
    void push_frame(const Proto* fn, const ClosureObj* closure, const Value* args, std::uint32_t argc,
                    std::uint32_t ret, bool discard);
    Value pop_frame(const Value& result);
    void ensure_stack(std::size_t size);
    std::uint32_t stack_top() const;
    void unwind_to(std::size_t depth);
    PanicInfo panic_info(const std::string& message, std::size_t depth) const;

    // Coroutines.
    CoroObj* start_coroutine(const Proto* fn, const Value* args, Object* owner);
    void resume(CoroObj* c);
    // Saves the frames of the running coroutine (from its base frame up) and
    // removes them from the stack; a coroutine cancelled while it ran is
    // dropped instead. `wait_value` is borrowed.
    void suspend(CoroObj::Wait wait, std::uint64_t wake_tick, Value wait_value, std::uint16_t dst);
    void drop_saved(CoroObj* c);
    void finish(CoroObj* c, const Value& result);

    // Manual objects.
    void register_manual(Object* o);
    void free_manual(Object* o, const FreeSite& site);
    void reset_arena(ArenaKind arena);
    std::string free_text(const Object* o) const;
    [[noreturn]] void stale_panic(const Value& v, std::string_view what);
    void release_owner(CoroObj* c);

    // Generic operations on tagged values.
    Value dyn_arith(Op op, const Value& a, const Value& b);
    bool dyn_equal(const Value& a, const Value& b);
    bool dyn_less(const Value& a, const Value& b, bool or_equal);
    Value copy_value(const Value& v);
    const std::uint32_t* find_method(const Value& receiver, std::string_view name);
    void format_into(std::string& out, const Value& v, bool repr, int depth);
    Value call_native(std::uint16_t id, Value* args, std::uint32_t argc);
    // The Milestone 9.2a library natives (pluma/vm_lib.cpp).
    Value call_lib(std::uint16_t id, Value* args, std::uint32_t argc);
    // IterNext: the next element of a Map/Set/Deque `for` loop (see vm_lib.cpp).
    bool iter_next(Value* dst, const Value& coll, Value& cursor, std::uint8_t k);
    std::uint32_t random_below(std::uint32_t bound);
    float m_sin(float x) const;
    float m_cos(float x) const;
    float m_tan(float x) const;
    float m_asin(float x) const;
    float m_acos(float x) const;
    float m_atan2(float y, float x) const;
    float m_exp(float x) const;
    float m_log(float x) const;
    float m_pow(float b, float e) const;

    std::string location(const Frame& f, std::uint32_t pc) const;

    VmOptions options_;
    std::vector<Value> stack_;
    std::vector<Frame> frames_;
    std::vector<std::string> names_;
    std::map<std::string, std::uint32_t, std::less<>> name_ids_;

    struct Native {
        std::string name;
        NativeFn fn;
    };
    std::vector<Native> natives_;
    std::map<std::string, std::uint32_t, std::less<>> native_ids_;
    std::map<std::string, void (*)(void*), std::less<>> finalizers_;

    struct Running {
        CoroObj* coro = nullptr; // holds a reference while it runs
        std::size_t depth = 0;   // frame depth below its base frame
    };
    std::vector<CoroObj*> coros_;  // scheduled coroutines, in start order (owned references)
    std::vector<Running> running_; // coroutines on the stack, innermost last
    std::uint64_t tick_ = 0;
    // Manual objects by Arena, with the generation they were registered at;
    // freed ones are skipped and compacted away.
    struct ManualList {
        std::vector<std::pair<Object*, std::uint16_t>> items;
        std::size_t compact_at = 64;
    };
    ManualList manual_[4];
    std::uint64_t rng_state_ = 0; // random_below's PCG32 state
    std::int64_t gc_credit_ = 0; // collect_cycles: work units owed or available
    bool suspended_ = false;        // the last execute() returned because a coroutine suspended
    DebugHooks* hooks_ = nullptr;
    bool hook_lines_ = false; // hooks_ wants DebugHooks::lines
    bool hook_calls_ = false; // hooks_ wants DebugHooks::calls
    bool hook_panics_ = false; // hooks_ wants DebugHooks::panics
    bool in_panic_ = false;    // inside DebugHooks::on_panic
    // describe(): format_into without script calls, up to a length.
    bool format_scripts_ = true;
    std::size_t format_limit_ = static_cast<std::size_t>(-1);

    friend struct Natives;
    friend struct AotRuntime;
};

} // namespace pluma

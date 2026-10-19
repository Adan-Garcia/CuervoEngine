#pragma once

// Debugger and profiler hooks of the VM (DESIGN.md 5.14 "a debugger over DAP,
// and a profiler view"). A host installs a DebugHooks with
// VM::set_debug_hooks; the VM then reports source lines and function
// entry/exit of bytecode it runs. AOT-native code (DESIGN.md 5.12) has no
// hooks: debugging is a development feature.

#include <cstdint>
#include <string>
#include <string_view>

#include "pluma/value.hpp"

namespace pluma {

class VM;
struct Proto;

class DebugHooks {
public:
    // Events to deliver (bit mask); read once by VM::set_debug_hooks.
    static constexpr std::uint32_t lines = 1;
    static constexpr std::uint32_t calls = 2;
    static constexpr std::uint32_t panics = 4;

    virtual ~DebugHooks() = default;
    virtual std::uint32_t events() const = 0;

    // Before the first instruction of a source line in a frame: on entering a
    // function, when the line changes, and on a backward jump (each pass of a
    // one-line loop). The VM is in a consistent state, so the host may inspect
    // it (VM::stack_frames, frame_locals, children) and may block here (a
    // debugger stopped at a breakpoint). It must not run script code.
    virtual void on_line(VM& vm) { (void)vm; }

    // A script frame was entered / left. Balanced: a coroutine's frames leave
    // when it suspends and enter again when it resumes, and a panic leaves
    // every unwound frame.
    virtual void on_call(VM& vm, const Proto& fn) { (void)vm, (void)fn; }
    virtual void on_return(VM& vm, const Proto& fn) { (void)vm, (void)fn; }

    // New functions were compiled into the VM and are about to run (a script,
    // a project, a hot reload's new VM): a debugger checks its breakpoints
    // against them here. Always delivered.
    virtual void on_code_loaded(VM& vm) { (void)vm; }

    // A panic, before the stack unwinds: the frames and locals are still
    // there to inspect (stack_frames reports the panicking instruction). The
    // host may block here; when it returns the panic proceeds as usual.
    virtual void on_panic(VM& vm, std::string_view message) { (void)vm, (void)message; }
};

// One script frame, innermost first (VM::stack_frames).
struct StackFrameInfo {
    const Proto* fn = nullptr;
    std::string_view function;
    std::string_view file; // the source part's path ("scripts/game.plm")
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

// A live local of a frame (VM::frame_locals); `value` is borrowed.
struct LocalInfo {
    std::string_view name;
    std::string_view type;
    Value value;
};

// A named child of a value (VM::children): a field, element or component.
// `value` is borrowed.
struct ChildInfo {
    std::string name;
    Value value;
};

} // namespace pluma

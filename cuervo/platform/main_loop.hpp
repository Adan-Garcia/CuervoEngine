#pragma once

#include <memory>

// The frame loop, written so a browser can drive it (DESIGN.md 2 "Web").
//
// A browser never lets a page block in `while (running) { ... }`: the page
// must hand control back after every frame and be called again on the next
// animation frame (emscripten_set_main_loop). So applications put one frame
// of work in `FrameLoop::frame()` and all state in the object, never on the
// stack of the function that started the loop:
//
//   native: while (loop->frame()) {}  return loop->finish();
//   web:    emscripten_set_main_loop_arg(..., simulate_infinite_loop = true);
//           every animation frame calls frame(); when it returns false the
//           loop is cancelled, finish() runs and its result is the exit code.
//
// On the web `run_main_loop` does not return to its caller (Emscripten
// unwinds the C stack), which is why the loop object is heap-owned and why
// nothing after the call may matter. VSync is the browser's
// requestAnimationFrame cadence there; without it (`vsync` false, e.g.
// --bench) frames run back to back through setImmediate, on every backend
// (WebGPU has no swap interval to turn off, milestone 10.4).
namespace cuervo::platform {

class FrameLoop {
public:
    virtual ~FrameLoop() = default;
    // One frame: poll events, simulate, render, present. False = quit.
    virtual bool frame() = 0;
    // Releases everything after the last frame; the process exit code.
    virtual int finish() = 0;
};

// Runs `loop` until frame() returns false and returns finish()'s result
// (natively). On the web, see above; `vsync` only matters there (natively
// the device's present mode paces frames).
int run_main_loop(std::unique_ptr<FrameLoop> loop, bool vsync = true);

} // namespace cuervo::platform

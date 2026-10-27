#include "cuervo/platform/main_loop.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include "cuervo/core/log.hpp"

namespace cuervo::platform {

#if defined(__EMSCRIPTEN__)

namespace {

// Tells the page (web/shell.html) that the game ended.
EM_JS(void, notify_page_exit, (int code), {
    if (Module.onEngineExit) Module.onEngineExit(code);
});

bool web_uncapped = false;

void web_frame(void* user) {
    // The timing can only change once the loop exists (Emscripten), so the
    // first frame switches it. setImmediate, not setTimeout(0): browsers
    // clamp nested timeouts to about 4 ms, which capped benchmarks at 250 FPS.
    if (web_uncapped) {
        web_uncapped = false;
        emscripten_set_main_loop_timing(EM_TIMING_SETIMMEDIATE, 0);
    }
    auto* loop = static_cast<FrameLoop*>(user);
    if (loop->frame()) return;
    emscripten_cancel_main_loop();
    const int code = loop->finish();
    delete loop;
    LOG_INFO("Main loop finished (exit code {})", code);
    // The page stays up (EXIT_RUNTIME=0); the shell shows the result.
    notify_page_exit(code);
}

} // namespace

int run_main_loop(std::unique_ptr<FrameLoop> loop, bool vsync) {
    // fps = 0: requestAnimationFrame. simulate_infinite_loop = true: never
    // returns; the loop object is now owned by the browser's callback.
    web_uncapped = !vsync;
    emscripten_set_main_loop_arg(web_frame, loop.release(), 0, true);
    return 0; // not reached
}

#else

int run_main_loop(std::unique_ptr<FrameLoop> loop, bool /*vsync*/) {
    while (loop->frame()) {
    }
    return loop->finish();
}

#endif

} // namespace cuervo::platform

#pragma once

#include <cstdint>
#include <string>

#include "cuervo/render/camera.hpp"
#include "cuervo/rhi/rhi.hpp"

// `cuervo --game <dir>`: boots a script project (cuervo/script/project.hpp)
// in a window and runs it at a fixed tick (DESIGN.md 4.1), with hot reload of
// its scripts in development (DESIGN.md 5.13): the script files are watched,
// and on save the project is recompiled and swapped in, keeping state.
//
// Keys handled by the runner (not the game): Escape quits (unless the
// project binds Key.Escape to an action, e.g. a pause menu; the game then
// quits with Scene.quit()), F5 forces a reload, F6 restarts the game from
// scratch. The mouse pointer is mapped to virtual coordinates and recorded
// in each input frame (UI, DESIGN.md 4.10).
namespace pluma::dap {
class Debugger;
}

namespace cuervo::script {

struct GameOptions {
    std::string dir;
    // Release: run this exported pack instead of a project directory
    // (docs/EXPORT.md); hot reload is then off.
    std::string pack;
    rhi::Backend backend = rhi::Backend::OpenGL;
    bool allow_fallback = true;   // Vulkan unavailable -> OpenGL
    bool vsync = true;
    double quit_after = 0.0;      // seconds; 0 = run until closed
    std::string screenshot;       // capture this tick's frame to a PPM and exit
    std::uint64_t screenshot_tick = 240;
    // Scripted input for automated runs: "tick:action,tick:action" holds each
    // action during that tick (e.g. "60:ui_accept,300:pause"), so headless
    // captures can reach menus. Added on top of the devices' input.
    std::string presses;   // a tick may be a range: "10-20:ui_click"
    // More scripted input (GameSession::set_pointer_script / set_typed):
    // "tick:x:y,..." moves the pointer, "tick:text,..." types characters.
    std::string pointers;
    std::string typed;
    std::string font_path;        // TTF/OTF or .efont for Canvas.text
    bool hot_reload = true;
    render::ScaleMode scale = render::ScaleMode::Fit;
    // Play sound on a device. Off (or no device, or CUERVO_AUDIO=null, or the
    // offscreen video driver): the null backend, same triggers, no output.
    bool audio = true;
    // OpenGL: request an OpenGL ES 3.0 context (WebGL2's API) instead of GL 3.3 core.
    bool gl_es = false;
    // Job system threads (--jobs); 0 = CUERVO_JOBS, project.toml `jobs`, or
    // auto, in that order (jobs::resolve_thread_count). The simulation's
    // result is the same for every count (DESIGN.md 4.15).
    unsigned jobs = 0;
    // --record <file>: write every tick's input and state hash as a replay
    // (cuervo/input/replay.hpp) when the game ends.
    std::string record;
    // --bench <seconds>: run `bench_warmup` seconds (filling pools, settling
    // caches), then measure frame times for `bench` seconds, log avg / p50 /
    // p95 / p99 / max and exit. Pair with vsync = false. 0 = off.
    double bench = 0.0;
    double bench_warmup = 3.0;
    // `cuervo --dap` (runtime/dap_cli.hpp): the debugger, attached to the
    // game's script VMs; the loop polls it every frame and ends when the
    // client disconnects.
    pluma::dap::Debugger* debugger = nullptr;
    // --profile: engine zones (cuervo/core/profiler.hpp) and script
    // profiling (pluma/profiler.hpp) on; a summary is logged at exit.
    bool profile = false;
};

// Runs the game to the end and returns the exit code. The frame loop is
// callback-driven (platform/main_loop.hpp): on the web this hands the loop
// to the browser and never returns.
int run_game(const GameOptions& options);

// Whether a device can run a project of `tier` (DESIGN.md 4.4): logs what is
// missing; false (refuse) when a Full project lands on a device that can't
// serve Full. Compatible projects only get warnings.
bool check_device_tier(const rhi::DeviceInfo& info, rhi::Tier tier);

} // namespace cuervo::script

#include "cuervo/script/game_runner.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/audio/audio_system.hpp"
#include "cuervo/core/fixed_timestep.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/core/profiler.hpp"
#include "cuervo/input/input_map.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/platform/file_watcher.hpp"
#include "cuervo/platform/main_loop.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/script/vector_cache.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/game_session.hpp"
#include "cuervo/script/script_host.hpp"
#include "pluma/dap.hpp"

namespace cuervo::script {

namespace fs = std::filesystem;

namespace {

double now_seconds() {
    return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
}

GraphicsApi graphics_api_for(rhi::Backend backend) {
    return backend == rhi::Backend::Vulkan ? GraphicsApi::Vulkan : GraphicsApi::OpenGL;
}

std::unique_ptr<rhi::Device> open_device(Window& window, rhi::Backend backend, const rhi::DeviceOptions& options,
                                         const std::string& title, ivec2 size, bool gl_es) {
    if (!window.create({.title = title,
                        .width = size.x(),
                        .height = size.y(),
                        .graphics = graphics_api_for(backend),
                        .gl_es = gl_es})) {
        return nullptr;
    }
    auto device = rhi::create_device(backend, window, options);
    if (!device) window.destroy();
    return device;
}

// --profile's summary (DESIGN.md 5.14 "Clarified (Phase 8 -- Profiler)"),
// one log line per table row.
void log_profile(ScriptHost& host) {
    const auto log_lines = [](const std::string& text) {
        for (std::size_t pos = 0; pos < text.size();) {
            const std::size_t end = std::min(text.find('\n', pos), text.size());
            LOG_INFO("profile | {}", std::string_view(text).substr(pos, end - pos));
            pos = end + 1;
        }
    };
    if constexpr (profiler::compiled_in) {
        log_lines("engine zones (per frame, last " + std::to_string(profiler::frame_stats().frames) + " frames)\n" +
                  profiler::report());
    } else {
        LOG_INFO("profile | engine zones compiled out (CUERVO_PROFILING=OFF)");
    }
    if (const pluma::ScriptProfiler* p = host.script_profiler(); p != nullptr && p->total_ns() > 0) {
        log_lines(p->report(20));
    } else {
        LOG_INFO("profile | no script calls recorded (AOT-native scripts have no hooks, DESIGN.md 5.12)");
    }
}

} // namespace

// DESIGN.md 4.4: a project runs only where its render tier is served. A Full
// project on a device that can't serve Full is refused (it would silently
// lose features); a Compatible project on a device missing something is
// warned about and runs.
bool check_device_tier(const rhi::DeviceInfo& info, rhi::Tier tier) {
    const rhi::TierReport report = rhi::check_tier(info, tier);
    if (report.gated_down) {
        LOG_ERROR("This project uses render_tier = \"{}\", but the {} backend only provides the {} tier. Run it with "
                  "--backend vulkan, or migrate it: cuervo-tools tier check <project> --to compatible",
                  rhi::tier_name(tier), rhi::backend_name(info.backend), rhi::tier_name(info.max_tier));
        return false;
    }
    for (rhi::Feature f : report.missing_required) {
        if (tier == rhi::Tier::Full) {
            LOG_ERROR("This device lacks {} (needed by render_tier = \"full\")", rhi::feature_name(f));
        } else {
            LOG_WARN("This device lacks {}; parts of the game may not render", rhi::feature_name(f));
        }
    }
    if (tier == rhi::Tier::Full && !report.missing_required.empty()) return false;
    for (rhi::Feature f : report.missing_optional) LOG_INFO("Device has no {} (optional)", rhi::feature_name(f));
    for (const std::string& w : report.limit_warnings) {
        LOG_WARN("Device limit below the {} tier: {}", rhi::tier_name(tier), w);
    }
    return true;
}

namespace {

// One running game: everything the frame loop needs lives here, never on the
// stack of run_game (the web's main loop unwinds it; platform/main_loop.hpp).
// The simulation is a GameSession (game_session.hpp), the same one the
// editor's play-in-editor runs.
class GameLoop final : public platform::FrameLoop {
public:
    explicit GameLoop(const GameOptions& options) : options_(options) {}

    bool init();
    bool frame() override;
    int finish() override;

private:
    void hot_reload(double now);
    void render();
    void bench_frame(double now, double frame_ms, double sim_seconds, int ticks);
    void bench_report() const;

    GameOptions options_;
    GameSession session_;
    Window window_;
    std::unique_ptr<rhi::Device> device_;
    render::SdfBatcher shapes_;
    render::TextRenderer text_;
    // Sprites (Canvas.vector): the meshes and the images they come from.
    render::MeshBatcher meshes_;
    VectorCache vectors_{meshes_};
    render::FontHandle font_;
    std::optional<FixedTimestep> timestep_;
    DeviceState devices_;
    bool escape_quits_ = true;
    bool capture_mode_ = false;
    double start_ = 0.0;
    double previous_ = 0.0;
    double last_watch_ = 0.0;
    std::uint64_t frames_ = 0;
    double render_cpu_ = 0.0; // seconds of the last render() after begin_frame

    // --bench samples, taken after the warmup.
    struct Bench {
        double measure_start = -1.0; // < 0: still warming up
        std::vector<double> frame_ms;
        double particle_sum = 0.0;
        double sim_seconds = 0.0;
        double render_seconds = 0.0;
        std::uint64_t ticks = 0;
        bool done = false;
    } bench_;
};

bool GameLoop::init() {
    // Headless runs (tests, CI, the offscreen driver) never open an audio device.
    const bool headless = platform::env_is("SDL_VIDEO_DRIVER", "offscreen");
    SessionOptions session_options;
    session_options.dir = options_.dir;
    session_options.pack = options_.pack;
    session_options.jobs = options_.jobs;
    session_options.font_path = options_.font_path;
    session_options.audio_device = options_.audio && !headless && !platform::env_is("CUERVO_AUDIO", "null");
    if (pluma::dap::Debugger* dbg = options_.debugger) {
        session_options.debug_hooks = dbg;
        session_options.on_active_vm = [dbg](pluma::VM& vm) { dbg->attach(vm); };
        // While stopped at a breakpoint: keep the window responsive (input is dropped).
        dbg->set_idle([this] {
            if (device_ == nullptr) return;
            DeviceState ignored;
            window_.poll_events(ignored);
        });
    }
    session_options.profile_scripts = options_.profile;
    if (options_.profile) profiler::set_enabled(true);
    std::string error;
    const double compile_start = now_seconds();
    if (!session_.init(session_options, error)) return false; // logged
    const Project& project = session_.project();
    LOG_INFO("Game '{}': {} script file(s) compiled in {:.1f} ms, main {}, {} Hz, seed {}, {} tier", project.name,
             session_.script_count(),
             (now_seconds() - compile_start) * 1000.0,
             project.main_scene.empty() ? "class '" + project.main_class + "'" : "scene '" + project.main_scene + "'",
             project.tick_rate, project.seed, rhi::tier_name(project.render_tier));

    rhi::DeviceOptions device_options;
    device_options.vsync = options_.vsync;
    // The device is gated to the project's tier, so a Compatible game tested
    // on Vulkan can't use what WebGL2 lacks (rhi::Feature).
    device_options.tier = project.render_tier;
    device_ = open_device(window_, options_.backend, device_options, project.name, project.virtual_size, options_.gl_es);
    // A Full project must not quietly fall back to GL: it would be refused anyway.
    if (!device_ && options_.allow_fallback && options_.backend == rhi::Backend::Vulkan &&
        project.render_tier == rhi::Tier::Compatible) {
        LOG_WARN("Vulkan unavailable, falling back to OpenGL");
        device_ = open_device(window_, rhi::Backend::OpenGL, device_options, project.name, project.virtual_size,
                              options_.gl_es);
    }
    if (!device_) return false;
    if (!check_device_tier(device_->info(), project.render_tier)) return false;

    if (!shapes_.init(*device_, 4096) || !text_.init(*device_) || !meshes_.init(*device_)) return false;
    if (session_.font_atlas() != nullptr) font_ = text_.add_font(*session_.font_atlas());

    timestep_.emplace(project.tick_rate);
    capture_mode_ = !options_.screenshot.empty();
    session_.set_presses(options_.presses);
    if (!options_.record.empty()) session_.start_recording();
    // Escape quits unless the game binds it to an action (e.g. a pause menu,
    // which then quits through Scene.quit()). A browser tab has no "quit".
    escape_quits_ = platform::os != platform::Os::Web;
    for (const auto& [name, bindings] : project.actions) {
        if (std::find(bindings.begin(), bindings.end(), "Key.Escape") != bindings.end()) escape_quits_ = false;
    }
    start_ = previous_ = last_watch_ = now_seconds();
    return true;
}

void GameLoop::hot_reload(double now) {
    const bool force_reload = devices_.keys_tapped[static_cast<std::size_t>(Key::F5)];
    const bool restart = devices_.keys_tapped[static_cast<std::size_t>(Key::F6)];
    // Development only: an exported game (a pack) never reloads.
    if (!options_.hot_reload || session_.packed() || capture_mode_ || !(force_reload || restart || now - last_watch_ >= 0.5)) return;
    last_watch_ = now;
    const double t0 = now_seconds();
    if (session_.reload(true, force_reload, restart) == GameSession::Reload::Reloaded) {
        LOG_INFO("Hot reload #{} in {:.1f} ms", session_.host().reloads(), (now_seconds() - t0) * 1000.0);
    }
}

bool GameLoop::frame() {
    // One profiler frame per loop iteration (cuervo/core/profiler.hpp).
    profiler::frame_mark();
    CUERVO_PROFILE_ZONE("frame");
    const double now = now_seconds();
    bool running = window_.poll_events(devices_);
    const std::uint64_t stops_before = options_.debugger != nullptr ? options_.debugger->stops() : 0;
    if (options_.debugger != nullptr) {
        options_.debugger->poll(); // breakpoint changes, pause, disconnect
        if (options_.debugger->terminate_requested()) running = false;
    }
    if (escape_quits_ && devices_.key(Key::Escape)) running = false;

    hot_reload(now); // development; DESIGN.md 5.13

    ScriptHost& host = session_.host();
    const double frame_ms = (now - previous_) * 1000.0;
    FixedTimestep::Step step = timestep_->advance(now - previous_);
    previous_ = now;
    if (capture_mode_) {
        // Deterministic capture: one tick per frame, independent of timing.
        step = {host.tick_count() < options_.screenshot_tick ? 1 : 0, 1.0f, false};
    }
    // The pointer in virtual coordinates, recorded in the input frame.
    const Project& project = session_.project();
    const render::ViewportFit pointer_fit = render::fit_viewport(options_.scale, project.virtual_size, window_.pixel_size());
    const vec2 pointer = render::framebuffer_to_virtual(pointer_fit, devices_.mouse_position);
    const double sim_start = now_seconds();
    {
        CUERVO_PROFILE_ZONE("simulation");
        for (int i = 0; i < step.ticks; ++i) session_.tick(session_.sample(devices_, pointer));
    }
    const double sim_seconds = now_seconds() - sim_start;
    if (host.quit_requested()) running = false;

    render();
    const bool capture = capture_mode_ && host.tick_count() >= options_.screenshot_tick;
    if (capture) device_->capture_frame(options_.screenshot);
    {
        CUERVO_PROFILE_ZONE("end_frame");
        device_->end_frame();
    }
    ++frames_;
    if (options_.bench > 0.0 && !capture_mode_) {
        bench_frame(now, frame_ms, sim_seconds, step.ticks);
        if (bench_.done) running = false;
    }
    // Time spent stopped in the debugger isn't game time: restart the clock
    // instead of catching up on it.
    if (options_.debugger != nullptr && options_.debugger->stops() != stops_before) previous_ = now_seconds();
    if (capture) {
        LOG_INFO("Screenshot written to '{}' at tick {} (state hash {:016x})", options_.screenshot, host.tick_count(),
                 host.state_hash());
        running = false;
    }
    if (options_.quit_after > 0.0 && now - start_ >= options_.quit_after) {
#if CUERVO_FEATURE_PARTICLES
        const std::size_t pcount = host.particles().particle_count();
#else
        const std::size_t pcount = 0;
#endif
        LOG_INFO("--quit-after reached ({} ticks, {} frames, {} particles, state hash {:016x})", host.tick_count(),
                 frames_, pcount, host.state_hash());
        running = false;
    }
    return running;
}

// Records the latest tick's frame; end_frame (and a capture) is frame()'s.
void GameLoop::render() {
    const Project& project = session_.project();
    ScriptHost& host = session_.host();
    CUERVO_PROFILE_ZONE("render");
    {
        CUERVO_PROFILE_ZONE("begin_frame");
        device_->begin_frame(window_.pixel_size());
    }
    // CPU time from here (begin_frame may block on VSync / frames in flight)
    // to the end of command recording: clears, batch build, upload, draws.
    const double render_start = now_seconds();
    const ivec2 fb = device_->framebuffer_size();
    const render::ViewportFit fit = render::fit_viewport(options_.scale, project.virtual_size, fb);
    device_->clear({0.0f, 0.0f, 0.0f, 1.0f});
    device_->set_viewport(fit.viewport);
    device_->set_scissor(fit.viewport);
    device_->clear(project.clear_color);
    shapes_.clear();
    text_.clear();
    const float pixels = std::min(fit.scale.x(), fit.scale.y());
    const vec2 virtual_extent{static_cast<float>(project.virtual_size.x()), static_cast<float>(project.virtual_size.y())};
    // The scene's Camera2D (Canvas.set_view) composes with this; the error
    // overlay below is in screen coordinates.
    const Affine2 world_to_ndc = render::virtual_to_ndc(virtual_extent);
    meshes_.clear();
    {
        CUERVO_PROFILE_ZONE("draw");
        host.draw(make_draw_targets(shapes_, text_, font_, &vectors_, world_to_ndc, pixels, true));
    }
    // Errors (compile errors after a reload, panics) as an overlay.
    const std::string& err = host.error();
    if (!err.empty() && font_) {
        render::TextStyle style;
        style.size = 15.0f;
        style.max_width = virtual_extent.x() - 40.0f;
        shapes_.rounded_rect({virtual_extent.x() * 0.5f, 110.0f}, {virtual_extent.x() * 0.5f - 10.0f, 100.0f}, 8.0f,
                             0.0f, {0.25f, 0.02f, 0.05f, 0.92f});
        text_.draw(font_,
                   (host.panicked() ? "Paused by a panic (fix and save to reload):\n" : "Script errors:\n") + err,
                   vec2{20.0f, 18.0f}, style, {1.0f, 0.85f, 0.85f, 1.0f});
    }
    CUERVO_PROFILE_ZONE("flush");
    shapes_.flush(world_to_ndc, pixels > 0.0f ? 1.0f / pixels : 1.0f);
    text_.flush(world_to_ndc);
    render_cpu_ = now_seconds() - render_start;
}

void GameLoop::bench_frame(double now, double frame_ms, double sim_seconds, int ticks) {
    if (bench_.done) return;
    if (bench_.measure_start < 0.0) {
        if (now - start_ < options_.bench_warmup) return;
        bench_.measure_start = now; // this frame's time still includes the warmup's tail
        LOG_INFO("Benchmark: warmed up for {:.1f} s, measuring {:.1f} s", options_.bench_warmup, options_.bench);
        return;
    }
    bench_.frame_ms.push_back(frame_ms);
    bench_.sim_seconds += sim_seconds;
    bench_.render_seconds += render_cpu_;
    bench_.ticks += static_cast<std::uint64_t>(ticks);
#if CUERVO_FEATURE_PARTICLES
    bench_.particle_sum += static_cast<double>(session_.host().particles().particle_count());
#endif
    if (now - bench_.measure_start >= options_.bench) {
        bench_report();
        bench_.done = true;
    }
}

void GameLoop::bench_report() const {
    std::vector<double> sorted = bench_.frame_ms;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](double p) {
        if (sorted.empty()) return 0.0;
        const auto index = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
        return sorted[std::min(index, sorted.size() - 1)];
    };
    double total = 0.0;
    for (double ms : sorted) total += ms;
    const double frames = static_cast<double>(sorted.size());
    const rhi::DeviceInfo& info = device_->info();
    const ivec2 fb = device_->framebuffer_size();
    LOG_INFO("bench | {} | {} | {}x{} {}x MSAA | {:.0f} avg particles | {} frames in {:.2f} s | {:.1f} FPS | "
             "frame ms avg {:.3f} p50 {:.3f} p95 {:.3f} p99 {:.3f} max {:.3f} | sim {:.3f} ms/tick | "
             "render CPU {:.3f} ms/frame",
             session_.project().name, rhi::backend_name(info.backend), fb.x(), fb.y(), info.msaa_samples,
             frames > 0 ? bench_.particle_sum / frames : 0.0, sorted.size(), total / 1000.0,
             total > 0.0 ? frames * 1000.0 / total : 0.0, frames > 0 ? total / frames : 0.0, percentile(0.50),
             percentile(0.95), percentile(0.99), sorted.empty() ? 0.0 : sorted.back(),
             bench_.ticks ? bench_.sim_seconds * 1000.0 / static_cast<double>(bench_.ticks) : 0.0,
             frames > 0 ? bench_.render_seconds * 1000.0 / frames : 0.0);
}

int GameLoop::finish() {
    if (options_.profile && session_.started()) log_profile(session_.host());
    if (options_.debugger != nullptr) options_.debugger->detach(); // before the VMs go
    bool failed = false;
    if (session_.recording()) {
        std::string error;
        if (save_replay(options_.record, session_.recorded(), error)) {
            LOG_INFO("Replay of {} ticks written to '{}'", session_.recorded().ticks(), options_.record);
        } else {
            LOG_ERROR("--record: {}", error);
            failed = true;
        }
    }
    text_.shutdown();
    meshes_.shutdown();
    shapes_.shutdown();
    device_.reset();
    window_.destroy();
    // The session (host, audio) goes with the loop; a panic is a failure.
    return failed || (session_.started() && session_.host().panicked()) ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace

int run_game(const GameOptions& options) {
    auto loop = std::make_unique<GameLoop>(options);
    if (!loop->init()) {
        loop->finish();
        return EXIT_FAILURE;
    }
    return platform::run_main_loop(std::move(loop));
}

} // namespace cuervo::script

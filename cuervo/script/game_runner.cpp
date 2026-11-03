#include "cuervo/script/game_runner.hpp"

#include <SDL3/SDL_timer.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "cuervo/profile/trace.hpp"
#include "pluma/profiler.hpp"
#include "cuervo/input/input_map.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/platform/file_watcher.hpp"
#include "cuervo/platform/main_loop.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/assets/image.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/post_fx.hpp"
#include "cuervo/render/user_shader.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/render/sprite_batcher.hpp"
#include "cuervo/render/sprite_library.hpp"
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
#if defined(__EMSCRIPTEN__)
    // performance.now(): microseconds in a cross-origin isolated page. SDL's
    // counter only advances in whole milliseconds in the browser, which made
    // every web --bench frame 16 or 17 ms (milestone 10.4).
    return emscripten_get_now() / 1000.0;
#else
    return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
#endif
}

GraphicsApi graphics_api_for(rhi::Backend backend) { return rhi::graphics_api(backend); }

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
    int init_exit_code() const { return init_exit_code_; }

private:
    void hot_reload(double now);
    void render();
    void bench_frame(double now, double frame_ms, double sim_seconds, int ticks);
    void bench_report() const;
    bool init_net(std::string& error);
    void net_step(DeviceState& source, vec2 pointer, int ticks);
    void net_collect();
    std::uint64_t sim_ticks_limit(const NetContext& net) const;
    // Ticks simulated and final: the confirmed ones while a network session runs (rollback predicts beyond).
    std::uint64_t sim_ticks() const;

    GameOptions options_;
    int init_exit_code_ = EXIT_FAILURE;
    GameSession session_;
    Window window_;
    std::unique_ptr<rhi::Device> device_;
    render::SdfBatcher shapes_;
    render::LightBatcher lights_;
    render::TextRenderer text_;
    // Sprites (Canvas.vector): the meshes and the images they come from.
    render::MeshBatcher meshes_;
    VectorCache vectors_{meshes_};
    // Raster sprites (Canvas.sprite / nine_patch): the images and their batcher.
    render::SpriteBatcher sprite_batcher_;
    render::SpriteLibrary sprite_library_;
    render::FontHandle font_;
    // Post-processing / linear color (DESIGN.md 4.19): the frame is drawn into
    // `scene_target_` and PostFx::apply writes it to the window.
    render::PostFx post_;
    rhi::RenderTargetHandle scene_target_;
    ivec2 scene_target_size_{0, 0};
    rhi::TextureHandle lut_;
    std::unique_ptr<render::UserShader> user_shader_;
    rhi::PipelineHandle custom_pipeline_;
    render::PostSettings post_settings_;
    bool use_post_ = false;
    bool linear_ = false;
    std::optional<FixedTimestep> timestep_;
    DeviceState devices_;
    bool escape_quits_ = true;
    bool capture_mode_ = false;
    // Networked play: the delivered frames of every player and the state hash after each confirmed
    // tick, written as the replay (docs/REPLAYS.md version 4) so the match replays offline.
    bool net_seen_ = false;
    bool net_recording_ = false;
    bool net_collect_failed_ = false;
    std::uint64_t net_base_ = 0; // sim ticks that ran before the session started (the replay's prefix)
    unsigned net_owed_ = 0; // ticks the clock owes the session (it stalls while a peer's input is missing)
    Replay net_replay_;
    static constexpr unsigned net_max_catch_up = 4; // bounds the burst after a stall (frame pacing)
    double start_ = 0.0;
    double previous_ = 0.0;
    double last_watch_ = 0.0;
    std::uint64_t frames_ = 0;
    pluma::HeapProfiler heap_profile_; // --profile / --trace: one sample per frame
    double render_cpu_ = 0.0; // seconds of the last render() after begin_frame

    // --bench samples, taken after the warmup.
    struct Bench {
        double measure_start = -1.0; // < 0: still warming up
        std::vector<double> frame_ms;
        std::vector<double> sim_tick_ms; // per frame: its simulation time / its ticks (frames with no tick skipped)
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
    if (!options_.trace.empty()) options_.profile = true;
    session_options.profile_scripts = options_.profile;
    if (options_.profile) profiler::set_enabled(true);
    if (!options_.trace.empty()) profile::start_capture();
    std::string error;
    const double compile_start = now_seconds();
    if (!session_.init(session_options, error)) {
        if (session_.compile_failed()) init_exit_code_ = exit_compile_error; // logged
        return false;
    }
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
    device_options.linear_color = project.linear_color;
    device_ = open_device(window_, options_.backend, device_options, project.name, project.virtual_size, options_.gl_es);
    // A Full project must not quietly fall back to GL: it would be refused anyway.
    // The default backend (Vulkan, or WebGPU in a browser) falls back to GL
    // (WebGL2 on the web, DESIGN.md 4.4 "Clarified (Milestone 10.3)").
    if (!device_ && options_.allow_fallback && options_.backend != rhi::Backend::OpenGL &&
        project.render_tier == rhi::Tier::Compatible) {
        LOG_WARN("{} unavailable, falling back to OpenGL", rhi::backend_name(options_.backend));
        device_ = open_device(window_, rhi::Backend::OpenGL, device_options, project.name, project.virtual_size,
                              options_.gl_es);
    }
    if (!device_) return false;
    if (!check_device_tier(device_->info(), project.render_tier)) return false;

    if (!shapes_.init(*device_, 4096) || !lights_.init(*device_) || !text_.init(*device_) || !meshes_.init(*device_) ||
        !sprite_batcher_.init(*device_)) {
        return false;
    }
    sprite_library_.init(device_.get());
    // Linear color needs sRGB targets, which the backend may not offer (then the
    // game blends in sRGB space, as with color_space = "srgb").
    linear_ = project.linear_color && device_->info().linear_color;
    if (project.linear_color && !linear_) LOG_WARN("color_space = \"linear\" is unavailable on this device; using sRGB blending");
    use_post_ = (project.post_enabled || linear_) && device_->info().supports(rhi::Feature::RenderTargets) &&
                post_.init(*device_);
    if (project.linear_color || project.post_enabled) {
        LOG_INFO("Color: {} blending, post-processing {}", linear_ ? "linear" : "sRGB", use_post_ ? "on" : "off");
    }
    if (use_post_) {
        post_settings_ = project.post;
        // Files come through the VFS (dev directory or pack). Packs hold images as
        // .ctex and shaders as .cshader (cuervo-tools export imports them).
        const assets::Vfs& vfs = session_.vfs();
        const auto swap_ext = [](std::string path, std::string_view ext) {
            const std::size_t dot = path.rfind('.');
            return (dot == std::string::npos ? path : path.substr(0, dot)) + std::string(ext);
        };
        const auto read_vfs = [&](const std::string& path, std::vector<std::byte>& out) {
            std::string err;
            auto file = vfs.open(path, err);
            if (!file) return false;
            out.assign(file->bytes().begin(), file->bytes().end());
            return true;
        };
        if (!project.post_lut.empty()) {
            std::vector<std::byte> raw;
            std::optional<assets::Image> image;
            std::string failure = "not found";
            if (read_vfs(swap_ext(project.post_lut, ".ctex"), raw)) {
                if (auto tex = assets::decode_ctex(raw)) {
                    const auto level0 = tex->level(0);
                    image = assets::Image{tex->width, tex->height, std::vector<std::uint8_t>(level0.begin(), level0.end())};
                }
            } else if (read_vfs(project.post_lut, raw)) {
                assets::DecodeResult decoded = assets::decode_image(raw);
                image = std::move(decoded.image);
                failure = decoded.error;
            }
            if (image && image->width == image->height * image->height) {
                rhi::TextureDesc lut;
                lut.width = image->width;
                lut.height = image->height;
                lut.filter = rhi::Filter::Linear;
                lut.initial_data = image->rgba.data();
                lut.debug_name = "post_lut";
                lut_ = device_->create_texture(lut); // LUT values are display-referred bytes: not sRGB-decoded
                post_settings_.lut = lut_;
                post_settings_.lut_size = image->height;
            } else {
                LOG_WARN("[render.post] lut '{}': {}", project.post_lut, image ? "expected a (N*N x N) strip" : failure);
            }
        }
        if (!project.post_shader.empty()) {
            std::vector<std::byte> raw;
            if (!read_vfs(swap_ext(project.post_shader, ".cshader"), raw)) {
                LOG_WARN("[render.post] shader '{}': no compiled .cshader (run: cuervo-tools shader build <project>)",
                         project.post_shader);
            } else if (auto compiled = render::decode_cshader(raw)) {
                user_shader_ = std::make_unique<render::UserShader>(std::move(*compiled));
                custom_pipeline_ = post_.create_pass_pipeline(user_shader_->source(), rhi::BlendMode::None, "user_post");
                post_settings_.custom = custom_pipeline_;
                post_settings_.custom_params = user_shader_->defaults();
                for (const auto& [name, values] : project.post_params) {
                    if (!user_shader_->set(post_settings_.custom_params, name, values)) {
                        LOG_WARN("[render.post.params] '{}' is not an @export of '{}'", name, project.post_shader);
                    }
                }
            } else {
                LOG_WARN("[render.post] shader '{}': not a valid .cshader", project.post_shader);
            }
        }
    } else if (project.post_enabled) {
        LOG_WARN("[render.post] is unavailable on this device");
    }
    // Integer scaling means crisp pixels: sprites default to nearest filtering there (DESIGN.md 4.4).
    sprite_library_.set_default_filter(options_.scale == render::ScaleMode::Integer ? rhi::Filter::Nearest
                                                                                    : rhi::Filter::Linear);
    if (session_.font_atlas() != nullptr) {
        font_ = text_.add_font(*session_.font_atlas());
        std::vector<render::FontHandle> fallbacks;
        for (const render::FontAtlas& f : session_.fallback_atlases()) fallbacks.push_back(text_.add_font(f));
        if (!fallbacks.empty()) text_.set_fallbacks(font_, std::move(fallbacks));
    }

    timestep_.emplace(project.tick_rate);
    capture_mode_ = !options_.screenshot.empty();
    session_.set_presses(options_.presses);
    session_.set_pointer_script(options_.pointers);
    session_.set_typed(options_.typed);
    if (!options_.record.empty()) session_.start_recording();
    if (!init_net(error)) return false;
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
    if (profile::capturing()) {
        // Frame boundary: draw calls of the frame just rendered, and the Pluma heap.
        profile::end_frame();
        profile::add_counter("heap_live", profiler::clock_ns(), static_cast<double>(heap_profile_.sample().live));
    }
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
        step = {sim_ticks() < options_.screenshot_tick ? 1 : 0, 1.0f, false};
    }
    // The pointer in virtual coordinates, recorded in the input frame.
    const Project& project = session_.project();
    const render::ViewportFit pointer_fit = render::fit_viewport(options_.scale, project.virtual_size, window_.pixel_size());
    vec2 pointer = render::framebuffer_to_virtual(pointer_fit, devices_.mouse_position);
    // A capture reads only the scripted input (--press, --pointer, --type), never
    // the devices: on a desktop without an offscreen driver (Windows) the window
    // is real, and a cursor over it would move UI hover and change the state hash.
    DeviceState no_devices;
    DeviceState& source = capture_mode_ ? no_devices : devices_;
    if (capture_mode_) pointer = {0.0f, 0.0f};
    const double sim_start = now_seconds();
    {
        CUERVO_PROFILE_ZONE("simulation");
        if (host.net_context().active()) {
            net_step(source, pointer, step.ticks);
        } else {
            for (int i = 0; i < step.ticks; ++i) {
                session_.tick(session_.sample(source, pointer));
                // A game that started a session this tick hands the rest to the session, so every peer's
                // first networked tick is the same one (the replay's prefix ends here).
                if (host.net_context().active()) break;
            }
        }
    }
    const double sim_seconds = now_seconds() - sim_start;
    if (host.quit_requested()) running = false;
    window_.set_text_input(host.wants_text_input());
    // IME composition is live display state (never in the input frame, so replays carry only
    // committed text); capture mode shows none.
    if (host.wants_text_input() && !capture_mode_) {
        host.set_composition(devices_.composition, devices_.composition_cursor);
    } else {
        host.set_composition({}, -1);
        devices_.composition.clear();
        devices_.composition_cursor = -1;
    }
    host.clear_text_area();

    render();
    if (host.has_text_area()) {
        const vec2 p = host.text_area_position(), sz = host.text_area_size();
        window_.set_text_input_area({static_cast<int>(p.x()), static_cast<int>(p.y())},
                                    {static_cast<int>(sz.x()), static_cast<int>(sz.y())},
                                    static_cast<int>(host.text_area_caret()));
    }
    const bool capture = capture_mode_ && sim_ticks() >= options_.screenshot_tick;
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
        if (net_seen_) {
            // The confirmed tick's hashes (rollback's live state may be ahead on predicted input).
            const std::uint64_t tick = std::min<std::uint64_t>(sim_ticks(), options_.screenshot_tick);
            const std::uint64_t stick = tick - net_base_; // the session's own numbering
            const NetContext& net = host.net_context();
            LOG_INFO("Screenshot written to '{}' at tick {} (state hash {:016x})", options_.screenshot, tick,
                     net.state_hash_at(static_cast<std::uint32_t>(stick)).value_or(0));
            LOG_INFO("Snapshot hash {:016x} at tick {}",
                     net.snapshot_hash_at(static_cast<std::uint32_t>(stick)).value_or(0), tick);
        } else {
            LOG_INFO("Screenshot written to '{}' at tick {} (state hash {:016x})", options_.screenshot,
                     host.tick_count(), host.state_hash());
            // The stronger hash over all simulation state (ScriptHost::snapshot_hash); the determinism
            // matrix compares it as well.
            LOG_INFO("Snapshot hash {:016x} at tick {}", host.snapshot_hash(), host.tick_count());
        }
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

std::uint64_t GameLoop::sim_ticks() const {
    const ScriptHost& host = session_.host();
    return host.net_context().active() ? net_base_ + host.net_context().confirmed_tick() : host.tick_count();
}

// Starts the session the flags ask for (a game may also call Net.host / Net.join itself).
bool GameLoop::init_net(std::string& error) {
    NetContext& net = session_.host().net_context();
    const bool wanted = options_.net_host >= 0 || !options_.net_join.empty() || !options_.net_relay.empty() || !options_.net_mesh.empty();
    if (!options_.net_mode.empty()) {
        net.set_mode(options_.net_mode == "rollback" ? NetContext::Mode::Rollback : NetContext::Mode::Lockstep);
    }
    if (!wanted) return true;
    bool ok = false;
    if (options_.net_host >= 0) {
        ok = net.host(static_cast<std::uint16_t>(options_.net_host), error);
    } else if (!options_.net_mesh.empty()) {
        ok = net.mesh(options_.net_mesh, options_.net_player, error);
    } else if (!options_.net_join.empty()) {
        ok = net.join(options_.net_join, error);
    } else {
        ok = net.relay(options_.net_relay, options_.net_room, options_.net_players, error);
    }
    if (!ok) {
        LOG_ERROR("network: {}", error);
        return false;
    }
    // Two peers on a wall clock must not hot-reload scripts under each other.
    options_.hot_reload = false;
    return true;
}

// Drives the session at the fixed step. `ticks` is what the clock owes this frame; a tick the session
// cannot run yet (a peer's input is missing, or rollback is at its limit) stays owed, so the match
// advances at the pace of the slowest peer, and the backlog is bounded so a long stall is not
// followed by a burst of ticks.
void GameLoop::net_step(DeviceState& source, vec2 pointer, int ticks) {
    ScriptHost& host = session_.host();
    if (!net_seen_) {
        net_seen_ = true;
        net_base_ = host.tick_count();
        // A session that starts after tick 0 keeps the ticks played before it as the replay's prefix (the
        // local input frames and hashes, with the other players idle), so an offline replay runs the whole
        // run and reaches the session start in the same state. Session ticks are numbered from 1 at the start.
        const Replay& before = session_.recorded();
        net_recording_ = !options_.record.empty() && before.frames.size() == net_base_ && before.hashes.size() == net_base_;
        if (!options_.record.empty() && !net_recording_) {
            LOG_WARN("--record: the ticks before the network session were not all recorded; this run is not recorded");
        }
        const Replay prefix = net_recording_ ? before : Replay{};
        session_.stop_recording();
        net_replay_ = net_recording_ ? prefix : session_.replay_header();
        net_replay_.extra_players.assign(host.net_context().players() - 1, std::vector<InputFrame>(net_base_));
    }
    net_owed_ = std::min(net_owed_ + static_cast<unsigned>(std::max(ticks, 0)), net_max_catch_up);
    while (net_owed_ > 0) {
        if (capture_mode_ && sim_ticks() >= options_.screenshot_tick) break;
        if (!host.net_tick(session_.sample(source, pointer))) {
            // Waiting on a peer: every step re-sends our unacked input, and a loop that spins (VSync off,
            // a busy machine) would flood the link and starve the relay. A few milliseconds of rest bound it.
#ifndef __EMSCRIPTEN__
            if (!capture_mode_ || sim_ticks() < options_.screenshot_tick) SDL_Delay(3);
#endif
            break;
        }
        --net_owed_;
        if (!host.net_context().active()) return; // the game left the session
        net_collect();
    }
    net_collect();
}

std::uint64_t GameLoop::sim_ticks_limit(const NetContext& net) const {
    const std::uint64_t confirmed = net_base_ + net.confirmed_tick();
    return capture_mode_ ? std::min<std::uint64_t>(confirmed, options_.screenshot_tick) : confirmed;
}

// Appends every newly confirmed tick (all players' delivered frames and the state hash after it).
void GameLoop::net_collect() {
    if (!net_recording_ || net_collect_failed_) return;
    const NetContext& net = session_.host().net_context();
    const std::uint64_t limit = sim_ticks_limit(net);
    while (net_replay_.frames.size() < limit) {
        const auto tick = static_cast<std::uint32_t>(net_replay_.frames.size() + 1 - net_base_); // session tick
        const auto hash = net.state_hash_at(tick);
        const InputFrame* first = net.confirmed_frame(0, tick);
        if (!hash || !first) {
            LOG_WARN("--record: tick {} is no longer retained; the replay ends at tick {}", tick + net_base_,
                     tick + net_base_ - 1);
            net_collect_failed_ = true;
            return;
        }
        net_replay_.frames.push_back(*first);
        for (std::size_t p = 1; p <= net_replay_.extra_players.size(); ++p) {
            const InputFrame* f = net.confirmed_frame(static_cast<std::uint32_t>(p), tick);
            net_replay_.extra_players[p - 1].push_back(f != nullptr ? *f : InputFrame{});
        }
        net_replay_.hashes.push_back(*hash);
    }
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
    if (use_post_) {
        const ivec2 size = device_->framebuffer_size();
        if (!scene_target_ || scene_target_size_ != size) {
            if (scene_target_) device_->destroy_render_target(scene_target_);
            scene_target_ = device_->create_render_target({size.x(), size.y(), rhi::Filter::Linear, "scene"});
            scene_target_size_ = size;
        }
        if (!scene_target_) LOG_ERROR("could not create the {}x{} scene render target", size.x(), size.y());
        if (scene_target_) device_->set_render_target(scene_target_);
    }
    const ivec2 fb = device_->framebuffer_size();
    const render::ViewportFit fit = render::fit_viewport(options_.scale, project.virtual_size, fb);
    device_->clear({0.0f, 0.0f, 0.0f, 1.0f});
    device_->set_viewport(fit.viewport);
    device_->set_scissor(fit.viewport);
    device_->clear(linear_ ? render::srgb_to_linear(project.clear_color) : project.clear_color);
    shapes_.clear();
    text_.clear();
    const float pixels = std::min(fit.scale.x(), fit.scale.y());
    const vec2 virtual_extent{static_cast<float>(project.virtual_size.x()), static_cast<float>(project.virtual_size.y())};
    // The scene's Camera2D (Canvas.set_view) composes with this; the error
    // overlay below is in screen coordinates.
    const Affine2 world_to_ndc = render::virtual_to_ndc(virtual_extent);
    meshes_.clear();
    sprite_batcher_.clear();
    {
        CUERVO_PROFILE_ZONE("draw");
        // A capture (--screenshot) shows only its last frame: the frames before
        // it skip the 2D lighting pass, which is render only (DESIGN.md 4.17,
        // 9.7) and slow on software WebGL (the web export's verify run).
        const bool lit = !capture_mode_ || host.tick_count() >= options_.screenshot_tick;
        DrawTargets targets =
            make_draw_targets(shapes_, text_, font_, &vectors_, world_to_ndc, pixels, true, lit ? &lights_ : nullptr,
                               &sprite_library_, &sprite_batcher_);
        // Canvas.set_clip: virtual pixels to a scissor in the letterboxed viewport (DESIGN.md 4.17, 9.3).
        rhi::Device* const dev = device_.get();
        targets.clip = [dev, fit](bool on, vec2 pos, vec2 size) {
            if (!on) {
                dev->set_scissor(fit.viewport);
                return;
            }
            const auto px = [&](float v, float scale, std::int32_t origin) {
                return origin + static_cast<std::int32_t>(std::lround(v * scale));
            };
            const std::int32_t x0 = std::max(px(pos.x(), fit.scale.x(), fit.viewport.x), fit.viewport.x);
            const std::int32_t y0 = std::max(px(pos.y(), fit.scale.y(), fit.viewport.y), fit.viewport.y);
            const std::int32_t x1 = std::min(px(pos.x() + size.x(), fit.scale.x(), fit.viewport.x),
                                             fit.viewport.x + fit.viewport.width);
            const std::int32_t y1 = std::min(px(pos.y() + size.y(), fit.scale.y(), fit.viewport.y),
                                             fit.viewport.y + fit.viewport.height);
            dev->set_scissor({x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0)});
        };
        host.draw(targets);
        dev->set_scissor(fit.viewport);
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
    // The SDF batcher flushes as one instanced draw; sprites, lights and the
    // other batchers report through profile::add_draw_calls themselves.
    profile::add_draw_calls(1 + static_cast<std::uint32_t>(text_.last_draw_calls() + meshes_.last_draw_calls()));
    if (use_post_ && scene_target_) {
        device_->set_viewport({0, 0, scene_target_size_.x(), scene_target_size_.y()});
        post_.apply(scene_target_, scene_target_size_, {}, post_settings_);
    }
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
    if (ticks > 0) bench_.sim_tick_ms.push_back(sim_seconds * 1000.0 / ticks);
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
    std::vector<double> sim_sorted = bench_.sim_tick_ms;
    std::sort(sim_sorted.begin(), sim_sorted.end());
    const auto sim_percentile = [&](double p) {
        if (sim_sorted.empty()) return 0.0;
        const auto index = static_cast<std::size_t>(p * static_cast<double>(sim_sorted.size() - 1) + 0.5);
        return sim_sorted[std::min(index, sim_sorted.size() - 1)];
    };
    double total = 0.0;
    for (double ms : sorted) total += ms;
    const double frames = static_cast<double>(sorted.size());
    const rhi::DeviceInfo& info = device_->info();
    const ivec2 fb = device_->framebuffer_size();
    LOG_INFO("bench | {} | {} | {}x{} {}x MSAA | {:.0f} avg particles | {} frames in {:.2f} s | {:.1f} FPS | "
             "frame ms avg {:.3f} p50 {:.3f} p95 {:.3f} p99 {:.3f} max {:.3f} | sim {:.3f} ms/tick (p95 {:.3f} max {:.3f}, {} ticks) | "
             "render CPU {:.3f} ms/frame",
             session_.project().name, rhi::backend_name(info.backend), fb.x(), fb.y(), info.msaa_samples,
             frames > 0 ? bench_.particle_sum / frames : 0.0, sorted.size(), total / 1000.0,
             total > 0.0 ? frames * 1000.0 / total : 0.0, frames > 0 ? total / frames : 0.0, percentile(0.50),
             percentile(0.95), percentile(0.99), sorted.empty() ? 0.0 : sorted.back(),
             bench_.ticks ? bench_.sim_seconds * 1000.0 / static_cast<double>(bench_.ticks) : 0.0,
             sim_percentile(0.95), sim_sorted.empty() ? 0.0 : sim_sorted.back(), bench_.ticks,
             frames > 0 ? bench_.render_seconds * 1000.0 / frames : 0.0);
    // Mean frame time of each second of the run, in order (a trailing part of
    // under half a second is dropped). A cliff -- a stretch of seconds much
    // slower than the rest, as a GPU memory or driver limit would cause -- shows
    // as a worst window far above the median one (tools/bench.py "cliff_ratio").
    std::vector<double> windows;
    double window_ms = 0.0;
    std::size_t window_frames = 0;
    for (double ms : bench_.frame_ms) {
        window_ms += ms;
        ++window_frames;
        if (window_ms >= 1000.0) {
            windows.push_back(window_ms / static_cast<double>(window_frames));
            window_ms = 0.0;
            window_frames = 0;
        }
    }
    if (window_frames > 0 && window_ms >= 500.0) windows.push_back(window_ms / static_cast<double>(window_frames));
    if (windows.empty()) return;
    std::vector<double> windows_sorted = windows;
    std::sort(windows_sorted.begin(), windows_sorted.end());
    const double median = windows_sorted[(windows_sorted.size() - 1) / 2];
    std::string means;
    for (double ms : windows) means += std::format("{}{:.3f}", means.empty() ? "" : " ", ms);
    LOG_INFO("bench windows | {} x 1 s | frame ms mean {} | worst / median {:.3f}", windows.size(), means,
             median > 0.0 ? windows_sorted.back() / median : 1.0);
}

int GameLoop::finish() {
    if (options_.profile && session_.started()) log_profile(session_.host());
    if (options_.profile && heap_profile_.samples() > 0) LOG_INFO("profile | {}", heap_profile_.report());
    if (profile::capturing()) {
        profile::stop_capture();
        std::string error;
        if (profile::write_chrome_trace(options_.trace, &error)) {
            LOG_INFO("profile | trace written to {} ({} events, {} dropped)", options_.trace, profile::events().size(),
                     profile::dropped_events());
        } else {
            LOG_ERROR("profile | {}", error);
        }
    }
    if (options_.debugger != nullptr) options_.debugger->detach(); // before the VMs go
    bool failed = false;
    if (net_recording_ || session_.recording()) {
        std::string error;
        const Replay& replay = net_recording_ ? net_replay_ : session_.recorded();
        if (save_replay(options_.record, replay, error)) {
            LOG_INFO("Replay of {} ticks written to '{}'{}", replay.ticks(), options_.record,
                     net_recording_ ? std::format(" ({} players)", replay.extra_players.size() + 1) : std::string());
        } else {
            LOG_ERROR("--record: {}", error);
            failed = true;
        }
    }
    if (scene_target_) device_->destroy_render_target(scene_target_);
    if (lut_) device_->destroy_texture(lut_);
    if (custom_pipeline_) device_->destroy_pipeline(custom_pipeline_);
    post_.shutdown();
    text_.shutdown();
    sprite_library_.shutdown();
    sprite_batcher_.shutdown();
    meshes_.shutdown();
    lights_.shutdown();
    shapes_.shutdown();
    device_.reset();
    window_.destroy();
    // The session (host, audio) goes with the loop; a panic is exit code 3.
    if (session_.started() && session_.host().panicked()) return exit_script_panic;
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace

int run_game(const GameOptions& options) {
    auto loop = std::make_unique<GameLoop>(options);
    if (!loop->init()) {
        loop->finish();
        return loop->init_exit_code();
    }
    return platform::run_main_loop(std::move(loop), options.vsync);
}

} // namespace cuervo::script

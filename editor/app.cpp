#include "editor/file_import.hpp"
#include "editor/app.hpp"
#include "editor/license_view.hpp"
#include "editor/launcher.hpp"
#include "cuervo/platform/license.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "cuervo/core/log.hpp"
#include "cuervo/core/profiler.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/input/replay.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/render/font_import.hpp"
#include "editor/cli_run.hpp"
#include "editor/command_script.hpp"
#include "editor/scene_painter.hpp"
#include "editor/imgui_theme.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

namespace {

double now_seconds() {
    return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
}

// Draw-list callback (ImGuiRenderer runs it in order): the scene view's
// rectangle travels as callback data.
struct SceneViewCallback {
    EditorApp* app;
    rhi::Rect rect;
};

void scene_view_callback(const ImDrawList*, const ImDrawCmd* cmd) {
    const auto* data = static_cast<const SceneViewCallback*>(cmd->UserCallbackData);
    data->app->render_scene_view(data->rect);
}

} // namespace

// ------------------------------------------------------------ PanelRegistry

Panel& PanelRegistry::add(std::unique_ptr<Panel> panel) {
    panels_.push_back(std::move(panel));
    return *panels_.back();
}

Panel* PanelRegistry::find(std::string_view title) const {
    for (const auto& p : panels_) {
        if (title == p->title()) return p.get();
    }
    return nullptr;
}

std::vector<Panel*> PanelRegistry::in(Dock dock) const {
    std::vector<Panel*> out;
    for (const auto& p : panels_) {
        if (p->dock() == dock && p->visible) out.push_back(p.get());
    }
    return out;
}

// ------------------------------------------------------------ app

EditorApp::EditorApp(const EditorOptions& options) : options_(options) {
    ctl_.console().install();
    ctl_.set_play_setup({options_.font_path, options_.jobs,
                         options_.audio && !platform::env_is("SDL_VIDEO_DRIVER", "offscreen") && !platform::env_is("CUERVO_AUDIO", "null")});
    ctl_.hooks.scene_opened = [this] { frame_scene(); };
    ctl_.hooks.persist = [this] { save_user_state(); };
    ctl_.hooks.scripts_reloaded = [this] {
        preview_revision_ = ~std::uint64_t{0}; // rebuild the scene view
        ctl_.invalidate_ready_preview();       // and restart the ready() preview on the new scripts
        rebuild_actions();
    };
    if (options_.view) {
        camera_.center = {options_.view->x(), options_.view->y()};
        camera_.zoom = std::clamp(options_.view->z(), ViewCamera::min_zoom, ViewCamera::max_zoom);
        frame_request_ = false;
    }
    panels_.add(make_scene_tree_panel());
    panels_.add(make_scene_view_panel());
    panels_.add(make_inspector_panel());
    panels_.add(make_debugger_panel());
    panels_.add(make_console_panel());
    panels_.add(make_asset_panel());
    panels_.add(make_replay_panel());
    panels_.add(make_input_map_panel());
    panels_.add(make_tier_panel());
    panels_.add(make_profiler_panel());
    panels_.add(make_history_panel());
    panels_.add(make_animation_panel());
    panels_.add(make_sprites_panel());
    panels_.add(make_particles_panel());
    for (const std::string& t : options_.tabs) {
        if (Panel* p = panels_.find(t)) {
            p->select = true;
        } else {
            LOG_WARN("--tabs: no panel '{}'", t);
        }
    }
}

EditorApp::~EditorApp() {
    shutdown();
    ctl_.console().uninstall();
}

bool EditorApp::start(std::string& error) {
    const bool headless = !options_.script_commands.empty() && options_.screenshot.empty();
    // The game font: UI layout measures text with it (UI.measure) and the
    // scene view draws text with it, so it loads before the scripts compile.
    if (!headless) {
        font_atlas_ = render::load_font_file(options_.font_path);
        if (!font_atlas_) LOG_WARN("No font ('{}'): scene text is not drawn", options_.font_path);
        ctl_.set_font_atlas(font_atlas_ ? &*font_atlas_ : nullptr);
    }
    if (!ctl_.env().open(options_.project, font_atlas_ ? &*font_atlas_ : nullptr, error)) return false;
    ctl_.doc().set_registry(&ctl_.env().classes());
    LOG_INFO("Project '{}': {} node classes", ctl_.env().project().name, ctl_.env().classes().classes().size());
    ctl_.assets().open(ctl_.env().project_dir());
    if (!ctl_.input_map().load(ctl_.env().project_dir(), error)) return false;
    restore_user_state();
    open_initial_scene();
    rebuild_actions();
    if (options_.tool) ctl_.set_tool(*options_.tool);
    if (!options_.select_paths.empty()) {
        select_paths(ctl_, options_.select_paths);
    }
    if (options_.palette) palette_request_ = true, palette_query_ = *options_.palette;
    return true;
}

void EditorApp::open_initial_scene() {
    // 1. Explicit CLI argument --scene <path>
    if (!options_.scene.empty()) {
        std::string error;
        const fs::path path = ctl_.env().resolve(options_.scene);
        if (ctl_.doc().open(path, error)) {
            LOG_INFO("Opened '{}'", ctl_.env().relative(path));
            ctl_.settings().set_last_scene(ctl_.env().project_dir(), ctl_.env().relative(path));
            save_user_state();
            return;
        }
        LOG_ERROR("{}", error);
        report_failure(error);
        ctl_.doc().new_scene();
        return;
    }

    // 2. Remembered per-project last scene
    const std::string remembered = ctl_.settings().last_scene(ctl_.env().project_dir());
    if (!remembered.empty()) {
        const fs::path path = ctl_.env().resolve(remembered);
        std::error_code ec;
        if (fs::exists(path, ec)) {
            std::string error;
            if (ctl_.doc().open(path, error)) {
                LOG_INFO("Opened '{}'", ctl_.env().relative(path));
                return;
            }
        }
    }

    // 3. project.toml main_scene
    if (!ctl_.env().project().main_scene.empty()) {
        const fs::path path = ctl_.env().resolve(ctl_.env().project().main_scene);
        std::error_code ec;
        if (fs::exists(path, ec)) {
            std::string error;
            if (ctl_.doc().open(path, error)) {
                LOG_INFO("Opened '{}'", ctl_.env().relative(path));
                ctl_.settings().set_last_scene(ctl_.env().project_dir(), ctl_.env().relative(path));
                save_user_state();
                return;
            }
        }
    }

    // 4. Convention scenes: scenes/main.scene, scenes/level.scene, main.scene
    const std::string conventions[] = {"scenes/main.scene", "scenes/level.scene", "main.scene"};
    for (const auto& candidate : conventions) {
        const fs::path path = ctl_.env().resolve(candidate);
        std::error_code ec;
        if (fs::exists(path, ec)) {
            std::string error;
            if (ctl_.doc().open(path, error)) {
                LOG_INFO("Opened '{}'", ctl_.env().relative(path));
                ctl_.settings().set_last_scene(ctl_.env().project_dir(), ctl_.env().relative(path));
                save_user_state();
                return;
            }
        }
    }

    // 5. First .scene found in scenes/ directory or project root
    std::vector<fs::path> candidate_scenes;
    std::error_code ec;
    const fs::path scenes_dir = ctl_.env().project_dir() / "scenes";
    if (fs::exists(scenes_dir, ec) && fs::is_directory(scenes_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(scenes_dir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".scene") {
                candidate_scenes.push_back(entry.path());
            }
        }
    }
    if (candidate_scenes.empty()) {
        for (const auto& entry : fs::directory_iterator(ctl_.env().project_dir(), ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".scene") {
                candidate_scenes.push_back(entry.path());
            }
        }
    }
    std::sort(candidate_scenes.begin(), candidate_scenes.end());
    for (const auto& p : candidate_scenes) {
        std::string error;
        if (ctl_.doc().open(p, error)) {
            LOG_INFO("Opened '{}'", ctl_.env().relative(p));
            ctl_.settings().set_last_scene(ctl_.env().project_dir(), ctl_.env().relative(p));
            save_user_state();
            return;
        }
    }

    // 6. Fallback: blank scene
    ctl_.doc().new_scene();
}

int EditorApp::run_script() {
    return run_script_file(ctl_, options_.script_commands, options_.font_path, options_.jobs, &ui_steps_);
}

int EditorApp::run_replay_verify() {
    return run_replay_verify_file(options_.project, options_.replay_verify, options_.font_path, options_.jobs);
}

namespace {

void (*g_backend_create_window)(ImGuiViewport*) = nullptr;

void create_window_keep_context(ImGuiViewport* viewport) {
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    SDL_GLContext context = SDL_GL_GetCurrentContext();
    g_backend_create_window(viewport);
    SDL_GL_MakeCurrent(window, context);
}

} // namespace

bool EditorApp::open_window(std::string& error) {
    auto try_open = [&](rhi::Backend backend) -> bool {
        if (!window_.create({.title = "Cuervo Engine Editor",
                             .width = options_.width,
                             .height = options_.height,
                             .graphics = rhi::graphics_api(backend),
                             .gl_es = options_.gl_es})) {
            return false;
        }
        rhi::DeviceOptions o;
        o.vsync = options_.vsync;
        // The editor shows what the game will look like: gated to its tier.
        o.tier = ctl_.env().project().render_tier;
        device_ = rhi::create_device(backend, window_, o);
        if (!device_) window_.destroy();
        return device_ != nullptr;
    };
    if (!try_open(options_.backend)) {
        if (!(options_.allow_fallback && options_.backend == rhi::Backend::Vulkan && try_open(rhi::Backend::OpenGL))) {
            error = "could not open a window and render device";
            return false;
        }
        LOG_WARN("Vulkan unavailable, falling back to OpenGL");
    }
    LOG_INFO("Editor renders with {} ({})", rhi::backend_name(device_->info().backend), device_->info().renderer);
    if (!shapes_.init(*device_, 4096) || !lights_.init(*device_) || !text_.init(*device_) || !meshes_.init(*device_) ||
        !sprite_batcher_.init(*device_)) {
        error = "could not create the scene renderers";
        return false;
    }
    sprite_library_.init(device_.get());
    if (font_atlas_) font_ = text_.add_font(*font_atlas_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Docking (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): panels dock, float and redock inside
    // the one window. ImGui's own imgui.ini is off; saved layouts keep the dock text per user.
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingTransparentPayload = true;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // The brand theme and fonts (DESIGN.md 4.16): --theme, else the OS's preference.
    theme::apply(options_.theme.value_or(theme::system_theme()));
    theme::load_fonts(options_.font_path);
    // The splash, unless asked not to; screenshots show it only with --splash.
    const bool want_splash = options_.splash == SplashMode::On ||
                             (options_.splash == SplashMode::Auto && options_.screenshot.empty() && ui_steps_.empty());
    if (want_splash) {
        const std::string brand_dir = platform::utf8(platform::find_asset("brand", CUERVO_BRAND_DIR));
        const std::string display_font = platform::utf8(platform::find_asset("fonts/Outfit-Medium.ttf", CUERVO_FONT_DISPLAY));
        if (splash_.init(*device_, text_, brand_dir, display_font)) {
            splash_state_ = options_.splash == SplashMode::On ? SplashState::Held : SplashState::Timed;
        } else {
            LOG_WARN("Splash: the brand mark could not be loaded from '{}'", brand_dir);
        }
    }
    // Undockable panels (DESIGN.md 4.13, 11.7b): ImGui's multi-viewport needs one RHI surface per
    // OS window, which only devices with Feature::MultiSurface have. The SDL3 backend must know
    // the graphics API so the windows it creates can present (Vulkan / OpenGL flags).
    const rhi::Backend backend = device_->info().backend;
    const bool multi = device_->info().supports(rhi::Feature::MultiSurface) && !options_.no_viewports;
    bool sdl_ok = false;
    // The SDL3 backend offers platform viewports only where it can read the global mouse state
    // (X11, Windows, macOS); Wayland compositors don't let an app place its windows, so panels
    // stay inside the editor window there. The offscreen driver (tests) is allowed: it has no
    // compositor and honors window positions.
    const char* video_driver = SDL_GetCurrentVideoDriver();
    if (multi && video_driver != nullptr && std::string_view(video_driver) == "offscreen") {
        io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
    }
    // (Set before the backend starts: it installs its window callbacks only when the flag is on.)
    if (multi && backend == rhi::Backend::Vulkan) sdl_ok = ImGui_ImplSDL3_InitForVulkan(window_.native());
    else if (multi && backend == rhi::Backend::OpenGL) sdl_ok = ImGui_ImplSDL3_InitForOpenGL(window_.native(), SDL_GL_GetCurrentContext());
    else sdl_ok = ImGui_ImplSDL3_InitForOther(window_.native());
    if (!sdl_ok) {
        error = "ImGui SDL3 backend failed to start";
        return false;
    }
    imgui_ready_ = true;
    if (!imgui_renderer_.init(*device_)) {
        error = "ImGui renderer failed to start";
        return false;
    }
    const bool platform_ok = (io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) != 0;
    if (multi && platform_ok && imgui_renderer_.enable_viewports()) {
        viewports_on_ = true;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        // The editor draws each OS window through the RHI surface (imgui_rhi.cpp); the SDL3
        // backend's own GL make-current / swap would fight the device's contexts.
        ImGui::GetPlatformIO().Platform_RenderWindow = nullptr;
        ImGui::GetPlatformIO().Platform_SwapBuffers = nullptr;
        if (backend == rhi::Backend::OpenGL) {
            // The backend makes the new window's context current when it is done; the device
            // expects its own back.
            g_backend_create_window = ImGui::GetPlatformIO().Platform_CreateWindow;
            ImGui::GetPlatformIO().Platform_CreateWindow = create_window_keep_context;
        }
    } else {
        LOG_INFO("Panels stay inside the editor window ({})",
                 options_.no_viewports                                   ? "--no-viewports"
                 : !device_->info().supports(rhi::Feature::MultiSurface) ? "this device has no multi-window support"
                                                                         : "this window system cannot place app windows");
    }
    return true;
}

void EditorApp::shutdown() {
    if (imgui_ready_) save_user_state();
    if (imgui_ready_) {
        // Surfaces go before their windows, and before the renderer that owns the pipeline.
        if (viewports_on_) ImGui::DestroyPlatformWindows();
        imgui_renderer_.shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    splash_.shutdown();
    text_.shutdown();
    sprite_library_.shutdown();
    sprite_batcher_.shutdown();
    meshes_.shutdown();
    lights_.shutdown();
    shapes_.shutdown();
    device_.reset();
    window_.destroy();
}

int EditorApp::run() {
    std::string error;
    if (!options_.replay_verify.empty()) return run_replay_verify(); // headless, no scene
    if (!start(error)) {
        LOG_ERROR("{}", error);
        return EXIT_FAILURE;
    }
    if (!options_.script_commands.empty()) {
        const int code = run_script();
        if (code != EXIT_SUCCESS || (options_.screenshot.empty() && ui_steps_.empty())) return code;
    }
    if (!open_window(error)) {
        LOG_ERROR("{}", error);
        return EXIT_FAILURE;
    }
    const double start_time = now_seconds();
    while (running_ && !ctl_.switch_requested()) {
        frame();
        if (options_.quit_after > 0.0 && now_seconds() - start_time >= options_.quit_after) {
            LOG_INFO("--quit-after reached ({} frames)", frames_);
            running_ = false;
        }
    }
    return ui_failed_ ? EXIT_FAILURE : EXIT_SUCCESS;
}

void EditorApp::frame() {
    // One profiler frame per editor frame (the Profiler panel's graph).
    profiler::frame_mark();
    CUERVO_PROFILE_ZONE("editor frame");
    const double now = now_seconds();
    const double seconds = last_time_ > 0.0 ? std::min(now - last_time_, 0.25) : 0.0;
    last_time_ = now;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // A click or key press dismisses the splash and goes no further.
        if (splash_state_ != SplashState::Off &&
            (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_KEY_DOWN)) {
            splash_state_ = SplashState::Off;
            continue;
        }
        // Files dragged in from the OS: collected until the drop completes, then imported.
        if (event.type == SDL_EVENT_DROP_BEGIN) dropped_.clear();
        if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
            dropped_.push_back(platform::path_from_utf8(event.drop.data));
        }
        if (event.type == SDL_EVENT_DROP_COMPLETE && !dropped_.empty()) {
            import_dropped_files(std::exchange(dropped_, {}));
        }
        ImGui_ImplSDL3_ProcessEvent(&event);
        // The same events drive the game's devices (play-in-editor) and the
        // input map's "press a key" capture.
        window_.handle_event(event, devices_);
        // A secondary window's close request belongs to ImGui (it docks the panel back or hides it).
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_.native()))) {
            running_ = false;
        }
    }
    if (now - last_poll_ >= 1.0) {
        last_poll_ = now;
        poll_assets();
    }
    // The scene view shows the document as of this frame's start.
    if (preview_revision_ != ctl_.doc().revision()) {
        preview_revision_ = ctl_.doc().revision();
        ctl_.env().rebuild_preview(ctl_.doc().model().to_doc());
    }
    ctl_.update_ready_preview();
    // The timeline's scrub poses the fresh preview (every frame: cheap, and
    // it survives the rebuilds edits cause).
    if (anim_preview_.on && !ctl_.play().active()) {
        const int index = ctl_.doc().model().doc_index(anim_preview_.player);
        std::string why;
        if (index < 0 || !ctl_.env().preview_animation(static_cast<std::size_t>(index), anim_preview_.clip,
                                                 static_cast<float>(anim_preview_.tick), why)) {
            anim_preview_.on = false;
            refresh_preview();
        }
    }
    update_title();

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    clock_ = now;
    ctl_.set_clock(now);
    handle_shortcuts();
    draw_menu_bar();
    draw_docks();
    apply_prompt();
    draw_dialogs();
    draw_toolbar();
    draw_palette();
    draw_messages();
    // Continuous edits end when nothing is held: the next edit is a new
    // undo step (backs up widgets whose release isn't seen, e.g. pickers).
    if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ctl_.doc().seal();
    // The game ticks after the UI (which may have paused or stepped it), so
    // this frame draws the latest tick. Screenshots freeze it where a
    // command script left it.
    if (options_.screenshot.empty()) {
        CUERVO_PROFILE_ZONE("play");
        update_play(seconds);
    }
    ImGui::Render();
    if (viewports_on_) ImGui::UpdatePlatformWindows(); // creates / moves / closes the OS windows

    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window_.native(), &w, &h);
    device_->begin_frame({w, h});
    device_->clear(theme::colors().bg);
    imgui_renderer_.render(ImGui::GetDrawData());
    render_splash({w, h}, now);
    ++frames_;
    const bool capture = !options_.screenshot.empty() && frames_ >= static_cast<std::uint64_t>(options_.screenshot_frame);
    if (capture) device_->capture_frame(options_.screenshot);
    device_->end_frame();
    // The undocked panels' windows, after the main frame is presented (frames never overlap).
    if (viewports_on_) ImGui::RenderPlatformWindowsDefault();
    run_ui_steps();
    if (capture) {
        LOG_INFO("Screenshot written to '{}' ({} frames)", options_.screenshot, frames_);
        running_ = false;
    }
}

// Window steps of a command script (undock, save-layout, expect-viewports): one every few frames
// so ImGui has made the OS window (or dropped it) before the next step looks.
void EditorApp::run_ui_steps() {
    if (ui_steps_.empty() || frames_ < ui_step_wait_ || frames_ < 8) return;
    if (ui_step_next_ >= ui_steps_.size()) {
        running_ = false;
        return;
    }
    const UiStep& step = ui_steps_[ui_step_next_++];
    ui_step_wait_ = frames_ + 4;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("--script-commands: {}", why);
        ui_failed_ = true;
        running_ = false;
    };
    switch (step.kind) {
    case UiStep::Kind::Undock:
        if (!viewports_on_) return fail("undock: this device cannot open a second window (see the log)");
        if (panels_.find(step.name) == nullptr) return fail(std::format("undock: there is no panel '{}'", step.name));
        undock_requests_[step.name] = {step.x, step.y};
        break;
    case UiStep::Kind::SaveLayout: {
        std::string error;
        if (!layouts_.save_as(step.name, current_layout(), error)) return fail("save-layout: " + error);
        if (!layouts_.save(LayoutStore::user_file(), error)) return fail("save-layout: " + error);
        LOG_INFO("Layout '{}' saved ({} undocked window(s))", step.name, dock_viewports(current_layout().dock).size());
        break;
    }
    case UiStep::Kind::ExpectViewport: {
        const ImGuiWindow* w = ImGui::FindWindowByName(step.name.c_str());
        if (w == nullptr || w->Viewport == nullptr || w->Viewport == ImGui::GetMainViewport()) {
            return fail(std::format("expect-viewport: '{}' is not in a window of its own", step.name));
        }
        if (std::abs(w->Pos.x - step.x) > 1.0f || std::abs(w->Pos.y - step.y) > 1.0f) {
            return fail(std::format("expect-viewport: '{}' is at {},{}, expected {},{}", step.name, w->Pos.x, w->Pos.y,
                                    step.x, step.y));
        }
        LOG_INFO("expect-viewport {}: ok", step.name);
        break;
    }
    case UiStep::Kind::ExpectViewports: {
        const int have = viewports_on_ ? ImGui::GetPlatformIO().Viewports.Size - 1 : 0;
        if (have != step.count) return fail(std::format("expect-viewports: expected {} extra window(s), got {}", step.count, have));
        LOG_INFO("expect-viewports {}: ok", have);
        break;
    }
    }
}

void EditorApp::render_splash(ivec2 size, double now) {
    if (splash_state_ == SplashState::Off) return;
    // Held (--splash) until dismissed; otherwise shown, then faded out.
    constexpr double hold = 1.4;
    constexpr double fade = 0.45;
    if (splash_start_ <= 0.0) splash_start_ = now;
    float opacity = 1.0f;
    if (splash_state_ == SplashState::Timed) {
        const double t = now - splash_start_;
        if (t >= hold + fade) {
            splash_state_ = SplashState::Off;
            return;
        }
        if (t > hold) opacity = static_cast<float>(1.0 - (t - hold) / fade);
    }
    splash_.render(*device_, shapes_, text_, size, theme::current(), opacity,
                   std::format("Engine Editor  ·  {}", ctl_.env().project().name));
}

void EditorApp::update_play(double seconds) {
    if (ctl_.play().active()) {
        // The game reads the devices only while the view has the input focus.
        static DeviceState none;
        none = DeviceState{};
        DeviceState& source = game_input_ ? devices_ : none;
        if (game_input_) last_pointer_ = game_pointer();
        ctl_.play().update(seconds, source, last_pointer_);
        if (ctl_.play().panic() && !panic_seen_) {
            panic_seen_ = true;
            if (Panel* p = panels_.find("Debugger")) p->select = true;
        }
        if (!ctl_.play().panic()) panic_seen_ = false;
    }
    // Taps the game didn't consume this frame (no tick ran, or the game
    // doesn't have the input) don't linger into later ticks.
    if (!(ctl_.play().active() && game_input_)) devices_.clear_transients();
}

vec2 EditorApp::game_pointer() const {
    if (game_rect_.width <= 0 || game_rect_.height <= 0) return last_pointer_;
    const render::ViewportFit fit = render::fit_viewport(render::ScaleMode::Fit, ctl_.env().project().virtual_size,
                                                         {game_rect_.width, game_rect_.height});
    const vec2 local = devices_.mouse_position - vec2{static_cast<float>(game_rect_.x), static_cast<float>(game_rect_.y)};
    return render::framebuffer_to_virtual(fit, local);
}

void EditorApp::set_game_view(rhi::Rect rect, bool has_input) {
    game_rect_ = rect;
    game_input_ = has_input && ctl_.play().active();
}

script::SessionOptions EditorApp::play_options(bool current_scene) const {
    return ctl_.play_options(current_scene);
}

void EditorApp::start_play(bool current_scene, bool record) {
    if (!ctl_.start_play(current_scene, record)) return;
    panic_seen_ = false;
    if (Panel* p = panels_.find("Scene View")) p->select = true;
}

void EditorApp::stop_play() { ctl_.play().stop(); }

void EditorApp::start_replay(const std::string& path) {
    if (!ctl_.start_replay(path)) return;
    if (Panel* p = panels_.find("Replay")) p->select = true;
}

void EditorApp::import_dropped_files(const std::vector<std::filesystem::path>& files) {
    const EditorController::ImportSummary sum = ctl_.import_files(files);
    if (!sum.first_image.empty()) show_sprite_asset(sum.first_image);
}

void EditorApp::show_sprite_asset(std::string path) {
    sprite_request_ = std::move(path);
    if (Panel* p = panels_.find("Sprites")) {
        p->visible = true;
        p->select = true;
    }
}

render::SpriteRef EditorApp::sprite_preview(std::string_view asset, std::string_view frame) {
    if (!sprite_vfs_ || sprite_vfs_dir_ != ctl_.env().project_dir()) {
        sprite_vfs_ = assets::Vfs::dev(ctl_.env().project_dir());
        sprite_vfs_dir_ = ctl_.env().project_dir();
    }
    return sprite_library_.resolve(*sprite_vfs_, asset, frame, render::SpriteFilter::Nearest, 1.0f);
}

void EditorApp::poll_assets() {
    std::vector<std::string> changed;
    if (!ctl_.assets().poll(&changed)) return;
    bool scripts = false;
    for (const std::string& c : changed) {
        if (c.ends_with(".plm")) scripts = true;
        // An image, atlas or SVG changed on disk: the library reloads it on next use.
        if (c.ends_with(".png") || c.ends_with(".jpg") || c.ends_with(".qoi") || c.ends_with(".svg") ||
            c.ends_with(".atlas") || c.ends_with(".catlas")) {
            sprite_library_.shutdown();
            sprite_library_.init(device_.get());
        }
        if (ctl_.doc().has_path() && c == ctl_.env().relative(ctl_.doc().path()) && std::filesystem::exists(ctl_.doc().path())) {
            LOG_WARN("'{}' changed on disk{}", c, ctl_.doc().dirty() ? " (the editor has unsaved changes to it)" : "");
        }
        if (c == "project.toml" && !ctl_.input_map().dirty()) {
            std::string error;
            if (!ctl_.input_map().load(ctl_.env().project_dir(), error)) LOG_WARN("{}", error);
        }
    }
    // Playing: the session hot-reloads by itself (PlayController::update).
    if (scripts && !ctl_.play().active()) reload_project_scripts();
}

void EditorApp::open_location(const std::string& file, std::uint32_t line) {
    if (file.ends_with(".scene")) {
        request_open_scene(file);
        return;
    }
    reveal_asset(file);
    if (Panel* p = panels_.find("Assets")) p->select = true;
    const std::string where = line > 0 ? std::format("{}:{}", file, line) : file;
    SDL_SetClipboardText(platform::utf8(ctl_.env().resolve(file)).c_str());
    if (file.ends_with(".plm") || file.ends_with(".txt") || file.ends_with(".toml") || file.ends_with(".json")) {
        open_in_external_editor(ctl_.env().resolve(file), static_cast<int>(line));
    }
    LOG_INFO("{} (path copied to the clipboard)", where);
}

void EditorApp::update_title() {
    std::string title = std::format("{}{} \u2014 {} \u2014 Cuervo Engine Editor", ctl_.doc().dirty() ? "*" : "",
                                    ctl_.doc().has_path() ? ctl_.env().relative(ctl_.doc().path()) : std::string("(new scene)"),
                                    ctl_.env().project().name);
    if (title != title_) {
        title_ = title;
        SDL_SetWindowTitle(window_.native(), title_.c_str());
    }
}

// Maps the controller's pending question onto this UI's dialogs.
void EditorApp::apply_prompt() {
    const PendingPrompt p = ctl_.take_prompt();
    switch (p.kind) {
        case Prompt::None: return;
        case Prompt::SaveAs: dialog_ = Dialog::SaveAs; break;
        case Prompt::DiscardOpen:
        case Prompt::DiscardLeave: dialog_ = Dialog::DiscardChanges; break;
        case Prompt::AddChild:
            add_parent_ = p.node;
            class_filter_.clear();
            dialog_ = Dialog::AddChild;
            break;
        case Prompt::AttachScript: {
            attach_node_ = p.node;
            dialog_ = Dialog::AttachScript;
            attach_tab_ = 0;
            attach_filter_.clear();
            attach_error_.clear();
            if (const ModelNode* n = ctl_.doc().model().get(p.node)) {
                std::tie(new_script_class_, new_script_path_) = EditorController::suggest_script(n->name);
            }
            break;
        }
    }
    dialog_opened_ = false;
}

void EditorApp::draw_dialogs() {
    const char* names[] = {"",           "New Scene",         "Open Scene", "Save Scene As",
                           "Add Child Node", "Unsaved Changes", "License", "Save Layout", "Attach Script"};
    if (dialog_ == Dialog::None) return;
    const char* name = names[static_cast<int>(dialog_)];
    if (!dialog_opened_) {
        ImGui::OpenPopup(name);
        dialog_opened_ = true;
        if (dialog_ == Dialog::Open || dialog_ == Dialog::SaveAs) {
            path_input_ = ctl_.doc().has_path() ? ctl_.env().relative(ctl_.doc().path()) : std::string("scenes/new.scene");
        }
    }
    if (dialog_ == Dialog::License) {
        const ImVec2 area = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize({std::min(820.0f, area.x - 80.0f), std::min(680.0f, area.y - 80.0f)}, ImGuiCond_Appearing);
    } else {
        ImGui::SetNextWindowSize({520.0f, 0.0f}, ImGuiCond_Appearing);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    bool open = true;
    if (!ImGui::BeginPopupModal(name, &open)) {
        dialog_ = Dialog::None;
        return;
    }
    auto close = [&] {
        ImGui::CloseCurrentPopup();
        dialog_ = Dialog::None;
    };
    const auto& classes = ctl_.env().classes().classes();
    if (dialog_ == Dialog::License) {
        draw_license_dialog(license_view_);
        if (ImGui::Button("Close")) close();
    } else if (dialog_ == Dialog::SaveLayout) {
        ImGui::TextUnformatted("Name for the current panel arrangement:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##layout_name", &layout_name_input_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Docked, floating and hidden panels are saved with it.");
        ImGui::BeginDisabled(layout_name_input_.empty());
        theme::push_primary_button();
        const bool go = ImGui::Button("Save") || enter;
        theme::pop_primary_button();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) close();
        if (go && !layout_name_input_.empty()) {
            save_layout_as(layout_name_input_);
            layout_name_input_.clear();
            close();
        }
    } else if (dialog_ == Dialog::DiscardChanges) {
        const bool leave = ctl_.discard_is_leave();
        if (leave) {
            ImGui::TextWrapped("The scene has unsaved changes. Leave this project and discard them?");
        } else {
            ImGui::TextWrapped("The scene has unsaved changes. Open '%s' and discard them?", ctl_.discard_path().c_str());
        }
        if (ImGui::Button(leave ? "Discard and Leave" : "Discard and Open")) {
            ctl_.confirm_discard();
            close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ctl_.cancel_discard();
            close();
        }
    } else if (dialog_ == Dialog::Open || dialog_ == Dialog::SaveAs) {
        ImGui::TextUnformatted("Path (relative to the project):");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##path", &path_input_, ImGuiInputTextFlags_EnterReturnsTrue);
        // The project's scenes, to pick from.
        if (ImGui::BeginListBox("##scenes", {-1.0f, 150.0f})) {
            std::error_code ec;
            std::vector<std::string> found;
            for (fs::recursive_directory_iterator it(ctl_.env().project_dir(), ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && it->path().extension() == ".scene") found.push_back(ctl_.env().relative(it->path()));
            }
            std::sort(found.begin(), found.end());
            for (const std::string& f : found) {
                if (ImGui::Selectable(f.c_str(), f == path_input_)) path_input_ = f;
            }
            ImGui::EndListBox();
        }
        const bool go = ImGui::Button(dialog_ == Dialog::Open ? "Open" : "Save") || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) close();
        if (go && !path_input_.empty()) {
            std::string error;
            const fs::path path = ctl_.env().resolve(path_input_);
            const bool ok = dialog_ == Dialog::Open ? ctl_.doc().open(path, error) : ctl_.doc().save_as(path, error);
            if (ok) {
                LOG_INFO("{} '{}'", dialog_ == Dialog::Open ? "Opened" : "Saved", ctl_.env().relative(path));
                ctl_.assets().poll();
                if (dialog_ == Dialog::Open) frame_scene();
                ctl_.settings().set_last_scene(ctl_.env().project_dir(), ctl_.env().relative(path));
                save_user_state();
                close();
            } else {
                LOG_ERROR("{}", error);
                notify(dialog_ == Dialog::Open ? message_open_failed(path_input_, error, true) : message_save_failed(path_input_, error));
            }
        }
    } else if (dialog_ == Dialog::NewScene) {
        ImGui::TextUnformatted("Root node class:");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##root", new_root_class_.c_str())) {
            for (const ClassSpec& c : classes) {
                if (ImGui::Selectable(c.name.c_str(), c.name == new_root_class_)) new_root_class_ = c.name;
            }
            ImGui::EndCombo();
        }
        if (ctl_.doc().dirty()) {
            ImGui::TextColored(theme::im(theme::colors().warning), "Unsaved changes to the current scene will be lost.");
        }
        if (ImGui::Button("Create")) {
            ctl_.doc().new_scene(new_root_class_, "root");
            frame_scene();
            close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) close();
    } else if (dialog_ == Dialog::AddChild) {
        const ModelNode* parent = ctl_.doc().model().get(add_parent_ != 0 ? add_parent_ : ctl_.doc().model().root());
        ImGui::Text("Add a child to '%s':", parent != nullptr ? parent->name.c_str() : "?");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputTextWithHint("##filter", "filter classes", &class_filter_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        std::string chosen;
        std::string first_match;
        if (ImGui::BeginListBox("##classes", {-1.0f, 260.0f})) {
            for (const ClassSpec& c : classes) {
                std::string lower = c.name;
                std::string filter = class_filter_;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
                std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) { return std::tolower(ch); });
                if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
                if (first_match.empty()) first_match = c.name;
                const std::string label = c.base.empty() ? c.name : std::format("{}  ({})", c.name, c.base);
                if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    chosen = c.name;
                }
            }
            ImGui::EndListBox();
        }
        if (enter && !first_match.empty()) chosen = first_match;
        ImGui::TextDisabled("Double-click a class, or Enter for the first match.");
        if (ImGui::Button("Cancel")) close();
        if (!chosen.empty() && parent != nullptr) {
            // A positioned node appears in the middle of the scene view.
            std::vector<scene::Field> fields;
            const ClassRegistry& reg = ctl_.env().classes();
            if (reg.is_a(chosen, "Node2D") && !reg.is_a(chosen, "Control")) {
                fields.push_back({"position", "vec2",
                                  scene::Value::vector({std::round(camera_.center.x()), std::round(camera_.center.y())})});
            }
            report(ctl_.doc().add_child(parent->id, chosen, {}, std::move(fields)) != 0);
            close();
        }
    } else if (dialog_ == Dialog::AttachScript) {
        const ModelNode* n = ctl_.doc().model().get(attach_node_);
        if (n == nullptr) {
            close();
            return;
        }
        ImGui::Text("Attach Script to '%s' (%s):", n->name.c_str(), n->type.c_str());
        ImGui::Separator();

        if (ImGui::RadioButton("Select Existing Script", attach_tab_ == 0)) attach_tab_ = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Create New Script", attach_tab_ == 1)) attach_tab_ = 1;
        ImGui::Spacing();

        if (attach_tab_ == 0) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##filter", "filter scripts / classes", &attach_filter_);
            const auto script_classes = project_script_classes();
            std::string chosen;
            if (ImGui::BeginListBox("##script_classes", {-1.0f, 180.0f})) {
                for (const auto& [cls_name, script_path] : script_classes) {
                    std::string filter = attach_filter_;
                    std::string label = std::format("{}  ({})", cls_name, script_path);
                    std::string lower = label;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
                    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char ch) { return std::tolower(ch); });
                    if (!filter.empty() && lower.find(filter) == std::string::npos) continue;

                    const bool selected = (n->type == cls_name);
                    if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        chosen = cls_name;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            if (ctl_.doc().change_type(attach_node_, chosen)) {
                                notify_ok(std::format("Bound '{}' to '{}'.", chosen, n->name));
                                close();
                            }
                        }
                    }
                }
                ImGui::EndListBox();
            }
            if (!attach_error_.empty()) {
                ImGui::TextColored(theme::im(theme::colors().danger), "%s", attach_error_.c_str());
            }
            ImGui::BeginDisabled(chosen.empty());
            theme::push_primary_button();
            if (ImGui::Button("Attach") && !chosen.empty()) {
                if (ctl_.doc().change_type(attach_node_, chosen)) {
                    notify_ok(std::format("Bound '{}' to '{}'.", chosen, n->name));
                    close();
                } else {
                    attach_error_ = ctl_.doc().error();
                }
            }
            theme::pop_primary_button();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) close();
        } else {
            ImGui::TextUnformatted("Class Name:");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##class_name", &new_script_class_);

            ImGui::TextUnformatted("Script File Path (project-relative):");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##file_path", &new_script_path_);

            ImGui::TextDisabled("Inherits base: %s", n->type.c_str());
            if (!attach_error_.empty()) {
                ImGui::TextColored(theme::im(theme::colors().danger), "%s", attach_error_.c_str());
            }
            ImGui::Spacing();
            theme::push_primary_button();
            if (ImGui::Button("Create and Attach")) {
                std::string err;
                if (create_and_attach_script(attach_node_, new_script_class_, new_script_path_, err)) {
                    close();
                } else {
                    attach_error_ = err;
                }
            }
            theme::pop_primary_button();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) close();
        }
    }
    ImGui::EndPopup();
    if (!open) {
        dialog_ = Dialog::None;
        ctl_.cancel_discard();
    }
}

// ------------------------------------------------------------ scene view

void EditorApp::render_game_view(rhi::Rect view) {
    script::GameSession* session = ctl_.play().session();
    if (session == nullptr) {
        device_->set_viewport(view);
        device_->set_scissor(view);
        device_->clear({0.0f, 0.0f, 0.0f, 1.0f});
        return;
    }
    paint_game_view({device_.get(), &shapes_, &lights_, &text_, &meshes_, &vectors_, &sprite_batcher_, &sprite_library_, font_}, *session, view);
}

void EditorApp::render_scene_view(rhi::Rect view) {
    if (view.width <= 0 || view.height <= 0) return;
    if (ctl_.play().active()) {
        render_game_view(view);
        return;
    }
    // The painter is shared with the RmlUi shell (scene_painter.hpp); the ImGui panel draws its own overlay.
    ScenePaintParams p;
    p.virtual_size = ctl_.env().virtual_size();
    p.clear_color = ctl_.env().project().clear_color;
    p.camera = camera_;
    p.colors = &theme::colors();
    p.collision_shapes = ctl_.collision_shapes();
    p.lighting = ctl_.preview_lighting();
    // With the ready() preview on and started, that session is drawn (the edit-time one still serves picking).
    if (ctl_.ready_preview_on() && ctl_.ready_preview().active() && !ctl_.ready_preview().panicked()) {
        p.drawn = &ctl_.ready_preview().session()->host();
    } else if (ctl_.env().preview_ok()) {
        p.drawn = &ctl_.env().host();
    }
    paint_scene_view({device_.get(), &shapes_, &lights_, &text_, &meshes_, &vectors_, &sprite_batcher_, &sprite_library_, font_}, p,
                     view);
}

// ------------------------------------------------------------ entry

namespace {

LauncherOptions launcher_options(const EditorOptions& o) {
    LauncherOptions l;
    l.backend = o.backend;
    l.allow_fallback = o.allow_fallback;
    l.gl_es = o.gl_es;
    l.vsync = o.vsync;
    l.font_path = o.font_path;
    l.theme = o.theme;
    l.screenshot = o.screenshot;
    l.screenshot_frame = o.screenshot_frame;
    return l;
}

} // namespace

// The start sequence: the welcome screen when no project was given, then
// the editor; "Close Project" and the recent-projects menu come back through the loop.
int run_editor(const EditorOptions& options) {
    EditorOptions opts = options;
    const bool headless = (!opts.script_commands.empty() && opts.screenshot.empty()) || !opts.replay_verify.empty();
    const bool shot = !opts.screenshot.empty();
    // Screenshots of a first-launch screen: show it, exit.
    if (shot && opts.screen == "welcome") {
        LauncherOptions l = launcher_options(opts);
        l.show_welcome = true;
        const LauncherResult r = run_launcher(l);
        return r.outcome == LauncherOutcome::Failed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    bool want_welcome = (opts.project.empty() || opts.screen == "welcome") && !headless;
    if (want_welcome) {
        LauncherOptions l = launcher_options(opts);
        l.show_welcome = true;
        const LauncherResult r = run_launcher(l);
        if (r.outcome == LauncherOutcome::Failed) return EXIT_FAILURE;
        if (r.outcome == LauncherOutcome::Closed) return EXIT_SUCCESS;
        if (r.outcome == LauncherOutcome::OpenProject) opts.project = r.project;
    }
    while (true) {
        int code = EXIT_SUCCESS;
        std::string next;
        bool again = false;
        {
            EditorApp app(opts);
            code = app.run();
            again = code == EXIT_SUCCESS && app.switch_requested();
            next = app.switch_target();
        }
        if (!again) return code;
        opts.scene.clear();
        opts.script_commands.clear();
        opts.project = next;
        if (opts.project.empty()) {
            LauncherOptions l = launcher_options(opts);
            const LauncherResult r = run_launcher(l);
            if (r.outcome != LauncherOutcome::OpenProject) return EXIT_SUCCESS;
            opts.project = r.project;
        }
    }
}

void EditorApp::queue_scene_view(ImDrawList* list, rhi::Rect rect) {
    SceneViewCallback data{this, rect};
    list->AddCallback(&scene_view_callback, &data, sizeof(data)); // copied into the draw list
}

} // namespace cuervo::editor

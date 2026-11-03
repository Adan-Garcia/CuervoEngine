#include "cuervo/core/profiler.hpp"
#include "editor/rml/rml_ui.hpp"
#include "editor/frame_policy.hpp"

#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/scene/scene_file.hpp"
#include <optional>

#include "cuervo/render/font_import.hpp"
#include "editor/cli_run.hpp"
#include "editor/console_log.hpp"
#include "editor/editor_controller.hpp"
#include "editor/scene_input.hpp"
#include "editor/scene_painter.hpp"
#include "editor/editor_document.hpp"
#include "editor/icons.hpp"
#include "editor/project_env.hpp"
#include "editor/rml/rml_rhi.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml/rml_theme.hpp"
#include "editor/rml/script/code_view.hpp"
#include "editor/settings.hpp"
#include "editor/splash.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/script/game_session.hpp"
#include "editor/theme.hpp"

namespace cuervo::editor {

namespace {

namespace fs = std::filesystem;

// Schemes the renderer resolves itself (rml_rhi.cpp LoadTexture).
bool is_renderer_source(const Rml::String& s) {
    for (const char* scheme : {"file:", "pixel:", "mem:", "check:", "icon:", "grad:"}) if (s.starts_with(scheme)) return true;
    return false;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// RmlUi joins every href onto the document's directory, mangling absolute paths; the generated
// tokens file lives in the temp directory, so absolute paths pass through.
class EditorSystem final : public SystemInterface_SDL {
public:
    using SystemInterface_SDL::SystemInterface_SDL;
    void JoinPath(Rml::String& translated, const Rml::String& document_path, const Rml::String& path) override {
        if (is_renderer_source(path) || (!path.empty() && fs::path(platform::path_from_utf8(path)).is_absolute())) translated = path; // the tokens file is an absolute path (a drive letter on Windows)
        else SystemInterface_SDL::JoinPath(translated, document_path, path);
    }
};

// Sends pointer and key events to the scene region or the UI (FocusRouter decides). Returns true when
// the scene (or, in play mode, the game) consumed the event, so the UI never sees it.
struct RouteCtx {
    FocusRouter& router;
    SceneInput& scene;
    Shell& shell;
    bool& over_scene;
    bool playing;
    bool text_editing;
};

bool route_event(const SDL_Event& ev, Rml::Context* ctx, RouteCtx& rc) {
    FocusRouter& router = rc.router;
    SceneInput& scene = rc.scene;
    const SDL_Keymod mods = SDL_GetModState();
    scene.set_mods((mods & SDL_KMOD_CTRL) != 0, (mods & SDL_KMOD_SHIFT) != 0);
    switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const vec2 p{ev.motion.x, ev.motion.y};
        const bool to_scene = router.pointer(p) == Route::Scene;
        if (to_scene) {
            if (!rc.over_scene) ctx->ProcessMouseLeave(); // drop the UI's hover once the pointer enters the scene
            if (!rc.playing) scene.move(p - router.origin());
        }
        rc.over_scene = to_scene;
        return to_scene;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const vec2 p{ev.button.x, ev.button.y};
        const bool to_scene = router.press(p, ev.button.button) == Route::Scene;
        if (to_scene && !rc.playing) scene.down(ev.button.button - 1, p - router.origin(), (SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) != 0);
        return to_scene;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const vec2 p{ev.button.x, ev.button.y};
        const bool to_scene = router.pointer(p) == Route::Scene;
        router.release(ev.button.button);
        if (to_scene && !rc.playing) scene.up(ev.button.button - 1, p - router.origin());
        return to_scene;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        const vec2 p{ev.wheel.mouse_x, ev.wheel.mouse_y};
        if (router.pointer(p) != Route::Scene) return false;
        if (!rc.playing) scene.wheel(ev.wheel.y, p - router.origin());
        return true;
    }
    case SDL_EVENT_KEY_DOWN:
        if (ev.key.key == SDLK_ESCAPE && rc.shell.handle_escape()) return true;
        if (rc.shell.script_key(ev)) return true; // Script mode: Ctrl+S, Ctrl+W, Ctrl+Tab
        // Menu shortcuts work wherever the focus is, except while typing in a field.
        if (!rc.text_editing && rc.shell.handle_shortcut(ev)) return true;
        [[fallthrough]];
    case SDL_EVENT_KEY_UP:
        if (ev.type == SDL_EVENT_KEY_UP) rc.shell.script_key(ev);
        if (router.key() != Route::Scene) return false;
        if (ev.type == SDL_EVENT_KEY_DOWN && !rc.playing) {
            if (ev.key.key == SDLK_F) scene.frame_all();
            else rc.shell.handle_tool_key(ev);
        }
        return true;
    default: return false;
    }
}

// --ui-events (steps separated by ; or |; a click needs a move to the same point one step earlier so hover is up to date): "move 10,20; down 10,20; up 10,20; click 10,20; rclick 10,20; key Escape; key ctrl+s".
struct UiEvent {
    std::string kind;
    float x = 0.0f, y = 0.0f;
    std::string key;
};

std::vector<UiEvent> parse_ui_events(const std::string& spec) {
    std::vector<UiEvent> out;
    std::string text = spec;
    std::replace(text.begin(), text.end(), '|', ';'); // '|' survives CMake argument lists, ';' does not
    std::istringstream steps(text);
    for (std::string step; std::getline(steps, step, ';');) {
        std::istringstream words(step);
        UiEvent e;
        words >> e.kind;
        if (e.kind.empty()) continue;
        if (e.kind == "key") words >> e.key;
        else {
            char comma = 0;
            words >> e.x >> comma >> e.y;
        }
        out.push_back(e);
    }
    return out;
}

void post_ui_event(const UiEvent& e, SDL_Window* win) {
    const SDL_WindowID id = SDL_GetWindowID(win);
    const auto mouse = [&](SDL_EventType type, int button) {
        SDL_Event ev{};
        ev.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            ev.motion.windowID = id;
            ev.motion.x = e.x;
            ev.motion.y = e.y;
        } else {
            ev.button.windowID = id;
            ev.button.x = e.x;
            ev.button.y = e.y;
            ev.button.button = static_cast<Uint8>(button);
            ev.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            ev.button.clicks = 1;
        }
        SDL_PushEvent(&ev);
    };
    if (e.kind == "move") mouse(SDL_EVENT_MOUSE_MOTION, 0);
    else if (e.kind == "down") mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
    else if (e.kind == "up") mouse(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    else if (e.kind == "click" || e.kind == "rclick") {
        const int b = e.kind == "click" ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT;
        mouse(SDL_EVENT_MOUSE_MOTION, 0);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, b);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, b);
    } else if (e.kind == "key") {
        SDL_Keymod mod = SDL_KMOD_NONE;
        std::string name = e.key;
        for (const auto& [prefix, m] : {std::pair<const char*, SDL_Keymod>{"ctrl+", SDL_KMOD_CTRL}, {"shift+", SDL_KMOD_SHIFT}, {"alt+", SDL_KMOD_ALT}}) {
            if (name.starts_with(prefix)) { mod = static_cast<SDL_Keymod>(mod | m); name = name.substr(std::string_view(prefix).size()); }
        }
        const SDL_Keycode key = SDL_GetKeyFromName(name.c_str());
        for (const bool down : {true, false}) {
            SDL_Event ev{};
            ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            ev.key.windowID = id;
            ev.key.key = key;
            ev.key.mod = mod;
            ev.key.down = down;
            SDL_PushEvent(&ev);
        }
    } else {
        LOG_WARN("--ui-events: unknown step '{}'", e.kind);
    }
}

} // namespace

int run_rml_ui(const EditorOptions& o, bool continuous) {
    if (!o.replay_verify.empty()) return run_replay_verify_file(o.project, o.replay_verify, o.font_path, o.jobs); // headless, no scene
    // Screenshot and script runs neither read nor write the user's settings.
    const bool persist = o.screenshot.empty() && o.script_commands.empty();
    // --script-commands alone is headless (no window, no font: the same game text measuring as the ImGui editor).
    const bool headless = !o.script_commands.empty() && o.screenshot.empty();
    EditorController ctl;
    ctl.console().install();
    ctl.set_play_setup({o.font_path, o.jobs, o.audio && !platform::env_is("SDL_VIDEO_DRIVER", "offscreen") && !platform::env_is("CUERVO_AUDIO", "null")});
    if (persist) ctl.settings().load(EditorSettings::user_file());
    RmlLayout saved = ctl.settings().rml_layout();
    if (o.ui_scale) saved.ui_scale = *o.ui_scale;
    if (o.theme) saved.theme = *o.theme == brand::Theme::Light ? "light" : "dark";
    ctl.settings().rml_layout() = saved;

    // Tokens come from editor/theme.cpp (plain structs, no UI library).
    theme::set_ui_scale(saved.ui_scale > 0.0f ? saved.ui_scale : 1.0f);
    theme::set_current(saved.theme == "light" ? brand::Theme::Light : saved.theme == "dark" ? brand::Theme::Dark : theme::system_theme());
    float scale = theme::ui_scale();

    std::optional<render::FontAtlas> atlas = headless ? std::nullopt : render::load_font_file(o.font_path);
    if (!atlas && !headless) LOG_WARN("No font ('{}'): scene text is not drawn", o.font_path);
    ctl.set_font_atlas(atlas ? &*atlas : nullptr);
    ProjectEnv& env = ctl.env();
    const render::FontAtlas* font_ptr = atlas ? &*atlas : nullptr;
    EditorDocument& doc = ctl.doc();
    std::string error;
    if (!o.project.empty() ? !env.open(o.project, atlas ? &*atlas : nullptr, error) : !env.open_prelude_only(error)) {
        LOG_ERROR("{}", error);
        return EXIT_FAILURE;
    }
    doc.set_registry(&env.classes());
    ctl.assets().open(env.project_dir());
    if (env.has_project()) { std::string ime; if (!ctl.input_map().load(env.project_dir(), ime)) LOG_WARN("{}", ime); }
    const fs::path scene_path = !o.scene.empty() ? env.resolve(o.scene) : env.resolve(env.project().main_scene);
    std::error_code scene_ec;
    if (!fs::is_regular_file(scene_path, scene_ec) || !doc.open(scene_path, error)) {
        LOG_WARN("{}", error);
        doc.new_scene();
    }

    if (o.tool) ctl.set_tool(*o.tool);
    if (!o.select_paths.empty()) select_paths(ctl, o.select_paths);
    if (!o.script_commands.empty()) {
        const int code = run_script_file(ctl, o.script_commands, o.font_path, o.jobs, nullptr);
        if (code != EXIT_SUCCESS || headless) {
            return code;
        }
    }

    Window window;
    std::unique_ptr<rhi::Device> device;
    for (const rhi::Backend b : {o.backend, rhi::Backend::OpenGL}) {
        if (!window.create({.title = "Cuervo Engine Editor (RmlUi)", .width = o.width, .height = o.height,
                            .graphics = rhi::graphics_api(b), .gl_es = o.gl_es})) continue;
        rhi::DeviceOptions dopt;
        dopt.vsync = o.vsync;
        device = rhi::create_device(b, window, dopt);
        if (device) break;
        window.destroy();
        if (!o.allow_fallback) break;
    }
    if (!device) {
        LOG_ERROR("could not open a window and render device");
        return EXIT_FAILURE;
    }
    LOG_INFO("RmlUi editor renders with {}", rhi::backend_name(device->info().backend));

    RmlRenderer renderer;
    renderer.init(*device);
    renderer.set_pixel_ratio(scale);
    SceneResources scene_res;
    if (!scene_res.init(*device, atlas ? &*atlas : nullptr)) {
        LOG_ERROR("could not create the scene renderers");
        return EXIT_FAILURE;
    }
    // The splash (same as the ImGui editor): shown on startup unless --no-splash, held with --splash
    // until a click or key; screenshots show it only with --splash.
    Splash splash;
    enum class SplashState { Off, Timed, Held } splash_state = SplashState::Off;
    double splash_start = -1.0;
    if (o.splash == SplashMode::On || (o.splash == SplashMode::Auto && o.screenshot.empty() && o.script_commands.empty())) {
        const std::string brand_dir = platform::utf8(platform::find_asset("brand", CUERVO_BRAND_DIR));
        const std::string display_font = platform::utf8(platform::find_asset("fonts/Outfit-Medium.ttf", CUERVO_FONT_DISPLAY));
        if (splash.init(*device, scene_res.text, brand_dir, display_font)) splash_state = o.splash == SplashMode::On ? SplashState::Held : SplashState::Timed;
        else LOG_WARN("Splash: the brand mark could not be loaded from '{}'", brand_dir);
    }
    ViewCamera camera;
    SceneInput scene(ctl, camera);
    FocusRouter router;
    std::uint64_t preview_revision = ~std::uint64_t{0};
    bool camera_framed = false;
    bool over_scene = false;
    bool quit = false;
    EditorSystem system(window.native());
    Rml::SetSystemInterface(&system);
    Rml::SetRenderInterface(&renderer);
    Rml::Initialise();
    register_particle_view();
    register_plot_view();
    register_key_view();
    register_code_view();
    const auto face = [](const char* path, Rml::Style::FontWeight w = Rml::Style::FontWeight::Auto) {
        if (!Rml::LoadFontFace(path, false, w)) LOG_WARN("RmlUi: font '{}' failed to load", path);
    };
    face(CUERVO_FONT_UI);
    face(CUERVO_FONT_UI_BOLD, Rml::Style::FontWeight::Bold);
    face(CUERVO_FONT_MONO);

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window.native(), &w, &h);
    Rml::Context* ctx = Rml::CreateContext("editor", {w, h});
    ctx->SetDensityIndependentPixelRatio(scale);

    // Generated tokens next to the (unedited) static files; {{TOKENS}} in the RML names them.
    const fs::path src = fs::path(CUERVO_RML_DIR);
    // One directory per run, so editors started together (or the tests in parallel) never share a tokens file.
    const fs::path tmp = fs::temp_directory_path() / std::format("cuervo_rml_{:x}", static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code tmp_ec;
    fs::create_directories(tmp, tmp_ec);
    const auto write_tokens = [&] { std::ofstream(tmp / "tokens.rcss") << rml_theme::export_tokens(theme::current()); };
    write_tokens();
    std::string rml = read_file(src / "editor.rml");
    for (const auto& [key, val] : {std::pair<std::string, std::string>{"{{TOKENS}}", platform::utf8(tmp / "tokens.rcss")},
                                   {"{{RCSS}}", platform::utf8(src / "editor.rcss")},
                                   {"{{WIDGETS}}", platform::utf8(src / "widgets.rcss")},
                                   {"{{SCRIPT}}", platform::utf8(src / "script" / "script.rcss")},
                                   {"{{BACKEND}}", std::string(rhi::backend_name(device->info().backend))}}) {
        for (std::size_t at; (at = rml.find(key)) != std::string::npos;) rml.replace(at, key.size(), val);
    }
    Rml::ElementDocument* page = ctx->LoadDocumentFromMemory(rml);
    if (page == nullptr) {
        LOG_ERROR("RmlUi: editor.rml failed to load");
        return EXIT_FAILURE;
    }
    page->Show();

    ShellHost host;
    host.window = window.native();
    host.quit = [&] { quit = true; };
    host.frame_scene = [&] { scene.frame_all(); };
    host.zoom_by = [&](float f) { camera.zoom = std::clamp(f == 0.0f ? 1.0f : camera.zoom * f, ViewCamera::min_zoom, ViewCamera::max_zoom); };
    host.zoom_percent = [&] { return camera.zoom * 100.0f; };
    host.view_readout = [&] { return scene.readout(); };
    host.view_center = [&] { return camera.center; };
    host.world_at = [&](float x, float y) -> std::optional<vec2> {
        const vec2 p{x, y};
        if (!router.inside(p) || ctl.play().active()) return std::nullopt;
        return scene.world_at(p - router.origin());
    };
    host.set_theme = [&](brand::Theme t) {
        theme::set_current(t);
        write_tokens();
        Rml::Factory::ClearStyleSheetCache();
        page->ReloadStyleSheet();
    };
    host.system_theme = [] { return theme::system_theme(); };
    host.set_scale = [&](float s) {
        scale = s;
        theme::set_ui_scale(s);
        renderer.set_pixel_ratio(s);
        ctx->SetDensityIndependentPixelRatio(s);
    };
    Shell shell(ctl, host, persist);
    ctl.hooks.scene_opened = [&] { camera_framed = false; };
    const auto base_reload = ctl.hooks.scripts_reloaded;
    ctl.hooks.scripts_reloaded = [&, base_reload] {
        if (base_reload) base_reload();
        preview_revision = ~std::uint64_t{0};
        ctl.invalidate_ready_preview();
    };
    ctl.hooks.persist = [&] { shell.save_layout(); };
    shell.bind(ctx, page);
    if (env.has_project()) shell.note_project(env.project_dir());
    shell.apply_start({o.tabs, o.layout, o.screen == "welcome", o.palette});

    DeviceState devices;
    vec2 last_pointer{0.0f, 0.0f};
    rhi::Rect game_rect{};

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto last_input = start;
    auto last_tick = start;
    auto last_frame = start;
    std::uint64_t frames = 0, shown = 0;
    double busy_ms = 0.0;
    std::vector<fs::path> dropped;
    const std::vector<UiEvent> ui_events = parse_ui_events(o.ui_events);
    std::size_t ui_next = 0;
    std::uint64_t ui_wait = ui_events.empty() ? 0 : 8; // frame at which the next step is posted
    // The game stopped at a breakpoint: the VM waits inside the debugger and calls this about every 20 ms.
    // One UI frame (input, shell, render) without touching the game: no play update, no game view, and
    // Stop/Play are deferred by the PlayController (it must not drop the session under the blocked VM).
    ctl.play().set_debug_idle([&] {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { quit = true; ctl.stop_play(); }
            if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) ctx->SetDimensions({ev.window.data1, ev.window.data2});
            const Rml::Element* focus = ctx->GetFocusElement();
            const bool typing = focus != nullptr && (focus->GetTagName() == "input" || focus->GetTagName() == "textarea" || focus->GetTagName() == "code-view");
            router.set_text_editing(typing);
            router.set_ui_modal(shell.ui_modal());
            RouteCtx rc{router, scene, shell, over_scene, false, typing};
            if (!route_event(ev, ctx, rc)) RmlSDL::InputEventHandler(ctx, window.native(), ev);
            last_input = clock::now();
        }
        shell.refresh();
        ctx->Update();
        SDL_GetWindowSizeInPixels(window.native(), &w, &h);
        device->begin_frame({w, h});
        device->clear(brand::mix(rml_theme::surfaces(theme::current()).page, theme::colors().warning, 0.10f));
        renderer.begin_frame();
        ctx->Render();
        renderer.end_frame();
        device->end_frame();
    });
    while (!quit) {
        if (ui_next < ui_events.size() && frames >= ui_wait) {
            post_ui_event(ui_events[ui_next++], window.native());
            ui_wait = frames + 3;
        }
        const bool playing = ctl.play().active();
        const bool busy = ui_next < ui_events.size() || playing || shell.animating() || splash_state == SplashState::Timed; // the particle preview and timeline playback keep frames coming
        bool got = false;
        SDL_Event ev;
        // Idle costs nothing: block for events up to 250 ms; after input, redraw continuously for
        // 400 ms so hover transitions finish. Play mode runs every frame.
        FrameState fs{continuous, busy, std::chrono::duration<double>(clock::now() - last_input).count(), false, static_cast<unsigned>(frames)};
        if (may_block_for_events(fs) && SDL_WaitEventTimeout(&ev, 250)) {
            got = true;
        } else if (SDL_PollEvent(&ev)) {
            got = true;
        }
        while (got) {
            if (ev.type == SDL_EVENT_QUIT) quit = true;
            if (splash_state != SplashState::Off && (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_KEY_DOWN)) { // dismisses the splash, goes no further
                splash_state = SplashState::Off;
                got = SDL_PollEvent(&ev);
                continue;
            }
            if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data != nullptr) dropped.push_back(platform::path_from_utf8(ev.drop.data));
            if (ev.type == SDL_EVENT_DROP_COMPLETE && !dropped.empty()) shell.import_dropped(std::exchange(dropped, {}));
            if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) ctx->SetDimensions({ev.window.data1, ev.window.data2});
            if (playing) window.handle_event(ev, devices);
            if (shell.capturing_binding()) { // input map "Bind": raw devices see the event, nothing else does
                window.handle_event(ev, shell.capture_devices());
                last_input = clock::now();
                got = SDL_PollEvent(&ev);
                continue;
            }
            const Rml::Element* focus = ctx->GetFocusElement();
            const bool typing = focus != nullptr && (focus->GetTagName() == "input" || focus->GetTagName() == "textarea" || focus->GetTagName() == "code-view");
            router.set_text_editing(typing);
            router.set_ui_modal(shell.ui_modal());
            RouteCtx rc{router, scene, shell, over_scene, ctl.play().active(), typing};
            if (!route_event(ev, ctx, rc)) RmlSDL::InputEventHandler(ctx, window.native(), ev);
            last_input = clock::now();
            got = SDL_PollEvent(&ev);
        }
        const auto now = clock::now();
        const double dt = std::min(std::chrono::duration<double>(now - last_frame).count(), 0.1);
        const bool tick = std::chrono::duration<double>(now - last_tick).count() >= 0.25;
        fs.since_input_s = std::chrono::duration<double>(now - last_input).count();
        fs.tick_due = tick;
        fs.frames = static_cast<unsigned>(frames);
        if (!should_draw(fs)) continue;
        last_frame = now;
        if (tick) last_tick = now;
        const auto t0 = clock::now();
        profiler::frame_mark(); // frame boundary for the Profiler tab (a no-op until Record is on)
        CUERVO_PROFILE_ZONE("editor frame");
        ctl.set_clock(std::chrono::duration<double>(now - start).count());
        if (ctl.assets().revision() == 0 || tick) ctl.assets().poll();
        if (ctl.switch_requested()) {
            // Open the other project in place: same window and device, new environment (the welcome
            // screen is the ImGui editor's; here an empty target opens the prelude-only environment).
            const std::string target = ctl.switch_target();
            ctl.acknowledge_switch();
            ctl.stop_play();
            const fs::path previous = env.has_project() ? env.project_dir() : fs::path();
            std::string err;
            bool ok = target.empty() ? env.open_prelude_only(err) : env.open(platform::path_from_utf8(target), font_ptr, err);
            if (!ok) {
                ctl.report_failure(err);
                LOG_ERROR("{}", err);
                err.clear();
                ok = previous.empty() ? env.open_prelude_only(err) : env.open(previous, font_ptr, err);
            }
            if (ok) {
                doc.set_registry(&env.classes());
                ctl.assets().open(env.project_dir());
                if (env.has_project()) { std::string ime; if (!ctl.input_map().load(env.project_dir(), ime)) LOG_WARN("{}", ime); }
                if (env.has_project()) shell.note_project(env.project_dir());
                const fs::path next_scene = env.has_project() ? env.resolve(env.project().main_scene) : fs::path();
                std::error_code fec;
                if (next_scene.empty() || !fs::is_regular_file(next_scene, fec) || !doc.open(next_scene, err)) doc.new_scene();
                camera_framed = false;
                preview_revision = ~std::uint64_t{0};
                if (ctl.hooks.scripts_reloaded) ctl.hooks.scripts_reloaded();
                ctl.notify_ok(env.has_project() ? std::format("Opened project {}.", env.project().name) : std::string("Closed the project."));
            }
        }
        if (frames == 2 && platform::env("RML_AUTOPLAY").has_value()) ctl.start_play(false); // screenshots of play mode
        shell.refresh();
        ctl.update_ready_preview();

        // Play: the game reads the devices only while the scene region has the input focus.
        const bool game_input = ctl.play().active() && router.scene_focus();
        if (ctl.play().active()) {
            static DeviceState none;
            none = DeviceState{};
            if (game_input && game_rect.width > 0 && game_rect.height > 0) {
                const render::ViewportFit fit = render::fit_viewport(render::ScaleMode::Fit, env.project().virtual_size, {game_rect.width, game_rect.height});
                last_pointer = render::framebuffer_to_virtual(fit, devices.mouse_position - vec2{static_cast<float>(game_rect.x), static_cast<float>(game_rect.y)});
            }
            CUERVO_PROFILE_ZONE("play");
            ctl.play().update(dt, game_input ? devices : none, last_pointer);
        }
        if (!(ctl.play().active() && game_input)) devices.clear_transients();

        ctx->Update();
        if (!ctl.play().active() && preview_revision != doc.revision()) {
            preview_revision = doc.revision();
            env.rebuild_preview(doc.model().to_doc());
        }
        // The timeline's "Pose": the clip at the playhead on the fresh preview, every frame (it survives rebuilds).
        if (shell.take_pose_dirty()) preview_revision = ~std::uint64_t{0}; // pose turned on or off: rebuild from the document
        if (const auto pose = shell.anim_pose(); pose && !ctl.play().active()) {
            const int index = doc.model().doc_index(pose->player);
            std::string why;
            if (index < 0 || !env.preview_animation(static_cast<std::size_t>(index), pose->clip, static_cast<float>(pose->tick), why)) shell.stop_pose();
        }
        Rml::Element* canvas = page->GetElementById("canvas");
        rhi::Rect view_rect{};
        if (canvas != nullptr && shell.scene_visible()) {
            view_rect = {static_cast<int>(canvas->GetAbsoluteLeft()), static_cast<int>(canvas->GetAbsoluteTop()),
                         static_cast<int>(canvas->GetOffsetWidth()), static_cast<int>(canvas->GetOffsetHeight())};
        }
        router.set_scene_rect(static_cast<float>(view_rect.x), static_cast<float>(view_rect.y), static_cast<float>(view_rect.width), static_cast<float>(view_rect.height));
        if (view_rect.width > 0) {
            scene.set_size({static_cast<float>(view_rect.width), static_cast<float>(std::max(view_rect.height, 1))});
            if (!camera_framed) {
                scene.frame_all();
                camera_framed = true;
            }
        }
        game_rect = view_rect;
        SDL_GetWindowSizeInPixels(window.native(), &w, &h);
        device->begin_frame({w, h});
        // Play mode tints the page: green while the game runs, amber while paused (the status line says it in text).
        const auto& tc = theme::colors();
        rhi::Color page_color = rml_theme::surfaces(theme::current()).page;
        if (ctl.play().state() == PlayState::Playing) page_color = brand::mix(page_color, tc.success, 0.10f);
        else if (ctl.play().state() == PlayState::Paused) page_color = brand::mix(page_color, tc.warning, 0.10f);
        device->clear(page_color);
        if (view_rect.width > 0 && view_rect.height > 0) {
            const ivec2 fb = device->framebuffer_size();
            const rhi::Rect clamped = clamp_rect(view_rect, fb.x(), fb.y());
            if (ctl.play().active()) {
                if (script::GameSession* session = ctl.play().session()) paint_game_view(scene_res.targets(*device), *session, clamped);
            } else {
                // The scene first, into its region; the UI (with a transparent canvas) draws over it.
                ScenePaintParams sp;
                sp.virtual_size = env.virtual_size();
                sp.clear_color = env.project().clear_color;
                sp.camera = camera;
                sp.colors = &theme::colors();
                sp.collision_shapes = ctl.collision_shapes();
                sp.lighting = ctl.preview_lighting();
                sp.overlay = &scene;
                if (ctl.ready_preview_on() && ctl.ready_preview().active() && !ctl.ready_preview().panicked()) sp.drawn = &ctl.ready_preview().session()->host();
                else if (env.preview_ok()) sp.drawn = &env.host();
                paint_scene_view(scene_res.targets(*device), sp, clamped);
            }
        }
        renderer.begin_frame();
        ctx->Render();
        renderer.end_frame();
        if (splash_state != SplashState::Off) {
            constexpr double hold = 1.4;
            constexpr double fade = 0.45; // same timings as the ImGui editor's splash
            const double now_s = std::chrono::duration<double>(clock::now() - start).count();
            if (splash_start < 0.0) splash_start = now_s;
            float opacity = 1.0f;
            if (splash_state == SplashState::Timed) {
                const double t = now_s - splash_start;
                if (t >= hold + fade) splash_state = SplashState::Off;
                else if (t > hold) opacity = static_cast<float>(1.0 - (t - hold) / fade);
            }
            if (splash_state != SplashState::Off) {
                splash.render(*device, scene_res.shapes, scene_res.text, {w, h}, theme::current(), opacity,
                              std::format("Engine Editor  ·  {}", env.has_project() ? env.project().name : std::string("welcome")));
            }
        }
        ++frames;
        const bool capture = !o.screenshot.empty() && ui_next >= ui_events.size() && frames >= std::max<std::uint64_t>(ui_wait, static_cast<std::uint64_t>(o.screenshot_frame)) + (platform::env("RML_AUTOPLAY").has_value() ? 40 : 0);
        if (capture) device->capture_frame(o.screenshot);
        device->end_frame();
        busy_ms += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        ++shown;
        if (capture) break;
        if (o.quit_after > 0.0 && std::chrono::duration<double>(clock::now() - start).count() > o.quit_after) break;
    }
    ctl.stop_play();
    shell.save_layout();
    splash.shutdown();
    LOG_INFO("RmlUi editor: {} frames drawn, {:.2f} ms average CPU per drawn frame, {} draw calls in the last frame",
             shown, shown > 0 ? busy_ms / static_cast<double>(shown) : 0.0, renderer.draw_calls());
    fs::remove_all(tmp, tmp_ec);
    Rml::RemoveContext("editor");
    Rml::Shutdown();
    renderer.shutdown();
    return EXIT_SUCCESS;
}

} // namespace cuervo::editor

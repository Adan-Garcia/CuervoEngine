#include "editor/app.hpp"
#include "editor/license_view.hpp"
#include "editor/launcher.hpp"
#include "cuervo/platform/license.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
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
#include "editor/command_script.hpp"
#include "editor/theme.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

namespace {

double now_seconds() {
    return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
}

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
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
    console_.install();
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
    console_.uninstall();
}

void EditorApp::report(bool ok) {
    if (!ok && !doc_.error().empty()) report_failure(doc_.error());
}

void EditorApp::request_add_child(NodeId parent) {
    add_parent_ = parent;
    class_filter_.clear();
    dialog_ = Dialog::AddChild;
    dialog_opened_ = false;
}

bool EditorApp::start(std::string& error) {
    const bool headless = !options_.script_commands.empty() && options_.screenshot.empty();
    // The game font: UI layout measures text with it (UI.measure) and the
    // scene view draws text with it, so it loads before the scripts compile.
    if (!headless) {
        font_atlas_ = render::load_font_file(options_.font_path);
        if (!font_atlas_) LOG_WARN("No font ('{}'): scene text is not drawn", options_.font_path);
    }
    if (!env_.open(options_.project, font_atlas_ ? &*font_atlas_ : nullptr, error)) return false;
    doc_.set_registry(&env_.classes());
    LOG_INFO("Project '{}': {} node classes", env_.project().name, env_.classes().classes().size());
    assets_.open(env_.project_dir());
    if (!input_map_.load(env_.project_dir(), error)) return false;
    open_initial_scene();
    restore_user_state();
    rebuild_actions();
    if (options_.tool) tool_ = *options_.tool;
    if (!options_.select_paths.empty()) {
        std::vector<NodeId> ids;
        for (const std::string& path : options_.select_paths) {
            if (const NodeId id = doc_.model().find_path(path); id != 0) ids.push_back(id);
            else LOG_WARN("--select: no node at '{}'", path);
        }
        doc_.select_set(ids);
    }
    if (options_.palette) palette_request_ = true, palette_query_ = *options_.palette;
    return true;
}

void EditorApp::open_initial_scene() {
    std::string scene = options_.scene.empty() ? env_.project().main_scene : options_.scene;
    if (!scene.empty()) {
        std::string error;
        const fs::path path = env_.resolve(scene);
        if (doc_.open(path, error)) {
            LOG_INFO("Opened '{}'", env_.relative(path));
            return;
        }
        if (!options_.scene.empty() || fs::exists(path)) {
            LOG_ERROR("{}", error);
        }
    }
    doc_.new_scene();
}

int EditorApp::run_script() {
    bool ok = false;
    const std::string text = read_file(options_.script_commands, ok);
    if (!ok) {
        LOG_ERROR("--script-commands: can't read '{}'", options_.script_commands);
        return EXIT_FAILURE;
    }
    CommandContext context{doc_, env_, &play_, &input_map_, &assets_, options_.font_path, options_.jobs};
    const ScriptResult r = run_command_script(text, context);
    if (!r.ok) {
        LOG_ERROR("{}:{}: {}", options_.script_commands, r.line, r.error);
        return EXIT_FAILURE;
    }
    if (doc_.dirty()) {
        std::string error;
        if (!doc_.save(error)) {
            LOG_ERROR("--script-commands: {}", error);
            return EXIT_FAILURE;
        }
        LOG_INFO("Saved '{}'", env_.relative(doc_.path()));
        assets_.poll(); // our own write is not an external change
    }
    if (input_map_.dirty()) LOG_WARN("--script-commands: the input map was edited but not saved ('input save')");
    LOG_INFO("--script-commands: {} command(s) applied, {} node(s) in '{}'", r.commands, doc_.model().size(),
             env_.relative(doc_.path()));
    return EXIT_SUCCESS;
}

int EditorApp::run_replay_verify() {
    std::string error;
    auto replay = load_replay(platform::path_from_utf8(options_.replay_verify), error);
    if (!replay) {
        LOG_ERROR("--replay-verify: {}", error);
        return EXIT_FAILURE;
    }
    script::SessionOptions base;
    base.font_path = options_.font_path;
    base.jobs = options_.jobs;
    const script::ReplayCheck c = script::verify_replay(platform::path_from_utf8(options_.project), *replay, base);
    if (!c.error.empty()) {
        LOG_ERROR("--replay-verify: {}", c.error);
        return EXIT_FAILURE;
    }
    if (c.first_mismatch) {
        LOG_ERROR("--replay-verify: state hash mismatch at tick {} of {}: recorded {:016x}, got {:016x}",
                  *c.first_mismatch, replay->ticks(), c.expected, c.actual);
        return EXIT_FAILURE;
    }
    LOG_INFO("--replay-verify: {} ticks replayed on {} thread(s), all {} state hashes match (final state hash {:016x})",
             c.ticks, jobs::default_jobs().thread_count(), c.compared, c.final_hash);
    return EXIT_SUCCESS;
}

bool EditorApp::open_window(std::string& error) {
    auto try_open = [&](rhi::Backend backend) -> bool {
        if (!window_.create({.title = "Cuervo Engine Editor",
                             .width = options_.width,
                             .height = options_.height,
                             .graphics = backend == rhi::Backend::Vulkan ? GraphicsApi::Vulkan : GraphicsApi::OpenGL,
                             .gl_es = options_.gl_es})) {
            return false;
        }
        rhi::DeviceOptions o;
        o.vsync = options_.vsync;
        // The editor shows what the game will look like: gated to its tier.
        o.tier = env_.project().render_tier;
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
    if (!shapes_.init(*device_, 4096) || !text_.init(*device_) || !meshes_.init(*device_)) {
        error = "could not create the scene renderers";
        return false;
    }
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
                             (options_.splash == SplashMode::Auto && options_.screenshot.empty());
    if (want_splash) {
        const std::string brand_dir = platform::utf8(platform::find_asset("brand", CUERVO_BRAND_DIR));
        const std::string display_font = platform::utf8(platform::find_asset("fonts/Outfit-Medium.ttf", CUERVO_FONT_DISPLAY));
        if (splash_.init(*device_, text_, brand_dir, display_font)) {
            splash_state_ = options_.splash == SplashMode::On ? SplashState::Held : SplashState::Timed;
        } else {
            LOG_WARN("Splash: the brand mark could not be loaded from '{}'", brand_dir);
        }
    }
    if (!ImGui_ImplSDL3_InitForOther(window_.native())) {
        error = "ImGui SDL3 backend failed to start";
        return false;
    }
    imgui_ready_ = true;
    if (!imgui_renderer_.init(*device_)) {
        error = "ImGui renderer failed to start";
        return false;
    }
    return true;
}

void EditorApp::shutdown() {
    if (imgui_ready_) save_user_state();
    if (imgui_ready_) {
        imgui_renderer_.shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    splash_.shutdown();
    text_.shutdown();
    meshes_.shutdown();
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
        if (code != EXIT_SUCCESS || options_.screenshot.empty()) return code;
    }
    if (!open_window(error)) {
        LOG_ERROR("{}", error);
        return EXIT_FAILURE;
    }
    const double start_time = now_seconds();
    while (running_) {
        frame();
        if (options_.quit_after > 0.0 && now_seconds() - start_time >= options_.quit_after) {
            LOG_INFO("--quit-after reached ({} frames)", frames_);
            running_ = false;
        }
    }
    return EXIT_SUCCESS;
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
        ImGui_ImplSDL3_ProcessEvent(&event);
        // The same events drive the game's devices (play-in-editor) and the
        // input map's "press a key" capture.
        window_.handle_event(event, devices_);
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running_ = false;
    }
    if (now - last_poll_ >= 1.0) {
        last_poll_ = now;
        poll_assets();
    }
    // The scene view shows the document as of this frame's start.
    if (preview_revision_ != doc_.revision()) {
        preview_revision_ = doc_.revision();
        env_.rebuild_preview(doc_.model().to_doc());
    }
    update_title();

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    clock_ = now;
    handle_shortcuts();
    draw_menu_bar();
    draw_docks();
    draw_dialogs();
    draw_toolbar();
    draw_palette();
    draw_messages();
    // Continuous edits end when nothing is held: the next edit is a new
    // undo step (backs up widgets whose release isn't seen, e.g. pickers).
    if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) doc_.seal();
    // The game ticks after the UI (which may have paused or stepped it), so
    // this frame draws the latest tick. Screenshots freeze it where a
    // command script left it.
    if (options_.screenshot.empty()) {
        CUERVO_PROFILE_ZONE("play");
        update_play(seconds);
    }
    ImGui::Render();

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
    if (capture) {
        LOG_INFO("Screenshot written to '{}' ({} frames)", options_.screenshot, frames_);
        running_ = false;
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
                   std::format("Engine Editor  ·  {}", env_.project().name));
}

void EditorApp::update_play(double seconds) {
    if (play_.active()) {
        // The game reads the devices only while the view has the input focus.
        static DeviceState none;
        none = DeviceState{};
        DeviceState& source = game_input_ ? devices_ : none;
        if (game_input_) last_pointer_ = game_pointer();
        play_.update(seconds, source, last_pointer_);
        if (play_.panic() && !panic_seen_) {
            panic_seen_ = true;
            if (Panel* p = panels_.find("Debugger")) p->select = true;
        }
        if (!play_.panic()) panic_seen_ = false;
    }
    // Taps the game didn't consume this frame (no tick ran, or the game
    // doesn't have the input) don't linger into later ticks.
    if (!(play_.active() && game_input_)) devices_.clear_transients();
}

vec2 EditorApp::game_pointer() const {
    if (game_rect_.width <= 0 || game_rect_.height <= 0) return last_pointer_;
    const render::ViewportFit fit = render::fit_viewport(render::ScaleMode::Fit, env_.project().virtual_size,
                                                         {game_rect_.width, game_rect_.height});
    const vec2 local = devices_.mouse_position - vec2{static_cast<float>(game_rect_.x), static_cast<float>(game_rect_.y)};
    return render::framebuffer_to_virtual(fit, local);
}

void EditorApp::set_game_view(rhi::Rect rect, bool has_input) {
    game_rect_ = rect;
    game_input_ = has_input && play_.active();
}

script::SessionOptions EditorApp::play_options(bool current_scene) const {
    script::SessionOptions o;
    o.dir = env_.project_dir();
    o.font_path = options_.font_path;
    o.jobs = options_.jobs;
    const bool headless = platform::env_is("SDL_VIDEO_DRIVER", "offscreen");
    o.audio_device = options_.audio && !headless && !platform::env_is("CUERVO_AUDIO", "null");
    if (current_scene) {
        o.scene = std::make_shared<const scene::SceneDoc>(doc_.model().to_doc());
        o.scene_label = doc_.has_path() ? env_.relative(doc_.path()) : std::string("(new scene)");
    }
    return o;
}

void EditorApp::start_play(bool current_scene, bool record) {
    std::string error;
    if (!play_.start(play_options(current_scene), error)) {
        LOG_ERROR("Play: {}", error.empty() ? std::string("the game could not start") : error);
        return;
    }
    if (record) play_.start_recording();
    panic_seen_ = false;
    if (Panel* p = panels_.find("Scene View")) p->select = true;
}

void EditorApp::stop_play() { play_.stop(); }

void EditorApp::start_replay(const std::string& path) {
    std::string error;
    auto replay = load_replay(env_.resolve(path), error);
    if (!replay || !play_.start_replay(play_options(false), std::move(*replay), error)) {
        LOG_ERROR("Replay: {}", error);
        return;
    }
    if (Panel* p = panels_.find("Replay")) p->select = true;
}

void EditorApp::poll_assets() {
    std::vector<std::string> changed;
    if (!assets_.poll(&changed)) return;
    bool scripts = false;
    for (const std::string& c : changed) {
        if (c.ends_with(".plm")) scripts = true;
        if (doc_.has_path() && c == env_.relative(doc_.path()) && std::filesystem::exists(doc_.path())) {
            LOG_WARN("'{}' changed on disk{}", c, doc_.dirty() ? " (the editor has unsaved changes to it)" : "");
        }
        if (c == "project.toml" && !input_map_.dirty()) {
            std::string error;
            if (!input_map_.load(env_.project_dir(), error)) LOG_WARN("{}", error);
        }
    }
    // Playing: the session hot-reloads by itself (PlayController::update).
    if (scripts && !play_.active()) reload_project_scripts();
}

void EditorApp::reload_project_scripts() {
    ProjectEnv fresh;
    std::string error;
    const render::FontAtlas* font = font_atlas_ ? &*font_atlas_ : nullptr;
    if (!fresh.open(env_.project_dir(), font, error)) {
        LOG_ERROR("Scripts: {}", error);
        return;
    }
    if (!env_.open(env_.project_dir(), font, error)) {
        LOG_ERROR("Scripts: {}", error);
        return;
    }
    doc_.set_registry(&env_.classes());
    preview_revision_ = ~std::uint64_t{0}; // rebuild the scene view
    LOG_INFO("Scripts recompiled: {} node classes", env_.classes().classes().size());
    rebuild_actions();
}

void EditorApp::request_open_scene(const std::string& path) {
    if (doc_.dirty()) {
        pending_open_ = path;
        dialog_ = Dialog::DiscardChanges;
        dialog_opened_ = false;
        return;
    }
    std::string error;
    const std::filesystem::path p = env_.resolve(path);
    if (doc_.open(p, error)) {
        LOG_INFO("Opened '{}'", env_.relative(p));
        frame_scene();
    } else {
        LOG_ERROR("{}", error);
    }
}

void EditorApp::open_location(const std::string& file, std::uint32_t line) {
    if (file.ends_with(".scene")) {
        request_open_scene(file);
        return;
    }
    reveal_asset(file);
    if (Panel* p = panels_.find("Assets")) p->select = true;
    const std::string where = line > 0 ? std::format("{}:{}", file, line) : file;
    SDL_SetClipboardText(platform::utf8(env_.resolve(file)).c_str());
    LOG_INFO("{} (path copied to the clipboard)", where);
}

void EditorApp::update_title() {
    std::string title = std::format("{}{} \u2014 {} \u2014 Cuervo Engine Editor", doc_.dirty() ? "*" : "",
                                    doc_.has_path() ? env_.relative(doc_.path()) : std::string("(new scene)"),
                                    env_.project().name);
    if (title != title_) {
        title_ = title;
        SDL_SetWindowTitle(window_.native(), title_.c_str());
    }
}

void EditorApp::draw_dialogs() {
    const char* names[] = {"",           "New Scene",         "Open Scene", "Save Scene As",
                           "Add Child Node", "Unsaved Changes", "License", "Save Layout"};
    if (dialog_ == Dialog::None) return;
    const char* name = names[static_cast<int>(dialog_)];
    if (!dialog_opened_) {
        ImGui::OpenPopup(name);
        dialog_opened_ = true;
        if (dialog_ == Dialog::Open || dialog_ == Dialog::SaveAs) {
            path_input_ = doc_.has_path() ? env_.relative(doc_.path()) : std::string("scenes/new.scene");
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
    const auto& classes = env_.classes().classes();
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
        if (discard_switch_) {
            ImGui::TextWrapped("The scene has unsaved changes. Leave this project and discard them?");
        } else {
            ImGui::TextWrapped("The scene has unsaved changes. Open '%s' and discard them?", pending_open_.c_str());
        }
        if (ImGui::Button(discard_switch_ ? "Discard and Leave" : "Discard and Open")) {
            if (discard_switch_) {
                discard_switch_ = false;
                switch_target_ = pending_open_;
                switch_requested_ = true;
                running_ = false;
            } else {
                std::string error;
                const std::filesystem::path p = env_.resolve(pending_open_);
                if (doc_.open(p, error)) {
                    LOG_INFO("Opened '{}'", env_.relative(p));
                    frame_scene();
                } else {
                    LOG_ERROR("{}", error);
                    notify(message_open_failed(env_.relative(p), error, true));
                }
            }
            close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            discard_switch_ = false;
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
            for (fs::recursive_directory_iterator it(env_.project_dir(), ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && it->path().extension() == ".scene") found.push_back(env_.relative(it->path()));
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
            const fs::path path = env_.resolve(path_input_);
            const bool ok = dialog_ == Dialog::Open ? doc_.open(path, error) : doc_.save_as(path, error);
            if (ok) {
                LOG_INFO("{} '{}'", dialog_ == Dialog::Open ? "Opened" : "Saved", env_.relative(path));
                assets_.poll();
                if (dialog_ == Dialog::Open) frame_scene();
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
        if (doc_.dirty()) {
            ImGui::TextColored(theme::im(theme::colors().warning), "Unsaved changes to the current scene will be lost.");
        }
        if (ImGui::Button("Create")) {
            doc_.new_scene(new_root_class_, "root");
            frame_scene();
            close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) close();
    } else if (dialog_ == Dialog::AddChild) {
        const ModelNode* parent = doc_.model().get(add_parent_ != 0 ? add_parent_ : doc_.model().root());
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
            const ClassRegistry& reg = env_.classes();
            if (reg.is_a(chosen, "Node2D") && !reg.is_a(chosen, "Control")) {
                fields.push_back({"position", "vec2",
                                  scene::Value::vector({std::round(camera_.center.x()), std::round(camera_.center.y())})});
            }
            report(doc_.add_child(parent->id, chosen, {}, std::move(fields)) != 0);
            close();
        }
    }
    ImGui::EndPopup();
    if (!open) {
        dialog_ = Dialog::None;
        discard_switch_ = false;
    }
}

// ------------------------------------------------------------ scene view

void EditorApp::render_game_view(rhi::Rect view) {
    script::GameSession* session = play_.session();
    device_->set_viewport(view);
    device_->set_scissor(view);
    device_->clear({0.0f, 0.0f, 0.0f, 1.0f});
    if (session == nullptr) return;
    const script::Project& project = session->project();
    // Letterboxed like the runtime's `fit` scale mode, inside the view.
    render::ViewportFit fit = render::fit_viewport(render::ScaleMode::Fit, project.virtual_size, {view.width, view.height});
    fit.viewport.x += view.x;
    fit.viewport.y += view.y;
    device_->set_viewport(fit.viewport);
    device_->set_scissor(fit.viewport);
    device_->clear(project.clear_color);
    shapes_.clear();
    text_.clear();
    const float pixels = std::min(fit.scale.x(), fit.scale.y());
    const vec2 extent{static_cast<float>(project.virtual_size.x()), static_cast<float>(project.virtual_size.y())};
    // The game's own view: its Camera2D composes with the screen transform.
    meshes_.clear();
    session->host().draw(script::make_draw_targets(shapes_, text_, font_, &vectors_, render::virtual_to_ndc(extent),
                                                   pixels, true));
}

void EditorApp::render_scene_view(rhi::Rect view) {
    if (view.width <= 0 || view.height <= 0) return;
    if (play_.active()) {
        render_game_view(view);
        return;
    }
    device_->set_viewport(view);
    device_->set_scissor(view);
    // Around the game's screen: the theme's subtle page color.
    const brand::ThemeColors& ui = theme::colors();
    device_->clear(ui.bg_subtle);
    const vec2 size{static_cast<float>(view.width), static_cast<float>(view.height)};
    const Affine2 world_to_ndc = camera_.world_to_ndc(size);
    const float upp = camera_.world_units_per_pixel();
    shapes_.clear();
    text_.clear();

    // The game's screen (virtual coordinates) in its clear color.
    const vec2 screen = env_.virtual_size();
    shapes_.rounded_rect(screen * 0.5f, screen * 0.5f, 0.0f, 0.0f, env_.project().clear_color);
    shapes_.flush(world_to_ndc, upp);

    // The scene itself, drawn by its nodes' draw() exactly as in the game.
    if (env_.preview_ok() && !env_.host().panicked()) {
        // The editor's camera, not the scene's Camera2D (cameras off).
        meshes_.clear();
        env_.host().draw(script::make_draw_targets(shapes_, text_, font_, &vectors_, world_to_ndc,
                                                   upp > 0.0f ? 1.0f / upp : 1.0f, false));
        if (env_.host().panicked()) LOG_WARN("scene view: {}", env_.host().error());
    }
    shapes_.flush(world_to_ndc, upp);
    text_.flush(world_to_ndc);

    // A faint grid over the scene (a level's backdrop would hide one drawn
    // under it), in world units, lines one pixel wide at any zoom.
    const vec2 lo = camera_.screen_to_world({0.0f, 0.0f}, {0.0f, 0.0f}, size);
    const vec2 hi = camera_.screen_to_world(size, {0.0f, 0.0f}, size);
    const float step = grid_step(camera_.zoom);
    const float half_px = 0.5f * upp;
    const rhi::Color minor = brand::with_alpha(ui.text_muted, 0.08f);
    const rhi::Color major = brand::with_alpha(ui.text_muted, 0.18f);
    const rhi::Color axis = brand::with_alpha(ui.info, 0.55f);
    const vec2 mid = (lo + hi) * 0.5f;
    const vec2 half_extent = (hi - lo) * 0.5f;
    for (float x = std::floor(lo.x() / step) * step; x <= hi.x(); x += step) {
        const bool is_major = std::fmod(std::abs(x), step * 4.0f) < 0.5f * step;
        shapes_.rounded_rect({x, mid.y()}, {half_px, half_extent.y()}, 0.0f, 0.0f,
                             x == 0.0f ? axis : (is_major ? major : minor));
    }
    for (float y = std::floor(lo.y() / step) * step; y <= hi.y(); y += step) {
        const bool is_major = std::fmod(std::abs(y), step * 4.0f) < 0.5f * step;
        shapes_.rounded_rect({mid.x(), y}, {half_extent.x(), half_px}, 0.0f, 0.0f,
                             y == 0.0f ? axis : (is_major ? major : minor));
    }
    // The screen's frame on top of the grid.
    const rhi::Color frame = brand::with_alpha(ui.accent, 0.9f);
    const float w = 1.5f * upp;
    shapes_.rounded_rect({screen.x() * 0.5f, 0.0f}, {screen.x() * 0.5f + w, w}, 0.0f, 0.0f, frame);
    shapes_.rounded_rect({screen.x() * 0.5f, screen.y()}, {screen.x() * 0.5f + w, w}, 0.0f, 0.0f, frame);
    shapes_.rounded_rect({0.0f, screen.y() * 0.5f}, {w, screen.y() * 0.5f + w}, 0.0f, 0.0f, frame);
    shapes_.rounded_rect({screen.x(), screen.y() * 0.5f}, {w, screen.y() * 0.5f + w}, 0.0f, 0.0f, frame);
    shapes_.flush(world_to_ndc, upp);
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

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cuervo/input/devices.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/render/font.hpp"
#include "cuervo/render/light_batcher.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/sprite_batcher.hpp"
#include "cuervo/assets/vfs.hpp"
#include "cuervo/render/sprite_library.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/script/game_session.hpp"
#include "cuervo/script/vector_cache.hpp"
#include "editor/actions.hpp"
#include "editor/asset_library.hpp"
#include "editor/command_script.hpp"
#include "editor/console_log.hpp"
#include "editor/editor_controller.hpp"
#include "editor/editor_document.hpp"
#include "editor/editor_options.hpp"
#include "editor/license_view.hpp"
#include "editor/imgui_rhi.hpp"
#include "editor/input_map_doc.hpp"
#include "editor/layouts.hpp"
#include "editor/messages.hpp"
#include "editor/panels.hpp"
#include "editor/play_controller.hpp"
#include "editor/scene_preview.hpp"
#include "editor/project_env.hpp"
#include "editor/projects.hpp"
#include "editor/settings.hpp"
#include "editor/splash.hpp"
#include "editor/view_camera.hpp"

struct ImDrawList;
struct ImVec2;
using ImGuiID = unsigned int;

// `cuervo-editor`: the standalone Dear ImGui editor (DESIGN.md 4.13). See
// editor/README.md for usage, panels, shortcuts and the command format.
namespace cuervo::editor {

int run_editor(const EditorOptions& options);

// State shared by the panels (they get it in Panel::draw).
class EditorApp {
public:
    explicit EditorApp(const EditorOptions& options);
    ~EditorApp();
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    int run();

    EditorController& controller() { return ctl_; }
    EditorDocument& doc() { return ctl_.doc(); }
    // The animation timeline's scrub (milestone 9.8): while `on`, the scene
    // view shows `player`'s clip `clip` at `tick` (ProjectEnv::preview_animation).
    struct AnimPreview {
        bool on = false;
        NodeId player = 0;
        std::string clip;
        int tick = 0;
    };
    AnimPreview& anim_preview() { return anim_preview_; }
    // Rebuilds the scene view from the document next frame (drops a pose).
    void refresh_preview() {
        preview_revision_ = ~std::uint64_t{0};
        ctl_.invalidate_ready_preview();
    }
    bool ready_preview_on() const { return ctl_.ready_preview_on(); }
    void toggle_ready_preview() { ctl_.toggle_ready_preview(); }
    ProjectEnv& env() { return ctl_.env(); }
    ConsoleLog& console() { return ctl_.console(); }
    ViewCamera& camera() { return camera_; }
    AssetLibrary& assets() { return ctl_.assets(); }
    InputMapDoc& input_map() { return ctl_.input_map(); }
    PlayController& play() { return ctl_.play(); }
    PanelRegistry& panels() { return panels_; }
    DeviceState& devices() { return devices_; }
    ActionRegistry& actions() { return ctl_.actions(); }
    MessageCenter& messages() { return ctl_.messages(); }
    LayoutStore& layouts() { return layouts_; }

    // ---- 8.3: tools, palette, messages, layouts, projects
    Tool tool() const { return ctl_.tool(); }
    void set_tool(Tool tool) { ctl_.set_tool(tool); }
    int paint_tile_id() const { return ctl_.paint_tile_id(); }
    void set_paint_tile_id(int id) { ctl_.set_paint_tile_id(id); }
    // Snap to the grid / 15 degrees / 0.1 (Ctrl while dragging inverts it).
    bool snap() const { return ctl_.snap(); }
    void set_snap(bool snap) { ctl_.set_snap(snap); }
    bool collision_shapes() const { return ctl_.collision_shapes(); }
    void set_collision_shapes(bool enabled) { ctl_.set_collision_shapes(enabled); }
    bool preview_lighting() const { return ctl_.preview_lighting(); }
    void set_preview_lighting(bool on) { ctl_.set_preview_lighting(on); }
    // Ctrl+Shift+P; `query` pre-fills the search.
    void open_palette(std::string query = {});
    // A banner or toast in the kit's formula (messages.hpp); the console keeps the raw log line.
    void notify(UserMessage message) { ctl_.notify(std::move(message)); }
    void notify_ok(std::string text) { ctl_.notify_ok(std::move(text)); }
    // Reports a failed document operation: a message plus a log line.
    void report_failure(const std::string& raw) { ctl_.report_failure(raw); }
    // Leaves this project: the welcome screen (empty path) or another project opens next.
    void request_switch_project(std::string path = {}) { ctl_.request_switch_project(std::move(path)); }
    bool run_action(std::string_view id) { return ctl_.run_action(id); }
    bool switch_requested() const { return ctl_.switch_requested(); }
    const std::string& switch_target() const { return ctl_.switch_target(); }
    // Applies a saved layout by name (docks and visible panels).
    void apply_layout(const std::string& name);
    void save_layout_as(const std::string& name);
    // The window's current dock sizes and shown panels as a Layout.
    Layout current_layout() const;
    // The license agreement, read-only (Help menu).
    void show_license() { dialog_ = Dialog::License; dialog_opened_ = false; }

    // ---- play-in-editor (play_controller.hpp)
    // Plays the project's main (`current_scene` false, like `cuervo --game`)
    // or the open scene as edited; optionally recording from tick 0.
    void start_play(bool current_scene, bool record = false);
    void stop_play();
    // Replays a replay file (project-relative or absolute) in the view.
    void start_replay(const std::string& path);
    // The session options plays and replays use (font, audio, jobs).
    script::SessionOptions play_options(bool current_scene) const;
    // The scene view panel tells where the game is drawn and whether it has
    // the keyboard (the view is focused while playing).
    void set_game_view(rhi::Rect rect, bool has_input);
    bool game_has_input() const { return game_input_; }
    // The input map is waiting for a key to bind: editor shortcuts are off.
    bool capturing_binding = false;

    // ---- files
    // Opens a scene (asks first when the open one has unsaved changes).
    void request_open_scene(const std::string& path) { ctl_.request_open_scene(path); }
    // A file:line link (tier report, console): scenes open, other files are
    // shown in the asset browser and their location copied to the clipboard.
    void open_location(const std::string& file, std::uint32_t line);
    // The asset browser scrolls to and selects this path next frame.
    std::string take_reveal_request() { return std::exchange(reveal_asset_, {}); }
    void reveal_asset(std::string path) { reveal_asset_ = std::move(path); }
    // Recompiles the project's scripts (class registry and preview) after
    // they change on disk while not playing.
    void reload_project_scripts() { ctl_.reload_project_scripts(); }

    // Dynamic UI & Font scaling
    void zoom_in();
    void zoom_out();
    void reset_zoom();
    void set_ui_scale(float scale);
    void set_font_scale(float scale);
    float ui_scale() const;
    float font_scale() const;
    EditorSettings& settings() { return ctl_.settings(); }
    const EditorSettings& settings() const { return ctl_.settings(); }

    // Opens a script, document, or directory in the default code editor of the device.
    bool open_in_external_editor(const std::filesystem::path& path, int line = 0) { return ctl_.open_in_external_editor(path, line); }
    // Searches project scripts for `class <class_name>`. Returns { relative_file_path, line_number }.
    std::pair<std::string, int> find_class_script(const std::string& c) const { return ctl_.find_class_script(c); }
    bool is_script_class(const std::string& c) const { return ctl_.is_script_class(c); }
    bool has_script_bound(NodeId id) const { return ctl_.has_script_bound(id); }
    std::pair<std::string, int> node_script(NodeId id) const { return ctl_.node_script(id); }
    std::vector<std::pair<std::string, std::string>> project_script_classes() const { return ctl_.project_script_classes(); }
    std::vector<std::string> find_script_classes_in_file(const std::filesystem::path& f) const { return ctl_.find_script_classes_in_file(f); }
    bool attach_script_file(NodeId id, const std::filesystem::path& file, std::string& error) { return ctl_.attach_script_file(id, file, error); }
    bool create_and_attach_script(NodeId id, const std::string& class_name, const std::filesystem::path& rel_path, std::string& error) {
        return ctl_.create_and_attach_script(id, class_name, rel_path, error);
    }
    bool detach_script(NodeId id) { return ctl_.detach_script(id); }
    void request_attach_script(NodeId id) { ctl_.request_attach_script(id); }

    // Logs a rejected edit (EditorDocument::error) to the console.
    void report(bool ok) { ctl_.report(ok); }
    // ---- 11.7c: raster tooling. The Sprites panel shows this image or atlas next
    // (the Assets panel sets it on double-click), and previews through the same
    // SpriteLibrary the scene view draws with.
    void show_sprite_asset(std::string path);
    // OS files dropped on the window (SDL_EVENT_DROP_FILE ... DROP_COMPLETE): imports the
    // supported ones (file_import.hpp), reports each, and shows the first image.
    void import_dropped_files(const std::vector<std::filesystem::path>& files);
    std::string take_sprite_request() { return std::exchange(sprite_request_, {}); }
    render::SpriteRef sprite_preview(std::string_view asset, std::string_view frame);
    // Opens the class picker; the chosen class becomes a child of `parent`.
    void request_add_child(NodeId parent) { ctl_.request_add_child(parent); }
    // Frames the virtual screen in the scene view.
    void frame_scene() { frame_request_ = true; }
    bool take_frame_request() { return std::exchange(frame_request_, false); }

    // Queues the scene's rendering into `list` at this point of its draws:
    // the engine renders straight into the window inside `rect` (pixels),
    // between ImGui's draws, so overlays added after it land on top.
    void queue_scene_view(ImDrawList* list, rhi::Rect rect);
    // The callback's work: renders the scene into `view` (pixels).
    void render_scene_view(rhi::Rect view);
    // The running game, fit into `view` (letterboxed), drawn by its nodes.
    void render_game_view(rhi::Rect view);

private:
    bool start(std::string& error);
    bool open_window(std::string& error);
    void open_initial_scene();
    int run_script();
    void run_ui_steps();
    int run_replay_verify();
    void poll_assets();
    void update_play(double seconds);
    vec2 game_pointer() const;
    void frame();
    void draw_menu_bar();
    void draw_toolbar();
    void draw_docks();
    void build_default_dock(ImGuiID root, ImVec2 size);
    void draw_dialogs();
    void draw_palette();
    void draw_messages();
    void rebuild_actions();
    void add_app_actions();
    void restore_user_state();
    void save_user_state();
    void save_current_scene() { ctl_.save_current_scene(); }
    void apply_prompt();
    void handle_shortcuts();
    void update_title();
    void render_splash(ivec2 size, double now);
    void shutdown();

    // The UI-free session (editor_controller.hpp): document, project, play, actions, messages.
    EditorController ctl_;
    EditorOptions options_;
    std::optional<render::FontAtlas> font_atlas_;
    Window window_;
    std::unique_ptr<rhi::Device> device_;
    render::SdfBatcher shapes_;
    render::LightBatcher lights_;
    render::TextRenderer text_;
    // Sprites (Canvas.vector) in the scene view and the game view.
    render::MeshBatcher meshes_;
    script::VectorCache vectors_{meshes_};
    // Raster sprites (Sprite2D family) in the scene view and the game view.
    render::SpriteBatcher sprite_batcher_;
    render::SpriteLibrary sprite_library_;
    render::FontHandle font_;
    ImGuiRenderer imgui_renderer_;
    bool imgui_ready_ = false;
    bool viewports_on_ = false;       // ImGui multi-viewport (undocked panels in their own OS windows)
    // Window steps from a command script (undock, save-layout, expect-viewports), run one every few
    // frames once the window has settled; a failed expectation ends the run with an error.
    std::vector<UiStep> ui_steps_;
    std::size_t ui_step_next_ = 0;
    std::uint64_t ui_step_wait_ = 0;  // frames_ value before which no step runs
    bool ui_failed_ = false;
    std::map<std::string, std::pair<float, float>> undock_requests_; // panel title -> screen position, applied by draw_docks
    std::string sprite_request_;
    std::vector<std::filesystem::path> dropped_;
    std::shared_ptr<assets::Vfs> sprite_vfs_;
    std::filesystem::path sprite_vfs_dir_;
    PanelRegistry panels_;
    ViewCamera camera_;
    bool frame_request_ = true;
    std::uint64_t preview_revision_ = ~std::uint64_t{0};
    AnimPreview anim_preview_;
    bool running_ = true;
    std::uint64_t frames_ = 0;
    std::string title_;

    // 8.1: the brand splash over the first moments (or held with --splash).
    enum class SplashState { Off, Timed, Held };
    Splash splash_;
    SplashState splash_state_ = SplashState::Off;
    double splash_start_ = 0.0;

    // 6B: assets, input map, play-in-editor.
    // 11.7a: the scene view draws a ready()-run, never-ticked session instead of the edit-time preview.
    DeviceState devices_;
    bool game_input_ = false;
    rhi::Rect game_rect_{};
    vec2 last_pointer_{0.0f, 0.0f};
    double clock_ = 0.0;
    double last_time_ = 0.0;
    double last_poll_ = 0.0;
    bool panic_seen_ = false;
    std::string reveal_asset_;

    std::size_t actions_classes_ = 0; // the class count the "Add Node" actions were built for
    LayoutStore layouts_;
    Layout layout_;                   // the window's docks and panels right now
    RecentProjects recents_;
    bool palette_open_ = false;
    bool palette_request_ = false;
    std::string palette_query_;
    int palette_index_ = 0;
    std::string layout_name_input_;
    LicenseView license_view_;
    // Docking (ImGui docking branch): a layout to apply at the next draw_docks.
    std::string pending_dock_;        // ImGui ini text to load
    bool rebuild_dock_ = false;       // rebuild the default arrangement from the fractions
    bool dock_built_ = false;

    // Dialog state.
    enum class Dialog { None, NewScene, Open, SaveAs, AddChild, DiscardChanges, License, SaveLayout, AttachScript };
    Dialog dialog_ = Dialog::None;
    bool dialog_opened_ = false;
    std::string path_input_;
    std::string class_filter_;
    std::string new_root_class_ = "Node2D";
    NodeId add_parent_ = 0;
    NodeId attach_node_ = 0;
    int attach_tab_ = 0; // 0 = Existing, 1 = New
    std::string attach_filter_;
    std::string new_script_class_;
    std::string new_script_path_;
    std::string attach_error_;
};

} // namespace cuervo::editor

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cuervo/input/devices.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/render/font.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/script/game_session.hpp"
#include "cuervo/script/vector_cache.hpp"
#include "editor/actions.hpp"
#include "editor/asset_library.hpp"
#include "editor/console_log.hpp"
#include "editor/editor_document.hpp"
#include "editor/license_view.hpp"
#include "editor/imgui_rhi.hpp"
#include "editor/input_map_doc.hpp"
#include "editor/layouts.hpp"
#include "editor/messages.hpp"
#include "editor/panels.hpp"
#include "editor/play_controller.hpp"
#include "editor/project_env.hpp"
#include "editor/projects.hpp"
#include "editor/splash.hpp"
#include "editor/view_camera.hpp"

struct ImDrawList;
struct ImVec2;
using ImGuiID = unsigned int;

// `cuervo-editor`: the standalone Dear ImGui editor (DESIGN.md 4.13). See
// editor/README.md for usage, panels, shortcuts and the command format.
namespace cuervo::editor {

enum class SplashMode { Auto, On, Off };

// The scene view's transform tool (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): Q, W, E, R.
enum class Tool { Select, Move, Rotate, Scale };
const char* tool_name(Tool tool);

struct EditorOptions {
    std::string project;          // --project <dir>; none: the welcome screen picks one
    std::string scene;            // --scene <path>: open this scene (project-relative or absolute)
    rhi::Backend backend = rhi::Backend::OpenGL;
    bool allow_fallback = true;   // no --backend: Vulkan, else GL
    bool gl_es = false;
    bool vsync = true;
    std::string script_commands;  // --script-commands <file>: apply, save, exit (headless)
    std::string screenshot;       // --screenshot <file.ppm>: capture the whole window, exit
    int screenshot_frame = 6;     // frames rendered before the capture (layout settles)
    double quit_after = 0.0;      // seconds; 0 = until closed
    std::string font_path;        // UI and scene text
    // --view <x>,<y>,<zoom>: start the scene view there instead of framing
    // the game screen (screenshots of a zoomed view).
    std::optional<vec3> view;
    int width = 1600;
    int height = 900;
    // --replay-verify <file>: play a replay back headless, compare every
    // tick's state hash, exit 1 at the first mismatch (no window).
    std::string replay_verify;
    unsigned jobs = 0;            // --jobs: job system threads for play sessions
    bool audio = true;            // --no-audio: play sessions use the null audio backend
    // --tabs <Title,Title>: bring these panels' tabs to the front at start
    // (screenshots of the 6B panels).
    std::vector<std::string> tabs;
    // --theme light|dark: the brand theme (DESIGN.md 4.16); default the OS's.
    std::optional<brand::Theme> theme;
    // --splash: show the splash until a click or key (screenshots of it);
    // --no-splash: never. Default: shown at start, except for screenshots.
    SplashMode splash = SplashMode::Auto;
    // --screen welcome: show the welcome screen even when a project is given (for screenshots/tests).
    std::string screen;
    // --tool select|move|rotate|scale, --select </a,/b>, --palette [query]: start in that
    // state (screenshots of the gizmos and the palette).
    std::optional<Tool> tool;
    std::vector<std::string> select_paths;
    std::optional<std::string> palette;
    // --layout <name>: start in that saved layout.
    std::string layout;
};

int run_editor(const EditorOptions& options);

// State shared by the panels (they get it in Panel::draw).
class EditorApp {
public:
    explicit EditorApp(const EditorOptions& options);
    ~EditorApp();
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    int run();

    EditorDocument& doc() { return doc_; }
    ProjectEnv& env() { return env_; }
    ConsoleLog& console() { return console_; }
    ViewCamera& camera() { return camera_; }
    AssetLibrary& assets() { return assets_; }
    InputMapDoc& input_map() { return input_map_; }
    PlayController& play() { return play_; }
    PanelRegistry& panels() { return panels_; }
    DeviceState& devices() { return devices_; }
    ActionRegistry& actions() { return actions_; }
    MessageCenter& messages() { return messages_; }
    LayoutStore& layouts() { return layouts_; }

    // ---- 8.3: tools, palette, messages, layouts, projects
    Tool tool() const { return tool_; }
    void set_tool(Tool tool) { tool_ = tool; }
    // Snap to the grid / 15 degrees / 0.1 (Ctrl while dragging inverts it).
    bool snap() const { return snap_; }
    void set_snap(bool snap) { snap_ = snap; }
    // Ctrl+Shift+P; `query` pre-fills the search.
    void open_palette(std::string query = {});
    // A banner or toast in the kit's formula (messages.hpp); the console keeps the raw log line.
    void notify(UserMessage message);
    void notify_ok(std::string text);
    // Reports a failed document operation: a message plus a log line.
    void report_failure(const std::string& raw);
    // Leaves this project: the welcome screen (empty path) or another project opens next.
    void request_switch_project(std::string path = {});
    bool run_action(std::string_view id);
    bool switch_requested() const { return switch_requested_; }
    const std::string& switch_target() const { return switch_target_; }
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
    void request_open_scene(const std::string& path);
    // A file:line link (tier report, console): scenes open, other files are
    // shown in the asset browser and their location copied to the clipboard.
    void open_location(const std::string& file, std::uint32_t line);
    // The asset browser scrolls to and selects this path next frame.
    std::string take_reveal_request() { return std::exchange(reveal_asset_, {}); }
    void reveal_asset(std::string path) { reveal_asset_ = std::move(path); }
    // Recompiles the project's scripts (class registry and preview) after
    // they change on disk while not playing.
    void reload_project_scripts();

    // Logs a rejected edit (EditorDocument::error) to the console.
    void report(bool ok);
    // Opens the class picker; the chosen class becomes a child of `parent`.
    void request_add_child(NodeId parent);
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
    void save_current_scene();
    void handle_shortcuts();
    void update_title();
    void render_splash(ivec2 size, double now);
    void shutdown();

    EditorOptions options_;
    ConsoleLog console_;
    std::optional<render::FontAtlas> font_atlas_;
    ProjectEnv env_;
    EditorDocument doc_;
    Window window_;
    std::unique_ptr<rhi::Device> device_;
    render::SdfBatcher shapes_;
    render::TextRenderer text_;
    // Sprites (Canvas.vector) in the scene view and the game view.
    render::MeshBatcher meshes_;
    script::VectorCache vectors_{meshes_};
    render::FontHandle font_;
    ImGuiRenderer imgui_renderer_;
    bool imgui_ready_ = false;
    PanelRegistry panels_;
    ViewCamera camera_;
    bool frame_request_ = true;
    std::uint64_t preview_revision_ = ~std::uint64_t{0};
    bool running_ = true;
    std::uint64_t frames_ = 0;
    std::string title_;

    // 8.1: the brand splash over the first moments (or held with --splash).
    enum class SplashState { Off, Timed, Held };
    Splash splash_;
    SplashState splash_state_ = SplashState::Off;
    double splash_start_ = 0.0;

    // 6B: assets, input map, play-in-editor.
    AssetLibrary assets_;
    InputMapDoc input_map_;
    PlayController play_;
    DeviceState devices_;
    bool game_input_ = false;
    rhi::Rect game_rect_{};
    vec2 last_pointer_{0.0f, 0.0f};
    double last_time_ = 0.0;
    double last_poll_ = 0.0;
    bool panic_seen_ = false;
    std::string reveal_asset_;
    std::string pending_open_;

    // 8.3 state.
    Tool tool_ = Tool::Move;
    bool snap_ = false;
    ActionRegistry actions_;
    std::size_t actions_classes_ = 0; // the class count the "Add Node" actions were built for
    MessageCenter messages_;
    LayoutStore layouts_;
    Layout layout_;                   // the window's docks and panels right now
    RecentProjects recents_;
    bool palette_open_ = false;
    bool palette_request_ = false;
    std::string palette_query_;
    int palette_index_ = 0;
    bool switch_requested_ = false;
    std::string switch_target_;
    std::string layout_name_input_;
    double clock_ = 0.0;
    LicenseView license_view_;
    bool discard_switch_ = false;     // the discard dialog was opened by a project switch
    // Docking (ImGui docking branch): a layout to apply at the next draw_docks.
    std::string pending_dock_;        // ImGui ini text to load
    bool rebuild_dock_ = false;       // rebuild the default arrangement from the fractions
    bool dock_built_ = false;

    // Dialog state.
    enum class Dialog { None, NewScene, Open, SaveAs, AddChild, DiscardChanges, License, SaveLayout };
    Dialog dialog_ = Dialog::None;
    bool dialog_opened_ = false;
    std::string path_input_;
    std::string class_filter_;
    std::string new_root_class_ = "Node2D";
    NodeId add_parent_ = 0;
};

} // namespace cuervo::editor

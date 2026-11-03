#pragma once
// The RmlUi editor shell (DESIGN.md 4.13, mockup EB1): menus, rail, sidebar panes, center card, drawer
// strip, splitters and the dialogs, all driven through EditorController. No logic of its own beyond
// layout and presentation; every edit goes through the controller or the document.

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cuervo/brand/brand.hpp"
#include "cuervo/input/devices.hpp"
#include "editor/class_catalog.hpp"
#include "editor/console_log.hpp"
#include "editor/editor_controller.hpp"
#include "editor/inspector_model.hpp"
#include "editor/projects.hpp"
#include "editor/particle_preview.hpp"
#include "editor/sprite_view.hpp"
#include "editor/timeline.hpp"
#include "cuervo/script/tier_check.hpp"
#include "pluma/heap.hpp"
#include "editor/tree_view.hpp"

namespace cuervo::editor {

class ScriptPanel; // editor/rml/script (script editor workspace)

// What the window side provides.
struct ShellHost {
    std::function<void(brand::Theme)> set_theme;
    std::function<void(float)> set_scale;
    std::function<brand::Theme()> system_theme;
    std::function<void()> quit;
    std::function<void()> frame_scene;
    std::function<float()> zoom_percent;
    std::function<std::string()> view_readout; // rotate/scale drag value, empty otherwise
    std::function<void(float)> zoom_by;   // factor; 0 resets to 100%
    std::function<std::optional<vec2>(float, float)> world_at; // window px -> world, only inside the scene view (and not while playing)
    SDL_Window* window = nullptr;         // parent of the native file dialogs
    std::function<vec2()> view_center;    // world point at the middle of the scene view (where a new positioned node appears)
};

std::string rml_escape(std::string_view v);
// Registers the <particles> element; call once after Rml::Initialise.
void register_particle_view();
// Registers the <plot> element (profiler graphs).
void register_plot_view();
void register_key_view();

// "Ctrl+Shift+S" for a key event (the strings Action::shortcut uses); empty for a bare modifier.
std::string shortcut_string(const SDL_Event& ev);

class Shell final : public Rml::EventListener {
public:
    Shell(EditorController& ctl, ShellHost host, bool persist);

    void bind(Rml::Context* ctx, Rml::ElementDocument* page);
    // Once per frame: rebuilds what changed, handles controller prompts, applies play-mode classes.
    void refresh();
    void ProcessEvent(Rml::Event& e) override;

    // True while a menu, popup or dialog is open: the scene must not see the pointer.
    // True while something in the UI animates by itself (particle preview, timeline playback): keep drawing.
    bool animating() const;
    // The clip pose to show in the scene view (timeline "Pose" on), and a one-shot flag when it just went off.
    struct AnimPose { NodeId player; std::string clip; int tick; };
    std::optional<AnimPose> anim_pose() const;
    void stop_pose() { pose_on_ = false; pose_dirty_ = true; }
    bool take_pose_dirty() { return std::exchange(pose_dirty_, false); }
    bool ui_modal() const { return popup_open_ || dialog_ != Dialog::None || !im_capture_.empty() || welcome_shown_; }
    // Input-map "Bind": while true the event loop feeds raw device events to capture_devices() and routes nothing else.
    bool capturing_binding() const { return !im_capture_.empty(); }
    DeviceState& capture_devices() { return capture_dev_; }
    // Runs the action bound to a key press (not while typing); true when one ran.
    bool handle_shortcut(const SDL_Event& ev);
    // Tool letters Q W E R B when the scene has the focus.
    bool handle_tool_key(const SDL_Event& ev);
    // Script editor hooks (editor/rml/script): open a script in the Script workspace; keys that work in Script mode.
    void open_script(const std::filesystem::path& file, int line = 0);
    bool script_key(const SDL_Event& ev);
    bool script_wants_frames() const;
    void register_script_actions(); // palette commands for Script mode (script_glue.cpp)
    // Escape closes the open dialog, picker or menu (text fields swallow the key before the DOM sees it); true when it did.
    bool handle_escape();

    const RmlLayout& layout() const { return layout_; }
    void set_layout(const RmlLayout& l);
    // The center card showing the scene (not the Script placeholder).
    bool scene_visible() const { return layout_.center == "scene"; }
    // Tick counter for status; the play devices (the shell's Step action runs with empty input).
    bool playing() const { return ctl_.play().active(); }
    // Pointer in the game's virtual pixels etc. live in rml_ui.cpp; this is the Step action's input.
    DeviceState& idle_devices() { return idle_; }

    // Start-up switches shared with the ImGui editor: --tabs (rail panes and drawer tabs by title),
    // --layout (a named layout), --screen welcome (the welcome overlay even with a project), --palette [query].
    struct StartOptions {
        std::vector<std::string> tabs;
        std::string layout;
        bool welcome = false;
        std::optional<std::string> palette;
    };
    void apply_start(const StartOptions& o);

    // Test hooks and the persisted state.
    void save_layout();
    // The project that is open now (recent list; call after every successful open).
    void note_project(const std::filesystem::path& dir);
    // Test hooks for the pickers and dialogs.
    bool picker_open() const { return picker_; }
    const std::vector<std::string>& picker_names() const { return picker_names_; }
    std::string last_toast() const { return toast_; }

private:
    enum class Dialog { None, SaveAs, DiscardNew, DiscardQuit, DiscardOpen, DiscardLeave, OpenScene, AttachScript, OpenProject, AssetFolder, AssetRename, AssetDelete, AssetImport, NewProject, License };

    void register_actions();
    void apply_layout();
    void reset_panel_layout();
    void save_named_layout(const std::string& name);
    bool load_named_layout(const std::string& name);
    void set_class_if(Rml::Element* e, const char* cls, bool on);
    Rml::Element* by_id(const char* id) const { return page_->GetElementById(id); }

    void open_menu(const std::string& name, Rml::Element* anchor);
    // Native file dialogs (SDL3; rml_pick.cpp). They answer on another thread; refresh() applies the result.
    // Without a dialog implementation (headless) the user is told to type the path, which every dialog still accepts.
    enum class PickKind { ProjectFolder, ImportFile, SaveScene, OpenScene };
    void native_pick(PickKind kind);
    void poll_native_pick();
    void open_palette(const std::string& query);
    void build_palette(const std::string& query);
    void close_popup();
    void open_dialog(Dialog d);
    void close_dialog();
    void dialog_confirm();

    // Add Child picker (rml_dialogs.cpp)
    void open_picker(NodeId parent);
    void build_picker();
    void close_picker();
    void pick_class(const std::string& name);
    bool picker_key(Rml::Input::KeyIdentifier key, bool ctrl);
    // Attach script dialog (rml_dialogs.cpp)
    void build_attach();
    void build_attach_list();
    void attach_confirm();
    // Open project dialog (rml_dialogs.cpp)
    void build_open_project();
    void open_project_confirm(const std::string& path);

    void persist_tree();
    void build_tree();
    void build_tree_window();
    void tree_scroll_to(NodeId id);
    void begin_rename(NodeId id);
    void end_rename(bool commit);
    void tree_select(NodeId id, bool toggle, bool range);
    void open_tree_menu(NodeId id, Rml::Vector2f at);
    void run_tree_menu(const std::string& key);
    void tree_drag_update(Rml::Element* over, Rml::Vector2f at);
    void tree_drag_end(bool drop);
    void tree_key(Rml::Input::KeyIdentifier key);
    void build_inspector();
    std::string field_group(const std::string& title, const std::vector<std::string>& fields);
    // Drawer tabs that follow the selection (rml_drawer.cpp)
    void build_node_drawer();
    void build_debugger_drawer();
    bool drawer_click(Rml::Element* t);
    void build_sprite_drawer(const ModelNode* n);
    void sprite_apply();
    bool sprite_mousedown(Rml::Element* t, Rml::Vector2f at, int button);
    void sprite_mousemove(Rml::Vector2f at);
    void sprite_mouseup() { sprite_pan_ = false; }
    void sprite_scroll(Rml::Element* t, Rml::Vector2f at, float dy);
    bool sprite_click(Rml::Element* t);
    bool sprite_change(Rml::Element* t, const std::string& value, bool enter);
    void build_tiles_drawer(const ModelNode& n);
    void timeline_head();
    void step_timeline();
    int lane_tick(float x) const;
    bool timeline_mousedown(Rml::Element* t, Rml::Vector2f at, int button);
    void timeline_mousemove(Rml::Vector2f at);
    void timeline_mouseup();
    bool timeline_click(Rml::Element* t);
    bool timeline_change(Rml::Element* t, const std::string& value, bool enter);
    void build_particle_drawer();
    void bind_particles();
    void step_particles();
    bool particles_click(Rml::Element* t);
    void build_animation_drawer(const ModelNode& n);
    void build_script_drawer(const ModelNode& n);
    void build_debugger_body(std::string& out);
    void build_live_tree(std::string& out, script::GameSession& session);
    bool with_target(const std::string& key, const std::function<void(scene::Value&)>& fn, bool merge);
    scene::Value target_value(const std::string& key) const;
    void commit_edits();
    void cancel_edits();
    void apply_text(const std::string& key, const std::string& text);
    bool inspector_mousedown(Rml::Element* t, Rml::Vector2f at);
    void inspector_mousemove(Rml::Vector2f at);
    void inspector_mouseup();
    bool inspector_click(Rml::Element* t);
    bool inspector_change(Rml::Element* t, const std::string& value, bool enter);
    void open_color_picker(const std::string& key, Rml::Element* anchor);
    void cp_refresh(bool hex);
    void cp_apply(bool merge);
    void cp_drag_to(Rml::Vector2f at);
    bool cp_mousedown(Rml::Element* t, Rml::Vector2f at);
    bool cp_mouseup();
    bool cp_change(Rml::Element* t, const std::string& value, bool enter);
    void build_assets();
    void build_asset_preview();
    std::string asset_thumb_src(const std::string& path, int px) const;
    bool asset_open(int depth, const AssetEntry& e) const;
    bool asset_matches(const AssetEntry& e, const std::string& q) const;
    std::string asset_selected_folder() const;
    void assets_filter(const std::string& value);
    void asset_activate(const std::string& path);
    void play_replay(const std::string& path);
    std::string entry_dialog_html(Dialog d);
    void open_new_project();
    bool new_project_confirm();
    bool entry_click(Rml::Element* t);
    bool entry_change(Rml::Element* t, const std::string& value, bool enter);
    // Welcome screen (no project open): rml_welcome.cpp.
    void refresh_welcome();
    void build_welcome();
    bool welcome_click(Rml::Element* t);
    void welcome_open(const std::string& path);
public:
    // Files dropped on the window (SDL drop events collected by the loop).
    void import_dropped(const std::vector<std::filesystem::path>& files);
private:
    bool assets_mousedown(Rml::Element* t, Rml::Vector2f at, int button);
    void assets_mousemove(Rml::Element* t, Rml::Vector2f at);
    void assets_mouseup(Rml::Element* t, Rml::Vector2f at);
    void open_asset_menu(const std::string& path, Rml::Vector2f at);
    void run_asset_menu(const std::string& key);
    std::string asset_dialog_html(Dialog d);
    bool asset_dialog_confirm(Dialog d);
    void build_settings();
    void build_search(const std::string& query);
    void init_script_panel();
    void refresh_script();
    void script_diagnostics(bool refs = false);
    bool script_debug_drawer();           // builds the script debugger view when one is active; false: show nodes
    std::string script_debug_chips() const;
    void build_drawer();
    void build_console();
    void console_copy();
    void set_drawer_head(const std::string& html);
    void build_history();
    void build_replay();
    void replay_seek(std::uint64_t tick);
    bool replay_click(Rml::Element* t);
    bool replay_mousedown(Rml::Element* t, Rml::Vector2f at);
    void replay_mousemove(Rml::Vector2f at);
    void replay_mouseup();
    void replay_scrub_to(float x);
    void run_tier();
    void build_tiers();
    bool tier_click(Rml::Element* t);
    void open_location(const std::string& file, int line);
    void build_inputmap();
    bool inputmap_click(Rml::Element* t);
    bool inputmap_change(Rml::Element* t, const std::string& value, bool enter);
    void poll_capture();
    bool drawer_input_busy() const;
    void build_profiler();
    bool profiler_click(Rml::Element* t);
    void refresh_toolbar();
    void refresh_strip();
    void refresh_status();

    void click(Rml::Element* t);
    void begin_drag(Rml::Element* split, Rml::Vector2f at);
    void drag_to(Rml::Vector2f at);
    void end_drag();
    void set_drawer(const std::string& which);
    void show_sprite_asset(const std::string& path);
    void show_rail(const std::string& rail, bool toggle);
    bool menu_checked(std::string_view id) const;
    void hide_tip();
    void tip_over(Rml::Element* t);
    void update_tip();

    std::shared_ptr<ScriptPanel> script_;
    bool script_entered_ = false;
    EditorController& ctl_;
    ShellHost host_;
    bool persist_ = true;
    RmlLayout layout_;
    Rml::Context* ctx_ = nullptr;
    Rml::ElementDocument* page_ = nullptr;
    DeviceState idle_;

    std::uint64_t revision_ = ~0ull;
    NodeId selected_ = 0;
    std::uint64_t assets_rev_ = ~0ull;
    std::uint64_t seen_ = 0;
    std::vector<ConsoleLine> lines_;
    std::uint32_t node_drawer_ms_ = 0;
    int node_drawer_play_ = -1;
    std::string tree_scene_;
    int console_mask_ = 7;
    int console_sel_ = -1;
    bool console_dirty_ = true;
    std::uint64_t history_rev_ = ~0ull;
    std::string node_tab_;           // title of the selection's own drawer tab, empty: none
    std::string search_query_;

    bool popup_open_ = false;
    bool picker_ = false;
    NodeId picker_parent_ = 0;
    std::string picker_query_;
    int picker_sel_ = 0;
    std::vector<std::string> picker_names_;
    NodeId attach_node_ = 0;
    int attach_tab_ = 0;   // 0 existing, 1 create
    std::string attach_filter_, attach_sel_, attach_error_, new_class_, new_path_;
    std::vector<std::pair<std::string, std::string>> attach_shown_;
    RecentProjects recents_;
    std::string project_text_, project_error_;
    bool palette_ = false;
    std::vector<std::string> palette_ids_;
    Dialog dialog_ = Dialog::None;
    std::string dialog_text_;
    std::string toast_, title_;

    struct Scrub {
        std::string key;
        scene::Value start;
        Rml::Vector2f press, last;
        Rml::Element* el = nullptr;
        double acc = 0.0;
        bool down = false, active = false;
    };
    Scrub scrub_;
    std::string assets_filter_, assets_sel_, asset_press_, asset_last_click_, asset_ctx_, asset_op_path_, asset_error_;
    std::map<std::string, bool> assets_open_;
    Rml::Vector2f asset_press_at_;
    std::uint32_t asset_last_ms_ = 0;
    bool asset_dragging_ = false;
    Rml::Element* asset_drop_el_ = nullptr;
    bool cp_open_ = false;
    int cp_drag_ = 0; // 0 none, 1 saturation/value, 2 hue, 3 alpha
    std::string cp_key_;
    scene::Value cp_start_;
    ve::Hsva cp_;
    bool inspector_busy_ = false;
    std::string edit_key_, edit_text_, dd_key_, filter_;
    std::vector<ve::Section> sections_;
    TreeState tree_;
    std::vector<TreeRow> rows_;
    RowWindow built_{~std::size_t{0}, 0};
    float tree_scroll_ = -1.0f;
    NodeId anchor_ = 0, rename_ = 0, press_node_ = 0, ctx_node_ = 0;
    Rml::Vector2f press_at_;
    bool tree_dragging_ = false, tree_focus_ = false;
    NodeId last_click_node_ = 0;
    std::uint32_t last_click_ms_ = 0;
    DropTarget drop_;
    Rml::Element* drop_row_ = nullptr;

    EmitterPreview emitter_;
    EmitterParams emitter_params_;
    NodeId emitter_node_ = 0;
    bool particles_play_ = true, timeline_play_ = false;
    std::uint32_t particles_ms_ = 0;
    float particles_idle_ = 0.0f;
    std::string particles_count_text_ = "Live preview of the selected emitter";
    std::string rp_path_ = "replays/last.replay", rp_key_;
    bool rp_dirty_ = true, rp_scrub_ = false;
    std::uint64_t rp_pos_ = 0;
    int tier_target_ = 0;
    bool tier_ran_ = false;
    std::optional<script::TierCheckReport> tier_report_;
    std::string tier_error_, tier_text_;
    std::string im_new_, im_error_, im_rename_, im_rename_text_, im_capture_, im_axis_menu_;
    int im_capture_frames_ = 0;
    DeviceState capture_dev_;
    std::string np_name_, np_dir_, np_error_, np_template_;
    bool np_notices_ = false;
    std::string layout_name_;
    bool welcome_shown_ = false;
    bool force_welcome_ = false; // --screen welcome: shown although a project is open, until another opens
    std::optional<std::string> start_palette_; // --palette: opened on the first refresh
    bool welcome_dirty_ = true;
    std::string welcome_path_, welcome_error_;
    pluma::HeapProfiler heap_;
    std::set<std::uintptr_t> live_open_, live_closed_;
    std::uintptr_t live_sel_ = 0, live_root_ = 0;
    std::vector<float> prof_frame_, prof_sim_;
    std::string prof_saved_;
    std::uint32_t prof_ms_ = 0;
    bool prof_built_ = false;
    Timeline tl_;
    bool pose_on_ = false, pose_dirty_ = false, tl_new_open_ = false, tl_drag_ = false, tl_scrub_ = false;
    std::string tl_new_name_, tl_new_track_, tl_new_kind_ = "Property", tl_error_;
    std::uint32_t tl_ms_ = 0;
    float tl_acc_ = 0.0f;
    SpriteView sprite_;
    bool sprite_pan_ = false, sprite_fit_ = false;
    Rml::Vector2f sprite_last_;
    std::string sprite_msg_, sprite_asset_;
    NodeId sprite_node_ = 0;
    std::string sprite_pick_; // asset sent to the Sprites tab from Assets (empty: follow the selected node)

    Rml::Element* tip_target_ = nullptr;
    std::uint32_t tip_since_ = 0;
    bool tip_shown_ = false;

    Rml::Element* drag_ = nullptr;
    Rml::Vector2f drag_start_;
    int drag_size_ = 0; // dp at the start
    bool layout_dirty_ = false;
};

} // namespace cuervo::editor

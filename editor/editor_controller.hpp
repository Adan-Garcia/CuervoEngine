#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/actions.hpp"
#include "editor/asset_library.hpp"
#include "editor/console_log.hpp"
#include "editor/editor_document.hpp"
#include "editor/input_map_doc.hpp"
#include "editor/messages.hpp"
#include "editor/play_controller.hpp"
#include "editor/project_env.hpp"
#include "editor/scene_preview.hpp"
#include "editor/settings.hpp"

namespace cuervo::editor {

// The scene view's transform tool (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): Q, W, E, R, B.
enum class Tool { Select, Move, Rotate, Scale, Paint };
const char* tool_name(Tool tool);

// A question the user has to answer before the controller continues. The controller only says
// what is needed; each UI (the ImGui dialogs, the RmlUi popups) shows it its own way.
enum class Prompt {
    None,
    SaveAs,         // the scene has no path yet
    DiscardOpen,    // unsaved changes; `path` is the scene to open afterwards
    DiscardLeave,   // unsaved changes; the project is being left (`switch_target`)
    AddChild,       // choose a class for a child of `node`
    AttachScript,   // choose or create a script for `node`
};
struct PendingPrompt {
    Prompt kind = Prompt::None;
    NodeId node = 0;
    std::string path;
};

// The UI-free editor session (DESIGN.md 4.13): the open project, scene document, assets, input map,
// play session, actions, messages, tools, and the operations on them. No window, device, SDL or
// ImGui here; both editor UIs call it and the window side (EditorApp) is a client like any other.
class EditorController {
public:
    // What the window side wants to hear about.
    struct Hooks {
        std::function<void()> scene_opened;      // frame the scene view
        std::function<void()> scripts_reloaded;  // rebuild previews and the class-dependent actions
        std::function<void()> persist;           // write user state (settings, layout)
    };

    EditorController() = default;
    EditorController(const EditorController&) = delete;
    EditorController& operator=(const EditorController&) = delete;

    Hooks hooks;

    ConsoleLog& console() { return console_; }
    const ConsoleLog& console() const { return console_; }
    ProjectEnv& env() { return env_; }
    const ProjectEnv& env() const { return env_; }
    EditorDocument& doc() { return doc_; }
    const EditorDocument& doc() const { return doc_; }
    AssetLibrary& assets() { return assets_; }
    const AssetLibrary& assets() const { return assets_; }
    InputMapDoc& input_map() { return input_map_; }
    const InputMapDoc& input_map() const { return input_map_; }
    PlayController& play() { return play_; }
    const PlayController& play() const { return play_; }
    ActionRegistry& actions() { return actions_; }
    const ActionRegistry& actions() const { return actions_; }
    MessageCenter& messages() { return messages_; }
    const MessageCenter& messages() const { return messages_; }
    EditorSettings& settings() { return settings_; }
    const EditorSettings& settings() const { return settings_; }

    // The font atlas scripts are compiled against (null: none).
    void set_font_atlas(const render::FontAtlas* font) { font_ = font; }
    // The app's clock in seconds, used to time messages.
    void set_clock(double seconds) { clock_ = seconds; }
    double clock() const { return clock_; }

    // ---- tools and view toggles
    Tool tool() const { return tool_; }
    void set_tool(Tool tool) { tool_ = tool; }
    int paint_tile_id() const { return paint_tile_id_; }
    void set_paint_tile_id(int id) { paint_tile_id_ = id; }
    bool snap() const { return snap_; }
    void set_snap(bool snap) { snap_ = snap; }
    bool collision_shapes() const { return collision_shapes_; }
    void set_collision_shapes(bool enabled) { collision_shapes_ = enabled; }
    bool preview_lighting() const { return preview_lighting_; }
    void set_preview_lighting(bool on) { preview_lighting_ = on; }

    // ---- play
    // What a play session needs from the window side (the command line).
    struct PlaySetup {
        std::string font_path;
        unsigned jobs = 0;
        bool audio = false;
    };
    void set_play_setup(PlaySetup setup) { play_setup_ = std::move(setup); }
    script::SessionOptions play_options(bool current_scene) const;
    // "Scene preview (run ready())": the open scene run once in a session the scene view draws instead
    // of the static preview. Restarts 0.3 s after an edit or a script reload settles (it recompiles the
    // project, so not on every frame of a drag). update_ready_preview() runs once per frame while editing.
    bool ready_preview_on() const { return ready_on_; }
    void toggle_ready_preview();
    void invalidate_ready_preview() { ready_revision_ = ~std::uint64_t{0}; }
    void update_ready_preview();
    ScenePreview& ready_preview() { return ready_; }
    // Starts a live session (the open scene when `current_scene`, else the project's main), optionally
    // recording; a failure is logged. Returns whether a session is running.
    bool start_play(bool current_scene, bool record = false);
    void stop_play() { play_.stop(); }
    // Copies files into the project (file_import.hpp: images, SVG, sound, fonts), rescans Assets and reports.
    // A drop or the Import dialog; `first_dest` is the project path of the first imported file ("" when none).
    struct ImportSummary { int imported = 0; std::string first_dest, first_image, error; };
    ImportSummary import_files(const std::vector<std::filesystem::path>& files);
    // Plays a replay file (project-relative or absolute) in the view; a failure is logged. Returns whether it runs.
    bool start_replay(const std::string& path);

    // ---- messages
    void notify(UserMessage message);
    void notify_ok(std::string text);
    // Reports a failed document operation: a message plus a log line.
    void report_failure(const std::string& raw);
    // Logs a rejected edit (EditorDocument::error) to the console.
    void report(bool ok);
    // Seals undo groups around the action and reports a failure.
    bool run_action(std::string_view id);

    // ---- scenes and projects
    // Opens a scene; asks first (Prompt::DiscardOpen) when the open one has unsaved changes.
    void request_open_scene(const std::string& path);
    // An asset dropped on the scene view: a .scene is instanced under the selection (or the root) at
    // `at` (rounded to whole units); any other kind reports why not. Shared by both editor UIs.
    bool drop_asset_on_scene(const std::string& path, std::optional<vec2> at);
    // Instances a scene asset under the selection (the asset browser's context menu).
    bool instance_scene_asset(const std::string& path, std::optional<vec2> at = std::nullopt);
    // Saves the open scene; asks for a path (Prompt::SaveAs) when it has none.
    void save_current_scene();
    // Leaves this project (empty path: the welcome screen); asks first when there are unsaved changes.
    void request_switch_project(std::string path = {});
    bool switch_requested() const { return switch_requested_; }
    const std::string& switch_target() const { return switch_target_; }
    // The UI has acted on the request (opened the target in place); clears it.
    void acknowledge_switch() { switch_requested_ = false; switch_target_.clear(); }
    // The answer to DiscardOpen / DiscardLeave: go ahead (discarding the changes) or cancel.
    void confirm_discard();
    void cancel_discard();
    // Pending question for the UI; take_prompt() hands it over once.
    void request_add_child(NodeId parent);
    void request_attach_script(NodeId node);
    const PendingPrompt& prompt() const { return prompt_; }
    PendingPrompt take_prompt() { return std::exchange(prompt_, {}); }
    // The path a DiscardOpen / DiscardLeave question is about (set until answered).
    const std::string& discard_path() const { return discard_path_; }
    bool discard_is_leave() const { return discard_leave_; }

    // Opens a script, document, or directory in the default code editor of the device.
    bool open_in_external_editor(const std::filesystem::path& path, int line = 0);

    // ---- scripts
    // Recompiles the project's scripts (class registry and preview) after they change on disk.
    void reload_project_scripts();
    // Searches project scripts for `class <class_name>`. Returns { relative_file_path, line_number }.
    std::pair<std::string, int> find_class_script(const std::string& class_name) const;
    bool is_script_class(const std::string& class_name) const;
    bool has_script_bound(NodeId id) const;
    std::pair<std::string, int> node_script(NodeId id) const;
    std::vector<std::pair<std::string, std::string>> project_script_classes() const;
    std::vector<std::string> find_script_classes_in_file(const std::filesystem::path& file) const;
    bool attach_script_file(NodeId id, const std::filesystem::path& file, std::string& error);
    bool create_and_attach_script(NodeId id, const std::string& class_name, const std::filesystem::path& rel_path, std::string& error);
    bool detach_script(NodeId id);
    // Suggested class name and project-relative path for a new script on `node`.
    static std::pair<std::string, std::string> suggest_script(const std::string& node_name);

private:
    ConsoleLog console_;
    ProjectEnv env_;
    EditorDocument doc_;
    AssetLibrary assets_;
    InputMapDoc input_map_;
    PlayController play_;
    ActionRegistry actions_;
    MessageCenter messages_;
    EditorSettings settings_;
    const render::FontAtlas* font_ = nullptr;
    double clock_ = 0.0;
    ScenePreview ready_;
    bool ready_on_ = false;
    std::uint64_t ready_revision_ = ~std::uint64_t{0};
    double ready_changed_at_ = -1.0;
    PlaySetup play_setup_;

    Tool tool_ = Tool::Move;
    int paint_tile_id_ = 0;
    bool snap_ = false;
    bool collision_shapes_ = true;
    bool preview_lighting_ = true;

    PendingPrompt prompt_;
    std::string discard_path_;
    bool discard_leave_ = false;
    bool switch_requested_ = false;
    std::string switch_target_;
    mutable std::map<std::string, std::pair<std::string, int>> class_scripts_cache_;
};

} // namespace cuervo::editor

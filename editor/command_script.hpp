#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cuervo::editor {

class AssetLibrary;
class EditorDocument;
class InputMapDoc;
class PlayController;
class ProjectEnv;
class ScenePreview;

// Headless editing (`cuervo-editor --script-commands <file>`): a text file of
// editor operations, one per line, applied to the document exactly as the UI
// applies them (EditorDocument), so a level can be built, saved and checked
// in CI without a window. editor/README.md documents the format:
//
//   # comment
//   new <RootClass> [name]                 open <scene>
//   add <parent> <Class> [name]            remove <node>
//   rename <node> <name>                   duplicate <node>
//   reparent <node> <new parent> [index]   select <node>
//   set <node> <field> <value>             reset <node> <field>
//   move <node> <dx> <dy>                  undo | redo
//   save [path]                            expect <node> <field> <value>
//   instance <parent> <scene> [x y]
//   play [scene] | play-record [scene] | pause | resume | stop | step [n]
//   hold <action> | release <action> | expect-tick <n> | expect-hash <hex> | print-hash
//   preview | preview-clear | expect-preview <node path> <field> <value> |
//         expect-preview-tick <n> | expect-preview-nodes <n>   (ready() run, never ticked)
//   expect-state <state> | expect-panic <text> | expect-clean
//   append-file <path> <text> | reload     (edit a project file; force a hot reload)
//   record start|stop|save <path>          replay load|seek|verify|expect-mismatch
//   input add-action|remove-action|rename-action|bind|unbind|add-axis|
//         remove-axis|axis-source|remove-axis-source|undo|redo|save|expect ...
//   asset mkdir|rename|delete|expect|expect-missing ...
//   undock <panel> <x> <y> | save-layout <name> | expect-viewports <n> |
//         expect-viewport <panel> <x> <y>
//         (window steps, 11.7b: they need ImGui's live viewports, so they run in the editor
//         window opened after the script, one per few frames; see UiStep)
//
// Nodes are paths from the root ("/", "/enemies/turret"), or "." for the
// current selection (the node the previous command added or touched).
// Values use the scene text view's syntax (docs/SCENES.md): 12.5, 3,
// (100.0, 50.0), "text", #name, Variant or Enum.Variant, "ff4060" for a
// Color, Struct { field = value }, [a, b]. Words may be "quoted".
struct ScriptResult {
    bool ok = true;
    int line = 0;          // the failing line (1-based)
    std::string error;
    int commands = 0;      // commands applied
    bool saved = false;    // a `save` ran
};

// A step that needs the editor's window rather than the document: queued by the script runner and
// run by the app once the window is up (`undock` pulls a panel out into its own OS window at a
// screen position, `save-layout` stores the layout the window is in, `expect-viewports` checks how
// many OS windows ImGui has besides the main one, `expect-viewport` where one panel's window is).
struct UiStep {
    enum class Kind { Undock, SaveLayout, ExpectViewports, ExpectViewport };
    Kind kind = Kind::Undock;
    std::string name; // panel title, or layout name
    float x = 0.0f;
    float y = 0.0f;
    int count = 0;
};

// Everything a script can drive. Phase 6B's commands (play, record and
// replay, input map, assets; editor/README.md) need the matching member;
// without it they fail with "not available".
struct CommandContext {
    EditorDocument& doc;
    const ProjectEnv& env;
    PlayController* play = nullptr;
    InputMapDoc* input = nullptr;
    AssetLibrary* assets = nullptr;
    // The game font for play sessions (UI layout measures text with it, so
    // the runtime's state hash needs the same font), and job threads.
    std::string font_path;
    unsigned jobs = 0;
    ScenePreview* preview = nullptr; // `preview`, `expect-preview...`
    std::vector<UiStep>* ui_steps = nullptr; // `undock`, `save-layout`, `expect-viewports`
};

ScriptResult run_command_script(std::string_view text, CommandContext& context);
// Scene edits only (6A's commands).
ScriptResult run_command_script(std::string_view text, EditorDocument& doc, const ProjectEnv& env);

} // namespace cuervo::editor

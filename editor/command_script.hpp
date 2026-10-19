#pragma once

#include <string>
#include <string_view>

namespace cuervo::editor {

class AssetLibrary;
class EditorDocument;
class InputMapDoc;
class PlayController;
class ProjectEnv;

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
//   hold <action> | release <action> | expect-tick <n> | expect-hash <hex>
//   expect-state <state> | expect-panic <text> | expect-clean
//   record start|stop|save <path>          replay load|seek|verify|expect-mismatch
//   input add-action|remove-action|rename-action|bind|unbind|add-axis|
//         remove-axis|axis-source|remove-axis-source|undo|redo|save|expect ...
//   asset mkdir|rename|delete|expect|expect-missing ...
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
};

ScriptResult run_command_script(std::string_view text, CommandContext& context);
// Scene edits only (6A's commands).
ScriptResult run_command_script(std::string_view text, EditorDocument& doc, const ProjectEnv& env);

} // namespace cuervo::editor

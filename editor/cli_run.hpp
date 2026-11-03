#pragma once
// Command-line operations both editor UIs share (--script-commands, --replay-verify, --select):
// no window, device or UI library here, so the ImGui and RmlUi entries behave the same.
#include <string>
#include <vector>

namespace cuervo::editor {

class EditorController;
struct UiStep;

// --script-commands <file>: apply the script to the open scene, save it when it changed, log a
// summary. Returns EXIT_SUCCESS or EXIT_FAILURE (errors are logged with file:line).
int run_script_file(EditorController& ctl, const std::string& file, const std::string& font_path, unsigned jobs,
                    std::vector<UiStep>* ui_steps);

// --replay-verify <file>: play the replay back headless against `project`, exit status as above.
int run_replay_verify_file(const std::string& project, const std::string& replay_file, const std::string& font_path, unsigned jobs);

// --select </a,/b>: select those nodes (unknown paths are warned about).
void select_paths(EditorController& ctl, const std::vector<std::string>& paths);

} // namespace cuervo::editor

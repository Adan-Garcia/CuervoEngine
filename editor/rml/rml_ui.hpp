#pragma once

#include "editor/editor_options.hpp"

namespace cuervo::editor {

// The RmlUi feasibility spike (`cuervo-editor --rml-ui`, AUDIT.md 14): a fixed-layout editor
// shell in RML/RCSS over the same EditorDocument. Returns the process exit code.
int run_rml_ui(const EditorOptions& options, bool continuous);

} // namespace cuervo::editor

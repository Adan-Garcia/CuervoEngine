#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Writing the input map back into project.toml (the editor's input-map
// editor, DESIGN.md 4.13), keeping the file as the user wrote it: only the
// `key = [...]` lines of [input.actions] and [input.axes] that changed are
// rewritten (their trailing comments kept), removed entries lose their line,
// new entries go after the last entry of their section (the section is
// appended if missing). Every other line -- comments, blank lines, other
// keys and sections -- stays byte for byte, and an unchanged map rewrites
// nothing.
namespace cuervo::script {

struct InputEntry {
    std::string name;                  // the action / axis name (a TOML bare key)
    std::vector<std::string> bindings; // "Key.Z", "Pad.South"... or axis sources "Keys.Arrows"
    // The name it has in the file (empty: new). Lets a rename keep its line.
    std::string original;
};

struct InputSections {
    std::vector<InputEntry> actions;
    std::vector<InputEntry> axes;
};

// The [input.actions] / [input.axes] entries of project.toml text, in file
// order, each with `original` = its name.
InputSections read_input_sections(std::string_view toml);

// `toml` with the input sections replaced by `input` (see above).
std::string rewrite_input_sections(std::string_view toml, const InputSections& input);

// Reads `<dir>/project.toml`, rewrites its input sections and writes it back.
bool write_project_input(const std::filesystem::path& dir, const InputSections& input, std::string& error);

// A valid action/axis name: a TOML bare key the language can spell as a
// `#name` (letters, digits, `_`; not starting with a digit).
bool is_input_name(std::string_view name);

} // namespace cuervo::script

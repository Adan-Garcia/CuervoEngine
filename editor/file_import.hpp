#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "editor/asset_library.hpp"

// Importing files dropped on the editor window from the OS (DESIGN.md 4.13,
// milestone 11.7b): images (PNG, JPEG, QOI), SVG, sound (WAV, OGG, FLAC, MP3) and
// fonts (TTF, OTF) are copied into the project; anything else is refused with
// a reason. The window's drop event and the `import` command-script command run
// this same function, so the test needs no window.
//
// Without a folder the file goes to the project folder of its kind (`art`,
// `sounds`, `fonts`). A name that exists gets `_2`, `_3`... (a drop never
// overwrites). Images are decoded after the copy and removed again if they are
// not valid images; an SVG must contain an <svg> element.
namespace cuervo::editor {

struct ImportResult {
    std::string source; // as given
    std::string dest;   // project-relative path of the copy ("" if refused)
    AssetKind kind = AssetKind::Other;
    bool ok = false;
    std::string error;
};

// The default folder for a kind ("" for kinds that can't be imported).
std::string_view import_folder(AssetKind kind);

std::vector<ImportResult> import_files(const std::filesystem::path& project_dir,
                                       const std::vector<std::filesystem::path>& sources,
                                       std::string_view folder = {});

// Largest file a drop may bring in.
inline constexpr std::uintmax_t max_import_bytes = 256ull * 1024 * 1024;

} // namespace cuervo::editor

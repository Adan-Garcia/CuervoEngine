#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/assets/pack.hpp"

// What an exported pack carries besides the project's own files
// (docs/EXPORT.md "Pack contents"). Engine entries live under `.cuervo/`, a
// hidden directory, so they never collide with project files (hidden files
// are never packed) and never show up in Vfs::list.
//
//   .cuervo/export.toml    this manifest
//   .cuervo/default.efont  the game font (Canvas.text and UI.measure)
//   .cuervo/scripts.so     AOT-compiled native scripts (ScriptForm::Native)
//
// The manifest is a few `key = "value"` lines, written in a fixed order with
// no timestamps, so packing stays byte-deterministic.
namespace cuervo::assets {

inline constexpr std::string_view manifest_path = ".cuervo/export.toml";
inline constexpr std::string_view default_font_entry = ".cuervo/default.efont";
inline constexpr std::string_view native_scripts_entry = ".cuervo/scripts.so";
inline constexpr int manifest_format = 1;

// How the game's scripts ship. The release pipeline (docs/EXPORT.md
// "Pipeline") is staged so Phase 7B can add AOT-to-C as a step:
//   Source   scripts/**/*.plm, compiled by the runtime's VM at startup (now)
//   Bytecode precompiled VM bytecode (needs bytecode serialization in pluma/)
//   Native   compiled to C and linked into the runtime (7B, DESIGN.md 5.12)
enum class ScriptForm { Source, Bytecode, Native };
std::string_view script_form_name(ScriptForm f);
std::optional<ScriptForm> parse_script_form(std::string_view text);

struct ExportManifest {
    int format = manifest_format;
    std::string game;    // project.toml `name`
    std::string engine;  // engine version that packed it
    PackPlatform platform = PackPlatform::Any;
    ScriptForm scripts = ScriptForm::Source;
    bool stripped = false;
    std::vector<std::string> features;
    std::vector<std::string> stripped_features;
};

std::string write_manifest(const ExportManifest& m);
// False + `error` on a malformed or newer manifest. Unknown keys are ignored
// (an older runtime reading a newer manifest of the same format).
std::optional<ExportManifest> parse_manifest(std::string_view text, std::string& error);

} // namespace cuervo::assets

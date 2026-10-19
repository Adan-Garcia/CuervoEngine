#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cuervo/render/font.hpp"

// Font import (DESIGN.md 4.11, 7.2): TTF/OTF -> MTSDF FontAtlas with
// FreeType + msdfgen + msdf-atlas-gen. Built only with CUERVO_FONT_IMPORT
// (the default); shipping builds load .efont files instead.
//
// Shaping: glyphs are laid out per codepoint with the font's pair kerning
// (`kern` table). HarfBuzz shaping (GPOS kerning, ligatures, complex scripts)
// is not part of this step yet.
namespace cuervo::render {

struct FontImportOptions {
    // Inclusive codepoint ranges to import. Default: printable ASCII + Latin-1.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges{{0x20, 0x7E}, {0xA0, 0xFF}};
    // Atlas texels per em. 48 keeps corners crisp at any magnification while a
    // Latin-1 atlas stays around 512x512.
    float em_size = 48.0f;
    // Width of the distance field across an edge, in atlas texels. Bounds how
    // far the field reaches outside a glyph (outlines/glow) and how small text
    // can get before edges alias.
    float distance_range = 8.0f;
    int spacing = 1;   // empty texels between packed glyphs
    int threads = 0;   // generator threads; 0 = hardware concurrency
};

// True if this build can import TTF/OTF (CUERVO_FONT_IMPORT).
bool font_import_available();

// Imports a TTF/OTF font from memory. Logs and returns nullopt on failure
// (including when font import is compiled out).
std::optional<FontAtlas> import_font(std::span<const std::byte> font_data, std::string_view name,
                                     const FontImportOptions& options = {});

// True if `data` looks like a TrueType/OpenType font (sfnt or collection header).
bool is_sfnt_font(std::span<const std::byte> data);

// Loads a .efont file, or (when font import is available) imports a TTF/OTF
// file, detected by content.
std::optional<FontAtlas> load_font_file(const std::string& path, const FontImportOptions& options = {});
// Same, from bytes already in memory (a pack entry, assets::Vfs); `name`
// labels messages.
std::optional<FontAtlas> load_font_data(std::span<const std::byte> data, std::string_view name,
                                        const FontImportOptions& options = {});

} // namespace cuervo::render

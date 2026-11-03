#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cuervo/text/shaping.hpp"

// MSDF font atlases (DESIGN.md 4.4 "Text", 7.2 "Fonts"): the engine's runtime
// font format. A TTF/OTF font is imported once (font_import.hpp) into an
// MTSDF atlas plus glyph metrics and kerning; shipping builds load the binary
// form (.efont) and never touch FreeType.
//
// Units: glyph and font metrics are in ems (1 = the font size); the atlas is
// in texels with a top-left origin. Glyph quads are y-DOWN relative to the pen
// position on the baseline, matching the engine's coordinates (DESIGN.md 7.1).
namespace cuervo::render {

struct FontGlyph {
    std::uint32_t codepoint = 0; // 0 for glyphs reachable only through shaping (FontAtlas::extra_glyphs)
    // The font's glyph index (what HarfBuzz reports); meaningful when
    // FontAtlas::has_glyph_ids. Shaped text draws by glyph id, not codepoint.
    std::uint32_t glyph_id = 0;
    float advance = 0.0f; // ems
    // Quad around the glyph (plus the distance field margin), ems, relative to
    // the pen on the baseline, y down: top < 0 < bottom for most letters.
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    // The same quad's area in the atlas, texels, top-left origin.
    float atlas_left = 0.0f, atlas_top = 0.0f, atlas_right = 0.0f, atlas_bottom = 0.0f;

    bool has_quad() const { return right > left && bottom > top; } // false for whitespace
};

// Added to `first`'s advance when `second` follows it, ems.
struct KerningPair {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    float adjust = 0.0f;
};

struct FontMetrics {
    float ascender = 0.0f;  // ems above the baseline (positive)
    float descender = 0.0f; // ems below the baseline (negative, as in the font)
    float line_height = 0.0f; // baseline-to-baseline distance, ems
    float underline_y = 0.0f; // ems, y up like the font (negative = below the baseline)
    float underline_thickness = 0.0f;
};

struct FontAtlas {
    std::string name;
    FontMetrics metrics;
    float em_size = 0.0f;        // atlas texels per em
    float distance_range = 0.0f; // distance field range across the edge, atlas texels
    std::int32_t width = 0;
    std::int32_t height = 0;
    // RGBA8 MTSDF: rgb = multi-channel signed distance (median = edge at 0.5),
    // a = true signed distance. Top row first.
    std::vector<std::uint8_t> pixels;
    std::vector<FontGlyph> glyphs;     // sorted by codepoint, unique
    std::vector<KerningPair> kerning;  // sorted by (first, second), unique

    // Shaping support (version 2). Glyphs that no codepoint maps to (contextual
    // Arabic forms, ligatures) live here, sorted by glyph_id, codepoint 0.
    bool has_glyph_ids = false;
    std::vector<FontGlyph> extra_glyphs;
    // The source sfnt font, kept only when import asked for it: HarfBuzz needs
    // it at run time to shape. Empty = per-codepoint layout.
    std::vector<std::byte> shaping_data;
    // HarfBuzz face made from shaping_data by prepare_shaping(); null when
    // shaping is unavailable. Shared between copies of the atlas.
    std::shared_ptr<const text::ShapingFont> shaper;
    // Indices (into glyphs, then extra_glyphs) sorted by glyph id; see build_glyph_index().
    std::vector<std::uint32_t> by_id;

    const FontGlyph* find(std::uint32_t codepoint) const;
    // The glyph with this font glyph index (null without has_glyph_ids).
    const FontGlyph* find_by_id(std::uint32_t glyph_id) const;
    void build_glyph_index();
    // build_glyph_index() plus the shaper; call after filling in or loading an atlas.
    void prepare_shaping();
    float kerning_between(std::uint32_t first, std::uint32_t second) const;
};

// Binary .efont layout (little-endian):
//   header  magic "EFNT", u32 format version, u32 flags (bit 0: glyph ids valid), u32 reserved
//   body    name, em_size, distance_range, metrics (5 f32), width, height,
//           glyphs (u32 count + 11 x 4 bytes each; the last u32 is the glyph id in
//           version 2, reserved before), kerning (u32 count + 12 bytes each),
//           [v2] extra glyphs (u32 count + the same record, codepoint 0),
//           pixels (u32 byte count + RGBA8 rows), [v2] shaping font (u32 byte count + sfnt)
// Same versioning rule as ImportedVector: every change bumps the version, old
// versions keep loading, newer ones are rejected.
inline constexpr std::uint32_t font_atlas_version = 2;
inline constexpr char font_atlas_magic[4] = {'E', 'F', 'N', 'T'};

std::vector<std::byte> serialize_font_atlas(const FontAtlas& font);
// Returns false (with `error`) on truncated, corrupt or newer-version data.
bool deserialize_font_atlas(std::span<const std::byte> data, FontAtlas& out, std::string* error = nullptr);
bool is_font_atlas(std::span<const std::byte> data);

// Checks the invariants the layout code relies on (sorted unique glyphs and
// kerning, pixel buffer size, sane metrics). Returns an empty string if valid.
std::string validate_font_atlas(const FontAtlas& font);

} // namespace cuervo::render

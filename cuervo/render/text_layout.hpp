#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "cuervo/math/vec.hpp"
#include "cuervo/render/font.hpp"

// Text layout (DESIGN.md 4.4 "Text"): UTF-8 -> positioned glyph quads, in
// layout space (world units, y down, origin at the anchor point chosen by the
// style). Per-codepoint layout with pair kerning, explicit line breaks, word
// wrapping at spaces and per-line horizontal alignment. No shaping, bidi or
// line breaking beyond spaces yet (DESIGN.md 7.2 lists HarfBuzz for that).
namespace cuervo::render {

// Decodes one UTF-8 sequence at `pos` and advances past it. Malformed input
// (bad lead/continuation bytes, overlong forms, surrogates, > U+10FFFF,
// truncation) yields U+FFFD and consumes one byte, so decoding always progresses.
std::uint32_t decode_utf8(std::string_view text, std::size_t& pos);
std::vector<std::uint32_t> decode_utf8(std::string_view text);

enum class TextAlign { Left, Center, Right };

// Which point of the text block sits at the layout origin vertically.
enum class TextBaseline {
    Top,        // top of the first line's ascender
    Middle,     // middle of the block (ascender of the first line to descender of the last)
    Alphabetic, // the first line's baseline
    Bottom,     // bottom of the last line's descender
};

struct TextStyle {
    float size = 16.0f;         // em size, world units
    TextAlign align = TextAlign::Left;
    TextBaseline baseline = TextBaseline::Top;
    float line_spacing = 1.0f;  // multiplier on the font's line height
    float max_width = 0.0f;     // > 0: wrap lines at spaces to fit this width
    float tab_width = 4.0f;     // in spaces
};

struct LaidOutGlyph {
    std::uint32_t codepoint = 0;
    vec2 min{0.0f, 0.0f};    // quad, layout space (y down)
    vec2 max{0.0f, 0.0f};
    vec2 uv_min{0.0f, 0.0f}; // normalized atlas coordinates of `min`
    vec2 uv_max{0.0f, 0.0f};
};

struct TextLine {
    std::size_t first_glyph = 0; // range into TextLayout::glyphs
    std::size_t glyph_count = 0;
    float x = 0.0f;        // left edge after alignment, layout space
    float baseline = 0.0f; // y of the baseline, layout space
    float width = 0.0f;    // advance width (trailing spaces excluded)
};

struct TextLayout {
    std::vector<LaidOutGlyph> glyphs; // only glyphs with a visible quad
    std::vector<TextLine> lines;
    vec2 min{0.0f, 0.0f}; // layout box: widest line x line count line heights
    vec2 max{0.0f, 0.0f};
    vec2 size() const { return max - min; }
};

// Codepoint used when the font lacks one: U+FFFD, else '?', else nothing.
std::uint32_t fallback_codepoint(const FontAtlas& font);

// Advance width of a single line of codepoints (kerning included, no wrapping).
float measure_line(const FontAtlas& font, std::span<const std::uint32_t> codepoints, float size);

TextLayout layout_text(const FontAtlas& font, std::string_view utf8, const TextStyle& style);

} // namespace cuervo::render

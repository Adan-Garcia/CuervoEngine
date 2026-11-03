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
// wrapping at spaces and per-line horizontal alignment. Fonts imported with
// shaping, font fallback chains and right-to-left text take the shaped path
// (HarfBuzz glyphs drawn by glyph id, UAX #9 visual order); plain Latin text in
// one font keeps the per-codepoint path. Line breaking is at spaces only.
namespace cuervo::render {

// Decodes one UTF-8 sequence at `pos` and advances past it. Malformed input
// (bad lead/continuation bytes, overlong forms, surrogates, > U+10FFFF,
// truncation) yields U+FFFD and consumes one byte, so decoding always progresses.
std::uint32_t decode_utf8(std::string_view text, std::size_t& pos);
std::vector<std::uint32_t> decode_utf8(std::string_view text);

// Start/End follow the paragraph direction (Start = Right for right-to-left text).
enum class TextAlign { Left, Center, Right, Start, End };

// Paragraph base direction for bidirectional text; Auto = the first strong character.
enum class TextDirection { Auto, Ltr, Rtl };

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
    TextDirection direction = TextDirection::Auto;
    // Fill TextLayout::caret_x (editable text; forces the shaped path so bidi order is known).
    bool caret_stops = false;
};

struct LaidOutGlyph {
    std::uint32_t codepoint = 0; // 0 for shaped glyphs (see glyph_id)
    std::uint32_t glyph_id = 0;  // the font's glyph index on the shaped path
    std::uint32_t font = 0;      // index into the font stack the text was laid out with
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
    bool rtl = false;     // the first paragraph is right-to-left
    // With TextStyle::caret_stops: the x of the caret at each logical codepoint index 0..n (n + 1
    // entries), layout space. In a right-to-left run the caret between two characters sits at the
    // right edge of the later one, so these are not increasing for Arabic text.
    std::vector<float> caret_x;
    vec2 size() const { return max - min; }
};

// Codepoint used when the font lacks one: U+FFFD, else '?', else nothing.
std::uint32_t fallback_codepoint(const FontAtlas& font);

// Advance width of a single line of codepoints (kerning included, no wrapping).
float measure_line(const FontAtlas& font, std::span<const std::uint32_t> codepoints, float size);

// Caret movement in visual order (what the arrow keys do in bidirectional text): the logical
// index whose caret is the nearest one strictly left (dir < 0) or right (dir > 0) of `index`'s
// on screen, or `index` itself at the end. Needs a layout made with TextStyle::caret_stops.
std::size_t caret_step_visual(const TextLayout& layout, std::size_t index, int dir);
// The logical index whose caret is nearest to layout-space `x`.
std::size_t caret_index_at(const TextLayout& layout, float x);

TextLayout layout_text(const FontAtlas& font, std::string_view utf8, const TextStyle& style);
// With a fallback chain: each character is drawn by the first font of `fonts`
// that has it (the first font when none does). Line metrics are the first
// font's, so a line is as tall in every language. LaidOutGlyph::font indexes `fonts`.
TextLayout layout_text(std::span<const FontAtlas* const> fonts, std::string_view utf8, const TextStyle& style);

} // namespace cuervo::render

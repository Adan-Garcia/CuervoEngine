#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// Complex-script text (DESIGN.md 4.19 "11.3"): bidirectional reordering
// (SheenBidi, Unicode UAX #9) and glyph shaping (HarfBuzz). Both are display
// concerns outside the simulation. Built with CUERVO_TEXT_SHAPING; without it
// shaping_available() is false, reordering returns one left-to-right run and
// shaping returns nothing, so callers keep the per-codepoint layout.
namespace cuervo::text {

bool shaping_available();

// ---- Bidirectional reordering ----------------------------------------------

enum class Direction { Auto, Ltr, Rtl }; // Auto: the first strong character decides

// A maximal run of one embedding level. Offsets index the codepoint input.
struct BidiRun {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::uint8_t level = 0; // odd = right-to-left
    bool rtl() const { return (level & 1u) != 0; }
};

struct BidiResult {
    bool base_rtl = false;      // paragraph direction (of the first paragraph)
    std::vector<BidiRun> runs;  // in visual order, left to right on screen
};

// Resolves the runs of one line of text (UAX #9). Text with several
// paragraphs (separators inside) returns the runs of each, in order.
BidiResult reorder_line(std::span<const std::uint32_t> codepoints, Direction base = Direction::Auto);

// The mirrored form of a bracket-like codepoint ('(' <-> ')'), or `cp` itself.
// Right-to-left runs draw mirrored characters.
std::uint32_t mirrored(std::uint32_t cp);

// ---- Shaping ---------------------------------------------------------------

// A font face kept for shaping (the TTF/OTF bytes are copied).
class ShapingFont {
public:
    // Null if shaping is unavailable or the data is not a font.
    static std::shared_ptr<ShapingFont> create(std::span<const std::byte> font_data, int face_index = 0);
    ~ShapingFont();
    ShapingFont(const ShapingFont&) = delete;
    ShapingFont& operator=(const ShapingFont&) = delete;

    std::uint32_t glyph_count() const;
    // Glyph id for a codepoint (0 = .notdef).
    std::uint32_t glyph_for(std::uint32_t codepoint) const;
    // `glyphs` plus every glyph the default GSUB features (ligatures,
    // contextual and joining forms, mark composition) can substitute for them,
    // sorted and unique. Font import uses it to atlas the glyphs shaping can
    // produce, not only the ones a codepoint maps to.
    std::vector<std::uint32_t> glyph_closure(std::span<const std::uint32_t> glyphs) const;

    struct Impl;
    const Impl& impl() const { return *impl_; }

private:
    ShapingFont();
    std::unique_ptr<Impl> impl_;
};

struct ShapedGlyph {
    std::uint32_t glyph_id = 0;
    std::uint32_t cluster = 0; // index of the first codepoint this glyph stands for
    float x_advance = 0.0f;    // ems
    float x_offset = 0.0f;     // ems, positioning offset from the pen
    float y_offset = 0.0f;     // ems, y up like the font
};

struct ShapedRun {
    std::vector<ShapedGlyph> glyphs; // visual order (left to right on screen)
    float advance = 0.0f;            // ems
    bool rtl = false;
};

// Shapes codepoints of one script and direction into positioned glyphs (GSUB
// ligatures and contextual forms, GPOS kerning and mark attachment). The
// script is guessed from the text. Codepoints are logical order; for an
// `rtl` run the glyphs come back in visual order, with `cluster` pointing
// into `codepoints`.
ShapedRun shape_run(const ShapingFont& font, std::span<const std::uint32_t> codepoints, bool rtl);

} // namespace cuervo::text

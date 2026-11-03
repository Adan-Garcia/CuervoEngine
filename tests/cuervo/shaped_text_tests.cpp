#include <doctest.h>

#include <format>
#include <optional>
#include <string>
#include <vector>

#include "cuervo/render/font.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/text_layout.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/vector/imported_vector.hpp"

// Shaped text drawn through the text renderer (DESIGN.md 4.19 "11.3"): glyph-id
// atlases, ligatures and joining, bidi order, font fallback chains. The
// golden glyph runs pin the glyph ids of the test fonts (cmake/Dependencies.cmake).

using namespace cuervo;
using namespace cuervo::render;

namespace {

#ifdef CUERVO_TEST_FONT_ARABIC

std::vector<std::byte> read_bytes(const std::string& path) {
    std::vector<std::byte> data;
    vector::read_file(path, data);
    return data;
}

// Latin + Arabic blocks + presentation forms: what a Latin/Arabic UI font needs.
FontImportOptions arabic_options() {
    FontImportOptions o;
    o.ranges = {{0x20, 0x7E}, {0x600, 0x6FF}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFF}};
    o.em_size = 32.0f;
    o.shaping = true;
    return o;
}

const FontAtlas* arabic_font() {
    static const std::optional<FontAtlas> font = [] {
        if (!font_import_available() || !text::shaping_available()) return std::optional<FontAtlas>{};
        return import_font(read_bytes(CUERVO_TEST_FONT_ARABIC), "NotoSansArabic", arabic_options());
    }();
    return font ? &*font : nullptr;
}

const FontAtlas* latin_font() {
    static const std::optional<FontAtlas> font = [] {
        if (!font_import_available()) return std::optional<FontAtlas>{};
        return import_font(read_bytes(std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf"),
                           "LiberationSans");
    }();
    return font ? &*font : nullptr;
}

// "id1 id2 ..." in the order the glyphs appear on screen (left to right).
std::string glyph_run(const TextLayout& layout) {
    std::string out;
    for (const LaidOutGlyph& g : layout.glyphs) {
        out += g.codepoint != 0 ? std::format("{}.{} ", g.font, g.glyph_id)
                                : std::format("{}.{} ", g.font, g.glyph_id);
    }
    return out;
}

constexpr const char* salaam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"; // seen lam alef meem

TEST_CASE("shaped import: glyph-id atlas holds the forms shaping produces") {
    const FontAtlas* font = arabic_font();
    if (!font) return;
    CHECK(font->has_glyph_ids);
    CHECK(font->shaper != nullptr);
    CHECK_FALSE(font->extra_glyphs.empty()); // contextual forms no codepoint maps to
    CHECK(validate_font_atlas(*font).empty());
    for (std::uint32_t id : {74u, 11u, 71u, 37u}) { // the glyph run of "salaam" (shaping_tests)
        const FontGlyph* g = font->find_by_id(id);
        REQUIRE(g != nullptr);
        CHECK(g->has_quad());
    }
    CHECK(font->find_by_id(0) != nullptr); // .notdef
}

TEST_CASE("shaped layout: an Arabic word is drawn by glyph id in visual order") {
    const FontAtlas* font = arabic_font();
    if (!font) return;
    TextStyle style;
    style.size = 40.0f;
    const TextLayout layout = layout_text(*font, salaam, style);
    CHECK(layout.rtl);
    CHECK(glyph_run(layout) == "0.74 0.11 0.71 0.37 "); // golden; left to right on screen
    for (std::size_t i = 1; i < layout.glyphs.size(); ++i) {
        CHECK(layout.glyphs[i].min.x() > layout.glyphs[i - 1].min.x()); // joined letters advance leftwards in logic, rightwards on screen
    }
    CHECK(layout.lines[0].width > 0.0f);
}

TEST_CASE("shaped layout: bidi order of mixed text") {
    const FontAtlas* latin = latin_font();
    const FontAtlas* arabic = arabic_font();
    if (!latin || !arabic) return;
    const FontAtlas* chain[] = {latin, arabic}; // Noto Sans Arabic has no Latin letters
    TextStyle style;
    style.size = 40.0f;
    const auto id = [&](char c) { return std::format("0.{} ", latin->find(static_cast<unsigned char>(c))->glyph_id); };
    const std::string arabic_run = "1.74 1.11 1.71 1.37 ";
    // LTR paragraph: Latin, then the Arabic run, then Latin again.
    const TextLayout ltr = layout_text(chain, (std::string("ab ") + salaam + " cd").c_str(), style);
    CHECK_FALSE(ltr.rtl);
    CHECK(glyph_run(ltr) == id('a') + id('b') + arabic_run + id('c') + id('d'));
    // RTL paragraph (Arabic first): the Latin run ends up at the left.
    const TextLayout rtl = layout_text(chain, (std::string(salaam) + " ab").c_str(), style);
    CHECK(rtl.rtl);
    CHECK(glyph_run(rtl) == id('a') + id('b') + arabic_run);
    // A forced direction overrides the first strong character.
    style.direction = TextDirection::Rtl;
    CHECK(layout_text(chain, "ab", style).rtl);
}

TEST_CASE("shaped layout: wrapping breaks at spaces and reorders each line") {
    const FontAtlas* font = arabic_font();
    if (!font) return;
    TextStyle style;
    style.size = 40.0f;
    const std::string text = std::string(salaam) + " " + salaam + " " + salaam;
    const float one_line = layout_text(*font, text, style).lines[0].width;
    style.max_width = one_line * 0.5f;
    const TextLayout wrapped = layout_text(*font, text, style);
    CHECK(wrapped.lines.size() >= 2);
    for (const TextLine& line : wrapped.lines) {
        CHECK(line.width <= style.max_width + 0.01f);
    }
}

TEST_CASE("shaped layout: Start and End alignment follow the paragraph direction") {
    const FontAtlas* font = arabic_font();
    if (!font) return;
    TextStyle style;
    style.size = 40.0f;
    style.align = TextAlign::Start;
    const TextLayout rtl = layout_text(*font, salaam, style);
    CHECK(rtl.lines[0].x == doctest::Approx(-rtl.lines[0].width)); // right-aligned at the origin
    const TextLayout ltr = layout_text(*font, "ab", style);
    CHECK(ltr.lines[0].x == doctest::Approx(0.0f));
}

TEST_CASE("shaped atlas survives the .efont round trip") {
    const FontAtlas* font = arabic_font();
    if (!font) return;
    const std::vector<std::byte> bytes = serialize_font_atlas(*font);
    FontAtlas loaded;
    std::string error;
    REQUIRE(deserialize_font_atlas(bytes, loaded, &error));
    CHECK(error.empty());
    CHECK(loaded.has_glyph_ids);
    CHECK(loaded.extra_glyphs.size() == font->extra_glyphs.size());
    CHECK(loaded.shaper != nullptr);
    CHECK(serialize_font_atlas(loaded) == bytes);
    TextStyle style;
    style.size = 40.0f;
    CHECK(glyph_run(layout_text(loaded, salaam, style)) == glyph_run(layout_text(*font, salaam, style)));
}

TEST_CASE("font fallback: each character uses the first font that has it") {
    const FontAtlas* latin = latin_font();
    const FontAtlas* arabic = arabic_font();
    if (!latin || !arabic) return;
    const FontAtlas* chain[] = {latin, arabic};
    TextStyle style;
    style.size = 40.0f;
    const TextLayout layout = layout_text(chain, (std::string("Hi ") + salaam).c_str(), style);
    REQUIRE(layout.glyphs.size() == 6);
    CHECK(layout.glyphs[0].font == 0); // 'H', 'i'
    CHECK(layout.glyphs[1].font == 0);
    for (std::size_t i = 2; i < 6; ++i) CHECK(layout.glyphs[i].font == 1);
    // Line metrics are the largest of the fonts used.
    CHECK(layout.lines[0].baseline >= latin->metrics.ascender * style.size);

    // Latin-only text in the chain never touches the fallback.
    const TextLayout latin_only = layout_text(chain, "Hi", style);
    for (const LaidOutGlyph& g : latin_only.glyphs) CHECK(g.font == 0);
}

TEST_CASE("TextRenderer: fallback chains draw one run per font") {
    const FontAtlas* latin = latin_font();
    const FontAtlas* arabic = arabic_font();
    if (!latin || !arabic) return;
    TextRenderer text;
    const FontHandle primary = text.add_font(*latin);
    const FontHandle fallback = text.add_font(*arabic);
    REQUIRE(primary);
    REQUIRE(fallback);
    text.set_fallbacks(primary, {fallback});
    CHECK(text.font_stack(primary).size() == 2);
    TextStyle style;
    style.size = 40.0f;
    text.draw(primary, std::string("Hi ") + salaam, vec2{0.0f, 0.0f}, style, rhi::Color{1, 1, 1, 1});
    REQUIRE(text.plan().size() == 2);
    CHECK(text.plan()[0].font == primary);
    CHECK(text.plan()[0].instance_count == 2);
    CHECK(text.plan()[1].font == fallback);
    CHECK(text.plan()[1].instance_count == 4);
    // Arabic-only text is a single run in the fallback's atlas.
    text.clear();
    text.draw(primary, salaam, vec2{0.0f, 0.0f}, style, rhi::Color{1, 1, 1, 1});
    REQUIRE(text.plan().size() == 1);
    CHECK(text.plan()[0].font == fallback);
}

#ifdef CUERVO_TEST_FONT_JAPANESE

// Kana and the few kanji the tests use.
const FontAtlas* japanese_font() {
    static const std::optional<FontAtlas> font = [] {
        if (!font_import_available()) return std::optional<FontAtlas>{};
        FontImportOptions o;
        o.ranges = {{0x20, 0x7E}, {0x3000, 0x30FF}, {0x65E5, 0x65E5}, {0x672C, 0x672C}, {0x8A9E, 0x8A9E}};
        o.em_size = 32.0f;
        o.shaping = true;
        return import_font(read_bytes(CUERVO_TEST_FONT_JAPANESE), "MPLUS1p", o);
    }();
    return font ? &*font : nullptr;
}

TEST_CASE("font fallback: Latin, then Arabic, then Japanese") {
    const FontAtlas* latin = latin_font();
    const FontAtlas* arabic = arabic_font();
    const FontAtlas* japanese = japanese_font();
    if (!latin || !arabic || !japanese) return;
    const FontAtlas* chain[] = {latin, arabic, japanese};
    TextStyle style;
    style.size = 40.0f;
    // "Hi" + salaam + nihongo (kanji, kanji, kanji) + hiragana a
    const std::string text = std::string("Hi ") + salaam + " \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\x82";
    const TextLayout layout = layout_text(chain, text, style);
    std::vector<std::uint32_t> fonts;
    for (const LaidOutGlyph& g : layout.glyphs) fonts.push_back(g.font);
    CHECK(fonts == std::vector<std::uint32_t>{0, 0, 1, 1, 1, 1, 2, 2, 2, 2});
    // Japanese-only text is one run in the third font and keeps its order left to right.
    const TextLayout jp = layout_text(chain, "\xE6\x97\xA5\xE6\x9C\xAC", style);
    REQUIRE(jp.glyphs.size() == 2);
    CHECK(jp.glyphs[0].min.x() < jp.glyphs[1].min.x());
    CHECK(jp.glyphs[0].font == 2);
    CHECK_FALSE(jp.rtl);
    // Line metrics are the primary font's (11.3): the mixed line is not taller than a Latin one.
    const TextLayout latin_only = layout_text(chain, "ab", style);
    CHECK(layout.lines[0].baseline == doctest::Approx(latin_only.lines[0].baseline));
    CHECK(layout.size().y() == doctest::Approx(latin_only.size().y()));
}

#endif // CUERVO_TEST_FONT_JAPANESE

#endif // CUERVO_TEST_FONT_ARABIC

} // namespace

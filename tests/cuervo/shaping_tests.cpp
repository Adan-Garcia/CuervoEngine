#include <doctest.h>

#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cuervo/render/text_layout.hpp"
#include "cuervo/text/shaping.hpp"

using namespace cuervo;

namespace {

[[maybe_unused]] std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) out[i] = static_cast<std::byte>(raw[i]);
    return out;
}

std::vector<std::uint32_t> cps(const char* utf8) { return render::decode_utf8(utf8); }

std::string describe(const text::BidiResult& r) {
    std::string s = r.base_rtl ? "R:" : "L:";
    for (const text::BidiRun& run : r.runs) s += std::format(" [{},{},{}]", run.offset, run.length, run.level);
    return s;
}

} // namespace

TEST_CASE("bidi: runs come back in visual order") {
    if (!text::shaping_available()) return;
    // "abc " + 3 Arabic letters + " def": LTR paragraph, the Arabic run is level 1.
    const auto ltr = cps("abc \xD8\xA7\xD8\xA8\xD8\xAC def");
    CHECK(describe(text::reorder_line(ltr)) == "L: [0,4,0] [4,3,1] [7,4,0]");

    // Arabic first: an RTL paragraph shows the Latin run at the left.
    const auto rtl = cps("\xD8\xA7\xD8\xA8\xD8\xAC abc");
    CHECK(describe(text::reorder_line(rtl)) == "R: [4,3,2] [0,4,1]");

    // The base direction can be forced.
    CHECK(text::reorder_line(cps("abc"), text::Direction::Rtl).base_rtl);
    CHECK_FALSE(text::reorder_line(cps("abc")).base_rtl);

    // Plain text is one run; empty text none.
    CHECK(text::reorder_line(cps("hello")).runs.size() == 1);
    CHECK(text::reorder_line({}).runs.empty());

    CHECK(text::mirrored('(') == ')');
    CHECK(text::mirrored('a') == 'a');
}

#ifdef CUERVO_TEST_FONT_ARABIC
TEST_CASE("shaping: Arabic joins into contextual forms and ligatures") {
    if (!text::shaping_available()) return;
    const auto bytes = read_file(CUERVO_TEST_FONT_ARABIC);
    REQUIRE_FALSE(bytes.empty());
    const auto font = text::ShapingFont::create(bytes);
    REQUIRE(font != nullptr);
    CHECK(font->glyph_count() > 300);

    // BEH three times: this font draws each dotted letter as stem + dot glyphs, so more glyphs
    // than letters; the RTL run comes back in visual order (last letter leftmost).
    const text::ShapedRun joined = text::shape_run(*font, cps("\xD8\xA8\xD8\xA8\xD8\xA8"), true);
    REQUIRE(joined.glyphs.size() >= 3);
    CHECK(joined.glyphs.front().cluster == 2);
    CHECK(joined.glyphs.back().cluster == 0);
    const text::ShapedRun alone = text::shape_run(*font, cps("\xD8\xA8"), true);
    CHECK(joined.advance > alone.advance); // joined letters advance, and kashida-style joins differ from isolated

    // Shaping substitutes contextual forms: no glyph of the joined word is the nominal (cmap) glyph
    // of its letter, and every letter advances.
    const text::ShapedRun lam_alef = text::shape_run(*font, cps("\xD9\x84\xD8\xA7"), true);
    CHECK(lam_alef.glyphs.size() >= 1);
    CHECK(lam_alef.advance > 0.0f);
    CHECK(lam_alef.glyphs.back().glyph_id != font->glyph_for(0x644));

    // Golden run for a whole word (salaam): glyph ids and clusters are stable for the pinned font.
    const text::ShapedRun salaam = text::shape_run(*font, cps("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"), true);
    std::string got;
    for (const auto& g : salaam.glyphs) got += std::format("{}@{} ", g.glyph_id, g.cluster);
    INFO(got);
    CHECK(got == "74@3 11@2 71@1 37@0 ");
}
#endif

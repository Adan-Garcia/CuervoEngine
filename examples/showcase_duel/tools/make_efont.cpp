// Imports a TTF/OTF as a shaping-enabled .efont for [text] fallbacks (DESIGN.md 4.19, 11.3).
//
//   showcase_make_efont <font.ttf> <out.efont> <locale/strings.csv>
//
// The runtime's own `--font ... --export` imports the Latin range without shaping data, and the
// project loader reads [text] fallbacks as plain .efont files, so a fallback font for Arabic or
// Japanese has to be imported with ranges and shaping on. The ranges are printable ASCII, the Arabic
// block and presentation forms, kana, the Arabic-Indic digits and every codepoint the translation
// table uses; HarfBuzz's joining forms and ligatures are atlased by glyph id on top of that.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/vector/imported_vector.hpp"

namespace {

// Appends the codepoints of a UTF-8 string (invalid bytes are skipped).
void decode_utf8(const std::string& s, std::vector<std::uint32_t>& out) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + static_cast<std::size_t>(len) > s.size()) {
            ++i;
            continue;
        }
        std::uint32_t cp = len == 1 ? c : c & (0xFFu >> (len + 1));
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3Fu);
        out.push_back(cp);
        i += static_cast<std::size_t>(len);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fputs("usage: showcase_make_efont <font.ttf> <out.efont> <strings.csv>\n", stderr);
        return 2;
    }
    std::vector<std::byte> font_bytes, csv_bytes;
    if (!cuervo::vector::read_file(argv[1], font_bytes) || !cuervo::vector::read_file(argv[3], csv_bytes)) {
        std::fputs("cannot read an input file\n", stderr);
        return 1;
    }
    std::vector<std::uint32_t> cps;
    decode_utf8(std::string(reinterpret_cast<const char*>(csv_bytes.data()), csv_bytes.size()), cps);
    for (std::uint32_t c = 0x20; c <= 0x7E; ++c) cps.push_back(c);
    for (std::uint32_t c = 0x660; c <= 0x669; ++c) cps.push_back(c);
    for (std::uint32_t c = 0x3041; c <= 0x30FF; ++c) cps.push_back(c);
    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());

    cuervo::render::FontImportOptions options;
    options.ranges.clear();
    for (const std::uint32_t c : cps) {
        if (c < 0x20) continue;
        if (!options.ranges.empty() && options.ranges.back().second + 1 == c) {
            options.ranges.back().second = c;
        } else {
            options.ranges.emplace_back(c, c);
        }
    }
    options.em_size = 32.0f;
    options.shaping = true;
    const auto font = cuervo::render::import_font(font_bytes, argv[1], options);
    if (!font || !cuervo::vector::write_file(argv[2], cuervo::render::serialize_font_atlas(*font))) return 1;
    LOG_INFO("Imported '{}' -> '{}' ({} glyphs)", argv[1], argv[2], font->glyphs.size());
    return 0;
}

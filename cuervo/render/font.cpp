#include "cuervo/render/font.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

#include "cuervo/core/binary_io.hpp"

namespace cuervo::render {

namespace {

constexpr std::size_t glyph_record_size = 11 * 4;
constexpr std::size_t kerning_record_size = 12;
constexpr std::int32_t max_atlas_side = 16384;

bool finite(float v) { return std::isfinite(v); }

} // namespace

const FontGlyph* FontAtlas::find(std::uint32_t codepoint) const {
    auto it = std::lower_bound(glyphs.begin(), glyphs.end(), codepoint,
                               [](const FontGlyph& g, std::uint32_t cp) { return g.codepoint < cp; });
    return it != glyphs.end() && it->codepoint == codepoint ? &*it : nullptr;
}

const FontGlyph* FontAtlas::find_by_id(std::uint32_t glyph_id) const {
    if (!has_glyph_ids) {
        return nullptr;
    }
    const auto at = [&](std::uint32_t i) -> const FontGlyph& {
        return i < glyphs.size() ? glyphs[i] : extra_glyphs[i - glyphs.size()];
    };
    if (by_id.size() == glyphs.size() + extra_glyphs.size()) {
        auto it = std::lower_bound(by_id.begin(), by_id.end(), glyph_id,
                                   [&](std::uint32_t i, std::uint32_t id) { return at(i).glyph_id < id; });
        return it != by_id.end() && at(*it).glyph_id == glyph_id ? &at(*it) : nullptr;
    }
    for (const FontGlyph& g : glyphs) { // index not built: slow path
        if (g.glyph_id == glyph_id) {
            return &g;
        }
    }
    for (const FontGlyph& g : extra_glyphs) {
        if (g.glyph_id == glyph_id) {
            return &g;
        }
    }
    return nullptr;
}

void FontAtlas::build_glyph_index() {
    by_id.clear();
    if (!has_glyph_ids) {
        return;
    }
    by_id.resize(glyphs.size() + extra_glyphs.size());
    for (std::uint32_t i = 0; i < by_id.size(); ++i) {
        by_id[i] = i;
    }
    const auto at = [&](std::uint32_t i) -> const FontGlyph& {
        return i < glyphs.size() ? glyphs[i] : extra_glyphs[i - glyphs.size()];
    };
    std::stable_sort(by_id.begin(), by_id.end(),
                     [&](std::uint32_t a, std::uint32_t b) { return at(a).glyph_id < at(b).glyph_id; });
}

void FontAtlas::prepare_shaping() {
    build_glyph_index();
    if (!shaper && has_glyph_ids && !shaping_data.empty()) {
        shaper = text::ShapingFont::create(shaping_data);
    }
}

float FontAtlas::kerning_between(std::uint32_t first, std::uint32_t second) const {
    auto it = std::lower_bound(kerning.begin(), kerning.end(), std::pair{first, second},
                               [](const KerningPair& k, std::pair<std::uint32_t, std::uint32_t> key) {
                                   return k.first != key.first ? k.first < key.first : k.second < key.second;
                               });
    return it != kerning.end() && it->first == first && it->second == second ? it->adjust : 0.0f;
}

std::string validate_font_atlas(const FontAtlas& font) {
    if (font.width <= 0 || font.height <= 0 || font.width > max_atlas_side || font.height > max_atlas_side) {
        return std::format("bad atlas size {}x{}", font.width, font.height);
    }
    if (font.pixels.size() != static_cast<std::size_t>(font.width) * static_cast<std::size_t>(font.height) * 4) {
        return std::format("{} pixel bytes for a {}x{} RGBA8 atlas", font.pixels.size(), font.width, font.height);
    }
    if (!(font.em_size > 0.0f) || !(font.distance_range > 0.0f) || !finite(font.em_size) ||
        !finite(font.distance_range)) {
        return "em size and distance range must be positive";
    }
    const FontMetrics& m = font.metrics;
    if (!finite(m.ascender) || !finite(m.descender) || !(m.line_height > 0.0f) || !finite(m.line_height)) {
        return "bad font metrics";
    }
    for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
        const FontGlyph& g = font.glyphs[i];
        if (i > 0 && font.glyphs[i - 1].codepoint >= g.codepoint) {
            return std::format("glyphs not sorted/unique at U+{:04X}", g.codepoint);
        }
        for (float v : {g.advance, g.left, g.top, g.right, g.bottom}) {
            if (!finite(v)) {
                return std::format("non-finite metric in glyph U+{:04X}", g.codepoint);
            }
        }
        if (g.has_quad() && (g.atlas_left < 0.0f || g.atlas_top < 0.0f || g.atlas_right > static_cast<float>(font.width) ||
                             g.atlas_bottom > static_cast<float>(font.height) || g.atlas_left >= g.atlas_right ||
                             g.atlas_top >= g.atlas_bottom)) {
            return std::format("glyph U+{:04X} atlas rectangle outside the atlas", g.codepoint);
        }
    }
    for (std::size_t i = 0; i < font.extra_glyphs.size(); ++i) {
        const FontGlyph& g = font.extra_glyphs[i];
        if (!font.has_glyph_ids || g.codepoint != 0 || (i > 0 && font.extra_glyphs[i - 1].glyph_id >= g.glyph_id)) {
            return "extra glyphs must be glyph-id sorted, unique and have no codepoint";
        }
        for (float v : {g.advance, g.left, g.top, g.right, g.bottom}) {
            if (!finite(v)) {
                return std::format("non-finite metric in glyph id {}", g.glyph_id);
            }
        }
        if (g.has_quad() && (g.atlas_left < 0.0f || g.atlas_top < 0.0f || g.atlas_right > static_cast<float>(font.width) ||
                             g.atlas_bottom > static_cast<float>(font.height) || g.atlas_left >= g.atlas_right ||
                             g.atlas_top >= g.atlas_bottom)) {
            return std::format("glyph id {} atlas rectangle outside the atlas", g.glyph_id);
        }
    }
    for (std::size_t i = 1; i < font.kerning.size(); ++i) {
        const KerningPair& a = font.kerning[i - 1];
        const KerningPair& b = font.kerning[i];
        if (a.first > b.first || (a.first == b.first && a.second >= b.second)) {
            return "kerning pairs not sorted/unique";
        }
    }
    return {};
}

std::vector<std::byte> serialize_font_atlas(const FontAtlas& font) {
    ByteWriter w;
    w.raw(font_atlas_magic, 4);
    w.u32(font_atlas_version);
    w.u32(font.has_glyph_ids ? 1u : 0u); // flags
    w.u32(0); // reserved
    w.str(font.name);
    w.f32(font.em_size);
    w.f32(font.distance_range);
    const FontMetrics& m = font.metrics;
    for (float v : {m.ascender, m.descender, m.line_height, m.underline_y, m.underline_thickness}) {
        w.f32(v);
    }
    w.i32(font.width);
    w.i32(font.height);
    const auto write_glyphs = [&](const std::vector<FontGlyph>& list) {
        w.u32(static_cast<std::uint32_t>(list.size()));
        for (const FontGlyph& g : list) {
            w.u32(g.codepoint);
            for (float v : {g.advance, g.left, g.top, g.right, g.bottom, g.atlas_left, g.atlas_top, g.atlas_right,
                            g.atlas_bottom}) {
                w.f32(v);
            }
            w.u32(g.glyph_id);
        }
    };
    write_glyphs(font.glyphs);
    w.u32(static_cast<std::uint32_t>(font.kerning.size()));
    for (const KerningPair& k : font.kerning) {
        w.u32(k.first);
        w.u32(k.second);
        w.f32(k.adjust);
    }
    write_glyphs(font.extra_glyphs);
    w.u32(static_cast<std::uint32_t>(font.pixels.size()));
    w.raw(font.pixels.data(), font.pixels.size());
    w.u32(static_cast<std::uint32_t>(font.shaping_data.size()));
    w.raw(font.shaping_data.data(), font.shaping_data.size());
    return w.take();
}

bool is_font_atlas(std::span<const std::byte> data) {
    return data.size() >= 4 && std::memcmp(data.data(), font_atlas_magic, 4) == 0;
}

bool deserialize_font_atlas(std::span<const std::byte> data, FontAtlas& out, std::string* error) {
    const auto failed = [&](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (!is_font_atlas(data)) {
        return failed("not a font atlas (bad magic)");
    }
    ByteReader r(data.subspan(4), "FontAtlas");
    const std::uint32_t version = r.u32();
    if (r.ok() && (version == 0 || version > font_atlas_version)) {
        return failed(std::format("font atlas version {} is not supported (newest known: {})", version,
                                  font_atlas_version));
    }
    const std::uint32_t flags = r.u32();
    r.u32(); // reserved

    FontAtlas font;
    font.has_glyph_ids = version >= 2 && (flags & 1u) != 0;
    font.name = r.str();
    font.em_size = r.f32();
    font.distance_range = r.f32();
    FontMetrics& m = font.metrics;
    m.ascender = r.f32();
    m.descender = r.f32();
    m.line_height = r.f32();
    m.underline_y = r.f32();
    m.underline_thickness = r.f32();
    font.width = r.i32();
    font.height = r.i32();
    const auto read_glyphs = [&](std::vector<FontGlyph>& list) {
        list.resize(r.count(glyph_record_size));
        for (FontGlyph& g : list) {
            g.codepoint = r.u32();
            for (float* v : {&g.advance, &g.left, &g.top, &g.right, &g.bottom, &g.atlas_left, &g.atlas_top,
                             &g.atlas_right, &g.atlas_bottom}) {
                *v = r.f32();
            }
            const std::uint32_t id = r.u32(); // reserved (0) before version 2
            g.glyph_id = font.has_glyph_ids ? id : 0;
        }
    };
    read_glyphs(font.glyphs);
    font.kerning.resize(r.count(kerning_record_size));
    for (KerningPair& k : font.kerning) {
        k.first = r.u32();
        k.second = r.u32();
        k.adjust = r.f32();
    }
    if (version >= 2) {
        read_glyphs(font.extra_glyphs);
    }
    font.pixels.resize(r.count(1));
    r.raw(font.pixels.data(), font.pixels.size());
    if (version >= 2) {
        font.shaping_data.resize(r.count(1));
        r.raw(font.shaping_data.data(), font.shaping_data.size());
    }
    if (!r.ok()) {
        return failed(r.error());
    }
    if (!r.at_end()) {
        return failed("FontAtlas: trailing bytes after the pixel data");
    }
    if (std::string invalid = validate_font_atlas(font); !invalid.empty()) {
        return failed("FontAtlas: " + invalid);
    }
    font.prepare_shaping();
    out = std::move(font);
    return true;
}

} // namespace cuervo::render

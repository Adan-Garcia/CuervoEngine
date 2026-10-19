#include "cuervo/render/text_layout.hpp"

#include <algorithm>

namespace cuervo::render {

namespace {

constexpr std::uint32_t replacement_character = 0xFFFD;
constexpr std::uint32_t no_codepoint = 0xFFFFFFFFu;

bool is_space(std::uint32_t cp) { return cp == ' ' || cp == '\t'; }

// Resolves codepoints to glyphs (with fallback) and advances (with kerning),
// in ems. Shared by measuring, wrapping and positioning so they always agree.
class GlyphRun {
public:
    GlyphRun(const FontAtlas& font, float tab_width)
        : font_(font), fallback_(fallback_codepoint(font)) {
        const FontGlyph* space = font.find(' ');
        tab_advance_ = (space ? space->advance : 0.25f) * std::max(tab_width, 0.0f);
    }

    // The glyph drawn for `cp` (null for tabs and unknown codepoints without fallback).
    const FontGlyph* glyph(std::uint32_t cp) const {
        if (cp == '\t') {
            return nullptr;
        }
        if (const FontGlyph* g = font_.find(cp)) {
            return g;
        }
        return fallback_ != no_codepoint ? font_.find(fallback_) : nullptr;
    }

    // Advance in ems for `cp` following `previous` (no_codepoint at line start),
    // including the pair kerning between the glyphs actually drawn.
    float advance(std::uint32_t previous, std::uint32_t cp) const {
        if (cp == '\t') {
            return tab_advance_;
        }
        const FontGlyph* g = glyph(cp);
        if (!g) {
            return 0.0f;
        }
        float kern = 0.0f;
        if (previous != no_codepoint && previous != '\t') {
            if (const FontGlyph* p = glyph(previous)) {
                kern = font_.kerning_between(p->codepoint, g->codepoint);
            }
        }
        return kern + g->advance;
    }

private:
    const FontAtlas& font_;
    std::uint32_t fallback_;
    float tab_advance_ = 0.0f;
};

float measure(const GlyphRun& run, std::span<const std::uint32_t> cps) {
    float pen = 0.0f;
    std::uint32_t previous = no_codepoint;
    for (std::uint32_t cp : cps) {
        pen += run.advance(previous, cp);
        previous = cp;
    }
    return pen;
}

struct LineRange {
    std::size_t begin = 0;
    std::size_t end = 0; // trailing spaces excluded
};

// Greedy word wrap of one paragraph (no '\n' inside) into lines.
void wrap_paragraph(const GlyphRun& run, std::span<const std::uint32_t> cps, std::size_t begin, std::size_t end,
                    float max_width_ems, std::vector<LineRange>& out) {
    std::size_t start = begin;
    for (;;) {
        std::size_t line_end = end;
        if (max_width_ems > 0.0f) {
            float pen = 0.0f;
            std::uint32_t previous = no_codepoint;
            std::size_t last_break = end; // index of a space to break before
            for (std::size_t i = start; i < end; ++i) {
                const std::uint32_t cp = cps[i];
                const float advance = run.advance(previous, cp);
                if (is_space(cp)) {
                    last_break = i;
                } else if (pen + advance > max_width_ems && last_break != end && last_break > start) {
                    line_end = last_break;
                    break;
                }
                pen += advance;
                previous = cp;
            }
        }
        std::size_t trimmed = line_end;
        while (trimmed > start && is_space(cps[trimmed - 1])) {
            --trimmed;
        }
        out.push_back({start, trimmed});
        if (line_end >= end) {
            return;
        }
        start = line_end;
        while (start < end && is_space(cps[start])) {
            ++start;
        }
        if (start >= end) {
            return;
        }
    }
}

} // namespace

std::uint32_t decode_utf8(std::string_view text, std::size_t& pos) {
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
    const std::uint8_t lead = byte(pos);
    if (lead < 0x80) {
        ++pos;
        return lead;
    }
    std::size_t length = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2, cp = lead & 0x1Fu, min = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3, cp = lead & 0x0Fu, min = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4, cp = lead & 0x07u, min = 0x10000;
    } else {
        ++pos; // stray continuation byte or invalid lead
        return replacement_character;
    }
    if (pos + length > text.size()) {
        ++pos;
        return replacement_character;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const std::uint8_t b = byte(pos + i);
        if ((b & 0xC0) != 0x80) {
            ++pos;
            return replacement_character;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++pos;
        return replacement_character;
    }
    pos += length;
    return cp;
}

std::vector<std::uint32_t> decode_utf8(std::string_view text) {
    std::vector<std::uint32_t> out;
    out.reserve(text.size());
    for (std::size_t pos = 0; pos < text.size();) {
        out.push_back(decode_utf8(text, pos));
    }
    return out;
}

std::uint32_t fallback_codepoint(const FontAtlas& font) {
    if (font.find(replacement_character)) {
        return replacement_character;
    }
    if (font.find('?')) {
        return '?';
    }
    return no_codepoint;
}

float measure_line(const FontAtlas& font, std::span<const std::uint32_t> codepoints, float size) {
    return measure(GlyphRun(font, 4.0f), codepoints) * size;
}

TextLayout layout_text(const FontAtlas& font, std::string_view utf8, const TextStyle& style) {
    TextLayout layout;
    const std::vector<std::uint32_t> cps = decode_utf8(utf8);
    const GlyphRun run(font, style.tab_width);
    const float size = style.size;

    // Lines: paragraphs split at '\n' (a "\r\n" pair counts as one break), then wrapped.
    std::vector<LineRange> ranges;
    const float max_width_ems = style.max_width > 0.0f && size > 0.0f ? style.max_width / size : 0.0f;
    std::size_t paragraph = 0;
    for (std::size_t i = 0; i <= cps.size(); ++i) {
        if (i == cps.size() || cps[i] == '\n') {
            std::size_t end = i;
            if (end > paragraph && cps[end - 1] == '\r') {
                --end;
            }
            wrap_paragraph(run, cps, paragraph, end, max_width_ems, ranges);
            paragraph = i + 1;
        }
    }

    const FontMetrics& m = font.metrics;
    const float line_advance = m.line_height * size * style.line_spacing;
    const float ascent = m.ascender * size;
    const float height = ascent + line_advance * static_cast<float>(ranges.size() - 1) - m.descender * size;
    float top = 0.0f;
    switch (style.baseline) {
        case TextBaseline::Top:        top = 0.0f; break;
        case TextBaseline::Middle:     top = -0.5f * height; break;
        case TextBaseline::Alphabetic: top = -ascent; break;
        case TextBaseline::Bottom:     top = -height; break;
    }

    const float inv_w = 1.0f / static_cast<float>(std::max(font.width, 1));
    const float inv_h = 1.0f / static_cast<float>(std::max(font.height, 1));
    float min_x = 0.0f;
    float max_x = 0.0f;
    for (std::size_t l = 0; l < ranges.size(); ++l) {
        const LineRange& range = ranges[l];
        const std::span<const std::uint32_t> line_cps(cps.data() + range.begin, range.end - range.begin);
        TextLine line;
        line.width = measure(run, line_cps) * size;
        line.baseline = top + ascent + line_advance * static_cast<float>(l);
        switch (style.align) {
            case TextAlign::Left:   line.x = 0.0f; break;
            case TextAlign::Center: line.x = -0.5f * line.width; break;
            case TextAlign::Right:  line.x = -line.width; break;
        }
        min_x = l == 0 ? line.x : std::min(min_x, line.x);
        max_x = l == 0 ? line.x + line.width : std::max(max_x, line.x + line.width);
        line.first_glyph = layout.glyphs.size();

        float pen = line.x;
        std::uint32_t previous = no_codepoint;
        for (std::uint32_t cp : line_cps) {
            const float advance = run.advance(previous, cp) * size;
            const FontGlyph* g = run.glyph(cp);
            if (g && g->has_quad()) {
                // The glyph's own advance comes last: kerning shifts the glyph itself.
                const float origin = pen + advance - g->advance * size;
                LaidOutGlyph out;
                out.codepoint = g->codepoint;
                out.min = {origin + g->left * size, line.baseline + g->top * size};
                out.max = {origin + g->right * size, line.baseline + g->bottom * size};
                out.uv_min = {g->atlas_left * inv_w, g->atlas_top * inv_h};
                out.uv_max = {g->atlas_right * inv_w, g->atlas_bottom * inv_h};
                layout.glyphs.push_back(out);
            }
            pen += advance;
            previous = cp;
        }
        line.glyph_count = layout.glyphs.size() - line.first_glyph;
        layout.lines.push_back(line);
    }
    layout.min = {min_x, top};
    layout.max = {max_x, top + height};
    return layout;
}

} // namespace cuervo::render

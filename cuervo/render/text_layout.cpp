#include "cuervo/render/text_layout.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

#include "cuervo/text/shaping.hpp"

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

static TextLayout layout_simple(const FontAtlas& font, std::string_view utf8, const TextStyle& style) {
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
            case TextAlign::Left:
            case TextAlign::Start:  line.x = 0.0f; break;
            case TextAlign::Center: line.x = -0.5f * line.width; break;
            case TextAlign::Right:
            case TextAlign::End:    line.x = -line.width; break;
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

namespace {

// ---- Shaped path ------------------------------------------------------------
// Fallback chains, HarfBuzz glyph runs and bidirectional order. A paragraph is
// split into segments of one bidi level and one font, each shaped on its own;
// lines are wrapped on per-codepoint widths read back from the clusters, and
// each line's segments are put in visual order (UAX #9 rule L2).

// One positioned glyph, ems, y up for the offsets.
struct Item {
    std::uint32_t glyph_id = 0;
    std::uint32_t codepoint = 0; // used when !by_id
    bool by_id = false;
    bool tab = false;
    std::uint32_t cluster = 0; // paragraph-relative index of the first codepoint
    float advance = 0.0f;
    float x_offset = 0.0f;
    float y_offset = 0.0f;
};

struct Segment {
    std::size_t begin = 0; // paragraph-relative codepoint range
    std::size_t end = 0;
    std::uint8_t level = 0;
    std::uint32_t font = 0;
    std::vector<Item> items; // left-to-right on screen
    std::size_t base = 0;    // codepoint index of the paragraph's start in the whole text
};

struct Piece {
    const Segment* segment;
    std::size_t begin, end;
    std::uint8_t level;
};

bool weak_codepoint(std::uint32_t cp) {
    return cp < 0x30 || (cp >= 0x3A && cp < 0x41) || (cp >= 0x5B && cp < 0x61) || (cp >= 0x7B && cp < 0x80) ||
           (cp >= 0x300 && cp < 0x370) || (cp >= 0x200B && cp <= 0x200F) || cp == 0x3000 || cp == 0x3001 ||
           cp == 0x3002 || cp == 0xFE0F || cp == 0xFE0E;
}

std::uint32_t pick_font(std::span<const FontAtlas* const> fonts, std::uint32_t cp, std::uint32_t previous) {
    if (cp == '\t' || cp == ' ') {
        return previous;
    }
    if (weak_codepoint(cp) && fonts[previous]->find(cp)) {
        return previous;
    }
    for (std::uint32_t f = 0; f < fonts.size(); ++f) {
        if (fonts[f]->find(cp)) {
            return f;
        }
    }
    return weak_codepoint(cp) ? previous : 0;
}

void shape_segment(const FontAtlas& font, std::span<const std::uint32_t> para, Segment& seg, float tab_width) {
    const bool rtl = (seg.level & 1u) != 0;
    std::vector<std::uint32_t> cps(para.begin() + static_cast<std::ptrdiff_t>(seg.begin),
                                   para.begin() + static_cast<std::ptrdiff_t>(seg.end));
    if (rtl) {
        for (std::uint32_t& cp : cps) {
            cp = text::mirrored(cp);
        }
    }
    const FontGlyph* space = font.find(' ');
    const float tab_advance = (space ? space->advance : 0.25f) * std::max(tab_width, 0.0f);
    if (font.shaper) {
        const text::ShapedRun run = text::shape_run(*font.shaper, cps, rtl);
        for (const text::ShapedGlyph& g : run.glyphs) {
            Item item;
            item.glyph_id = g.glyph_id;
            item.by_id = true;
            item.cluster = static_cast<std::uint32_t>(seg.begin) + g.cluster;
            item.advance = g.x_advance;
            item.x_offset = g.x_offset;
            item.y_offset = g.y_offset;
            if (para[item.cluster] == '\t') {
                item.tab = true;
                item.advance = tab_advance;
            }
            seg.items.push_back(item);
        }
    } else {
        const std::uint32_t fallback = fallback_codepoint(font);
        std::uint32_t previous = no_codepoint;
        for (std::size_t i = 0; i < cps.size(); ++i) {
            Item item;
            item.cluster = static_cast<std::uint32_t>(seg.begin + i);
            if (cps[i] == '\t') {
                item.tab = true;
                item.advance = tab_advance;
                previous = no_codepoint;
            } else {
                const FontGlyph* g = font.find(cps[i]);
                if (!g && fallback != no_codepoint) {
                    g = font.find(fallback);
                }
                if (g) {
                    item.codepoint = g->codepoint;
                    item.advance = g->advance;
                    if (previous != no_codepoint) {
                        item.advance += font.kerning_between(previous, g->codepoint);
                    }
                    previous = g->codepoint;
                } else {
                    previous = no_codepoint;
                }
            }
            seg.items.push_back(item);
        }
        if (rtl) {
            std::reverse(seg.items.begin(), seg.items.end());
        }
    }
}

struct LineSpec {
    std::vector<Piece> pieces;
    bool rtl = false;
};

TextLayout layout_shaped(std::span<const FontAtlas* const> fonts, std::string_view utf8, const TextStyle& style) {
    TextLayout layout;
    const std::vector<std::uint32_t> all = decode_utf8(utf8);
    const float size = style.size;
    const float max_width_ems = style.max_width > 0.0f && size > 0.0f ? style.max_width / size : 0.0f;
    text::Direction base = text::Direction::Auto;
    if (style.direction == TextDirection::Ltr) base = text::Direction::Ltr;
    if (style.direction == TextDirection::Rtl) base = text::Direction::Rtl;

    // Segments of all paragraphs stay alive until the end (pieces point into them).
    std::deque<std::vector<Segment>> paragraphs;
    std::vector<LineSpec> lines;
    std::vector<std::uint8_t> cp_level(all.size(), 0);
    const float no_span = std::numeric_limits<float>::quiet_NaN();
    std::vector<std::pair<float, float>> spans; // per codepoint: left and right x, layout space
    if (style.caret_stops) spans.assign(all.size(), {no_span, no_span});

    std::size_t paragraph = 0;
    bool first_paragraph = true;
    for (std::size_t i = 0; i <= all.size(); ++i) {
        if (i != all.size() && all[i] != '\n') {
            continue;
        }
        std::size_t end = i;
        if (end > paragraph && all[end - 1] == '\r') {
            --end;
        }
        const std::span<const std::uint32_t> para(all.data() + paragraph, end - paragraph);
        const std::size_t para_base = paragraph;
        paragraph = i + 1;

        // Levels per codepoint from the bidi runs, fonts per codepoint from the chain.
        std::vector<std::uint8_t> levels(para.size(), 0);
        bool para_rtl = base == text::Direction::Rtl;
        if (!para.empty()) {
            const text::BidiResult bidi = text::reorder_line(para, base);
            para_rtl = bidi.base_rtl;
            for (const text::BidiRun& run : bidi.runs) {
                std::fill_n(levels.begin() + static_cast<std::ptrdiff_t>(run.offset), run.length, run.level);
            }
        }
        std::copy(levels.begin(), levels.end(), cp_level.begin() + static_cast<std::ptrdiff_t>(para_base));
        if (first_paragraph) {
            layout.rtl = para_rtl;
            first_paragraph = false;
        }
        std::vector<Segment>& segments = paragraphs.emplace_back();
        std::uint32_t previous_font = 0;
        for (std::size_t k = 0; k < para.size(); ++k) {
            const std::uint32_t f = pick_font(fonts, para[k], previous_font);
            previous_font = f;
            if (segments.empty() || segments.back().level != levels[k] || segments.back().font != f) {
                segments.push_back({k, k, levels[k], f, {}});
            }
            segments.back().end = k + 1;
            segments.back().base = para_base;
        }
        std::vector<float> width(para.size(), 0.0f);
        for (Segment& seg : segments) {
            shape_segment(*fonts[seg.font], para, seg, style.tab_width);
            for (const Item& item : seg.items) {
                width[item.cluster] += item.advance;
            }
        }

        // Greedy wrap at spaces on the cluster widths.
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        std::size_t start = 0;
        for (;;) {
            std::size_t line_end = para.size();
            if (max_width_ems > 0.0f) {
                float pen = 0.0f;
                std::size_t last_break = para.size();
                for (std::size_t k = start; k < para.size(); ++k) {
                    if (is_space(para[k])) {
                        last_break = k;
                    } else if (pen + width[k] > max_width_ems && last_break != para.size() && last_break > start) {
                        line_end = last_break;
                        break;
                    }
                    pen += width[k];
                }
            }
            std::size_t trimmed = line_end;
            while (trimmed > start && is_space(para[trimmed - 1])) {
                --trimmed;
            }
            ranges.emplace_back(start, trimmed);
            if (line_end >= para.size()) break;
            start = line_end;
            while (start < para.size() && is_space(para[start])) ++start;
            if (start >= para.size()) break;
        }

        for (const auto& [b, e] : ranges) {
            LineSpec line;
            line.rtl = para_rtl;
            for (const Segment& seg : segments) {
                const std::size_t pb = std::max(b, seg.begin);
                const std::size_t pe = std::min(e, seg.end);
                if (pb < pe) line.pieces.push_back({&seg, pb, pe, seg.level});
            }
            // L2: from the highest level down to the lowest odd one, reverse every run at or above it.
            std::uint8_t highest = 0;
            std::uint8_t lowest_odd = 255;
            for (const Piece& p : line.pieces) {
                highest = std::max(highest, p.level);
                if (p.level & 1u) lowest_odd = std::min(lowest_odd, p.level);
            }
            for (int lv = highest; lv >= lowest_odd && lv > 0; --lv) {
                for (std::size_t a = 0; a < line.pieces.size();) {
                    if (line.pieces[a].level < lv) {
                        ++a;
                        continue;
                    }
                    std::size_t z = a;
                    while (z < line.pieces.size() && line.pieces[z].level >= lv) ++z;
                    std::reverse(line.pieces.begin() + static_cast<std::ptrdiff_t>(a),
                                 line.pieces.begin() + static_cast<std::ptrdiff_t>(z));
                    a = z;
                }
            }
            lines.push_back(std::move(line));
        }
    }

    // Metrics: the primary font's, whatever fonts the text falls back to. A chain
    // member with taller metrics (CJK, Arabic) would otherwise make a line, and so
    // a button, taller when the language changes (DESIGN.md 4.19 "11.3").
    const FontMetrics m = fonts[0]->metrics;
    const float line_advance = m.line_height * size * style.line_spacing;
    const float ascent = m.ascender * size;
    const float height = ascent + line_advance * static_cast<float>(lines.size() - 1) - m.descender * size;
    float top = 0.0f;
    switch (style.baseline) {
        case TextBaseline::Top:        top = 0.0f; break;
        case TextBaseline::Middle:     top = -0.5f * height; break;
        case TextBaseline::Alphabetic: top = -ascent; break;
        case TextBaseline::Bottom:     top = -height; break;
    }

    float min_x = 0.0f;
    float max_x = 0.0f;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const LineSpec& spec = lines[l];
        TextLine line;
        float width = 0.0f;
        for (const Piece& p : spec.pieces) {
            for (const Item& item : p.segment->items) {
                if (item.cluster >= p.begin && item.cluster < p.end) width += item.advance * size;
            }
        }
        line.width = width;
        line.baseline = top + ascent + line_advance * static_cast<float>(l);
        TextAlign align = style.align;
        if (align == TextAlign::Start) align = spec.rtl ? TextAlign::Right : TextAlign::Left;
        if (align == TextAlign::End) align = spec.rtl ? TextAlign::Left : TextAlign::Right;
        switch (align) {
            case TextAlign::Center: line.x = -0.5f * width; break;
            case TextAlign::Right:  line.x = -width; break;
            default:                line.x = 0.0f; break;
        }
        min_x = l == 0 ? line.x : std::min(min_x, line.x);
        max_x = l == 0 ? line.x + width : std::max(max_x, line.x + width);
        line.first_glyph = layout.glyphs.size();
        float pen = line.x;
        for (const Piece& p : spec.pieces) {
            const FontAtlas& atlas = *fonts[p.segment->font];
            const float inv_w = 1.0f / static_cast<float>(std::max(atlas.width, 1));
            const float inv_h = 1.0f / static_cast<float>(std::max(atlas.height, 1));
            for (const Item& item : p.segment->items) {
                if (item.cluster < p.begin || item.cluster >= p.end) continue;
                if (style.caret_stops) {
                    auto& sp = spans[p.segment->base + item.cluster];
                    const float a = pen;
                    const float b = pen + item.advance * size;
                    sp.first = std::isnan(sp.first) ? std::min(a, b) : std::min(sp.first, std::min(a, b));
                    sp.second = std::isnan(sp.second) ? std::max(a, b) : std::max(sp.second, std::max(a, b));
                }
                const FontGlyph* g = nullptr;
                if (!item.tab) g = item.by_id ? atlas.find_by_id(item.glyph_id) : atlas.find(item.codepoint);
                if (g && g->has_quad()) {
                    const float ox = pen + item.x_offset * size;
                    const float oy = line.baseline - item.y_offset * size;
                    LaidOutGlyph out;
                    out.codepoint = item.by_id ? 0 : g->codepoint;
                    out.glyph_id = g->glyph_id;
                    out.font = p.segment->font;
                    out.min = {ox + g->left * size, oy + g->top * size};
                    out.max = {ox + g->right * size, oy + g->bottom * size};
                    out.uv_min = {g->atlas_left * inv_w, g->atlas_top * inv_h};
                    out.uv_max = {g->atlas_right * inv_w, g->atlas_bottom * inv_h};
                    layout.glyphs.push_back(out);
                }
                pen += item.advance * size;
            }
        }
        line.glyph_count = layout.glyphs.size() - line.first_glyph;
        layout.lines.push_back(line);
    }
    layout.min = {min_x, top};
    layout.max = {max_x, top + height};
    if (style.caret_stops) {
        const std::size_t n = all.size();
        // A ligature's trailing codepoints have no glyph of their own: they share the head's span.
        for (std::size_t j = 0; j < n; ++j) {
            if (std::isnan(spans[j].first)) continue;
            std::size_t k = j + 1;
            while (k < n && std::isnan(spans[k].first) && all[k] != '\n' && all[k] != '\r') ++k;
            const std::size_t parts = k - j;
            if (parts < 2) continue;
            const float lo = spans[j].first;
            const float w = (spans[j].second - lo) / static_cast<float>(parts);
            const bool rtl = (cp_level[j] & 1u) != 0;
            for (std::size_t q = 0; q < parts; ++q) {
                const float slot = static_cast<float>(rtl ? parts - 1 - q : q);
                spans[j + q] = {lo + w * slot, lo + w * (slot + 1.0f)};
            }
        }
        layout.caret_x.assign(n + 1, 0.0f);
        float last = layout.lines.empty() ? 0.0f : layout.lines[0].x + (layout.rtl ? layout.lines[0].width : 0.0f);
        for (std::size_t j = 0; j < n; ++j) {
            if (!std::isnan(spans[j].first)) {
                last = (cp_level[j] & 1u) != 0 ? spans[j].second : spans[j].first;
            }
            layout.caret_x[j] = last;
        }
        if (n > 0 && !std::isnan(spans[n - 1].first)) {
            layout.caret_x[n] = (cp_level[n - 1] & 1u) != 0 ? spans[n - 1].first : spans[n - 1].second;
        } else {
            layout.caret_x[n] = last;
        }
    }
    return layout;
}

// The shaped path is needed for fallback chains, shaped fonts and scripts
// that read right to left (Hebrew and up).
bool needs_shaped_path(std::span<const FontAtlas* const> fonts, std::string_view utf8, const TextStyle& style) {
    if (style.caret_stops) return true;
    if (fonts.size() > 1) return true;
    for (const FontAtlas* f : fonts) {
        if (f->shaper) return true;
    }
    for (std::size_t pos = 0; pos < utf8.size();) {
        if (decode_utf8(utf8, pos) >= 0x590) return true;
    }
    return false;
}

} // namespace

TextLayout layout_text(const FontAtlas& font, std::string_view utf8, const TextStyle& style) {
    const FontAtlas* one = &font;
    return needs_shaped_path({&one, 1}, utf8, style) ? layout_shaped({&one, 1}, utf8, style) : layout_simple(font, utf8, style);
}

TextLayout layout_text(std::span<const FontAtlas* const> fonts, std::string_view utf8, const TextStyle& style) {
    if (fonts.empty()) return {};
    return needs_shaped_path(fonts, utf8, style) ? layout_shaped(fonts, utf8, style) : layout_simple(*fonts[0], utf8, style);
}

std::size_t caret_step_visual(const TextLayout& layout, std::size_t index, int dir) {
    const std::vector<float>& xs = layout.caret_x;
    if (xs.empty() || index >= xs.size() || dir == 0) return index;
    const float here = xs[index];
    constexpr float eps = 0.01f;
    std::size_t best = index;
    float best_x = 0.0f;
    std::size_t best_dist = 0;
    for (std::size_t j = 0; j < xs.size(); ++j) {
        const bool ok = dir < 0 ? xs[j] < here - eps : xs[j] > here + eps;
        if (!ok) continue;
        const std::size_t dist = j > index ? j - index : index - j;
        const bool nearer = best == index || (dir < 0 ? xs[j] > best_x + eps : xs[j] < best_x - eps) ||
                            (std::abs(xs[j] - best_x) <= eps && dist < best_dist);
        if (nearer) {
            best = j;
            best_x = xs[j];
            best_dist = dist;
        }
    }
    return best;
}

std::size_t caret_index_at(const TextLayout& layout, float x) {
    const std::vector<float>& xs = layout.caret_x;
    std::size_t best = 0;
    for (std::size_t j = 1; j < xs.size(); ++j) {
        if (std::abs(xs[j] - x) < std::abs(xs[best] - x)) best = j;
    }
    return best;
}

} // namespace cuervo::render

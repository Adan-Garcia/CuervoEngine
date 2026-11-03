#include "cuervo/text/shaping.hpp"

#include <algorithm>

#ifdef CUERVO_HAS_TEXT_SHAPING
#include <hb-ot.h>
#include <hb.h>

#include <SheenBidi/SheenBidi.h>
#endif

namespace cuervo::text {

#ifdef CUERVO_HAS_TEXT_SHAPING

bool shaping_available() { return true; }

BidiResult reorder_line(std::span<const std::uint32_t> codepoints, Direction base) {
    BidiResult out;
    if (codepoints.empty()) return out;
    SBCodepointSequence seq{SBStringEncodingUTF32, codepoints.data(), static_cast<SBUInteger>(codepoints.size())};
    SBAlgorithmRef algorithm = SBAlgorithmCreate(&seq);
    if (algorithm == nullptr) {
        out.runs.push_back({0, codepoints.size(), 0});
        return out;
    }
    const SBLevel base_level = base == Direction::Ltr ? 0 : base == Direction::Rtl ? 1 : SBLevelDefaultLTR;
    SBUInteger offset = 0;
    bool first = true;
    while (offset < codepoints.size()) {
        SBUInteger length = 0, separator = 0;
        SBAlgorithmGetParagraphBoundary(algorithm, offset, static_cast<SBUInteger>(codepoints.size()) - offset, &length,
                                        &separator);
        SBParagraphRef paragraph = SBAlgorithmCreateParagraph(algorithm, offset, length, base_level);
        if (paragraph == nullptr) break;
        if (first) out.base_rtl = (SBParagraphGetBaseLevel(paragraph) & 1u) != 0;
        first = false;
        SBLineRef line = SBParagraphCreateLine(paragraph, offset, length);
        if (line != nullptr) {
            const SBRun* runs = SBLineGetRunsPtr(line);
            for (SBUInteger i = 0; i < SBLineGetRunCount(line); ++i) {
                out.runs.push_back({runs[i].offset, runs[i].length, runs[i].level});
            }
            SBLineRelease(line);
        }
        SBParagraphRelease(paragraph);
        offset += length;
    }
    SBAlgorithmRelease(algorithm);
    return out;
}

std::uint32_t mirrored(std::uint32_t cp) {
    const SBCodepoint m = SBCodepointGetMirror(cp);
    return m != 0 ? m : cp;
}

struct ShapingFont::Impl {
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    float upem = 1000.0f;
};

ShapingFont::ShapingFont() : impl_(std::make_unique<Impl>()) {}

ShapingFont::~ShapingFont() {
    if (impl_->font != nullptr) hb_font_destroy(impl_->font);
    if (impl_->face != nullptr) hb_face_destroy(impl_->face);
    if (impl_->blob != nullptr) hb_blob_destroy(impl_->blob);
}

std::shared_ptr<ShapingFont> ShapingFont::create(std::span<const std::byte> data, int face_index) {
    if (data.empty()) return nullptr;
    std::shared_ptr<ShapingFont> f(new ShapingFont());
    Impl& i = *f->impl_;
    // Duplicate mode: the face outlives the caller's buffer.
    i.blob = hb_blob_create(reinterpret_cast<const char*>(data.data()), static_cast<unsigned>(data.size()),
                            HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
    i.face = hb_face_create(i.blob, static_cast<unsigned>(face_index));
    if (hb_face_get_glyph_count(i.face) == 0) return nullptr;
    i.font = hb_font_create(i.face);
    hb_ot_font_set_funcs(i.font);
    i.upem = static_cast<float>(std::max(1u, hb_face_get_upem(i.face)));
    hb_font_set_scale(i.font, static_cast<int>(i.upem), static_cast<int>(i.upem));
    return f;
}

std::uint32_t ShapingFont::glyph_count() const { return hb_face_get_glyph_count(impl_->face); }

std::uint32_t ShapingFont::glyph_for(std::uint32_t cp) const {
    hb_codepoint_t g = 0;
    return hb_font_get_nominal_glyph(impl_->font, cp, &g) ? g : 0;
}

std::vector<std::uint32_t> ShapingFont::glyph_closure(std::span<const std::uint32_t> glyphs) const {
    // The features hb_shape applies by default to the scripts we ship.
    static constexpr hb_tag_t feature_tags[] = {
        HB_TAG('c', 'c', 'm', 'p'), HB_TAG('l', 'o', 'c', 'l'), HB_TAG('r', 'l', 'i', 'g'), HB_TAG('l', 'i', 'g', 'a'),
        HB_TAG('c', 'l', 'i', 'g'), HB_TAG('c', 'a', 'l', 't'), HB_TAG('i', 's', 'o', 'l'), HB_TAG('i', 'n', 'i', 't'),
        HB_TAG('m', 'e', 'd', 'i'), HB_TAG('f', 'i', 'n', 'a'), HB_TAG('m', 'e', 'd', '2'), HB_TAG('f', 'i', 'n', '2'),
        HB_TAG('f', 'i', 'n', '3'), HB_TAG('r', 'c', 'l', 't'), HB_TAG('r', 't', 'l', 'm'), HB_TAG('v', 'e', 'r', 't'),
        HB_TAG('m', 's', 'e', 't'), HB_TAG('n', 'u', 'k', 't'), HB_TAG('a', 'k', 'h', 'n'), HB_TAG('c', 'j', 'c', 't'),
        HB_TAG_NONE};
    hb_set_t* set = hb_set_create();
    for (std::uint32_t g : glyphs) hb_set_add(set, g);
    hb_set_t* lookups = hb_set_create();
    hb_ot_layout_collect_lookups(impl_->face, HB_OT_TAG_GSUB, nullptr, nullptr, feature_tags, lookups);
    hb_ot_layout_lookups_substitute_closure(impl_->face, lookups, set);
    std::vector<std::uint32_t> out;
    for (hb_codepoint_t g = HB_SET_VALUE_INVALID; hb_set_next(set, &g);) out.push_back(g);
    hb_set_destroy(lookups);
    hb_set_destroy(set);
    return out;
}

ShapedRun shape_run(const ShapingFont& font, std::span<const std::uint32_t> codepoints, bool rtl) {
    ShapedRun out;
    out.rtl = rtl;
    if (codepoints.empty()) return out;
    const ShapingFont::Impl& impl = font.impl();
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf32(buffer, codepoints.data(), static_cast<int>(codepoints.size()), 0,
                        static_cast<int>(codepoints.size()));
    hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buffer);
    hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR); // the bidi level wins over the guess
    hb_shape(impl.font, buffer, nullptr, 0);
    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);
    out.glyphs.reserve(count);
    for (unsigned k = 0; k < count; ++k) {
        ShapedGlyph g;
        g.glyph_id = info[k].codepoint;
        g.cluster = info[k].cluster;
        g.x_advance = static_cast<float>(pos[k].x_advance) / impl.upem;
        g.x_offset = static_cast<float>(pos[k].x_offset) / impl.upem;
        g.y_offset = static_cast<float>(pos[k].y_offset) / impl.upem;
        out.advance += g.x_advance;
        out.glyphs.push_back(g);
    }
    // HarfBuzz returns RTL buffers already in visual (left-to-right) order.
    hb_buffer_destroy(buffer);
    return out;
}

#else // no shaping library: callers keep the per-codepoint layout

bool shaping_available() { return false; }

BidiResult reorder_line(std::span<const std::uint32_t> codepoints, Direction) {
    BidiResult out;
    if (!codepoints.empty()) out.runs.push_back({0, codepoints.size(), 0});
    return out;
}

std::uint32_t mirrored(std::uint32_t cp) { return cp; }

struct ShapingFont::Impl {};
ShapingFont::ShapingFont() = default;
ShapingFont::~ShapingFont() = default;
std::shared_ptr<ShapingFont> ShapingFont::create(std::span<const std::byte>, int) { return nullptr; }
std::uint32_t ShapingFont::glyph_count() const { return 0; }
std::uint32_t ShapingFont::glyph_for(std::uint32_t) const { return 0; }
std::vector<std::uint32_t> ShapingFont::glyph_closure(std::span<const std::uint32_t>) const { return {}; }
ShapedRun shape_run(const ShapingFont&, std::span<const std::uint32_t>, bool rtl) {
    ShapedRun out;
    out.rtl = rtl;
    return out;
}

#endif

} // namespace cuervo::text

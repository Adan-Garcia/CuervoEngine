#include "editor/splash.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/vector/imported_vector.hpp"

namespace cuervo::editor {

namespace {

// The kit's horizontal lockup (design kit 02_Visual_Identity 2.4): the
// wordmark is 92% of the mark's height, 0.32 mark heights to its right,
// vertically centered.
constexpr float wordmark_height = 0.92f;
constexpr float lockup_gap = 0.32f;

// The wordmark takes the two-tone mark's body color, so the lockup reads as
// one unit: Raven on light (mark-ember.svg), Graphite 100 on dark
// (mark-ember-dark.svg).
rhi::Color wordmark_color(brand::Theme theme) {
    return theme == brand::Theme::Light ? brand::palette::raven : brand::palette::graphite_100;
}

std::optional<vector::ImportedVector> import_brand_svg(const std::string& path, rhi::Color current_color) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_WARN("Splash: can't read '{}'", path);
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    // currentColor is the color of the context the mark sits in: fill it in.
    const std::string hex = brand::to_hex(current_color);
    for (std::size_t at = text.find("currentColor"); at != std::string::npos; at = text.find("currentColor", at)) {
        text.replace(at, 12, hex);
    }
    return vector::import_svg(text, path);
}

} // namespace

bool Splash::init(rhi::Device& device, render::TextRenderer& text, const std::string& brand_dir,
                  const std::string& display_font) {
    const brand::Theme themes[] = {brand::Theme::Light, brand::Theme::Dark};
    for (brand::Theme theme : themes) {
        const auto t = static_cast<std::size_t>(theme);
        const std::string mark = brand_dir + (theme == brand::Theme::Light ? "/logo/mark-ember.svg" : "/logo/mark-ember-dark.svg");
        auto m = import_brand_svg(mark, wordmark_color(theme));
        auto w = import_brand_svg(brand_dir + "/logo/wordmark.svg", wordmark_color(theme));
        if (!m || !w) return false;
        parts_[t][0].asset.assign(std::move(*m));
        parts_[t][1].asset.assign(std::move(*w));
    }
    if (!meshes_.init(device)) return false;
    if (!display_font.empty()) {
        if (auto atlas = render::load_font_file(display_font)) {
            font_ = text.add_font(std::move(*atlas));
        } else {
            LOG_WARN("Splash: no display font ('{}'); the caption is not drawn", display_font);
        }
    }
    ready_ = true;
    return true;
}

void Splash::shutdown() {
    meshes_.shutdown();
    ready_ = false;
}

void Splash::draw_part(Part& part, const Affine2& transform, float pixels_per_unit, rhi::Color tint) {
    const vector::VectorAsset::Lod lod = part.asset.mesh_for_scale(pixels_per_unit);
    if (lod.revision != part.revision) {
        if (part.mesh) {
            meshes_.update_mesh(part.mesh, *lod.mesh);
        } else {
            part.mesh = meshes_.create_mesh(*lod.mesh);
        }
        part.revision = lod.revision;
    }
    meshes_.draw(part.mesh, transform, tint);
}

void Splash::render(rhi::Device& device, render::SdfBatcher& shapes, render::TextRenderer& text, ivec2 size,
                    brand::Theme theme, float opacity, std::string_view caption) {
    if (!ready_ || size.x() <= 0 || size.y() <= 0 || opacity <= 0.0f) return;
    const brand::ThemeColors& colors = brand::colors(theme);
    const rhi::Rect all{0, 0, size.x(), size.y()};
    device.set_viewport(all);
    device.set_scissor(all);
    const vec2 extent{static_cast<float>(size.x()), static_cast<float>(size.y())};
    const Affine2 world_to_ndc = render::Camera2D::screen(extent).view_projection(extent);

    // The page.
    shapes.clear();
    shapes.rounded_rect(extent * 0.5f, extent * 0.5f, 0.0f, 0.0f, brand::with_alpha(colors.bg, opacity));
    shapes.flush(world_to_ndc, 1.0f);

    // The lockup, scaled to the window (pixels are world units here).
    auto& parts = parts_[static_cast<std::size_t>(theme)];
    const vec2 mark_size = parts[0].asset.document().size;
    const vec2 word_size = parts[1].asset.document().size;
    const float word_scale = wordmark_height * mark_size.y() / std::max(word_size.y(), 1e-3f); // wordmark units -> mark units
    const float lockup_units = mark_size.x() + lockup_gap * mark_size.y() + word_size.x() * word_scale;
    const float width = std::min(extent.x() * 0.56f, 960.0f);
    const float s = width / lockup_units; // pixels per mark unit
    const float mark_h = mark_size.y() * s;
    const vec2 origin{(extent.x() - width) * 0.5f, extent.y() * 0.44f - mark_h * 0.5f};
    const rhi::Color tint{1.0f, 1.0f, 1.0f, opacity};
    meshes_.clear();
    draw_part(parts[0], Affine2::translate(origin) * Affine2::scale({s, s}), s, tint);
    const float ws = s * word_scale;
    const vec2 word_origin{origin.x() + (mark_size.x() + lockup_gap * mark_size.y()) * s,
                           origin.y() + (mark_h - word_size.y() * ws) * 0.5f};
    draw_part(parts[1], Affine2::translate(word_origin) * Affine2::scale({ws, ws}), ws, tint);
    meshes_.flush(world_to_ndc);

    // Caption under the lockup, in the display face.
    if (font_ && !caption.empty()) {
        text.clear();
        render::TextStyle style;
        style.size = std::max(14.0f, mark_h * 0.2f);
        style.align = render::TextAlign::Center;
        text.draw(font_, caption, vec2{extent.x() * 0.5f, origin.y() + mark_h * 1.55f}, style,
                  brand::with_alpha(colors.text_secondary, opacity));
        text.flush(world_to_ndc);
    }
}

void Splash::render_lockup(rhi::Device& device, ivec2 size, brand::Theme theme, vec2 center, float width) {
    if (!ready_ || size.x() <= 0 || size.y() <= 0 || width <= 0.0f) return;
    const rhi::Rect all{0, 0, size.x(), size.y()};
    device.set_viewport(all);
    device.set_scissor(all);
    const vec2 extent{static_cast<float>(size.x()), static_cast<float>(size.y())};
    const Affine2 world_to_ndc = render::Camera2D::screen(extent).view_projection(extent);
    auto& parts = parts_[static_cast<std::size_t>(theme)];
    const vec2 mark_size = parts[0].asset.document().size;
    const vec2 word_size = parts[1].asset.document().size;
    const float word_scale = wordmark_height * mark_size.y() / std::max(word_size.y(), 1e-3f);
    const float lockup_units = mark_size.x() + lockup_gap * mark_size.y() + word_size.x() * word_scale;
    const float s = width / lockup_units;
    const float mark_h = mark_size.y() * s;
    const vec2 origin{center.x() - width * 0.5f, center.y() - mark_h * 0.5f};
    const rhi::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    meshes_.clear();
    draw_part(parts[0], Affine2::translate(origin) * Affine2::scale({s, s}), s, tint);
    const float ws = s * word_scale;
    const vec2 word_origin{origin.x() + (mark_size.x() + lockup_gap * mark_size.y()) * s,
                           origin.y() + (mark_h - word_size.y() * ws) * 0.5f};
    draw_part(parts[1], Affine2::translate(word_origin) * Affine2::scale({ws, ws}), ws, tint);
    meshes_.flush(world_to_ndc);
}

} // namespace cuervo::editor

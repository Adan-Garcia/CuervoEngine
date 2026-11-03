#include "editor/scene_painter.hpp"

#include <algorithm>
#include <cmath>

#include "cuervo/core/log.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/script/game_session.hpp"
#include "cuervo/script/script_host.hpp"
#include "editor/scene_input.hpp"

namespace cuervo::editor {

bool SceneResources::init(rhi::Device& device, const render::FontAtlas* atlas) {
    if (!shapes.init(device, 4096) || !lights.init(device) || !text.init(device) || !meshes.init(device) ||
        !sprites.init(device)) {
        return false;
    }
    library.init(&device);
    if (atlas != nullptr) font = text.add_font(*atlas);
    return true;
}

SceneTargets SceneResources::targets(rhi::Device& device) {
    return {&device, &shapes, &lights, &text, &meshes, &vectors, &sprites, &library, font};
}

rhi::Rect clamp_rect(rhi::Rect r, int target_w, int target_h) {
    const int x0 = std::clamp(r.x, 0, std::max(target_w, 0));
    const int y0 = std::clamp(r.y, 0, std::max(target_h, 0));
    const int x1 = std::clamp(r.x + r.width, 0, std::max(target_w, 0));
    const int y1 = std::clamp(r.y + r.height, 0, std::max(target_h, 0));
    return {x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0)};
}

namespace {
int grid_lines_guard(float lo, float hi, float step) {
    return step > 0.0f ? static_cast<int>((hi - lo) / step) : 0;
}
} // namespace

void paint_scene_view(const SceneTargets& t, const ScenePaintParams& p, rhi::Rect view) {
    if (view.width <= 0 || view.height <= 0) return;
    rhi::Device& device = *t.device;
    render::SdfBatcher& shapes = *t.shapes;
    render::TextRenderer& text = *t.text;
    device.set_viewport(view);
    device.set_scissor(view);
    // Around the game's screen: the theme's subtle page color.
    const brand::ThemeColors& ui = *p.colors;
    device.clear(ui.bg_subtle);
    const vec2 size{static_cast<float>(view.width), static_cast<float>(view.height)};
    const Affine2 world_to_ndc = p.camera.world_to_ndc(size);
    const float upp = p.camera.world_units_per_pixel();
    shapes.clear();
    text.clear();

    // The game's screen (virtual coordinates) in its clear color.
    const vec2 screen = p.virtual_size;
    shapes.rounded_rect(screen * 0.5f, screen * 0.5f, 0.0f, 0.0f, p.clear_color);
    shapes.flush(world_to_ndc, upp);

    // The scene itself, drawn by its nodes' draw() exactly as in the game.
    script::ScriptHost* drawn = p.drawn;
    if (drawn != nullptr && !drawn->panicked()) {
#if CUERVO_FEATURE_PHYSICS
        drawn->physics_world().set_editor_debug_shapes(p.collision_shapes);
#endif
        // The editor's camera, not the scene's Camera2D (cameras off).
        t.meshes->clear();
        t.sprites->clear();
        drawn->draw(script::make_draw_targets(shapes, text, t.font, t.vectors, world_to_ndc, upp > 0.0f ? 1.0f / upp : 1.0f,
                                              false, p.lighting ? t.lights : nullptr, t.library, t.sprites));
        if (drawn->panicked()) LOG_WARN("scene view: {}", drawn->error());
    }
    shapes.flush(world_to_ndc, upp);
    text.flush(world_to_ndc);

    // A faint grid over the scene (a level's backdrop would hide one drawn under it), in world units,
    // lines one pixel wide at any zoom.
    const vec2 lo = p.camera.screen_to_world({0.0f, 0.0f}, {0.0f, 0.0f}, size);
    const vec2 hi = p.camera.screen_to_world(size, {0.0f, 0.0f}, size);
    const float step = grid_step(p.camera.zoom);
    const float half_px = 0.5f * upp;
    const rhi::Color minor = brand::with_alpha(ui.text_muted, 0.08f);
    const rhi::Color major = brand::with_alpha(ui.text_muted, 0.18f);
    const rhi::Color axis = brand::with_alpha(ui.info, 0.55f);
    const vec2 mid = (lo + hi) * 0.5f;
    const vec2 half_extent = (hi - lo) * 0.5f;
    if (grid_lines_guard(lo.x(), hi.x(), step) < 4000 && grid_lines_guard(lo.y(), hi.y(), step) < 4000) {
        for (float x = std::floor(lo.x() / step) * step; x <= hi.x(); x += step) {
            const bool is_major = std::fmod(std::abs(x), step * 4.0f) < 0.5f * step;
            shapes.rounded_rect({x, mid.y()}, {half_px, half_extent.y()}, 0.0f, 0.0f,
                                x == 0.0f ? axis : (is_major ? major : minor));
        }
        for (float y = std::floor(lo.y() / step) * step; y <= hi.y(); y += step) {
            const bool is_major = std::fmod(std::abs(y), step * 4.0f) < 0.5f * step;
            shapes.rounded_rect({mid.x(), y}, {half_extent.x(), half_px}, 0.0f, 0.0f,
                                y == 0.0f ? axis : (is_major ? major : minor));
        }
    }
    // The screen's frame on top of the grid.
    const rhi::Color frame = brand::with_alpha(ui.accent, 0.9f);
    const float w = 1.5f * upp;
    shapes.rounded_rect({screen.x() * 0.5f, 0.0f}, {screen.x() * 0.5f + w, w}, 0.0f, 0.0f, frame);
    shapes.rounded_rect({screen.x() * 0.5f, screen.y()}, {screen.x() * 0.5f + w, w}, 0.0f, 0.0f, frame);
    shapes.rounded_rect({0.0f, screen.y() * 0.5f}, {w, screen.y() * 0.5f + w}, 0.0f, 0.0f, frame);
    shapes.rounded_rect({screen.x(), screen.y() * 0.5f}, {w, screen.y() * 0.5f + w}, 0.0f, 0.0f, frame);
    if (p.overlay != nullptr) p.overlay->draw_overlay(shapes, ui, upp);
    shapes.flush(world_to_ndc, upp);
}

} // namespace cuervo::editor

namespace cuervo::editor {

void paint_game_view(const SceneTargets& t, script::GameSession& session, rhi::Rect view) {
    rhi::Device& device = *t.device;
    device.set_viewport(view);
    device.set_scissor(view);
    device.clear({0.0f, 0.0f, 0.0f, 1.0f});
    const script::Project& project = session.project();
    render::ViewportFit fit = render::fit_viewport(render::ScaleMode::Fit, project.virtual_size, {view.width, view.height});
    fit.viewport.x += view.x;
    fit.viewport.y += view.y;
    device.set_viewport(fit.viewport);
    device.set_scissor(fit.viewport);
    device.clear(project.clear_color);
    t.shapes->clear();
    t.text->clear();
    const float pixels = std::min(fit.scale.x(), fit.scale.y());
    const vec2 extent{static_cast<float>(project.virtual_size.x()), static_cast<float>(project.virtual_size.y())};
    // The game's own view: its Camera2D composes with the screen transform.
    t.meshes->clear();
    t.sprites->clear();
    session.host().draw(script::make_draw_targets(*t.shapes, *t.text, t.font, t.vectors, render::virtual_to_ndc(extent), pixels, true,
                                                  t.lights, t.library, t.sprites));
}

} // namespace cuervo::editor

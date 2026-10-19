#include "editor/view_camera.hpp"

#include <algorithm>

namespace cuervo::editor {

vec2 ViewCamera::world_to_screen(vec2 world, vec2 view_min, vec2 view_size) const {
    return view_min + view_size * 0.5f + (world - center) * zoom;
}

vec2 ViewCamera::screen_to_world(vec2 screen, vec2 view_min, vec2 view_size) const {
    return center + (screen - view_min - view_size * 0.5f) / zoom;
}

Affine2 ViewCamera::world_to_ndc(vec2 view_size) const {
    // world -> view pixels (origin top-left) -> NDC (x right, y up).
    const vec2 w{std::max(view_size.x(), 1.0f), std::max(view_size.y(), 1.0f)};
    const Affine2 to_pixels = Affine2::translate(w * 0.5f) * Affine2::scale({zoom, zoom}) * Affine2::translate(-center);
    const Affine2 to_ndc = Affine2::translate({-1.0f, 1.0f}) * Affine2::scale({2.0f / w.x(), -2.0f / w.y()});
    return to_ndc * to_pixels;
}

void ViewCamera::zoom_at(vec2 screen, float factor, vec2 view_min, vec2 view_size) {
    const vec2 anchor = screen_to_world(screen, view_min, view_size);
    zoom = std::clamp(zoom * factor, min_zoom, max_zoom);
    // Keep `anchor` under the cursor.
    center = anchor - (screen - view_min - view_size * 0.5f) / zoom;
}

void ViewCamera::frame(vec2 world_min, vec2 world_size, vec2 view_size) {
    const float margin = 0.9f;
    const float zx = view_size.x() * margin / std::max(world_size.x(), 1.0f);
    const float zy = view_size.y() * margin / std::max(world_size.y(), 1.0f);
    zoom = std::clamp(std::min(zx, zy), min_zoom, max_zoom);
    center = world_min + world_size * 0.5f;
}

float grid_step(float zoom, float min_pixels) {
    float step = 16.0f;
    while (step * zoom < min_pixels && step < 1.0e6f) step *= 2.0f;
    while (step * zoom >= min_pixels * 2.0f && step > 1.0f) step *= 0.5f;
    return step;
}

int pick_node(std::span<const NodeBounds> bounds, vec2 world, float radius) {
    int best = -1;
    float best_area = 0.0f;
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const NodeBounds& b = bounds[i];
        if (!b.has_rect) continue;
        const vec2 hi = b.rect_min + b.rect_size;
        if (world.x() < b.rect_min.x() || world.y() < b.rect_min.y() || world.x() > hi.x() || world.y() > hi.y()) {
            continue;
        }
        const float area = b.rect_size.x() * b.rect_size.y();
        if (best < 0 || area <= best_area) {
            best = static_cast<int>(i);
            best_area = area;
        }
    }
    if (best >= 0) return best;
    float best_d2 = radius * radius;
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const NodeBounds& b = bounds[i];
        if (!b.has_position || b.has_rect) continue;
        const vec2 d = b.position - world;
        const float d2 = dot(d, d);
        if (d2 <= best_d2) {
            best = static_cast<int>(i);
            best_d2 = d2;
        }
    }
    return best;
}

} // namespace cuervo::editor

#pragma once

#include <span>

#include "cuervo/math/affine2.hpp"
#include "cuervo/math/vec.hpp"
#include "editor/project_env.hpp"

// The scene view's camera (DESIGN.md 4.13 "vector-aware zoom"). The view is
// a rectangle of framebuffer pixels; the camera maps world units onto it with
// a uniform zoom. Zooming changes the world -> NDC transform the engine's
// renderer draws with, never a bitmap: SDF shapes and MSDF text are
// re-evaluated per pixel at the new scale (and the SDF anti-aliasing width
// follows `world_units_per_pixel`), so vectors stay sharp at any zoom.
namespace cuervo::editor {

struct ViewCamera {
    vec2 center{640.0f, 360.0f}; // world point at the middle of the view
    float zoom = 1.0f;           // framebuffer pixels per world unit

    static constexpr float min_zoom = 0.05f;
    static constexpr float max_zoom = 64.0f;

    // `view_min`/`view_size`: the view rectangle in framebuffer pixels.
    vec2 world_to_screen(vec2 world, vec2 view_min, vec2 view_size) const;
    vec2 screen_to_world(vec2 screen, vec2 view_min, vec2 view_size) const;
    // World -> NDC of the view rectangle (y up), for the batchers.
    Affine2 world_to_ndc(vec2 view_size) const;
    float world_units_per_pixel() const { return 1.0f / zoom; }

    // Zooms by `factor` keeping the world point under `screen` fixed.
    void zoom_at(vec2 screen, float factor, vec2 view_min, vec2 view_size);
    // Fits a world rectangle (with a margin) into the view.
    void frame(vec2 world_min, vec2 world_size, vec2 view_size);
};

// Grid spacing in world units: a power-of-two multiple of 16 whose lines are
// at least `min_pixels` apart at `zoom`.
float grid_step(float zoom, float min_pixels = 12.0f);

// The node (document index) under `world`, or -1: a Control whose rect
// contains the point (the smallest wins, then the later one in draw order),
// else the Node2D whose position is nearest within `radius` world units.
int pick_node(std::span<const NodeBounds> bounds, vec2 world, float radius);

} // namespace cuervo::editor

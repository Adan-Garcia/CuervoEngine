#pragma once

#include "cuervo/math/affine2.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/rhi/rhi.hpp"

// 2D camera and virtual-resolution scaling (DESIGN.md 4.4).
//
// Coordinate conventions (user decision): world and virtual coordinates are
// Y-DOWN with the origin at the top-left (like SVG and the screen), units are
// pixels of the virtual coordinate space, angles are radians. With y down, a
// positive rotation turns clockwise on screen.
namespace cuervo::render {

// The 2D affine transform lives in cuervo/math (shared with cuervo/vector).
using cuervo::Affine2;

// Maps virtual coordinates ([0, size], y down) to NDC ([-1, 1], y up). The RHI
// guarantees NDC +1 is the top on every backend.
Affine2 virtual_to_ndc(vec2 virtual_size);

class Camera2D {
public:
    vec2 position{0.0f, 0.0f}; // world point shown at the center of the view
    float zoom = 1.0f;         // > 1 magnifies
    float rotation = 0.0f;     // radians; positive turns the camera clockwise (world appears counter-clockwise)

    // A camera that shows world [0, virtual_size] exactly, like screen space.
    static Camera2D screen(vec2 virtual_size);

    // World -> virtual coordinates.
    Affine2 world_to_virtual(vec2 virtual_size) const;
    // World -> NDC: what shaders multiply positions by.
    Affine2 view_projection(vec2 virtual_size) const;
    vec2 virtual_to_world(vec2 p, vec2 virtual_size) const { return world_to_virtual(virtual_size).inverse().apply(p); }
};

// How the virtual coordinate space maps onto the framebuffer (DESIGN.md 4.4).
enum class ScaleMode {
    Fit,     // default: uniform scale to fit, letterboxed; vectors render at native resolution
    Integer, // largest whole-number uniform scale (pixel art), letterboxed
    Stretch, // fill the framebuffer, non-uniform scale
};

struct ViewportFit {
    rhi::Rect viewport;   // framebuffer pixels, top-left origin; may exceed the framebuffer in Integer mode
    vec2 scale{1.0f, 1.0f}; // framebuffer pixels per virtual unit
};

ViewportFit fit_viewport(ScaleMode mode, ivec2 virtual_size, ivec2 framebuffer_size);

// Framebuffer pixel -> virtual coordinates (e.g. for mouse input).
vec2 framebuffer_to_virtual(const ViewportFit& fit, vec2 pixel);

} // namespace cuervo::render

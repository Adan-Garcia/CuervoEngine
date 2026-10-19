#include "cuervo/render/camera.hpp"

#include <algorithm>

#include "cuervo/math/dmath.hpp"

namespace cuervo::render {

Affine2 virtual_to_ndc(vec2 virtual_size) {
    const float sx = virtual_size.x() > 0.0f ? 2.0f / virtual_size.x() : 0.0f;
    const float sy = virtual_size.y() > 0.0f ? 2.0f / virtual_size.y() : 0.0f;
    return {sx, 0.0f, 0.0f, -sy, -1.0f, 1.0f};
}

Camera2D Camera2D::screen(vec2 virtual_size) {
    Camera2D camera;
    camera.position = virtual_size * 0.5f;
    return camera;
}

Affine2 Camera2D::world_to_virtual(vec2 virtual_size) const {
    // Move the camera to the origin, undo its rotation, zoom, then put the
    // origin at the center of the view.
    return Affine2::translate(virtual_size * 0.5f) * Affine2::scale(vec2(zoom)) * Affine2::rotate(-rotation) *
           Affine2::translate(-position);
}

Affine2 Camera2D::view_projection(vec2 virtual_size) const {
    return virtual_to_ndc(virtual_size) * world_to_virtual(virtual_size);
}

ViewportFit fit_viewport(ScaleMode mode, ivec2 virtual_size, ivec2 framebuffer_size) {
    const int vw = std::max(virtual_size.x(), 1);
    const int vh = std::max(virtual_size.y(), 1);
    const int fw = std::max(framebuffer_size.x(), 0);
    const int fh = std::max(framebuffer_size.y(), 0);
    const float rx = static_cast<float>(fw) / static_cast<float>(vw);
    const float ry = static_cast<float>(fh) / static_cast<float>(vh);

    ViewportFit fit;
    switch (mode) {
        case ScaleMode::Stretch:
            fit.viewport = {0, 0, fw, fh};
            fit.scale = {rx, ry};
            return fit;
        case ScaleMode::Fit: {
            const float s = std::min(rx, ry);
            const int w = std::min(fw, static_cast<int>(static_cast<float>(vw) * s + 0.5f));
            const int h = std::min(fh, static_cast<int>(static_cast<float>(vh) * s + 0.5f));
            fit.viewport = {(fw - w) / 2, (fh - h) / 2, w, h};
            fit.scale = {static_cast<float>(w) / static_cast<float>(vw), static_cast<float>(h) / static_cast<float>(vh)};
            return fit;
        }
        case ScaleMode::Integer: {
            // At least 1x: a framebuffer smaller than the virtual space crops
            // (centered) rather than shrinking pixel art.
            const int s = std::max(1, std::min(fw / vw, fh / vh));
            const int w = vw * s;
            const int h = vh * s;
            fit.viewport = {(fw - w) / 2, (fh - h) / 2, w, h};
            fit.scale = vec2(static_cast<float>(s));
            return fit;
        }
    }
    return fit;
}

vec2 framebuffer_to_virtual(const ViewportFit& fit, vec2 pixel) {
    const vec2 origin{static_cast<float>(fit.viewport.x), static_cast<float>(fit.viewport.y)};
    return (pixel - origin) / fit.scale;
}

} // namespace cuervo::render

#pragma once

#include "cuervo/math/dmath.hpp"
#include "cuervo/math/vec.hpp"

namespace cuervo {

// 2D affine transform: p' = (a*x + c*y + tx, b*x + d*y + ty). Same layout as
// an SVG matrix(a b c d e f). Y-down conventions (DESIGN.md 7.1): a positive
// rotation turns clockwise on screen.
struct Affine2 {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;

    static constexpr Affine2 translate(vec2 t) { return {1.0f, 0.0f, 0.0f, 1.0f, t.x(), t.y()}; }
    static constexpr Affine2 scale(vec2 s) { return {s.x(), 0.0f, 0.0f, s.y(), 0.0f, 0.0f}; }
    // Positive angles turn clockwise on a y-down screen.
    static Affine2 rotate(float radians) {
        // dmath, not libm: render code should look the same on every platform too.
        const float co = dmath::cos(radians);
        const float si = dmath::sin(radians);
        return {co, si, -si, co, 0.0f, 0.0f};
    }

    constexpr vec2 apply(vec2 p) const { return {a * p.x() + c * p.y() + tx, b * p.x() + d * p.y() + ty}; }
    // Applies only the linear part (directions, extents).
    constexpr vec2 apply_vector(vec2 v) const { return {a * v.x() + c * v.y(), b * v.x() + d * v.y()}; }
    constexpr float determinant() const { return a * d - b * c; }

    // Returns the identity when the transform isn't invertible.
    constexpr Affine2 inverse() const {
        const float det = determinant();
        if (det == 0.0f) {
            return {};
        }
        const float inv = 1.0f / det;
        const float ia = d * inv;
        const float ib = -b * inv;
        const float ic = -c * inv;
        const float id = a * inv;
        return {ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
    }

    // (lhs * rhs).apply(p) == lhs.apply(rhs.apply(p))
    friend constexpr Affine2 operator*(const Affine2& l, const Affine2& r) {
        return {l.a * r.a + l.c * r.b,         l.b * r.a + l.d * r.b,
                l.a * r.c + l.c * r.d,         l.b * r.c + l.d * r.d,
                l.a * r.tx + l.c * r.ty + l.tx, l.b * r.tx + l.d * r.ty + l.ty};
    }
    friend constexpr bool operator==(const Affine2&, const Affine2&) = default;
};

} // namespace cuervo

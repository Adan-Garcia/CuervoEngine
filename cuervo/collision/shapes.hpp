#pragma once

#include <algorithm>

#include "cuervo/math/vec.hpp"

// Collision shapes (DESIGN.md 4.7). Everything here is plain + - * / on f32
// (no libm, no sqrt in the overlap tests), so results are bit-identical for
// the same inputs on the same build (DESIGN.md 4.1). Shapes touching exactly
// at their boundary count as overlapping.
namespace cuervo::collision {

struct Circle {
    vec2 center;
    float radius = 0.0f;
};

// The set of points within `radius` of the segment a-b: lasers, elongated
// bullets. A capsule with a == b is a circle.
struct Capsule {
    vec2 a;
    vec2 b;
    float radius = 0.0f;
};

struct Aabb {
    vec2 min;
    vec2 max;

    constexpr bool overlaps(const Aabb& o) const {
        return min.x() <= o.max.x() && o.min.x() <= max.x() && min.y() <= o.max.y() && o.min.y() <= max.y();
    }
};

constexpr Aabb bounds(const Circle& c) {
    return {c.center - vec2(c.radius), c.center + vec2(c.radius)};
}

constexpr Aabb bounds(const Capsule& c) {
    return {cuervo::min(c.a, c.b) - vec2(c.radius), cuervo::max(c.a, c.b) + vec2(c.radius)};
}

// Squared distance from p to the segment a-b.
constexpr float distance_sq_point_segment(vec2 p, vec2 a, vec2 b) {
    const vec2 ab = b - a;
    const float len_sq = dot(ab, ab);
    float t = 0.0f;
    if (len_sq > 0.0f) {
        t = std::clamp(dot(p - a, ab) / len_sq, 0.0f, 1.0f);
    }
    const vec2 d = p - (a + ab * t);
    return dot(d, d);
}

// Squared distance between segments p1-q1 and p2-q2 (Ericson, Real-Time
// Collision Detection 5.1.9), with the degenerate cases handled explicitly.
constexpr float distance_sq_segment_segment(vec2 p1, vec2 q1, vec2 p2, vec2 q2) {
    const vec2 d1 = q1 - p1;
    const vec2 d2 = q2 - p2;
    const vec2 r = p1 - p2;
    const float a = dot(d1, d1);
    const float e = dot(d2, d2);
    const float f = dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= 0.0f && e <= 0.0f) {
        return dot(r, r);
    }
    if (a <= 0.0f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = dot(d1, r);
        if (e <= 0.0f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > 0.0f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    const vec2 diff = (p1 + d1 * s) - (p2 + d2 * t);
    return dot(diff, diff);
}

constexpr bool overlaps(const Circle& x, const Circle& y) {
    const vec2 d = x.center - y.center;
    const float r = x.radius + y.radius;
    return dot(d, d) <= r * r;
}

constexpr bool overlaps(const Circle& x, const Capsule& y) {
    const float r = x.radius + y.radius;
    return distance_sq_point_segment(x.center, y.a, y.b) <= r * r;
}

constexpr bool overlaps(const Capsule& x, const Circle& y) { return overlaps(y, x); }

constexpr bool overlaps(const Capsule& x, const Capsule& y) {
    const float r = x.radius + y.radius;
    return distance_sq_segment_segment(x.a, x.b, y.a, y.b) <= r * r;
}

} // namespace cuervo::collision

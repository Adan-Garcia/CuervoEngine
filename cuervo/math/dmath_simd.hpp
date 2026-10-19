#pragma once

#include <limits>

#include "cuervo/math/dmath.hpp"
#include "cuervo/math/simd.hpp"

// SIMD versions of dmath's transcendental functions (DESIGN.md 4.15). They
// mirror cuervo/math/dmath.cpp operation for operation (same constants, same
// Horner order, branches as selects), so every lane is bit-identical to the
// scalar function; tests/cuervo/simd_tests.cpp checks this over wide ranges.
// Keep the two files in sync: a change to one is a change to both.
#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2) || defined(CUERVO_SIMD_WASM)

namespace cuervo::dmath::simd {

using cuervo::simd::F;
using cuervo::simd::M;
using cuervo::simd::select;
using cuervo::simd::splat;

inline F reduce(F x) {
    const F k = cuervo::simd::floor(x * splat(0.159154943091895335769f) + splat(0.5f));
    return (x - k * splat(6.28125f)) - k * splat(1.9353071795864769e-3f);
}

inline F sin_poly(F r) {
    const F r2 = r * r;
    F p = splat(-2.5052108385441718775e-8f);
    p = p * r2 + splat(2.7557319223985890653e-6f);
    p = p * r2 - splat(1.9841269841269841270e-4f);
    p = p * r2 + splat(8.3333333333333333333e-3f);
    p = p * r2 - splat(1.6666666666666666667e-1f);
    return r + r * r2 * p;
}

inline F cos_poly(F r) {
    const F r2 = r * r;
    F p = splat(2.0876756987868098979e-9f);
    p = p * r2 - splat(2.7557319223985890653e-7f);
    p = p * r2 + splat(2.4801587301587301587e-5f);
    p = p * r2 - splat(1.3888888888888888889e-3f);
    p = p * r2 + splat(4.1666666666666666667e-2f);
    p = p * r2 - splat(0.5f);
    return splat(1.0f) + r2 * p;
}

inline F atan_poly(F t) {
    const F t2 = t * t;
    F p = splat(1.0f / 17.0f);
    p = p * t2 - splat(1.0f / 15.0f);
    p = p * t2 + splat(1.0f / 13.0f);
    p = p * t2 - splat(1.0f / 11.0f);
    p = p * t2 + splat(1.0f / 9.0f);
    p = p * t2 - splat(1.0f / 7.0f);
    p = p * t2 + splat(1.0f / 5.0f);
    p = p * t2 - splat(1.0f / 3.0f);
    return t + t * t2 * p;
}

inline F atan_unit(F x) {
    const M upper = x > splat(0.41421356237309504880f);
    const F folded = splat(0.78539816339744830962f) + atan_poly((x - splat(1.0f)) / (x + splat(1.0f)));
    return select(upper, folded, atan_poly(x));
}

inline F sin(F x) {
    F r = reduce(x);
    r = select(r > splat(half_pi), splat(pi) - r, select(r < splat(-half_pi), splat(-pi) - r, r));
    return sin_poly(r);
}

inline F cos(F x) {
    const F r = reduce(x);
    const M above = r > splat(half_pi);
    const M below = r < splat(-half_pi);
    const F c = cos_poly(select(above, splat(pi) - r, select(below, splat(-pi) - r, r)));
    return select(above | below, -c, c);
}

inline F atan2(F y, F x) {
    const F ax = cuervo::simd::abs(x);
    const F ay = cuervo::simd::abs(y);
    const M zero = (ax == splat(0.0f)) & (ay == splat(0.0f));
    F a = select(ay > ax, splat(half_pi) - atan_unit(ax / ay), atan_unit(ay / ax));
    a = select(x < splat(0.0f), splat(pi) - a, a);
    a = select(y < splat(0.0f), -a, a);
    return select(zero, splat(0.0f), a);
}

inline F tan(F x) {
    return sin(x) / cos(x);
}

inline F asin(F x) {
    const F d = cuervo::simd::max(splat(0.0f), splat(1.0f) - x * x);
    const F r = atan2(x, cuervo::simd::sqrt(d));
    return select(cuervo::simd::abs(x) > splat(1.0f), splat(std::numeric_limits<float>::quiet_NaN()), r);
}

inline F acos(F x) {
    const F d = cuervo::simd::max(splat(0.0f), splat(1.0f) - x * x);
    const F r = atan2(cuervo::simd::sqrt(d), x);
    return select(cuervo::simd::abs(x) > splat(1.0f), splat(std::numeric_limits<float>::quiet_NaN()), r);
}

inline F atan(F x) {
    return atan2(x, splat(1.0f));
}

inline F sign(F x) {
    return select(x > splat(0.0f), splat(1.0f), select(x < splat(0.0f), splat(-1.0f), splat(0.0f)));
}

inline F step(F edge, F x) {
    return select(x < edge, splat(0.0f), splat(1.0f));
}

inline F lerp(F a, F b, F t) {
    return a + (b - a) * t;
}

inline F smoothstep(F edge0, F edge1, F x) {
    const F denom = edge1 - edge0;
    const M zero_len = edge0 == edge1;
    const F t = cuervo::simd::clamp((x - edge0) / denom, splat(0.0f), splat(1.0f));
    const F smooth = t * t * (splat(3.0f) - splat(2.0f) * t);
    return select(zero_len, select(x < edge0, splat(0.0f), splat(1.0f)), smooth);
}

inline F rad_to_deg(F rad) {
    return rad * splat(rad_to_deg_factor);
}

inline F deg_to_rad(F deg) {
    return deg * splat(deg_to_rad_factor);
}

inline F wrap(F val, F min_val, F max_val) {
    const F range = max_val - min_val;
    const M zero_range = range == splat(0.0f);
    const F w = val - range * cuervo::simd::floor((val - min_val) / range);
    return select(zero_range, min_val, w);
}

inline F wrap_angle(F angle) {
    return wrap(angle + splat(pi), splat(0.0f), splat(tau)) - splat(pi);
}

} // namespace cuervo::dmath::simd

#endif

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "cuervo/math/dmath.hpp"

#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2)
#include <immintrin.h>
#define CUERVO_VEC_SIMD 1
#elif defined(CUERVO_SIMD_WASM)
#include <wasm_simd128.h>
#define CUERVO_VEC_SIMD 1
#endif

namespace cuervo {

#if defined(CUERVO_VEC_SIMD)
// Explicit SIMD for float vectors of 2..4 components (DESIGN.md 4.15): one
// 128-bit register (SSE / WASM SIMD128; AVX2 builds use the same SSE forms),
// unused lanes zero. Lane-wise IEEE operations give the same bits as the
// scalar loops; tests/cuervo/simd_tests.cpp compares them.
namespace vec_simd {
#if defined(CUERVO_SIMD_WASM)
using R = v128_t;
inline R add(R a, R b) { return wasm_f32x4_add(a, b); }
inline R sub(R a, R b) { return wasm_f32x4_sub(a, b); }
inline R mul(R a, R b) { return wasm_f32x4_mul(a, b); }
inline R div(R a, R b) { return wasm_f32x4_div(a, b); }
inline R splat(float s) { return wasm_f32x4_splat(s); }
inline R neg(R a) { return wasm_v128_xor(a, wasm_f32x4_splat(-0.0f)); }
// b < a ? b : a  /  a < b ? b : a  (std::min / std::max semantics)
inline R min(R a, R b) { return wasm_v128_bitselect(b, a, wasm_f32x4_lt(b, a)); }
inline R max(R a, R b) { return wasm_v128_bitselect(b, a, wasm_f32x4_lt(a, b)); }
inline R load4(const float* p) { return wasm_v128_load(p); }
inline void store4(float* p, R a) { wasm_v128_store(p, a); }
#else
using R = __m128;
inline R add(R a, R b) { return _mm_add_ps(a, b); }
inline R sub(R a, R b) { return _mm_sub_ps(a, b); }
inline R mul(R a, R b) { return _mm_mul_ps(a, b); }
inline R div(R a, R b) { return _mm_div_ps(a, b); }
inline R splat(float s) { return _mm_set1_ps(s); }
inline R neg(R a) { return _mm_xor_ps(a, _mm_set1_ps(-0.0f)); }
inline R select(R m, R a, R b) { return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b)); }
inline R min(R a, R b) { return select(_mm_cmplt_ps(b, a), b, a); }
inline R max(R a, R b) { return select(_mm_cmplt_ps(a, b), b, a); }
inline R load4(const float* p) { return _mm_loadu_ps(p); }
inline void store4(float* p, R a) { _mm_storeu_ps(p, a); }
#endif

template<std::size_t N>
inline R load(const float* p) {
    if constexpr (N == 4) {
        return load4(p);
    } else {
        float t[4]{};
        std::memcpy(t, p, N * sizeof(float));
        return load4(t);
    }
}

template<std::size_t N>
inline void store(float* p, R a) {
    if constexpr (N == 4) {
        store4(p, a);
    } else {
        float t[4];
        store4(t, a);
        std::memcpy(p, t, N * sizeof(float));
    }
}
} // namespace vec_simd

template<typename T, std::size_t N>
inline constexpr bool has_vec_simd = std::is_same_v<T, float> && N >= 2 && N <= 4;
#else
template<typename T, std::size_t N>
inline constexpr bool has_vec_simd = false;
#endif

// Generic N-component vector (DESIGN.md 4.2): vec1 ... vecN.
//
// The loops below are fixed-length; float vectors of 2..4 components take
// the explicit SIMD path above at run time (constant evaluation stays
// scalar), with bit-identical results (DESIGN.md 4.15).
template<typename T, std::size_t N>
struct Vec {
    static_assert(N >= 1, "Vec needs at least one component");
    std::array<T, N> v{};

    constexpr Vec() = default;

    // Splat: vec3(0) -> (0, 0, 0).
    constexpr explicit Vec(T s) { v.fill(s); }

    // Per-component: vec3(1, 2, 3).
    template<typename... Args>
        requires(N > 1 && sizeof...(Args) == N && (std::convertible_to<Args, T> && ...))
    constexpr Vec(Args... args) : v{static_cast<T>(args)...} {}

    // Extend: vec3(some_vec2, z).
    template<std::size_t M = N>
        requires(M > 1)
    constexpr Vec(const Vec<T, M - 1>& head, T last) {
        for (std::size_t i = 0; i < N - 1; ++i) {
            v[i] = head[i];
        }
        v[N - 1] = last;
    }

    static constexpr std::size_t size() { return N; }

    constexpr T& operator[](std::size_t i) { return v[i]; }
    constexpr const T& operator[](std::size_t i) const { return v[i]; }

    // Named accessors for the first four components.
    constexpr T& x() requires(N >= 1) { return v[0]; }
    constexpr T& y() requires(N >= 2) { return v[1]; }
    constexpr T& z() requires(N >= 3) { return v[2]; }
    constexpr T& w() requires(N >= 4) { return v[3]; }
    constexpr const T& x() const requires(N >= 1) { return v[0]; }
    constexpr const T& y() const requires(N >= 2) { return v[1]; }
    constexpr const T& z() const requires(N >= 3) { return v[2]; }
    constexpr const T& w() const requires(N >= 4) { return v[3]; }

    // Swizzle by index: v.swizzle<1, 0>() is v.yx.
    template<std::size_t... I>
        requires(sizeof...(I) >= 1 && ((I < N) && ...))
    constexpr Vec<T, sizeof...(I)> swizzle() const {
        Vec<T, sizeof...(I)> out;
        std::size_t k = 0;
        ((out[k++] = v[I]), ...);
        return out;
    }

    constexpr Vec<T, 2> xy() const requires(N >= 2) { return swizzle<0, 1>(); }
    constexpr Vec<T, 2> yx() const requires(N >= 2) { return swizzle<1, 0>(); }
    constexpr Vec<T, 3> xyz() const requires(N >= 3) { return swizzle<0, 1, 2>(); }

    friend constexpr bool operator==(const Vec&, const Vec&) = default;

    // --- arithmetic -------------------------------------------------------
#if defined(CUERVO_VEC_SIMD)
#define CUERVO_VEC_SIMD_OP(OP, RHS)                                                                  \
    if constexpr (has_vec_simd<T, N>) {                                                              \
        if (!std::is_constant_evaluated()) {                                                         \
            vec_simd::store<N>(v.data(), vec_simd::OP(vec_simd::load<N>(v.data()), RHS));            \
            return *this;                                                                            \
        }                                                                                            \
    }
#else
#define CUERVO_VEC_SIMD_OP(OP, RHS)
#endif
    constexpr Vec& operator+=(const Vec& o) {
        CUERVO_VEC_SIMD_OP(add, vec_simd::load<N>(o.v.data()))
        for (std::size_t i = 0; i < N; ++i) v[i] += o.v[i];
        return *this;
    }
    constexpr Vec& operator-=(const Vec& o) {
        CUERVO_VEC_SIMD_OP(sub, vec_simd::load<N>(o.v.data()))
        for (std::size_t i = 0; i < N; ++i) v[i] -= o.v[i];
        return *this;
    }
    constexpr Vec& operator*=(const Vec& o) {
        CUERVO_VEC_SIMD_OP(mul, vec_simd::load<N>(o.v.data()))
        for (std::size_t i = 0; i < N; ++i) v[i] *= o.v[i];
        return *this;
    }
    constexpr Vec& operator/=(const Vec& o) {
        CUERVO_VEC_SIMD_OP(div, vec_simd::load<N>(o.v.data()))
        for (std::size_t i = 0; i < N; ++i) v[i] /= o.v[i];
        return *this;
    }
    constexpr Vec& operator*=(T s) {
        CUERVO_VEC_SIMD_OP(mul, vec_simd::splat(s))
        for (std::size_t i = 0; i < N; ++i) v[i] *= s;
        return *this;
    }
    constexpr Vec& operator/=(T s) {
        CUERVO_VEC_SIMD_OP(div, vec_simd::splat(s))
        for (std::size_t i = 0; i < N; ++i) v[i] /= s;
        return *this;
    }
#undef CUERVO_VEC_SIMD_OP

    friend constexpr Vec operator+(Vec a, const Vec& b) { return a += b; }
    friend constexpr Vec operator-(Vec a, const Vec& b) { return a -= b; }
    friend constexpr Vec operator*(Vec a, const Vec& b) { return a *= b; }
    friend constexpr Vec operator/(Vec a, const Vec& b) { return a /= b; }
    friend constexpr Vec operator*(Vec a, T s) { return a *= s; }
    friend constexpr Vec operator*(T s, Vec a) { return a *= s; }
    friend constexpr Vec operator/(Vec a, T s) { return a /= s; }

    constexpr Vec operator-() const {
        Vec out;
#if defined(CUERVO_VEC_SIMD)
        if constexpr (has_vec_simd<T, N>) {
            if (!std::is_constant_evaluated()) {
                vec_simd::store<N>(out.v.data(), vec_simd::neg(vec_simd::load<N>(v.data())));
                return out;
            }
        }
#endif
        for (std::size_t i = 0; i < N; ++i) out.v[i] = -v[i];
        return out;
    }
};

// --- free functions --------------------------------------------------------

template<typename T, std::size_t N>
constexpr T dot(const Vec<T, N>& a, const Vec<T, N>& b) {
    T sum{};
#if defined(CUERVO_VEC_SIMD)
    if constexpr (has_vec_simd<T, N>) {
        if (!std::is_constant_evaluated()) {
            // Products in parallel, then summed in lane order like the loop.
            float p[4];
            vec_simd::store4(p, vec_simd::mul(vec_simd::load<N>(a.v.data()), vec_simd::load<N>(b.v.data())));
            for (std::size_t i = 0; i < N; ++i) sum += p[i];
            return sum;
        }
    }
#endif
    for (std::size_t i = 0; i < N; ++i) sum += a[i] * b[i];
    return sum;
}

template<typename T, std::size_t N>
constexpr T length_sq(const Vec<T, N>& a) { return dot(a, a); }

template<std::size_t N>
float length(const Vec<float, N>& a) { return dmath::sqrt(length_sq(a)); }

// Returns the zero vector for zero-length input instead of NaNs.
template<std::size_t N>
Vec<float, N> normalize(const Vec<float, N>& a) {
    float len = length(a);
    return len > 0.0f ? a / len : Vec<float, N>{};
}

template<typename T, std::size_t N>
constexpr Vec<T, N> min(const Vec<T, N>& a, const Vec<T, N>& b) {
    Vec<T, N> out;
#if defined(CUERVO_VEC_SIMD)
    if constexpr (has_vec_simd<T, N>) {
        if (!std::is_constant_evaluated()) {
            vec_simd::store<N>(out.v.data(), vec_simd::min(vec_simd::load<N>(a.v.data()), vec_simd::load<N>(b.v.data())));
            return out;
        }
    }
#endif
    for (std::size_t i = 0; i < N; ++i) out[i] = b[i] < a[i] ? b[i] : a[i];
    return out;
}

template<typename T, std::size_t N>
constexpr Vec<T, N> max(const Vec<T, N>& a, const Vec<T, N>& b) {
    Vec<T, N> out;
#if defined(CUERVO_VEC_SIMD)
    if constexpr (has_vec_simd<T, N>) {
        if (!std::is_constant_evaluated()) {
            vec_simd::store<N>(out.v.data(), vec_simd::max(vec_simd::load<N>(a.v.data()), vec_simd::load<N>(b.v.data())));
            return out;
        }
    }
#endif
    for (std::size_t i = 0; i < N; ++i) out[i] = a[i] < b[i] ? b[i] : a[i];
    return out;
}

template<std::size_t N>
constexpr Vec<float, N> lerp(const Vec<float, N>& a, const Vec<float, N>& b, float t) {
    return a + (b - a) * t;
}

// --- aliases ---------------------------------------------------------------

template<std::size_t N> using vecn = Vec<float, N>;
template<std::size_t N> using ivecn = Vec<std::int32_t, N>;

using vec1 = Vec<float, 1>;
using vec2 = Vec<float, 2>;
using vec3 = Vec<float, 3>;
using vec4 = Vec<float, 4>;
using ivec1 = Vec<std::int32_t, 1>;
using ivec2 = Vec<std::int32_t, 2>;
using ivec3 = Vec<std::int32_t, 3>;
using ivec4 = Vec<std::int32_t, 4>;

// --- geometric utilities ---------------------------------------------------

// 2D perpendicular dot product
inline constexpr float cross(const vec2& a, const vec2& b) {
    return a.x() * b.y() - a.y() * b.x();
}

// 3D cross product
inline constexpr vec3 cross(const vec3& a, const vec3& b) {
    return vec3(a.y() * b.z() - a.z() * b.y(),
                a.z() * b.x() - a.x() * b.z(),
                a.x() * b.y() - a.y() * b.x());
}

template<std::size_t N>
float distance(const Vec<float, N>& a, const Vec<float, N>& b) {
    return length(a - b);
}

template<typename T, std::size_t N>
constexpr T distance_sq(const Vec<T, N>& a, const Vec<T, N>& b) {
    return length_sq(a - b);
}

template<std::size_t N>
constexpr Vec<float, N> reflect(const Vec<float, N>& v, const Vec<float, N>& normal) {
    return v - 2.0f * dot(v, normal) * normal;
}

template<std::size_t N>
constexpr Vec<float, N> project(const Vec<float, N>& v, const Vec<float, N>& onto) {
    float d = dot(onto, onto);
    return d > 0.0f ? onto * (dot(v, onto) / d) : Vec<float, N>{};
}

template<std::size_t N>
constexpr Vec<float, N> slide(const Vec<float, N>& v, const Vec<float, N>& normal) {
    return v - project(v, normal);
}

inline constexpr vec2 perpendicular(const vec2& v) {
    return vec2(-v.y(), v.x());
}

inline float angle_to(const vec2& a, const vec2& b) {
    return dmath::atan2(cross(a, b), dot(a, b));
}

template<typename T, std::size_t N>
constexpr Vec<T, N> clamp(const Vec<T, N>& v, const Vec<T, N>& lo, const Vec<T, N>& hi) {
    return min(max(v, lo), hi);
}

template<std::size_t N>
Vec<float, N> clamp_length(const Vec<float, N>& v, float min_len, float max_len) {
    float len = length(v);
    if (len == 0.0f) return Vec<float, N>{};
    float clamped = dmath::clamp(len, min_len, max_len);
    return v * (clamped / len);
}

// Spherical linear interpolation between two 2D vectors along the circular arc between them.
inline vec2 slerp(const vec2& a, const vec2& b, float t) {
    float theta = angle_to(a, b);
    if (dmath::abs(theta) < 1e-6f) {
        return lerp(a, b, t);
    }
    float len = dmath::lerp(length(a), length(b), t);
    float angle = dmath::atan2(a.y(), a.x()) + theta * t;
    return vec2(dmath::cos(angle), dmath::sin(angle)) * len;
}

} // namespace cuervo

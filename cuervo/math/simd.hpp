#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

// Explicit SIMD (DESIGN.md 4.15): a thin wrapper over AVX2, SSE2 or WASM
// SIMD128, chosen at build time (CMake option CUERVO_SIMD, which defines one
// of CUERVO_SIMD_AVX2 / CUERVO_SIMD_SSE2 / CUERVO_SIMD_WASM). Hot SoA loops
// are written once against `simd::F` (a vector of `width` floats) and run
// width lanes at a time, with the scalar code handling the remainder.
//
// Bit-identity with the scalar code is a hard rule (DESIGN.md 4.1, 4.15):
//   - only correctly rounded IEEE operations (+ - * / sqrt), compares,
//     selects, floor and exact conversions; no FMA (and -ffp-contract=off),
//     no approximate rsqrt/rcp, no horizontal reassociation;
//   - every scalar expression is mirrored with the same operations in the
//     same order; branches become selects of the same values;
//   - std::min/max/clamp are written as the selects they are defined as
//     (`b < a ? b : a`), so NaN and signed-zero cases agree too.
// tests/cuervo/simd_tests.cpp runs SIMD and scalar paths on the same inputs
// and requires identical bits.
namespace cuervo::simd {

#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2) || defined(CUERVO_SIMD_WASM)
inline constexpr bool available = true;
#else
inline constexpr bool available = false;
#endif

#if defined(CUERVO_SIMD_AVX2)
inline constexpr int width = 8;
inline constexpr const char* isa = "avx2";
#elif defined(CUERVO_SIMD_SSE2)
inline constexpr int width = 4;
inline constexpr const char* isa = "sse2";
#elif defined(CUERVO_SIMD_WASM)
inline constexpr int width = 4;
inline constexpr const char* isa = "wasm-simd128";
#else
inline constexpr int width = 1;
inline constexpr const char* isa = "none";
#endif

// Runtime switch for the SoA kernels (tests and `--simd off` compare the two
// paths in one binary). Always false when no SIMD ISA is compiled in.
namespace detail {
inline std::atomic<bool> enabled{available};
}
inline bool enabled() { return available && detail::enabled.load(std::memory_order_relaxed); }
inline void set_enabled(bool on) { detail::enabled.store(on && available, std::memory_order_relaxed); }

} // namespace cuervo::simd

#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2)
#include <immintrin.h>
#elif defined(CUERVO_SIMD_WASM)
#include <wasm_simd128.h>
#endif

#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2) || defined(CUERVO_SIMD_WASM)

namespace cuervo::simd {

#if defined(CUERVO_SIMD_AVX2)
using NativeF = __m256;
using NativeI = __m256i;
#elif defined(CUERVO_SIMD_SSE2)
using NativeF = __m128;
using NativeI = __m128i;
#else
using NativeF = v128_t;
using NativeI = v128_t;
#endif

// `width` floats. Masks (results of compares) are F values with all bits set
// in true lanes, as the hardware produces them.
struct F {
    NativeF v;
};
// `width` 32-bit integers.
struct I {
    NativeI v;
};
using M = F;

#if defined(CUERVO_SIMD_AVX2)

inline F load(const float* p) { return {_mm256_loadu_ps(p)}; }
inline void store(float* p, F a) { _mm256_storeu_ps(p, a.v); }
inline F splat(float s) { return {_mm256_set1_ps(s)}; }
inline F operator+(F a, F b) { return {_mm256_add_ps(a.v, b.v)}; }
inline F operator-(F a, F b) { return {_mm256_sub_ps(a.v, b.v)}; }
inline F operator*(F a, F b) { return {_mm256_mul_ps(a.v, b.v)}; }
inline F operator/(F a, F b) { return {_mm256_div_ps(a.v, b.v)}; }
inline F sqrt(F a) { return {_mm256_sqrt_ps(a.v)}; }
inline F floor(F a) { return {_mm256_floor_ps(a.v)}; }
inline M operator<(F a, F b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_LT_OQ)}; }
inline M operator<=(F a, F b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_LE_OQ)}; }
inline M operator>(F a, F b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_GT_OQ)}; }
inline M operator>=(F a, F b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_GE_OQ)}; }
inline M operator==(F a, F b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_EQ_OQ)}; }
inline M operator!=(F a, F b) { return {_mm256_cmp_ps(a.v, b.v, _CMP_NEQ_UQ)}; }
inline M operator&(M a, M b) { return {_mm256_and_ps(a.v, b.v)}; }
inline M operator|(M a, M b) { return {_mm256_or_ps(a.v, b.v)}; }
inline M operator^(F a, F b) { return {_mm256_xor_ps(a.v, b.v)}; }
inline M andnot(M a, M b) { return {_mm256_andnot_ps(b.v, a.v)}; } // a & ~b
inline F select(M m, F a, F b) { return {_mm256_blendv_ps(b.v, a.v, m.v)}; }
inline int bits(M m) { return _mm256_movemask_ps(m.v); }

inline I load_i(const std::uint32_t* p) { return {_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p))}; }
inline void store_i(std::uint32_t* p, I a) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), a.v); }
inline void store_i(std::int32_t* p, I a) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), a.v); }
inline I splat_i(std::int32_t s) { return {_mm256_set1_epi32(s)}; }
inline I truncate(F a) { return {_mm256_cvttps_epi32(a.v)}; } // in-range values only
inline F to_float(I a) { return {_mm256_cvtepi32_ps(a.v)}; }
inline I operator+(I a, I b) { return {_mm256_add_epi32(a.v, b.v)}; }
inline I operator-(I a, I b) { return {_mm256_sub_epi32(a.v, b.v)}; }
inline I operator*(I a, I b) { return {_mm256_mullo_epi32(a.v, b.v)}; } // low 32 bits (wrapping)
inline I operator&(I a, I b) { return {_mm256_and_si256(a.v, b.v)}; }
inline I operator|(I a, I b) { return {_mm256_or_si256(a.v, b.v)}; }
inline I operator^(I a, I b) { return {_mm256_xor_si256(a.v, b.v)}; }
template<int N> inline I shl(I a) { return {_mm256_slli_epi32(a.v, N)}; }
template<int N> inline I shr(I a) { return {_mm256_srli_epi32(a.v, N)}; } // logical
inline I as_int(M m) { return {_mm256_castps_si256(m.v)}; }
inline I select(I m, I a, I b) { return {_mm256_blendv_epi8(b.v, a.v, m.v)}; }

#elif defined(CUERVO_SIMD_SSE2)

inline F load(const float* p) { return {_mm_loadu_ps(p)}; }
inline void store(float* p, F a) { _mm_storeu_ps(p, a.v); }
inline F splat(float s) { return {_mm_set1_ps(s)}; }
inline F operator+(F a, F b) { return {_mm_add_ps(a.v, b.v)}; }
inline F operator-(F a, F b) { return {_mm_sub_ps(a.v, b.v)}; }
inline F operator*(F a, F b) { return {_mm_mul_ps(a.v, b.v)}; }
inline F operator/(F a, F b) { return {_mm_div_ps(a.v, b.v)}; }
inline F sqrt(F a) { return {_mm_sqrt_ps(a.v)}; }
inline M operator<(F a, F b) { return {_mm_cmplt_ps(a.v, b.v)}; }
inline M operator<=(F a, F b) { return {_mm_cmple_ps(a.v, b.v)}; }
inline M operator>(F a, F b) { return {_mm_cmpgt_ps(a.v, b.v)}; }
inline M operator>=(F a, F b) { return {_mm_cmpge_ps(a.v, b.v)}; }
inline M operator==(F a, F b) { return {_mm_cmpeq_ps(a.v, b.v)}; }
inline M operator!=(F a, F b) { return {_mm_cmpneq_ps(a.v, b.v)}; } // unordered: true, like C++
inline M operator&(M a, M b) { return {_mm_and_ps(a.v, b.v)}; }
inline M operator|(M a, M b) { return {_mm_or_ps(a.v, b.v)}; }
inline M operator^(F a, F b) { return {_mm_xor_ps(a.v, b.v)}; }
inline M andnot(M a, M b) { return {_mm_andnot_ps(b.v, a.v)}; } // a & ~b
inline F select(M m, F a, F b) { return {_mm_or_ps(_mm_and_ps(m.v, a.v), _mm_andnot_ps(m.v, b.v))}; }
inline int bits(M m) { return _mm_movemask_ps(m.v); }

inline I load_i(const std::uint32_t* p) { return {_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))}; }
inline void store_i(std::uint32_t* p, I a) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), a.v); }
inline void store_i(std::int32_t* p, I a) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), a.v); }
inline I splat_i(std::int32_t s) { return {_mm_set1_epi32(s)}; }
inline I truncate(F a) { return {_mm_cvttps_epi32(a.v)}; } // in-range values only
inline F to_float(I a) { return {_mm_cvtepi32_ps(a.v)}; }
inline I operator+(I a, I b) { return {_mm_add_epi32(a.v, b.v)}; }
inline I operator-(I a, I b) { return {_mm_sub_epi32(a.v, b.v)}; }
inline I operator*(I a, I b) {
    // SSE2 has no 32-bit low multiply (pmulld is SSE4.1): two 32x32->64
    // multiplies on the even and odd lanes, keeping the low halves.
    const __m128i even = _mm_mul_epu32(a.v, b.v);
    const __m128i odd = _mm_mul_epu32(_mm_srli_si128(a.v, 4), _mm_srli_si128(b.v, 4));
    return {_mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)),
                               _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0)))};
}
inline I operator&(I a, I b) { return {_mm_and_si128(a.v, b.v)}; }
inline I operator|(I a, I b) { return {_mm_or_si128(a.v, b.v)}; }
inline I operator^(I a, I b) { return {_mm_xor_si128(a.v, b.v)}; }
template<int N> inline I shl(I a) { return {_mm_slli_epi32(a.v, N)}; }
template<int N> inline I shr(I a) { return {_mm_srli_epi32(a.v, N)}; }
inline I as_int(M m) { return {_mm_castps_si128(m.v)}; }
inline I select(I m, I a, I b) { return {_mm_or_si128(_mm_and_si128(m.v, a.v), _mm_andnot_si128(m.v, b.v))}; }

// SSE2 has no roundps: floor by truncation, exact for |x| < 2^23; larger
// values (and NaN) are already integral and returned as they are, and an
// integral x is returned unchanged so floor(-0) stays -0 like std::floor.
inline F floor(F a) {
    const __m128 abs = _mm_andnot_ps(_mm_set1_ps(-0.0f), a.v);
    const __m128 small = _mm_cmplt_ps(abs, _mm_set1_ps(8388608.0f)); // false for NaN
    const __m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(a.v));
    const __m128 r = _mm_sub_ps(t, _mm_and_ps(_mm_cmpgt_ps(t, a.v), _mm_set1_ps(1.0f)));
    const __m128 keep = _mm_or_ps(_mm_andnot_ps(small, _mm_castsi128_ps(_mm_set1_epi32(-1))), _mm_cmpeq_ps(t, a.v));
    return select({keep}, a, {r});
}

#else // CUERVO_SIMD_WASM

inline F load(const float* p) { return {wasm_v128_load(p)}; }
inline void store(float* p, F a) { wasm_v128_store(p, a.v); }
inline F splat(float s) { return {wasm_f32x4_splat(s)}; }
inline F operator+(F a, F b) { return {wasm_f32x4_add(a.v, b.v)}; }
inline F operator-(F a, F b) { return {wasm_f32x4_sub(a.v, b.v)}; }
inline F operator*(F a, F b) { return {wasm_f32x4_mul(a.v, b.v)}; }
inline F operator/(F a, F b) { return {wasm_f32x4_div(a.v, b.v)}; }
inline F sqrt(F a) { return {wasm_f32x4_sqrt(a.v)}; }
inline F floor(F a) { return {wasm_f32x4_floor(a.v)}; }
inline M operator<(F a, F b) { return {wasm_f32x4_lt(a.v, b.v)}; }
inline M operator<=(F a, F b) { return {wasm_f32x4_le(a.v, b.v)}; }
inline M operator>(F a, F b) { return {wasm_f32x4_gt(a.v, b.v)}; }
inline M operator>=(F a, F b) { return {wasm_f32x4_ge(a.v, b.v)}; }
inline M operator==(F a, F b) { return {wasm_f32x4_eq(a.v, b.v)}; }
inline M operator!=(F a, F b) { return {wasm_f32x4_ne(a.v, b.v)}; }
inline M operator&(M a, M b) { return {wasm_v128_and(a.v, b.v)}; }
inline M operator|(M a, M b) { return {wasm_v128_or(a.v, b.v)}; }
inline M operator^(F a, F b) { return {wasm_v128_xor(a.v, b.v)}; }
inline M andnot(M a, M b) { return {wasm_v128_andnot(a.v, b.v)}; } // a & ~b
inline F select(M m, F a, F b) { return {wasm_v128_bitselect(a.v, b.v, m.v)}; }
inline int bits(M m) { return static_cast<int>(wasm_i32x4_bitmask(m.v)); }

inline I load_i(const std::uint32_t* p) { return {wasm_v128_load(p)}; }
inline void store_i(std::uint32_t* p, I a) { wasm_v128_store(p, a.v); }
inline void store_i(std::int32_t* p, I a) { wasm_v128_store(p, a.v); }
inline I splat_i(std::int32_t s) { return {wasm_i32x4_splat(s)}; }
inline I truncate(F a) { return {wasm_i32x4_trunc_sat_f32x4(a.v)}; } // in-range values only
inline F to_float(I a) { return {wasm_f32x4_convert_i32x4(a.v)}; }
inline I operator+(I a, I b) { return {wasm_i32x4_add(a.v, b.v)}; }
inline I operator-(I a, I b) { return {wasm_i32x4_sub(a.v, b.v)}; }
inline I operator*(I a, I b) { return {wasm_i32x4_mul(a.v, b.v)}; }
inline I operator&(I a, I b) { return {wasm_v128_and(a.v, b.v)}; }
inline I operator|(I a, I b) { return {wasm_v128_or(a.v, b.v)}; }
inline I operator^(I a, I b) { return {wasm_v128_xor(a.v, b.v)}; }
template<int N> inline I shl(I a) { return {wasm_i32x4_shl(a.v, N)}; }
template<int N> inline I shr(I a) { return {wasm_u32x4_shr(a.v, N)}; }
inline I as_int(M m) { return {m.v}; }
inline I select(I m, I a, I b) { return {wasm_v128_bitselect(a.v, b.v, m.v)}; }

#endif

// --- helpers shared by every ISA ---------------------------------------------

inline M all_true() { return splat(0.0f) == splat(0.0f); }
inline M operator!(M m) { return andnot(all_true(), m); }
inline F operator-(F a) { return a ^ splat(-0.0f); } // exact negation (sign flip), like scalar -x
inline F abs(F a) { return andnot(a, splat(-0.0f)); } // clears the sign, like std::fabs
// std::min(a, b) == (b < a ? b : a); std::max(a, b) == (a < b ? b : a).
inline F min(F a, F b) { return select(b < a, b, a); }
inline F max(F a, F b) { return select(a < b, b, a); }
// std::clamp(v, lo, hi) == (v < lo ? lo : (hi < v ? hi : v)).
inline F clamp(F v, F lo, F hi) { return select(v < lo, lo, select(hi < v, hi, v)); }
inline bool any(M m) { return bits(m) != 0; }

// Loads `width` bytes (0/1 flags) and widens them to a mask of nonzero lanes.
inline M load_flags(const std::uint8_t* p) {
    alignas(32) float f[width];
    for (int k = 0; k < width; ++k) f[k] = p[k] != 0 ? 1.0f : 0.0f;
    return load(f) != splat(0.0f);
}
// Sets p[k] = 1 for every true lane (leaves false lanes untouched).
inline void set_flags(std::uint8_t* p, M m) {
    const int b = bits(m);
    if (b == 0) return;
    for (int k = 0; k < width; ++k) {
        if (b & (1 << k)) p[k] = 1;
    }
}

} // namespace cuervo::simd

#endif

#include "cuervo/math/dmath.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace cuervo::dmath {

namespace {

// Range-reduce x to r in [-pi, pi] with x = r + k*tau. tau is split into a
// high part (exact in f32) and a low correction (Cody-Waite).
float reduce(float x) {
    constexpr float inv_tau = 0.159154943091895335769f;
    constexpr float tau_hi = 6.28125f;
    constexpr float tau_lo = 1.9353071795864769e-3f;
    float k = floor(x * inv_tau + 0.5f);
    return (x - k * tau_hi) - k * tau_lo;
}

// Taylor polynomials, valid on [-pi/2, pi/2]. Evaluated in Horner form with
// plain f32 operations (FMA contraction is disabled project-wide).
float sin_poly(float r) {
    float r2 = r * r;
    float p = -2.5052108385441718775e-8f;   // -1/11!
    p = p * r2 + 2.7557319223985890653e-6f;  //  1/9!
    p = p * r2 - 1.9841269841269841270e-4f;  // -1/7!
    p = p * r2 + 8.3333333333333333333e-3f;  //  1/5!
    p = p * r2 - 1.6666666666666666667e-1f;  // -1/3!
    return r + r * r2 * p;
}

float cos_poly(float r) {
    float r2 = r * r;
    float p = 2.0876756987868098979e-9f;     //  1/12!
    p = p * r2 - 2.7557319223985890653e-7f;  // -1/10!
    p = p * r2 + 2.4801587301587301587e-5f;  //  1/8!
    p = p * r2 - 1.3888888888888888889e-3f;  // -1/6!
    p = p * r2 + 4.1666666666666666667e-2f;  //  1/4!
    p = p * r2 - 0.5f;                       // -1/2!
    return 1.0f + r2 * p;
}

// atan on [-tan(pi/8), tan(pi/8)]: odd Taylor series through x^17; the
// first omitted term is below 2e-8 there.
float atan_poly(float t) {
    float t2 = t * t;
    float p = 1.0f / 17.0f;
    p = p * t2 - 1.0f / 15.0f;
    p = p * t2 + 1.0f / 13.0f;
    p = p * t2 - 1.0f / 11.0f;
    p = p * t2 + 1.0f / 9.0f;
    p = p * t2 - 1.0f / 7.0f;
    p = p * t2 + 1.0f / 5.0f;
    p = p * t2 - 1.0f / 3.0f;
    return t + t * t2 * p;
}

// atan for x in [0, 1]: above tan(pi/8), use atan(x) = pi/4 + atan((x-1)/(x+1)).
float atan_unit(float x) {
    constexpr float tan_pi_8 = 0.41421356237309504880f;
    constexpr float quarter_pi = 0.78539816339744830962f;
    if (x > tan_pi_8) {
        return quarter_pi + atan_poly((x - 1.0f) / (x + 1.0f));
    }
    return atan_poly(x);
}

} // namespace

float sqrt(float x) { return std::sqrt(x); } // IEEE: correctly rounded on all targets
float abs(float x) { return std::fabs(x); }
float floor(float x) { return std::floor(x); }
float ceil(float x) { return std::ceil(x); }
float round(float x) { return std::round(x); }
float trunc(float x) { return std::trunc(x); }

float min(float a, float b) { return b < a ? b : a; }
float max(float a, float b) { return a < b ? b : a; }
float clamp(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }
float sign(float x) {
    if (std::isnan(x)) return x;
    return (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f);
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }
float inverse_lerp(float a, float b, float v) { return (b == a) ? 0.0f : ((v - a) / (b - a)); }
float remap(float in_min, float in_max, float out_min, float out_max, float v) {
    float t = inverse_lerp(in_min, in_max, v);
    return lerp(out_min, out_max, t);
}

float smoothstep(float edge0, float edge1, float x) {
    if (edge0 == edge1) return x < edge0 ? 0.0f : 1.0f;
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float step(float edge, float x) { return x < edge ? 0.0f : 1.0f; }

float move_toward(float from, float to, float max_delta) {
    if (abs(to - from) <= max_delta) return to;
    return from + (to > from ? max_delta : -max_delta);
}

float rad_to_deg(float rad) { return rad * rad_to_deg_factor; }
float deg_to_rad(float deg) { return deg * deg_to_rad_factor; }

float wrap(float val, float min_val, float max_val) {
    float range = max_val - min_val;
    if (range == 0.0f) return min_val;
    return val - range * floor((val - min_val) / range);
}

float wrap_angle(float angle) {
    return wrap(angle + pi, 0.0f, tau) - pi;
}

float sin(float x) {
    float r = reduce(x);
    // sin(pi - r) == sin(r): fold into [-pi/2, pi/2].
    if (r > half_pi) {
        r = pi - r;
    } else if (r < -half_pi) {
        r = -pi - r;
    }
    return sin_poly(r);
}

float cos(float x) {
    float r = reduce(x);
    // cos(pi - r) == -cos(r): fold into [-pi/2, pi/2] and flip the sign.
    if (r > half_pi) {
        return -cos_poly(pi - r);
    }
    if (r < -half_pi) {
        return -cos_poly(-pi - r);
    }
    return cos_poly(r);
}

float tan(float x) {
    return sin(x) / cos(x);
}

float asin(float x) {
    if (abs(x) > 1.0f) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    float d = 1.0f - x * x;
    if (d < 0.0f) d = 0.0f;
    return atan2(x, sqrt(d));
}

float acos(float x) {
    if (abs(x) > 1.0f) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    float d = 1.0f - x * x;
    if (d < 0.0f) d = 0.0f;
    return atan2(sqrt(d), x);
}

float atan(float x) {
    return atan2(x, 1.0f);
}

float atan2(float y, float x) {
    const float ax = abs(x);
    const float ay = abs(y);
    if (ax == 0.0f && ay == 0.0f) {
        return 0.0f;
    }
    // Fold into the first octant so the polynomial argument stays in [0, 1].
    float a = ay > ax ? half_pi - atan_unit(ax / ay) : atan_unit(ay / ax);
    if (x < 0.0f) {
        a = pi - a;
    }
    return y < 0.0f ? -a : a;
}

float exp(float x) {
    if (std::isnan(x)) return x;
    if (x > 88.722839f) return std::numeric_limits<float>::infinity();
    if (x < -104.0f) return 0.0f;

    // Range reduction: x = k * ln(2) + r, with k = round(x * log2(e)).
    constexpr float inv_ln2 = 1.44269504088896340736f;
    constexpr float ln2_hi = 0.693145751953125f;
    constexpr float ln2_lo = 1.42860682030941723212e-6f;

    float k = floor(x * inv_ln2 + 0.5f);
    float r = (x - k * ln2_hi) - k * ln2_lo;

    // Taylor polynomial for e^r on [-ln(2)/2, ln(2)/2]
    float p = 1.0f / 5040.0f;
    p = p * r + 1.0f / 720.0f;
    p = p * r + 1.0f / 120.0f;
    p = p * r + 1.0f / 24.0f;
    p = p * r + 1.0f / 6.0f;
    p = p * r + 0.5f;
    float er = 1.0f + r + (r * r) * p;

    int ik = static_cast<int>(k);
    if (ik >= -126 && ik <= 127) {
        float scale = std::bit_cast<float>(static_cast<std::uint32_t>((ik + 127) << 23));
        return er * scale;
    } else if (ik >= -149) {
        float s1 = std::bit_cast<float>(static_cast<std::uint32_t>((ik + 50 + 127) << 23));
        float s2 = std::bit_cast<float>(static_cast<std::uint32_t>((127 - 50) << 23));
        return (er * s1) * s2;
    }
    return 0.0f;
}

float log(float x) {
    if (std::isnan(x)) return x;
    if (x < 0.0f) return std::numeric_limits<float>::quiet_NaN();
    if (x == 0.0f) return -std::numeric_limits<float>::infinity();
    if (std::isinf(x)) return x;

    int k = 0;
    if (x < 1.17549435e-38f) {
        x *= 16777216.0f; // 2^24
        k -= 24;
    }

    std::uint32_t u = std::bit_cast<std::uint32_t>(x);
    k += static_cast<int>(u >> 23) - 127;
    float m = std::bit_cast<float>((u & 0x007FFFFFu) | 0x3F800000u);

    constexpr float sqrt2 = 1.41421356237309504880f;
    if (m > sqrt2) {
        m *= 0.5f;
        k += 1;
    }

    float t = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;

    float p = 2.0f / 9.0f;
    p = p * t2 + 2.0f / 7.0f;
    p = p * t2 + 2.0f / 5.0f;
    p = p * t2 + 2.0f / 3.0f;
    p = p * t2 + 2.0f;
    float ln_m = t * p;

    constexpr float ln2_hi = 0.693145751953125f;
    constexpr float ln2_lo = 1.42860682030941723212e-6f;
    float fk = static_cast<float>(k);
    return ln_m + (fk * ln2_hi + fk * ln2_lo);
}

float log2(float x) {
    constexpr float inv_ln2 = 1.44269504088896340736f;
    return log(x) * inv_ln2;
}

float log10(float x) {
    constexpr float inv_ln10 = 0.43429448190325182765f;
    return log(x) * inv_ln10;
}

float pow(float base, float exp_val) {
    if (exp_val == 0.0f) return 1.0f;
    if (std::isnan(base) || std::isnan(exp_val)) return std::numeric_limits<float>::quiet_NaN();
    if (base == 1.0f) return 1.0f;
    if (exp_val == 1.0f) return base;
    if (base == 0.0f) {
        if (exp_val > 0.0f) return 0.0f;
        return std::numeric_limits<float>::infinity();
    }
    if (base > 0.0f) {
        return exp(exp_val * log(base));
    }
    float int_part = floor(exp_val);
    if (int_part != exp_val) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    float result = exp(exp_val * log(-base));
    // Every float of magnitude >= 2^24 is an even integer; below that the
    // exponent fits an int32 exactly (no out-of-range conversion).
    const bool odd = abs(exp_val) < 16777216.0f && static_cast<std::int32_t>(exp_val) % 2 != 0;
    return odd ? -result : result;
}

} // namespace cuervo::dmath

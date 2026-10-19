#pragma once

// Deterministic math (DESIGN.md 4.1). Simulation code calls these instead of
// <cmath> so results don't depend on the platform's libm. Basic IEEE
// operations (+ - * / sqrt) are correctly rounded everywhere and can use the
// hardware; transcendental functions are implemented here.
//
// TODO: tighten sin/cos accuracy (minimax coefficients), add tan,
// asin/acos, exp/log, pow, and verify bit-exactness native vs. WASM in CI.

namespace cuervo::dmath {

inline constexpr float pi = 3.14159265358979323846f;
inline constexpr float tau = 6.28318530717958647692f;
inline constexpr float half_pi = 1.57079632679489661923f;
inline constexpr float e = 2.71828182845904523536f;
inline constexpr float deg_to_rad_factor = 0.0174532925199432957692f;
inline constexpr float rad_to_deg_factor = 57.2957795130823208768f;

float sqrt(float x);
float abs(float x);
float floor(float x);
float ceil(float x);
float round(float x);
float trunc(float x);
float min(float a, float b);
float max(float a, float b);
float clamp(float v, float lo, float hi);
float sign(float x);
float lerp(float a, float b, float t);
float inverse_lerp(float a, float b, float v);
float remap(float in_min, float in_max, float out_min, float out_max, float v);
float smoothstep(float edge0, float edge1, float x);
float step(float edge, float x);
float move_toward(float from, float to, float max_delta);
float rad_to_deg(float rad);
float deg_to_rad(float deg);
float wrap(float val, float min, float max);
float wrap_angle(float angle);

float sin(float x);
float cos(float x);
float tan(float x);
float asin(float x);
float acos(float x);
float atan(float x);
// Angle of (x, y) in [-pi, pi]; atan2(0, 0) == 0. Max error ~1e-7 rad.
float atan2(float y, float x);

float exp(float x);
float log(float x);
float log2(float x);
float log10(float x);
float pow(float base, float exp);

} // namespace cuervo::dmath

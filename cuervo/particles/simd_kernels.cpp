#include "cuervo/particles/simd_kernels.hpp"

#include <algorithm>
#include <type_traits>
#include <variant>

#include "cuervo/math/dmath.hpp"
#include "cuervo/math/dmath_simd.hpp"
#include "cuervo/math/simd.hpp"

namespace cuervo::particles::kernels {

#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2) || defined(CUERVO_SIMD_WASM)

namespace {

using simd::F;
using simd::M;
using simd::select;
using simd::splat;
constexpr std::size_t W = simd::width;

// Runs body(i) for i = begin, begin + W, ... while a whole block fits;
// returns the first index left for the scalar loop.
template<typename Body>
std::size_t blocks(std::size_t begin, std::size_t end, Body&& body) {
    std::size_t i = begin;
    for (; i + W <= end; i += W) body(i);
    return i;
}

// normalized_age(age, life) of emitter.cpp:
//   life > 0 ? clamp(age / life, 0, 1) : 0
F normalized_age(F age, F life) {
    return select(life > splat(0.0f), simd::clamp(age / life, splat(0.0f), splat(1.0f)), splat(0.0f));
}

F ease(Ease e, F t) {
    switch (e) {
        case Ease::EaseIn: return t * t;
        case Ease::EaseOut: return t * (splat(2.0f) - t);
        case Ease::Smooth: return t * t * (splat(3.0f) - splat(2.0f) * t);
        case Ease::Linear: break;
    }
    return t;
}

// render::to_byte of sdf_batcher.cpp: (u32)(clamp(v, 0, 1) * 255 + 0.5),
// masked to a byte (the scalar cast of a NaN gives 0 there, too).
simd::I to_byte(F v) {
    return simd::truncate(simd::clamp(v, splat(0.0f), splat(1.0f)) * splat(255.0f) + splat(0.5f)) & simd::splat_i(0xff);
}

} // namespace

std::size_t integrate(ParticlePool& p, float dt, std::size_t begin, std::size_t end) {
    const F vdt = splat(dt);
    return blocks(begin, end, [&](std::size_t i) {
        const F x = simd::load(&p.x[i]);
        const F y = simd::load(&p.y[i]);
        simd::store(&p.prev_x[i], x);
        simd::store(&p.prev_y[i], y);
        simd::store(&p.age[i], simd::load(&p.age[i]) + vdt);
        simd::store(&p.x[i], x + simd::load(&p.vx[i]) * vdt);
        simd::store(&p.y[i], y + simd::load(&p.vy[i]) * vdt);
        simd::store(&p.rotation[i], simd::load(&p.rotation[i]) + simd::load(&p.spin[i]) * vdt);
    });
}

std::size_t lifetime(ParticlePool& p, std::size_t begin, std::size_t end) {
    return blocks(begin, end, [&](std::size_t i) {
        const F life = simd::load(&p.life[i]);
        simd::set_flags(&p.dead[i], (life > splat(0.0f)) & (simd::load(&p.age[i]) >= life));
    });
}

std::size_t module(ParticlePool& p, const Module& module, const Frame& f, std::size_t begin, std::size_t end) {
    const float dt = f.dt;
    return std::visit(
        [&](const auto& m) -> std::size_t {
            using Mod = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<Mod, Accelerate>) {
                const F ax = splat(m.acceleration.x() * dt);
                const F ay = splat(m.acceleration.y() * dt);
                return blocks(begin, end, [&](std::size_t i) {
                    simd::store(&p.vx[i], simd::load(&p.vx[i]) + ax);
                    simd::store(&p.vy[i], simd::load(&p.vy[i]) + ay);
                });
            } else if constexpr (std::is_same_v<Mod, Drag>) {
                const F k = splat(std::max(0.0f, 1.0f - m.coefficient * dt));
                return blocks(begin, end, [&](std::size_t i) {
                    simd::store(&p.vx[i], simd::load(&p.vx[i]) * k);
                    simd::store(&p.vy[i], simd::load(&p.vy[i]) * k);
                });
            } else if constexpr (std::is_same_v<Mod, SpeedRamp>) {
                const F dv = splat(m.acceleration * dt);
                const F lo = splat(m.min_speed);
                const F hi = splat(m.max_speed);
                return blocks(begin, end, [&](std::size_t i) {
                    const F vx = simd::load(&p.vx[i]);
                    const F vy = simd::load(&p.vy[i]);
                    const F speed = simd::sqrt(vx * vx + vy * vy);
                    const M skip = speed <= splat(0.0f);
                    const F scale = simd::clamp(speed + dv, lo, hi) / speed;
                    simd::store(&p.vx[i], select(skip, vx, vx * scale));
                    simd::store(&p.vy[i], select(skip, vy, vy * scale));
                });
            } else if constexpr (std::is_same_v<Mod, Turn>) {
                const F c = splat(dmath::cos(m.rate * dt));
                const F s = splat(dmath::sin(m.rate * dt));
                return blocks(begin, end, [&](std::size_t i) {
                    const F vx = simd::load(&p.vx[i]);
                    const F vy = simd::load(&p.vy[i]);
                    simd::store(&p.vx[i], vx * c - vy * s);
                    simd::store(&p.vy[i], vx * s + vy * c);
                });
            } else if constexpr (std::is_same_v<Mod, Homing>) {
                const float max_turn = m.turn_rate * dt;
                const F tx = splat(f.target.x());
                const F ty = splat(f.target.y());
                const F delay = splat(m.delay);
                const F until = splat(m.delay + m.duration);
                const bool limited = m.duration > 0.0f;
                return blocks(begin, end, [&](std::size_t i) {
                    const F a = simd::load(&p.age[i]);
                    M skip = a < delay;
                    if (limited) skip = skip | (a >= until);
                    const F vx = simd::load(&p.vx[i]);
                    const F vy = simd::load(&p.vy[i]);
                    const F to_x = tx - simd::load(&p.x[i]);
                    const F to_y = ty - simd::load(&p.y[i]);
                    const F angle = dmath::simd::atan2(vx * to_y - vy * to_x, vx * to_x + vy * to_y);
                    const F turn = simd::clamp(angle, splat(-max_turn), splat(max_turn));
                    skip = skip | (turn == splat(0.0f));
                    if (bits(skip) == (1 << W) - 1) return;
                    const F c = dmath::simd::cos(turn);
                    const F s = dmath::simd::sin(turn);
                    simd::store(&p.vx[i], select(skip, vx, vx * c - vy * s));
                    simd::store(&p.vy[i], select(skip, vy, vx * s + vy * c));
                });
            } else if constexpr (std::is_same_v<Mod, Orbit>) {
                const F c = splat(dmath::cos(m.angular_speed * dt));
                const F s = splat(dmath::sin(m.angular_speed * dt));
                const F cx = splat(f.position.x());
                const F cy = splat(f.position.y());
                const F radial = splat(m.radial_speed * dt);
                const F inv_dt = splat(dt > 0.0f ? 1.0f / dt : 0.0f);
                return blocks(begin, end, [&](std::size_t i) {
                    const F px = simd::load(&p.prev_x[i]);
                    const F py = simd::load(&p.prev_y[i]);
                    const F vx = simd::load(&p.vx[i]);
                    const F vy = simd::load(&p.vy[i]);
                    const F ox = px - cx;
                    const F oy = py - cy;
                    const F len = simd::sqrt(ox * ox + oy * oy);
                    const F new_len = simd::max(splat(0.0f), len + radial);
                    // len > 0: rotate and scale the offset.
                    const F k = new_len / len;
                    const F rx = (ox * c - oy * s) * k;
                    const F ry = (ox * s + oy * c) * k;
                    // At the center: leave along the spawn velocity.
                    const F speed = simd::sqrt(vx * vx + vy * vy);
                    const F ks = select(speed > splat(0.0f), new_len / speed, splat(0.0f));
                    const M moving = len > splat(0.0f);
                    const F x = cx + select(moving, rx, vx * ks);
                    const F y = cy + select(moving, ry, vy * ks);
                    simd::store(&p.x[i], x);
                    simd::store(&p.y[i], y);
                    simd::store(&p.vx[i], (x - px) * inv_dt);
                    simd::store(&p.vy[i], (y - py) * inv_dt);
                });
            } else if constexpr (std::is_same_v<Mod, Wave>) {
                const F w = splat(dmath::tau * m.frequency);
                const F phase = splat(m.phase);
                const F amplitude = splat(m.amplitude);
                const F vdt = splat(dt);
                return blocks(begin, end, [&](std::size_t i) {
                    const F vx = simd::load(&p.vx[i]);
                    const F vy = simd::load(&p.vy[i]);
                    const F speed = simd::sqrt(vx * vx + vy * vy);
                    const M skip = speed <= splat(0.0f);
                    if (bits(skip) == (1 << W) - 1) return;
                    const F age = simd::load(&p.age[i]);
                    const F now = dmath::simd::sin(w * age + phase);
                    const F before = dmath::simd::sin(w * (age - vdt) + phase);
                    const F d = amplitude * (now - before) / speed;
                    const F x = simd::load(&p.x[i]);
                    const F y = simd::load(&p.y[i]);
                    simd::store(&p.x[i], select(skip, x, x - vy * d));
                    simd::store(&p.y[i], select(skip, y, y + vx * d));
                });
            } else if constexpr (std::is_same_v<Mod, FaceVelocity>) {
                return blocks(begin, end, [&](std::size_t i) {
                    const F vx = simd::load(&p.vx[i]);
                    const F vy = simd::load(&p.vy[i]);
                    const M moving = (vx != splat(0.0f)) | (vy != splat(0.0f));
                    simd::store(&p.rotation[i], select(moving, dmath::simd::atan2(vy, vx), simd::load(&p.rotation[i])));
                });
            } else if constexpr (std::is_same_v<Mod, ColorOverLife>) {
                const F r0 = splat(m.start.r), dr = splat(m.end.r - m.start.r);
                const F g0 = splat(m.start.g), dg = splat(m.end.g - m.start.g);
                const F b0 = splat(m.start.b), db = splat(m.end.b - m.start.b);
                const F a0 = splat(m.start.a), da = splat(m.end.a - m.start.a);
                return blocks(begin, end, [&](std::size_t i) {
                    const F t = normalized_age(simd::load(&p.age[i]), simd::load(&p.life[i]));
                    const simd::I packed = to_byte(r0 + dr * t) | simd::shl<8>(to_byte(g0 + dg * t)) |
                                           simd::shl<16>(to_byte(b0 + db * t)) | simd::shl<24>(to_byte(a0 + da * t));
                    simd::store_i(&p.color[i], packed);
                });
            } else if constexpr (std::is_same_v<Mod, SizeOverLife>) {
                const F start = splat(m.start);
                const F span = splat(m.end - m.start);
                return blocks(begin, end, [&](std::size_t i) {
                    const F t = ease(m.ease, normalized_age(simd::load(&p.age[i]), simd::load(&p.life[i])));
                    simd::store(&p.size[i], simd::load(&p.base_size[i]) * (start + span * t));
                });
            } else if constexpr (std::is_same_v<Mod, KillOutside>) {
                const F x0 = splat(m.min.x());
                const F y0 = splat(m.min.y());
                const F x1 = splat(m.max.x());
                const F y1 = splat(m.max.y());
                return blocks(begin, end, [&](std::size_t i) {
                    const F x = simd::load(&p.x[i]);
                    const F y = simd::load(&p.y[i]);
                    simd::set_flags(&p.dead[i], (x < x0) | (x > x1) | (y < y0) | (y > y1));
                });
            } else {
                return begin;
            }
        },
        module);
}

#else

std::size_t integrate(ParticlePool&, float, std::size_t begin, std::size_t) { return begin; }
std::size_t lifetime(ParticlePool&, std::size_t begin, std::size_t) { return begin; }
std::size_t module(ParticlePool&, const Module&, const Frame&, std::size_t begin, std::size_t) { return begin; }

#endif

} // namespace cuervo::particles::kernels

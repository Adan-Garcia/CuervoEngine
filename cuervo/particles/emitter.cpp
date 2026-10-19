#include "cuervo/particles/emitter.hpp"

#include <algorithm>
#include <type_traits>

#include "cuervo/math/dmath.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/particles/simd_kernels.hpp"

namespace cuervo::particles {

namespace {

// Gameplay emitters must stay deterministic (DESIGN.md 4.1): everything below
// is plain f32 arithmetic plus dmath, iterating the pool in index order.

// Keeps an angle in [0, tau) so dmath's range reduction stays accurate after
// many accumulated steps (Spiral phases).
float wrap_angle(float a) {
    if (a >= dmath::tau || a < 0.0f) {
        a -= dmath::tau * dmath::floor(a / dmath::tau);
    }
    return a;
}

// Normalized age in [0, 1]; immortal particles stay at 0.
float normalized_age(float age, float life) {
    return life > 0.0f ? std::clamp(age / life, 0.0f, 1.0f) : 0.0f;
}

// Uniform +/- jitter; draws from the RNG only when there is jitter, so adding
// a jitter-free pattern doesn't shift anyone else's random sequence.
float jitter(Pcg32& rng, float amount) { return amount != 0.0f ? rng.range(-amount, amount) : 0.0f; }

} // namespace

Emitter::Emitter(EmitterConfig config) : config_(std::move(config)), rng_(config_.seed) {
    pool_.max_particles = config_.max_particles;
    if (config_.mode == Mode::Visual) {
        config_.collision.enabled = false; // DESIGN.md 4.6: visual particles never collide
    }
    for (Spawner& s : config_.spawners) {
        s.countdown = s.delay;
    }
}

std::size_t Emitter::add_spawner(Spawner spawner) {
    spawner.countdown = spawner.delay;
    config_.spawners.push_back(spawner);
    return config_.spawners.size() - 1;
}

void Emitter::simulate(float dt) {
    begin_tick();
    step_range(dt, 0, pool_.size_live());
    finish_tick(dt);
}

void Emitter::step_range(float dt, std::size_t begin, std::size_t end) {
    integrate(dt, begin, end);
    for (const Module& module : config_.modules) {
        run_module(module, dt, begin, end);
    }
    apply_lifetime(begin, end);
}

void Emitter::finish_tick(float dt) {
    if (foreach_) {
        foreach_(*this, dt);
    }
    remove_dead();
    ++ticks_;
}

// Each loop below first hands whole SIMD blocks to its kernel
// (simd_kernels.hpp, DESIGN.md 4.15) and finishes the remainder here; with
// SIMD off the scalar loop does everything. Both give the same bits.
void Emitter::integrate(float dt, std::size_t begin, std::size_t end) {
    ParticlePool& p = pool_;
    const std::size_t first = simd::enabled() ? kernels::integrate(p, dt, begin, end) : begin;
    for (std::size_t i = first; i < end; ++i) {
        p.prev_x[i] = p.x[i];
        p.prev_y[i] = p.y[i];
        p.age[i] += dt;
        p.x[i] += p.vx[i] * dt;
        p.y[i] += p.vy[i] * dt;
        p.rotation[i] += p.spin[i] * dt;
    }
}

void Emitter::run_module(const Module& module, float dt, std::size_t begin, std::size_t end) {
    ParticlePool& p = pool_;
    if (simd::enabled()) begin = kernels::module(p, module, {dt, position, target}, begin, end);
    std::visit(
        [&](const auto& m) {
            using M = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<M, Accelerate>) {
                const float ax = m.acceleration.x() * dt;
                const float ay = m.acceleration.y() * dt;
                for (std::size_t i = begin; i < end; ++i) {
                    p.vx[i] += ax;
                    p.vy[i] += ay;
                }
            } else if constexpr (std::is_same_v<M, Drag>) {
                const float k = std::max(0.0f, 1.0f - m.coefficient * dt);
                for (std::size_t i = begin; i < end; ++i) {
                    p.vx[i] *= k;
                    p.vy[i] *= k;
                }
            } else if constexpr (std::is_same_v<M, SpeedRamp>) {
                const float dv = m.acceleration * dt;
                for (std::size_t i = begin; i < end; ++i) {
                    const float speed = dmath::sqrt(p.vx[i] * p.vx[i] + p.vy[i] * p.vy[i]);
                    if (speed <= 0.0f) continue; // no heading to speed up along
                    const float scale = std::clamp(speed + dv, m.min_speed, m.max_speed) / speed;
                    p.vx[i] *= scale;
                    p.vy[i] *= scale;
                }
            } else if constexpr (std::is_same_v<M, Turn>) {
                // Same rotation for every particle: one sin/cos per tick.
                const float c = dmath::cos(m.rate * dt);
                const float s = dmath::sin(m.rate * dt);
                for (std::size_t i = begin; i < end; ++i) {
                    const float vx = p.vx[i];
                    const float vy = p.vy[i];
                    p.vx[i] = vx * c - vy * s;
                    p.vy[i] = vx * s + vy * c;
                }
            } else if constexpr (std::is_same_v<M, Homing>) {
                const float max_turn = m.turn_rate * dt;
                const float tx = target.x();
                const float ty = target.y();
                for (std::size_t i = begin; i < end; ++i) {
                    const float a = p.age[i];
                    if (a < m.delay || (m.duration > 0.0f && a >= m.delay + m.duration)) continue;
                    const float vx = p.vx[i];
                    const float vy = p.vy[i];
                    const float to_x = tx - p.x[i];
                    const float to_y = ty - p.y[i];
                    // Signed angle from the velocity to the target direction
                    // (positive = clockwise on screen, y down).
                    const float angle = dmath::atan2(vx * to_y - vy * to_x, vx * to_x + vy * to_y);
                    const float turn = std::clamp(angle, -max_turn, max_turn);
                    if (turn == 0.0f) continue;
                    const float c = dmath::cos(turn);
                    const float s = dmath::sin(turn);
                    p.vx[i] = vx * c - vy * s;
                    p.vy[i] = vx * s + vy * c;
                }
            } else if constexpr (std::is_same_v<M, Orbit>) {
                const float c = dmath::cos(m.angular_speed * dt);
                const float s = dmath::sin(m.angular_speed * dt);
                const float cx = position.x();
                const float cy = position.y();
                const float inv_dt = dt > 0.0f ? 1.0f / dt : 0.0f;
                for (std::size_t i = begin; i < end; ++i) {
                    // Rotate the offset at the start of the tick, replacing
                    // the integrated move.
                    float ox = p.prev_x[i] - cx;
                    float oy = p.prev_y[i] - cy;
                    const float len = dmath::sqrt(ox * ox + oy * oy);
                    const float new_len = std::max(0.0f, len + m.radial_speed * dt);
                    if (len > 0.0f) {
                        const float k = new_len / len;
                        const float rx = (ox * c - oy * s) * k;
                        const float ry = (ox * s + oy * c) * k;
                        ox = rx;
                        oy = ry;
                    } else {
                        // At the center: leave along the spawn velocity.
                        const float speed = dmath::sqrt(p.vx[i] * p.vx[i] + p.vy[i] * p.vy[i]);
                        const float k = speed > 0.0f ? new_len / speed : 0.0f;
                        ox = p.vx[i] * k;
                        oy = p.vy[i] * k;
                    }
                    p.x[i] = cx + ox;
                    p.y[i] = cy + oy;
                    p.vx[i] = (p.x[i] - p.prev_x[i]) * inv_dt;
                    p.vy[i] = (p.y[i] - p.prev_y[i]) * inv_dt;
                }
            } else if constexpr (std::is_same_v<M, Wave>) {
                const float w = dmath::tau * m.frequency;
                for (std::size_t i = begin; i < end; ++i) {
                    const float speed = dmath::sqrt(p.vx[i] * p.vx[i] + p.vy[i] * p.vy[i]);
                    if (speed <= 0.0f) continue;
                    const float now = dmath::sin(w * p.age[i] + m.phase);
                    const float before = dmath::sin(w * (p.age[i] - dt) + m.phase);
                    const float d = m.amplitude * (now - before) / speed;
                    // Perpendicular to the heading: (-vy, vx) / speed.
                    p.x[i] -= p.vy[i] * d;
                    p.y[i] += p.vx[i] * d;
                }
            } else if constexpr (std::is_same_v<M, FaceVelocity>) {
                for (std::size_t i = begin; i < end; ++i) {
                    if (p.vx[i] != 0.0f || p.vy[i] != 0.0f) {
                        p.rotation[i] = dmath::atan2(p.vy[i], p.vx[i]);
                    }
                }
            } else if constexpr (std::is_same_v<M, ColorOverLife>) {
                for (std::size_t i = begin; i < end; ++i) {
                    const float t = normalized_age(p.age[i], p.life[i]);
                    const rhi::Color c{m.start.r + (m.end.r - m.start.r) * t, m.start.g + (m.end.g - m.start.g) * t,
                                       m.start.b + (m.end.b - m.start.b) * t, m.start.a + (m.end.a - m.start.a) * t};
                    p.color[i] = render::pack_rgba8(c);
                }
            } else if constexpr (std::is_same_v<M, SizeOverLife>) {
                for (std::size_t i = begin; i < end; ++i) {
                    const float t = ease(m.ease, normalized_age(p.age[i], p.life[i]));
                    p.size[i] = p.base_size[i] * (m.start + (m.end - m.start) * t);
                }
            } else if constexpr (std::is_same_v<M, KillOutside>) {
                const float x0 = m.min.x();
                const float y0 = m.min.y();
                const float x1 = m.max.x();
                const float y1 = m.max.y();
                for (std::size_t i = begin; i < end; ++i) {
                    if (p.x[i] < x0 || p.x[i] > x1 || p.y[i] < y0 || p.y[i] > y1) {
                        p.dead[i] = 1;
                    }
                }
            }
        },
        module);
}

void Emitter::apply_lifetime(std::size_t begin, std::size_t end) {
    ParticlePool& p = pool_;
    const std::size_t first = simd::enabled() ? kernels::lifetime(p, begin, end) : begin;
    for (std::size_t i = first; i < end; ++i) {
        if (p.life[i] > 0.0f && p.age[i] >= p.life[i]) {
            p.dead[i] = 1;
        }
    }
}

void Emitter::remove_dead() {
    const bool record = config_.record_death_events;
    pool_.remove_dead([&](std::size_t i, ParticleHandle handle) {
        if (record) {
            events_.push_back({EventType::Death, handle, {pool_.x[i], pool_.y[i]}, 0});
        }
    });
}

void Emitter::clear() {
    pool_.kill_all();
    remove_dead();
}

void Emitter::record_hit(std::size_t index, std::uint64_t other) {
    events_.push_back({EventType::Hit, pool_.handle_at(index), {pool_.x[index], pool_.y[index]}, other});
    if (config_.collision.kill_on_hit) {
        pool_.kill(index);
    }
}

ParticleHandle Emitter::spawn(const ParticleSpawn& p) {
    const ParticleHandle handle = pool_.spawn(p);
    if (handle.valid() && config_.record_spawn_events) {
        events_.push_back({EventType::Spawn, handle, p.position, 0});
    }
    return handle;
}

void Emitter::spawn_heading(vec2 origin, float angle, const ParticleTemplate& t, Pcg32& rng) {
    const float speed = t.speed + jitter(rng, t.speed_jitter);
    const float life = t.life + jitter(rng, t.life_jitter);
    const vec2 dir{dmath::cos(angle), dmath::sin(angle)};
    ParticleSpawn s;
    s.position = origin + dir * t.offset;
    s.velocity = dir * speed;
    s.life = t.life > 0.0f ? std::max(life, 1e-6f) : 0.0f; // jitter never makes a mortal particle immortal
    s.size = t.size;
    s.color = render::pack_rgba8(t.color);
    s.rotation = t.face_heading ? angle : 0.0f;
    s.spin = t.spin;
    spawn(s);
}

std::uint32_t Emitter::emit(Pattern& pattern, vec2 origin, const ParticleTemplate& particle, Pcg32& rng) {
    const std::size_t before = pool_.size_live();
    // `count` headings evenly across `spread` centered on `center`; a full
    // circle (spread >= tau) doesn't repeat its first heading at the end.
    const auto fan = [&](std::uint32_t count, float center, float spread) {
        if (count == 0) return;
        if (count == 1) {
            spawn_heading(origin, center, particle, rng);
            return;
        }
        const bool full = spread >= dmath::tau;
        const float step = full ? dmath::tau / static_cast<float>(count) : spread / static_cast<float>(count - 1);
        const float start = full ? center : center - spread * 0.5f;
        for (std::uint32_t k = 0; k < count; ++k) {
            spawn_heading(origin, wrap_angle(start + step * static_cast<float>(k)), particle, rng);
        }
    };
    std::visit(
        [&](auto& p) {
            using P = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<P, Ring>) {
                fan(p.count, p.phase, dmath::tau);
            } else if constexpr (std::is_same_v<P, Arc>) {
                fan(p.count, p.direction, p.spread);
            } else if constexpr (std::is_same_v<P, Aimed>) {
                const vec2 to = target - origin;
                const float dir = (to.x() != 0.0f || to.y() != 0.0f) ? dmath::atan2(to.y(), to.x()) : dmath::half_pi;
                fan(p.count, dir, p.spread);
            } else if constexpr (std::is_same_v<P, Burst>) {
                for (std::uint32_t k = 0; k < p.count; ++k) {
                    const float a = p.direction + (rng.next_float() - 0.5f) * p.spread;
                    spawn_heading(origin, wrap_angle(a), particle, rng);
                }
            } else if constexpr (std::is_same_v<P, Spiral>) {
                fan(p.arms, p.phase, dmath::tau);
                p.phase = wrap_angle(p.phase + p.step);
            }
        },
        pattern);
    return static_cast<std::uint32_t>(pool_.size_live() - before);
}

void Emitter::run_spawners(Pcg32& rng) {
    for (Spawner& s : config_.spawners) {
        if (!s.active || s.shots == 0) continue;
        if (s.countdown > 0) {
            --s.countdown;
            continue;
        }
        emit(s.pattern, position + s.offset, s.particle, rng);
        if (s.shots > 0) --s.shots;
        s.countdown = s.interval > 0 ? s.interval - 1 : 0;
    }
}

void Emitter::hash_state(StateHasher& h) const {
    const ParticlePool& p = pool_;
    for (const auto* column : {&p.x, &p.y, &p.prev_x, &p.prev_y, &p.vx, &p.vy, &p.age, &p.life, &p.size,
                               &p.base_size, &p.rotation, &p.spin}) {
        h.span(std::span<const float>(*column));
    }
    h.span(std::span<const std::uint32_t>(p.color));
    h.span(std::span<const std::uint32_t>(p.tag));
    h.span(std::span<const std::uint32_t>(p.slots()));
    h.span(std::span<const std::uint32_t>(p.free_slots()));
    for (const Spawner& s : config_.spawners) {
        h.value(s.countdown);
        h.value(s.shots);
        h.value(s.active);
        if (const auto* spiral = std::get_if<Spiral>(&s.pattern)) h.value(spiral->phase);
    }
    // Pcg32 keeps its state private; the next outputs of a copy fingerprint it.
    Pcg32 probe = rng_;
    h.value(probe.next_u32());
    h.value(probe.next_u32());
    h.value(position);
    h.value(target);
    h.value(ticks_);
}

} // namespace cuervo::particles

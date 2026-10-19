#pragma once

#include <cstdint>
#include <variant>

#include "cuervo/math/vec.hpp"
#include "cuervo/rhi/rhi.hpp"

// Native particle modules (DESIGN.md 4.6 "Native modules"). An emitter runs
// its module stack in order, each module as one tight loop over the whole
// pool, after the built-in integration step:
//
//   prev = pos; age += dt; pos += vel * dt; rotation += spin * dt
//   modules...                (stack order)
//   lifetime: age >= life -> killed
//   foreach hook              (script `foreach`, LANGUAGE.md 5)
//   remove dead, run spawners
//
// Velocity changes made by a module therefore move the particle from the
// next tick on (explicit Euler), except Orbit, which places it directly.
// Units: world units and seconds; angles in radians, positive = clockwise.
// Gameplay emitters must stay deterministic: modules use only dmath and
// plain f32 arithmetic.
namespace cuervo::particles {

// Constant acceleration: gravity, wind.
struct Accelerate {
    vec2 acceleration;
};

// Linear drag: velocity *= max(0, 1 - coefficient * dt) every tick.
struct Drag {
    float coefficient = 1.0f;
};

// Speeds particles up (or down, if negative) along their heading, clamped to
// [min_speed, max_speed]. Bullets that start slow and accelerate.
struct SpeedRamp {
    float acceleration = 0.0f;
    float min_speed = 0.0f;
    float max_speed = 1.0e9f;
};

// Rotates the velocity at a constant rate: curving shots.
struct Turn {
    float rate = 0.0f; // radians per second
};

// Steers the velocity towards the emitter's target, turning at most
// `turn_rate` per second and keeping the speed. Starts after `delay` seconds
// of age, and stops after `duration` seconds of homing (<= 0: forever).
struct Homing {
    float turn_rate = 3.0f;
    float delay = 0.0f;
    float duration = 0.0f;
};

// Circles the emitter's position: the offset from the emitter rotates by
// `angular_speed` per second while its length changes by `radial_speed` per
// second. Overrides the integrated position (the velocity is set to match,
// for interpolation and FaceVelocity).
struct Orbit {
    float angular_speed = 1.0f;
    float radial_speed = 0.0f;
};

// Sine wobble across the direction of travel: the particle is displaced
// sideways by amplitude * sin(tau * frequency * age + phase) (DESIGN.md 4.6
// "sine/wobble"). The displacement does not change the velocity.
struct Wave {
    float amplitude = 8.0f; // world units
    float frequency = 2.0f; // cycles per second
    float phase = 0.0f;     // radians
};

// rotation = heading of the velocity (capsules pointing where they fly).
struct FaceVelocity {};

// Color interpolated over the normalized age (age / life). Immortal particles
// keep `start`.
struct ColorOverLife {
    rhi::Color start{1.0f, 1.0f, 1.0f, 1.0f};
    rhi::Color end{1.0f, 1.0f, 1.0f, 0.0f};
};

enum class Ease : std::uint8_t { Linear, EaseIn, EaseOut, Smooth };

// Size = spawn size * lerp(start, end, ease(age / life)). Immortal particles
// keep `start`.
struct SizeOverLife {
    float start = 1.0f;
    float end = 0.0f;
    Ease ease = Ease::Linear;
};

// Kills particles whose position leaves the rectangle (bullets leaving the
// screen). Uses the integrated position of this tick.
struct KillOutside {
    vec2 min;
    vec2 max;
};

using Module = std::variant<Accelerate, Drag, SpeedRamp, Turn, Homing, Orbit, Wave, FaceVelocity, ColorOverLife,
                            SizeOverLife, KillOutside>;

// Normalized-age easing shared by the over-life modules; t in [0, 1].
constexpr float ease(Ease e, float t) {
    switch (e) {
        case Ease::EaseIn: return t * t;
        case Ease::EaseOut: return t * (2.0f - t);
        case Ease::Smooth: return t * t * (3.0f - 2.0f * t);
        case Ease::Linear: break;
    }
    return t;
}

} // namespace cuervo::particles

#pragma once

#include <cstdint>
#include <variant>

#include "cuervo/math/dmath.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/rhi/rhi.hpp"

// Spawn patterns (DESIGN.md 4.6 "Spawn rules") for bullet-hell style
// emission. A pattern decides where particles appear and which way they head;
// a ParticleTemplate fills in speed, life, size and color. All randomness
// comes from the PCG32 passed to Emitter::emit (the simulation's RNG for
// gameplay emitters), so patterns replay exactly.
namespace cuervo::particles {

// What every particle of one shot gets. Jitters are uniform +/- ranges.
struct ParticleTemplate {
    float speed = 120.0f;
    float speed_jitter = 0.0f;
    float life = 6.0f;          // seconds; <= 0 = immortal
    float life_jitter = 0.0f;
    float size = 4.0f;
    rhi::Color color{1.0f, 1.0f, 1.0f, 1.0f};
    float spin = 0.0f;
    float offset = 0.0f;        // spawn distance from the origin along the heading
    bool face_heading = true;   // rotation = heading angle (else 0)
};

// `count` headings evenly around the circle, starting at `phase`.
struct Ring {
    std::uint32_t count = 12;
    float phase = 0.0f;
};

// `count` headings evenly across `spread` radians centered on `direction`
// (a single particle flies straight along `direction`).
struct Arc {
    std::uint32_t count = 5;
    float direction = dmath::half_pi; // down the screen
    float spread = 0.5f;
};

// An Arc centered on the direction from the origin to the emitter's target.
struct Aimed {
    std::uint32_t count = 1;
    float spread = 0.0f;
};

// `count` particles at random headings within `spread` around `direction`,
// each with its own speed/life jitter: explosions, shotgun blasts.
struct Burst {
    std::uint32_t count = 16;
    float direction = 0.0f;
    float spread = dmath::tau;
};

// A Ring of `arms` whose phase advances by `step` every shot; fired
// repeatedly by a Spawner it draws spiral arms.
struct Spiral {
    std::uint32_t arms = 4;
    float phase = 0.0f;
    float step = 0.2f;
};

using Pattern = std::variant<Ring, Arc, Aimed, Burst, Spiral>;

// Fires a pattern every `interval` ticks from the emitter's position plus
// `offset`, after an initial `delay` (ticks). `shots` < 0 = forever.
struct Spawner {
    Pattern pattern;
    ParticleTemplate particle;
    std::uint32_t interval = 10;
    std::uint32_t delay = 0;
    std::int32_t shots = -1;
    vec2 offset{};
    bool active = true;
    std::uint32_t countdown = 0; // ticks until the next shot (state)
};

} // namespace cuervo::particles

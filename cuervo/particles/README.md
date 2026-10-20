# cuervo/particles and cuervo/collision

The general particle system (DESIGN.md 4.6) and the collision broadphase
(DESIGN.md 4.7). There is no bullet system: bullets are gameplay particles
with a collision layer.

## Files
| File | What |
|---|---|
| `particle_pool.hpp` | `ParticlePool` (SoA columns, stable handles), `ParticleRef` |
| `modules.hpp` | native modules (`Module` variant) |
| `patterns.hpp` | spawn patterns (`Pattern` variant), `ParticleTemplate`, `Spawner` |
| `emitter.hpp` | `Emitter`, `EmitterConfig`, collision/render config, events, `ForeachHook` |
| `particle_system.hpp` | `ParticleSystem`: emitters + simulation RNG (+ per-emitter child streams) + `CollisionWorld`, `Hit` |
| `particle_render.hpp` | `draw_emitter()` into an `SdfBatcher` |
| `../collision/shapes.hpp` | `Circle`, `Capsule`, `Aabb`, exact overlap tests |
| `../collision/collision_world.hpp` | `CollisionWorld`: spatial hash, layers, queries |

## Quick start
```cpp
particles::ParticleSystem system(seed);            // owns the simulation RNG + emitter streams

particles::EmitterConfig cfg;                      // gameplay mode by default
cfg.modules = {particles::Turn{0.4f}, particles::FaceVelocity{},
               particles::KillOutside{{-40, -40}, {1320, 760}}};
cfg.collision = {.enabled = true, .layer = 1, .hits = collision::layer_bit(0), .radius = 3.0f};
cfg.render.shape = render::SdfShape::Capsule;
particles::Spawner ring;
ring.pattern = particles::Spiral{.arms = 8, .step = 0.12f};
ring.particle.speed = 120.0f;
ring.interval = 3;                                 // ticks
cfg.spawners.push_back(ring);
const particles::EmitterId boss = system.add_emitter(std::move(cfg));

// every fixed tick:
system.emitter(boss).position = boss_pos;
system.emitter(boss).target = player_pos;
system.tick(dt, [&](collision::CollisionWorld& w) {   // the game's own shapes
    w.add_circle({player_pos, 3.0f}, /*layer*/ 0, /*user*/ player_id);
});
for (const particles::Hit& hit : system.hits()) { /* hit.target_user == player_id ... */ }

// every rendered frame:
for (EmitterId id = 0; id < system.emitter_count(); ++id)
    particles::draw_emitter(system.emitter(id), batch, step.alpha);
```
`cuervo --demo particles` (runtime/particles_demo.cpp) is a complete example.

## The tick
`ParticleSystem::tick(dt, add_colliders)` runs, in this fixed order:

1. **simulate** every emitter, in emitter id order:
   1. clear last tick's events
   2. integrate: `prev = pos; age += dt; pos += vel * dt; rotation += spin * dt`
   3. modules, in stack order, each one tight loop over the pool
   4. lifetime: `life > 0 && age >= life` kills
   5. **foreach hook** (scripts)
   6. remove dead (swap-remove, ascending index; Death events)
2. **spawn**: every emitter's spawners, in emitter id order, each drawing
   from the emitter's own RNG stream (see "Random streams").
3. **colliders**: clear the world; add every collidable emitter's particles
   (emitter id order, then pool index order); call `add_colliders`; build.
4. **hits**: per collidable emitter, `pairs(its layer, its hits mask)`;
   each pair is a `Hit` (sorted by emitter, particle proxy, target proxy) and
   a Hit event; `kill_on_hit` particles die and are removed immediately.

Every phase runs on the job system (`set_jobs`, default
`jobs::default_jobs()`; DESIGN.md 4.15): particle ranges for 1 and 3, emitters
for 2 and 4, a parallel counting sort for the hash build. Jobs write disjoint
data and merge in emitter order, so results are bit-identical for any thread
count; `foreach` hooks always run on the calling thread, in id order.

The phases are public (`simulate`, `spawn`, `collect_colliders`,
`resolve_hits`) for callers that need to run something in between — the
demo queries the graze radius between 3 and 4, while pool indices encoded in
proxy `user` values are still valid. After the tick the world stays built for
the game's own queries.

Velocity changes made by a module move the particle from the next tick on
(explicit Euler); `Orbit` and `Wave` move the position directly.

Collision is discrete: it tests positions after the tick's move. A shot
moving more than its own size plus the target's per tick can pass through
(use a capsule collider for fast shots).

## Modes
| | Gameplay | Visual |
|---|---|---|
| RNG | the emitter's own child stream | the emitter's own child stream |
| Collision | opt-in (`CollisionConfig`) | never (forced off) |
| Runs on | CPU | CPU; GPU compute on the Full tier is planned |

Visual emitters never feed back into the simulation, but their state is part
of `state_hash()` (it's still deterministic, only isolated).

## Random streams (DESIGN.md 4.15)
Every emitter, gameplay or visual, draws from its own PCG32 stream:
`derive_stream(system seed, {rng_stream::particles, emitter id, generation})`
(`cuervo/core/rng.hpp`). `generation` counts `replace_emitter()` calls on that
id. Consequences:
- emitters never touch `system.rng()`, the simulation stream that scripts see
  as `Random`; a script drawing a number can't shift a bullet pattern, and an
  emitter firing can't shift the script's numbers;
- adding an emitter (e.g. a visual one for sparks) changes no other
  emitter's draws, as long as ids are appended (ids are part of the key);
- emitters can spawn on different jobs without a serial RNG order.
`emit(pattern, origin, template)` and `run_spawners()` use the emitter's stream;
the overloads taking a `Pcg32&` draw from the given one instead (tests).
A standalone `Emitter` (not in a system) seeds its stream from `config.seed`.

## Native modules (`modules.hpp`)
Units are world units and seconds; angles are radians, positive = clockwise
(y down). The built-in step covers velocity, angular velocity (`spin`) and
lifetime.

| Module | Effect |
|---|---|
| `Accelerate{a}` | `vel += a * dt` (gravity, wind) |
| `Drag{k}` | `vel *= max(0, 1 - k * dt)` |
| `SpeedRamp{accel, min, max}` | speed along the heading changes by `accel`, clamped |
| `Turn{rate}` | rotate the velocity by `rate * dt` (curving shots) |
| `Homing{turn_rate, delay, duration}` | steer towards `emitter.target`, at most `turn_rate` rad/s, keeping speed |
| `Orbit{angular_speed, radial_speed}` | circle `emitter.position`, radius growing by `radial_speed` |
| `Wave{amplitude, frequency, phase}` | sine displacement across the heading |
| `FaceVelocity{}` | `rotation = heading` |
| `ColorOverLife{start, end}` | color lerp over `age / life` |
| `SizeOverLife{start, end, ease}` | `size = spawn size * lerp(start, end, ease(age / life))` |
| `KillOutside{min, max}` | kill outside a rectangle |

Adding a module: add a struct to `modules.hpp`, append it to the `Module`
variant, add its loop to `Emitter::run_module` (dmath only; no libm), test it
in `tests/cuervo/particle_tests.cpp`.

## Patterns (`patterns.hpp`)
`Ring{count, phase}`, `Arc{count, direction, spread}`, `Aimed{count, spread}`
(centered on `emitter.target`), `Burst{count, direction, spread}` (random
headings), `Spiral{arms, phase, step}` (a ring whose phase advances every
shot). A `ParticleTemplate` gives speed/life (with +/- jitter), size, color,
spin, spawn offset and whether rotation faces the heading. The RNG is drawn
from only for `Burst` headings and non-zero jitters.

`Spawner{pattern, particle, interval, delay, shots, offset, active}` fires
every `interval` ticks after `delay` ticks, `shots` times (< 0 = forever).
`Emitter::emit(pattern, origin, template, rng)` fires once, immediately.

## Handles, indices and events
- Pool indices change when particles are removed; `ParticleHandle`
  (slot + generation) stays valid while the particle lives and goes stale
  after (`pool.alive(h)`, `pool.index_of(h)`).
- `emitter.events()`: Spawn (opt-in: `record_spawn_events`), Death (default
  on), Hit (`other` = the target proxy's `user`). Kept until the emitter's
  next simulate, in the order they happened.
- Collision proxies of particles carry
  `ParticleSystem::particle_user(emitter, index)` (bit 63 set); game shapes
  must leave bit 63 clear.

## Script binding notes
The API is meant to be bound as-is:
- `EmitterConfig`, `Module` alternatives, `Pattern` alternatives,
  `ParticleTemplate`, `Spawner`, `CollisionConfig`, `RenderConfig` are plain
  aggregates of numbers, `vec2` and `rhi::Color` — script structs/enums.
- `Emitter`: `position`, `target`, `emit(...)`, `spawn(...)`, `kill(handle)`,
  `events()`, `size()`, `pool()`. Emitters live behind `unique_ptr` in the
  system, so `Emitter&` stays valid for the system's lifetime.
- `foreach p in emitter.pool { ... }` (LANGUAGE.md 5): install the compiled
  body with `emitter.set_foreach(ForeachHook)`; it runs at step 1.5. The fast
  path reads/writes the SoA columns directly (`pool.x[i]`, ...); the slow path
  can use `ParticleRef` (`p.pos()`, `p.set_vel(v)`, `p.age()`, `p.kill()`).
  Field names follow LANGUAGE.md: `pos`, `vel`, `age`, `life`, `size`,
  `rotation`, `spin`, `color`, `tag` (a free u32 for game use).
  The hook may kill but must not spawn (spawning belongs to the spawn phase:
  scripts call `emit` from `update()`).
- `on_spawn` / `on_death` / `on_hit` signals: dispatch from `events()` after
  the tick (or from `system.hits()`), in their stored order.

## Collision world (`cuervo/collision`)
- Shapes: `Circle`, `Capsule` (segment + radius). Overlap tests are plain f32
  `+ - * /` (no sqrt); touching counts as overlapping. AABB/OBB hitboxes
  (DESIGN.md 4.7) are planned.
- `CollisionWorld(cell_size)`: `clear()`, `add_circle/add_capsule(shape,
  layer 0..31, user)` (ids in call order; or `add_uninitialized(n)` then
  `set_circle/set_capsule` from parallel jobs), `build()`, then queries:
  `overlap(circle|capsule, mask)`, `within(point, distance, mask)`,
  `any(circle, mask)`, `pairs(mask_a, mask_b)`. Results are appended sorted
  by proxy id (pairs by (a, b)); with overlapping masks an unordered pair is
  reported once as (lower, higher). Queries are const and thread-safe on a
  built world.
- The hash is a counting sort into `bit_ceil(entries)` buckets (no unordered
  containers, no per-cell allocation). A proxy spanning more than 64 cells
  (bosses, lasers, screen-wide hitboxes) goes to a secondary hash with 8x
  coarser cells instead; queries consult both (DESIGN.md 4.15). Only absurdly
  large shapes (> 1024 coarse cells) fall back to a list every query scans.
  A cell size around the common bullet diameter is best (the demo uses 32).

## Performance (release, RTX 3060 Ti host CPU)
`cuervo --demo particles --bench 8 [--intensity x]`, GL; sim = one 60 Hz tick:

| Bullets | sim ms/tick | particles | collider build | queries + hits |
|---|---|---|---|---|
| ~11.6k | 0.54 | 0.15 | 0.32 | 0.08 |
| ~28.7k | 1.31 | 0.36 | 0.78 | 0.18 |
| ~51.7k | 2.34 | 0.63 | 1.39 | 0.33 |

All single-threaded, scalar (the baseline before the job system and SIMD).

### Jobs + SIMD (release, Ryzen 7 5800X3D)
`cuervo --demo particles --intensity 4.5 --bench 10 --jobs N [--simd off]`,
GL headless, ~52.3k bullets; sim ms per 60 Hz tick (particles / hash build /
queries+hits):

| Build | --jobs 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| sse2 (default), SIMD on | 1.80 (0.22 / 1.22 / 0.36) | 0.99 | 0.63 | 0.50 (0.06 / 0.34 / 0.10) |
| sse2, `--simd off` | 2.42 (0.66 / 1.40 / 0.36) | 1.32 | 0.78 | 0.56 |
| avx2, SIMD on | 1.71 (0.13 / 1.22 / 0.36) | 0.94 | 0.60 | 0.47 (0.05 / 0.32 / 0.10) |
| none (scalar build) | 2.40 (0.66 / 1.38 / 0.36) | 1.29 | 0.78 | 0.57 |

The state hash is the same in every cell of this table (and on WASM). The
hash build (counting sort, mostly memory traffic) dominates now; its SIMD part
is only the cell-range computation.

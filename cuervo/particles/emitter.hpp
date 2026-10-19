#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "cuervo/collision/collision_world.hpp"
#include "cuervo/core/hash.hpp"
#include "cuervo/core/rng.hpp"
#include "cuervo/particles/modules.hpp"
#include "cuervo/particles/particle_pool.hpp"
#include "cuervo/particles/patterns.hpp"
#include "cuervo/render/sdf_batcher.hpp"

// One emitter: a pool, a module stack, spawners and a render shape
// (DESIGN.md 4.6 "Emitters"). See cuervo/particles/README.md.
namespace cuervo::particles {

// DESIGN.md 4.6 "Two simulation modes".
enum class Mode : std::uint8_t {
    Gameplay, // CPU, deterministic, may collide
    Visual,   // never feeds back into the simulation, never collides.
              // TODO(Full tier): GPU compute path; this CPU path is the Compatible/web fallback.
};

// Which way a capsule collider points.
enum class CapsuleAxis : std::uint8_t {
    Velocity, // along the direction of travel (one sqrt; the usual case for shots)
    Rotation, // along the `rotation` column (spinning lasers; costs a sin + cos)
};

// Opt-in collision (gameplay mode only, DESIGN.md 4.6 "Collision"): every
// live particle joins the spatial hash on `layer` as a circle, or as a
// capsule of half segment length `half_length` when that is > 0. The radius
// is `radius`, or the particle's current size when `radius` <= 0. Overlaps
// with proxies on `hits` layers become Hit events (and kill the particle if
// `kill_on_hit`).
struct CollisionConfig {
    bool enabled = false;
    std::uint32_t layer = 0;           // 0..31
    collision::LayerMask hits = 0;     // layers that count as hits
    float radius = 0.0f;               // world units; <= 0: the particle's size
    float half_length = 0.0f;          // world units; > 0: capsule
    CapsuleAxis axis = CapsuleAxis::Velocity;
    bool kill_on_hit = true;
};

// How the emitter draws (DESIGN.md 4.6 "Rendering"). Shape dimensions are
// multiples of the particle's `size` column; the color is the particle's.
struct RenderConfig {
    bool visible = true;
    render::SdfShape shape = render::SdfShape::Circle;
    float half_length = 1.5f;   // Capsule: half segment length / size (oriented by rotation)
    float thickness = 0.35f;    // Ring: thickness / size
    float corner = 0.3f;        // RoundedRect: corner radius / size
    float outline_width = 0.0f; // world units; 0 = none
    rhi::Color outline_color{1.0f, 1.0f, 1.0f, 1.0f};
};

enum class EventType : std::uint8_t { Spawn, Death, Hit };

// `on_spawn` / `on_death` / `on_hit` (DESIGN.md 4.6 "Script hooks"). The
// events of one tick are kept until the emitter's next simulate(), in the
// order they happened. For Hit, `other` is the `user` value of the collision
// proxy that was hit. A Death event's handle is already stale (the particle
// is gone); it identifies which particle died.
struct ParticleEvent {
    EventType type = EventType::Spawn;
    ParticleHandle particle;
    vec2 position{};
    std::uint64_t other = 0;
};

struct EmitterConfig {
    Mode mode = Mode::Gameplay;
    std::size_t max_particles = 1u << 20;
    std::vector<Module> modules;
    std::vector<Spawner> spawners;
    CollisionConfig collision;
    RenderConfig render;
    bool record_spawn_events = false; // spawns are numerous; off by default
    bool record_death_events = true;
    // Seed of the emitter's own RNG stream while it stands alone. Adding it to
    // a ParticleSystem replaces the stream with a child of the system seed
    // (DESIGN.md 4.15), so inside a system this value is ignored.
    std::uint64_t seed = 0x9e3779b97f4a7c15ULL;
};

class Emitter;

// Script hook (LANGUAGE.md 5 `foreach`): runs every tick after the native
// modules and the lifetime check, before dead particles are removed. Phase 3
// bindings install the compiled `foreach` body here. It may read and write
// any column and kill particles (pool.kill(i) or ParticleRef::kill), but must
// not spawn: spawning happens in the spawn phase (emit()/spawn() from scripts
// run in update(), which is outside the hook).
using ForeachHook = std::function<void(Emitter& emitter, float dt)>;

class Emitter {
public:
    explicit Emitter(EmitterConfig config = {});

    Mode mode() const { return config_.mode; }
    const EmitterConfig& config() const { return config_; }

    // Per-tick inputs: where the emitter is and what it aims at (Aimed
    // patterns, Homing, Orbit). Set by game code or scripts before the tick.
    vec2 position;
    vec2 target;

    ParticlePool& pool() { return pool_; }
    const ParticlePool& pool() const { return pool_; }
    std::size_t size() const { return pool_.size_live(); }

    std::vector<Module>& modules() { return config_.modules; }
    std::vector<Spawner>& spawners() { return config_.spawners; }
    // Adds a spawner; its first shot comes after `delay` ticks.
    std::size_t add_spawner(Spawner spawner);
    CollisionConfig& collision() { return config_.collision; }
    const CollisionConfig& collision() const { return config_.collision; }
    RenderConfig& render() { return config_.render; }
    const RenderConfig& render() const { return config_.render; }
    bool collides() const { return config_.mode == Mode::Gameplay && config_.collision.enabled; }

    // One fixed tick = simulate(dt) then run_spawners(). Both touch only this
    // emitter (its pool, spawners and own RNG stream), so ParticleSystem may
    // run different emitters on different jobs (DESIGN.md 4.14, 4.15).
    void update(float dt) {
        simulate(dt);
        run_spawners();
    }

    // Clears last tick's events, integrates, runs the modules, the lifetime
    // check and the foreach hook, then removes dead particles. Equivalent to
    // begin_tick(); step_range(dt, 0, size()); finish_tick(dt).
    void simulate(float dt);

    // The phases of simulate(), for ParticleSystem's job split (DESIGN.md
    // 4.15). step_range() integrates, runs every module and the lifetime
    // check on particles [begin, end) only; every module is per-particle, so
    // disjoint ranges may run on different threads and the result is the same
    // for any split. finish_tick() runs the foreach hook (scripts: main
    // thread only) and removes dead particles.
    void begin_tick() { events_.clear(); }
    void step_range(float dt, std::size_t begin, std::size_t end);
    void finish_tick(float dt);
    bool has_foreach() const { return static_cast<bool>(foreach_); }
    // Fires due spawners, drawing from the emitter's own stream (rng()).
    // The overload draws from `rng` instead (tests, custom setups).
    void run_spawners() { run_spawners(rng_); }
    void run_spawners(Pcg32& rng);

    // Immediate emission (scripts: `emitter.emit(Ring { count = 36 }, ...)`).
    // Returns the number of particles spawned. A Spiral's phase advances.
    // Draws from the emitter's own stream, or from `rng` when given.
    std::uint32_t emit(Pattern& pattern, vec2 origin, const ParticleTemplate& particle, Pcg32& rng);
    std::uint32_t emit(Pattern&& pattern, vec2 origin, const ParticleTemplate& particle, Pcg32& rng) {
        return emit(pattern, origin, particle, rng);
    }
    std::uint32_t emit(Pattern& pattern, vec2 origin, const ParticleTemplate& particle) {
        return emit(pattern, origin, particle, rng_);
    }
    std::uint32_t emit(Pattern&& pattern, vec2 origin, const ParticleTemplate& particle) {
        return emit(pattern, origin, particle, rng_);
    }
    ParticleHandle spawn(const ParticleSpawn& p);

    void kill(std::size_t index) { pool_.kill(index); }
    bool kill(ParticleHandle handle) { return pool_.kill(handle); }
    // Removes killed particles now (recording Death events) instead of at the
    // next simulate(); ParticleSystem uses it after resolving hits.
    void remove_dead();
    // Kills and removes every particle (Death events are recorded).
    void clear();

    std::span<const ParticleEvent> events() const { return events_; }
    void record_hit(std::size_t index, std::uint64_t other);

    void set_foreach(ForeachHook hook) { foreach_ = std::move(hook); }
    // Calls fn(ParticleRef) for every live particle, in index order.
    template<typename Fn>
    void for_each(Fn&& fn) {
        for (std::size_t i = 0; i < pool_.size_live(); ++i) {
            fn(ParticleRef(pool_, i));
        }
    }

    std::uint64_t ticks() const { return ticks_; }
    // The emitter's own random stream (both modes): a child of the system
    // seed keyed by emitter id (DESIGN.md 4.15), or `config.seed` standalone.
    Pcg32& rng() { return rng_; }
    void set_rng(const Pcg32& rng) { rng_ = rng; }

    // Adds everything that defines this emitter's future (columns, slots,
    // spawner state, inputs) to `hasher`, for replay comparison.
    void hash_state(StateHasher& hasher) const;

private:
    void integrate(float dt, std::size_t begin, std::size_t end);
    void run_module(const Module& module, float dt, std::size_t begin, std::size_t end);
    void apply_lifetime(std::size_t begin, std::size_t end);
    void spawn_heading(vec2 origin, float angle, const ParticleTemplate& t, Pcg32& rng);

    EmitterConfig config_;
    ParticlePool pool_;
    std::vector<ParticleEvent> events_;
    ForeachHook foreach_;
    Pcg32 rng_;
    std::uint64_t ticks_ = 0;
};

} // namespace cuervo::particles

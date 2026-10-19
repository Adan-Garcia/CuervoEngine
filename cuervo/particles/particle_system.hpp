#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "cuervo/collision/collision_world.hpp"
#include "cuervo/core/rng.hpp"
#include "cuervo/particles/emitter.hpp"

// All emitters of one simulation plus their collision (DESIGN.md 4.6, 4.7).
// Owns the simulation RNG, the per-emitter child streams derived from it and
// the CollisionWorld that is rebuilt every tick.
//
// One fixed tick (tick()), in this order:
//   1. simulate   every emitter in id order (integrate, modules, lifetime,
//                 foreach hook, remove dead)
//   2. spawn      every emitter in id order (spawners, each emitter's own
//                 RNG stream)
//   3. colliders  clear the world, add every collidable emitter's particles
//                 (emitter id order, then particle index order), then the
//                 game's own shapes (the `add_colliders` callback), build
//   4. hits       per collidable emitter: overlaps with its `hits` layers
//                 become Hit events + hits() entries, kill_on_hit particles
//                 die and are removed
// After tick() the world stays built, so game code can run its own queries
// (graze radius, player-vs-enemy...) against the same state.
//
// Determinism: fixed iteration orders everywhere, results sorted by proxy id
// (collision_world.hpp), one seed: the simulation RNG plus child streams
// keyed by emitter id (DESIGN.md 4.15). The same seed, emitter setup and inputs
// give a bit-identical state_hash() every tick (DESIGN.md 4.1).
//
// Jobs (DESIGN.md 4.14, 4.15): simulate, spawn, collider registration, the
// hash build and hit queries run on the job system. Each job writes a
// disjoint range (an emitter, a range of one emitter's particles, a range of
// proxy ids) and per-job results merge in emitter order, so state_hash() is
// the same for any thread count. foreach hooks (scripts) always run on the
// calling thread, in emitter id order.
namespace cuervo::jobs {
class JobSystem;
}

namespace cuervo::particles {

using EmitterId = std::uint32_t;

// One particle-vs-proxy overlap found in step 4.
struct Hit {
    EmitterId emitter = 0;
    ParticleHandle particle;          // stale after the tick if the particle died
    vec2 position{};                  // particle position when it hit
    collision::ProxyId particle_proxy = 0;
    collision::ProxyId target_proxy = 0;
    std::uint64_t target_user = 0;    // the target proxy's `user` value
};

class ParticleSystem {
public:
    explicit ParticleSystem(std::uint64_t seed = 0, float cell_size = 32.0f);

    // Emitters are never removed (ids stay stable); clear() one to reuse it,
    // or replace_emitter() to give the id a new configuration. Each emitter
    // draws from its own child stream of the system seed, keyed by
    // (rng_stream::particles, id, generation): the generation counts
    // replacements, so a reused id doesn't replay its predecessor's draws.
    EmitterId add_emitter(EmitterConfig config);
    Emitter& replace_emitter(EmitterId id, EmitterConfig config);
    Emitter& emitter(EmitterId id) { return *emitters_[id]; }
    const Emitter& emitter(EmitterId id) const { return *emitters_[id]; }
    std::size_t emitter_count() const { return emitters_.size(); }
    std::size_t particle_count() const;

    // The simulation RNG (the script-visible `Random`). Gameplay code draws
    // from it in a fixed order (DESIGN.md 4.1). Emitters never touch it: they
    // spawn from their own child streams (DESIGN.md 4.15).
    Pcg32& rng() { return rng_; }
    std::uint64_t seed() const { return seed_; }

    // The job system the tick runs on (also used by the collision world);
    // nullptr runs everything inline. Defaults to jobs::default_jobs().
    void set_jobs(jobs::JobSystem* jobs);
    jobs::JobSystem* jobs() const { return jobs_; }

    collision::CollisionWorld& collision() { return world_; }
    const collision::CollisionWorld& collision() const { return world_; }

    using ColliderFn = std::function<void(collision::CollisionWorld&)>;
    // Runs one fixed tick (steps 1-4 above).
    void tick(float dt, const ColliderFn& add_colliders = {});

    // The steps of tick(), for callers that need to interleave work.
    void simulate(float dt);
    void spawn();
    void collect_colliders(const ColliderFn& add_colliders = {});
    void resolve_hits();

    // Hits of the last resolve_hits(), sorted by (emitter id, particle proxy,
    // target proxy).
    std::span<const Hit> hits() const { return hits_; }

    // Collision proxies added for particles carry this `user` value, so
    // queries can tell particles from game shapes. Game shapes must leave
    // bit 63 clear.
    static constexpr std::uint64_t particle_user_bit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t particle_user(EmitterId emitter, std::uint32_t index) {
        return particle_user_bit | (std::uint64_t{emitter} << 32) | index;
    }
    static constexpr bool is_particle(std::uint64_t user) { return (user & particle_user_bit) != 0; }
    static constexpr EmitterId user_emitter(std::uint64_t user) {
        return static_cast<EmitterId>((user & ~particle_user_bit) >> 32);
    }
    // Index of the particle in its pool; valid until that emitter removes or
    // spawns particles (i.e. until the next tick's simulate()).
    static constexpr std::uint32_t user_index(std::uint64_t user) { return static_cast<std::uint32_t>(user); }

    std::uint64_t ticks() const { return ticks_; }

    // Fingerprint of everything that determines future ticks: RNG, every
    // emitter's state, the tick count (DESIGN.md 4.1 replay comparison).
    std::uint64_t state_hash() const;

private:
    Pcg32 stream_for(EmitterId id) const;
    // Runs fn(k) for k in [0, count) on the job system (or inline).
    template<typename Fn>
    void run_jobs(std::size_t count, Fn&& fn);

    // A contiguous range of one emitter's particles: the unit of parallel
    // per-particle work.
    struct ParticleRange {
        EmitterId emitter;
        std::uint32_t begin, end;
    };
    // Splits every selected emitter's particles into ranges, in emitter order.
    template<typename Pred>
    void split_ranges(Pred&& include);

    jobs::JobSystem* jobs_;
    std::vector<ParticleRange> ranges_;
    std::vector<std::uint32_t> proxy_base_;                 // per emitter: first collision proxy
    std::vector<std::vector<collision::ProxyPair>> emitter_pairs_; // per emitter: resolve_hits scratch

    std::uint64_t seed_;
    Pcg32 rng_;
    std::vector<std::uint32_t> generations_; // per emitter id: replace_emitter() count
    std::vector<std::unique_ptr<Emitter>> emitters_; // stable addresses for bindings
    collision::CollisionWorld world_;
    std::vector<Hit> hits_;
    std::uint64_t ticks_ = 0;
};

} // namespace cuervo::particles

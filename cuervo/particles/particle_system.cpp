#include "cuervo/particles/particle_system.hpp"

#include <algorithm>

#include "cuervo/core/hash.hpp"
#include "cuervo/core/profiler.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/math/dmath.hpp"

namespace cuervo::particles {

ParticleSystem::ParticleSystem(std::uint64_t seed, float cell_size)
    : jobs_(&jobs::default_jobs()), seed_(seed), rng_(seed), world_(cell_size) {
    world_.set_jobs(jobs_);
}

Pcg32 ParticleSystem::stream_for(EmitterId id) const {
    return derive_stream(seed_, {rng_stream::particles, id, generations_[id]});
}

EmitterId ParticleSystem::add_emitter(EmitterConfig config) {
    if (config.mode == Mode::Visual && config.collision.enabled) {
        LOG_WARN("Visual particle emitters never collide (DESIGN.md 4.6); collision disabled");
    }
    const auto id = static_cast<EmitterId>(emitters_.size());
    emitters_.push_back(std::make_unique<Emitter>(std::move(config)));
    generations_.push_back(0);
    emitters_.back()->set_rng(stream_for(id));
    return id;
}

Emitter& ParticleSystem::replace_emitter(EmitterId id, EmitterConfig config) {
    if (config.mode == Mode::Visual && config.collision.enabled) {
        LOG_WARN("Visual particle emitters never collide (DESIGN.md 4.6); collision disabled");
    }
    *emitters_[id] = Emitter(std::move(config));
    ++generations_[id];
    emitters_[id]->set_rng(stream_for(id));
    return *emitters_[id];
}

std::size_t ParticleSystem::particle_count() const {
    std::size_t total = 0;
    for (const auto& e : emitters_) total += e->size();
    return total;
}

void ParticleSystem::tick(float dt, const ColliderFn& add_colliders) {
    // Profiling zones observe only (cuervo/core/profiler.hpp); job workers'
    // zones are not recorded, the main thread's phases are.
    CUERVO_PROFILE_ZONE("particles");
    {
        CUERVO_PROFILE_ZONE("simulate");
        simulate(dt);
    }
    {
        CUERVO_PROFILE_ZONE("spawn");
        spawn();
    }
    {
        CUERVO_PROFILE_ZONE("colliders");
        collect_colliders(add_colliders);
    }
    {
        CUERVO_PROFILE_ZONE("hits");
        resolve_hits();
    }
    ++ticks_;
}

// Particles per job range: big enough to amortize the dispatch, small enough
// to balance a few large emitters over the workers.
constexpr std::uint32_t particle_grain = 2048;

template<typename Fn>
void ParticleSystem::run_jobs(std::size_t count, Fn&& fn) {
    if (jobs_ != nullptr) {
        jobs_->run(count, fn);
    } else {
        for (std::size_t k = 0; k < count; ++k) fn(k);
    }
}

template<typename Pred>
void ParticleSystem::split_ranges(Pred&& include) {
    ranges_.clear();
    for (EmitterId id = 0; id < emitters_.size(); ++id) {
        if (!include(*emitters_[id])) continue;
        const auto n = static_cast<std::uint32_t>(emitters_[id]->size());
        for (std::uint32_t begin = 0; begin < n; begin += particle_grain) {
            ranges_.push_back({id, begin, std::min(n, begin + particle_grain)});
        }
    }
}

void ParticleSystem::set_jobs(jobs::JobSystem* jobs) {
    jobs_ = jobs;
    world_.set_jobs(jobs);
}

void ParticleSystem::simulate(float dt) {
    for (auto& e : emitters_) e->begin_tick();
    // Integration, modules and lifetime are per particle (DESIGN.md 4.15):
    // ranges of every emitter run in parallel.
    split_ranges([](const Emitter&) { return true; });
    run_jobs(ranges_.size(), [&](std::size_t k) {
        const ParticleRange& r = ranges_[k];
        emitters_[r.emitter]->step_range(dt, r.begin, r.end);
    });
    // foreach hooks are script code: main thread, emitter id order, each
    // emitter's hook before its removal (a hook may look at other emitters,
    // so with hooks present the whole finish runs serially, as before).
    bool hooks = false;
    for (const auto& e : emitters_) hooks = hooks || e->has_foreach();
    if (hooks) {
        for (auto& e : emitters_) e->finish_tick(dt);
    } else {
        run_jobs(emitters_.size(), [&](std::size_t id) { emitters_[id]->finish_tick(dt); });
    }
}

void ParticleSystem::spawn() {
    // Each emitter spawns from its own stream into its own pool (DESIGN.md
    // 4.15), so emitters spawn in parallel.
    run_jobs(emitters_.size(), [&](std::size_t id) { emitters_[id]->run_spawners(); });
}

void ParticleSystem::collect_colliders(const ColliderFn& add_colliders) {
    world_.clear();
    // Proxy ids stay in (emitter id, pool index) order: each emitter gets a
    // contiguous block, filled in parallel.
    proxy_base_.assign(emitters_.size(), 0);
    std::size_t total = 0;
    for (EmitterId id = 0; id < emitters_.size(); ++id) {
        proxy_base_[id] = static_cast<std::uint32_t>(total);
        if (emitters_[id]->collides()) total += emitters_[id]->size();
    }
    world_.reserve(total + 64);
    world_.add_uninitialized(total);
    split_ranges([](const Emitter& e) { return e.collides(); });
    run_jobs(ranges_.size(), [&](std::size_t k) {
        const ParticleRange& range = ranges_[k];
        const EmitterId id = range.emitter;
        const Emitter& e = *emitters_[id];
        const CollisionConfig& c = e.collision();
        const ParticlePool& p = e.pool();
        for (std::uint32_t i = range.begin; i < range.end; ++i) {
            const float r = c.radius > 0.0f ? c.radius : p.size[i];
            const std::uint64_t user = particle_user(id, i);
            const collision::ProxyId proxy = proxy_base_[id] + i;
            if (c.half_length > 0.0f) {
                vec2 axis;
                if (c.axis == CapsuleAxis::Rotation) {
                    axis = {dmath::cos(p.rotation[i]) * c.half_length, dmath::sin(p.rotation[i]) * c.half_length};
                } else {
                    const float speed = dmath::sqrt(p.vx[i] * p.vx[i] + p.vy[i] * p.vy[i]);
                    const float k_len = speed > 0.0f ? c.half_length / speed : 0.0f; // at rest: a circle
                    axis = {p.vx[i] * k_len, p.vy[i] * k_len};
                }
                const vec2 center{p.x[i], p.y[i]};
                world_.set_capsule(proxy, {center - axis, center + axis, r}, c.layer, user);
            } else {
                world_.set_circle(proxy, {{p.x[i], p.y[i]}, r}, c.layer, user);
            }
        }
    });
    if (add_colliders) add_colliders(world_);
    world_.build();
}

void ParticleSystem::resolve_hits() {
    hits_.clear();
    if (!world_.built()) world_.build();
    // Query batches (DESIGN.md 4.15): every emitter's pairs() runs as its own
    // job into its own vector; hits are recorded afterwards in emitter order.
    emitter_pairs_.resize(emitters_.size());
    run_jobs(emitters_.size(), [&](std::size_t id) {
        std::vector<collision::ProxyPair>& pairs = emitter_pairs_[id];
        pairs.clear();
        const Emitter& e = *emitters_[id];
        if (!e.collides() || e.collision().hits == 0 || e.size() == 0) return;
        // Pairs between this emitter's layer and its hit layers. Other
        // emitters may share the layer, so keep only this emitter's proxies.
        world_.pairs(collision::layer_bit(e.collision().layer), e.collision().hits, pairs);
    });
    for (EmitterId id = 0; id < emitters_.size(); ++id) {
        Emitter& e = *emitters_[id];
        for (const collision::ProxyPair& pair : emitter_pairs_[id]) {
            const std::uint64_t user = world_.user(pair.a);
            if (!is_particle(user) || user_emitter(user) != id) continue;
            const std::uint32_t index = user_index(user);
            const std::uint64_t target_user = world_.user(pair.b);
            hits_.push_back({id, e.pool().handle_at(index), {e.pool().x[index], e.pool().y[index]}, pair.a, pair.b,
                             target_user});
            e.record_hit(index, target_user);
        }
    }
    // Particles killed by hits leave now, so the rendered state and the next
    // tick's simulate() never see them. The world keeps their proxies until
    // the next collect_colliders(); game queries should use proxy user values
    // rather than pool indices after this point.
    run_jobs(emitters_.size(), [&](std::size_t id) { emitters_[id]->remove_dead(); });
}

std::uint64_t ParticleSystem::state_hash() const {
    StateHasher h;
    // Pcg32 keeps its state private; the next outputs of a copy fingerprint it.
    Pcg32 probe = rng_;
    h.value(probe.next_u32());
    h.value(probe.next_u32());
    h.value(ticks_);
    for (const auto& e : emitters_) e->hash_state(h);
    return h.digest();
}

} // namespace cuervo::particles

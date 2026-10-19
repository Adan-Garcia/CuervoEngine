#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "cuervo/math/vec.hpp"

// SoA particle storage (DESIGN.md 4.6 "Data layout"). Particles are not ECS
// entities: each emitter owns one pool, and hot loops (native modules, the
// script `foreach` lowering, rendering, collision registration) run straight
// over the column arrays.
//
// Removal is two-phase: kill(i) only marks a particle, and remove_dead()
// swap-removes the marked ones in ascending index order, so the resulting
// order depends only on what happened in the simulation (DESIGN.md 4.1).
//
// Handles stay valid while a particle lives, even though swap-removal moves
// it: a handle names a slot (with a generation) and the slot tracks the
// particle's current index.
namespace cuervo::particles {

struct ParticleHandle {
    static constexpr std::uint32_t invalid_slot = 0xffffffffu;
    std::uint32_t slot = invalid_slot;
    std::uint32_t generation = 0;

    constexpr bool valid() const { return slot != invalid_slot; }
    friend constexpr bool operator==(const ParticleHandle&, const ParticleHandle&) = default;
};

// Initial values of one particle. `life` <= 0 means it never expires.
struct ParticleSpawn {
    vec2 position{};
    vec2 velocity{};
    float life = 0.0f;
    float size = 4.0f;
    std::uint32_t color = 0xffffffffu; // RGBA8 straight alpha (render::pack_rgba8)
    float rotation = 0.0f;
    float spin = 0.0f;                 // radians per second
};

class ParticlePool {
public:
    // --- columns (one entry per live particle, same index everywhere) -------
    std::vector<float> x, y;           // position (world units)
    std::vector<float> prev_x, prev_y; // position at the start of the tick (render interpolation)
    std::vector<float> vx, vy;         // velocity, units per second
    std::vector<float> age;            // seconds since spawn
    std::vector<float> life;           // seconds; <= 0 = immortal
    std::vector<float> size;           // radius-like render/collision scale
    std::vector<float> base_size;      // size at spawn (SizeOverLife scales this)
    std::vector<float> rotation;       // radians, clockwise
    std::vector<float> spin;           // radians per second
    std::vector<std::uint32_t> color;  // RGBA8
    std::vector<std::uint32_t> tag;    // free for game/script use; 0 at spawn
    std::vector<std::uint8_t> dead;    // marked by kill(), cleared by remove_dead()

    std::size_t size_live() const { return x.size(); }
    bool empty() const { return x.empty(); }
    void reserve(std::size_t count);

    // Returns an invalid handle when `max_particles` is reached.
    ParticleHandle spawn(const ParticleSpawn& p);
    void kill(std::size_t index) { dead[index] = 1; }
    bool kill(ParticleHandle handle);
    void kill_all();

    // Swap-removes every killed particle in ascending index order, calling
    // on_remove(index, handle) for each before it is overwritten.
    template<typename Fn>
    void remove_dead(Fn&& on_remove);
    void remove_dead() {
        remove_dead([](std::size_t, ParticleHandle) {});
    }

    // Applies fn(column) to every float column (used for reserve/move/pop).
    template<typename Fn>
    void for_each_float_column(Fn&& fn) {
        for (auto* column : {&x, &y, &prev_x, &prev_y, &vx, &vy, &age, &life, &size, &base_size, &rotation, &spin}) {
            fn(*column);
        }
    }

    std::optional<std::size_t> index_of(ParticleHandle handle) const;
    bool alive(ParticleHandle handle) const { return index_of(handle).has_value(); }
    ParticleHandle handle_at(std::size_t index) const {
        return {slot_of_[index], slot_generation_[slot_of_[index]]};
    }

    std::size_t max_particles = 1u << 20;

    // Slot bookkeeping, exposed for state hashing.
    const std::vector<std::uint32_t>& slots() const { return slot_of_; }
    const std::vector<std::uint32_t>& free_slots() const { return free_slots_; }

private:
    void remove_at(std::size_t index);

    std::vector<std::uint32_t> slot_of_;         // per particle: its slot
    std::vector<std::uint32_t> slot_index_;      // per slot: particle index (if live)
    std::vector<std::uint32_t> slot_generation_; // per slot: bumped when freed
    std::vector<std::uint32_t> free_slots_;      // LIFO, deterministic reuse

    friend class Emitter;
};

// A by-index view of one particle: the shape script bindings expose to
// `foreach p in emitter.pool { ... }` (LANGUAGE.md 5), where `p` is a mutable
// reference. Valid until the pool next removes or spawns particles.
class ParticleRef {
public:
    ParticleRef(ParticlePool& pool, std::size_t index) : pool_(&pool), index_(index) {}

    std::size_t index() const { return index_; }
    ParticleHandle handle() const { return pool_->handle_at(index_); }

    vec2 pos() const { return {pool_->x[index_], pool_->y[index_]}; }
    void set_pos(vec2 p) { pool_->x[index_] = p.x(); pool_->y[index_] = p.y(); }
    vec2 vel() const { return {pool_->vx[index_], pool_->vy[index_]}; }
    void set_vel(vec2 v) { pool_->vx[index_] = v.x(); pool_->vy[index_] = v.y(); }
    float& age() const { return pool_->age[index_]; }
    float& life() const { return pool_->life[index_]; }
    float& size() const { return pool_->size[index_]; }
    float& rotation() const { return pool_->rotation[index_]; }
    float& spin() const { return pool_->spin[index_]; }
    std::uint32_t& color() const { return pool_->color[index_]; }
    std::uint32_t& tag() const { return pool_->tag[index_]; }
    bool dead() const { return pool_->dead[index_] != 0; }
    void kill() const { pool_->kill(index_); }

private:
    ParticlePool* pool_;
    std::size_t index_;
};

template<typename Fn>
void ParticlePool::remove_dead(Fn&& on_remove) {
    std::size_t i = 0;
    while (i < x.size()) {
        if (dead[i]) {
            on_remove(i, handle_at(i));
            remove_at(i); // the former last particle now sits at i: check it next
        } else {
            ++i;
        }
    }
}

} // namespace cuervo::particles

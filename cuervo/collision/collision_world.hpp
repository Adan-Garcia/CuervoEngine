#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cuervo/collision/shapes.hpp"

// Broadphase + queries (DESIGN.md 4.7): circles and capsules in a uniform
// spatial hash that is rebuilt from scratch every tick (no persistent tree).
//
// Two hashes (DESIGN.md 4.15 "Secondary spatial hash"): small shapes go to
// the fine hash (`cell_size`); shapes spanning more than
// `large_cells_threshold` fine cells (bosses, lasers, screen-wide hitboxes)
// go to a coarse hash with cells `coarse_factor` times bigger, so a large
// shape costs a handful of entries instead of hundreds and small queries
// don't wade through it. Queries consult both; each proxy lives in exactly
// one of them, and results are sorted by proxy id as before.
//
// Per tick:
//   world.clear();
//   world.add_circle(...); world.add_capsule(...); particles.register_colliders(world);
//   world.build();
//   world.pairs(layer_bit(shots), layer_bit(player), hits);
//
// Determinism (DESIGN.md 4.1, 4.14): proxies are numbered in insertion order,
// the hash is built with a stable counting sort (no unordered containers), and
// every query returns results sorted by proxy id, so the same insertions give
// the same results in the same order. Queries are const and keep no scratch
// state, so several threads may query one built world at once.
//
// Jobs (DESIGN.md 4.15): build() and large pairs() queries split their work
// over the job system. Every split writes disjoint ranges and is merged in a
// fixed order, so the hash and all results are identical for any thread count
// (tests/cuervo/collision_tests.cpp checks 1 vs N threads).
namespace cuervo::jobs {
class JobSystem;
}

namespace cuervo::collision {

using ProxyId = std::uint32_t;
using LayerMask = std::uint32_t;

inline constexpr std::uint32_t max_layers = 32;
inline constexpr LayerMask all_layers = 0xffffffffu;

constexpr LayerMask layer_bit(std::uint32_t layer) { return LayerMask{1} << (layer & 31u); }

enum class ShapeKind : std::uint8_t { Circle, Capsule };

// An ordered pair from pairs(): `a` comes from the first layer set, `b` from
// the second.
struct ProxyPair {
    ProxyId a = 0;
    ProxyId b = 0;
    friend constexpr bool operator==(const ProxyPair&, const ProxyPair&) = default;
};

class CollisionWorld {
public:
    // `cell_size` should be around the size of the common small shapes
    // (bullets); bigger shapes simply span several cells.
    explicit CollisionWorld(float cell_size = 32.0f);

    // The job system build() and pairs() use; nullptr runs them inline.
    // Defaults to jobs::default_jobs().
    void set_jobs(jobs::JobSystem* jobs) { jobs_ = jobs; }
    jobs::JobSystem* jobs() const { return jobs_; }

    // Starts a new tick: drops every proxy. Keeps allocations.
    void clear();
    void set_cell_size(float cell_size);
    float cell_size() const { return cell_size_; }

    // `layer` is 0..31; `user` is opaque (an entity handle, a packed particle
    // reference...). Ids are handed out in call order, starting at 0.
    ProxyId add_circle(const Circle& circle, std::uint32_t layer, std::uint64_t user = 0);
    ProxyId add_capsule(const Capsule& capsule, std::uint32_t layer, std::uint64_t user = 0);
    void reserve(std::size_t proxies);

    // Bulk insertion for parallel producers: appends `count` proxies and
    // returns the first id. Every one of them must then be filled exactly once
    // with set_circle/set_capsule (any thread, distinct ids) before build().
    ProxyId add_uninitialized(std::size_t count);
    void set_circle(ProxyId id, const Circle& circle, std::uint32_t layer, std::uint64_t user = 0);
    void set_capsule(ProxyId id, const Capsule& capsule, std::uint32_t layer, std::uint64_t user = 0);

    // Builds the spatial hash. Queries are valid until the next clear()/add.
    void build();
    bool built() const { return built_; }

    std::size_t size() const { return count_; }
    ShapeKind kind(ProxyId id) const { return kind_[id]; }
    std::uint32_t layer(ProxyId id) const { return layer_[id]; }
    std::uint64_t user(ProxyId id) const { return user_[id]; }
    Circle circle(ProxyId id) const { return {{ax_[id], ay_[id]}, radius_[id]}; }
    Capsule capsule(ProxyId id) const { return {{ax_[id], ay_[id]}, {bx_[id], by_[id]}, radius_[id]}; }
    Aabb aabb(ProxyId id) const { return {{min_x_[id], min_y_[id]}, {max_x_[id], max_y_[id]}}; }
    // Number of proxies per layer in the current build.
    std::uint32_t layer_count(std::uint32_t layer) const { return layer_counts_[layer & 31u]; }

    // Exact overlap test between two proxies.
    bool overlaps(ProxyId x, ProxyId y) const;

    // --- queries (call after build); results are appended to `out`, sorted ---

    // Proxies on `mask` layers overlapping the shape.
    void overlap(const Circle& shape, LayerMask mask, std::vector<ProxyId>& out) const;
    void overlap(const Capsule& shape, LayerMask mask, std::vector<ProxyId>& out) const;
    // Proximity: proxies within `distance` of a point (a circle query). Use an
    // inflated shape for "within d of this hitbox" (grazing).
    void within(vec2 point, float distance, LayerMask mask, std::vector<ProxyId>& out) const {
        overlap(Circle{point, distance}, mask, out);
    }
    bool any(const Circle& shape, LayerMask mask) const;

    // Every overlapping pair (a, b) with a on a `mask_a` layer, b on a
    // `mask_b` layer and a != b, sorted by (a, b). When both proxies are on
    // layers of both sets the unordered pair is reported once, as (lower id,
    // higher id). The smaller set drives the hash lookups, so "50k bullets vs
    // one player" costs one query.
    void pairs(LayerMask mask_a, LayerMask mask_b, std::vector<ProxyPair>& out) const;

    // Stats of the last build (for HUDs and tuning).
    std::size_t bucket_count() const { return bucket_start_.empty() ? 0 : bucket_start_.size() - 1; }
    std::size_t entry_count() const { return entries_.size(); }
    // Proxies in the coarse hash, and in the fallback list (see below).
    std::size_t large_count() const { return large_.size(); }
    std::size_t huge_count() const { return huge_.size(); }
    std::size_t coarse_entry_count() const { return large_entries_.size(); }

    // A proxy spanning more fine cells than this goes to the coarse hash.
    static constexpr std::uint32_t large_cells_threshold = 64;
    // Coarse cell size = cell_size * coarse_factor.
    static constexpr std::uint32_t coarse_factor = 8;
    // A proxy spanning more coarse cells than this (wider than ~8000 fine
    // cells: only absurd sizes) is kept in a fallback list every query
    // scans, so no shape can blow up either hash. Normally empty.
    static constexpr std::uint32_t max_cells_per_proxy = 1024;

private:
    struct CellRange {
        std::int32_t x0, y0, x1, y1;
    };

    ProxyId push(ShapeKind kind, vec2 a, vec2 b, float radius, std::uint32_t layer, std::uint64_t user);
    void write(ProxyId id, ShapeKind kind, vec2 a, vec2 b, float radius, std::uint32_t layer, std::uint64_t user);
    void resize_proxies(std::size_t count);
    CellRange cell_range(const Aabb& box) const;
    CellRange coarse_range(const Aabb& box) const;
    static std::uint32_t bucket_of(std::int32_t cx, std::int32_t cy, std::uint32_t mask);
    void build_coarse();
    std::size_t simd_cell_ranges(std::size_t begin, std::size_t end);
    bool overlaps_shape(ProxyId id, const Circle& shape) const;
    bool overlaps_shape(ProxyId id, const Capsule& shape) const;

    // Calls fn(id) once for every proxy on `mask` whose AABB overlaps `box`,
    // in an order that depends only on the build (not sorted).
    template<typename Fn>
    void visit_candidates(const Aabb& box, LayerMask mask, Fn&& fn) const;

    jobs::JobSystem* jobs_;
    float cell_size_;
    float inv_cell_size_;
    float inv_coarse_size_;
    bool built_ = false;

    // Proxies (SoA, indexed by ProxyId; the first count_ entries are live).
    // Circles use only (ax, ay).
    std::size_t count_ = 0;
    std::vector<ShapeKind> kind_;
    std::vector<std::uint8_t> layer_;
    std::vector<std::uint64_t> user_;
    std::vector<float> ax_, ay_, bx_, by_, radius_;
    std::vector<float> min_x_, min_y_, max_x_, max_y_;
    std::vector<CellRange> cells_;
    std::vector<std::uint8_t> grid_; // per proxy: which structure holds it (Grid in the .cpp)

    // Hash: bucket b holds entries_[bucket_start_[b] .. bucket_start_[b + 1]),
    // proxy ids in ascending order (stable counting sort).
    std::vector<std::uint32_t> bucket_start_;
    std::vector<ProxyId> entries_;
    // Build scratch (parallel counting sort, collision_world.cpp build()).
    std::vector<std::uint32_t> entry_bucket_; // bucket of each entry, in proxy/cell order
    std::vector<std::uint32_t> histogram_;    // per chunk x bucket: count, then the chunk's cursor
    struct ChunkStats {
        std::size_t entries = 0;     // hash entries of the chunk's proxies
        std::size_t entry_start = 0; // offset of its first entry in entry_bucket_
        std::size_t not_fine = 0;    // proxies of the chunk in the coarse hash or the fallback list
        std::uint32_t layers[max_layers]{};
    };
    std::vector<ChunkStats> chunks_;
    std::vector<std::size_t> range_base_;     // per bucket range: first slot
    std::uint32_t bucket_mask_ = 0;

    // Coarse hash: member k is proxy large_[k] (ascending ids) with cell range
    // large_cells_[k]; its buckets hold member indices, ascending.
    std::vector<ProxyId> large_;
    std::vector<CellRange> large_cells_;
    std::vector<std::uint32_t> large_bucket_start_;
    std::vector<std::uint32_t> large_entries_;
    std::vector<std::uint32_t> large_cursor_;
    std::uint32_t large_mask_ = 0;
    std::vector<ProxyId> huge_; // fallback list, ascending ids
    std::uint32_t layer_counts_[max_layers]{};
};

} // namespace cuervo::collision

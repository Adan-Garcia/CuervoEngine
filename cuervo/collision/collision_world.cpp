#include "cuervo/collision/collision_world.hpp"

#include <algorithm>
#include <bit>

#include "cuervo/jobs/job_system.hpp"
#include "cuervo/math/simd.hpp"

namespace cuervo::collision {

namespace {

// Cell coordinates are clamped well inside int32 so huge or non-finite
// positions can't overflow the cast (NaN lands on the lower bound). Floor by
// truncation + correction: exact, and avoids a libm floorf call per bound on
// baseline x86-64 (no SSE4.1 roundss), which dominated build().
constexpr float max_cell_coord = 1073741824.0f; // 2^30

std::int32_t to_cell(float v, float inv_cell_size) {
    float c = v * inv_cell_size;
    if (!(c >= -max_cell_coord)) {
        c = -max_cell_coord;
    } else if (c > max_cell_coord) {
        c = max_cell_coord;
    }
    const auto t = static_cast<std::int32_t>(c); // toward zero
    return static_cast<float>(t) > c ? t - 1 : t;
}

// Runs fn(chunk, begin, end) over `chunks` contiguous ranges of [0, count),
// on the job system when there is one.
template<typename Fn>
void for_chunks(jobs::JobSystem* jobs, std::size_t count, std::size_t chunks, Fn&& fn) {
    const std::size_t step = (count + chunks - 1) / std::max<std::size_t>(chunks, 1);
    const auto task = [&](std::size_t c) {
        const std::size_t begin = std::min(count, c * step);
        fn(c, begin, std::min(count, begin + step));
    };
    if (jobs != nullptr) {
        jobs->run(chunks, task);
    } else {
        for (std::size_t c = 0; c < chunks; ++c) task(c);
    }
}

// Where a proxy lives after build() (CollisionWorld::grid_).
enum Grid : std::uint8_t { fine = 0, coarse = 1, huge = 2 };

std::uint64_t cell_count(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
    const std::uint64_t w = static_cast<std::uint64_t>(static_cast<std::int64_t>(x1) - x0 + 1);
    const std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::int64_t>(y1) - y0 + 1);
    return w * h;
}

// Below this many proxies a build or pairs() query isn't worth a batch.
constexpr std::size_t min_parallel_proxies = 4096;

} // namespace

CollisionWorld::CollisionWorld(float cell_size)
    : jobs_(&jobs::default_jobs()), cell_size_(1.0f), inv_cell_size_(1.0f), inv_coarse_size_(1.0f) {
    set_cell_size(cell_size);
}

void CollisionWorld::set_cell_size(float cell_size) {
    cell_size_ = cell_size > 0.0f ? cell_size : 32.0f;
    inv_cell_size_ = 1.0f / cell_size_;
    inv_coarse_size_ = 1.0f / (cell_size_ * static_cast<float>(coarse_factor));
    built_ = false;
}

void CollisionWorld::clear() {
    // Hash arrays keep their size: build() overwrites what it uses.
    resize_proxies(0);
    built_ = false;
}

// The columns only ever grow: clear() just resets count_, so refilling the
// world every tick doesn't re-initialize megabytes of columns (add_uninitialized
// for 50k particle proxies would otherwise zero 13 arrays serially).
void CollisionWorld::resize_proxies(std::size_t count) {
    count_ = count;
    if (kind_.size() >= count) return;
    kind_.resize(count);
    layer_.resize(count);
    user_.resize(count);
    for (auto* v : {&ax_, &ay_, &bx_, &by_, &radius_, &min_x_, &min_y_, &max_x_, &max_y_}) {
        v->resize(count);
    }
}

void CollisionWorld::reserve(std::size_t proxies) {
    kind_.reserve(proxies);
    layer_.reserve(proxies);
    user_.reserve(proxies);
    for (auto* v : {&ax_, &ay_, &bx_, &by_, &radius_, &min_x_, &min_y_, &max_x_, &max_y_}) {
        v->reserve(proxies);
    }
    cells_.reserve(proxies);
    grid_.reserve(proxies);
}

void CollisionWorld::write(ProxyId id, ShapeKind kind, vec2 a, vec2 b, float radius, std::uint32_t layer,
                           std::uint64_t user) {
    const float r = std::max(radius, 0.0f);
    kind_[id] = kind;
    layer_[id] = static_cast<std::uint8_t>(layer & 31u);
    user_[id] = user;
    ax_[id] = a.x();
    ay_[id] = a.y();
    bx_[id] = b.x();
    by_[id] = b.y();
    radius_[id] = r;
    min_x_[id] = std::min(a.x(), b.x()) - r;
    min_y_[id] = std::min(a.y(), b.y()) - r;
    max_x_[id] = std::max(a.x(), b.x()) + r;
    max_y_[id] = std::max(a.y(), b.y()) + r;
}

ProxyId CollisionWorld::push(ShapeKind kind, vec2 a, vec2 b, float radius, std::uint32_t layer, std::uint64_t user) {
    const auto id = static_cast<ProxyId>(count_);
    resize_proxies(count_ + 1);
    write(id, kind, a, b, radius, layer, user);
    built_ = false;
    return id;
}

ProxyId CollisionWorld::add_circle(const Circle& circle, std::uint32_t layer, std::uint64_t user) {
    return push(ShapeKind::Circle, circle.center, circle.center, circle.radius, layer, user);
}

ProxyId CollisionWorld::add_capsule(const Capsule& capsule, std::uint32_t layer, std::uint64_t user) {
    return push(ShapeKind::Capsule, capsule.a, capsule.b, capsule.radius, layer, user);
}

ProxyId CollisionWorld::add_uninitialized(std::size_t count) {
    const auto first = static_cast<ProxyId>(count_);
    resize_proxies(count_ + count);
    built_ = false;
    return first;
}

void CollisionWorld::set_circle(ProxyId id, const Circle& circle, std::uint32_t layer, std::uint64_t user) {
    write(id, ShapeKind::Circle, circle.center, circle.center, circle.radius, layer, user);
}

void CollisionWorld::set_capsule(ProxyId id, const Capsule& capsule, std::uint32_t layer, std::uint64_t user) {
    write(id, ShapeKind::Capsule, capsule.a, capsule.b, capsule.radius, layer, user);
}

CollisionWorld::CellRange CollisionWorld::cell_range(const Aabb& box) const {
    return {to_cell(box.min.x(), inv_cell_size_), to_cell(box.min.y(), inv_cell_size_),
            to_cell(box.max.x(), inv_cell_size_), to_cell(box.max.y(), inv_cell_size_)};
}

CollisionWorld::CellRange CollisionWorld::coarse_range(const Aabb& box) const {
    return {to_cell(box.min.x(), inv_coarse_size_), to_cell(box.min.y(), inv_coarse_size_),
            to_cell(box.max.x(), inv_coarse_size_), to_cell(box.max.y(), inv_coarse_size_)};
}

std::uint32_t CollisionWorld::bucket_of(std::int32_t cx, std::int32_t cy, std::uint32_t mask) {
    // Spatial hash of Teschner et al. 2003, on unsigned (wrapping) integers.
    const std::uint32_t h = (static_cast<std::uint32_t>(cx) * 73856093u) ^ (static_cast<std::uint32_t>(cy) * 19349663u);
    return (h ^ (h >> 16)) & mask;
}

// A counting sort of (bucket, proxy) entries, split over `chunks` contiguous
// proxy ranges (one per thread). Each pass writes disjoint data:
//   1. per chunk: cell ranges, which hash each proxy goes to, entry and
//      layer counts;
//   (serial) chunk entry offsets, layer totals, the coarse members and the
//   fallback list, the coarse hash (few proxies: serial);
//   2. per chunk: each entry's bucket, and the chunk's histogram row;
//   3. per bucket range: totals, then (serial) range offsets;
//   4. per bucket range: bucket starts, and each chunk's first slot in every
//      bucket (chunks in order, so lower proxy ids come first);
//   5. per chunk: scatter entries to their slots.
// The result is exactly the serial stable counting sort for any chunk count:
// every bucket lists its proxies in ascending id order.
void CollisionWorld::build() {
    const std::size_t n = count_;
    jobs::JobSystem* jobs = n >= min_parallel_proxies ? jobs_ : nullptr;
    const std::size_t chunk_count = jobs != nullptr ? std::max(1u, jobs->thread_count()) : 1;
    cells_.resize(n);
    grid_.resize(n);
    chunks_.assign(chunk_count, ChunkStats{});

    // 1. cell ranges and counts
    for_chunks(jobs, n, chunk_count, [&](std::size_t c, std::size_t begin, std::size_t end) {
        ChunkStats& stats = chunks_[c];
        std::size_t i = simd::enabled() ? simd_cell_ranges(begin, end) : begin;
        for (; i < end; ++i) {
            cells_[i] = cell_range({{min_x_[i], min_y_[i]}, {max_x_[i], max_y_[i]}});
        }
        for (i = begin; i < end; ++i) {
            ++stats.layers[layer_[i]];
            const CellRange r = cells_[i];
            const std::uint64_t cells = cell_count(r.x0, r.y0, r.x1, r.y1);
            const bool large = cells > large_cells_threshold;
            grid_[i] = large ? Grid::coarse : Grid::fine; // coarse vs huge: decided serially below
            stats.not_fine += large ? 1 : 0;
            stats.entries += large ? 0 : static_cast<std::size_t>(cells);
        }
    });
    std::fill(std::begin(layer_counts_), std::end(layer_counts_), 0u);
    large_.clear();
    huge_.clear();
    std::size_t total_entries = 0;
    const std::size_t step = (n + chunk_count - 1) / chunk_count;
    for (std::size_t c = 0; c < chunk_count; ++c) {
        ChunkStats& stats = chunks_[c];
        stats.entry_start = total_entries;
        total_entries += stats.entries;
        for (std::uint32_t l = 0; l < max_layers; ++l) layer_counts_[l] += stats.layers[l];
        if (stats.not_fine > 0) {
            for (std::size_t i = c * step; i < std::min(n, (c + 1) * step); ++i) {
                if (grid_[i] == Grid::fine) continue;
                const CellRange r = coarse_range(aabb(static_cast<ProxyId>(i)));
                if (cell_count(r.x0, r.y0, r.x1, r.y1) > max_cells_per_proxy) {
                    grid_[i] = Grid::huge;
                    huge_.push_back(static_cast<ProxyId>(i));
                } else {
                    large_.push_back(static_cast<ProxyId>(i));
                }
            }
        }
    }
    build_coarse();

    // About one bucket per entry: few enough that the prefix sum stays cheap,
    // enough that unrelated cells rarely share a bucket.
    const std::size_t buckets = std::bit_ceil(std::max<std::size_t>(total_entries, 16));
    bucket_mask_ = static_cast<std::uint32_t>(buckets - 1);
    bucket_start_.resize(buckets + 1);
    entry_bucket_.resize(total_entries);
    entries_.resize(total_entries);
    // Rows are padded off a power of two: rows exactly 2^k apart map to the
    // same cache sets, and pass 4 walks all rows at once.
    const std::size_t row = buckets + 16;
    histogram_.resize(chunk_count * row);

    // 2. buckets of every entry + per-chunk histograms
    for_chunks(jobs, n, chunk_count, [&](std::size_t c, std::size_t begin, std::size_t end) {
        std::uint32_t* hist = histogram_.data() + c * row;
        std::fill(hist, hist + buckets, 0u);
        std::size_t e = chunks_[c].entry_start;
        for (std::size_t i = begin; i < end; ++i) {
            if (grid_[i] != Grid::fine) continue;
            const CellRange& r = cells_[i];
            for (std::int32_t cy = r.y0; cy <= r.y1; ++cy) {
                for (std::int32_t cx = r.x0; cx <= r.x1; ++cx) {
                    const std::uint32_t b = bucket_of(cx, cy, bucket_mask_);
                    entry_bucket_[e++] = b;
                    ++hist[b];
                }
            }
        }
    });

    // 3. + 4. bucket starts and per-chunk cursors, by bucket ranges
    std::vector<std::size_t>& range_base = range_base_;
    range_base.assign(chunk_count + 1, 0);
    for_chunks(jobs, buckets, chunk_count, [&](std::size_t r, std::size_t begin, std::size_t end) {
        std::size_t sum = 0;
        for (std::size_t c = 0; c < chunk_count; ++c) {
            const std::uint32_t* hist = histogram_.data() + c * row;
            for (std::size_t b = begin; b < end; ++b) sum += hist[b];
        }
        range_base[r + 1] = sum;
    });
    for (std::size_t r = 0; r < chunk_count; ++r) range_base[r + 1] += range_base[r];
    for_chunks(jobs, buckets, chunk_count, [&](std::size_t r, std::size_t begin, std::size_t end) {
        auto running = static_cast<std::uint32_t>(range_base[r]);
        for (std::size_t b = begin; b < end; ++b) {
            bucket_start_[b] = running;
            for (std::size_t c = 0; c < chunk_count; ++c) {
                std::uint32_t& slot = histogram_[c * row + b];
                const std::uint32_t count = slot;
                slot = running;
                running += count;
            }
        }
    });
    bucket_start_[buckets] = static_cast<std::uint32_t>(total_entries);

    // 5. scatter: proxy order within each chunk, chunks own disjoint slots
    for_chunks(jobs, n, chunk_count, [&](std::size_t c, std::size_t begin, std::size_t end) {
        std::uint32_t* cursor = histogram_.data() + c * row;
        std::size_t e = chunks_[c].entry_start;
        for (std::size_t i = begin; i < end; ++i) {
            if (grid_[i] != Grid::fine) continue;
            const CellRange& r = cells_[i];
            const auto count = static_cast<std::size_t>(r.x1 - r.x0 + 1) * static_cast<std::size_t>(r.y1 - r.y0 + 1);
            for (std::size_t k = 0; k < count; ++k) {
                entries_[cursor[entry_bucket_[e++]]++] = static_cast<ProxyId>(i);
            }
        }
    });
    built_ = true;
}

// SIMD to_cell over the four bound columns (DESIGN.md 4.15 "hash cell
// computation"): the same clamp, truncation and correction as to_cell(), so
// the cell ranges are bit-identical. Returns the first index not processed.
std::size_t CollisionWorld::simd_cell_ranges(std::size_t begin, std::size_t end) {
#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2) || defined(CUERVO_SIMD_WASM)
    using simd::F;
    constexpr std::size_t w = simd::width;
    const F inv = simd::splat(inv_cell_size_);
    const F hi = simd::splat(max_cell_coord);
    const F lo = simd::splat(-max_cell_coord);
    const auto to_cells = [&](F v) {
        F c = v * inv;
        c = simd::select(!(c >= lo), lo, simd::select(c > hi, hi, c));
        const simd::I t = simd::truncate(c);
        return t + simd::as_int(simd::to_float(t) > c); // true lanes are -1
    };
    std::size_t i = begin;
    for (; i + w <= end; i += w) {
        alignas(32) std::int32_t x0[w], y0[w], x1[w], y1[w];
        simd::store_i(x0, to_cells(simd::load(&min_x_[i])));
        simd::store_i(y0, to_cells(simd::load(&min_y_[i])));
        simd::store_i(x1, to_cells(simd::load(&max_x_[i])));
        simd::store_i(y1, to_cells(simd::load(&max_y_[i])));
        for (std::size_t k = 0; k < w; ++k) cells_[i + k] = {x0[k], y0[k], x1[k], y1[k]};
    }
    return i;
#else
    (void)end;
    return begin;
#endif
}

// The coarse hash: a plain serial counting sort of the (few) large proxies.
void CollisionWorld::build_coarse() {
    const std::size_t m = large_.size();
    large_cells_.resize(m);
    std::size_t total = 0;
    for (std::size_t k = 0; k < m; ++k) {
        const CellRange r = coarse_range(aabb(large_[k]));
        large_cells_[k] = r;
        total += static_cast<std::size_t>(cell_count(r.x0, r.y0, r.x1, r.y1));
    }
    const std::size_t buckets = std::bit_ceil(std::max<std::size_t>(total, 16));
    large_mask_ = static_cast<std::uint32_t>(buckets - 1);
    large_bucket_start_.assign(buckets + 1, 0);
    for (const CellRange& r : large_cells_) {
        for (std::int32_t cy = r.y0; cy <= r.y1; ++cy) {
            for (std::int32_t cx = r.x0; cx <= r.x1; ++cx) ++large_bucket_start_[bucket_of(cx, cy, large_mask_) + 1];
        }
    }
    for (std::size_t b = 0; b < buckets; ++b) large_bucket_start_[b + 1] += large_bucket_start_[b];
    large_cursor_.assign(large_bucket_start_.begin(), large_bucket_start_.end() - 1);
    large_entries_.resize(total);
    for (std::size_t k = 0; k < m; ++k) {
        const CellRange& r = large_cells_[k];
        for (std::int32_t cy = r.y0; cy <= r.y1; ++cy) {
            for (std::int32_t cx = r.x0; cx <= r.x1; ++cx) {
                large_entries_[large_cursor_[bucket_of(cx, cy, large_mask_)]++] = static_cast<std::uint32_t>(k);
            }
        }
    }
}

template<typename Fn>
void CollisionWorld::visit_candidates(const Aabb& box, LayerMask mask, Fn&& fn) const {
    if (!built_) return;
    const CellRange q = cell_range(box);
    const auto box_overlaps = [&](ProxyId id) {
        return min_x_[id] <= box.max.x() && box.min.x() <= max_x_[id] && min_y_[id] <= box.max.y() &&
               box.min.y() <= max_y_[id];
    };
    if (cell_count(q.x0, q.y0, q.x1, q.y1) > std::max<std::uint64_t>(count_, max_cells_per_proxy)) {
        // A query bigger than the world: a linear scan is cheaper.
        for (ProxyId id = 0; id < count_; ++id) {
            if ((layer_bit(layer_[id]) & mask) && box_overlaps(id)) fn(id);
        }
        return;
    }
    for (std::int32_t cy = q.y0; cy <= q.y1; ++cy) {
        for (std::int32_t cx = q.x0; cx <= q.x1; ++cx) {
            const std::uint32_t b = bucket_of(cx, cy, bucket_mask_);
            ProxyId previous = 0xffffffffu;
            for (std::uint32_t e = bucket_start_[b]; e < bucket_start_[b + 1]; ++e) {
                const ProxyId id = entries_[e];
                if (id == previous) continue; // same proxy via another cell in this bucket
                previous = id;
                if (!(layer_bit(layer_[id]) & mask)) continue;
                // A proxy shares several cells with the query; report it only
                // from the first shared cell (the minimum corner of the
                // intersection of both cell ranges) so it comes up once.
                const CellRange& r = cells_[id];
                if (cx != std::max(r.x0, q.x0) || cy != std::max(r.y0, q.y0)) continue;
                if (box_overlaps(id)) fn(id);
            }
        }
    }
    // The coarse hash, same scheme with member indices (DESIGN.md 4.15).
    if (!large_.empty()) {
        const CellRange cq = coarse_range(box);
        if (cell_count(cq.x0, cq.y0, cq.x1, cq.y1) > std::max<std::uint64_t>(large_.size(), max_cells_per_proxy)) {
            for (ProxyId id : large_) {
                if ((layer_bit(layer_[id]) & mask) && box_overlaps(id)) fn(id);
            }
        } else {
            for (std::int32_t cy = cq.y0; cy <= cq.y1; ++cy) {
                for (std::int32_t cx = cq.x0; cx <= cq.x1; ++cx) {
                    const std::uint32_t b = bucket_of(cx, cy, large_mask_);
                    std::uint32_t previous = 0xffffffffu;
                    for (std::uint32_t e = large_bucket_start_[b]; e < large_bucket_start_[b + 1]; ++e) {
                        const std::uint32_t k = large_entries_[e];
                        if (k == previous) continue;
                        previous = k;
                        const ProxyId id = large_[k];
                        if (!(layer_bit(layer_[id]) & mask)) continue;
                        const CellRange& r = large_cells_[k];
                        if (cx != std::max(r.x0, cq.x0) || cy != std::max(r.y0, cq.y0)) continue;
                        if (box_overlaps(id)) fn(id);
                    }
                }
            }
        }
    }
    for (ProxyId id : huge_) {
        if ((layer_bit(layer_[id]) & mask) && box_overlaps(id)) fn(id);
    }
}

bool CollisionWorld::overlaps_shape(ProxyId id, const Circle& shape) const {
    return kind_[id] == ShapeKind::Circle ? collision::overlaps(circle(id), shape)
                                          : collision::overlaps(capsule(id), shape);
}

bool CollisionWorld::overlaps_shape(ProxyId id, const Capsule& shape) const {
    return kind_[id] == ShapeKind::Circle ? collision::overlaps(circle(id), shape)
                                          : collision::overlaps(capsule(id), shape);
}

bool CollisionWorld::overlaps(ProxyId x, ProxyId y) const {
    return kind_[y] == ShapeKind::Circle ? overlaps_shape(x, circle(y)) : overlaps_shape(x, capsule(y));
}

void CollisionWorld::overlap(const Circle& shape, LayerMask mask, std::vector<ProxyId>& out) const {
    const std::size_t first = out.size();
    visit_candidates(bounds(shape), mask, [&](ProxyId id) {
        if (overlaps_shape(id, shape)) out.push_back(id);
    });
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
}

void CollisionWorld::overlap(const Capsule& shape, LayerMask mask, std::vector<ProxyId>& out) const {
    const std::size_t first = out.size();
    visit_candidates(bounds(shape), mask, [&](ProxyId id) {
        if (overlaps_shape(id, shape)) out.push_back(id);
    });
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
}

bool CollisionWorld::any(const Circle& shape, LayerMask mask) const {
    bool found = false;
    visit_candidates(bounds(shape), mask, [&](ProxyId id) {
        if (!found && overlaps_shape(id, shape)) found = true;
    });
    return found;
}

void CollisionWorld::pairs(LayerMask mask_a, LayerMask mask_b, std::vector<ProxyPair>& out) const {
    if (!built_ || mask_a == 0 || mask_b == 0) return;
    std::size_t count_a = 0;
    std::size_t count_b = 0;
    for (std::uint32_t l = 0; l < max_layers; ++l) {
        if (mask_a & layer_bit(l)) count_a += layer_counts_[l];
        if (mask_b & layer_bit(l)) count_b += layer_counts_[l];
    }
    if (count_a == 0 || count_b == 0) return;

    // Drive the lookups from the smaller set; the sort below makes the result
    // independent of that choice.
    const bool drive_b = count_b <= count_a;
    const LayerMask driver_mask = drive_b ? mask_b : mask_a;
    const LayerMask other_mask = drive_b ? mask_a : mask_b;
    const auto query = [&](ProxyId q, std::vector<ProxyPair>& found) {
        if (!(layer_bit(layer_[q]) & driver_mask)) return;
        const bool q_in_both = (layer_bit(layer_[q]) & other_mask) != 0;
        visit_candidates(aabb(q), other_mask, [&](ProxyId p) {
            if (p == q) return;
            const bool p_in_both = (layer_bit(layer_[p]) & driver_mask) != 0;
            if (q_in_both && p_in_both) {
                // Found from both sides; keep the visit where q is lower.
                if (q > p) return;
                if (overlaps(q, p)) found.push_back({q, p});
                return;
            }
            if (overlaps(q, p)) found.push_back(drive_b ? ProxyPair{p, q} : ProxyPair{q, p});
        });
    };

    const std::size_t first = out.size();
    const std::size_t n = count_;
    jobs::JobSystem* jobs = std::min(count_a, count_b) >= min_parallel_proxies ? jobs_ : nullptr;
    if (jobs != nullptr && jobs->thread_count() > 1) {
        // Query batches (DESIGN.md 4.15): each range of driver proxies
        // collects into its own vector; they are appended in range order and
        // the sort below gives the same result as one serial pass.
        const std::size_t chunks = jobs->chunk_count(n, 1024);
        std::vector<std::vector<ProxyPair>> found(chunks);
        for_chunks(jobs, n, chunks, [&](std::size_t c, std::size_t begin, std::size_t end) {
            for (std::size_t q = begin; q < end; ++q) query(static_cast<ProxyId>(q), found[c]);
        });
        for (const auto& part : found) out.insert(out.end(), part.begin(), part.end());
    } else {
        for (ProxyId q = 0; q < n; ++q) query(q, out);
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
              [](const ProxyPair& x, const ProxyPair& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
}

} // namespace cuervo::collision

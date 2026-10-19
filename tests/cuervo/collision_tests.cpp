#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "cuervo/collision/collision_world.hpp"
#include "cuervo/core/rng.hpp"
#include "cuervo/jobs/job_system.hpp"

using namespace cuervo;
using namespace cuervo::collision;

TEST_CASE("Collision shape overlap tests") {
    CHECK(overlaps(Circle{{0, 0}, 1}, Circle{{1.5f, 0}, 0.5f})); // touching counts
    CHECK_FALSE(overlaps(Circle{{0, 0}, 1}, Circle{{1.6f, 0}, 0.5f}));

    const Capsule laser{{0, 0}, {10, 0}, 1};
    CHECK(overlaps(Circle{{5, 1.9f}, 1}, laser));
    CHECK_FALSE(overlaps(Circle{{5, 2.1f}, 1}, laser));
    CHECK(overlaps(Circle{{11.5f, 0}, 1}, laser)); // round cap
    CHECK_FALSE(overlaps(Circle{{12.5f, 0}, 1}, laser));
    CHECK(overlaps(laser, Circle{{-1.5f, 0.5f}, 1}));

    // Crossing, parallel and degenerate capsules.
    CHECK(overlaps(laser, Capsule{{5, -5}, {5, 5}, 0.1f}));
    CHECK(overlaps(laser, Capsule{{0, 1.9f}, {10, 1.9f}, 1}));
    CHECK_FALSE(overlaps(laser, Capsule{{0, 2.1f}, {10, 2.1f}, 1}));
    CHECK(overlaps(Capsule{{3, 3}, {3, 3}, 1}, Capsule{{3, 4.5f}, {3, 4.5f}, 1}));
    CHECK_FALSE(overlaps(laser, Capsule{{12.5f, 0}, {20, 0}, 0.4f}));
}

TEST_CASE("CollisionWorld assigns ids in order and answers simple queries") {
    CollisionWorld world(16.0f);
    const ProxyId a = world.add_circle({{0, 0}, 4}, 0, 100);
    const ProxyId b = world.add_circle({{6, 0}, 4}, 1, 200);
    const ProxyId c = world.add_capsule({{-50, 20}, {50, 20}, 2}, 2, 300);
    CHECK(a == 0);
    CHECK(b == 1);
    CHECK(c == 2);

    std::vector<ProxyId> out;
    world.overlap(Circle{{0, 0}, 1}, all_layers, out);
    CHECK(out.empty()); // not built yet
    world.build();

    world.overlap(Circle{{3, 0}, 1}, all_layers, out);
    CHECK(out == std::vector<ProxyId>{0, 1});
    out.clear();
    world.overlap(Circle{{3, 0}, 1}, layer_bit(1), out);
    CHECK(out == std::vector<ProxyId>{1});
    out.clear();
    world.within({0, 17}, 1.5f, all_layers, out);
    CHECK(out == std::vector<ProxyId>{2});
    CHECK(world.user(2) == 300);
    CHECK(world.any(Circle{{40, 20}, 0.5f}, layer_bit(2)));
    CHECK_FALSE(world.any(Circle{{40, 20}, 0.5f}, layer_bit(0)));

    std::vector<ProxyPair> pairs;
    world.pairs(layer_bit(0), layer_bit(1), pairs);
    CHECK(pairs == std::vector<ProxyPair>{{0, 1}});
    pairs.clear();
    world.pairs(layer_bit(1), layer_bit(0), pairs);
    CHECK(pairs == std::vector<ProxyPair>{{1, 0}});
}

TEST_CASE("CollisionWorld survives non-finite and huge coordinates") {
    CollisionWorld world(8.0f);
    world.add_circle({{std::numeric_limits<float>::quiet_NaN(), 0}, 1}, 0);
    world.add_circle({{1e30f, -1e30f}, 1}, 0);
    world.add_circle({{0, 0}, 1}, 0);
    world.build();
    std::vector<ProxyId> out;
    world.overlap(Circle{{0, 0}, 2}, all_layers, out);
    CHECK(out == std::vector<ProxyId>{2});
}

namespace {

struct Shape {
    bool capsule = false;
    Circle circle;
    Capsule caps;
    std::uint32_t layer = 0;
};

Shape random_shape(Pcg32& rng, float extent) {
    Shape s;
    s.layer = rng.next_below(4);
    // Mostly small shapes, some large ones that span many cells (the coarse
    // hash), a few huge ones (the coarse hash, or the fallback list at small
    // cell sizes).
    const std::uint32_t kind = rng.next_below(100);
    float radius = rng.range(0.5f, 6.0f);
    if (kind >= 95) radius = rng.range(20.0f, 80.0f);
    if (kind == 99) radius = rng.range(400.0f, 900.0f);
    const vec2 p{rng.range(-extent, extent), rng.range(-extent, extent)};
    if (rng.next_below(3) == 0) {
        s.capsule = true;
        const vec2 d{rng.range(-40.0f, 40.0f), rng.range(-40.0f, 40.0f)};
        s.caps = {p, p + d, radius * 0.5f};
    } else {
        s.circle = {p, radius};
    }
    return s;
}

bool shape_overlaps(const Shape& x, const Shape& y) {
    if (x.capsule) return y.capsule ? overlaps(x.caps, y.caps) : overlaps(x.caps, y.circle);
    return y.capsule ? overlaps(x.circle, y.caps) : overlaps(x.circle, y.circle);
}

} // namespace

TEST_CASE("Spatial hash matches brute force on seeded random scenes") {
    for (const float cell : {4.0f, 16.0f, 64.0f}) {
        for (std::uint64_t seed : {1ull, 2ull, 77ull}) {
            Pcg32 rng(seed);
            std::vector<Shape> shapes;
            for (int i = 0; i < 1500; ++i) shapes.push_back(random_shape(rng, 300.0f));

            CollisionWorld world(cell);
            for (const Shape& s : shapes) {
                if (s.capsule) {
                    world.add_capsule(s.caps, s.layer);
                } else {
                    world.add_circle(s.circle, s.layer);
                }
            }
            world.build();
            CHECK(world.size() == shapes.size());
            CHECK(world.large_count() > 0); // both hashes are exercised
            if (cell == 4.0f) CHECK(world.huge_count() > 0);

            // Overlap queries with random shapes and layer masks.
            for (int q = 0; q < 200; ++q) {
                const Shape query = random_shape(rng, 350.0f);
                const LayerMask mask = rng.next_below(16) | (q % 3 == 0 ? all_layers : 0u);
                std::vector<ProxyId> expected;
                for (ProxyId id = 0; id < shapes.size(); ++id) {
                    if ((layer_bit(shapes[id].layer) & mask) && shape_overlaps(shapes[id], query)) {
                        expected.push_back(id);
                    }
                }
                std::vector<ProxyId> got;
                if (query.capsule) {
                    world.overlap(query.caps, mask, got);
                } else {
                    world.overlap(query.circle, mask, got);
                }
                REQUIRE(got == expected);
            }

            // Pairs between layer sets, disjoint and overlapping.
            const std::pair<LayerMask, LayerMask> mask_sets[] = {
                {layer_bit(0), layer_bit(1)},
                {layer_bit(1) | layer_bit(2), layer_bit(3)},
                {layer_bit(0) | layer_bit(1), layer_bit(1) | layer_bit(2)},
                {layer_bit(2), layer_bit(2)},
                {all_layers, all_layers},
            };
            for (const auto& [ma, mb] : mask_sets) {
                std::set<std::pair<ProxyId, ProxyId>> expected_set;
                for (ProxyId a = 0; a < shapes.size(); ++a) {
                    for (ProxyId b = 0; b < shapes.size(); ++b) {
                        const LayerMask la = layer_bit(shapes[a].layer);
                        const LayerMask lb = layer_bit(shapes[b].layer);
                        if (a == b || !(la & ma) || !(lb & mb) || !shape_overlaps(shapes[a], shapes[b])) continue;
                        if ((la & mb) && (lb & ma)) {
                            expected_set.insert({std::min(a, b), std::max(a, b)});
                        } else {
                            expected_set.insert({a, b});
                        }
                    }
                }
                std::vector<ProxyPair> expected;
                for (const auto& [a, b] : expected_set) expected.push_back({a, b});
                std::vector<ProxyPair> got;
                world.pairs(ma, mb, got);
                REQUIRE(got.size() == expected.size());
                CHECK(got == expected);
            }
        }
    }
}

TEST_CASE("CollisionWorld results do not depend on rebuilds") {
    Pcg32 rng(9);
    CollisionWorld world(10.0f);
    std::vector<Shape> shapes;
    for (int i = 0; i < 500; ++i) shapes.push_back(random_shape(rng, 100.0f));
    std::vector<ProxyPair> first;
    for (int round = 0; round < 3; ++round) {
        world.clear();
        for (const Shape& s : shapes) {
            if (s.capsule) {
                world.add_capsule(s.caps, s.layer);
            } else {
                world.add_circle(s.circle, s.layer);
            }
        }
        world.build();
        std::vector<ProxyPair> got;
        world.pairs(layer_bit(0) | layer_bit(1), layer_bit(2) | layer_bit(3), got);
        if (round == 0) {
            first = got;
            CHECK_FALSE(first.empty());
        } else {
            CHECK(got == first);
        }
    }
}

TEST_CASE("CollisionWorld: parallel build and pairs match the single-threaded ones (DESIGN.md 4.15)") {
    Pcg32 rng(31);
    std::vector<Shape> shapes;
    for (int i = 0; i < 20000; ++i) shapes.push_back(random_shape(rng, 1200.0f));
    const auto fill = [&](CollisionWorld& world) {
        world.clear();
        for (const Shape& s : shapes) {
            if (s.capsule) {
                world.add_capsule(s.caps, s.layer);
            } else {
                world.add_circle(s.circle, s.layer);
            }
        }
        world.build();
    };
    jobs::JobSystem one(1);
    CollisionWorld reference(16.0f);
    reference.set_jobs(&one);
    fill(reference);
    std::vector<ProxyPair> expected;
    reference.pairs(layer_bit(0) | layer_bit(1), layer_bit(1) | layer_bit(2) | layer_bit(3), expected);
    CHECK(expected.size() > 1000);

    for (unsigned threads : {2u, 5u, 8u}) {
        jobs::JobSystem js(threads);
        CollisionWorld world(16.0f);
        world.set_jobs(&js);
        fill(world);
        CHECK(world.entry_count() == reference.entry_count());
        CHECK(world.bucket_count() == reference.bucket_count());
        CHECK(world.large_count() == reference.large_count());
        CHECK(world.coarse_entry_count() == reference.coarse_entry_count());
        for (std::uint32_t l = 0; l < 4; ++l) CHECK(world.layer_count(l) == reference.layer_count(l));
        std::vector<ProxyPair> got;
        world.pairs(layer_bit(0) | layer_bit(1), layer_bit(1) | layer_bit(2) | layer_bit(3), got);
        CHECK(got == expected);
        for (int q = 0; q < 100; ++q) {
            const Shape query = random_shape(rng, 1200.0f);
            std::vector<ProxyId> a;
            std::vector<ProxyId> b;
            reference.overlap(query.circle, all_layers, a);
            world.overlap(query.circle, all_layers, b);
            REQUIRE(a == b);
        }
        CHECK(js.parallel_batches() > 0);
    }
}

namespace {

// Brute-force reference for one world: every query and pair checked against
// all shapes (DESIGN.md 4.15: the secondary hash must not change results).
void check_against_brute_force(const std::vector<Shape>& shapes, float cell, Pcg32& rng, int queries) {
    CollisionWorld world(cell);
    for (const Shape& s : shapes) {
        if (s.capsule) {
            world.add_capsule(s.caps, s.layer);
        } else {
            world.add_circle(s.circle, s.layer);
        }
    }
    world.build();
    for (int q = 0; q < queries; ++q) {
        // Small, medium and screen-sized queries.
        Shape query = random_shape(rng, 700.0f);
        if (q % 7 == 0) query.circle.radius *= 40.0f;
        const LayerMask mask = q % 2 == 0 ? all_layers : rng.next_below(16);
        std::vector<ProxyId> expected;
        for (ProxyId id = 0; id < shapes.size(); ++id) {
            if ((layer_bit(shapes[id].layer) & mask) && shape_overlaps(shapes[id], query)) expected.push_back(id);
        }
        std::vector<ProxyId> got;
        if (query.capsule) {
            world.overlap(query.caps, mask, got);
        } else {
            world.overlap(query.circle, mask, got);
        }
        REQUIRE(got == expected);
        if (!query.capsule) CHECK(world.any(query.circle, mask) == !expected.empty());
    }
    for (const auto& [ma, mb] : {std::pair{layer_bit(0), layer_bit(1) | layer_bit(2)},
                                 std::pair{layer_bit(1) | layer_bit(2), layer_bit(1) | layer_bit(3)},
                                 std::pair{all_layers, all_layers}}) {
        std::set<std::pair<ProxyId, ProxyId>> expected_set;
        for (ProxyId a = 0; a < shapes.size(); ++a) {
            for (ProxyId b = 0; b < shapes.size(); ++b) {
                const LayerMask la = layer_bit(shapes[a].layer);
                const LayerMask lb = layer_bit(shapes[b].layer);
                if (a == b || !(la & ma) || !(lb & mb) || !shape_overlaps(shapes[a], shapes[b])) continue;
                expected_set.insert((la & mb) && (lb & ma) ? std::pair{std::min(a, b), std::max(a, b)} : std::pair{a, b});
            }
        }
        std::vector<ProxyPair> expected;
        for (const auto& [a, b] : expected_set) expected.push_back({a, b});
        std::vector<ProxyPair> got;
        world.pairs(ma, mb, got);
        REQUIRE(got.size() == expected.size());
        CHECK(got == expected);
    }
}

} // namespace

TEST_CASE("Secondary spatial hash: large shapes match brute force (DESIGN.md 4.15)") {
    // A bullet-hell frame: many bullets, plus bosses, lasers sweeping the
    // screen and screen-wide hitboxes, all mixed in id order.
    for (const float cell : {8.0f, 32.0f}) {
        Pcg32 rng(2026);
        std::vector<Shape> shapes;
        for (int i = 0; i < 1200; ++i) {
            Shape s;
            s.layer = rng.next_below(4);
            const vec2 p{rng.range(0.0f, 1280.0f), rng.range(0.0f, 720.0f)};
            switch (rng.next_below(40)) {
                case 0: // boss body
                    s.circle = {p, rng.range(60.0f, 140.0f)};
                    break;
                case 1: { // laser: a long thin capsule across the screen
                    s.capsule = true;
                    const vec2 d{rng.range(-1400.0f, 1400.0f), rng.range(-800.0f, 800.0f)};
                    s.caps = {p, p + d, rng.range(3.0f, 12.0f)};
                    break;
                }
                case 2: // screen-wide hitbox
                    s.capsule = true;
                    s.caps = {{0.0f, p.y()}, {1280.0f, p.y()}, rng.range(40.0f, 360.0f)};
                    break;
                default: // bullet
                    s.circle = {p, rng.range(2.0f, 6.0f)};
                    break;
            }
            shapes.push_back(s);
        }
        CollisionWorld probe(cell);
        for (const Shape& s : shapes) s.capsule ? probe.add_capsule(s.caps, s.layer) : probe.add_circle(s.circle, s.layer);
        probe.build();
        CHECK(probe.large_count() > 20);
        CHECK(probe.huge_count() == 0); // nothing is absurdly big
        // Large shapes cost few coarse entries instead of hundreds of fine ones.
        // Large shapes cost far fewer coarse entries than they would fine ones.
        std::uint64_t fine_cost = 0;
        for (ProxyId id = 0; id < probe.size(); ++id) {
            const Aabb box = probe.aabb(id);
            const auto cells = static_cast<std::uint64_t>((std::floor(box.max.x() / cell) - std::floor(box.min.x() / cell) + 1) *
                                                          (std::floor(box.max.y() / cell) - std::floor(box.min.y() / cell) + 1));
            if (cells > CollisionWorld::large_cells_threshold) fine_cost += cells;
        }
        CHECK(probe.coarse_entry_count() * 16 < fine_cost);
        check_against_brute_force(shapes, cell, rng, 300);
    }
}

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "cuervo/jobs/job_system.hpp"
#include "cuervo/math/dmath.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/particles/particle_render.hpp"
#include "cuervo/particles/particle_system.hpp"

using namespace cuervo;
using namespace cuervo::particles;

namespace {

constexpr float dt = 1.0f / 60.0f;

float speed_of(const ParticlePool& p, std::size_t i) { return std::sqrt(p.vx[i] * p.vx[i] + p.vy[i] * p.vy[i]); }

// Heading angle in [0, tau).
float heading_of(const ParticlePool& p, std::size_t i) {
    float a = std::atan2(p.vy[i], p.vx[i]);
    return a < 0.0f ? a + dmath::tau : a;
}

Emitter single(vec2 velocity, std::vector<Module> modules, float life = 0.0f) {
    EmitterConfig config;
    config.modules = std::move(modules);
    Emitter e(config);
    e.spawn({.position = {0, 0}, .velocity = velocity, .life = life});
    return e;
}

} // namespace

TEST_CASE("ParticlePool handles survive swap-removal and go stale on death") {
    ParticlePool pool;
    std::vector<ParticleHandle> h;
    for (int i = 0; i < 5; ++i) h.push_back(pool.spawn({.position = {static_cast<float>(i), 0}}));
    CHECK(pool.size_live() == 5);

    CHECK(pool.kill(h[1]));
    pool.kill(3);
    pool.remove_dead();
    CHECK(pool.size_live() == 3);
    CHECK_FALSE(pool.alive(h[1]));
    CHECK_FALSE(pool.alive(h[3]));
    // Survivors moved but their handles still find them.
    for (int i : {0, 2, 4}) {
        const auto index = pool.index_of(h[static_cast<std::size_t>(i)]);
        REQUIRE(index.has_value());
        CHECK(pool.x[*index] == static_cast<float>(i));
    }
    // Swap-removal order: index 1 <- 4, then index 3 (now last) is popped.
    CHECK(pool.x == std::vector<float>{0, 4, 2});

    // Slots are reused LIFO with a new generation: old handles stay stale.
    const ParticleHandle again = pool.spawn({});
    CHECK(again.slot == h[3].slot);
    CHECK(again.generation != h[3].generation);
    CHECK_FALSE(pool.alive(h[3]));
    CHECK(pool.alive(again));
    CHECK_FALSE(pool.kill(h[1]));

    pool.max_particles = 4;
    CHECK(pool.spawn({}).valid() == false);
}

TEST_CASE("Particle integration, lifetime and death events") {
    Emitter e = single({60, -30}, {}, 0.5f);
    e.pool().spin[0] = 2.0f;
    for (int i = 0; i < 10; ++i) e.simulate(dt);
    REQUIRE(e.size() == 1);
    CHECK(e.pool().x[0] == doctest::Approx(10.0f));
    CHECK(e.pool().y[0] == doctest::Approx(-5.0f));
    CHECK(e.pool().rotation[0] == doctest::Approx(2.0f * 10.0f * dt));
    CHECK(e.pool().prev_x[0] == doctest::Approx(9.0f));

    for (int i = 0; i < 25 && e.size() > 0; ++i) e.simulate(dt);
    CHECK(e.size() == 0);
    REQUIRE(e.events().size() == 1);
    CHECK(e.events()[0].type == EventType::Death);
}

TEST_CASE("Velocity modules") {
    SUBCASE("Accelerate") {
        Emitter e = single({0, 0}, {Accelerate{{0, 600}}});
        for (int i = 0; i < 60; ++i) e.simulate(dt);
        CHECK(e.pool().vy[0] == doctest::Approx(600.0f));
    }
    SUBCASE("Drag") {
        Emitter e = single({100, 0}, {Drag{6.0f}});
        e.simulate(dt);
        CHECK(e.pool().vx[0] == doctest::Approx(90.0f));
    }
    SUBCASE("SpeedRamp clamps") {
        Emitter e = single({30, 40}, {SpeedRamp{600.0f, 0.0f, 100.0f}});
        e.simulate(dt);
        CHECK(speed_of(e.pool(), 0) == doctest::Approx(60.0f));
        for (int i = 0; i < 30; ++i) e.simulate(dt);
        CHECK(speed_of(e.pool(), 0) == doctest::Approx(100.0f));
        CHECK(heading_of(e.pool(), 0) == doctest::Approx(std::atan2(40.0f, 30.0f)));
    }
    SUBCASE("Turn rotates at a constant rate and keeps the speed") {
        Emitter e = single({100, 0}, {Turn{dmath::half_pi}});
        for (int i = 0; i < 60; ++i) e.simulate(dt);
        CHECK(speed_of(e.pool(), 0) == doctest::Approx(100.0f).epsilon(1e-4));
        CHECK(heading_of(e.pool(), 0) == doctest::Approx(dmath::half_pi).epsilon(1e-4)); // clockwise = +y
    }
    SUBCASE("Homing turns towards the target, bounded by turn_rate") {
        Emitter e = single({100, 0}, {Homing{1.0f, 0.0f, 0.0f}});
        e.target = {0, 1000}; // straight down from the particle
        e.simulate(dt);
        CHECK(heading_of(e.pool(), 0) == doctest::Approx(dt).epsilon(1e-4));
        for (int i = 0; i < 600; ++i) e.simulate(dt);
        const vec2 to = e.target - vec2{e.pool().x[0], e.pool().y[0]};
        const vec2 v{e.pool().vx[0], e.pool().vy[0]};
        CHECK(dot(normalize(to), normalize(v)) > 0.999f);
        CHECK(speed_of(e.pool(), 0) == doctest::Approx(100.0f).epsilon(1e-4));
    }
    SUBCASE("Homing respects delay") {
        Emitter e = single({100, 0}, {Homing{5.0f, 1.0f, 0.0f}});
        e.target = {0, 1000};
        for (int i = 0; i < 59; ++i) e.simulate(dt);
        CHECK(e.pool().vy[0] == 0.0f);
        e.simulate(dt);
        e.simulate(dt);
        CHECK(e.pool().vy[0] > 0.0f);
    }
    SUBCASE("Orbit keeps the radius and circles the emitter") {
        EmitterConfig config;
        config.modules = {Orbit{dmath::pi, 0.0f}};
        Emitter e(config);
        e.position = {100, 100};
        e.spawn({.position = {150, 100}});
        for (int i = 0; i < 60; ++i) e.simulate(dt); // half a turn
        const float dx = e.pool().x[0] - 100.0f;
        const float dy = e.pool().y[0] - 100.0f;
        CHECK(std::sqrt(dx * dx + dy * dy) == doctest::Approx(50.0f).epsilon(1e-4));
        CHECK(e.pool().x[0] == doctest::Approx(50.0f).epsilon(1e-3));
    }
    SUBCASE("Orbit spiral out from the center") {
        EmitterConfig config;
        config.modules = {Orbit{1.0f, 60.0f}};
        Emitter e(config);
        e.spawn({.position = {0, 0}, .velocity = {1, 0}});
        for (int i = 0; i < 60; ++i) e.simulate(dt);
        CHECK(std::sqrt(e.pool().x[0] * e.pool().x[0] + e.pool().y[0] * e.pool().y[0]) ==
              doctest::Approx(60.0f).epsilon(1e-3));
    }
    SUBCASE("Wave displaces sideways") {
        Emitter e = single({100, 0}, {Wave{10.0f, 1.0f, 0.0f}});
        for (int i = 0; i < 15; ++i) e.simulate(dt); // a quarter period
        CHECK(e.pool().y[0] == doctest::Approx(10.0f).epsilon(1e-3));
        CHECK(e.pool().x[0] == doctest::Approx(25.0f).epsilon(1e-4));
        CHECK(e.pool().vy[0] == 0.0f);
    }
    SUBCASE("FaceVelocity") {
        Emitter e = single({0, 50}, {FaceVelocity{}});
        e.simulate(dt);
        CHECK(e.pool().rotation[0] == doctest::Approx(dmath::half_pi));
    }
}

TEST_CASE("Over-life modules and KillOutside") {
    EmitterConfig config;
    config.modules = {ColorOverLife{{1, 0, 0, 1}, {0, 0, 1, 0}}, SizeOverLife{2.0f, 0.0f, Ease::Linear},
                      KillOutside{{-100, -100}, {100, 100}}};
    Emitter e(config);
    e.spawn({.position = {0, 0}, .velocity = {0, 0}, .life = 1.0f, .size = 4.0f});
    e.spawn({.position = {95, 0}, .velocity = {600, 0}, .life = 1.0f});
    for (int i = 0; i < 30; ++i) e.simulate(dt);
    REQUIRE(e.size() == 1); // the fast one left the box
    CHECK(e.pool().size[0] == doctest::Approx(4.0f));
    // Halfway from red to transparent blue (within one step of rounding).
    const std::uint32_t c = e.pool().color[0];
    const auto byte = [c](int k) { return static_cast<int>((c >> (8 * k)) & 0xffu); };
    CHECK(std::abs(byte(0) - 128) <= 1);
    CHECK(byte(1) == 0);
    CHECK(std::abs(byte(2) - 128) <= 1);
    CHECK(std::abs(byte(3) - 128) <= 1);
}

TEST_CASE("Emitter patterns") {
    Pcg32 rng(1);
    Emitter e;
    ParticleTemplate t;
    t.speed = 100.0f;

    SUBCASE("Ring: evenly spaced around the circle") {
        CHECK(e.emit(Ring{8, 0.25f}, {10, 20}, t, rng) == 8);
        for (std::size_t i = 0; i < 8; ++i) {
            CHECK(heading_of(e.pool(), i) == doctest::Approx(0.25f + dmath::tau * static_cast<float>(i) / 8.0f).epsilon(1e-5));
            CHECK(speed_of(e.pool(), i) == doctest::Approx(100.0f));
            CHECK(e.pool().x[i] == 10.0f);
            CHECK(e.pool().rotation[i] == doctest::Approx(heading_of(e.pool(), i)).epsilon(1e-5));
        }
    }
    SUBCASE("Arc: endpoints at +-spread/2") {
        e.emit(Arc{5, dmath::half_pi, 1.0f}, {}, t, rng);
        REQUIRE(e.size() == 5);
        CHECK(heading_of(e.pool(), 0) == doctest::Approx(dmath::half_pi - 0.5f));
        CHECK(heading_of(e.pool(), 2) == doctest::Approx(dmath::half_pi));
        CHECK(heading_of(e.pool(), 4) == doctest::Approx(dmath::half_pi + 0.5f));
    }
    SUBCASE("Aimed: centered on the target") {
        e.target = {100, 100};
        e.emit(Aimed{3, 0.4f}, {0, 0}, t, rng);
        REQUIRE(e.size() == 3);
        CHECK(heading_of(e.pool(), 1) == doctest::Approx(dmath::pi / 4.0f).epsilon(1e-5));
        CHECK(heading_of(e.pool(), 0) == doctest::Approx(dmath::pi / 4.0f - 0.2f).epsilon(1e-5));
    }
    SUBCASE("Burst: random headings inside the spread, seeded") {
        t.speed_jitter = 20.0f;
        e.emit(Burst{64, 1.0f, 0.5f}, {}, t, rng);
        REQUIRE(e.size() == 64);
        Pcg32 rng2(1);
        Emitter e2;
        e2.emit(Burst{64, 1.0f, 0.5f}, {}, t, rng2);
        CHECK(e.pool().vx == e2.pool().vx);
        CHECK(e.pool().vy == e2.pool().vy);
        float lo = 10.0f, hi = -10.0f;
        for (std::size_t i = 0; i < 64; ++i) {
            lo = std::min(lo, heading_of(e.pool(), i));
            hi = std::max(hi, heading_of(e.pool(), i));
            CHECK(speed_of(e.pool(), i) >= 80.0f - 1e-3f);
            CHECK(speed_of(e.pool(), i) <= 120.0f + 1e-3f);
        }
        CHECK(lo >= 0.75f - 1e-5f);
        CHECK(hi <= 1.25f + 1e-5f);
        CHECK(hi - lo > 0.3f);
    }
    SUBCASE("Spiral: the phase advances every shot") {
        Pattern spiral = Spiral{3, 0.0f, 0.1f};
        e.emit(spiral, {}, t, rng);
        e.emit(spiral, {}, t, rng);
        REQUIRE(e.size() == 6);
        CHECK(heading_of(e.pool(), 3) == doctest::Approx(0.1f).epsilon(1e-5));
        CHECK(std::get<Spiral>(spiral).phase == doctest::Approx(0.2f));
    }
    SUBCASE("Spawner: delay, interval and shot count") {
        Spawner s;
        s.pattern = Ring{4, 0.0f};
        s.particle = t;
        s.interval = 3;
        s.delay = 2;
        s.shots = 2;
        e.add_spawner(s);
        std::vector<std::size_t> sizes;
        for (int i = 0; i < 10; ++i) {
            e.run_spawners(rng);
            sizes.push_back(e.size());
        }
        CHECK(sizes == std::vector<std::size_t>{0, 0, 4, 4, 4, 8, 8, 8, 8, 8});
    }
}

TEST_CASE("Foreach hook runs after modules and can kill") {
    Emitter e;
    for (int i = 0; i < 10; ++i) e.spawn({.position = {static_cast<float>(i), 0}, .velocity = {1, 0}});
    int calls = 0;
    e.set_foreach([&](Emitter& em, float hook_dt) {
        ++calls;
        CHECK(hook_dt == dt);
        em.for_each([](ParticleRef p) {
            p.tag() += 1;
            if (p.pos().x() > 5.0f) p.kill();
        });
    });
    e.simulate(dt);
    CHECK(calls == 1);
    CHECK(e.size() == 5);
    for (std::size_t i = 0; i < e.size(); ++i) CHECK(e.pool().tag[i] == 1);
}

TEST_CASE("ParticleSystem collision: hits kill bullets and report in order") {
    ParticleSystem system(42, 16.0f);
    EmitterConfig config;
    config.collision = {.enabled = true, .layer = 1, .hits = collision::layer_bit(0), .radius = 2.0f};
    const EmitterId id = system.add_emitter(config);
    Emitter& e = system.emitter(id);
    // A column of bullets falling on a player hitbox at (0, 100).
    for (int i = 0; i < 5; ++i) e.spawn({.position = {0, 100.0f - 30.0f * static_cast<float>(i)}, .velocity = {0, 600}});
    e.spawn({.position = {50, 100}, .velocity = {0, 0}});

    const auto player = [](collision::CollisionWorld& w) { w.add_circle({{0, 100}, 3.0f}, 0, 7); };
    // Bullets move 10 units per tick; a hit needs |y - 100| <= 5 after the
    // move. Bullet 0 starts on the player but has left before the first check
    // (discrete collision); bullets 1..4 land exactly on y = 100.
    std::vector<std::uint64_t> hit_ticks;
    for (int t = 0; t < 30; ++t) {
        system.tick(dt, player);
        bool hit_event = false;
        for (const ParticleEvent& ev : system.emitter(id).events()) hit_event |= ev.type == EventType::Hit;
        CHECK(hit_event == !system.hits().empty());
        for (const Hit& h : system.hits()) {
            CHECK(h.target_user == 7);
            CHECK(h.emitter == id);
            CHECK(h.position.y() == doctest::Approx(100.0f));
            CHECK_FALSE(system.emitter(id).pool().alive(h.particle)); // kill_on_hit
            hit_ticks.push_back(system.ticks());
        }
    }
    CHECK(hit_ticks == std::vector<std::uint64_t>{3, 6, 9, 12});
    REQUIRE(system.emitter(id).size() == 2); // bullet 0 and the stationary one
    bool stationary_alive = false;
    for (std::size_t i = 0; i < 2; ++i) stationary_alive |= system.emitter(id).pool().x[i] == 50.0f;
    CHECK(stationary_alive);
}

namespace {

// A small bullet-hell scene exercising every module, pattern, collision and
// the foreach hook.
std::vector<std::uint64_t> run_scene(std::uint64_t seed, int ticks, jobs::JobSystem* jobs = nullptr,
                                     bool hook = true, float density = 1.0f) {
    ParticleSystem system(seed, 24.0f);
    system.set_jobs(jobs);

    EmitterConfig boss;
    boss.collision = {.enabled = true, .layer = 1, .hits = collision::layer_bit(0), .radius = 3.0f};
    boss.modules = {SpeedRamp{40.0f, 0.0f, 300.0f}, Turn{0.3f}, Wave{4.0f, 1.5f, 0.0f}, FaceVelocity{},
                    ColorOverLife{}, SizeOverLife{1.0f, 0.5f, Ease::Smooth}, KillOutside{{-50, -50}, {690, 410}}};
    ParticleTemplate shot;
    shot.speed = 90.0f;
    shot.speed_jitter = 15.0f;
    shot.life = 5.0f;
    shot.life_jitter = 0.5f;
    boss.spawners.push_back({.pattern = Spiral{static_cast<std::uint32_t>(5 * density), 0.0f, 0.17f},
                             .particle = shot, .interval = 4});
    boss.spawners.push_back({.pattern = Aimed{5, 0.6f}, .particle = shot, .interval = 25, .delay = 10});
    boss.spawners.push_back({.pattern = Burst{static_cast<std::uint32_t>(30 * density), 0.0f, dmath::tau},
                             .particle = shot, .interval = 60});
    const EmitterId boss_id = system.add_emitter(boss);

    EmitterConfig homing;
    homing.collision = {.enabled = true, .layer = 2, .hits = collision::layer_bit(0), .half_length = 4.0f};
    homing.modules = {Homing{2.0f, 0.5f, 2.0f}, Drag{0.1f}, FaceVelocity{}};
    homing.spawners.push_back({.pattern = Ring{8, 0.0f}, .particle = shot, .interval = 40});
    const EmitterId homing_id = system.add_emitter(homing);

    EmitterConfig orbit;
    orbit.modules = {Orbit{1.2f, 15.0f}, Accelerate{{0, 5}}};
    orbit.spawners.push_back({.pattern = Ring{6, 0.0f}, .particle = shot, .interval = 30});
    const EmitterId orbit_id = system.add_emitter(orbit);
    if (hook) {
        system.emitter(orbit_id).set_foreach([](Emitter& e, float) {
            e.for_each([](ParticleRef p) {
                if (p.age() > 3.0f) p.kill();
            });
        });
    }

    EmitterConfig sparks;
    sparks.mode = Mode::Visual;
    sparks.spawners.push_back({.pattern = Burst{10, 0.0f, dmath::tau}, .particle = shot, .interval = 5});
    system.add_emitter(sparks);

    std::vector<std::uint64_t> hashes;
    vec2 player{320, 300};
    for (int t = 0; t < ticks; ++t) {
        // Deterministic "input": the player strafes.
        player.x() = 320.0f + 200.0f * dmath::sin(static_cast<float>(t) * 0.02f);
        for (EmitterId id : {boss_id, homing_id, orbit_id}) {
            system.emitter(id).position = {320, 120};
            system.emitter(id).target = player;
        }
        system.tick(dt, [&](collision::CollisionWorld& w) { w.add_circle({player, 6.0f}, 0, 1); });
        hashes.push_back(system.state_hash());
    }
    return hashes;
}

} // namespace

TEST_CASE("Deterministic replay: same seed, same state hash every tick") {
    const auto a = run_scene(1234, 600);
    const auto b = run_scene(1234, 600);
    CHECK(a == b);
    const auto c = run_scene(1235, 600);
    CHECK(a.back() != c.back());
    // The hash changes over time (the scene is not static).
    CHECK(a[100] != a[200]);
}

TEST_CASE("Deterministic replay: the state hash doesn't depend on the job thread count (DESIGN.md 4.15)") {
    // With and without a foreach hook (which serializes the finish phase),
    // and dense enough that collision build and queries take their parallel
    // paths (> 4096 proxies).
    for (const bool hook : {true, false}) {
        jobs::JobSystem one(1);
        const auto expected = run_scene(99, 400, &one, hook, 12.0f);
        for (unsigned threads : {2u, 3u, 8u}) {
            jobs::JobSystem js(threads);
            const auto got = run_scene(99, 400, &js, hook, 12.0f);
            REQUIRE(got.size() == expected.size());
            for (std::size_t t = 0; t < got.size(); ++t) {
                if (got[t] != expected[t]) FAIL("hash differs at tick ", t, " with ", threads, " threads");
            }
            CHECK(js.parallel_batches() > 0);
        }
    }
}

TEST_CASE("draw_emitter emits one SDF instance per particle") {
    EmitterConfig config;
    config.render.shape = render::SdfShape::Capsule;
    config.render.half_length = 2.0f;
    Emitter e(config);
    e.spawn({.position = {10, 0}, .velocity = {60, 0}, .size = 3.0f, .rotation = 0.5f});
    e.spawn({.position = {20, 0}, .size = 1.0f});
    e.simulate(dt);
    render::SdfBatcher batch; // no device: collects only
    CHECK(draw_emitter(e, batch, 0.5f) == 2);
    REQUIRE(batch.size() == 2);
    const render::SdfInstance& inst = batch.instances()[0];
    CHECK(inst.center.x() == doctest::Approx(10.5f));
    CHECK(inst.half_size.x() == doctest::Approx(9.0f));
    CHECK(inst.half_size.y() == doctest::Approx(3.0f));
    CHECK(inst.rotation == doctest::Approx(0.5f));
    CHECK(inst.shape == render::SdfShape::Capsule);
}

namespace {

// Gameplay emitter with random headings and jitter: every spawn draws.
EmitterConfig random_shooter() {
    EmitterConfig c;
    ParticleTemplate t;
    t.speed = 80.0f;
    t.speed_jitter = 20.0f;
    t.life = 2.0f;
    t.life_jitter = 0.5f;
    c.spawners.push_back({.pattern = Burst{12, 0.0f, dmath::tau}, .particle = t, .interval = 3});
    return c;
}

EmitterConfig visual_sparks() {
    EmitterConfig c = random_shooter();
    c.mode = Mode::Visual;
    c.spawners[0].interval = 1;
    return c;
}

} // namespace

TEST_CASE("Sub-seeded streams: a new visual emitter doesn't shift gameplay draws (DESIGN.md 4.15)") {
    ParticleSystem plain(77);
    const EmitterId shooter = plain.add_emitter(random_shooter());
    ParticleSystem with_sparks(77);
    REQUIRE(with_sparks.add_emitter(random_shooter()) == shooter);
    const EmitterId sparks = with_sparks.add_emitter(visual_sparks());
    for (int t = 0; t < 120; ++t) {
        plain.tick(dt);
        with_sparks.tick(dt);
        if (t % 10 == 0) {
            // A script firing a random burst through the sparks emitter.
            with_sparks.emitter(sparks).emit(Burst{5, 0.0f, 1.0f}, {}, ParticleTemplate{});
        }
    }
    CHECK(with_sparks.emitter(sparks).size() > 0);
    const ParticlePool& a = plain.emitter(shooter).pool();
    const ParticlePool& b = with_sparks.emitter(shooter).pool();
    REQUIRE(a.size_live() > 0);
    CHECK(a.vx == b.vx);
    CHECK(a.vy == b.vy);
    CHECK(a.life == b.life);
    // Emitters never draw from the simulation stream (the scripts' Random).
    CHECK(plain.rng() == Pcg32(77));
    CHECK(with_sparks.rng() == Pcg32(77));
}

TEST_CASE("Sub-seeded streams: each emitter's stream is keyed by id and generation") {
    ParticleSystem system(5);
    const EmitterId a = system.add_emitter(random_shooter());
    const EmitterId b = system.add_emitter(random_shooter());
    CHECK(system.emitter(a).rng() == derive_stream(5, {rng_stream::particles, a, 0}));
    CHECK(system.emitter(b).rng() == derive_stream(5, {rng_stream::particles, b, 0}));
    CHECK_FALSE(system.emitter(a).rng() == system.emitter(b).rng());
    // A reused id gets a fresh stream instead of replaying the old one.
    system.replace_emitter(a, random_shooter());
    CHECK(system.emitter(a).rng() == derive_stream(5, {rng_stream::particles, a, 1}));
}

TEST_CASE("draw_emitters packs the same instances as draw_emitter, in parallel") {
    ParticleSystem system(3);
    for (int k = 0; k < 3; ++k) {
        EmitterConfig c = random_shooter();
        c.render.shape = static_cast<render::SdfShape>(k);
        c.spawners[0].pattern = Burst{3000, 0.0f, dmath::tau};
        system.add_emitter(c);
    }
    system.emitter(1).render().visible = false;
    for (int t = 0; t < 4; ++t) system.tick(dt);
    render::SdfBatcher serial;
    for (EmitterId id = 0; id < system.emitter_count(); ++id) draw_emitter(system.emitter(id), serial, 0.25f);
    jobs::JobSystem js(4);
    render::SdfBatcher parallel;
    parallel.circle({1, 2}, 3, {1, 1, 1, 1}); // appends after existing instances
    CHECK(draw_emitters(system, parallel, 0.25f, &js) == serial.size());
    REQUIRE(parallel.size() == serial.size() + 1);
    CHECK(std::memcmp(parallel.instances().data() + 1, serial.instances().data(),
                      serial.size() * sizeof(render::SdfInstance)) == 0);
}

TEST_CASE("Deterministic replay: SIMD and scalar paths give the same state hash every tick (DESIGN.md 4.15)") {
    const bool was = simd::enabled();
    simd::set_enabled(false);
    const auto scalar = run_scene(4242, 300, nullptr, false, 6.0f);
    simd::set_enabled(true);
    jobs::JobSystem js(4);
    const auto vector = run_scene(4242, 300, &js, false, 6.0f);
    simd::set_enabled(was);
    CHECK(scalar == vector);
}

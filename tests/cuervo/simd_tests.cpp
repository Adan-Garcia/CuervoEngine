#include <doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if CUERVO_FEATURE_COLLISION
#include "cuervo/collision/collision_world.hpp"
#endif
#include "cuervo/core/rng.hpp"
#include "cuervo/math/dmath.hpp"
#include "cuervo/math/dmath_simd.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/math/vec.hpp"
#if CUERVO_FEATURE_PARTICLES
#include "cuervo/particles/emitter.hpp"
#include "cuervo/particles/particle_render.hpp"
#endif

// DESIGN.md 4.15: SIMD and scalar paths must give bit-identical results. Every
// test runs both on the same inputs and compares raw bits.
using namespace cuervo;

namespace {

std::uint32_t bits_of(float f) { return std::bit_cast<std::uint32_t>(f); }

// Restores the runtime switch after a test.
struct SimdSwitch {
    bool was = simd::enabled();
    ~SimdSwitch() { simd::set_enabled(was); }
};

[[maybe_unused]] std::vector<float> test_values(Pcg32& rng, std::size_t count, float range) {
    std::vector<float> v = {0.0f, -0.0f, 1.0f, -1.0f, dmath::pi, -dmath::pi, dmath::half_pi, -dmath::half_pi,
                            1e-30f, -1e-30f, 1e6f, -1e6f, 8388608.5f, 3e9f, -3e9f};
    while (v.size() < count) v.push_back(rng.range(-range, range));
    while (v.size() % 8 != 0) v.push_back(rng.range(-range, range));
    return v;
}

} // namespace

TEST_CASE("SIMD build configuration") {
    MESSAGE("SIMD ISA: ", std::string(simd::isa), ", width ", simd::width);
    CHECK(simd::available == (simd::width > 1));
    SimdSwitch restore;
    simd::set_enabled(false);
    CHECK_FALSE(simd::enabled());
    simd::set_enabled(true);
    CHECK(simd::enabled() == simd::available);
}

#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2) || defined(CUERVO_SIMD_WASM)

TEST_CASE("SIMD dmath sin/cos/atan2/floor are bit-identical to scalar dmath") {
    Pcg32 rng(11);
    for (const float range : {4.0f, 100.0f, 1e5f}) {
        const std::vector<float> xs = test_values(rng, 4000, range);
        const std::vector<float> ys = test_values(rng, 4000, range);
        for (std::size_t i = 0; i < xs.size(); i += simd::width) {
            const simd::F x = simd::load(&xs[i]);
            const simd::F y = simd::load(&ys[i]);
            float s[simd::width], c[simd::width], a[simd::width], f[simd::width];
            simd::store(s, dmath::simd::sin(x));
            simd::store(c, dmath::simd::cos(x));
            simd::store(a, dmath::simd::atan2(y, x));
            simd::store(f, simd::floor(x));
            for (int k = 0; k < simd::width; ++k) {
                REQUIRE(bits_of(s[k]) == bits_of(dmath::sin(xs[i + k])));
                REQUIRE(bits_of(c[k]) == bits_of(dmath::cos(xs[i + k])));
                REQUIRE(bits_of(a[k]) == bits_of(dmath::atan2(ys[i + k], xs[i + k])));
                REQUIRE(bits_of(f[k]) == bits_of(dmath::floor(xs[i + k])));
            }
        }
    }
    // atan2 on axes and zeros of both signs.
    const float specials[8] = {0.0f, -0.0f, 1.0f, -1.0f, 0.0f, 3.0f, -0.0f, -2.0f};
    const float others[8] = {0.0f, 0.0f, 0.0f, -0.0f, 5.0f, 3.0f, -4.0f, 2.0f};
    for (int k = 0; k + simd::width <= 8; k += simd::width) {
        float a[simd::width];
        simd::store(a, dmath::simd::atan2(simd::load(&specials[k]), simd::load(&others[k])));
        for (int j = 0; j < simd::width; ++j) CHECK(bits_of(a[j]) == bits_of(dmath::atan2(specials[k + j], others[k + j])));
    }
}

TEST_CASE("SIMD dmath tan/asin/acos/atan/sign/step/lerp/smoothstep match scalar dmath") {
    Pcg32 rng(19);
    const std::vector<float> unit_xs = test_values(rng, 1000, 1.0f);
    for (std::size_t i = 0; i < unit_xs.size(); i += simd::width) {
        const simd::F x = simd::load(&unit_xs[i]);
        float as_s[simd::width], ac_s[simd::width];
        simd::store(as_s, dmath::simd::asin(x));
        simd::store(ac_s, dmath::simd::acos(x));
        for (int k = 0; k < simd::width; ++k) {
            float exp_as = dmath::asin(unit_xs[i + k]);
            float exp_ac = dmath::acos(unit_xs[i + k]);
            if (std::isnan(exp_as)) {
                CHECK(std::isnan(as_s[k]));
            } else {
                REQUIRE(bits_of(as_s[k]) == bits_of(exp_as));
            }
            if (std::isnan(exp_ac)) {
                CHECK(std::isnan(ac_s[k]));
            } else {
                REQUIRE(bits_of(ac_s[k]) == bits_of(exp_ac));
            }
        }
    }

    for (const float range : {1.4f, 20.0f, 100.0f}) {
        const std::vector<float> xs = test_values(rng, 1000, range);
        const std::vector<float> ys = test_values(rng, 1000, range);
        for (std::size_t i = 0; i < xs.size(); i += simd::width) {
            const simd::F x = simd::load(&xs[i]);
            const simd::F y = simd::load(&ys[i]);
            float t[simd::width], at[simd::width], sg[simd::width], st[simd::width];
            float lp[simd::width], sm[simd::width], r2d[simd::width], d2r[simd::width];
            simd::store(t, dmath::simd::tan(x));
            simd::store(at, dmath::simd::atan(x));
            simd::store(sg, dmath::simd::sign(x));
            simd::store(st, dmath::simd::step(y, x));
            simd::store(lp, dmath::simd::lerp(x, y, simd::splat(0.37f)));
            simd::store(sm, dmath::simd::smoothstep(simd::splat(-5.0f), simd::splat(5.0f), x));
            simd::store(r2d, dmath::simd::rad_to_deg(x));
            simd::store(d2r, dmath::simd::deg_to_rad(x));
            for (int k = 0; k < simd::width; ++k) {
                REQUIRE(bits_of(t[k]) == bits_of(dmath::tan(xs[i + k])));
                REQUIRE(bits_of(at[k]) == bits_of(dmath::atan(xs[i + k])));
                REQUIRE(bits_of(sg[k]) == bits_of(dmath::sign(xs[i + k])));
                REQUIRE(bits_of(st[k]) == bits_of(dmath::step(ys[i + k], xs[i + k])));
                REQUIRE(bits_of(lp[k]) == bits_of(dmath::lerp(xs[i + k], ys[i + k], 0.37f)));
                REQUIRE(bits_of(sm[k]) == bits_of(dmath::smoothstep(-5.0f, 5.0f, xs[i + k])));
                REQUIRE(bits_of(r2d[k]) == bits_of(dmath::rad_to_deg(xs[i + k])));
                REQUIRE(bits_of(d2r[k]) == bits_of(dmath::deg_to_rad(xs[i + k])));
            }
        }
    }
}

#endif

TEST_CASE("Vec<float, 2..4> operators match the scalar component loops bit for bit") {
    Pcg32 rng(5);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto check = [&]<std::size_t N>(std::integral_constant<std::size_t, N>) {
        for (int round = 0; round < 2000; ++round) {
            vecn<N> a, b;
            for (std::size_t i = 0; i < N; ++i) {
                a[i] = rng.range(-100.0f, 100.0f);
                b[i] = round % 17 == 0 ? 0.0f : rng.range(-100.0f, 100.0f);
            }
            if (round % 31 == 0) a[0] = -0.0f;
            if (round % 37 == 0) b[N - 1] = nan;
            const float s = rng.range(-3.0f, 3.0f);
            vecn<N> sum = a + b, diff = a - b, prod = a * b, quot = a / b, scaled = a * s, divided = a / s;
            const vecn<N> neg = -a, lo = min(a, b), hi = max(a, b);
            float d_expected = 0.0f;
            for (std::size_t i = 0; i < N; ++i) {
                CHECK(bits_of(sum[i]) == bits_of(a[i] + b[i]));
                CHECK(bits_of(diff[i]) == bits_of(a[i] - b[i]));
                CHECK(bits_of(prod[i]) == bits_of(a[i] * b[i]));
                CHECK(bits_of(quot[i]) == bits_of(a[i] / b[i]));
                CHECK(bits_of(scaled[i]) == bits_of(a[i] * s));
                CHECK(bits_of(divided[i]) == bits_of(a[i] / s));
                CHECK(bits_of(neg[i]) == bits_of(-a[i]));
                CHECK(bits_of(lo[i]) == bits_of(b[i] < a[i] ? b[i] : a[i]));
                CHECK(bits_of(hi[i]) == bits_of(a[i] < b[i] ? b[i] : a[i]));
                d_expected += a[i] * b[i];
            }
            CHECK(bits_of(dot(a, b)) == bits_of(d_expected));
        }
        // Constant evaluation takes the scalar path.
        static_assert((Vec<float, N>(1.0f) + Vec<float, N>(2.0f))[0] == 3.0f);
    };
    check(std::integral_constant<std::size_t, 2>{});
    check(std::integral_constant<std::size_t, 3>{});
    check(std::integral_constant<std::size_t, 4>{});
}

#if CUERVO_FEATURE_PARTICLES
namespace {

// A pool of varied particles (moving, at rest, dying, immortal), `count` not
// a multiple of the SIMD width so the scalar tails run too.
particles::Emitter make_emitter(std::size_t count, std::vector<particles::Module> modules) {
    particles::EmitterConfig config;
    config.modules = std::move(modules);
    particles::Emitter e(config);
    Pcg32 rng(42);
    for (std::size_t i = 0; i < count; ++i) {
        const bool rest = i % 13 == 0;
        e.spawn({.position = {rng.range(-50.0f, 700.0f), rng.range(-50.0f, 400.0f)},
                 .velocity = rest ? vec2{0.0f, 0.0f} : vec2{rng.range(-200.0f, 200.0f), rng.range(-200.0f, 200.0f)},
                 .life = i % 7 == 0 ? 0.0f : rng.range(0.05f, 3.0f),
                 .size = rng.range(1.0f, 8.0f),
                 .rotation = rng.range(-3.0f, 3.0f),
                 .spin = rng.range(-2.0f, 2.0f)});
    }
    e.position = {320.0f, 200.0f};
    e.target = {100.0f, 350.0f};
    return e;
}

void require_same_pool(const particles::ParticlePool& a, const particles::ParticlePool& b) {
    REQUIRE(a.size_live() == b.size_live());
    const auto same = [](const std::vector<float>& x, const std::vector<float>& y) {
        return x.size() == y.size() && std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0;
    };
    CHECK(same(a.x, b.x));
    CHECK(same(a.y, b.y));
    CHECK(same(a.prev_x, b.prev_x));
    CHECK(same(a.vx, b.vx));
    CHECK(same(a.vy, b.vy));
    CHECK(same(a.age, b.age));
    CHECK(same(a.size, b.size));
    CHECK(same(a.rotation, b.rotation));
    CHECK(a.color == b.color);
    CHECK(a.slots() == b.slots());
}

} // namespace

TEST_CASE("Particle SIMD kernels are bit-identical to the scalar loops, module by module") {
    using namespace particles;
    SimdSwitch restore;
    const std::vector<std::vector<Module>> stacks = {
        {},
        {Accelerate{{3.0f, 98.0f}}},
        {Drag{0.7f}},
        {SpeedRamp{40.0f, 20.0f, 150.0f}},
        {Turn{0.8f}},
        {Homing{2.5f, 0.1f, 1.5f}},
        {Homing{2.5f, 0.0f, 0.0f}},
        {Orbit{1.2f, 15.0f}},
        {Orbit{-0.6f, -40.0f}},
        {Wave{8.0f, 1.5f, 0.3f}},
        {FaceVelocity{}},
        {ColorOverLife{{1.0f, 0.2f, 0.3f, 1.0f}, {0.1f, 0.9f, 1.4f, -0.2f}}},
        {SizeOverLife{1.0f, 0.2f, Ease::Linear}},
        {SizeOverLife{0.5f, 2.0f, Ease::EaseIn}},
        {SizeOverLife{1.0f, 0.0f, Ease::EaseOut}},
        {SizeOverLife{1.0f, 0.3f, Ease::Smooth}},
        {KillOutside{{0.0f, 0.0f}, {640.0f, 360.0f}}},
        {SpeedRamp{40.0f, 0.0f, 170.0f}, Turn{0.35f}, FaceVelocity{}, Wave{12.0f, 0.7f, 0.0f}, Orbit{0.9f, 55.0f},
         Homing{1.8f, 0.6f, 2.2f}, ColorOverLife{}, SizeOverLife{1.0f, 0.4f, Ease::EaseIn}, Drag{0.1f},
         KillOutside{{-40.0f, -40.0f}, {1320.0f, 760.0f}}},
    };
    for (std::size_t s = 0; s < stacks.size(); ++s) {
        CAPTURE(s);
        Emitter scalar = make_emitter(1003, stacks[s]);
        Emitter vector = make_emitter(1003, stacks[s]);
        for (int t = 0; t < 40; ++t) {
            simd::set_enabled(false);
            scalar.simulate(1.0f / 60.0f);
            simd::set_enabled(true);
            vector.simulate(1.0f / 60.0f);
        }
        require_same_pool(scalar.pool(), vector.pool());
        // Ranges with odd bounds (job splits) give the same result as well.
        Emitter ranged = make_emitter(1003, stacks[s]);
        Emitter whole = make_emitter(1003, stacks[s]);
        ranged.begin_tick();
        ranged.step_range(0.02f, 0, 5);
        ranged.step_range(0.02f, 5, 517);
        ranged.step_range(0.02f, 517, ranged.size());
        ranged.finish_tick(0.02f);
        simd::set_enabled(false);
        whole.simulate(0.02f);
        require_same_pool(ranged.pool(), whole.pool());
    }
}
#endif

#if CUERVO_FEATURE_COLLISION
TEST_CASE("SIMD hash cell computation gives the same broadphase as scalar") {
    SimdSwitch restore;
    Pcg32 rng(8);
    collision::CollisionWorld scalar(16.0f);
    collision::CollisionWorld vector(16.0f);
    scalar.set_jobs(nullptr);
    vector.set_jobs(nullptr);
    std::vector<collision::Circle> shapes;
    for (int i = 0; i < 3001; ++i) {
        float x = rng.range(-2000.0f, 2000.0f);
        if (i % 97 == 0) x = std::numeric_limits<float>::quiet_NaN();
        if (i % 89 == 0) x = 1e30f;
        if (i % 83 == 0) x = -0.0f;
        if (i % 79 == 0) x = 16.0f * static_cast<float>(i % 7); // exactly on cell borders
        shapes.push_back({{x, rng.range(-500.0f, 500.0f)}, i % 50 == 0 ? rng.range(100.0f, 400.0f) : rng.range(0.5f, 9.0f)});
    }
    for (const collision::Circle& c : shapes) {
        scalar.add_circle(c, 0);
        vector.add_circle(c, 0);
    }
    simd::set_enabled(false);
    scalar.build();
    simd::set_enabled(true);
    vector.build();
    CHECK(scalar.entry_count() == vector.entry_count());
    CHECK(scalar.large_count() == vector.large_count());
    std::vector<collision::ProxyPair> a, b;
    scalar.pairs(collision::all_layers, collision::all_layers, a);
    vector.pairs(collision::all_layers, collision::all_layers, b);
    CHECK(a == b);
    CHECK_FALSE(a.empty());
}
#endif

#if CUERVO_FEATURE_PARTICLES
TEST_CASE("SIMD SDF instance packing is bit-identical to scalar") {
    using namespace particles;
    SimdSwitch restore;
    for (const render::SdfShape shape :
         {render::SdfShape::Circle, render::SdfShape::Capsule, render::SdfShape::RoundedRect, render::SdfShape::Ring}) {
        Emitter e = make_emitter(1001, {Turn{0.5f}, ColorOverLife{}});
        e.render().shape = shape;
        e.simulate(0.016f);
        render::SdfBatcher scalar;
        render::SdfBatcher vector;
        simd::set_enabled(false);
        draw_emitter(e, scalar, 0.37f);
        simd::set_enabled(true);
        draw_emitter(e, vector, 0.37f);
        REQUIRE(scalar.size() == vector.size());
        CHECK(std::memcmp(scalar.instances().data(), vector.instances().data(),
                          scalar.size() * sizeof(render::SdfInstance)) == 0);
    }
}
#endif

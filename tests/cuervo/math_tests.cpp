// Milestone 9.2b (DESIGN.md 4.17, LANGUAGE.md 9.20 - 9.24) in the engine: the
// math library's transcendental functions go through the host's deterministic
// math (dmath), Transform2D matches cuervo/math/affine2.hpp, Rng matches
// cuervo/core/rng.hpp, and `shuffle` / `Random.*` draw from the simulation's
// seeded PCG32. The language-side checks (VM against AOT, golden output) are
// in tests/pluma/math_tests.cpp and aot_tests.cpp.

#include <doctest.h>

#include <bit>
#include <cmath>
#include <vector>

#include "cuervo/core/rng.hpp"
#include "cuervo/math/affine2.hpp"
#include "cuervo/math/dmath.hpp"
#include "cuervo/math/dmath_simd.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;

namespace {

std::uint32_t bits_of(float f) { return std::bit_cast<std::uint32_t>(f); }

// Loads `Main` (a Node) and returns the float and int64 lists its ready()
// filled in (`fs`, `ns`).
struct Results {
    std::vector<float> fs;
    std::vector<std::int64_t> ns;
};

Results run_script(std::string_view body, std::uint64_t seed = 1) {
    InputMap map;
    script::HostOptions o;
    o.seed = seed;
    script::ScriptHost host(o, map);
    std::string source = "class Main extends Node {\n    var fs = List<float>()\n    var ns = List<i64>()\n"
                         "    fn ready() {\n" +
                         std::string(body) + "\n    }\n}\n";
    REQUIRE_MESSAGE(host.load({{"main.plm", source}}, "Main"), host.error());
    pluma::Value root = host.session()->global("Scene.root");
    auto* inst = static_cast<pluma::InstanceObj*>(root.o);
    Results r;
    const pluma::RecordRT* rt = inst->type;
    for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
        if (rt->field_names[i] != "fs" && rt->field_names[i] != "ns") continue;
        auto* list = static_cast<pluma::ListObj*>(inst->fields[i].o);
        for (const pluma::Value& v : list->items) {
            if (rt->field_names[i] == "fs") {
                r.fs.push_back(v.f);
            } else {
                r.ns.push_back(v.i);
            }
        }
    }
    return r;
}

void check_bits(const std::vector<float>& got, const std::vector<float>& want) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        INFO("value #" << i << ": got " << got[i] << ", want " << want[i]);
        CHECK(bits_of(got[i]) == bits_of(want[i]));
    }
}

} // namespace

TEST_CASE("math: hyperbolic functions and cbrt use dmath exp/log") {
    Results r = run_script(R"(
        fs.push(sinh(2.0))
        fs.push(cosh(2.0))
        fs.push(tanh(0.5))
        fs.push(asinh(2.0))
        fs.push(acosh(2.0))
        fs.push(atanh(0.5))
        fs.push(cbrt(27.0))
        fs.push(cbrt(-0.3))
    )");
    auto newton = [](float a) {
        float y = dmath::exp(dmath::log(a) * 0.33333334f);
        y = y - (y * y * y - a) / (3.0f * y * y);
        y = y - (y * y * y - a) / (3.0f * y * y);
        return y;
    };
    const float e2 = dmath::exp(2.0f * 0.5f);
    check_bits(r.fs, {
                         (dmath::exp(2.0f) - dmath::exp(-2.0f)) * 0.5f,
                         (dmath::exp(2.0f) + dmath::exp(-2.0f)) * 0.5f,
                         (e2 - 1.0f) / (e2 + 1.0f),
                         dmath::log(2.0f + std::sqrt(2.0f * 2.0f + 1.0f)),
                         dmath::log(2.0f + std::sqrt(2.0f * 2.0f - 1.0f)),
                         0.5f * dmath::log((1.0f + 0.5f) / (1.0f - 0.5f)),
                         newton(27.0f),
                         -newton(0.3f),
                     });
}

TEST_CASE("math: ease curves use dmath sin/cos/pow") {
    Results r = run_script(R"(
        fs.push(ease_in(Ease.Sine, 0.37))
        fs.push(ease_out(Ease.Sine, 0.37))
        fs.push(ease_in(Ease.Expo, 0.6))
        fs.push(ease_in(Ease.Elastic, 0.4))
        fs.push(ease(0.3, 2.5))
        fs.push(ease(0.3, 0.5))
        fs.push(ease(0.3, -2.0))
    )");
    const float pi_ = 3.1415927f;
    const float tau_ = 6.2831855f;
    auto sine_in = [&](float t) { return 1.0f - dmath::cos(t * pi_ * 0.5f); };
    check_bits(r.fs, {
                         sine_in(0.37f),
                         1.0f - sine_in(1.0f - 0.37f),
                         dmath::pow(2.0f, 10.0f * 0.6f - 10.0f),
                         -dmath::pow(2.0f, 10.0f * 0.4f - 10.0f) * dmath::sin((0.4f * 10.0f - 10.75f) * (tau_ / 3.0f)),
                         dmath::pow(0.3f, 2.5f),
                         1.0f - dmath::pow(1.0f - 0.3f, 1.0f / 0.5f),
                         dmath::pow(0.3f * 2.0f, 2.0f) * 0.5f,
                     });
}

TEST_CASE("math: lerp_angle, rotate_toward and angle_difference use the host's wrap") {
    Results r = run_script(R"(
        fs.push(angle_difference(3.0, -3.0))
        fs.push(lerp_angle(3.0, -3.0, 0.25))
        fs.push(rotate_toward(0.5, 2.5, 0.75))
    )");
    auto wrap_angle = [](float a) {
        const float pi_ = 3.1415927f;
        const float tau_ = 6.2831855f;
        return (a + pi_) - tau_ * std::floor((a + pi_) / tau_) - pi_;
    };
    float d = wrap_angle(-3.0f - 3.0f);
    check_bits(r.fs, {d, 3.0f + d * 0.25f, 0.5f + 0.75f});
}

TEST_CASE("math: Transform2D is Affine2") {
    Results r = run_script(R"(
        var t = Transform2D.translate(vec2(3.0, 4.0)) * Transform2D.rotate(0.7) * Transform2D.scale(vec2(2.0, 0.5))
        var u = t.inverse()
        fs.push(t.a)
        fs.push(t.b)
        fs.push(t.c)
        fs.push(t.d)
        fs.push(t.tx)
        fs.push(t.ty)
        fs.push(u.a)
        fs.push(u.b)
        fs.push(u.c)
        fs.push(u.d)
        fs.push(u.tx)
        fs.push(u.ty)
        var p = t.apply(vec2(1.5, -2.0))
        var q = t.apply_vector(vec2(1.5, -2.0))
        fs.push(p.x)
        fs.push(p.y)
        fs.push(q.x)
        fs.push(q.y)
        fs.push(t.determinant())
        var s = Transform2D.rotate(1.234)
        fs.push(s.a)
        fs.push(s.b)
    )");
    Affine2 t = Affine2::translate({3.0f, 4.0f}) * Affine2::rotate(0.7f) * Affine2::scale({2.0f, 0.5f});
    Affine2 u = t.inverse();
    vec2 p = t.apply({1.5f, -2.0f});
    vec2 q = t.apply_vector({1.5f, -2.0f});
    Affine2 s = Affine2::rotate(1.234f);
    check_bits(r.fs, {t.a, t.b, t.c, t.d, t.tx, t.ty, u.a, u.b, u.c, u.d, u.tx, u.ty, p.x(), p.y(), q.x(), q.y(),
                      t.determinant(), s.a, s.b});
}

TEST_CASE("math: Rng is the engine's Pcg32 and derive is derive_stream") {
    Results r = run_script(R"(
        var rng = Rng.seeded(42)
        ns.push(rng.next_u32() as i64)
        ns.push(rng.next_u32() as i64)
        ns.push(rng.below(10) as i64)
        ns.push(rng.below(1000000) as i64)
        ns.push(rng.range_int(-5, 5) as i64)
        fs.push(rng.next_float())
        fs.push(rng.range(10.0, 20.0))
        var child = Rng.derive(42, 2, 7, 3)
        ns.push(child.next_u32() as i64)
        ns.push(child.next_u32() as i64)
        var big = Rng.seeded(123456789012)
        ns.push(big.next_u32() as i64)
        var gauss = Rng.seeded(7)
        fs.push(gauss.gaussian())
        var circle = Rng.seeded(8)
        var pt = circle.in_circle(2.0)
        fs.push(pt.x)
        fs.push(pt.y)
        var on = circle.on_circle(3.0)
        fs.push(on.x)
        fs.push(on.y)
    )");
    Pcg32 rng(42);
    std::vector<std::int64_t> ns;
    ns.push_back(rng.next_u32());
    ns.push_back(rng.next_u32());
    ns.push_back(rng.next_below(10));
    ns.push_back(rng.next_below(1000000));
    ns.push_back(rng.range(-5, 5));
    // (Pcg32::range over the whole int range draws nothing and returns lo; Rng.range_int draws a full
    // 32-bit value there, so that one call is not compared.)
    const float f0 = rng.next_float();
    const float f1 = rng.range(10.0f, 20.0f);
    Pcg32 child = derive_stream(42, StreamKey{2, 7, 3});
    ns.push_back(child.next_u32());
    ns.push_back(child.next_u32());
    ns.push_back(Pcg32(123456789012ULL).next_u32());
    CHECK(r.ns == ns);

    Pcg32 g(7);
    const float u1 = 1.0f - g.next_float();
    const float u2 = g.next_float();
    const float gauss = 0.0f + 1.0f * (std::sqrt(-2.0f * dmath::log(u1)) * dmath::cos(6.2831855f * u2));
    Pcg32 c(8);
    const float rr = 2.0f * std::sqrt(c.next_float());
    const float aa = 6.2831855f * c.next_float();
    const float ba = 6.2831855f * c.next_float();
    check_bits(r.fs, {f0, f1, gauss, dmath::cos(aa) * rr, dmath::sin(aa) * rr, dmath::cos(ba) * 3.0f,
                      dmath::sin(ba) * 3.0f});
}

TEST_CASE("math: shuffle and the Random helpers draw from the simulation's PCG32") {
    // Seed 7: `shuffle` is Fisher-Yates over next_below, the same stream Random uses.
    Results r = run_script(R"(
        var l = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        l.shuffle()
        for x in l { ns.push(x as i64) }
        ns.push(Random.pick([100, 200, 300, 400]) as i64)
        ns.push(Random.weighted_pick([1.0, 0.0, 3.0]) as i64)
        fs.push(Random.gaussian(5.0, 2.0))
        var p = Random.in_circle(4.0)
        fs.push(p.x)
        fs.push(p.y)
        var on = Random.on_circle()
        fs.push(on.x)
        fs.push(on.y)
    )",
                           7);
    Pcg32 rng(7);
    std::vector<std::int64_t> want;
    std::vector<int> l = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (std::size_t i = l.size(); i > 1; --i) std::swap(l[i - 1], l[rng.next_below(static_cast<std::uint32_t>(i))]);
    for (int x : l) want.push_back(x);
    // range_int(0, 3) is lo + next_below(4).
    want.push_back(100 * (1 + static_cast<std::int64_t>(rng.range(0, 3))));
    // weighted_pick: r = range(0, 1) * total; total = 4.
    const float pick = rng.range(0.0f, 1.0f) * 4.0f;
    want.push_back(pick < 1.0f ? 0 : 2);
    CHECK(r.ns == want);

    const float u1 = 1.0f - rng.range(0.0f, 1.0f);
    const float u2 = rng.range(0.0f, 1.0f);
    const float gauss = 5.0f + 2.0f * (std::sqrt(-2.0f * dmath::log(u1)) * dmath::cos(6.2831855f * u2));
    const float rr = 4.0f * std::sqrt(rng.range(0.0f, 1.0f));
    const float aa = 6.2831855f * rng.range(0.0f, 1.0f);
    const float ba = 6.2831855f * rng.range(0.0f, 1.0f);
    check_bits(r.fs, {gauss, dmath::cos(aa) * rr, dmath::sin(aa) * rr, dmath::cos(ba) * 1.0f, dmath::sin(ba) * 1.0f});
}

TEST_CASE("math: the same seed gives the same script results") {
    const char* body = R"(
        var rng = Rng.seeded(9)
        fs.push(rng.gaussian())
        fs.push(perlin2(1.7, 2.3, 5))
        fs.push(simplex2(1.7, 2.3, 5))
        fs.push(Random.gaussian())
        ns.push(Random.range_int(0, 1000) as i64)
        ns.push(hash2(3, 4) as i64)
    )";
    Results a = run_script(body, 99);
    Results b = run_script(body, 99);
    Results c = run_script(body, 100);
    check_bits(a.fs, b.fs);
    CHECK(a.ns == b.ns);
    CHECK(bits_of(a.fs[3]) != bits_of(c.fs[3]));
    CHECK(bits_of(a.fs[0]) == bits_of(c.fs[0])); // a local Rng ignores the global seed
}

#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2) || defined(CUERVO_SIMD_WASM)
TEST_CASE("math: script sin/cos/atan2 equal dmath scalar and every SIMD lane") {
    Results r = run_script(R"(
        fs.push(sin(0.37))
        fs.push(cos(-2.5))
        fs.push(atan2(0.6, -1.25))
        fs.push(Transform2D.rotate(0.9).a)
    )");
    alignas(32) float xs[simd::width];
    alignas(32) float ys[simd::width];
    alignas(32) float out[simd::width];
    auto lanes = [&](float x, float y, auto fn) {
        for (int i = 0; i < simd::width; ++i) {
            xs[i] = x;
            ys[i] = y;
        }
        simd::store(out, fn(simd::load(ys), simd::load(xs)));
        for (int i = 0; i < simd::width; ++i) CHECK(bits_of(out[i]) == bits_of(out[0]));
        return out[0];
    };
    check_bits(r.fs, {dmath::sin(0.37f), dmath::cos(-2.5f), dmath::atan2(0.6f, -1.25f), dmath::cos(0.9f)});
    CHECK(bits_of(lanes(0.37f, 0.0f, [](auto, auto x) { return dmath::simd::sin(x); })) == bits_of(r.fs[0]));
    CHECK(bits_of(lanes(-2.5f, 0.0f, [](auto, auto x) { return dmath::simd::cos(x); })) == bits_of(r.fs[1]));
    CHECK(bits_of(lanes(-1.25f, 0.6f, [](auto y, auto x) { return dmath::simd::atan2(y, x); })) == bits_of(r.fs[2]));
    CHECK(bits_of(lanes(0.9f, 0.0f, [](auto, auto x) { return dmath::simd::cos(x); })) == bits_of(r.fs[3]));
}
#endif

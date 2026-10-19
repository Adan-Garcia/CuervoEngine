// Milestone 9.2b (DESIGN.md 4.17, LANGUAGE.md 9.20 - 9.24): scalar, integer
// and vector math, Rect2, Transform2D, geometry, noise and seeded random
// streams, in the VM. AOT parity of the same programs is in aot_tests.cpp.

#include <doctest.h>

#include "tests/pluma/math_program.hpp"
#include "tests/pluma/math_run.hpp"

using test::math_panics;
using test::math_prints;
using test::run_math;

TEST_CASE("math: the 9.2b exact-operation program prints its golden output") {
    test::Ran r = run_math(test::math_exact_program);
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    CHECK_MESSAGE(r.panic.empty(), r.panic);
    CHECK(r.out == test::math_exact_expected);
}

TEST_CASE("math: the transcendental program runs") {
    test::Ran r = run_math(test::math_transcendental_program);
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    CHECK_MESSAGE(r.panic.empty(), r.panic);
    CHECK_FALSE(r.out.empty());
}

TEST_CASE("math: the math prelude compiles with nothing else") {
    pluma::Session session({}, &test::math_bindings());
    CHECK_MESSAGE(session.prelude_result().ok, session.prelude_result().rendered);
}

TEST_CASE("math: scalar edge cases") {
    math_prints("print(trunc(-0.5))\nprint(fposmod(5.0, -3.0))\nprint(fposmod(-6.0, 3.0))", "-0.0\n-1.0\n0.0\n");
    math_prints("print(hypot(INF, NAN))\nprint(is_nan(hypot(NAN, 1.0)))\nprint(hypot(-3.0, -4.0))", "inf\ntrue\n5.0\n");
    math_prints("print(copysign(1.0, -1.0))\nprint(copysign(0.0, -1.0))\nprint(copysign(-0.0, 1.0))",
                "-1.0\n-0.0\n0.0\n");
    math_prints("print(pingpong(5.0, 0.0))\nprint(snapped(0.25, 0.5))\nprint(snapped(-0.25, 0.5))", "0.0\n0.5\n0.0\n");
    math_prints("print(NAN == NAN)\nprint(INF > 3.4e38)\nprint(-INF < -3.4e38)", "false\ntrue\ntrue\n");
    math_prints("print(is_equal_approx(0.0, 0.0000001))\nprint(is_equal_approx(1000000.0, 1000005.0))\n"
                "print(is_equal_approx(1.0, 1.5, 1.0))",
                "true\ntrue\ntrue\n");
}

TEST_CASE("math: ease curves hit 0 and 1 and are monotone on the smooth families") {
    math_prints(R"(
var kinds = [Ease.Sine, Ease.Quad, Ease.Cubic, Ease.Quart, Ease.Quint, Ease.Expo, Ease.Circ, Ease.Back,
             Ease.Elastic, Ease.Bounce]
var ok = true
for k in kinds {
    if !is_equal_approx(ease_in(k, 0.0), 0.0, 0.0001) { ok = false }
    if !is_equal_approx(ease_in(k, 1.0), 1.0, 0.0001) { ok = false }
    if !is_equal_approx(ease_out(k, 0.0), 0.0, 0.0001) { ok = false }
    if !is_equal_approx(ease_out(k, 1.0), 1.0, 0.0001) { ok = false }
    if !is_equal_approx(ease_in_out(k, 0.0), 0.0, 0.0001) { ok = false }
    if !is_equal_approx(ease_in_out(k, 1.0), 1.0, 0.0001) { ok = false }
    if !is_equal_approx(ease_in_out(k, 0.5), 0.5, 0.0001) { ok = false }
}
print(ok)
)",
                "true\n");
}

TEST_CASE("math: integer functions panic where documented") {
    math_panics("print(ipow(2, 31))", "integer overflow in 'ipow'");
    math_panics("print(ipow(2, -1))", "ipow: the exponent -1 is negative");
    math_panics("print(isqrt(-4))", "isqrt: -4 is negative");
    math_panics("print(lcm(2000000000, 3))", "lcm(2000000000, 3) does not fit in 'int'");
    math_panics("print(divmod(1, 0).quot)", "division by zero");
    math_panics("print(floor_mod(1, 0))", "division by zero");
}

TEST_CASE("math: arithmetic builtins follow the operand type") {
    math_prints("print(checked_add(100i8, 27i8))\nprint(checked_add(100i8, 28i8))\nprint(saturating_add(100i8, 28i8))\n"
                "print(wrapping_add(100i8, 28i8))\nprint(saturating_sub(-100i8, 100i8))\nprint(checked_mul(16u8, 16u8))\n"
                "print(wrapping_mul(16u8, 17u8))\nprint(saturating_mul(16u8, 17u8))",
                "127\nnull\n127\n-128\n-128\nnull\n16\n255\n");
    test::rejects("print(checked_add(1.5, 2.5))", "1:7: 'checked_add' is not defined for 'float'\n");
}

TEST_CASE("math: v[i] = e") {
    math_prints("var v = vec4(0.0)\nfor i in 0..4 { v[i] = (i * 2) as float }\nv[3] *= 2.0\nprint(v)",
                "vec4(0.0, 2.0, 4.0, 12.0)\n");
    // The index is evaluated once, even for a compound assignment.
    math_prints(R"(
var calls = 0
fn next() -> int {
    calls += 1
    return 1
}
var v = vec2(1.0, 2.0)
v[next()] += 10.0
print(v)
print(calls)
)",
                "vec2(1.0, 12.0)\n1\n");
    // A component of a vector held in a struct field or a list.
    math_prints(R"(
struct Body { var vel = vec2(0.0, 0.0) }
var b = Body {}
b.vel[1] = 3.0
var l = List<vec3>()
l.push(vec3(0.0))
l[0][2] = 7.0
print(b.vel)
print(l[0])
)",
                "vec2(0.0, 3.0)\nvec3(0.0, 0.0, 7.0)\n"
                );
    math_panics("var v = vec2(0.0, 0.0)\nvar i = 2\nv[i] = 1.0", "index 2 is out of bounds for a vec2");
    test::rejects("fn f(v: vec2) {\n    v[0] = 1.0\n}",
                  "2:5: cannot assign to a component of an immutable vector (a parameter, constant or temporary); "
                  "copy it into a 'var'\n");
}

TEST_CASE("math: vector methods keep their size and zero cases") {
    math_prints("print(vec3(0.0).direction_to(vec3(0.0)))\nprint(vec2(2.0, 0.0).limit_length(0.0))\n"
                "print(vec2(-0.4, 0.4).round())\nprint(vec2(5.0, 5.0).snapped(0.0))",
                "vec3(0.0, 0.0, 0.0)\nvec2(0.0, 0.0)\nvec2(-0.0, 0.0)\nvec2(5.0, 5.0)\n");
}

TEST_CASE("math: ivecN are structs with operators") {
    math_prints("var a = ivec3.of(1, 2, 3)\nprint(a * a - a + a.scaled(2))\nprint(-a == ivec3.of(-1, -2, -3))",
                "ivec3 { x = 2, y = 6, z = 12 }\ntrue\n");
    test::rejects("var v: ivec8", "1:8: unknown type 'ivec8'\n");
}

TEST_CASE("math: geometry on degenerate input") {
    math_prints(R"(
var none = List<vec2>()
print(polygon_area(none))
print(point_in_polygon(vec2(0.0, 0.0), none))
print(triangulate(none))
print(convex_hull(none))
var one = [vec2(1.0, 1.0), vec2(1.0, 1.0)]
print(convex_hull(one))
var line = [vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(2.0, 2.0)]
print(convex_hull(line).len())
print(triangulate(line))
)",
                "0.0\nfalse\n[]\n[]\n[vec2(1.0, 1.0)]\n2\n[]\n");
}

TEST_CASE("math: Rng is reproducible and independent of the global stream") {
    math_prints(R"(
var a = Rng.seeded(5)
var b = Rng.seeded(5)
var same = true
for i in 0..50 {
    if a.next_u32() != b.next_u32() { same = false }
}
print(same)
var c = Rng.seeded(6)
print(a.next_u32() != c.next_u32())
var xs = List<int>()
for i in 0..20 { xs.push(i) }
var ys = xs.slice(0)
Rng.seeded(1).shuffle(xs)
Rng.seeded(1).shuffle(ys)
print(xs == ys)
print(xs.sum())
)",
                "true\ntrue\nfalse\n190\n");
}

TEST_CASE("math: shuffle draws from VmOptions::random_below") {
    pluma::VmOptions o;
    int draws = 0;
    o.random_below = [&draws](std::uint32_t bound) {
        ++draws;
        return bound - 1; // always swap with the last element: a rotation
    };
    test::Ran r = run_math("var l = [1, 2, 3, 4]\nl.shuffle()\nprint(l)", o);
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    CHECK(r.out == "[1, 2, 3, 4]\n");
    CHECK(draws == 3);
}

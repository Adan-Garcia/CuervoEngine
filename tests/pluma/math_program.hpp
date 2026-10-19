#pragma once

// The Milestone 9.2b "done when" programs (DESIGN.md 6): scripts that call
// every function of the math library (pluma/std/math.plm) and the new
// built-ins and print the results. math_tests.cpp checks the exact-operation
// program against golden output in the VM; aot_tests.cpp runs both programs
// in the VM and in AOT-to-C and requires identical output (every float
// prints with its shortest round-trip digits, so equal text is equal bits).
// The engine-side check against dmath is tests/cuervo/math_tests.cpp.

namespace test {

// Only exact operations (no sin/cos/exp/log/pow): golden output is portable.
inline constexpr const char* math_exact_program = R"PLM(
// ---- scalar
print(trunc(-2.7))
print(trunc(2.7))
print(fract(-2.75))
print(fract(2.25))
print(fposmod(-1.0, 3.0))
print(fposmod(7.5, 2.0))
print(hypot(3.0, 4.0))
print(hypot(3.0e30, 4.0e30))
print(hypot(0.0, 0.0))
print(copysign(3.0, -0.0))
print(copysign(-3.0, 1.0))
print(snapped(7.3, 0.5))
print(snapped(-7.3, 2.0))
print(snapped(1.7, 0.0))
print(pingpong(7.0, 3.0))
print(pingpong(-1.0, 2.0))
print(angle_difference(3.0, -3.0))
print(lerp_angle(3.0, -3.0, 0.5))
print(rotate_toward(0.0, 1.0, 0.25))
print(rotate_toward(0.0, 1.0, 5.0))
print(is_nan(NAN))
print(is_nan(1.0))
print(is_inf(-INF))
print(is_inf(1.0))
print(is_finite(1.0))
print(is_finite(INF))
print(is_finite(NAN))
print(is_equal_approx(1.0, 1.000001))
print(is_equal_approx(1.0, 1.1))
print(bezier(0.0, 0.0, 1.0, 1.0, 0.25))
print(bezier_quad(0.0, 1.0, 0.0, 0.5))
print(bezier_vec2(vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0), 0.5))
print(bezier_quad_vec2(vec2(0.0, 0.0), vec2(1.0, 2.0), vec2(2.0, 0.0), 0.5))
print(ease_in(Ease.Quad, 0.5))
print(ease_out(Ease.Cubic, 0.5))
print(ease_in_out(Ease.Quart, 0.25))
print(ease_in(Ease.Quint, 0.5))
print(ease_in(Ease.Circ, 0.5))
print(ease_in(Ease.Back, 0.5))
print(ease_out(Ease.Bounce, 0.3))
print(ease_in_out(Ease.Bounce, 0.8))
print(PI)
print(TAU)
print(E)
print(SQRT2)
print(INF)
print(FLOAT_EPSILON)
print(INT_MAX)
print(INT_MIN)

// ---- integer
print(gcd(12, -18))
print(gcd(0, 5))
print(gcd(0, 0))
print(lcm(4, 6))
print(lcm(-3, 7))
print(lcm(0, 7))
print(ipow(3, 5))
print(ipow(-2, 31))
print(ipow(7, 0))
print(ipow(2, 30))
print(isqrt(0))
print(isqrt(99))
print(isqrt(100))
print(isqrt(2147483647))
var dm = divmod(-7, 2)
print("{dm.quot} {dm.rem}")
print(floor_div(-7, 2))
print(floor_mod(-7, 2))
print(floor_div(7, -2))
print(floor_mod(7, -2))
print(floor_mod(6, 3))
print(popcount(0))
print(popcount(-1))
print(popcount(255))
print(clz(1))
print(clz(0))
print(clz(-1))
print(ctz(8))
print(ctz(0))
print(ctz(-2147483648))
print(checked_add(2147483647, 1))
print(checked_add(5, 1))
print(checked_sub(-2147483648, 1))
print(checked_sub(5, 7))
print(checked_mul(65536, 65536))
print(checked_mul(-3, 7))
print(saturating_add(2147483647, 5))
print(saturating_add(-2147483648, -5))
print(saturating_sub(-2147483648, 5))
print(saturating_sub(2147483647, -5))
print(saturating_mul(100000, -100000))
print(saturating_mul(100000, 100000))
print(saturating_mul(3, 4))
print(wrapping_add(2147483647, 1))
print(wrapping_sub(-2147483648, 1))
print(wrapping_mul(65536, 65536))
print(wrapping_mul(123456789, 987654321))
print(wrapping_sub(0u8, 1u8))
print(saturating_sub(3u8, 5u8))
print(saturating_add(250u8, 10u8))
print(wrapping_add(255u8, 2u8))
print(checked_add(250u8, 10u8))
print(saturating_add(9223372036854775807i64, 1i64))
print(wrapping_add(9223372036854775807i64, 1i64))

// ---- vectors
var v3 = vec3(1.0, 2.0, 3.0)
v3[1] = 9.0
v3[2] += 0.5
var vi = 0
v3[vi] -= 1.0
print(v3)
var v2 = vec2(1.25, -2.5)
print(v2.floor())
print(v2.ceil())
print(v2.round())
print(v2.fract())
print(v2.sign())
print(vec2(1.3, -2.6).snapped(0.5))
print(vec2(3.0, 4.0).limit_length(2.5))
print(vec2(3.0, 4.0).limit_length(10.0))
print(vec2(0.0, 0.0).move_toward(vec2(3.0, 4.0), 2.5))
print(vec2(0.0, 0.0).move_toward(vec2(3.0, 4.0), 9.0))
print(vec2(1.0, 1.0).direction_to(vec2(1.0, 1.0)))
print(vec2(0.0, 0.0).direction_to(vec2(0.0, 5.0)))
print(vec2(1.0, -1.0).bounce(vec2(0.0, 1.0)))
print(vec4(1.5, -0.5, 2.5, -3.5).round())
var cells = List<vec2>()
cells.push(vec2(1.0, 2.0))
cells[0][1] = 5.0
print(cells)

// ---- integer vectors
var ia = ivec2.of(1, 2)
var ib = ivec2.of(3, -4)
print(ia + ib * ia)
print(ia - ib)
print(-ia)
print(ib.scaled(3))
print(ia.dot(ib))
print(ib.length_squared())
print(ib.abs())
print(ia.min(ib))
print(ia.max(ib))
print(ivec2.of(9, -9).clamp(ivec2.of(-1, -1), ivec2.of(5, 5)))
print(ivec2.floor_of(vec2(1.5, -0.5)))
print(ia.to_vec())
print(ia == ivec2.of(1, 2))
var i3 = ivec3.of(1, 2, 3)
print(i3 + i3)
print(i3.dot(i3))
print(ivec3.floor_of(vec3(0.5, -0.5, 2.5)))
var i4 = ivec4.of(1, -2, 3, -4)
print(i4.abs())
print(i4.to_vec())
print(i4.max(ivec4.of(0, 0, 0, 0)))

// ---- Rect2
var r1 = Rect2.of(0.0, 0.0, 10.0, 10.0)
var r2 = Rect2.of(5.0, 5.0, 10.0, 10.0)
print(r1.end())
print(r1.center())
print(r1.area())
print(r1.has_area())
print(r1.contains(vec2(10.0, 5.0)))
print(r1.contains(vec2(0.0, 0.0)))
print(r1.intersects(r2))
print(r1.intersects(Rect2.of(10.0, 0.0, 5.0, 5.0)))
print(r1.intersection(r2))
print(r1.intersection(Rect2.of(20.0, 20.0, 1.0, 1.0)).has_area())
print(r1.merge(r2))
print(r1.expand(vec2(-5.0, 20.0)))
print(r1.grow(2.0))
print(Rect2.of(10.0, 10.0, -4.0, -6.0).abs())
print(Rect2.from_corners(vec2(5.0, 1.0), vec2(2.0, 8.0)))
print(r1.encloses(Rect2.of(1.0, 1.0, 2.0, 2.0)))
print(r1.encloses(r2))

// ---- Transform2D
var t1 = Transform2D.translate(vec2(10.0, 0.0)) * Transform2D.scale(vec2(2.0, 3.0))
print(t1.apply(vec2(1.0, 1.0)))
print(t1.apply_vector(vec2(1.0, 1.0)))
print(t1.determinant())
print(t1.inverse().apply(t1.apply(vec2(4.0, 5.0))))
print(Transform2D.scale(vec2(0.0, 1.0)).inverse() == Transform2D.identity())
print(t1.origin())
print(Transform2D.identity() == Transform2D {})
print((t1 * t1).apply(vec2(1.0, 0.0)))

// ---- geometry
var sq = List<vec2>()
sq.push(vec2(0.0, 0.0))
sq.push(vec2(4.0, 0.0))
sq.push(vec2(4.0, 4.0))
sq.push(vec2(0.0, 4.0))
var ell = List<vec2>()
ell.push(vec2(0.0, 0.0))
ell.push(vec2(4.0, 0.0))
ell.push(vec2(4.0, 2.0))
ell.push(vec2(2.0, 2.0))
ell.push(vec2(2.0, 4.0))
ell.push(vec2(0.0, 4.0))
print(polygon_area(sq))
print(polygon_signed_area(sq))
print(polygon_area(ell))
print(point_in_polygon(vec2(1.0, 1.0), sq))
print(point_in_polygon(vec2(5.0, 1.0), sq))
print(point_in_polygon(vec2(3.0, 3.0), ell))
print(point_in_polygon(vec2(1.0, 3.0), ell))
print(segment_intersection(vec2(0.0, 0.0), vec2(4.0, 4.0), vec2(0.0, 4.0), vec2(4.0, 0.0)))
print(segment_intersection(vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 4.0), vec2(4.0, 0.0)))
print(segment_intersection(vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0)))
print(ray_circle_intersection(vec2(-5.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 0.0), 2.0))
print(ray_circle_intersection(vec2(-5.0, 3.0), vec2(1.0, 0.0), vec2(0.0, 0.0), 2.0))
print(ray_circle_intersection(vec2(5.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 0.0), 2.0))
print(ray_circle_intersection(vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 0.0), 2.0))
print(closest_point_on_segment(vec2(2.0, 5.0), vec2(0.0, 0.0), vec2(4.0, 0.0)))
print(closest_point_on_segment(vec2(-3.0, 1.0), vec2(0.0, 0.0), vec2(4.0, 0.0)))
print(closest_point_on_segment(vec2(1.0, 1.0), vec2(2.0, 2.0), vec2(2.0, 2.0)))
var cloud = List<vec2>()
for p in sq { cloud.push(p) }
cloud.push(vec2(2.0, 2.0))
cloud.push(vec2(2.0, 0.0))
cloud.push(vec2(0.0, 0.0))
print(convex_hull(cloud))
print(convex_hull(ell).len())
print(triangulate(sq))
print(triangulate(ell))
var cw = List<vec2>()
var k = ell.len() - 1
while k >= 0 {
    cw.push(ell[k])
    k -= 1
}
print(triangulate(cw))
var tri_area = 0.0
var tris = triangulate(ell)
var ti = 0
while ti < tris.len() {
    var ta = ell[tris[ti]]
    var tb = ell[tris[ti + 1]]
    var tc = ell[tris[ti + 2]]
    tri_area += cross3(ta, tb, tc) * 0.5
    ti += 3
}
print(tri_area)

// ---- hashing and noise (exact: floor and + - * / only)
print(hash(1))
print(hash(1, 7))
print(hash2(1, 2))
print(hash3(1, 2, 3))
print(hash_str("hello"))
print(hash_str(""))
print(hash_float(5))
print(perlin1(0.5))
print(perlin1(3.0))
print(perlin2(0.3, 0.7))
print(perlin2(2.0, 3.0))
print(perlin2(-1.5, 2.25, 9))
print(perlin3(0.3, 0.7, 0.1))
print(simplex2(0.3, 0.7))
print(simplex2(-4.2, 11.9, 3))
print(fractal_perlin2(0.3, 0.7))
print(fractal_perlin2(5.5, -2.5, 6, 2.0, 0.5, 4))

// ---- Rng (PCG32; exact integer arithmetic)
var rng = Rng.seeded(42)
print(rng.next_u32())
print(rng.next_u32())
print(rng.below(10))
print(rng.below(1000000))
print(rng.range_int(-5, 5))
print(rng.range_int(-2147483648, 2147483647))
print(rng.next_float())
print(rng.range(10.0, 20.0))
print(rng.chance(0.5))
var order = [1, 2, 3, 4, 5, 6, 7, 8]
rng.shuffle(order)
print(order)
print(rng.pick(order))
print(rng.pick(["a", "b", "c"]))
print(rng.weighted_pick([0.0, 1.0, 3.0, -2.0]))
var child_a = Rng.derive(42, 1, 5, 0)
var child_b = Rng.derive(42, 1, 5, 1)
var child_c = Rng.derive(42, 1, 5, 0)
print(child_a.next_u32())
print(child_b.next_u32())
print(child_c.next_u32())
var rng2 = Rng.seeded(42)
print(rng2.next_u32())
)PLM";

// Output of math_exact_program (reviewed by hand: each value was checked against its definition).
inline constexpr const char* math_exact_expected = R"PLM(-2.0
2.0
0.25
0.25
2.0
1.5
5.0
5e+30
0.0
-3.0
3.0
7.5
-8.0
1.7
1.0
1.0
0.28318548
3.1415927
0.25
1.0
true
false
true
false
true
false
false
true
false
0.15625
0.5
vec2(0.5, 0.75)
vec2(1.0, 1.0)
0.25
0.875
0.03125
0.03125
0.13397461
-0.087697506
0.6806251
0.88625
3.1415927
6.2831855
2.7182817
1.4142135
inf
1.1920929e-07
2147483647
-2147483648
6
5
0
12
21
0
243
-2147483648
1
1073741824
0
9
10
46340
-3 -1
-4
1
-4
-1
0
0
32
8
31
32
0
3
32
31
null
6
null
-2
null
-21
2147483647
-2147483648
-2147483648
2147483647
-2147483648
2147483647
12
-2147483648
2147483647
0
-67153019
255
0
255
1
null
9223372036854775807
-9223372036854775808
vec3(0.0, 9.0, 3.5)
vec2(1.0, -3.0)
vec2(2.0, -2.0)
vec2(1.0, -3.0)
vec2(0.25, 0.5)
vec2(1.0, -1.0)
vec2(1.5, -2.5)
vec2(1.5, 2.0)
vec2(3.0, 4.0)
vec2(1.5, 2.0)
vec2(3.0, 4.0)
vec2(0.0, 0.0)
vec2(0.0, 1.0)
vec2(-1.0, -1.0)
vec4(2.0, -1.0, 3.0, -4.0)
[vec2(1.0, 5.0)]
ivec2 { x = 4, y = -6 }
ivec2 { x = -2, y = 6 }
ivec2 { x = -1, y = -2 }
ivec2 { x = 9, y = -12 }
-5
25
ivec2 { x = 3, y = 4 }
ivec2 { x = 1, y = -4 }
ivec2 { x = 3, y = 2 }
ivec2 { x = 5, y = -1 }
ivec2 { x = 1, y = -1 }
vec2(1.0, 2.0)
true
ivec3 { x = 2, y = 4, z = 6 }
14
ivec3 { x = 0, y = -1, z = 2 }
ivec4 { x = 1, y = 2, z = 3, w = 4 }
vec4(1.0, -2.0, 3.0, -4.0)
ivec4 { x = 1, y = 0, z = 3, w = 0 }
vec2(10.0, 10.0)
vec2(5.0, 5.0)
100.0
true
false
true
true
false
Rect2 { pos = vec2(5.0, 5.0), size = vec2(5.0, 5.0) }
false
Rect2 { pos = vec2(0.0, 0.0), size = vec2(15.0, 15.0) }
Rect2 { pos = vec2(-5.0, 0.0), size = vec2(15.0, 20.0) }
Rect2 { pos = vec2(-2.0, -2.0), size = vec2(14.0, 14.0) }
Rect2 { pos = vec2(6.0, 4.0), size = vec2(4.0, 6.0) }
Rect2 { pos = vec2(2.0, 1.0), size = vec2(3.0, 7.0) }
true
false
vec2(12.0, 3.0)
vec2(2.0, 3.0)
6.0
vec2(4.0, 5.0)
true
vec2(10.0, 0.0)
true
vec2(34.0, 0.0)
16.0
16.0
12.0
true
false
false
true
vec2(2.0, 2.0)
null
null
3.0
null
null
0.0
vec2(2.0, 0.0)
vec2(0.0, 0.0)
vec2(2.0, 2.0)
[vec2(0.0, 0.0), vec2(4.0, 0.0), vec2(4.0, 4.0), vec2(0.0, 4.0)]
5
[3, 0, 1, 1, 2, 3]
[0, 1, 2, 0, 2, 3, 5, 0, 3, 3, 4, 5]
[5, 4, 3, 5, 3, 2, 0, 5, 2, 2, 1, 0]
12.0
-232234301
-816452278
1113887585
495982689
-2003995026
-1421968373
0.5194062
1.0
-0.0
0.10918793
0.0
-0.008544922
-0.24528246
0.8602344
0.3029835
0.0013163914
-0.06349207
1898997482
1014631766
4
901381
-2
-1865419252
0.32107574
13.276327
true
[3, 5, 4, 2, 6, 7, 8, 1]
4
c
2
3396514171
863722505
3396514171
1898997482
)PLM";

// Transcendental functions (through the host's math hooks): the VM and AOT
// outputs must be identical to each other; tests/cuervo/math_tests.cpp checks
// the engine-hooked values against dmath.
inline constexpr const char* math_transcendental_program = R"PLM(
print(sinh(0.05))
print(sinh(-2.0))
print(cosh(2.0))
print(cosh(0.0))
print(tanh(0.05))
print(tanh(0.5))
print(tanh(20.0))
print(asinh(2.0))
print(asinh(-0.5))
print(acosh(2.0))
print(is_nan(acosh(0.5)))
print(atanh(0.5))
print(atanh(-0.25))
print(cbrt(27.0))
print(cbrt(-8.0))
print(cbrt(0.0))
print(cbrt(2.0))
print(cbrt(1000.0))
print(cbrt(0.001))
print(cbrt(-1.0e30))
print(ease_in(Ease.Sine, 0.5))
print(ease_out(Ease.Sine, 0.5))
print(ease_in_out(Ease.Sine, 0.3))
print(ease_in(Ease.Expo, 0.5))
print(ease_out(Ease.Expo, 0.5))
print(ease_in_out(Ease.Expo, 0.7))
print(ease_in(Ease.Elastic, 0.3))
print(ease_out(Ease.Elastic, 0.3))
print(ease_in_out(Ease.Elastic, 0.9))
print(ease(0.5, 2.0))
print(ease(0.5, 0.5))
print(ease(0.25, -2.0))
print(ease(0.75, -2.0))
print(ease(0.5, 0.0))
var rg = Rng.seeded(7)
print(rg.gaussian())
print(rg.gaussian(10.0, 2.0))
print(rg.in_circle())
print(rg.in_circle(5.0))
print(rg.on_circle())
print(rg.on_circle(3.0))
var sum = 0.0
var sumsq = 0.0
for i in 0..400 {
    var g = rg.gaussian()
    sum += g
    sumsq += g * g
}
print(sum / 400.0)
print(sumsq / 400.0)
var inside = 0
for i in 0..400 {
    if rg.in_circle(1.0).length() <= 1.0 { inside += 1 }
}
print(inside)
print(Transform2D.rotate(1.5707964).apply(vec2(1.0, 0.0)))
print(Transform2D.rotate(0.5).rotation())
print((Transform2D.translate(vec2(3.0, 4.0)) * Transform2D.rotate(0.7) * Transform2D.scale(vec2(2.0, 0.5))).inverse())
print(Transform2D.from_trs(vec2(1.0, 2.0), 0.3, vec2(2.0, 2.0)))
)PLM";

} // namespace test

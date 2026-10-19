#include <doctest.h>

#include "cuervo/math/vec.hpp"

using namespace cuervo;

TEST_CASE("Vec construction") {
    CHECK(vec2().x() == 0.0f);
    CHECK(vec3(2.0f) == vec3(2.0f, 2.0f, 2.0f));
    CHECK(vec1(5.0f)[0] == 5.0f);

    vec3 extended(vec2(1.0f, 2.0f), 3.0f);
    CHECK(extended == vec3(1.0f, 2.0f, 3.0f));

    vecn<7> wide(1.0f);
    CHECK(wide.size() == 7);
    CHECK(wide[6] == 1.0f);
}

TEST_CASE("Vec accessors and swizzles") {
    vec4 v(1.0f, 2.0f, 3.0f, 4.0f);
    CHECK(v.x() == 1.0f);
    CHECK(v.w() == 4.0f);
    CHECK(v.yx() == vec2(2.0f, 1.0f));
    CHECK(v.xyz() == vec3(1.0f, 2.0f, 3.0f));
    CHECK(v.swizzle<3, 3, 0>() == vec3(4.0f, 4.0f, 1.0f));

    v.y() = 9.0f;
    CHECK(v[1] == 9.0f);
}

TEST_CASE("Vec arithmetic") {
    vec2 a(1.0f, 2.0f);
    vec2 b(3.0f, 5.0f);
    CHECK(a + b == vec2(4.0f, 7.0f));
    CHECK(b - a == vec2(2.0f, 3.0f));
    CHECK(a * b == vec2(3.0f, 10.0f));
    CHECK(a * 2.0f == vec2(2.0f, 4.0f));
    CHECK(2.0f * a == vec2(2.0f, 4.0f));
    CHECK(b / 2.0f == vec2(1.5f, 2.5f));
    CHECK(-a == vec2(-1.0f, -2.0f));

    ivec3 i(1, 2, 3);
    i += ivec3(1);
    CHECK(i == ivec3(2, 3, 4));
}

TEST_CASE("Vec geometry") {
    CHECK(dot(vec3(1.0f, 2.0f, 3.0f), vec3(4.0f, 5.0f, 6.0f)) == 32.0f);
    CHECK(length(vec2(3.0f, 4.0f)) == 5.0f);
    CHECK(normalize(vec2(0.0f, 10.0f)) == vec2(0.0f, 1.0f));
    CHECK(normalize(vec2()) == vec2()); // no NaNs for zero vectors
    CHECK(lerp(vec2(0.0f), vec2(10.0f), 0.25f) == vec2(2.5f));
    CHECK(min(ivec2(1, 5), ivec2(3, 2)) == ivec2(1, 2));
    CHECK(max(ivec2(1, 5), ivec2(3, 2)) == ivec2(3, 5));
}

TEST_CASE("Vec 2D and 3D cross products") {
    CHECK(cross(vec2(1.0f, 0.0f), vec2(0.0f, 1.0f)) == 1.0f);
    CHECK(cross(vec2(0.0f, 1.0f), vec2(1.0f, 0.0f)) == -1.0f);
    CHECK(cross(vec2(2.0f, 3.0f), vec2(4.0f, 5.0f)) == -2.0f);

    vec3 i(1.0f, 0.0f, 0.0f);
    vec3 j(0.0f, 1.0f, 0.0f);
    vec3 k(0.0f, 0.0f, 1.0f);
    CHECK(cross(i, j) == k);
    CHECK(cross(j, k) == i);
    CHECK(cross(k, i) == j);
    CHECK(cross(j, i) == -k);
}

TEST_CASE("Vec distance and distance_sq") {
    vec2 a(1.0f, 2.0f);
    vec2 b(4.0f, 6.0f);
    CHECK(distance(a, b) == 5.0f);
    CHECK(distance_sq(a, b) == 25.0f);
}

TEST_CASE("Vec reflect, project, slide") {
    vec2 v(1.0f, -1.0f);
    vec2 normal(0.0f, 1.0f);
    CHECK(reflect(v, normal) == vec2(1.0f, 1.0f));

    CHECK(project(vec2(3.0f, 4.0f), vec2(2.0f, 0.0f)) == vec2(3.0f, 0.0f));
    CHECK(project(vec2(3.0f, 4.0f), vec2(0.0f, 0.0f)) == vec2(0.0f, 0.0f));

    CHECK(slide(vec2(3.0f, 5.0f), vec2(1.0f, 0.0f)) == vec2(0.0f, 5.0f));
}

TEST_CASE("Vec 2D perpendicular and angle_to") {
    vec2 v(3.0f, 4.0f);
    vec2 perp = perpendicular(v);
    CHECK(dot(v, perp) == 0.0f);
    CHECK(perp == vec2(-4.0f, 3.0f));

    vec2 right(1.0f, 0.0f);
    vec2 up(0.0f, 1.0f);
    CHECK(angle_to(right, up) == doctest::Approx(dmath::half_pi).epsilon(1e-6));
    CHECK(angle_to(right, right) == 0.0f);
    CHECK(angle_to(up, right) == doctest::Approx(-dmath::half_pi).epsilon(1e-6));
}

TEST_CASE("Vec clamp and clamp_length") {
    vec2 v(-5.0f, 15.0f);
    CHECK(clamp(v, vec2(0.0f, 0.0f), vec2(10.0f, 10.0f)) == vec2(0.0f, 10.0f));

    vec2 long_v(30.0f, 40.0f); // length 50
    vec2 clamped = clamp_length(long_v, 5.0f, 10.0f);
    CHECK(length(clamped) == doctest::Approx(10.0f).epsilon(1e-6));
    CHECK(clamped == vec2(6.0f, 8.0f));

    vec2 short_v(0.6f, 0.8f); // length 1
    vec2 extended = clamp_length(short_v, 5.0f, 10.0f);
    CHECK(length(extended) == doctest::Approx(5.0f).epsilon(1e-6));
}

TEST_CASE("slerp: 2D spherical linear arc interpolation") {
    vec2 right(10.0f, 0.0f);
    vec2 up(0.0f, 10.0f);

    vec2 at_0 = slerp(right, up, 0.0f);
    CHECK(at_0.x() == doctest::Approx(10.0f).epsilon(1e-5));
    CHECK(at_0.y() == doctest::Approx(0.0f).epsilon(1e-5));

    vec2 at_1 = slerp(right, up, 1.0f);
    CHECK(at_1.x() == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(at_1.y() == doctest::Approx(10.0f).epsilon(1e-5));

    vec2 mid = slerp(right, up, 0.5f);
    CHECK(length(mid) == doctest::Approx(10.0f).epsilon(1e-5));
    CHECK(mid.x() == doctest::Approx(mid.y()).epsilon(1e-5));

    // Parallel vectors
    vec2 v(3.0f, 4.0f);
    vec2 p = slerp(v, v, 0.5f);
    CHECK(p.x() == doctest::Approx(3.0f).epsilon(1e-5));
    CHECK(p.y() == doctest::Approx(4.0f).epsilon(1e-5));
}

static_assert(sizeof(vec2) == 2 * sizeof(float), "Vec must have no padding");
static_assert(sizeof(vecn<5>) == 5 * sizeof(float));
static_assert(vec3(1.0f, 2.0f, 3.0f).swizzle<2, 1, 0>() == vec3(3.0f, 2.0f, 1.0f), "constexpr swizzle");

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "cuervo/vector/flatten.hpp"
#include "cuervo/vector/path.hpp"
#include "cuervo/vector/stroke.hpp"
#include "cuervo/vector/svg.hpp"
#include "cuervo/vector/tessellate.hpp"

using namespace cuervo;
using namespace cuervo::vector;

namespace {

constexpr double pi = 3.14159265358979323846;

bool near(vec2 a, vec2 b, float eps = 1e-3f) { return std::fabs(a.x() - b.x()) <= eps && std::fabs(a.y() - b.y()) <= eps; }

Path parse(std::string_view d) {
    Path p;
    std::string error;
    REQUIRE_MESSAGE(parse_path_data(d, p, &error), error);
    return p;
}

vec2 cubic_point(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t) {
    const float u = 1.0f - t;
    return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
}

float distance_to_polyline(vec2 p, const std::vector<vec2>& line, bool closed) {
    float best = 1e30f;
    const std::size_t n = line.size();
    for (std::size_t i = 0; i + (closed ? 0 : 1) < n; ++i) {
        const vec2 a = line[i], b = line[(i + 1) % n];
        const vec2 ab = b - a;
        const float t = std::clamp(dot(p - a, ab) / std::max(dot(ab, ab), 1e-20f), 0.0f, 1.0f);
        best = std::min(best, length(p - (a + ab * t)));
    }
    return best;
}

Polyline line_of(std::initializer_list<vec2> points, bool closed = false) { return {points, closed}; }

double fill_area(const std::vector<Polyline>& contours, FillRule rule) {
    TriMesh mesh;
    REQUIRE(tessellate_fill(contours, rule, mesh));
    return mesh.area();
}

double stroke_area(const Polyline& line, const StrokeStyle& style, float tolerance = 0.001f) {
    TriMesh mesh;
    REQUIRE(stroke_to_mesh(std::vector<Polyline>{line}, style, tolerance, mesh));
    return mesh.area();
}

} // namespace

// --- path data ------------------------------------------------------------------

TEST_CASE("Path data: absolute and relative line commands") {
    const Path p = parse("M10 20 L30 40 l10 10 H100 h-5 V0 v5 z");
    REQUIRE(p.verbs == std::vector<Verb>{Verb::MoveTo, Verb::LineTo, Verb::LineTo, Verb::LineTo, Verb::LineTo,
                                         Verb::LineTo, Verb::LineTo, Verb::Close});
    const std::vector<vec2> expected{{10, 20}, {30, 40}, {40, 50}, {100, 50}, {95, 50}, {95, 0}, {95, 5}};
    CHECK(p.points == expected);
    CHECK(p.current_point() == vec2(10, 20)); // Z returns to the subpath start
}

TEST_CASE("Path data: moveto repetition becomes lineto, m after z is relative to the start") {
    const Path p = parse("m10 10 20 0 0 20 z m5 5 l1 1");
    REQUIRE(p.verbs.size() == 6);
    CHECK(p.verbs[1] == Verb::LineTo);
    CHECK(p.points[1] == vec2(30, 10));
    CHECK(p.points[2] == vec2(30, 30));
    CHECK(p.verbs[4] == Verb::MoveTo);
    CHECK(p.points[3] == vec2(15, 15)); // relative to the closed subpath's start (10,10)
    CHECK(p.points[4] == vec2(16, 16));
}

TEST_CASE("Path data: cubic, smooth cubic reflection, relative") {
    const Path p = parse("M0 0 C10 0 20 10 20 20 S30 40 40 40 s10 0 10 10");
    REQUIRE(p.verbs == std::vector<Verb>{Verb::MoveTo, Verb::CubicTo, Verb::CubicTo, Verb::CubicTo});
    // S reflects (20,10) about (20,20) -> (20,30).
    CHECK(p.points[4] == vec2(20, 30));
    CHECK(p.points[5] == vec2(30, 40));
    CHECK(p.points[6] == vec2(40, 40));
    // s: reflection of (30,40) about (40,40) = (50,40); controls relative to (40,40).
    CHECK(p.points[7] == vec2(50, 40));
    CHECK(p.points[8] == vec2(50, 40));
    CHECK(p.points[9] == vec2(50, 50));
}

TEST_CASE("Path data: S without a preceding cubic uses the current point") {
    const Path p = parse("M5 5 S10 0 20 5");
    CHECK(p.points[1] == vec2(5, 5));
}

TEST_CASE("Path data: quadratic and smooth quadratic") {
    const Path p = parse("M0 0 Q10 10 20 0 T40 0 t20 0 q5 5 10 0");
    REQUIRE(p.verbs == std::vector<Verb>{Verb::MoveTo, Verb::QuadTo, Verb::QuadTo, Verb::QuadTo, Verb::QuadTo});
    CHECK(p.points[3] == vec2(30, -10)); // reflected control
    CHECK(p.points[4] == vec2(40, 0));
    CHECK(p.points[5] == vec2(50, 10));  // reflected again
    CHECK(p.points[6] == vec2(60, 0));
    CHECK(p.points[7] == vec2(65, 5));
    CHECK(p.points[8] == vec2(70, 0));
}

TEST_CASE("Path data: compact numbers and arc flags") {
    const Path p = parse("M.5.5L-1-2e1 1e-1,3E+1");
    CHECK(p.points[0] == vec2(0.5f, 0.5f));
    CHECK(p.points[1] == vec2(-1.0f, -20.0f));
    CHECK(near(p.points[2], {0.1f, 30.0f}));
    const Path arc = parse("M0 0a10 10 0 1110 10");
    CHECK(arc.verbs.back() == Verb::CubicTo);
    CHECK(near(arc.current_point(), {10.0f, 10.0f}));
}

TEST_CASE("Path data: errors keep the valid prefix") {
    Path p;
    std::string error;
    CHECK_FALSE(parse_path_data("M0 0 L10 10 L20", p, &error));
    CHECK_FALSE(error.empty());
    CHECK(p.verbs.size() == 2);
    Path q;
    CHECK_FALSE(parse_path_data("L10 10", q, &error)); // must start with a moveto
    Path r;
    CHECK_FALSE(parse_path_data("M0 0 X5", r, &error));
}

// --- arcs -----------------------------------------------------------------------

TEST_CASE("Arc: semicircle endpoints, center and sweep direction") {
    // From (0,0) to (20,0), r = 10. sweep=1 goes through positive-angle
    // direction, which with y down is clockwise on screen: via (10,-10).
    Path cw = parse("M0 0 A10 10 0 0 1 20 0");
    Path ccw = parse("M0 0 A10 10 0 0 0 20 0");
    REQUIRE(cw.verbs.size() == 3); // two 90-degree cubics
    CHECK(near(cw.points[3], {10.0f, -10.0f})); // end of the first cubic = arc midpoint
    CHECK(near(ccw.points[3], {10.0f, 10.0f}));
    CHECK(near(cw.current_point(), {20.0f, 0.0f}, 0.0f));

    // Every point of the cubics is ~10 from the center (10, 0).
    std::vector<Polyline> lines;
    flatten(cw, 0.001f, lines);
    for (vec2 p : lines[0].points) {
        CHECK(std::fabs(length(p - vec2(10, 0)) - 10.0f) < 0.01f);
    }
}

TEST_CASE("Arc: large-arc flag picks the long way round") {
    Path small = parse("M0 0 A10 10 0 0 1 10 10");
    Path large = parse("M0 0 A10 10 0 1 1 10 10");
    CHECK(small.verbs.size() == 2); // 90 degrees: one cubic
    CHECK(large.verbs.size() == 4); // 270 degrees: three cubics
    const Bounds b = large.bounds();
    CHECK(b.size().x() == doctest::Approx(20.0f).epsilon(0.01));
}

TEST_CASE("Arc: radii too small are scaled up; zero radius is a line; same endpoints omit the arc") {
    Path p = parse("M0 0 A1 1 0 0 1 40 0");
    const Bounds b = p.bounds();
    CHECK(b.size().y() == doctest::Approx(20.0f).epsilon(0.01)); // scaled to r = 20, a semicircle
    Path line = parse("M0 0 A0 5 0 0 1 10 10");
    CHECK(line.verbs.back() == Verb::LineTo);
    Path none = parse("M5 5 A5 5 0 0 1 5 5");
    CHECK(none.verbs.size() == 1);
}

TEST_CASE("Arc: rotated ellipse") {
    // Ellipse rx=20, ry=10 rotated 90 degrees: tall. Full ellipse through two arcs.
    Path p = parse("M0 -20 A20 10 90 1 1 0 20 A20 10 90 1 1 0 -20 Z");
    const Bounds b = p.bounds();
    CHECK(b.size().x() == doctest::Approx(20.0f).epsilon(0.01));
    CHECK(b.size().y() == doctest::Approx(40.0f).epsilon(0.01));
}

// --- transforms -----------------------------------------------------------------

TEST_CASE("Transform parsing") {
    Affine2 m;
    REQUIRE(parse_transform("translate(10, 20)", m));
    CHECK(m.apply({1, 1}) == vec2(11, 21));
    REQUIRE(parse_transform("translate(5)", m));
    CHECK(m.apply({0, 0}) == vec2(5, 0));
    REQUIRE(parse_transform("scale(2) translate(1 1)", m)); // translate first, then scale
    CHECK(m.apply({0, 0}) == vec2(2, 2));
    REQUIRE(parse_transform("scale(2, 3)", m));
    CHECK(m.apply({1, 1}) == vec2(2, 3));
    REQUIRE(parse_transform("rotate(90)", m)); // clockwise on a y-down screen
    CHECK(near(m.apply({1, 0}), {0, 1}));
    REQUIRE(parse_transform("rotate(180 5 5)", m));
    CHECK(near(m.apply({0, 0}), {10, 10}));
    REQUIRE(parse_transform("skewX(45)", m));
    CHECK(near(m.apply({0, 1}), {1, 1}));
    REQUIRE(parse_transform("skewY(45)", m));
    CHECK(near(m.apply({1, 0}), {1, 1}));
    REQUIRE(parse_transform("matrix(1 2 3 4 5 6)", m));
    CHECK(m == Affine2{1, 2, 3, 4, 5, 6});
    REQUIRE(parse_transform("  translate(1,1)rotate(0),scale(1)  ", m));
    CHECK_FALSE(parse_transform("translate(1", m));
    CHECK_FALSE(parse_transform("frobnicate(1)", m));
    CHECK_FALSE(parse_transform("matrix(1 2 3)", m));
    CHECK(m == Affine2{}); // invalid -> identity
}

TEST_CASE("Color parsing") {
    Color c;
    REQUIRE(parse_color("#f00", c));
    CHECK(c == Color{1, 0, 0, 1});
    REQUIRE(parse_color("#00FF0080", c));
    CHECK(c.g == 1.0f);
    CHECK(c.a == doctest::Approx(128.0 / 255.0));
    REQUIRE(parse_color("rgb(255, 128, 0)", c));
    CHECK(c.g == doctest::Approx(128.0 / 255.0));
    REQUIRE(parse_color("rgba(100%, 0%, 50%, 0.25)", c));
    CHECK(c == Color{1, 0, 0.5f, 0.25f});
    REQUIRE(parse_color("  CornflowerBlue ", c));
    CHECK(c.b == doctest::Approx(237.0 / 255.0));
    REQUIRE(parse_color("transparent", c));
    CHECK(c.a == 0.0f);
    CHECK_FALSE(parse_color("#12", c));
    CHECK_FALSE(parse_color("notacolor", c));
}

// --- flattening -----------------------------------------------------------------

TEST_CASE("Flattening stays within tolerance of the true curve") {
    const vec2 p0{0, 0}, p1{100, -200}, p2{300, 300}, p3{400, 0};
    Path p;
    p.move_to(p0);
    p.cubic_to(p1, p2, p3);
    for (float tolerance : {2.0f, 0.5f, 0.1f, 0.01f}) {
        std::vector<Polyline> lines;
        flatten(p, tolerance, lines);
        REQUIRE(lines.size() == 1);
        float worst = 0.0f;
        for (int i = 0; i <= 2000; ++i) {
            const vec2 q = cubic_point(p0, p1, p2, p3, static_cast<float>(i) / 2000.0f);
            worst = std::max(worst, distance_to_polyline(q, lines[0].points, false));
        }
        CHECK(worst <= tolerance * 1.01f);
        // Uses exactly the segment count Wang's bound asks for.
        CHECK(static_cast<int>(lines[0].points.size()) - 1 == cubic_segments(p0, p1, p2, p3, tolerance));
    }
}

TEST_CASE("Flattening: finer tolerance -> more segments, ~sqrt scaling; lines aren't subdivided") {
    Path circle;
    circle.add_ellipse({0, 0}, {100, 100});
    std::vector<Polyline> coarse, fine;
    flatten(circle, 1.0f, coarse);
    flatten(circle, 0.01f, fine);
    const double ratio = static_cast<double>(fine[0].points.size()) / static_cast<double>(coarse[0].points.size());
    CHECK(ratio == doctest::Approx(10.0).epsilon(0.25)); // segments ~ 1/sqrt(tolerance)
    CHECK(coarse[0].closed);

    Path square = parse("M0 0 H10 V10 H0 Z");
    std::vector<Polyline> lines;
    flatten(square, 0.0001f, lines);
    CHECK(lines[0].points.size() == 4);
}

TEST_CASE("Tolerance and zoom levels") {
    CHECK(tolerance_for_scale(1.0f) == doctest::Approx(0.25f));
    CHECK(tolerance_for_scale(8.0f) == doctest::Approx(0.25f / 8.0f));
    CHECK(lod_level(1.0f) == 0);
    CHECK(lod_level(1.5f) == 1);
    CHECK(lod_level(2.0f) == 1);
    CHECK(lod_level(2.01f) == 2);
    CHECK(lod_level(0.5f) == -1);
    CHECK(lod_level(0.3f) == -1);
    CHECK(lod_scale(3) == 8.0f);
    // A mesh for a level is fine for every scale it covers.
    for (float s : {0.3f, 0.9f, 1.0f, 1.7f, 5.0f, 8.0f}) {
        CHECK(lod_scale(lod_level(s)) >= s);
        CHECK(lod_scale(lod_level(s)) < s * 2.0f + 1e-4f);
    }
}

// --- tessellation ----------------------------------------------------------------

TEST_CASE("Tessellation: square and triangle counts") {
    const std::vector<Polyline> square{line_of({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true)};
    TriMesh mesh;
    REQUIRE(tessellate_fill(square, FillRule::NonZero, mesh));
    CHECK(mesh.triangle_count() == 2);
    CHECK(mesh.area() == doctest::Approx(100.0));
    CHECK(fill_area({}, FillRule::NonZero) == 0.0);
}

TEST_CASE("Tessellation: circle area converges") {
    Path circle;
    circle.add_ellipse({50, 50}, {40, 40});
    std::vector<Polyline> lines;
    flatten(circle, 0.01f, lines);
    CHECK(fill_area(lines, FillRule::NonZero) == doctest::Approx(pi * 40 * 40).epsilon(0.002));
}

TEST_CASE("Tessellation: evenodd holes vs nonzero") {
    const Polyline outer = line_of({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true);
    const Polyline inner_same = line_of({{3, 3}, {7, 3}, {7, 7}, {3, 7}}, true);
    const Polyline inner_reversed = line_of({{3, 3}, {3, 7}, {7, 7}, {7, 3}}, true);
    CHECK(fill_area({outer, inner_same}, FillRule::EvenOdd) == doctest::Approx(84.0));
    CHECK(fill_area({outer, inner_same}, FillRule::NonZero) == doctest::Approx(100.0));
    CHECK(fill_area({outer, inner_reversed}, FillRule::NonZero) == doctest::Approx(84.0));
    CHECK(fill_area({outer, inner_reversed}, FillRule::EvenOdd) == doctest::Approx(84.0));
}

TEST_CASE("Tessellation: self-intersecting star under both rules") {
    // Pentagram: evenodd excludes the inner pentagon, nonzero includes it.
    std::vector<vec2> star;
    for (int i = 0; i < 5; ++i) {
        const double a = -pi / 2 + i * 4 * pi / 5;
        star.emplace_back(static_cast<float>(100 * std::cos(a)), static_cast<float>(100 * std::sin(a)));
    }
    const std::vector<Polyline> contour{{star, true}};
    const double nonzero = fill_area(contour, FillRule::NonZero);
    const double evenodd = fill_area(contour, FillRule::EvenOdd);
    // Inner pentagon circumradius = R * sin(18deg) / sin(126deg)... compare via the difference.
    const double r_inner = 100.0 * std::sin(pi / 10) / std::sin(7 * pi / 10);
    const double pentagon = 2.5 * r_inner * r_inner * std::sin(2 * pi / 5);
    CHECK(nonzero - evenodd == doctest::Approx(pentagon).epsilon(0.001));
    // The bow tie: two triangles, same under both rules.
    const std::vector<Polyline> bow{line_of({{0, 0}, {10, 10}, {10, 0}, {0, 10}}, true)};
    CHECK(fill_area(bow, FillRule::NonZero) == doctest::Approx(50.0));
    CHECK(fill_area(bow, FillRule::EvenOdd) == doctest::Approx(50.0));
}

TEST_CASE("Regions: normalize, union and intersection") {
    const Polyline a = line_of({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true);
    const Polyline b_cw = line_of({{5, 5}, {5, 15}, {15, 15}, {15, 5}}, true); // opposite orientation on purpose
    const auto na = normalize_region(std::vector<Polyline>{a}, FillRule::NonZero);
    const auto nb = normalize_region(std::vector<Polyline>{b_cw}, FillRule::NonZero);
    CHECK(fill_area(union_regions(na, nb), FillRule::NonZero) == doctest::Approx(175.0));
    CHECK(fill_area(intersect_regions(na, nb), FillRule::NonZero) == doctest::Approx(25.0));
    TriMesh mesh;
    REQUIRE(tessellate_intersection(na, nb, mesh));
    CHECK(mesh.area() == doctest::Approx(25.0));
    // Intersection with a holed region keeps the hole.
    const Polyline hole = line_of({{6, 6}, {9, 6}, {9, 9}, {6, 9}}, true);
    const auto ring = normalize_region(std::vector<Polyline>{a, hole}, FillRule::EvenOdd);
    CHECK(fill_area(intersect_regions(ring, nb), FillRule::NonZero) == doctest::Approx(16.0));
}

// --- stroker ----------------------------------------------------------------------

TEST_CASE("Stroker: caps") {
    const Polyline segment = line_of({{0, 0}, {100, 0}});
    StrokeStyle style;
    style.width = 10;
    style.cap = LineCap::Butt;
    CHECK(stroke_area(segment, style) == doctest::Approx(1000.0).epsilon(1e-4));
    style.cap = LineCap::Square;
    CHECK(stroke_area(segment, style) == doctest::Approx(1100.0).epsilon(1e-4));
    style.cap = LineCap::Round;
    CHECK(stroke_area(segment, style) == doctest::Approx(1000.0 + pi * 25.0).epsilon(1e-3));
}

TEST_CASE("Stroker: zero-length subpaths draw dots for round and square caps") {
    const Polyline dot = line_of({{5, 5}});
    StrokeStyle style;
    style.width = 4;
    CHECK(stroke_area(dot, style) == 0.0);
    style.cap = LineCap::Square;
    CHECK(stroke_area(dot, style) == doctest::Approx(16.0));
    style.cap = LineCap::Round;
    CHECK(stroke_area(dot, style) == doctest::Approx(pi * 4.0).epsilon(1e-3));
}

TEST_CASE("Stroker: joins on a right angle") {
    // L shape (0,0) -> (100,0) -> (100,100), width 10, butt caps.
    const Polyline corner = line_of({{0, 0}, {100, 0}, {100, 100}});
    StrokeStyle style;
    style.width = 10;
    // Two 100x10 rectangles overlap in a 5x5 square at the inner corner side:
    // the union of the rectangles alone is 2000 - 25.
    const double base = 2000.0 - 25.0;
    style.join = LineJoin::Bevel;
    CHECK(stroke_area(corner, style) == doctest::Approx(base + 12.5).epsilon(1e-4)); // outer triangle 5*5/2
    style.join = LineJoin::Miter;
    CHECK(stroke_area(corner, style) == doctest::Approx(base + 25.0).epsilon(1e-4)); // outer 5x5 square
    style.join = LineJoin::Round;
    CHECK(stroke_area(corner, style) == doctest::Approx(base + pi * 25.0 / 4.0).epsilon(1e-3));
}

TEST_CASE("Stroker: miter limit falls back to bevel") {
    // A 30-degree spike: miter ratio 1 / sin(15deg) = 3.86.
    const double half = 15.0 * pi / 180.0;
    const vec2 tip{0, 0};
    const vec2 a{static_cast<float>(-100 * std::sin(half)), static_cast<float>(100 * std::cos(half))};
    const vec2 b{static_cast<float>(100 * std::sin(half)), static_cast<float>(100 * std::cos(half))};
    const Polyline spike = line_of({a, tip, b});
    StrokeStyle style;
    style.width = 10;
    style.join = LineJoin::Miter;
    style.miter_limit = 4.0f;
    const double mitered = stroke_area(spike, style);
    style.miter_limit = 3.5f;
    const double beveled = stroke_area(spike, style);
    style.join = LineJoin::Bevel;
    CHECK(beveled == doctest::Approx(stroke_area(spike, style)).epsilon(1e-5));
    CHECK(mitered > beveled + 10.0);
}

TEST_CASE("Stroker: closed rectangle with miter joins has no gaps") {
    const Polyline rect = line_of({{0, 0}, {100, 0}, {100, 50}, {0, 50}}, true);
    StrokeStyle style;
    style.width = 10;
    style.join = LineJoin::Miter;
    // Outer 110x60 minus inner 90x40.
    CHECK(stroke_area(rect, style) == doctest::Approx(110.0 * 60.0 - 90.0 * 40.0).epsilon(1e-4));
    style.join = LineJoin::Bevel;
    CHECK(stroke_area(rect, style) == doctest::Approx(110.0 * 60.0 - 90.0 * 40.0 - 4 * 12.5).epsilon(1e-4));
}

TEST_CASE("Stroker: a self-overlapping stroke covers each point once") {
    // Back and forth along the same line: area is still one rectangle.
    const Polyline back_forth = line_of({{0, 0}, {100, 0}, {50, 0}});
    StrokeStyle style;
    style.width = 10;
    style.join = LineJoin::Bevel;
    CHECK(stroke_area(back_forth, style) == doctest::Approx(1000.0).epsilon(1e-4));
}

TEST_CASE("Dashes: pattern, offset, odd-length repetition") {
    const std::vector<Polyline> line{line_of({{0, 0}, {100, 0}})};
    std::vector<Polyline> dashes;
    const std::vector<float> pattern{20, 10};
    apply_dashes(line, pattern, 0.0f, dashes);
    REQUIRE(dashes.size() == 4); // 0-20, 30-50, 60-80, 90-100
    CHECK(near(dashes[0].points.front(), {0, 0}));
    CHECK(near(dashes[0].points.back(), {20, 0}));
    CHECK(near(dashes[3].points.front(), {90, 0}));
    CHECK(near(dashes[3].points.back(), {100, 0}));

    dashes.clear();
    apply_dashes(line, pattern, 15.0f, dashes); // starts 15 into the first dash
    CHECK(near(dashes[0].points.back(), {5, 0}));
    CHECK(near(dashes[1].points.front(), {15, 0}));

    dashes.clear();
    const std::vector<float> odd{10};
    apply_dashes(line, odd, 0.0f, dashes); // "10" == "10 10"
    CHECK(dashes.size() == 5);

    dashes.clear();
    const std::vector<float> invalid{10, -1};
    apply_dashes(line, invalid, 0.0f, dashes);
    CHECK(dashes.size() == 1); // solid

    // Dashes follow corners.
    const std::vector<Polyline> corner{line_of({{0, 0}, {10, 0}, {10, 10}})};
    dashes.clear();
    const std::vector<float> long_dash{15, 100};
    apply_dashes(corner, long_dash, 0.0f, dashes);
    REQUIRE(dashes.size() == 1);
    CHECK(dashes[0].points.size() == 3);
    CHECK(near(dashes[0].points.back(), {10, 5}));

    // Stroke area of a dashed line = dash lengths * width.
    StrokeStyle style;
    style.width = 2;
    style.dashes = {20, 10};
    CHECK(stroke_area(line[0], style) == doctest::Approx(70.0 * 2.0).epsilon(1e-4));
}

TEST_CASE("Path::arc constructs circular arcs with cubic Beziers") {
    // 1. Quarter arc from 0 to pi/2 (clockwise)
    Path p1;
    p1.arc({100.0f, 100.0f}, 50.0f, 0.0f, static_cast<float>(pi / 2.0));
    REQUIRE(p1.verbs.size() == 2);
    CHECK(p1.verbs[0] == Verb::MoveTo);
    CHECK(near(p1.points[0], {150.0f, 100.0f}));
    CHECK(p1.verbs[1] == Verb::CubicTo);
    CHECK(near(p1.current_point(), {100.0f, 150.0f}));

    // 2. Full circle arc (sweep 2pi) splits into 4 cubics (<= 90 deg each)
    Path p2;
    p2.arc({0.0f, 0.0f}, 10.0f, 0.0f, static_cast<float>(2.0 * pi));
    REQUIRE(p2.verbs.size() == 5); // 1 MoveTo + 4 CubicTo
    CHECK(p2.verbs[0] == Verb::MoveTo);
    for (int i = 1; i <= 4; ++i) {
        CHECK(p2.verbs[i] == Verb::CubicTo);
    }
    CHECK(near(p2.current_point(), {10.0f, 0.0f}));

    // 3. Counter-clockwise arc from 0 to -pi/2
    Path p3;
    p3.arc({0.0f, 0.0f}, 20.0f, 0.0f, static_cast<float>(-pi / 2.0), true);
    REQUIRE(p3.verbs.size() == 2);
    CHECK(near(p3.points[0], {20.0f, 0.0f}));
    CHECK(near(p3.current_point(), {0.0f, -20.0f}));

    // 4. Line connection when path is already non-empty
    Path p4;
    p4.move_to({0.0f, 0.0f});
    p4.arc({50.0f, 50.0f}, 10.0f, 0.0f, static_cast<float>(pi / 2.0));
    // Should have MoveTo(0,0), LineTo(60, 50), CubicTo(...)
    REQUIRE(p4.verbs.size() == 3);
    CHECK(p4.verbs[0] == Verb::MoveTo);
    CHECK(p4.verbs[1] == Verb::LineTo);
    CHECK(near(p4.points[1], {60.0f, 50.0f}));
    CHECK(p4.verbs[2] == Verb::CubicTo);

    // 5. add_sector creates a closed subpath with MoveTo(center), LineTo(start), CubicTo(arc), Close()
    Path p5;
    p5.add_sector({0.0f, 0.0f}, 10.0f, 0.0f, static_cast<float>(pi / 2.0));
    REQUIRE(p5.verbs.size() == 4);
    CHECK(p5.verbs[0] == Verb::MoveTo);
    CHECK(near(p5.points[0], {0.0f, 0.0f}));
    CHECK(p5.verbs[1] == Verb::LineTo);
    CHECK(near(p5.points[1], {10.0f, 0.0f}));
    CHECK(p5.verbs[2] == Verb::CubicTo);
    CHECK(p5.verbs[3] == Verb::Close);
}


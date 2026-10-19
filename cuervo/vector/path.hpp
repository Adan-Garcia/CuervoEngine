#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/math/affine2.hpp"
#include "cuervo/math/vec.hpp"

// Vector paths (DESIGN.md 4.5): the curve representation shared by the SVG
// importer, the ImportedVector format and the runtime tessellator.
//
// Coordinates follow the engine conventions (DESIGN.md 7.1): y down, SVG user
// units map 1:1 to world units, angles in radians.
namespace cuervo::vector {

enum class Verb : std::uint8_t {
    MoveTo,  // 1 point
    LineTo,  // 1 point
    QuadTo,  // 2 points: control, end
    CubicTo, // 3 points: control 1, control 2, end
    Close,   // 0 points
};

// Number of points a verb consumes.
constexpr std::uint32_t verb_point_count(Verb verb) {
    switch (verb) {
        case Verb::MoveTo:
        case Verb::LineTo:  return 1;
        case Verb::QuadTo:  return 2;
        case Verb::CubicTo: return 3;
        case Verb::Close:   return 0;
    }
    return 0;
}

struct Bounds {
    vec2 min{0.0f, 0.0f};
    vec2 max{0.0f, 0.0f};
    bool empty = true;

    void add(vec2 p) {
        if (empty) {
            min = max = p;
            empty = false;
        } else {
            min = cuervo::min(min, p);
            max = cuervo::max(max, p);
        }
    }
    vec2 size() const { return empty ? vec2(0.0f) : max - min; }
    friend bool operator==(const Bounds&, const Bounds&) = default;
};

class Path {
public:
    std::vector<Verb> verbs;
    std::vector<vec2> points;

    void move_to(vec2 p);
    void line_to(vec2 p);
    void quad_to(vec2 control, vec2 end);
    void cubic_to(vec2 control1, vec2 control2, vec2 end);
    // SVG elliptical arc (endpoint parameterization, SVG 1.1 F.6) from the
    // current point to `end`, appended as cubic Béziers of at most 90 degrees
    // each. `x_axis_rotation` is in radians. Out-of-range radii are scaled up
    // and zero radii become a line, as the SVG spec requires.
    void arc_to(vec2 radii, float x_axis_rotation, bool large_arc, bool sweep, vec2 end);
    // Circular arc (center parameterization) around `center` with `radius` from
    // `start_angle` to `end_angle`. If the path is non-empty, a line connects the
    // current point to the arc's start; if empty, it starts with move_to.
    void arc(vec2 center, float radius, float start_angle, float end_angle, bool counter_clockwise = false);
    void close();

    // Shape helpers (SVG basic shapes, all as closed subpaths, clockwise on a
    // y-down screen like the SVG spec's construction).
    void add_rect(vec2 origin, vec2 size, vec2 corner_radii = vec2(0.0f));
    void add_ellipse(vec2 center, vec2 radii);
    void add_sector(vec2 center, float radius, float start_angle, float end_angle);

    bool empty() const { return verbs.empty(); }
    void clear() {
        verbs.clear();
        points.clear();
    }
    // Last point written, or the start of the subpath after a Close.
    vec2 current_point() const { return current_; }

    void transform(const Affine2& m);
    // Tight bounds of the curves (Bézier extrema, not just control points).
    Bounds bounds() const;

    friend bool operator==(const Path& a, const Path& b) { return a.verbs == b.verbs && a.points == b.points; }

private:
    void ensure_subpath();

    vec2 current_{0.0f, 0.0f};
    vec2 subpath_start_{0.0f, 0.0f};
    bool open_subpath_ = false;
};

// Parses SVG path data (`d` attribute): every command M m L l H h V v C c S s
// Q q T t A a Z z, implicit repetition, compact number forms ("1.5.5", "1e-3",
// "-1-2") and compact arc flags ("a1 1 0 01 2 2"). On a syntax error the path
// keeps everything parsed before it (as the SVG spec requires for rendering),
// `error` (if given) gets a message, and the function returns false.
bool parse_path_data(std::string_view data, Path& out, std::string* error = nullptr);

// Stable 64-bit content hash (FNV-1a over verbs and point bits). Used as the
// runtime tessellation cache key.
std::uint64_t hash_path(const Path& path, std::uint64_t seed = 0xcbf29ce484222325ull);

} // namespace cuervo::vector

#pragma once

#include <span>
#include <vector>

#include "cuervo/math/vec.hpp"
#include "cuervo/vector/path.hpp"

// Curve flattening (DESIGN.md 4.5). The tolerance is a distance in path
// units; callers derive it from the scale the mesh will be drawn at
// (`tolerance_for_scale`) so curves stay smooth at any zoom: meshes are
// rebuilt per zoom "level" (powers of two, see lod_level) and cached.
namespace cuervo::vector {

struct Polyline {
    std::vector<vec2> points;
    bool closed = false; // ended with Z: strokes join the last point to the first

    friend bool operator==(const Polyline&, const Polyline&) = default;
};

// Maximum distance between the true curve and its flattened polyline, in
// framebuffer pixels. A quarter pixel is invisible even with MSAA.
inline constexpr float default_pixel_tolerance = 0.25f;

// Path-space tolerance for content drawn at `pixels_per_unit` framebuffer
// pixels per path unit.
float tolerance_for_scale(float pixels_per_unit, float pixel_tolerance = default_pixel_tolerance);

// Zoom bucket for mesh caching: the smallest integer L with 2^L >= scale.
// A mesh built for level L (at scale 2^L) is fine for every scale in
// (2^(L-1), 2^L]; a zoom change of more than 2x moves to another level.
int lod_level(float pixels_per_unit);
float lod_scale(int level);

// Number of line segments Wang's formula guarantees for a deviation <= tolerance.
int quad_segments(vec2 p0, vec2 p1, vec2 p2, float tolerance);
int cubic_segments(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float tolerance);

// Flattens every subpath. Consecutive duplicate points are dropped; a
// subpath that collapses to one point is kept (strokes draw caps for it).
void flatten(const Path& path, float tolerance, std::vector<Polyline>& out);

// Number of segments for a circle of `radius` with chord error <= tolerance.
int circle_segments(float radius, float tolerance);

// Signed area (positive = counter-clockwise in a y-up frame, i.e. clockwise
// on a y-down screen). Treats the polyline as closed.
float signed_area(std::span<const vec2> polygon);

} // namespace cuervo::vector

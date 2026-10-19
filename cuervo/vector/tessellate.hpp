#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "cuervo/math/vec.hpp"
#include "cuervo/vector/flatten.hpp"

// Fill tessellation and region operations (DESIGN.md 4.5), built on libtess2:
// a sweep-line tessellator that splits self-intersecting and overlapping
// contours, so every SVG fill (nonzero or evenodd, any self-intersections)
// becomes non-overlapping triangles. Non-overlapping matters: translucent
// fills and strokes must cover each pixel once.
//
// Regions are lists of closed contours. A *normalized* region is libtess2's
// boundary-contour output: no self-intersections, and every contour oriented
// so the inside has winding +1. Normalized regions combine exactly:
//   union        = all contours of both, nonzero
//   intersection = all contours of both, |winding| >= 2
namespace cuervo::vector {

enum class FillRule : std::uint8_t { NonZero, EvenOdd };

struct TriMesh {
    std::vector<vec2> vertices;
    std::vector<std::uint32_t> indices; // triangle list

    std::size_t triangle_count() const { return indices.size() / 3; }
    // Sum of unsigned triangle areas.
    double area() const;
    void clear() {
        vertices.clear();
        indices.clear();
    }
};

// Triangulates the region the contours enclose under `rule` (open polylines
// are closed implicitly) and appends it to `out`. Returns false if libtess2
// fails (e.g. out of memory or NaN input); `out` is unchanged then.
bool tessellate_fill(std::span<const Polyline> contours, FillRule rule, TriMesh& out);

// Boundary contours of the region, normalized (see above).
std::vector<Polyline> normalize_region(std::span<const Polyline> contours, FillRule rule);

// Union / intersection of two normalized regions, normalized.
std::vector<Polyline> union_regions(std::span<const Polyline> a, std::span<const Polyline> b);
std::vector<Polyline> intersect_regions(std::span<const Polyline> a, std::span<const Polyline> b);
// a minus b (both normalized), normalized.
std::vector<Polyline> subtract_regions(std::span<const Polyline> a, std::span<const Polyline> b);

// Triangulates the intersection of two normalized regions.
bool tessellate_intersection(std::span<const Polyline> a, std::span<const Polyline> b, TriMesh& out);

} // namespace cuervo::vector

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "cuervo/vector/flatten.hpp"
#include "cuervo/vector/tessellate.hpp"

// Stroker (DESIGN.md 4.5): turns flattened polylines into the stroke's area.
//
// Each polyline becomes one outline polygon (two loops if closed): the left
// offset forward, the right offset backward, joins and caps inline, and a
// pivot through the vertex on the inner side of each join. That outline is,
// as a boundary chain, exactly the sum of one quad per segment plus one piece
// per join and cap, all oriented the same way, so its nonzero fill is the
// exact stroke area even where it crosses itself. The fill goes through the
// tessellator, so the final triangles never overlap: a translucent stroke
// covers each pixel once, even where a path doubles back on itself.
namespace cuervo::vector {

enum class LineJoin : std::uint8_t { Miter, Round, Bevel };
enum class LineCap : std::uint8_t { Butt, Round, Square };

struct StrokeStyle {
    float width = 1.0f;
    LineJoin join = LineJoin::Miter;
    LineCap cap = LineCap::Butt;
    float miter_limit = 4.0f; // SVG default; ratio of miter length to stroke width
    std::vector<float> dashes; // empty = solid
    float dash_offset = 0.0f;

    friend bool operator==(const StrokeStyle&, const StrokeStyle&) = default;
};

// Splits polylines into dashes (SVG stroke-dasharray / stroke-dashoffset). An
// odd-length pattern repeats twice, as in SVG. Invalid patterns (negative
// values or a zero sum) leave the lines solid.
void apply_dashes(std::span<const Polyline> lines, std::span<const float> dashes, float offset,
                  std::vector<Polyline>& out);

// Appends the stroke outlines (closed, positively oriented polygons) for
// `lines` to `out`. Dashing is applied first if the style has a pattern.
// `tolerance` controls round joins/caps.
void stroke_pieces(std::span<const Polyline> lines, const StrokeStyle& style, float tolerance,
                   std::vector<Polyline>& out);

// Convenience: pieces -> non-overlapping triangles.
bool stroke_to_mesh(std::span<const Polyline> lines, const StrokeStyle& style, float tolerance, TriMesh& out);

} // namespace cuervo::vector

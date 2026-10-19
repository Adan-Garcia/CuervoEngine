#include "cuervo/vector/stroke.hpp"

#include <algorithm>
#include <cmath>

// Render/import code, not simulation: <cmath> is fine here (DESIGN.md 4.1).

namespace cuervo::vector {

namespace {

constexpr float pi = 3.14159265358979323846f;

vec2 perp(vec2 d) { return {-d.y(), d.x()}; }

// Adds a polygon, reversed if needed so every piece has positive area.
void emit(std::vector<Polyline>& out, std::vector<vec2> points) {
    if (points.size() < 3) {
        return;
    }
    if (signed_area(points) < 0.0f) {
        std::reverse(points.begin(), points.end());
    }
    out.push_back({std::move(points), true});
}

void emit_disc(std::vector<Polyline>& out, vec2 center, float radius, float tolerance) {
    const int n = circle_segments(radius, tolerance);
    std::vector<vec2> points;
    points.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float a = 2.0f * pi * static_cast<float>(i) / static_cast<float>(n);
        points.push_back(center + vec2(std::cos(a), std::sin(a)) * radius);
    }
    emit(out, std::move(points));
}

// Appends the arc of `radius` around `center` that starts at unit direction
// `from` and turns by `sweep` radians, without its two endpoints (the caller
// adds those exactly).
void append_arc(std::vector<vec2>& out, vec2 center, vec2 from, float sweep, float radius, float tolerance) {
    const int full = circle_segments(radius, tolerance);
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / (2.0f * pi) * static_cast<float>(full))));
    for (int i = 1; i < n; ++i) {
        const float a = sweep * static_cast<float>(i) / static_cast<float>(n);
        const float c = std::cos(a), si = std::sin(a);
        out.push_back(center + vec2(from.x() * c - from.y() * si, from.x() * si + from.y() * c) * radius);
    }
}

// Outline construction (see stroke.hpp): each polyline becomes one polygon,
// its left offset forward and right offset backward with joins and caps
// inline. As a boundary chain it is exactly the sum of the per-segment quads
// plus the join and cap pieces (their shared edges cancel), so its nonzero
// winding at a point is the number of pieces covering it: the filled region
// is exactly the stroke, even where the outline crosses itself. On the inner
// side of a join the outline goes through the vertex itself ("pivot"), which
// is what makes the cancellation exact.

// Adds the join at vertex p (incoming d0, outgoing d1, unit) to the chains of
// the left (+perp) and right sides.
void add_join(std::vector<vec2>& left, std::vector<vec2>& right, vec2 p, vec2 d0, vec2 d1, float hw,
              const StrokeStyle& style, float tolerance) {
    const vec2 n0 = perp(d0) * hw;
    const vec2 n1 = perp(d1) * hw;
    const float turn = cross(d0, d1);
    const float along = dot(d0, d1);
    // Nearly collinear segments (every vertex of a flattened curve): the outer
    // notch is hw * (1 - cos(angle / 2)) deep. Below a fraction of the
    // flattening tolerance, bevel both sides directly; the inner side's tiny
    // overlap is covered twice by the segments, so skipping the pivot is safe.
    const float half_cos = std::sqrt(std::max(0.0f, (1.0f + along) * 0.5f));
    if (along > 0.0f && hw * (1.0f - half_cos) < tolerance * 0.25f) {
        left.push_back(p + n0);
        left.push_back(p + n1);
        right.push_back(p - n0);
        right.push_back(p - n1);
        return;
    }
    // Turning toward +perp makes the left side the inner one.
    const bool left_outer = turn <= 0.0f;
    const float s = left_outer ? 1.0f : -1.0f;
    std::vector<vec2>& outer = left_outer ? left : right;
    std::vector<vec2>& inner = left_outer ? right : left;
    inner.push_back(p - n0 * s);
    inner.push_back(p);
    inner.push_back(p - n1 * s);

    const vec2 u0 = perp(d0) * s; // outer normals, unit
    const vec2 u1 = perp(d1) * s;
    outer.push_back(p + u0 * hw);
    switch (style.join) {
        case LineJoin::Round: {
            float sweep = std::atan2(cross(u0, u1), dot(u0, u1));
            if (std::fabs(turn) < 1e-6f) {
                sweep = -pi * s; // a full reversal: go around the end, through p + d0 * hw
            }
            append_arc(outer, p, u0, sweep, hw, tolerance);
            break;
        }
        case LineJoin::Miter: {
            // Miter ratio = 1 / sin(theta / 2) for interior angle theta, which
            // is 2 / |u0 + u1| for unit normals.
            const vec2 sum = u0 + u1;
            const float sum_len = length(sum);
            if (sum_len > 1e-6f && 2.0f / sum_len <= style.miter_limit) {
                outer.push_back(p + sum * (2.0f * hw / (sum_len * sum_len)));
            }
            break; // otherwise bevel (the miter-limit fallback)
        }
        case LineJoin::Bevel:
            break;
    }
    outer.push_back(p + u1 * hw);
}

// Cap at `p`, going from p + perp(dir) * hw around the end (through
// p + dir * hw) to p - perp(dir) * hw, endpoints excluded.
void add_cap(std::vector<vec2>& out, vec2 p, vec2 dir, float hw, const StrokeStyle& style, float tolerance) {
    switch (style.cap) {
        case LineCap::Butt:
            break;
        case LineCap::Round:
            append_arc(out, p, perp(dir), -pi, hw, tolerance);
            break;
        case LineCap::Square:
            out.push_back(p + (perp(dir) + dir) * hw);
            out.push_back(p + (dir - perp(dir)) * hw);
            break;
    }
}

void emit_outline(std::vector<Polyline>& out, std::vector<vec2> points) {
    if (points.size() < 3) {
        return;
    }
    // The construction winds like the segment quads (negative area); flip to
    // the positive orientation every other piece uses, so nonzero unions of
    // several outlines (subpaths, dashes, dots) never cancel.
    std::reverse(points.begin(), points.end());
    out.push_back({std::move(points), true});
}

void stroke_line(const Polyline& line, const StrokeStyle& style, float tolerance, std::vector<Polyline>& out) {
    const float hw = style.width * 0.5f;
    // Unique consecutive points (flatten already dedups, user input may not).
    std::vector<vec2> pts;
    pts.reserve(line.points.size());
    for (vec2 p : line.points) {
        if (pts.empty() || length_sq(p - pts.back()) > 1e-12f) {
            pts.push_back(p);
        }
    }
    bool closed = line.closed;
    if (closed && pts.size() > 1 && length_sq(pts.back() - pts.front()) <= 1e-12f) {
        pts.pop_back();
    }
    if (pts.size() == 1) {
        // Zero-length subpath: SVG draws round/square caps as a dot.
        if (style.cap == LineCap::Round) {
            emit_disc(out, pts[0], hw, tolerance);
        } else if (style.cap == LineCap::Square) {
            emit(out, {pts[0] + vec2(-hw, -hw), pts[0] + vec2(hw, -hw), pts[0] + vec2(hw, hw), pts[0] + vec2(-hw, hw)});
        }
        return;
    }
    if (pts.size() < 2) {
        return;
    }
    if (pts.size() == 2) {
        closed = false; // a closed two-point path is a line there and back
    }

    const std::size_t n = pts.size();
    const std::size_t segments = closed ? n : n - 1;
    std::vector<vec2> dirs(segments);
    for (std::size_t i = 0; i < segments; ++i) {
        dirs[i] = normalize(pts[(i + 1) % n] - pts[i]);
    }
    std::vector<vec2> left, right;
    left.reserve(n * 2 + 8);
    right.reserve(n * 2 + 8);
    if (closed) {
        for (std::size_t i = 0; i < n; ++i) {
            add_join(left, right, pts[i], dirs[(i + segments - 1) % segments], dirs[i], hw, style, tolerance);
        }
        // Two loops: the left side forward, the right side backward.
        std::reverse(right.begin(), right.end());
        emit_outline(out, std::move(left));
        emit_outline(out, std::move(right));
        return;
    }
    left.push_back(pts[0] + perp(dirs[0]) * hw);
    right.push_back(pts[0] - perp(dirs[0]) * hw);
    for (std::size_t i = 1; i + 1 < n; ++i) {
        add_join(left, right, pts[i], dirs[i - 1], dirs[i], hw, style, tolerance);
    }
    left.push_back(pts[n - 1] + perp(dirs[segments - 1]) * hw);
    right.push_back(pts[n - 1] - perp(dirs[segments - 1]) * hw);

    std::vector<vec2> outline = std::move(left);
    add_cap(outline, pts[n - 1], dirs[segments - 1], hw, style, tolerance);
    outline.insert(outline.end(), right.rbegin(), right.rend());
    add_cap(outline, pts[0], -dirs[0], hw, style, tolerance);
    emit_outline(out, std::move(outline));
}

} // namespace

void apply_dashes(std::span<const Polyline> lines, std::span<const float> dashes, float offset,
                  std::vector<Polyline>& out) {
    std::vector<float> pattern(dashes.begin(), dashes.end());
    if (pattern.size() % 2 == 1) {
        pattern.insert(pattern.end(), dashes.begin(), dashes.end());
    }
    double total = 0.0;
    for (float d : pattern) {
        if (d < 0.0f || !std::isfinite(d)) {
            total = 0.0;
            break;
        }
        total += d;
    }
    if (pattern.empty() || total <= 1e-9) {
        out.insert(out.end(), lines.begin(), lines.end());
        return;
    }

    for (const Polyline& line : lines) {
        if (line.points.size() < 2) {
            continue;
        }
        std::vector<vec2> pts = line.points;
        if (line.closed) {
            pts.push_back(pts.front());
        }
        // Find where the offset lands in the pattern.
        double phase = std::fmod(static_cast<double>(offset), total);
        if (phase < 0.0) {
            phase += total;
        }
        std::size_t index = 0;
        while (phase >= pattern[index]) {
            phase -= pattern[index];
            index = (index + 1) % pattern.size();
        }
        double remaining = pattern[index] - phase; // length left in the current dash/gap
        bool on = index % 2 == 0;

        Polyline current;
        if (on) {
            current.points.push_back(pts[0]);
        }
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            vec2 a = pts[i];
            const vec2 b = pts[i + 1];
            double seg = length(b - a);
            while (seg > 0.0) {
                if (remaining >= seg) {
                    remaining -= seg;
                    if (on) {
                        current.points.push_back(b);
                    }
                    seg = 0.0;
                } else {
                    const vec2 cut = a + (b - a) * static_cast<float>(remaining / seg);
                    if (on) {
                        current.points.push_back(cut);
                        out.push_back(std::move(current));
                        current = {};
                    } else {
                        current.points.push_back(cut);
                    }
                    seg -= remaining;
                    a = cut;
                    on = !on;
                    index = (index + 1) % pattern.size();
                    remaining = pattern[index];
                }
            }
        }
        if (on && current.points.size() > 1) {
            out.push_back(std::move(current));
        }
    }
}

void stroke_pieces(std::span<const Polyline> lines, const StrokeStyle& style, float tolerance,
                   std::vector<Polyline>& out) {
    if (!(style.width > 0.0f)) {
        return;
    }
    if (!style.dashes.empty()) {
        std::vector<Polyline> dashed;
        apply_dashes(lines, style.dashes, style.dash_offset, dashed);
        for (const Polyline& line : dashed) {
            stroke_line(line, style, tolerance, out);
        }
        return;
    }
    for (const Polyline& line : lines) {
        stroke_line(line, style, tolerance, out);
    }
}

bool stroke_to_mesh(std::span<const Polyline> lines, const StrokeStyle& style, float tolerance, TriMesh& out) {
    std::vector<Polyline> pieces;
    stroke_pieces(lines, style, tolerance, pieces);
    return tessellate_fill(pieces, FillRule::NonZero, out);
}

} // namespace cuervo::vector

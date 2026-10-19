#include "cuervo/vector/flatten.hpp"

#include <algorithm>
#include <cmath>

// Render/import code, not simulation: <cmath> is fine here (DESIGN.md 4.1).

namespace cuervo::vector {

namespace {

constexpr int max_segments_per_curve = 4096;
constexpr float min_tolerance = 1e-5f;

void push_point(Polyline& line, vec2 p) {
    if (line.points.empty() || line.points.back() != p) {
        line.points.push_back(p);
    }
}

} // namespace

float tolerance_for_scale(float pixels_per_unit, float pixel_tolerance) {
    const float scale = pixels_per_unit > 1e-6f ? pixels_per_unit : 1e-6f;
    return std::max(pixel_tolerance / scale, min_tolerance);
}

int lod_level(float pixels_per_unit) {
    const float scale = std::clamp(pixels_per_unit, 1.0f / 1024.0f, 1024.0f);
    return static_cast<int>(std::ceil(std::log2(scale) - 1e-4f));
}

float lod_scale(int level) { return std::ldexp(1.0f, level); }

// Wang's formula: n >= sqrt(d(d-1)/8 * M / tol), M = the largest second
// difference of the control points. It bounds the distance between the curve
// and the polyline through n+1 uniformly spaced parameter values.
int quad_segments(vec2 p0, vec2 p1, vec2 p2, float tolerance) {
    const float m = length(p0 - p1 * 2.0f + p2);
    const float n = std::ceil(std::sqrt(0.25f * m / std::max(tolerance, min_tolerance)));
    return std::clamp(static_cast<int>(n), 1, max_segments_per_curve);
}

int cubic_segments(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float tolerance) {
    const float m = std::max(length(p0 - p1 * 2.0f + p2), length(p1 - p2 * 2.0f + p3));
    const float n = std::ceil(std::sqrt(0.75f * m / std::max(tolerance, min_tolerance)));
    return std::clamp(static_cast<int>(n), 1, max_segments_per_curve);
}

void flatten(const Path& path, float tolerance, std::vector<Polyline>& out) {
    Polyline* line = nullptr;
    std::size_t pi = 0;
    vec2 last{0.0f, 0.0f};
    for (Verb verb : path.verbs) {
        switch (verb) {
            case Verb::MoveTo:
                out.emplace_back();
                line = &out.back();
                last = path.points[pi++];
                line->points.push_back(last);
                break;
            case Verb::LineTo:
                last = path.points[pi++];
                push_point(*line, last);
                break;
            case Verb::QuadTo: {
                const vec2 c = path.points[pi], e = path.points[pi + 1];
                pi += 2;
                const int n = quad_segments(last, c, e, tolerance);
                for (int i = 1; i < n; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(n);
                    const float u = 1.0f - t;
                    push_point(*line, last * (u * u) + c * (2.0f * u * t) + e * (t * t));
                }
                push_point(*line, e);
                last = e;
                break;
            }
            case Verb::CubicTo: {
                const vec2 c1 = path.points[pi], c2 = path.points[pi + 1], e = path.points[pi + 2];
                pi += 3;
                const int n = cubic_segments(last, c1, c2, e, tolerance);
                for (int i = 1; i < n; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(n);
                    const float u = 1.0f - t;
                    push_point(*line, last * (u * u * u) + c1 * (3.0f * u * u * t) + c2 * (3.0f * u * t * t) +
                                          e * (t * t * t));
                }
                push_point(*line, e);
                last = e;
                break;
            }
            case Verb::Close:
                if (line) {
                    line->closed = true;
                    // The closing segment is implicit; drop an explicit copy of the start.
                    if (line->points.size() > 1 && line->points.back() == line->points.front()) {
                        line->points.pop_back();
                    }
                }
                break;
        }
    }
}

int circle_segments(float radius, float tolerance) {
    if (radius <= tolerance) {
        return 8;
    }
    // Chord sagitta r(1 - cos(pi/n)) <= tol.
    const double half_angle = std::acos(std::clamp(1.0 - static_cast<double>(tolerance) / radius, -1.0, 1.0));
    const double n = std::ceil(3.14159265358979323846 / std::max(half_angle, 1e-6));
    return std::clamp(static_cast<int>(n), 8, 1024);
}

float signed_area(std::span<const vec2> polygon) {
    double sum = 0.0;
    const std::size_t n = polygon.size();
    for (std::size_t i = 0; i < n; ++i) {
        const vec2 a = polygon[i];
        const vec2 b = polygon[(i + 1) % n];
        sum += static_cast<double>(a.x()) * b.y() - static_cast<double>(b.x()) * a.y();
    }
    return static_cast<float>(sum * 0.5);
}

} // namespace cuervo::vector

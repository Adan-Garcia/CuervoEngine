#include "cuervo/vector/mesh.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "cuervo/vector/flatten.hpp"
#include "cuervo/vector/stroke.hpp"

// Render/import code, not simulation: <cmath> is fine here (DESIGN.md 4.1).

namespace cuervo::vector {

namespace {

std::uint32_t to_byte(float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

void transform_lines(std::vector<Polyline>& lines, const Affine2& m) {
    for (Polyline& line : lines) {
        for (vec2& p : line.points) {
            p = m.apply(p);
        }
    }
}

// FNV-1a over 32-bit words.
struct Hasher {
    std::uint64_t h = 0xcbf29ce484222325ull;
    void word(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    }
    void f(float v) { word(std::bit_cast<std::uint32_t>(v)); }
    void affine(const Affine2& m) {
        f(m.a); f(m.b); f(m.c); f(m.d); f(m.tx); f(m.ty);
    }
};

std::uint64_t hash_clip(const ClipPath& clip) {
    Hasher h;
    h.word(static_cast<std::uint32_t>(clip.shapes.size()));
    for (const ClipShape& s : clip.shapes) {
        h.word(static_cast<std::uint32_t>(hash_path(s.path)));
        h.word(static_cast<std::uint32_t>(hash_path(s.path) >> 32));
        h.affine(s.transform);
        h.word(static_cast<std::uint32_t>(s.rule));
    }
    return h.h;
}

} // namespace

bool operator==(const MeshVertex& a, const MeshVertex& b) {
    return a.position == b.position && a.color == b.color && a.paint == b.paint;
}

std::uint32_t gradient_block_size(std::uint32_t stop_count) { return 4 + stop_count + (stop_count + 3) / 4; }

std::uint32_t pack_color(const Color& c, float alpha_scale) {
    return to_byte(c.r) | (to_byte(c.g) << 8) | (to_byte(c.b) << 16) | (to_byte(c.a * alpha_scale) << 24);
}

float max_scale(const Affine2& m) {
    // Largest singular value of [[a c] [b d]].
    const double a = m.a, b = m.b, c = m.c, d = m.d;
    const double s = a * a + b * b + c * c + d * d;
    const double t = std::sqrt(std::max(0.0, (a * a + b * b - c * c - d * d) * (a * a + b * b - c * c - d * d) +
                                                 4.0 * (a * c + b * d) * (a * c + b * d)));
    return static_cast<float>(std::sqrt(std::max(0.0, (s + t) * 0.5)));
}

bool append_painted(VectorMesh& mesh, const TriMesh& triangles, const Paint& paint, float alpha, const Shape& shape,
                    const VectorDocument& document) {
    if (triangles.indices.empty() || paint.kind == PaintKind::None || alpha <= 0.0f) {
        return false;
    }
    if (mesh.batches.empty()) {
        mesh.batches.push_back({static_cast<std::uint32_t>(mesh.indices.size()), 0, {}});
    }

    std::uint32_t color = 0;
    std::uint32_t paint_index = solid_paint;
    if (paint.kind == PaintKind::Solid) {
        color = pack_color(paint.color, alpha);
    } else {
        if (paint.gradient >= document.gradients.size()) {
            return false;
        }
        const Gradient& g = document.gradients[paint.gradient];
        if (g.stops.empty()) {
            return false;
        }
        Affine2 to_document = shape.transform;
        if (g.bbox_units) {
            const Bounds bb = shape.path.bounds();
            const vec2 size = bb.size();
            if (bb.empty || size.x() <= 0.0f || size.y() <= 0.0f) {
                return false; // SVG 1.1 13.2.2: bbox units on a zero-area box render nothing
            }
            to_document = to_document * Affine2::translate(bb.min) * Affine2::scale(size);
        }
        to_document = to_document * g.transform;

        // Degenerate geometry paints the last stop (13.2.3 / 13.2.4).
        const bool degenerate = g.kind == GradientKind::Linear ? g.p0 == g.p1 : !(g.radius > 0.0f);
        if (degenerate || g.stops.size() == 1 || to_document.determinant() == 0.0f) {
            color = pack_color(g.stops.back().color, alpha);
        } else {
            std::vector<GradientStop> stops = g.stops;
            if (stops.size() > max_gradient_stops) {
                std::vector<GradientStop> fewer;
                for (std::uint32_t i = 0; i < max_gradient_stops; ++i) {
                    fewer.push_back(stops[i * (stops.size() - 1) / (max_gradient_stops - 1)]);
                }
                stops = std::move(fewer);
            }
            const auto n = static_cast<std::uint32_t>(stops.size());
            const std::uint32_t block = gradient_block_size(n);
            MeshBatch* batch = &mesh.batches.back();
            if (batch->paints.size() + block > paint_window_vec4) {
                // Paint window full: start a new material batch (one more draw call).
                mesh.batches.push_back({static_cast<std::uint32_t>(mesh.indices.size()), 0, {}});
                batch = &mesh.batches.back();
            }
            paint_index = static_cast<std::uint32_t>(batch->paints.size());
            const Affine2 inv = to_document.inverse();
            vec2 focal = g.p1;
            float radius = g.radius;
            if (g.kind == GradientKind::Radial) {
                // A focal point outside the circle moves onto it (SVG 1.1 13.2.3);
                // keep it just inside so the shader's quadratic stays well-posed.
                const vec2 offset = focal - g.p0;
                const float dist = length(offset);
                if (dist > radius * 0.999f) {
                    focal = g.p0 + offset * (radius * 0.999f / dist);
                }
            }
            const float kind = g.kind == GradientKind::Linear ? 0.0f : 1.0f;
            batch->paints.push_back({inv.a, inv.c, inv.tx, kind});
            batch->paints.push_back({inv.b, inv.d, inv.ty, static_cast<float>(g.spread)});
            batch->paints.push_back({g.p0.x(), g.p0.y(), focal.x(), focal.y()});
            batch->paints.push_back({radius, static_cast<float>(n), 0.0f, 0.0f});
            for (const GradientStop& s : stops) {
                // Premultiplied, so stops fading to transparent don't darken (as browsers do).
                batch->paints.push_back({s.color.r * s.color.a, s.color.g * s.color.a, s.color.b * s.color.a, s.color.a});
            }
            for (std::uint32_t i = 0; i < n; i += 4) {
                std::array<float, 4> offsets{1.0f, 1.0f, 1.0f, 1.0f};
                for (std::uint32_t k = 0; k < 4 && i + k < n; ++k) {
                    offsets[k] = stops[i + k].offset;
                }
                batch->paints.push_back(offsets);
            }
            color = pack_color({1.0f, 1.0f, 1.0f, 1.0f}, alpha);
        }
    }

    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (vec2 p : triangles.vertices) {
        mesh.vertices.push_back({p, color, paint_index});
        mesh.bounds.add(p);
    }
    for (std::uint32_t i : triangles.indices) {
        mesh.indices.push_back(base + i);
    }
    mesh.batches.back().index_count =
        static_cast<std::uint32_t>(mesh.indices.size()) - mesh.batches.back().first_index;
    return true;
}

std::vector<Polyline> clip_region(const ClipPath& clip, float tolerance) {
    std::vector<Polyline> region;
    for (const ClipShape& shape : clip.shapes) {
        const float s = max_scale(shape.transform);
        if (!(s > 0.0f)) {
            continue;
        }
        std::vector<Polyline> lines;
        flatten(shape.path, tolerance / s, lines);
        transform_lines(lines, shape.transform);
        std::vector<Polyline> normalized = normalize_region(lines, shape.rule);
        region = region.empty() ? std::move(normalized) : union_regions(region, normalized);
    }
    return region;
}

bool stroke_occludes_fill(const Shape& shape, const VectorDocument& document) {
    if (shape.opacity >= 1.0f || shape.stroke_first || shape.fill.kind == PaintKind::None ||
        shape.stroke.kind == PaintKind::None ||
        shape.stroke_opacity < 1.0f || !(shape.stroke_style.width > 0.0f)) {
        return false;
    }
    if (shape.stroke.kind == PaintKind::Solid) {
        return shape.stroke.color.a >= 1.0f;
    }
    if (shape.stroke.gradient >= document.gradients.size()) {
        return false;
    }
    const auto& stops = document.gradients[shape.stroke.gradient].stops;
    return std::all_of(stops.begin(), stops.end(), [](const GradientStop& s) { return s.color.a >= 1.0f; });
}

void tessellate_shape(const Shape& shape, float tolerance, const std::vector<Polyline>* clip, ShapeGeometry& out,
                      bool occlude) {
    out.fill.clear();
    out.stroke.clear();
    const float scale = max_scale(shape.transform);
    if (!(scale > 0.0f) || (clip && clip->empty())) {
        return; // collapsed transform, or clipped away entirely
    }
    const float local_tolerance = tolerance / scale;
    std::vector<Polyline> lines;
    flatten(shape.path, local_tolerance, lines);

    const bool has_fill = shape.fill.kind != PaintKind::None;
    const bool has_stroke = shape.stroke.kind != PaintKind::None && shape.stroke_style.width > 0.0f;
    std::vector<Polyline> pieces;
    if (has_stroke) {
        // Stroke in the shape's own space, then transform: non-uniform scale
        // and skew distort the stroke exactly as SVG specifies.
        stroke_pieces(lines, shape.stroke_style, local_tolerance, pieces);
        transform_lines(pieces, shape.transform);
    }

    if (has_fill) {
        std::vector<Polyline> contours = lines;
        transform_lines(contours, shape.transform);
        if (occlude && has_stroke) {
            const std::vector<Polyline> visible = subtract_regions(normalize_region(contours, shape.fill_rule),
                                                                   normalize_region(pieces, FillRule::NonZero));
            if (clip) {
                tessellate_intersection(visible, *clip, out.fill);
            } else {
                tessellate_fill(visible, FillRule::NonZero, out.fill);
            }
        } else if (clip) {
            tessellate_intersection(normalize_region(contours, shape.fill_rule), *clip, out.fill);
        } else {
            tessellate_fill(contours, shape.fill_rule, out.fill);
        }
    }
    if (has_stroke) {
        if (clip) {
            tessellate_intersection(normalize_region(pieces, FillRule::NonZero), *clip, out.stroke);
        } else {
            tessellate_fill(pieces, FillRule::NonZero, out.stroke);
        }
    }
}

std::uint64_t hash_shape_geometry(const Shape& shape, const VectorDocument& document) {
    Hasher h;
    const std::uint64_t ph = hash_path(shape.path);
    h.word(static_cast<std::uint32_t>(ph));
    h.word(static_cast<std::uint32_t>(ph >> 32));
    h.affine(shape.transform);
    h.word(shape.fill.kind == PaintKind::None ? 0u : 1u);
    h.word(static_cast<std::uint32_t>(shape.fill_rule));
    h.word(shape.stroke.kind == PaintKind::None ? 0u : 1u);
    const StrokeStyle& s = shape.stroke_style;
    h.f(s.width);
    h.word(static_cast<std::uint32_t>(s.join));
    h.word(static_cast<std::uint32_t>(s.cap));
    h.f(s.miter_limit);
    h.f(s.dash_offset);
    h.word(static_cast<std::uint32_t>(s.dashes.size()));
    for (float d : s.dashes) h.f(d);
    h.word(stroke_occludes_fill(shape, document) ? 1u : 0u);
    for (std::uint32_t c : shape.clips) {
        h.word(c);
        if (c < document.clip_paths.size()) {
            const std::uint64_t ch = hash_clip(document.clip_paths[c]);
            h.word(static_cast<std::uint32_t>(ch));
            h.word(static_cast<std::uint32_t>(ch >> 32));
        }
    }
    return h.h;
}

const std::vector<Polyline>& DocumentTessellator::clip(const VectorDocument& document, std::uint32_t index, int level) {
    CachedClip& entry = clips_[index];
    const std::uint64_t key = hash_clip(document.clip_paths[index]);
    if (!entry.valid || entry.key != key || entry.level != level) {
        entry.region = clip_region(document.clip_paths[index], tolerance_for_scale(lod_scale(level)));
        entry.key = key;
        entry.level = level;
        entry.valid = true;
    }
    return entry.region;
}

void DocumentTessellator::build(const VectorDocument& document, int level, VectorMesh& out) {
    out.clear();
    last_tessellated_ = 0;
    last_reused_ = 0;
    shapes_.resize(document.shapes.size());
    clips_.resize(document.clip_paths.size());
    const float tolerance = tolerance_for_scale(lod_scale(level));

    for (std::size_t i = 0; i < document.shapes.size(); ++i) {
        const Shape& shape = document.shapes[i];
        CachedShape& entry = shapes_[i];
        const std::uint64_t key = hash_shape_geometry(shape, document);
        if (entry.valid && entry.key == key && entry.level == level) {
            ++last_reused_;
        } else {
            std::vector<Polyline> combined;
            const std::vector<Polyline>* clip_ptr = nullptr;
            bool valid_clips = true;
            for (std::size_t k = 0; k < shape.clips.size(); ++k) {
                if (shape.clips[k] >= document.clip_paths.size()) {
                    valid_clips = false;
                    break;
                }
                const std::vector<Polyline>& region = clip(document, shape.clips[k], level);
                combined = k == 0 ? region : intersect_regions(combined, region);
            }
            if (valid_clips && !shape.clips.empty()) {
                clip_ptr = &combined;
            }
            tessellate_shape(shape, tolerance, clip_ptr, entry.geometry, stroke_occludes_fill(shape, document));
            entry.key = key;
            entry.level = level;
            entry.valid = true;
            ++last_tessellated_;
        }
        const float fill_alpha = shape.fill_opacity * shape.opacity;
        const float stroke_alpha = shape.stroke_opacity * shape.opacity;
        if (shape.stroke_first) {
            append_painted(out, entry.geometry.stroke, shape.stroke, stroke_alpha, shape, document);
            append_painted(out, entry.geometry.fill, shape.fill, fill_alpha, shape, document);
        } else {
            append_painted(out, entry.geometry.fill, shape.fill, fill_alpha, shape, document);
            append_painted(out, entry.geometry.stroke, shape.stroke, stroke_alpha, shape, document);
        }
    }
}

} // namespace cuervo::vector

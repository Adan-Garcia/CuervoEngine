#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "cuervo/math/vec.hpp"
#include "cuervo/vector/document.hpp"
#include "cuervo/vector/tessellate.hpp"

// Vector meshes (DESIGN.md 4.4 "SVG art"): a whole document tessellated into
// one merged triangle mesh with per-vertex paint, ready for
// render::MeshBatcher. Gradients are not baked into vertex colors: each
// vertex points at a gradient block that the fragment shader evaluates, so
// gradients stay smooth at any zoom.
namespace cuervo::vector {

// Must match cuervo/render/shaders/mesh.vert (stream 0).
struct MeshVertex {
    vec2 position;            // document space
    std::uint32_t color = 0;  // straight-alpha RGBA8 (bytes r, g, b, a); multiplies the gradient for gradient paint
    std::uint32_t paint = 0;  // solid_paint, or the vec4 offset of a gradient block in the batch's paint window
};
static_assert(sizeof(MeshVertex) == 16);

inline constexpr std::uint32_t solid_paint = 0xFFFFFFFFu;

// Paint windows are bound as one std140 uniform block of this many vec4s:
// 16 KiB, the largest uniform block GLES 3.0 / WebGL2 guarantee.
inline constexpr std::uint32_t paint_window_vec4 = 1024;
// Gradients with more stops are resampled down to this many.
inline constexpr std::uint32_t max_gradient_stops = 64;

// Gradient block layout in the paint window (vec4 units, all floats):
//   [0] inverse paint transform row 0 (a, c, tx), w = kind (0 linear, 1 radial)
//   [1] inverse paint transform row 1 (b, d, ty), w = spread (0 pad, 1 reflect, 2 repeat)
//   [2] linear: (x1, y1, x2, y2)   radial: (cx, cy, fx, fy)
//   [3] (radius, stop count, 0, 0)
//   [4 .. 4+n)          stop colors, premultiplied sRGB
//   [4+n .. 4+n+n/4)    stop offsets, four per vec4
// The inverse transform maps document space to gradient space.
std::uint32_t gradient_block_size(std::uint32_t stop_count);

// A run of indices drawn with one paint window: one draw call ("material batch").
struct MeshBatch {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::vector<std::array<float, 4>> paints; // <= paint_window_vec4 entries
    friend bool operator==(const MeshBatch&, const MeshBatch&) = default;
};

struct VectorMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MeshBatch> batches; // cover `indices` in order
    Bounds bounds;

    void clear() {
        vertices.clear();
        indices.clear();
        batches.clear();
        bounds = {};
    }
    std::size_t triangle_count() const { return indices.size() / 3; }
};

bool operator==(const MeshVertex& a, const MeshVertex& b);

// Packs a straight-alpha color as bytes r, g, b, a (R8G8B8A8_UNORM), with
// `alpha_scale` applied to alpha.
std::uint32_t pack_color(const Color& color, float alpha_scale = 1.0f);

// Appends `triangles` painted with `paint` (opacity `alpha`) to `mesh`.
// `shape` gives the gradient's bounding box and transform. Returns false if the
// paint renders nothing (none, degenerate bounding box).
bool append_painted(VectorMesh& mesh, const TriMesh& triangles, const Paint& paint, float alpha, const Shape& shape,
                    const VectorDocument& document);

// Geometry of one shape in document space.
struct ShapeGeometry {
    TriMesh fill;
    TriMesh stroke;
};

// Largest stretch factor of the linear part (max singular value).
float max_scale(const Affine2& m);

// Clip region of a ClipPath in document space (normalized; empty = clips everything).
std::vector<Polyline> clip_region(const ClipPath& clip, float tolerance);

// Tessellates a shape at document-space tolerance `tolerance`. `clip` is the
// combined (intersected) clip region, or null when the shape isn't clipped.
// `stroke_occludes_fill` cuts the stroke's area out of the fill: with opacity
// < 1 and an opaque stroke this makes per-shape opacity exact (SVG composites
// fill + stroke first, then fades them; see stroke_occludes_fill()).
void tessellate_shape(const Shape& shape, float tolerance, const std::vector<Polyline>* clip, ShapeGeometry& out,
                      bool stroke_occludes_fill = false);

// True when the shape is translucent as a whole (opacity < 1) and its stroke
// fully hides the fill underneath it.
bool stroke_occludes_fill(const Shape& shape, const VectorDocument& document);

// Runtime tessellation path (DESIGN.md 4.4 "Canvas API, shape morphing"):
// rebuilds a document mesh for a zoom level, re-tessellating only shapes
// whose geometry (path, transform, stroke style, fill rule, clips) or level
// changed since the last build. Paint changes need no re-tessellation.
class DocumentTessellator {
public:
    // `level` from lod_level(): geometry is built for scale lod_scale(level).
    void build(const VectorDocument& document, int level, VectorMesh& out);

    std::size_t last_tessellated() const { return last_tessellated_; }
    std::size_t last_reused() const { return last_reused_; }
    void clear() {
        shapes_.clear();
        clips_.clear();
    }

private:
    struct CachedShape {
        std::uint64_t key = 0;
        int level = 0;
        bool valid = false;
        ShapeGeometry geometry;
    };
    struct CachedClip {
        std::uint64_t key = 0;
        int level = 0;
        bool valid = false;
        std::vector<Polyline> region;
    };

    const std::vector<Polyline>& clip(const VectorDocument& document, std::uint32_t index, int level);

    std::vector<CachedShape> shapes_;
    std::vector<CachedClip> clips_;
    std::size_t last_tessellated_ = 0;
    std::size_t last_reused_ = 0;
};

// Geometry hash of a shape (what DocumentTessellator keys its cache on).
std::uint64_t hash_shape_geometry(const Shape& shape, const VectorDocument& document);

} // namespace cuervo::vector

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/math/affine2.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/vector/vector_asset.hpp"

// The vector images `Canvas.vector` (the `Sprite` node, DESIGN.md 4.16
// milestone 8.2) draws: each project path is loaded once through the VFS
// (SVG, or the `.evec` an export replaced it with) and tessellated per zoom
// level (vector::VectorAsset); the mesh of the level in use lives in one
// MeshBatcher. Render-only state: nothing here feeds the simulation.
//
// The cache belongs with its batcher (the runner's or the editor's), not
// with a ScriptHost, because its MeshHandles are the batcher's and hosts come
// and go (hot reload, the editor's preview and play sessions).
namespace cuervo::assets {
class Vfs;
}

namespace cuervo::script {

// A polygon or polyline drawn from script data (Canvas.polygon / polyline,
// the Polygon2D and Line2D nodes), in the node's local space.
struct ProceduralShape {
    std::vector<vec2> points;
    bool closed = false;     // a polygon, or a polyline joined back to its start
    bool has_fill = false;
    vector::Color fill;
    bool has_stroke = false;
    vector::Color stroke;
    float stroke_width = 1.0f;
    vector::LineJoin join = vector::LineJoin::Round;
    vector::LineCap cap = vector::LineCap::Round;
    // Evenly spaced colors from the first point to the last (2+ stops make a
    // linear gradient stroke, 1 a solid one); empty uses `stroke`.
    std::vector<vector::Color> gradient;
    friend bool operator==(const ProceduralShape&, const ProceduralShape&) = default;
};

// The document size of a vector file without loading it into a cache (0, 0
// when it can't be read). UI layout uses it, in the simulation.
vec2 vector_file_size(const assets::Vfs& vfs, std::string_view path);

class VectorCache {
public:
    explicit VectorCache(render::MeshBatcher& batcher) : batcher_(batcher) {}
    VectorCache(const VectorCache&) = delete;
    VectorCache& operator=(const VectorCache&) = delete;

    // Queues `path` (project-relative) drawn through `transform` (document
    // units -> world), `centered` on the transform's origin or with its
    // top-left corner there. `pixels_per_unit` is framebuffer pixels per
    // document unit (picks the tessellation level). Returns false when the
    // file can't be loaded (logged once per path).
    bool draw(const assets::Vfs& vfs, std::string_view path, const Affine2& transform, bool centered,
              rhi::Color tint, float pixels_per_unit);

    // Starts a frame of procedural shapes: each draw_shape call of the frame
    // takes the next slot, so a shape drawn in the same order every frame
    // keeps its tessellation (it is rebuilt only when the shape changes).
    void begin_shapes() { next_slot_ = 0; }
    // Queues the shape drawn through `transform` (shape space -> world).
    // `pixels_per_unit` is framebuffer pixels per shape unit. Returns false if
    // there is nothing to draw.
    bool draw_shape(const ProceduralShape& shape, const Affine2& transform, float pixels_per_unit);

    // The document size of a loaded (or loadable) image, (0, 0) otherwise.
    vec2 size(const assets::Vfs& vfs, std::string_view path);

    render::MeshBatcher& batcher() { return batcher_; }
    std::size_t loaded() const { return entries_.size(); }

private:
    struct Entry {
        bool ok = false;
        vector::VectorAsset asset;
        render::MeshHandle mesh;
        std::uint64_t revision = 0;
    };
    Entry& entry(const assets::Vfs& vfs, std::string_view path);

    struct Slot {
        ProceduralShape shape;
        bool valid = false;
        vector::VectorAsset asset;
        render::MeshHandle mesh;
        std::uint64_t revision = 0;
    };
    std::vector<std::unique_ptr<Slot>> slots_;
    std::size_t next_slot_ = 0;

    render::MeshBatcher& batcher_;
    // Ordered, not hashed: iteration order never matters here, but the
    // engine avoids unordered containers by habit (DESIGN.md 4.1).
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> entries_;
};

} // namespace cuervo::script

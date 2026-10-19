#pragma once

#include <cstdint>
#include <list>
#include <string>

#include "cuervo/vector/document.hpp"
#include "cuervo/vector/imported_vector.hpp"
#include "cuervo/vector/mesh.hpp"

// A loaded vector asset plus its zoom-level mesh cache (DESIGN.md 4.4/4.5).
//
// mesh_for_scale() returns a mesh flattened for the scale it will be drawn at.
// Meshes are keyed by lod_level(): the level changes (and the mesh is rebuilt)
// only when the zoom moves past a power of two, i.e. by more than 2x, and the
// last few levels stay cached so zooming back and forth doesn't re-tessellate.
// Editing the document (morphing, script edits) re-tessellates only the shapes
// whose geometry changed (DocumentTessellator).
namespace cuervo::vector {

class VectorAsset {
public:
    struct Lod {
        const VectorMesh* mesh = nullptr;
        int level = 0;
        // Changes whenever the returned mesh's contents differ from the
        // previous call's (new level, or rebuilt after an edit): re-upload then.
        std::uint64_t revision = 0;
    };

    VectorAsset() = default;
    explicit VectorAsset(ImportedVector imported) { assign(std::move(imported)); }

    // SVG or ImportedVector file (see load_vector_file).
    bool load(const std::string& path);
    void assign(ImportedVector imported);

    const VectorDocument& document() const { return document_; }
    // Mutable access for runtime edits; call document_changed() afterwards.
    VectorDocument& edit_document() { return document_; }
    void document_changed();

    // `pixels_per_unit`: framebuffer pixels per document unit at draw time.
    Lod mesh_for_scale(float pixels_per_unit);

    std::size_t cached_levels() const { return lods_.size(); }
    std::size_t builds() const { return builds_; }
    const DocumentTessellator& tessellator() const { return tessellator_; }

    static constexpr std::size_t max_cached_levels = 4;

private:
    struct Entry {
        int level = 0;
        std::uint64_t revision = 0;
        VectorMesh mesh;
    };

    VectorDocument document_;
    std::list<Entry> lods_; // most recently used first
    DocumentTessellator tessellator_;
    std::uint64_t next_revision_ = 1;
    std::size_t builds_ = 0;
};

} // namespace cuervo::vector

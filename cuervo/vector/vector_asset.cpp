#include "cuervo/vector/vector_asset.hpp"

#include "cuervo/vector/flatten.hpp"

namespace cuervo::vector {

bool VectorAsset::load(const std::string& path) {
    auto imported = load_vector_file(path);
    if (!imported) {
        return false;
    }
    assign(std::move(*imported));
    return true;
}

void VectorAsset::assign(ImportedVector imported) {
    document_ = std::move(imported.document);
    lods_.clear();
    tessellator_.clear();
    if (imported.base_mesh) {
        // The importer's pre-tessellated level: no tessellation at load time.
        lods_.push_front({imported.base_mesh->level, next_revision_++, std::move(imported.base_mesh->mesh)});
    }
}

void VectorAsset::document_changed() {
    // Every cached level is stale; the tessellator still reuses unchanged shapes.
    lods_.clear();
}

VectorAsset::Lod VectorAsset::mesh_for_scale(float pixels_per_unit) {
    const int level = lod_level(pixels_per_unit);
    for (auto it = lods_.begin(); it != lods_.end(); ++it) {
        if (it->level == level) {
            lods_.splice(lods_.begin(), lods_, it);
            return {&lods_.front().mesh, level, lods_.front().revision};
        }
    }
    Entry entry;
    entry.level = level;
    entry.revision = next_revision_++;
    tessellator_.build(document_, level, entry.mesh);
    ++builds_;
    lods_.push_front(std::move(entry));
    while (lods_.size() > max_cached_levels) {
        lods_.pop_back();
    }
    return {&lods_.front().mesh, level, lods_.front().revision};
}

} // namespace cuervo::vector

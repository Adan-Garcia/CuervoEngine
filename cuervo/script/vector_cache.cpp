#include "cuervo/script/vector_cache.hpp"

#include <format>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/vector/imported_vector.hpp"

namespace cuervo::script {

namespace {

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// The file's bytes as an ImportedVector: `.evec` data directly, SVG through
// the importer (unless this build compiled SVG import out).
bool decode(std::span<const std::byte> data, std::string_view path, vector::ImportedVector& out, std::string& error) {
    if (vector::is_imported_vector(data)) return vector::deserialize_imported_vector(data, out, &error);
#ifdef CUERVO_NO_SVG_IMPORT
    (void)path;
    error = "not an .evec file (SVG import is compiled out of this build)";
    return false;
#else
    auto imported = vector::import_svg(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), path);
    if (!imported) {
        error = "SVG import failed";
        return false;
    }
    out = std::move(*imported);
    return true;
#endif
}

} // namespace

namespace {

// The file at `path` as an ImportedVector (an export imports SVG to .evec
// under the same name, docs/EXPORT.md).
bool load_imported(const assets::Vfs& vfs, std::string_view path, vector::ImportedVector& out, std::string& error) {
    std::string rel;
    if (!assets::Vfs::normalize(path, rel, error)) return false;
    auto file = vfs.open(rel, error);
    if (!file && ends_with(rel, ".svg")) {
        std::string evec_error;
        file = vfs.open(rel.substr(0, rel.size() - 4) + ".evec", evec_error);
    }
    return file && decode(file->bytes(), rel, out, error);
}

} // namespace

vec2 vector_file_size(const assets::Vfs& vfs, std::string_view path) {
    vector::ImportedVector imported;
    std::string error;
    if (!load_imported(vfs, path, imported, error)) return {0.0f, 0.0f};
    vector::VectorAsset asset;
    asset.assign(std::move(imported));
    return asset.document().size;
}

VectorCache::Entry& VectorCache::entry(const assets::Vfs& vfs, std::string_view path) {
    auto it = entries_.find(path);
    if (it != entries_.end()) return *it->second;
    auto e = std::make_unique<Entry>();
    std::string error;
    vector::ImportedVector imported;
    if (load_imported(vfs, path, imported, error)) {
        e->asset.assign(std::move(imported));
        e->ok = true;
    }
    if (!e->ok) LOG_WARN("Canvas.vector: can't load '{}': {}", path, error);
    Entry& ref = *e;
    entries_.emplace(std::string(path), std::move(e));
    return ref;
}

bool VectorCache::draw(const assets::Vfs& vfs, std::string_view path, const Affine2& transform, bool centered,
                       rhi::Color tint, float pixels_per_unit) {
    Entry& e = entry(vfs, path);
    if (!e.ok) return false;
    const vector::VectorAsset::Lod lod = e.asset.mesh_for_scale(pixels_per_unit);
    if (lod.mesh == nullptr) return false;
    if (!e.mesh) {
        e.mesh = batcher_.create_mesh(*lod.mesh);
        e.revision = lod.revision;
    } else if (e.revision != lod.revision) {
        batcher_.update_mesh(e.mesh, *lod.mesh);
        e.revision = lod.revision;
    }
    Affine2 m = transform;
    if (centered) m = m * Affine2::translate(e.asset.document().size * -0.5f);
    batcher_.draw(e.mesh, m, tint);
    return true;
}

bool VectorCache::draw_shape(const ProceduralShape& shape, const Affine2& transform, float pixels_per_unit) {
    if (shape.points.size() < 2 || (!shape.has_fill && !shape.has_stroke && shape.gradient.empty())) {
        ++next_slot_;
        return false;
    }
    if (next_slot_ >= slots_.size()) slots_.push_back(std::make_unique<Slot>());
    Slot& slot = *slots_[next_slot_++];
    if (!slot.valid || !(slot.shape == shape)) {
        slot.shape = shape;
        slot.valid = true;
        vector::VectorDocument& doc = slot.asset.edit_document();
        doc = {};
        vector::Shape s;
        s.path.move_to(shape.points[0]);
        for (std::size_t i = 1; i < shape.points.size(); ++i) s.path.line_to(shape.points[i]);
        if (shape.closed) s.path.close();
        if (shape.has_fill) {
            s.fill.kind = vector::PaintKind::Solid;
            s.fill.color = shape.fill;
        }
        if (shape.has_stroke || !shape.gradient.empty()) {
            if (shape.gradient.size() >= 2 && shape.points.front() != shape.points.back()) {
                vector::Gradient g;
                g.bbox_units = false;
                g.p0 = shape.points.front();
                g.p1 = shape.points.back();
                const std::size_t n = shape.gradient.size();
                for (std::size_t i = 0; i < n; ++i) {
                    g.stops.push_back({static_cast<float>(i) / static_cast<float>(n - 1), shape.gradient[i]});
                }
                doc.gradients.push_back(std::move(g));
                s.stroke.kind = vector::PaintKind::Gradient;
                s.stroke.gradient = 0;
            } else {
                s.stroke.kind = vector::PaintKind::Solid;
                s.stroke.color = shape.gradient.empty() ? shape.stroke : shape.gradient.front();
            }
            s.stroke_style.width = shape.stroke_width;
            s.stroke_style.join = shape.join;
            s.stroke_style.cap = shape.cap;
        }
        doc.shapes.push_back(std::move(s));
        slot.asset.document_changed();
    }
    const vector::VectorAsset::Lod lod = slot.asset.mesh_for_scale(pixels_per_unit);
    if (lod.mesh == nullptr || lod.mesh->indices.empty()) return false;
    if (!slot.mesh) {
        slot.mesh = batcher_.create_mesh(*lod.mesh);
        slot.revision = lod.revision;
    } else if (slot.revision != lod.revision) {
        batcher_.update_mesh(slot.mesh, *lod.mesh);
        slot.revision = lod.revision;
    }
    batcher_.draw(slot.mesh, transform, {1.0f, 1.0f, 1.0f, 1.0f});
    return true;
}

vec2 VectorCache::size(const assets::Vfs& vfs, std::string_view path) {
    Entry& e = entry(vfs, path);
    return e.ok ? e.asset.document().size : vec2{0.0f, 0.0f};
}

} // namespace cuervo::script

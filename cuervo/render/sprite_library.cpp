#include "cuervo/render/sprite_library.hpp"

#include <algorithm>
#include <cmath>

#include "cuervo/assets/svg_raster.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/vector/imported_vector.hpp"

namespace cuervo::render {

namespace {

std::string lower_ext(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    std::string e(path.substr(dot));
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string with_ext(std::string_view path, std::string_view ext) {
    return std::string(path.substr(0, path.size() - lower_ext(path).size())) + std::string(ext);
}

// First of `paths` that opens.
std::optional<assets::VfsFile> open_first(const assets::Vfs& vfs, const std::vector<std::string>& paths,
                                          std::string& error) {
    for (const std::string& p : paths) {
        std::string rel, e;
        if (!assets::Vfs::normalize(p, rel, e)) {
            error = e;
            continue;
        }
        if (auto f = vfs.open(rel, e)) return f;
        error = e;
    }
    return std::nullopt;
}

int index_of(rhi::Filter f) { return f == rhi::Filter::Nearest ? 0 : 1; }

} // namespace

void SpriteLibrary::destroy(Tex& tex) {
    if (device_ != nullptr) {
        for (rhi::TextureHandle& h : tex.handle) {
            if (h) device_->destroy_texture(h);
            h = {};
        }
    }
}

void SpriteLibrary::shutdown() {
    for (auto& [path, e] : entries_) {
        for (Tex& t : e->pages) destroy(t);
        for (auto& [k, t] : e->rasters) destroy(*t);
    }
    entries_.clear();
    device_ = nullptr;
}

std::size_t SpriteLibrary::texture_count() const {
    std::size_t n = 0;
    for (const auto& [path, e] : entries_) {
        for (const Tex& t : e->pages) n += (t.handle[0] ? 1 : 0) + (t.handle[1] ? 1 : 0);
        for (const auto& [k, t] : e->rasters) n += (t->handle[0] ? 1 : 0) + (t->handle[1] ? 1 : 0);
    }
    return n;
}

rhi::TextureHandle SpriteLibrary::upload(Tex& tex, rhi::Filter filter, std::string_view name) {
    if (device_ == nullptr) return {};
    rhi::TextureHandle& h = tex.handle[index_of(filter)];
    if (!h) h = create_sprite_texture(*device_, tex.data, filter, name);
    return h;
}

SpriteLibrary::Entry& SpriteLibrary::entry(const assets::Vfs& vfs, std::string_view asset) {
    auto it = entries_.find(asset);
    if (it != entries_.end()) return *it->second;
    auto e = std::make_unique<Entry>();
    std::string error;
    const std::string ext = lower_ext(asset);
    const std::string path(asset);
    if (ext == ".atlas" || ext == ".catlas") {
        auto packed_file = open_first(vfs, {with_ext(path, ".catlas")}, error);
        std::optional<assets::PackedAtlas> atlas;
        if (packed_file) {
            atlas = assets::decode_catlas(packed_file->bytes());
            if (!atlas) error = "corrupt .catlas";
        } else if (ext == ".atlas") {
            auto src = open_first(vfs, {path}, error);
            if (src) {
                assets::FileReader read = [&vfs](std::string_view p, std::vector<std::byte>& out, std::string& err) {
                    std::string rel;
                    if (!assets::Vfs::normalize(p, rel, err)) return false;
                    auto f = vfs.open(rel, err);
                    if (!f) return false;
                    out.assign(f->bytes().begin(), f->bytes().end());
                    return true;
                };
                assets::AtlasBuildResult built = assets::build_atlas_from_json(src->text(), read);
                for (const std::string& w : built.warnings) LOG_WARN("{}: {}", path, w);
                if (built.atlas) {
                    atlas = std::move(built.atlas);
                } else {
                    error = built.error;
                }
            }
        }
        if (atlas) {
            e->ok = !atlas->pages.empty();
            e->is_atlas = true;
            e->frames = std::move(atlas->frames);
            for (assets::TextureData& page : atlas->pages) {
                Tex t;
                t.data = std::move(page);
                e->pages.push_back(std::move(t));
            }
        }
    } else if (ext == ".svg" || ext == ".evec") {
        auto file = open_first(vfs, {path, with_ext(path, ".evec")}, error);
        if (file) {
            e->is_svg = true;
            e->svg_is_evec = vector::is_imported_vector(file->bytes());
            e->svg_source.assign(reinterpret_cast<const char*>(file->bytes().data()), file->bytes().size());
            // Size and feature check now; rasters come per scale.
            assets::SvgRasterResult r;
            if (e->svg_is_evec) {
                vector::ImportedVector vec;
                std::string err;
                if (vector::deserialize_imported_vector(file->bytes(), vec, &err)) {
                    e->doc_size = vec.document.size;
                    e->ok = e->doc_size.x() > 0.0f && e->doc_size.y() > 0.0f;
                } else {
                    error = err;
                }
            } else {
                r = assets::rasterize_svg(e->svg_source, 1.0f, path);
                if (r.raster) {
                    e->doc_size = r.raster->document_size;
                    e->ok = true;
                    if (assets::needs_raster_fallback(r.unsupported)) {
                        std::string list;
                        for (const std::string& u : r.unsupported) list += (list.empty() ? "" : ", ") + u;
                        LOG_WARN("Sprite: '{}' uses SVG features outside the supported subset ({}); drawn without them",
                                 path, list);
                    }
                } else {
                    error = r.error;
                }
            }
        }
    } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".qoi" || ext == ".ctex") {
        auto file = open_first(vfs, {path, with_ext(path, ".ctex")}, error);
        if (file) {
            Tex t;
            if (auto ctex = assets::decode_ctex(file->bytes())) {
                t.data = std::move(*ctex);
                e->ok = true;
            } else if (auto img = assets::decode_image(file->bytes()); img.image) {
                t.data = assets::build_texture(*img.image, true);
                e->ok = true;
            } else {
                error = img.error;
            }
            if (e->ok) e->pages.push_back(std::move(t));
        }
    } else {
        error = "unknown image type '" + ext + "'";
    }
    if (!e->ok) LOG_WARN("Sprite: can't load '{}': {}", path, error);
    Entry& ref = *e;
    entries_.emplace(path, std::move(e));
    return ref;
}

SpriteRef SpriteLibrary::resolve(const assets::Vfs& vfs, std::string_view asset, std::string_view frame,
                                 SpriteFilter filter, float pixels_per_unit) {
    SpriteRef ref;
    if (asset.empty()) return ref;
    Entry& e = entry(vfs, asset);
    if (!e.ok) return ref;
    const rhi::Filter f = filter == SpriteFilter::Nearest  ? rhi::Filter::Nearest
                          : filter == SpriteFilter::Linear ? rhi::Filter::Linear
                                                           : default_filter_;
    if (e.is_svg) {
        const float want = std::max(pixels_per_unit, 0.0f);
        const int bucket = want > 0.0f ? std::clamp(static_cast<int>(std::ceil(std::log2(want))), -2, 4) : 0;
        auto& slot = e.rasters[bucket];
        if (!slot) {
            slot = std::make_unique<Tex>();
            const float scale = std::ldexp(1.0f, bucket);
            std::optional<assets::RasterizedSvg> r;
            if (e.svg_is_evec) {
                vector::ImportedVector vec;
                const auto* p = reinterpret_cast<const std::byte*>(e.svg_source.data());
                if (vector::deserialize_imported_vector({p, e.svg_source.size()}, vec)) {
                    r = assets::rasterize_vector(vec, scale);
                }
            } else {
                r = assets::rasterize_svg(e.svg_source, scale, asset).raster;
            }
            if (r) slot->data = assets::build_texture(r->image, true);
        }
        if (slot->data.width <= 0) return ref;
        ref.texture = upload(*slot, f, asset);
        ref.uv = SpriteUv{};
        ref.frame_w = static_cast<float>(slot->data.width);
        ref.frame_h = static_cast<float>(slot->data.height);
        ref.size = e.doc_size;
        ref.ok = true;
        return ref;
    }
    if (e.is_atlas) {
        const assets::AtlasFrame* fr = nullptr;
        if (frame.empty()) {
            if (!e.frames.empty()) fr = &e.frames.front();
        } else {
            auto it = std::lower_bound(e.frames.begin(), e.frames.end(), frame,
                                       [](const assets::AtlasFrame& a, std::string_view n) { return a.name < n; });
            if (it != e.frames.end() && it->name == frame) fr = &*it;
        }
        if (fr == nullptr) {
            LOG_WARN("Sprite: '{}' has no frame '{}'", asset, frame);
            return ref;
        }
        Tex& page = e.pages[static_cast<std::size_t>(fr->page)];
        ref.texture = upload(page, f, asset);
        ref.uv = sprite_uv(*fr, page.data.width, page.data.height);
        ref.size = vec2(static_cast<float>(fr->w), static_cast<float>(fr->h));
        ref.frame_w = ref.size.x();
        ref.frame_h = ref.size.y();
        ref.ok = true;
        return ref;
    }
    Tex& tex = e.pages.front();
    ref.texture = upload(tex, f, asset);
    ref.uv = SpriteUv{};
    ref.size = vec2(static_cast<float>(tex.data.width), static_cast<float>(tex.data.height));
    ref.frame_w = ref.size.x();
    ref.frame_h = ref.size.y();
    ref.ok = true;
    return ref;
}

vec2 SpriteLibrary::size(const assets::Vfs& vfs, std::string_view asset, std::string_view frame) {
    if (asset.empty()) return {0.0f, 0.0f};
    // Resolving without a device never uploads; with one it would, so size
    // the entry directly.
    Entry& e = entry(vfs, asset);
    if (!e.ok) return {0.0f, 0.0f};
    if (e.is_svg) return e.doc_size;
    if (e.is_atlas) {
        for (const assets::AtlasFrame& fr : e.frames) {
            if (frame.empty() || fr.name == frame) return vec2(static_cast<float>(fr.w), static_cast<float>(fr.h));
        }
        return {0.0f, 0.0f};
    }
    return vec2(static_cast<float>(e.pages.front().data.width), static_cast<float>(e.pages.front().data.height));
}

std::vector<std::string> SpriteLibrary::frames(const assets::Vfs& vfs, std::string_view asset) {
    std::vector<std::string> out;
    Entry& e = entry(vfs, asset);
    if (e.ok && e.is_atlas) {
        for (const assets::AtlasFrame& fr : e.frames) out.push_back(fr.name);
    }
    return out;
}

} // namespace cuervo::render

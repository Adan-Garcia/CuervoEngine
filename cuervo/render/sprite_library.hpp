#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/assets/atlas_file.hpp"
#include "cuervo/assets/vfs.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/render/sprite_batcher.hpp"
#include "cuervo/rhi/rhi.hpp"

// The raster images sprites draw (DESIGN.md 4.4 "Raster pipeline", 11.2a):
// each project path is loaded once through the VFS and uploaded as an RHI
// texture, per filter mode. Render-only state, like VectorCache: nothing here
// feeds the simulation.
//
// An asset path names one of
//   - an image (`.png` `.jpg` `.qoi`; an export ships `.ctex` under the same
//     name, which is found when the source is absent),
//   - a `.ctex` texture,
//   - an atlas (`.atlas` source, or the packed `.catlas`): the `frame` argument
//     picks the named frame,
//   - an SVG (or the `.evec` an export replaced it with): rasterized by the
//     raster fallback (assets/svg_raster.hpp) at the scale it is drawn at,
//     rounded up to a power of two so zooming re-rasterizes only now and then.
namespace cuervo::render {

enum class SpriteFilter : std::uint8_t {
    Default, // the library's default (nearest under integer scaling, linear otherwise)
    Nearest, // pixel art
    Linear,  // smooth scaling (trilinear through the mip chain)
};

struct SpriteRef {
    bool ok = false;
    rhi::TextureHandle texture;
    SpriteUv uv;
    vec2 size{0.0f, 0.0f}; // natural size: the frame in texels (SVG: document units)
    float frame_w = 0.0f;  // the frame in texels of `texture` (9-slice margins are in these)
    float frame_h = 0.0f;
};

class SpriteLibrary {
public:
    SpriteLibrary() = default;
    ~SpriteLibrary() { shutdown(); }
    SpriteLibrary(const SpriteLibrary&) = delete;
    SpriteLibrary& operator=(const SpriteLibrary&) = delete;

    // Without a device (tests, headless) resolve() still loads and sizes
    // images; textures are null handles and `ok` stays true.
    void init(rhi::Device* device) { device_ = device; }
    void shutdown();

    // Filter used by SpriteFilter::Default. The runner sets Nearest for the
    // Integer scale mode (crisp pixel art), Linear otherwise.
    void set_default_filter(rhi::Filter f) { default_filter_ = f; }
    rhi::Filter default_filter() const { return default_filter_; }

    // `frame` picks an atlas frame ("" = the whole image; for an atlas, the
    // first frame). `pixels_per_unit` (framebuffer pixels per world unit)
    // matters for SVG only. Failures are logged once per path.
    SpriteRef resolve(const assets::Vfs& vfs, std::string_view asset, std::string_view frame, SpriteFilter filter,
                      float pixels_per_unit);

    // Natural size of the asset's frame (0, 0 when it can't be loaded).
    vec2 size(const assets::Vfs& vfs, std::string_view asset, std::string_view frame);

    // Frame names of an atlas, sorted (empty for anything else).
    std::vector<std::string> frames(const assets::Vfs& vfs, std::string_view asset);

    std::size_t loaded() const { return entries_.size(); }
    std::size_t texture_count() const;

private:
    struct Tex {
        assets::TextureData data; // kept for re-upload per filter
        rhi::TextureHandle handle[2];
    };
    struct Entry {
        bool ok = false;
        bool is_svg = false;
        bool is_atlas = false;
        std::vector<Tex> pages;                // image: one; atlas: one per page
        std::vector<assets::AtlasFrame> frames; // atlas
        vec2 doc_size{0.0f, 0.0f};              // svg
        std::string svg_source;                 // SVG text or .evec bytes, kept for re-raster
        bool svg_is_evec = false;
        std::map<int, std::unique_ptr<Tex>> rasters; // svg: by log2 of the raster scale
    };

    Entry& entry(const assets::Vfs& vfs, std::string_view asset);
    rhi::TextureHandle upload(Tex& tex, rhi::Filter filter, std::string_view name);
    void destroy(Tex& tex);

    rhi::Device* device_ = nullptr;
    rhi::Filter default_filter_ = rhi::Filter::Linear;
    // Ordered, not hashed (DESIGN.md 4.1 habit).
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> entries_;
};

} // namespace cuervo::render

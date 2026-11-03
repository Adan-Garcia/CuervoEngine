#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "cuervo/assets/atlas.hpp"
#include "cuervo/assets/image.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/rhi/rhi.hpp"

// Instanced textured quads (DESIGN.md 4.4 "Raster pipeline", 11.2a): sprites,
// atlas frames and the pieces of 9-slices. One pipeline, one unit quad, one
// instance per sprite; consecutive sprites that use the same texture (an atlas
// page) are one instanced draw, and the draws of a flush share one buffer
// upload. Layering follows the SDF batcher: flush() between other batchers'
// flushes to interleave them.
//
// Coordinates are world units (y down), angles radians (clockwise on screen);
// see camera.hpp. Colors are display-referred like the vector batchers': atlas
// bytes are sampled as stored (RHI textures made by create_sprite_texture are
// not sRGB-decoded), tint multiplies them, and blending is premultiplied.
namespace cuervo::render {

// Texture rectangle of a sprite, normalized, v = 0 at the top row.
struct SpriteUv {
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
};

// Source rectangle in texels of a `texture_w` x `texture_h` texture. Flips swap
// the corresponding pair, so mirrored sprites cost nothing.
SpriteUv sprite_uv(std::int32_t texture_w, std::int32_t texture_h, std::int32_t x, std::int32_t y, std::int32_t w,
                   std::int32_t h, bool flip_x = false, bool flip_y = false);
SpriteUv sprite_uv(const assets::AtlasFrame& frame, std::int32_t page_w, std::int32_t page_h, bool flip_x = false,
                   bool flip_y = false);

// GPU instance layout; must match cuervo/render/shaders/sprite.vert (stream 1).
struct SpriteInstance {
    vec2 center;
    vec2 half_size;
    float rotation = 0.0f;
    std::uint32_t color = 0xffffffffu; // tint, RGBA8 (pack_rgba8)
    float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // u0, v0 (corner 0, 0), u1, v1 (corner 1, 1)
};
static_assert(sizeof(SpriteInstance) == 40, "SpriteInstance must match the sprite.vert instance stream");

SpriteInstance make_sprite(vec2 center, vec2 size, float rotation, const SpriteUv& uv,
                           rhi::Color tint = {1.0f, 1.0f, 1.0f, 1.0f});

// std140 uniform block `Globals` of sprite.vert.
struct SpriteGlobals {
    float row0[4];
    float row1[4];
};
static_assert(sizeof(SpriteGlobals) == 32);

SpriteGlobals make_sprite_globals(const Affine2& world_to_ndc);

// Uploads a texture (with its mip chain) for the sprite pipeline. `filter`
// is the sprite filter mode: Nearest for pixel art, Linear (trilinear when the
// texture has mips) for smooth scaling. The texture is not sRGB-decoded by the
// GPU (see above).
rhi::TextureHandle create_sprite_texture(rhi::Device& device, const assets::TextureData& data, rhi::Filter filter,
                                         std::string_view debug_name = "sprite");

// Border widths of a 9-slice, in source texels.
struct NinePatchMargins {
    float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
};

class SpriteBatcher;

// Queues a 9-slice: the source frame `uv` (a `frame_w` x `frame_h` texel
// rectangle) stretched over a `size` rectangle centered on `center` and turned
// by `rotation`. The corners keep their texel size (times `border_scale`, so a
// scaled node keeps its proportions), the edges stretch along one axis, the
// middle along both (skipped when `draw_center` is false). Borders that don't
// fit shrink proportionally. Returns the number of pieces queued.
int add_nine_patch(SpriteBatcher& batcher, rhi::TextureHandle texture, const SpriteUv& uv, float frame_w,
                   float frame_h, const NinePatchMargins& margins, vec2 center, vec2 size, float rotation,
                   rhi::Color tint = {1.0f, 1.0f, 1.0f, 1.0f}, bool draw_center = true, float border_scale = 1.0f);

class SpriteBatcher {
public:
    SpriteBatcher() = default;
    ~SpriteBatcher();
    SpriteBatcher(const SpriteBatcher&) = delete;
    SpriteBatcher& operator=(const SpriteBatcher&) = delete;

    // Creates the pipeline and buffers. Without init() the batcher still
    // collects instances (useful for tests) but flush() draws nothing.
    bool init(rhi::Device& device, std::size_t initial_capacity = 1024);
    void shutdown();

    void clear() {
        instances_.clear();
        runs_.clear();
        flushed_ = 0;
    }

    // Appends a sprite drawn from `texture`; a different texture than the
    // previous sprite's starts a new draw, so sort by texture to batch.
    void add(rhi::TextureHandle texture, const SpriteInstance& instance);
    void add(rhi::TextureHandle texture, vec2 center, vec2 size, float rotation, const SpriteUv& uv,
             rhi::Color tint = {1.0f, 1.0f, 1.0f, 1.0f}) {
        add(texture, make_sprite(center, size, rotation, uv, tint));
    }

    std::size_t size() const { return instances_.size(); }
    std::size_t run_count() const { return runs_.size(); } // draws a full flush would issue
    const std::vector<SpriteInstance>& instances() const { return instances_; }

    // Uploads the sprites added since the previous flush (or clear) and draws
    // them, one instanced draw per texture run, into the device's current
    // viewport. Call between begin_frame and end_frame.
    void flush(const Affine2& world_to_ndc);

    std::size_t capacity() const { return capacity_; }

private:
    struct Run {
        rhi::TextureHandle texture;
        std::size_t first = 0;
        std::size_t count = 0;
    };

    bool ensure_capacity(std::size_t count);

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle quad_vertices_;
    rhi::BufferHandle quad_indices_;
    rhi::BufferHandle instance_buffer_;
    rhi::BufferHandle globals_;
    std::size_t capacity_ = 0;
    std::vector<SpriteInstance> instances_;
    std::vector<Run> runs_;
    std::size_t flushed_ = 0; // instances already drawn this frame
};

} // namespace cuervo::render

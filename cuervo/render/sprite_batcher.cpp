#include "cuervo/render/sprite_batcher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>

#include "cuervo/core/log.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "shaders/sprite_frag.hpp"
#include "shaders/sprite_vert.hpp"

namespace cuervo::render {

namespace {

static_assert(offsetof(SpriteInstance, center) == 0);
static_assert(offsetof(SpriteInstance, half_size) == 8);
static_assert(offsetof(SpriteInstance, rotation) == 16);
static_assert(offsetof(SpriteInstance, color) == 20);
static_assert(offsetof(SpriteInstance, uv) == 24);

constexpr std::array<float, 8> quad_corners{0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
constexpr std::array<std::uint16_t, 6> quad_index_data{0, 1, 2, 0, 2, 3};

constexpr std::array<rhi::VertexAttribute, 1> corner_attributes{{{0, rhi::VertexFormat::Float2, 0}}};
constexpr std::array<rhi::VertexAttribute, 5> instance_attributes{{
    {1, rhi::VertexFormat::Float2, offsetof(SpriteInstance, center)},
    {2, rhi::VertexFormat::Float2, offsetof(SpriteInstance, half_size)},
    {3, rhi::VertexFormat::Float1, offsetof(SpriteInstance, rotation)},
    {4, rhi::VertexFormat::UByte4Norm, offsetof(SpriteInstance, color)},
    {5, rhi::VertexFormat::Float4, offsetof(SpriteInstance, uv)},
}};

} // namespace

SpriteUv sprite_uv(std::int32_t texture_w, std::int32_t texture_h, std::int32_t x, std::int32_t y, std::int32_t w,
                   std::int32_t h, bool flip_x, bool flip_y) {
    const float iw = texture_w > 0 ? 1.0f / static_cast<float>(texture_w) : 0.0f;
    const float ih = texture_h > 0 ? 1.0f / static_cast<float>(texture_h) : 0.0f;
    SpriteUv uv{static_cast<float>(x) * iw, static_cast<float>(y) * ih, static_cast<float>(x + w) * iw,
                static_cast<float>(y + h) * ih};
    if (flip_x) std::swap(uv.u0, uv.u1);
    if (flip_y) std::swap(uv.v0, uv.v1);
    return uv;
}

SpriteUv sprite_uv(const assets::AtlasFrame& frame, std::int32_t page_w, std::int32_t page_h, bool flip_x,
                   bool flip_y) {
    return sprite_uv(page_w, page_h, frame.x, frame.y, frame.w, frame.h, flip_x, flip_y);
}

SpriteInstance make_sprite(vec2 center, vec2 size, float rotation, const SpriteUv& uv, rhi::Color tint) {
    SpriteInstance instance;
    instance.center = center;
    instance.half_size = vec2(std::max(size.x(), 0.0f) * 0.5f, std::max(size.y(), 0.0f) * 0.5f);
    instance.rotation = rotation;
    instance.color = pack_rgba8(tint);
    instance.uv[0] = uv.u0;
    instance.uv[1] = uv.v0;
    instance.uv[2] = uv.u1;
    instance.uv[3] = uv.v1;
    return instance;
}

int add_nine_patch(SpriteBatcher& batcher, rhi::TextureHandle texture, const SpriteUv& uv, float frame_w,
                   float frame_h, const NinePatchMargins& m, vec2 center, vec2 size, float rotation, rhi::Color tint,
                   bool draw_center, float border_scale) {
    if (!(size.x() > 0.0f) || !(size.y() > 0.0f) || !(frame_w > 0.0f) || !(frame_h > 0.0f)) return 0;
    const float ml = std::clamp(m.left, 0.0f, frame_w), mr = std::clamp(m.right, 0.0f, frame_w);
    const float mt = std::clamp(m.top, 0.0f, frame_h), mb = std::clamp(m.bottom, 0.0f, frame_h);
    const float bs = std::max(border_scale, 0.0f);
    float l = ml * bs, r = mr * bs, t = mt * bs, b = mb * bs;
    // Borders wider than the rectangle shrink together.
    if (l + r > size.x()) {
        const float k = size.x() / (l + r);
        l *= k;
        r *= k;
    }
    if (t + b > size.y()) {
        const float k = size.y() / (t + b);
        t *= k;
        b *= k;
    }
    // Column and row edges: destination offsets from the rectangle's top-left, and uv cuts.
    const float dx[4] = {0.0f, l, size.x() - r, size.x()};
    const float dy[4] = {0.0f, t, size.y() - b, size.y()};
    const float uw = uv.u1 - uv.u0, vh = uv.v1 - uv.v0;
    const float ux[4] = {uv.u0, uv.u0 + uw * ml / frame_w, uv.u0 + uw * (frame_w - mr) / frame_w, uv.u1};
    const float vy[4] = {uv.v0, uv.v0 + vh * mt / frame_h, uv.v0 + vh * (frame_h - mb) / frame_h, uv.v1};
    const float c = std::cos(rotation), s = std::sin(rotation);
    int count = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (row == 1 && col == 1 && !draw_center) continue;
            const float w = dx[col + 1] - dx[col], h = dy[row + 1] - dy[row];
            if (!(w > 0.0f) || !(h > 0.0f)) continue;
            // Piece center relative to the rectangle center, then turned.
            const float px = dx[col] + w * 0.5f - size.x() * 0.5f;
            const float py = dy[row] + h * 0.5f - size.y() * 0.5f;
            const vec2 at(center.x() + px * c - py * s, center.y() + px * s + py * c);
            batcher.add(texture, at, vec2(w, h), rotation, SpriteUv{ux[col], vy[row], ux[col + 1], vy[row + 1]}, tint);
            ++count;
        }
    }
    return count;
}

SpriteGlobals make_sprite_globals(const Affine2& m) {
    return {{m.a, m.c, m.tx, 0.0f}, {m.b, m.d, m.ty, 0.0f}};
}

rhi::TextureHandle create_sprite_texture(rhi::Device& device, const assets::TextureData& data, rhi::Filter filter,
                                         std::string_view debug_name) {
    if (data.width <= 0 || data.height <= 0 || data.level_offsets.empty()) {
        return {};
    }
    std::vector<std::span<const std::byte>> levels;
    levels.reserve(data.level_offsets.size());
    for (std::int32_t level = 0; level < data.mip_count(); ++level) {
        const std::span<const std::uint8_t> bytes = data.level(level);
        levels.emplace_back(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
    }
    rhi::TextureDesc desc{data.width, data.height, rhi::TextureFormat::RGBA8, filter, nullptr, debug_name};
    desc.mip_levels = static_cast<std::uint32_t>(data.mip_count());
    // Linear color: color textures decode to linear when sampled (4.19).
    desc.srgb = data.srgb && device.info().linear_color;
    desc.initial_levels = levels;
    return device.create_texture(desc);
}

SpriteBatcher::~SpriteBatcher() { shutdown(); }

bool SpriteBatcher::init(rhi::Device& device, std::size_t initial_capacity) {
    shutdown();
    device_ = &device;

    const std::array<rhi::VertexStream, 2> streams{{
        {sizeof(float) * 2, rhi::VertexRate::PerVertex, corner_attributes},
        {sizeof(SpriteInstance), rhi::VertexRate::PerInstance, instance_attributes},
    }};
    rhi::PipelineDesc desc;
    desc.vertex = shaders::sprite_vert;
    desc.fragment = shaders::sprite_frag;
    desc.streams = streams;
    desc.blend = rhi::BlendMode::Premultiplied; // sprite.frag premultiplies
    desc.debug_name = "sprite";
    pipeline_ = device.create_pipeline(desc);

    quad_vertices_ = device.create_buffer({rhi::BufferKind::Vertex, rhi::BufferUsage::Static, sizeof(quad_corners),
                                           quad_corners.data()});
    quad_indices_ = device.create_buffer({rhi::BufferKind::Index, rhi::BufferUsage::Static, sizeof(quad_index_data),
                                          quad_index_data.data()});
    globals_ = device.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, sizeof(SpriteGlobals), nullptr});
    if (!pipeline_ || !quad_vertices_ || !quad_indices_ || !globals_ || !ensure_capacity(initial_capacity)) {
        LOG_ERROR("SpriteBatcher: failed to create GPU resources");
        shutdown();
        return false;
    }
    return true;
}

void SpriteBatcher::shutdown() {
    if (!device_) {
        return;
    }
    device_->destroy_pipeline(pipeline_);
    device_->destroy_buffer(quad_vertices_);
    device_->destroy_buffer(quad_indices_);
    device_->destroy_buffer(instance_buffer_);
    device_->destroy_buffer(globals_);
    pipeline_ = {};
    quad_vertices_ = quad_indices_ = instance_buffer_ = globals_ = {};
    capacity_ = 0;
    device_ = nullptr;
}

bool SpriteBatcher::ensure_capacity(std::size_t count) {
    if (instance_buffer_ && count <= capacity_) {
        return true;
    }
    // Grows by a quarter like the SDF batcher: the stream buffer exists in
    // several versions per backend.
    const std::size_t capacity = std::max({count, capacity_ + capacity_ / 4, std::size_t{256}});
    device_->destroy_buffer(instance_buffer_); // deferred: recorded draws keep the old one
    instance_buffer_ = device_->create_buffer(
        {rhi::BufferKind::Vertex, rhi::BufferUsage::Stream, capacity * sizeof(SpriteInstance), nullptr});
    capacity_ = instance_buffer_ ? capacity : 0;
    return static_cast<bool>(instance_buffer_);
}

void SpriteBatcher::add(rhi::TextureHandle texture, const SpriteInstance& instance) {
    if (runs_.empty() || runs_.back().texture.id != texture.id) {
        runs_.push_back({texture, instances_.size(), 0});
    }
    ++runs_.back().count;
    instances_.push_back(instance);
}

void SpriteBatcher::flush(const Affine2& world_to_ndc) {
    const std::size_t first = flushed_;
    flushed_ = instances_.size();
    if (!device_ || first == instances_.size() || !ensure_capacity(instances_.size())) {
        return;
    }
    SpriteGlobals globals = make_sprite_globals(world_to_ndc);
    globals.row0[3] = device_->info().linear_color ? 1.0f : 0.0f;
    device_->update_buffer(globals_, 0, std::as_bytes(std::span(&globals, 1)));
    // Only this flush's range: earlier flushes' draws keep reading theirs.
    device_->update_buffer(instance_buffer_, first * sizeof(SpriteInstance),
                           std::as_bytes(std::span(instances_).subspan(first)));

    device_->bind_pipeline(pipeline_);
    device_->bind_vertex_buffer(0, quad_vertices_);
    device_->bind_vertex_buffer(1, instance_buffer_);
    device_->bind_index_buffer(quad_indices_, rhi::IndexFormat::UInt16);
    device_->bind_uniform_buffer(0, globals_);
    for (const Run& run : runs_) {
        const std::size_t end = run.first + run.count;
        if (end <= first) {
            continue; // drawn by an earlier flush
        }
        const std::size_t start = std::max(run.first, first);
        device_->bind_texture(0, run.texture);
        device_->draw_indexed(static_cast<std::uint32_t>(quad_index_data.size()),
                              static_cast<std::uint32_t>(end - start), 0, static_cast<std::uint32_t>(start));
    }
}

} // namespace cuervo::render

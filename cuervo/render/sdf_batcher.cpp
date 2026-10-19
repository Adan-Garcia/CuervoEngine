#include "cuervo/render/sdf_batcher.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <utility>

#include "cuervo/core/log.hpp"
#include "cuervo/math/dmath.hpp"
#include "shaders/sdf_frag.hpp"
#include "shaders/sdf_vert.hpp"

namespace cuervo::render {

namespace {

static_assert(std::endian::native == std::endian::little, "pack_rgba8 assumes a little-endian host");
static_assert(offsetof(SdfInstance, center) == 0);
static_assert(offsetof(SdfInstance, half_size) == 8);
static_assert(offsetof(SdfInstance, rotation) == 16);
static_assert(offsetof(SdfInstance, shape) == 28);
static_assert(offsetof(SdfInstance, color) == 32);
static_assert(offsetof(SdfInstance, outline_color) == 36);

std::uint32_t to_byte(float v) {
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
}

SdfInstance make(SdfShape shape, vec2 center, vec2 half_size, float rotation, float param, rhi::Color fill,
                 const SdfOutline& outline) {
    SdfInstance instance;
    instance.center = center;
    instance.half_size = half_size;
    instance.rotation = rotation;
    instance.param = param;
    instance.outline_width = std::max(outline.width, 0.0f);
    instance.shape = shape;
    instance.color = pack_rgba8(fill);
    instance.outline_color = pack_rgba8(outline.color);
    return instance;
}

constexpr std::array<float, 8> quad_corners{-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
constexpr std::array<std::uint16_t, 6> quad_index_data{0, 1, 2, 0, 2, 3};

constexpr std::array<rhi::VertexAttribute, 1> corner_attributes{{{0, rhi::VertexFormat::Float2, 0}}};
constexpr std::array<rhi::VertexAttribute, 6> instance_attributes{{
    {1, rhi::VertexFormat::Float2, offsetof(SdfInstance, center)},
    {2, rhi::VertexFormat::Float2, offsetof(SdfInstance, half_size)},
    {3, rhi::VertexFormat::Float3, offsetof(SdfInstance, rotation)}, // rotation, param, outline_width
    {4, rhi::VertexFormat::UInt1, offsetof(SdfInstance, shape)},
    {5, rhi::VertexFormat::UByte4Norm, offsetof(SdfInstance, color)},
    {6, rhi::VertexFormat::UByte4Norm, offsetof(SdfInstance, outline_color)},
}};

} // namespace

std::uint32_t pack_rgba8(rhi::Color color) {
    return to_byte(color.r) | (to_byte(color.g) << 8) | (to_byte(color.b) << 16) | (to_byte(color.a) << 24);
}

SdfInstance make_circle(vec2 center, float radius, rhi::Color fill, const SdfOutline& outline) {
    const float r = std::max(radius, 0.0f);
    return make(SdfShape::Circle, center, vec2(r), 0.0f, 0.0f, fill, outline);
}

SdfInstance make_capsule(vec2 center, float half_length, float radius, float rotation, rhi::Color fill,
                         const SdfOutline& outline) {
    const float r = std::max(radius, 0.0f);
    return make(SdfShape::Capsule, center, {std::max(half_length, 0.0f) + r, r}, rotation, 0.0f, fill, outline);
}

SdfInstance make_rounded_rect(vec2 center, vec2 half_size, float corner_radius, float rotation, rhi::Color fill,
                              const SdfOutline& outline) {
    const vec2 half = max(half_size, vec2(0.0f));
    const float corner = std::clamp(corner_radius, 0.0f, std::min(half.x(), half.y()));
    return make(SdfShape::RoundedRect, center, half, rotation, corner, fill, outline);
}

SdfInstance make_ring(vec2 center, float radius, float thickness, rhi::Color fill, const SdfOutline& outline) {
    const float r = std::max(radius, 0.0f);
    return make(SdfShape::Ring, center, vec2(r), 0.0f, std::clamp(thickness, 0.0f, r), fill, outline);
}

namespace {

// Rotation (the sweep's middle) and half-angle of an arc, either direction.
std::pair<float, float> arc_frame(float start_angle, float end_angle) {
    const float sweep = dmath::abs(end_angle - start_angle);
    const float half_angle = std::min(sweep * 0.5f, dmath::pi);
    return {std::min(start_angle, end_angle) + half_angle, half_angle};
}

} // namespace

SdfInstance make_sector(vec2 center, float radius, float start_angle, float end_angle, rhi::Color fill,
                        const SdfOutline& outline) {
    const auto [mid_angle, half_angle] = arc_frame(start_angle, end_angle);
    return make(SdfShape::Sector, center, vec2(std::max(radius, 0.0f)), mid_angle, half_angle, fill, outline);
}

SdfInstance make_arc(vec2 center, float radius, float start_angle, float end_angle, float width, rhi::Color color) {
    const auto [mid_angle, half_angle] = arc_frame(start_angle, end_angle);
    const float t = std::max(width, 0.0f) * 0.5f;
    return make(SdfShape::Arc, center, vec2(std::max(radius, 0.0f) + t, t), mid_angle, half_angle, color, {});
}

SdfGlobals make_sdf_globals(const Affine2& m, float world_units_per_pixel) {
    return {{m.a, m.c, m.tx, 0.0f}, {m.b, m.d, m.ty, 0.0f}, {world_units_per_pixel, 0.0f, 0.0f, 0.0f}};
}

SdfBatcher::~SdfBatcher() { shutdown(); }

bool SdfBatcher::init(rhi::Device& device, std::size_t initial_capacity) {
    shutdown();
    device_ = &device;

    const std::array<rhi::VertexStream, 2> streams{{
        {sizeof(float) * 2, rhi::VertexRate::PerVertex, corner_attributes},
        {sizeof(SdfInstance), rhi::VertexRate::PerInstance, instance_attributes},
    }};
    rhi::PipelineDesc desc;
    desc.vertex = shaders::sdf_vert;
    desc.fragment = shaders::sdf_frag;
    desc.streams = streams;
    // The fragment shader outputs premultiplied color, so outlines and
    // anti-aliased edges composite without dark fringes.
    desc.blend = rhi::BlendMode::Premultiplied;
    desc.debug_name = "sdf";
    pipeline_ = device.create_pipeline(desc);

    quad_vertices_ = device.create_buffer({rhi::BufferKind::Vertex, rhi::BufferUsage::Static, sizeof(quad_corners),
                                           quad_corners.data()});
    quad_indices_ = device.create_buffer({rhi::BufferKind::Index, rhi::BufferUsage::Static, sizeof(quad_index_data),
                                          quad_index_data.data()});
    globals_ = device.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, sizeof(SdfGlobals), nullptr});
    if (!pipeline_ || !quad_vertices_ || !quad_indices_ || !globals_ || !ensure_capacity(initial_capacity)) {
        LOG_ERROR("SdfBatcher: failed to create GPU resources");
        shutdown();
        return false;
    }
    return true;
}

void SdfBatcher::shutdown() {
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

bool SdfBatcher::ensure_capacity(std::size_t count) {
    if (instance_buffer_ && count <= capacity_) {
        return true;
    }
    const std::size_t capacity = std::max({count, capacity_ * 2, std::size_t{256}});
    // Deferred destroy: draws already recorded keep the old buffer alive.
    device_->destroy_buffer(instance_buffer_);
    instance_buffer_ = device_->create_buffer(
        {rhi::BufferKind::Vertex, rhi::BufferUsage::Stream, capacity * sizeof(SdfInstance), nullptr});
    capacity_ = instance_buffer_ ? capacity : 0;
    return static_cast<bool>(instance_buffer_);
}

void SdfBatcher::flush(const Affine2& world_to_ndc, float world_units_per_pixel) {
    const std::size_t first = flushed_;
    flushed_ = instances_.size();
    if (!device_ || first == instances_.size() || !ensure_capacity(instances_.size())) {
        return;
    }
    const SdfGlobals globals = make_sdf_globals(world_to_ndc, world_units_per_pixel);
    device_->update_buffer(globals_, 0, std::as_bytes(std::span(&globals, 1)));
    // Only this flush's range: draws of earlier flushes this frame keep
    // reading theirs (the RHI gives updates GL ordering semantics).
    device_->update_buffer(instance_buffer_, first * sizeof(SdfInstance),
                           std::as_bytes(std::span(instances_).subspan(first)));

    device_->bind_pipeline(pipeline_);
    device_->bind_vertex_buffer(0, quad_vertices_);
    device_->bind_vertex_buffer(1, instance_buffer_);
    device_->bind_index_buffer(quad_indices_, rhi::IndexFormat::UInt16);
    device_->bind_uniform_buffer(0, globals_);
    device_->draw_indexed(static_cast<std::uint32_t>(quad_index_data.size()),
                          static_cast<std::uint32_t>(instances_.size() - first), 0, static_cast<std::uint32_t>(first));
}

void SdfBatcher::arc(vec2 center, float radius, float start_angle, float end_angle, rhi::Color fill,
                     rhi::Color stroke, float width) {
    if (radius <= 0.0f || dmath::abs(end_angle - start_angle) < 1e-6f) {
        return;
    }
    if (fill.a > 0.0f) {
        sector(center, radius, start_angle, end_angle, fill);
    }
    if (stroke.a > 0.0f && width > 0.0f) {
        add(make_arc(center, radius, start_angle, end_angle, width, stroke));
    }
}

void SdfBatcher::arc(vec2 center, float radius, float start_angle, float end_angle, rhi::Color stroke,
                     float width) {
    arc(center, radius, start_angle, end_angle, rhi::Color{0.0f, 0.0f, 0.0f, 0.0f}, stroke, width);
}

} // namespace cuervo::render

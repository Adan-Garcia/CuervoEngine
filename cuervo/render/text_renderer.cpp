#include "cuervo/render/text_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

#include "cuervo/core/log.hpp"
#include "cuervo/render/sdf_batcher.hpp" // pack_rgba8
#include "shaders/text_frag.hpp"
#include "shaders/text_vert.hpp"

namespace cuervo::render {

namespace {

static_assert(offsetof(GlyphInstance, origin) == 0);
static_assert(offsetof(GlyphInstance, axis_x) == 8);
static_assert(offsetof(GlyphInstance, axis_y) == 16);
static_assert(offsetof(GlyphInstance, uv_min) == 24);
static_assert(offsetof(GlyphInstance, uv_max) == 32);
static_assert(offsetof(GlyphInstance, color) == 40);
static_assert(offsetof(GlyphInstance, distance_range) == 44);

constexpr std::array<float, 8> quad_corners{0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
constexpr std::array<std::uint16_t, 6> quad_index_data{0, 1, 2, 0, 2, 3};

constexpr std::array<rhi::VertexAttribute, 1> corner_attributes{{{0, rhi::VertexFormat::Float2, 0}}};
constexpr std::array<rhi::VertexAttribute, 6> instance_attributes{{
    {1, rhi::VertexFormat::Float2, offsetof(GlyphInstance, origin)},
    {2, rhi::VertexFormat::Float2, offsetof(GlyphInstance, axis_x)},
    {3, rhi::VertexFormat::Float2, offsetof(GlyphInstance, axis_y)},
    {4, rhi::VertexFormat::Float4, offsetof(GlyphInstance, uv_min)}, // uv_min, uv_max
    {5, rhi::VertexFormat::UByte4Norm, offsetof(GlyphInstance, color)},
    {6, rhi::VertexFormat::Float1, offsetof(GlyphInstance, distance_range)},
}};

} // namespace

GlyphInstance make_glyph_instance(const LaidOutGlyph& glyph, const Affine2& transform, std::uint32_t color,
                                  float distance_range) {
    GlyphInstance instance;
    instance.origin = transform.apply(glyph.min);
    instance.axis_x = transform.apply_vector({glyph.max.x() - glyph.min.x(), 0.0f});
    instance.axis_y = transform.apply_vector({0.0f, glyph.max.y() - glyph.min.y()});
    instance.uv_min = glyph.uv_min;
    instance.uv_max = glyph.uv_max;
    instance.color = color;
    instance.distance_range = distance_range;
    return instance;
}

TextRenderer::~TextRenderer() { shutdown(); }

bool TextRenderer::init(rhi::Device& device) {
    shutdown();
    device_ = &device;

    const std::array<rhi::VertexStream, 2> streams{{
        {sizeof(float) * 2, rhi::VertexRate::PerVertex, corner_attributes},
        {sizeof(GlyphInstance), rhi::VertexRate::PerInstance, instance_attributes},
    }};
    rhi::PipelineDesc desc;
    desc.vertex = shaders::text_vert;
    desc.fragment = shaders::text_frag;
    desc.streams = streams;
    desc.blend = rhi::BlendMode::Premultiplied;
    desc.debug_name = "text";
    pipeline_ = device.create_pipeline(desc);

    quad_vertices_ = device.create_buffer({rhi::BufferKind::Vertex, rhi::BufferUsage::Static, sizeof(quad_corners),
                                           quad_corners.data()});
    quad_indices_ = device.create_buffer({rhi::BufferKind::Index, rhi::BufferUsage::Static, sizeof(quad_index_data),
                                          quad_index_data.data()});
    globals_ = device.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, sizeof(TextGlobals), nullptr});
    if (!pipeline_ || !quad_vertices_ || !quad_indices_ || !globals_ || !ensure_capacity(1024)) {
        LOG_ERROR("TextRenderer: failed to create GPU resources");
        shutdown();
        return false;
    }
    // Fonts added before init() get their textures now.
    for (auto& [id, font] : fonts_) {
        if (!font.texture) {
            font.texture = device.create_texture({font.atlas.width, font.atlas.height, rhi::TextureFormat::RGBA8,
                                                  rhi::Filter::Linear, font.atlas.pixels.data(), font.atlas.name});
        }
    }
    return true;
}

void TextRenderer::shutdown() {
    if (!device_) {
        return;
    }
    for (auto& [id, font] : fonts_) {
        device_->destroy_texture(font.texture);
        font.texture = {};
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

FontHandle TextRenderer::add_font(FontAtlas atlas) {
    if (std::string invalid = validate_font_atlas(atlas); !invalid.empty()) {
        LOG_ERROR("TextRenderer: font '{}' rejected: {}", atlas.name, invalid);
        return {};
    }
    Font font{std::move(atlas), {}};
    if (device_) {
        font.texture = device_->create_texture({font.atlas.width, font.atlas.height, rhi::TextureFormat::RGBA8,
                                                rhi::Filter::Linear, font.atlas.pixels.data(), font.atlas.name});
        if (!font.texture) {
            LOG_ERROR("TextRenderer: could not upload the atlas of font '{}'", font.atlas.name);
            return {};
        }
    }
    const FontHandle handle{next_font_++};
    fonts_.emplace(handle.id, std::move(font));
    return handle;
}

void TextRenderer::remove_font(FontHandle handle) {
    auto it = fonts_.find(handle.id);
    if (it == fonts_.end()) {
        return;
    }
    if (device_) {
        device_->destroy_texture(it->second.texture); // deferred by the RHI while in flight
    }
    fonts_.erase(it);
}

const FontAtlas* TextRenderer::font(FontHandle handle) const {
    auto it = fonts_.find(handle.id);
    return it != fonts_.end() ? &it->second.atlas : nullptr;
}

rhi::TextureHandle TextRenderer::texture(FontHandle handle) const {
    auto it = fonts_.find(handle.id);
    return it != fonts_.end() ? it->second.texture : rhi::TextureHandle{};
}

TextLayout TextRenderer::draw(FontHandle handle, std::string_view text, const Affine2& transform,
                              const TextStyle& style, rhi::Color color) {
    const FontAtlas* atlas = font(handle);
    if (!atlas) {
        LOG_ERROR("TextRenderer::draw: invalid font handle {}", handle.id);
        return {};
    }
    TextLayout layout = layout_text(*atlas, text, style);
    draw_layout(handle, layout, transform, color);
    return layout;
}

void TextRenderer::draw_layout(FontHandle handle, const TextLayout& layout, const Affine2& transform,
                               rhi::Color color) {
    const FontAtlas* atlas = font(handle);
    if (!atlas || layout.glyphs.empty()) {
        return;
    }
    const std::uint32_t packed = pack_rgba8(color);
    // A run already drawn by an earlier flush is never extended.
    if (runs_.size() == flushed_runs_ || runs_.back().font != handle) {
        runs_.push_back({handle, static_cast<std::uint32_t>(instances_.size()), 0});
    }
    for (const LaidOutGlyph& glyph : layout.glyphs) {
        instances_.push_back(make_glyph_instance(glyph, transform, packed, atlas->distance_range));
    }
    runs_.back().instance_count += static_cast<std::uint32_t>(layout.glyphs.size());
}

bool TextRenderer::ensure_capacity(std::size_t count) {
    if (instance_buffer_ && count <= capacity_) {
        return true;
    }
    const std::size_t capacity = std::max({count, capacity_ * 2, std::size_t{256}});
    device_->destroy_buffer(instance_buffer_); // deferred while in flight
    instance_buffer_ = device_->create_buffer(
        {rhi::BufferKind::Vertex, rhi::BufferUsage::Stream, capacity * sizeof(GlyphInstance), nullptr});
    capacity_ = instance_buffer_ ? capacity : 0;
    return static_cast<bool>(instance_buffer_);
}

void TextRenderer::flush(const Affine2& m) {
    last_draw_calls_ = 0;
    const std::size_t first = flushed_instances_;
    const std::size_t first_run = flushed_runs_;
    flushed_instances_ = instances_.size();
    flushed_runs_ = runs_.size();
    if (!device_ || first == instances_.size() || !ensure_capacity(instances_.size())) {
        return;
    }
    const TextGlobals globals{{m.a, m.c, m.tx, 0.0f}, {m.b, m.d, m.ty, 0.0f}};
    device_->update_buffer(globals_, 0, std::as_bytes(std::span(&globals, 1)));
    // Only this flush's range: draws of earlier flushes keep reading theirs
    // (the RHI gives updates GL ordering semantics on every backend).
    device_->update_buffer(instance_buffer_, first * sizeof(GlyphInstance),
                           std::as_bytes(std::span(instances_).subspan(first)));

    device_->bind_pipeline(pipeline_);
    device_->bind_vertex_buffer(0, quad_vertices_);
    device_->bind_vertex_buffer(1, instance_buffer_);
    device_->bind_index_buffer(quad_indices_, rhi::IndexFormat::UInt16);
    device_->bind_uniform_buffer(0, globals_);
    for (std::size_t r = first_run; r < runs_.size(); ++r) {
        const DrawCall& run = runs_[r];
        auto it = fonts_.find(run.font.id);
        if (it == fonts_.end() || !it->second.texture || run.instance_count == 0) {
            continue; // font removed after its text was queued
        }
        device_->bind_texture(0, it->second.texture);
        device_->draw_indexed(static_cast<std::uint32_t>(quad_index_data.size()), run.instance_count, 0,
                              run.first_instance);
        ++last_draw_calls_;
    }
}

} // namespace cuervo::render

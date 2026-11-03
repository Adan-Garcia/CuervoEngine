#include "cuervo/render/post_fx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "cuervo/core/log.hpp"
#include "shaders/post_composite_frag.hpp"
#include "shaders/post_down_frag.hpp"
#include "shaders/post_up_frag.hpp"
#include "shaders/post_vert.hpp"

namespace cuervo::render {

namespace {

// One triangle covering the target (post.vert maps a_pos to uv).
constexpr std::array<float, 6> triangle_vertices = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};

constexpr std::array<rhi::VertexAttribute, 1> triangle_attributes{{{0, rhi::VertexFormat::Float2, 0}}};

// Uniform ranges used round-robin; a frame needs one per pass.
constexpr std::size_t param_slots = 64;
constexpr int max_bloom_levels = 8;

} // namespace

rhi::Color srgb_to_linear(rhi::Color c) {
    // The sRGB transfer function; pow is fine here (rendering only, never the simulation).
    const auto f = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return {f(c.r), f(c.g), f(c.b), c.a};
}

std::vector<std::uint8_t> make_identity_lut(int size) {
    size = std::max(size, 2);
    const auto n = static_cast<std::size_t>(size);
    std::vector<std::uint8_t> bytes(n * n * n * 4);
    const auto scale = [&](std::size_t v) {
        return static_cast<std::uint8_t>((v * 255 + (n - 1) / 2) / (n - 1));
    };
    for (std::size_t y = 0; y < n; ++y) {
        for (std::size_t x = 0; x < n * n; ++x) {
            std::uint8_t* texel = &bytes[(y * n * n + x) * 4];
            texel[0] = scale(x % n);
            texel[1] = scale(y);
            texel[2] = scale(x / n);
            texel[3] = 255;
        }
    }
    return bytes;
}

PostFx::~PostFx() { shutdown(); }

bool PostFx::init(rhi::Device& device) {
    shutdown();
    device_ = &device;
    slot_size_ = std::max<std::size_t>(device.info().uniform_offset_alignment, sizeof(PostParams));

    down_ = create_pass_pipeline(shaders::post_down_frag, rhi::BlendMode::None, "post_down");
    // Added onto the larger pyramid level: src + dst.
    up_ = create_pass_pipeline(shaders::post_up_frag, rhi::BlendMode::AdditivePremultiplied, "post_up");
    composite_ = create_pass_pipeline(shaders::post_composite_frag, rhi::BlendMode::None, "post_composite");
    triangle_ = device.create_buffer({rhi::BufferKind::Vertex, rhi::BufferUsage::Static, sizeof(triangle_vertices),
                                      triangle_vertices.data()});
    params_ = device.create_buffer(
        {rhi::BufferKind::Uniform, rhi::BufferUsage::Stream, slot_size_ * param_slots, nullptr});
    if (!down_ || !up_ || !composite_ || !triangle_ || !params_) {
        LOG_ERROR("PostFx: failed to create GPU resources");
        shutdown();
        return false;
    }
    return true;
}

void PostFx::shutdown() {
    if (!device_) {
        return;
    }
    destroy_pyramid();
    if (mid_) device_->destroy_render_target(mid_);
    mid_ = {};
    for (rhi::PipelineHandle p : {down_, up_, composite_}) {
        device_->destroy_pipeline(p);
    }
    device_->destroy_buffer(triangle_);
    device_->destroy_buffer(params_);
    down_ = up_ = composite_ = {};
    triangle_ = params_ = {};
    device_ = nullptr;
}

const rhi::ShaderSource& PostFx::vertex() const { return shaders::post_vert; }

rhi::PipelineHandle PostFx::create_pass_pipeline(const rhi::ShaderSource& fragment, rhi::BlendMode blend,
                                                 std::string_view debug_name) {
    if (!device_) {
        return {};
    }
    const std::array<rhi::VertexStream, 1> streams{{{sizeof(float) * 2, rhi::VertexRate::PerVertex, triangle_attributes}}};
    rhi::PipelineDesc desc;
    desc.vertex = shaders::post_vert;
    desc.fragment = fragment;
    desc.streams = streams;
    desc.blend = blend;
    desc.debug_name = debug_name;
    return device_->create_pipeline(desc);
}

void PostFx::fullscreen(rhi::PipelineHandle pipeline, std::span<const rhi::TextureHandle> textures, PostParams params,
                        bool flip_targets) {
    if (!device_ || !pipeline) {
        return;
    }
    params.v3[3] = flip_targets && device_->info().render_target_flip_v ? 1.0f : 0.0f;
    const std::size_t offset = (slot_++ % param_slots) * slot_size_;
    device_->update_buffer(params_, offset, std::as_bytes(std::span(&params, 1)));
    device_->bind_pipeline(pipeline);
    device_->bind_vertex_buffer(0, triangle_);
    device_->bind_uniform_buffer(0, params_, offset, sizeof(PostParams));
    for (std::uint32_t i = 0; i < textures.size() && i < rhi::max_texture_bindings; ++i) {
        device_->bind_texture(i, textures[i]);
    }
    device_->draw(3);
    ++passes_;
}

void PostFx::destroy_pyramid() {
    for (rhi::RenderTargetHandle target : pyramid_) {
        device_->destroy_render_target(target);
    }
    pyramid_.clear();
    pyramid_sizes_.clear();
    pyramid_source_ = {0, 0};
}

// Level i is half the size of level i - 1 (the first is half the source).
void PostFx::ensure_pyramid(ivec2 source_size, int levels) {
    levels = std::clamp(levels, 1, max_bloom_levels);
    if (pyramid_source_ == source_size && pyramid_.size() == static_cast<std::size_t>(levels)) {
        return;
    }
    destroy_pyramid();
    ivec2 size = source_size;
    for (int i = 0; i < levels; ++i) {
        size = {std::max(size.x() / 2, 1), std::max(size.y() / 2, 1)};
        const rhi::RenderTargetHandle target =
            device_->create_render_target({size.x(), size.y(), rhi::Filter::Linear, "bloom"});
        if (!target) {
            destroy_pyramid();
            return;
        }
        pyramid_.push_back(target);
        pyramid_sizes_.push_back(size);
    }
    pyramid_source_ = source_size;
}

void PostFx::apply(rhi::RenderTargetHandle source, ivec2 source_size, rhi::RenderTargetHandle destination,
                   const PostSettings& settings) {
    if (!device_ || !source) {
        return;
    }
    rhi::Device& device = *device_;
    const rhi::TextureHandle scene = device.render_target_texture(source);
    rhi::TextureHandle bloom = scene;

    bool bloom_on = settings.bloom && source_size.x() >= 4 && source_size.y() >= 4;
    if (bloom_on) {
        ensure_pyramid(source_size, settings.bloom_levels);
        bloom_on = !pyramid_.empty();
    }
    if (bloom_on) {
        // Down: threshold + 5-tap filter into each half-size level.
        rhi::TextureHandle in = scene;
        ivec2 in_size = source_size;
        for (std::size_t i = 0; i < pyramid_.size(); ++i) {
            device.set_render_target(pyramid_[i]);
            PostParams p;
            p.v0[0] = 1.0f / static_cast<float>(in_size.x());
            p.v0[1] = 1.0f / static_cast<float>(in_size.y());
            p.v1[0] = settings.bloom_threshold;
            p.v1[1] = settings.bloom_knee;
            p.v1[2] = i == 0 ? 1.0f : 0.0f;
            fullscreen(down_, std::array{in}, p, true);
            in = device.render_target_texture(pyramid_[i]);
            in_size = pyramid_sizes_[i];
        }
        // Up: tent filter of the smaller level added onto the larger one.
        for (std::size_t i = pyramid_.size() - 1; i-- > 0;) {
            device.set_render_target(pyramid_[i]);
            PostParams p;
            p.v0[0] = 1.0f / static_cast<float>(pyramid_sizes_[i + 1].x());
            p.v0[1] = 1.0f / static_cast<float>(pyramid_sizes_[i + 1].y());
            p.v1[0] = 1.0f;
            p.v1[1] = 1.0f;
            fullscreen(up_, std::array{device.render_target_texture(pyramid_[i + 1])}, p, true);
        }
        bloom = device.render_target_texture(pyramid_[0]);
    }

    // A custom pass follows: the effects render into an intermediate target.
    rhi::RenderTargetHandle composite_target = destination;
    if (settings.custom) {
        if (!mid_ || mid_size_ != source_size) {
            if (mid_) device.destroy_render_target(mid_);
            mid_ = device.create_render_target({source_size.x(), source_size.y(), rhi::Filter::Linear, "post_mid"});
            mid_size_ = source_size;
        }
        if (mid_) composite_target = mid_;
    }
    device.set_render_target(composite_target);
    const bool lut_on = static_cast<bool>(settings.lut);
    PostParams p;
    p.v0[0] = settings.bloom_intensity;
    p.v0[1] = settings.vignette;
    p.v0[2] = settings.vignette_radius;
    p.v0[3] = settings.lut_strength;
    p.v1[0] = settings.crt_scanlines;
    p.v1[1] = settings.crt_curvature;
    p.v1[2] = settings.crt_mask;
    p.v1[3] = static_cast<float>(std::max(settings.lut_size, 2));
    const ivec2 size = device.framebuffer_size();
    p.v2[0] = static_cast<float>(size.x());
    p.v2[1] = static_cast<float>(size.y());
    // Linear color: the scene target holds linear values; effects run on their
    // sRGB encoding and the result is handed back linear where the destination
    // encodes on write (a window with an sRGB surface, or an sRGB target).
    const rhi::DeviceInfo& info = device.info();
    p.v2[2] = info.linear_color ? 1.0f : 0.0f;
    p.v2[3] = info.linear_color && (composite_target ? true : info.window_srgb) ? 1.0f : 0.0f;
    p.v3[0] = bloom_on ? 1.0f : 0.0f;
    p.v3[1] = lut_on ? 1.0f : 0.0f;
    p.v3[2] = settings.crt ? 1.0f : 0.0f;
    // Unused samplers still need a valid texture: reuse the scene.
    fullscreen(composite_, std::array{scene, bloom, lut_on ? settings.lut : scene}, p, true);

    if (settings.custom && composite_target.id != destination.id) {
        device.set_render_target(destination);
        PostParams custom = settings.custom_params;
        // The shader writes linear values in linear mode: a window that does not
        // encode on write needs the shader's own epilogue to do it.
        custom.v3[0] = info.linear_color && !destination && !info.window_srgb ? 1.0f : 0.0f;
        const rhi::TextureHandle mid = device.render_target_texture(composite_target);
        fullscreen(settings.custom, std::array{mid, mid}, custom, true);
    }
}

} // namespace cuervo::render

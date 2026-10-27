#include "cuervo/rhi/webgpu/webgpu_device.hpp"

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <webgpu/webgpu.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>
#else
#include <webgpu/wgpu.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/rhi/rhi_internal.hpp"

// WebGPU backend (DESIGN.md 6, milestone 10.1). The RHI contract (rhi.hpp) is
// GL-shaped; the parts WebGPU doesn't give for free:
//  - Buffer updates follow GL ordering: queue writes land before the frame's
//    command buffer runs, so a buffer an unsubmitted draw reads is renamed
//    (a fresh GPU buffer filled from the CPU shadow) instead of written in
//    place. Stream buffers write disjoint ranges in place, like Vulkan.
//  - `clear` honors the scissor: a full-target clear becomes the next render
//    pass's load op, anything else draws a full-viewport triangle.
//  - Resources are bound per draw through two bind groups built from the
//    shader's reflection tables: @group(0) = uniform blocks (dynamic offsets),
//    @group(1) = texture binding N as texture 2N + sampler 2N + 1
//    (shader_tool --webgpu-bindings). Bindings a pipeline declares but the
//    caller left unbound get a zeroed dummy, as GL would draw anyway.
namespace cuervo::webgpu {

namespace {

WGPUStringView sv(std::string_view s) {
    WGPUStringView v = WGPU_STRING_VIEW_INIT;
    v.data = s.data();
    v.length = s.size();
    return v;
}

std::string to_string(WGPUStringView s) {
    if (!s.data) return {};
    return s.length == WGPU_STRLEN ? std::string(s.data) : std::string(s.data, s.length);
}

constexpr std::size_t align_up(std::size_t n, std::size_t a) { return (n + a - 1) / a * a; }

// Draws that read a renamed buffer keep the old GPU buffer alive (WebGPU
// reference counts it); retired buffers are reused once their epoch was
// submitted, and dropped after this many unused frames.
constexpr std::uint64_t retired_keep_frames = 8;
constexpr std::size_t max_retired_buffers = 64;
constexpr std::size_t max_cached_bind_groups = 4096;
// Submitted frames that may queue before end_frame waits (as GL's fence ring;
// the browser paces frames itself, so the web build never waits).
constexpr std::size_t max_frames_in_flight = 2;
// Uniform blocks the caller left unbound read zeros from this (the Compatible
// tier's largest block).
constexpr std::size_t dummy_uniform_size = 16384;
// Clears that can't be a load op: one 256-byte uniform slot each, cycling.
constexpr std::size_t clear_slot_size = 256;
constexpr std::size_t clear_slots = 64;
constexpr int max_logged_errors = 32;

WGPUVertexFormat to_wgpu(rhi::VertexFormat format) {
    switch (format) {
        case rhi::VertexFormat::Float1:     return WGPUVertexFormat_Float32;
        case rhi::VertexFormat::Float2:     return WGPUVertexFormat_Float32x2;
        case rhi::VertexFormat::Float3:     return WGPUVertexFormat_Float32x3;
        case rhi::VertexFormat::Float4:     return WGPUVertexFormat_Float32x4;
        case rhi::VertexFormat::UByte4Norm: return WGPUVertexFormat_Unorm8x4;
        case rhi::VertexFormat::UInt1:      return WGPUVertexFormat_Uint32;
    }
    return WGPUVertexFormat_Float32x4;
}

WGPUPrimitiveTopology to_wgpu(rhi::Topology topology) {
    switch (topology) {
        case rhi::Topology::TriangleList:  return WGPUPrimitiveTopology_TriangleList;
        case rhi::Topology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
        case rhi::Topology::LineList:      return WGPUPrimitiveTopology_LineList;
        case rhi::Topology::LineStrip:     return WGPUPrimitiveTopology_LineStrip;
        case rhi::Topology::PointList:     return WGPUPrimitiveTopology_PointList;
    }
    return WGPUPrimitiveTopology_TriangleList;
}

WGPUTextureFormat to_wgpu(rhi::TextureFormat format) {
    switch (format) {
        case rhi::TextureFormat::RGBA8: return WGPUTextureFormat_RGBA8Unorm;
        case rhi::TextureFormat::R8:    return WGPUTextureFormat_R8Unorm;
    }
    return WGPUTextureFormat_RGBA8Unorm;
}

WGPUBlendComponent blend_component(WGPUBlendFactor src, WGPUBlendFactor dst) {
    WGPUBlendComponent c = WGPU_BLEND_COMPONENT_INIT;
    c.operation = WGPUBlendOperation_Add;
    c.srcFactor = src;
    c.dstFactor = dst;
    return c;
}

// Same factors as the GL backend's glBlendFuncSeparate calls.
std::optional<WGPUBlendState> blend_state(rhi::BlendMode mode) {
    WGPUBlendState b = WGPU_BLEND_STATE_INIT;
    switch (mode) {
        case rhi::BlendMode::None:
            return std::nullopt;
        case rhi::BlendMode::Alpha:
            b.color = blend_component(WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha);
            b.alpha = blend_component(WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha);
            return b;
        case rhi::BlendMode::Premultiplied:
            b.color = blend_component(WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha);
            b.alpha = blend_component(WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha);
            return b;
        case rhi::BlendMode::Additive:
            b.color = blend_component(WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_One);
            b.alpha = blend_component(WGPUBlendFactor_One, WGPUBlendFactor_One);
            return b;
    }
    return std::nullopt;
}

#ifndef __EMSCRIPTEN__
std::string_view backend_type_name(WGPUBackendType type) {
    switch (type) {
        case WGPUBackendType_Vulkan:   return "Vulkan";
        case WGPUBackendType_Metal:    return "Metal";
        case WGPUBackendType_D3D12:    return "D3D12";
        case WGPUBackendType_D3D11:    return "D3D11";
        case WGPUBackendType_OpenGL:   return "OpenGL";
        case WGPUBackendType_OpenGLES: return "OpenGL ES";
        default:                       return "unknown";
    }
}
#endif

// The full-target clear for scissored clears (vertex_index -> a triangle that
// covers the viewport).
constexpr std::string_view clear_vertex_wgsl = R"wgsl(
@vertex fn main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}
)wgsl";
constexpr std::string_view clear_fragment_wgsl = R"wgsl(
struct ClearColor { color: vec4<f32> }
@group(0) @binding(0) var<uniform> clear_color: ClearColor;
@fragment fn main() -> @location(0) vec4<f32> { return clear_color.color; }
)wgsl";
constexpr rhi::UniformBlockBinding clear_uniforms[] = {{"ClearColor", 0}};

// Caches never key on WebGPU object pointers: a released object's address is
// handed to the next one created (a grown stream buffer got its predecessor's,
// so the pass kept the old, smaller one bound -- milestone 10.4). GPU buffers
// carry a serial instead, pipelines and textures their never-reused ids.
struct GpuBuffer {
    WGPUBuffer gpu = nullptr;
    std::uint64_t serial = 0; // unique per WGPUBuffer object, kept when a retired one is reused
};

struct Buffer {
    WGPUBuffer gpu = nullptr;
    std::uint64_t serial = 0; // of `gpu`
    WGPUBufferUsage usage_flags = WGPUBufferUsage_None;
    rhi::BufferUsage usage = rhi::BufferUsage::Static;
    std::size_t size = 0;          // as created
    std::vector<std::byte> shadow; // padded to whole words (WebGPU writes 4-byte units)
    std::uint64_t used_epoch = 0;  // an unsubmitted draw of this epoch reads `gpu`
    std::uint64_t written_epoch = 0;
    std::vector<std::pair<std::size_t, std::size_t>> written; // Stream: [begin, end) written in written_epoch
};

struct RetiredBuffer {
    WGPUBuffer gpu = nullptr;
    std::uint64_t serial = 0;
    WGPUBufferUsage usage_flags = WGPUBufferUsage_None;
    std::size_t size = 0;
    std::uint64_t epoch = 0; // reusable once this epoch is submitted
    std::uint64_t frame = 0;
};

struct Texture {
    WGPUTexture gpu = nullptr;
    WGPUTextureView view = nullptr;
    rhi::Filter filter = rhi::Filter::Linear;
    std::int32_t width = 0;
    std::int32_t height = 0;
    rhi::TextureFormat format = rhi::TextureFormat::RGBA8;
};

struct Pipeline {
    WGPURenderPipeline gpu = nullptr;
    std::array<WGPUBindGroupLayout, 2> layouts{};
    std::vector<std::uint32_t> uniform_bindings; // ascending (the dynamic offset order)
    std::vector<std::uint32_t> texture_bindings;
    std::uint32_t stream_count = 0;
};

struct VertexBinding {
    std::uint32_t buffer = 0;
    std::size_t offset = 0;
};

struct UniformBinding {
    std::uint32_t buffer = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
};

// What the open render pass has set, to skip redundant calls.
struct AppliedState {
    std::uint32_t pipeline = 0;                                                         // pipeline id
    std::array<std::pair<std::uint64_t, std::size_t>, rhi::max_vertex_streams> vertex{}; // buffer serial, offset
    std::uint64_t index = 0;                                                            // buffer serial
    WGPUIndexFormat index_format = WGPUIndexFormat_Undefined;
    std::optional<rhi::Rect> viewport;
    std::optional<rhi::Rect> scissor;
};

// A frame capture in a mapped readback buffer (rows padded to 256 bytes).
struct Capture {
    WGPUBuffer buffer = nullptr;
    std::string path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row = 0;
    bool bgra = false;

    bool write() const {
        const std::size_t size = std::size_t{row} * height;
        const auto* bytes = static_cast<const std::uint8_t*>(wgpuBufferGetConstMappedRange(buffer, 0, size));
        std::vector<std::uint8_t> rgb(std::size_t{width} * height * 3);
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::uint8_t* in = bytes + std::size_t{y} * row;
            std::uint8_t* out = rgb.data() + std::size_t{y} * width * 3;
            for (std::uint32_t x = 0; x < width; ++x, in += 4, out += 3) {
                out[0] = bgra ? in[2] : in[0];
                out[1] = in[1];
                out[2] = bgra ? in[0] : in[2];
            }
        }
        wgpuBufferUnmap(buffer);
        return rhi::detail::write_ppm(path, static_cast<int>(width), static_cast<int>(height), rgb);
    }
};

rhi::Rect clamp_rect(rhi::Rect r, ivec2 size) {
    const std::int32_t x0 = std::clamp(r.x, 0, size.x());
    const std::int32_t y0 = std::clamp(r.y, 0, size.y());
    const std::int32_t x1 = std::clamp(r.x + std::max(r.width, 0), 0, size.x());
    const std::int32_t y1 = std::clamp(r.y + std::max(r.height, 0), 0, size.y());
    return {x0, y0, x1 - x0, y1 - y0};
}

class WebGpuDevice final : public rhi::Device {
public:
    ~WebGpuDevice() override { shutdown(); }

    std::size_t peak_frames_in_flight() const { return peak_frames_in_flight_; }

    bool init(Window& window, const rhi::DeviceOptions& options) {
        instance_ = wgpuCreateInstance(nullptr);
        if (!instance_) {
            LOG_ERROR("WebGPU: wgpuCreateInstance failed");
            return false;
        }
#ifdef __EMSCRIPTEN__
        // The page requested the adapter and device (web/webgpu_init.js), so
        // nothing here waits on a promise.
        if (!browser_device_ready()) {
            LOG_ERROR("WebGPU: the page has no WebGPU device (no navigator.gpu, or its adapter or device "
                      "request failed)");
            return false;
        }
        device_ = emscripten_webgpu_get_device();
        surface_ = create_surface(window);
#else
        surface_ = create_surface(window);
        if (!request_adapter() || !request_device()) return false;
#endif
        queue_ = wgpuDeviceGetQueue(device_);

        WGPUAdapterInfo adapter_info = WGPU_ADAPTER_INFO_INIT;
#ifdef __EMSCRIPTEN__
        wgpuDeviceGetAdapterInfo(device_, &adapter_info);
#else
        wgpuAdapterGetInfo(adapter_, &adapter_info);
#endif
        renderer_ = to_string(adapter_info.device);
        if (renderer_.empty()) renderer_ = to_string(adapter_info.description);
        if (renderer_.empty()) { // browsers hide the device name
            renderer_ = std::format("{} {}", to_string(adapter_info.vendor), to_string(adapter_info.architecture));
            if (renderer_ == " ") renderer_ = "unnamed adapter";
        }
#ifdef __EMSCRIPTEN__
        api_version_ = "WebGPU (browser)";
#else
        api_version_ = std::format("WebGPU (wgpu-native, {})", backend_type_name(adapter_info.backendType));
#endif
        wgpuAdapterInfoFreeMembers(adapter_info);

        WGPULimits limits = WGPU_LIMITS_INIT;
        wgpuDeviceGetLimits(device_, &limits);
        max_uniform_binding_ = static_cast<std::size_t>(limits.maxUniformBufferBindingSize);

        target_format_ = WGPUTextureFormat_RGBA8Unorm;
        bool capture = true;
        if (surface_ && !choose_surface_config(options.vsync, capture)) {
            wgpuSurfaceRelease(surface_);
            surface_ = nullptr;
        }
        if (!surface_) {
            const char* driver = SDL_GetCurrentVideoDriver();
            LOG_INFO("WebGPU: no window surface ({} video driver): rendering offscreen", driver ? driver : "no");
            capture = true;
        }

        info_.backend = rhi::Backend::WebGPU;
        info_.max_tier = rhi::Tier::Compatible;
        info_.api_version = api_version_;
        info_.renderer = renderer_;
        info_.gles = false;
        rhi::FeatureSet hw;
        for (rhi::Feature f : {rhi::Feature::InstancedDraws, rhi::Feature::IndexUInt32, rhi::Feature::UniformBuffers,
                               rhi::Feature::Textures, rhi::Feature::IntegerAttributes, rhi::Feature::PointsAndLines,
                               rhi::Feature::Msaa}) {
            hw.set(f);
        }
        hw.set(rhi::Feature::FrameCapture, capture);
        info_.hardware_features = hw;
        info_.limits.max_texture_size = limits.maxTextureDimension2D;
        info_.limits.max_msaa_samples = 4; // WebGPU allows 1 or 4
        info_.limits.max_uniform_block_size =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(limits.maxUniformBufferBindingSize, 0xffffffffu));
        info_.limits.max_vertex_attributes = limits.maxVertexAttributes;
        info_.limits.uniform_offset_alignment = limits.minUniformBufferOffsetAlignment;
        rhi::detail::apply_tier_gate(info_, options.tier);
        // WebGPU has no 2x: anything below 4 renders without MSAA.
        msaa_samples_ = rhi::detail::msaa_sample_count(options.msaa_samples, info_.limits.max_msaa_samples) >= 4 ? 4 : 1;
        info_.msaa_samples = msaa_samples_;

        if (!create_internal_resources()) return false;
        LOG_INFO("WebGPU device: {} ({}), {}x MSAA, {}", renderer_, api_version_, msaa_samples_,
                 surface_ ? "presenting to the window" : "offscreen");
        return true;
    }

    const rhi::DeviceInfo& info() const override { return info_; }

    void begin_frame(ivec2 framebuffer_size) override {
        if (in_frame_) {
            LOG_ERROR("begin_frame: the previous frame was not ended");
            end_frame();
        }
        fb_size_ = ivec2{std::max(framebuffer_size.x(), 1), std::max(framebuffer_size.y(), 1)};
        acquire_target();
        begin_encoder();
        in_frame_ = true;
        first_pass_ = true;
        pending_clear_.reset();
        pipeline_ = 0;
        vertex_ = {};
        index_buffer_ = 0;
        index_format_ = rhi::IndexFormat::UInt16;
        uniforms_ = {};
        textures_ = {};
        viewport_ = scissor_ = rhi::Rect{0, 0, fb_size_.x(), fb_size_.y()};
    }

    void clear(rhi::Color color) override {
        if (!in_frame_) return;
        const rhi::Rect full{0, 0, fb_size_.x(), fb_size_.y()};
        if (!pass_ && clamp_rect(scissor_, fb_size_) == full) {
            pending_clear_ = color; // the next pass starts with it
            return;
        }
        // Scissored (or mid-pass) clear: a triangle over the whole target,
        // clipped by the scissor. The caller's pipeline, uniform 0 and
        // viewport are restored afterwards.
        const float c[4] = {color.r, color.g, color.b, color.a};
        const std::size_t offset = (clear_slot_++ % clear_slots) * clear_slot_size;
        update_buffer(clear_buffer_, offset, std::as_bytes(std::span<const float>(c)));
        const std::uint32_t saved_pipeline = pipeline_;
        const UniformBinding saved_uniform = uniforms_[0];
        const rhi::Rect saved_viewport = viewport_;
        pipeline_ = clear_pipeline_.id;
        uniforms_[0] = {clear_buffer_.id, offset, 16};
        viewport_ = full;
        if (prepare_draw()) wgpuRenderPassEncoderDraw(pass_, 3, 1, 0, 0);
        pipeline_ = saved_pipeline;
        uniforms_[0] = saved_uniform;
        viewport_ = saved_viewport;
    }

    void end_frame() override {
        if (!in_frame_) return;
        if (first_pass_ || pending_clear_) ensure_pass(); // clears land even in a frame without draws
        end_pass();
        submit(true);
#ifndef __EMSCRIPTEN__
        // The browser presents when control returns to its event loop.
        if (presenting_) wgpuSurfacePresent(surface_);
#endif
        release_frame_target();
        in_frame_ = false;
        ++frame_;
        trim_caches();
    }

    ivec2 framebuffer_size() const override { return fb_size_; }

    rhi::BufferHandle create_buffer(const rhi::BufferDesc& desc) override {
        Buffer b;
        b.usage = desc.usage;
        b.size = desc.size;
        b.usage_flags = WGPUBufferUsage_CopyDst;
        switch (desc.kind) {
            case rhi::BufferKind::Vertex:  b.usage_flags |= WGPUBufferUsage_Vertex; break;
            case rhi::BufferKind::Index:   b.usage_flags |= WGPUBufferUsage_Index; break;
            case rhi::BufferKind::Uniform: b.usage_flags |= WGPUBufferUsage_Uniform; break;
        }
        b.shadow.assign(std::max<std::size_t>(align_up(desc.size, 4), 4), std::byte{0});
        if (desc.initial_data) std::memcpy(b.shadow.data(), desc.initial_data, desc.size);
        const GpuBuffer gpu = new_gpu_buffer(b.usage_flags, b.shadow.size());
        if (!gpu.gpu) return {};
        b.gpu = gpu.gpu;
        b.serial = gpu.serial;
        if (desc.initial_data) wgpuQueueWriteBuffer(queue_, b.gpu, 0, b.shadow.data(), b.shadow.size());
        const std::uint32_t id = next_id_++;
        buffers_.emplace(id, std::move(b));
        return {id};
    }

    void update_buffer(rhi::BufferHandle handle, std::size_t offset, std::span<const std::byte> data) override {
        Buffer* b = find(buffers_, handle.id);
        if (!b) {
            LOG_ERROR("update_buffer: invalid buffer {}", handle.id);
            return;
        }
        if (offset > b->size || data.size() > b->size - offset) {
            LOG_ERROR("update_buffer: {} bytes at {} overflow a {}-byte buffer", data.size(), offset, b->size);
            return;
        }
        if (data.empty()) return;
        std::memcpy(b->shadow.data() + offset, data.data(), data.size());
        const std::size_t begin = offset / 4 * 4;
        const std::size_t end = align_up(offset + data.size(), 4);
        if (b->written_epoch != epoch_) {
            b->written.clear();
            b->written_epoch = epoch_;
        }
        bool in_place = b->used_epoch != epoch_;
        if (!in_place && b->usage == rhi::BufferUsage::Stream) {
            in_place = std::none_of(b->written.begin(), b->written.end(),
                                    [&](const auto& r) { return begin < r.second && r.first < end; });
        }
        if (in_place) {
            wgpuQueueWriteBuffer(queue_, b->gpu, begin, b->shadow.data() + begin, end - begin);
        } else {
            // Recorded draws read the current GPU buffer: they keep it, later
            // draws get a renamed copy (GL ordering, rhi.hpp).
            retire(*b);
            const GpuBuffer gpu = new_gpu_buffer(b->usage_flags, b->shadow.size());
            b->gpu = gpu.gpu;
            b->serial = gpu.serial;
            wgpuQueueWriteBuffer(queue_, b->gpu, 0, b->shadow.data(), b->shadow.size());
            b->used_epoch = 0;
        }
        if (b->usage == rhi::BufferUsage::Stream) b->written.emplace_back(begin, end);
    }

    void destroy_buffer(rhi::BufferHandle handle) override {
        auto it = buffers_.find(handle.id);
        if (it == buffers_.end()) return;
        if (it->second.gpu) wgpuBufferRelease(it->second.gpu); // recorded draws hold their own reference
        buffers_.erase(it);
    }

    rhi::TextureHandle create_texture(const rhi::TextureDesc& desc) override {
        if (!rhi::detail::texture_size_allowed(info_, desc)) return {};
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.label = sv(desc.debug_name);
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {static_cast<std::uint32_t>(desc.width), static_cast<std::uint32_t>(desc.height), 1};
        td.format = to_wgpu(desc.format);
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        Texture t;
        t.gpu = wgpuDeviceCreateTexture(device_, &td); // zero-initialized by WebGPU
        if (!t.gpu) return {};
        t.view = wgpuTextureCreateView(t.gpu, nullptr);
        t.filter = desc.filter;
        t.width = desc.width;
        t.height = desc.height;
        t.format = desc.format;
        if (desc.initial_data) {
            write_texture(t, {0, 0, desc.width, desc.height}, desc.initial_data);
        }
        const std::uint32_t id = next_id_++;
        textures_by_id_.emplace(id, t);
        return {id};
    }

    void update_texture(rhi::TextureHandle handle, rhi::Rect region, std::span<const std::byte> data) override {
        Texture* t = find(textures_by_id_, handle.id);
        if (!t) {
            LOG_ERROR("update_texture: invalid texture {}", handle.id);
            return;
        }
        if (!rhi::detail::valid_texture_update(t->width, t->height, t->format, region, data.size())) return;
        if (region.width == 0 || region.height == 0) return;
        // Queue writes run before the frame's draws: the whole frame sees the
        // update, as on Vulkan (rhi.hpp allows either).
        write_texture(*t, region, data.data());
    }

    void destroy_texture(rhi::TextureHandle handle) override {
        auto it = textures_by_id_.find(handle.id);
        if (it == textures_by_id_.end()) return;
        wgpuTextureViewRelease(it->second.view);
        wgpuTextureRelease(it->second.gpu); // deferred by reference counting
        textures_by_id_.erase(it);
    }

    rhi::PipelineHandle create_pipeline(const rhi::PipelineDesc& desc) override {
        if (desc.vertex.wgsl.empty() || desc.fragment.wgsl.empty()) {
            LOG_ERROR("create_pipeline '{}': the shaders have no WGSL (a build without CUERVO_WEBGPU?)",
                      desc.debug_name);
            return {};
        }
        if (desc.streams.size() > rhi::max_vertex_streams) {
            LOG_ERROR("create_pipeline '{}': {} vertex streams (max {})", desc.debug_name, desc.streams.size(),
                      rhi::max_vertex_streams);
            return {};
        }
        Pipeline p;
        for (const rhi::ShaderSource* stage : {&desc.vertex, &desc.fragment}) {
            for (const rhi::UniformBlockBinding& u : stage->uniform_blocks) p.uniform_bindings.push_back(u.binding);
            for (const rhi::TextureBinding& t : stage->textures) p.texture_bindings.push_back(t.binding);
        }
        for (std::vector<std::uint32_t>* v : {&p.uniform_bindings, &p.texture_bindings}) {
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }
        p.layouts = make_bind_group_layouts(p);
        p.stream_count = static_cast<std::uint32_t>(desc.streams.size());

        WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pl.bindGroupLayoutCount = p.layouts.size();
        pl.bindGroupLayouts = p.layouts.data();
        WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(device_, &pl);

        WGPUShaderModule vs = make_module(desc.vertex.wgsl, desc.debug_name);
        WGPUShaderModule fs = make_module(desc.fragment.wgsl, desc.debug_name);

        std::vector<std::vector<WGPUVertexAttribute>> attributes(desc.streams.size());
        std::vector<WGPUVertexBufferLayout> buffers(desc.streams.size());
        for (std::size_t s = 0; s < desc.streams.size(); ++s) {
            const rhi::VertexStream& stream = desc.streams[s];
            for (const rhi::VertexAttribute& a : stream.attributes) {
                WGPUVertexAttribute va = WGPU_VERTEX_ATTRIBUTE_INIT;
                va.format = to_wgpu(a.format);
                va.offset = a.offset;
                va.shaderLocation = a.location;
                attributes[s].push_back(va);
            }
            WGPUVertexBufferLayout& vb = buffers[s];
            vb = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            vb.stepMode = stream.rate == rhi::VertexRate::PerInstance ? WGPUVertexStepMode_Instance
                                                                       : WGPUVertexStepMode_Vertex;
            vb.arrayStride = stream.stride;
            vb.attributeCount = attributes[s].size();
            vb.attributes = attributes[s].data();
        }

        const std::optional<WGPUBlendState> blend = blend_state(desc.blend);
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = target_format_;
        target.blend = blend ? &*blend : nullptr;
        target.writeMask = WGPUColorWriteMask_All;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = fs;
        fragment.entryPoint = sv("main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rp.label = sv(desc.debug_name);
        rp.layout = layout;
        rp.vertex.module = vs;
        rp.vertex.entryPoint = sv("main");
        rp.vertex.bufferCount = buffers.size();
        rp.vertex.buffers = buffers.data();
        rp.primitive.topology = to_wgpu(desc.topology);
        rp.primitive.frontFace = WGPUFrontFace_CCW;
        rp.primitive.cullMode = WGPUCullMode_None;
        rp.multisample.count = msaa_samples_;
        rp.multisample.mask = 0xffffffffu;
        rp.fragment = &fragment;
        p.gpu = wgpuDeviceCreateRenderPipeline(device_, &rp);

        wgpuShaderModuleRelease(vs);
        wgpuShaderModuleRelease(fs);
        wgpuPipelineLayoutRelease(layout);
        if (!p.gpu) {
            release(p);
            return {};
        }
        const std::uint32_t id = next_id_++;
        pipelines_.emplace(id, std::move(p));
        return {id};
    }

    void destroy_pipeline(rhi::PipelineHandle handle) override {
        auto it = pipelines_.find(handle.id);
        if (it == pipelines_.end()) return;
        release(it->second);
        pipelines_.erase(it);
    }

    void set_viewport(rhi::Rect rect) override { viewport_ = rect; }
    void set_scissor(rhi::Rect rect) override { scissor_ = rect; }
    void bind_pipeline(rhi::PipelineHandle pipeline) override { pipeline_ = pipeline.id; }

    void bind_vertex_buffer(std::uint32_t stream, rhi::BufferHandle buffer, std::size_t offset) override {
        if (stream >= rhi::max_vertex_streams) {
            LOG_ERROR("bind_vertex_buffer: stream {} out of range", stream);
            return;
        }
        vertex_[stream] = {buffer.id, offset};
    }

    void bind_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format) override {
        index_buffer_ = buffer.id;
        index_format_ = format;
    }

    void bind_uniform_buffer(std::uint32_t binding, rhi::BufferHandle buffer, std::size_t offset,
                             std::size_t size) override {
        if (binding >= rhi::max_uniform_bindings) {
            LOG_ERROR("bind_uniform_buffer: binding {} out of range", binding);
            return;
        }
        if (offset % info_.uniform_offset_alignment != 0) {
            LOG_ERROR("bind_uniform_buffer: offset {} is not a multiple of {}", offset, info_.uniform_offset_alignment);
            return;
        }
        uniforms_[binding] = {buffer.id, offset, size};
    }

    void bind_texture(std::uint32_t binding, rhi::TextureHandle texture) override {
        if (binding >= rhi::max_texture_bindings) {
            LOG_ERROR("bind_texture: binding {} out of range", binding);
            return;
        }
        textures_[binding] = texture.id;
    }

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex,
              std::uint32_t first_instance) override {
        if (!prepare_draw()) return;
        wgpuRenderPassEncoderDraw(pass_, vertex_count, instance_count, first_vertex, first_instance);
    }

    void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                      std::uint32_t first_instance) override {
        if (!prepare_draw() || !prepare_index()) return;
        wgpuRenderPassEncoderDrawIndexed(pass_, index_count, instance_count, first_index, 0, first_instance);
    }

    void draw_indexed_base_vertex(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                                  std::int32_t base_vertex, std::uint32_t first_instance) override {
        // Full tier only (DESIGN.md 4.4): this Compatible-tier device refuses it.
        if (!rhi::detail::feature_allowed(info_, rhi::Feature::BaseVertexDraws, base_vertex_reported_)) return;
        if (!prepare_draw() || !prepare_index()) return;
        wgpuRenderPassEncoderDrawIndexed(pass_, index_count, instance_count, first_index, base_vertex, first_instance);
    }

    bool capture_frame(std::string path) override {
        if (!in_frame_) {
            LOG_ERROR("capture_frame: call it between begin_frame and end_frame");
            return false;
        }
        if (!info_.supports(rhi::Feature::FrameCapture) || (presenting_ && !surface_copy_src_)) {
            LOG_ERROR("capture_frame: this surface can't be read back");
            return false;
        }
        ensure_pass(); // pending clears land first
        end_pass();
        const auto width = static_cast<std::uint32_t>(fb_size_.x());
        const auto height = static_cast<std::uint32_t>(fb_size_.y());
        const std::uint32_t row = static_cast<std::uint32_t>(align_up(std::size_t{width} * 4, 256));
        const std::size_t size = std::size_t{row} * height;
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        bd.size = size;
        WGPUBuffer readback = wgpuDeviceCreateBuffer(device_, &bd);
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        src.texture = target_;
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        dst.buffer = readback;
        dst.layout.bytesPerRow = row;
        dst.layout.rowsPerImage = height;
        const WGPUExtent3D extent{width, height, 1};
        wgpuCommandEncoderCopyTextureToBuffer(encoder_, &src, &dst, &extent);
        submit(false);
        begin_encoder(); // the rest of the frame continues in a new pass (load op Load)

        auto capture = std::make_unique<Capture>(Capture{readback, std::move(path), width, height, row,
                                                         target_format_ == WGPUTextureFormat_BGRA8Unorm});
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
#ifdef __EMSCRIPTEN__
        // A main-loop callback can't block in the browser: the file is
        // written when the map completes, a frame or so later. The page
        // outlives the loop, so a last-frame capture lands.
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* user, void*) {
            std::unique_ptr<Capture> c(static_cast<Capture*>(user));
            if (status == WGPUMapAsyncStatus_Success) {
                c->write();
            } else {
                LOG_ERROR("capture_frame: mapping the readback failed: {}", to_string(message));
            }
            wgpuBufferRelease(c->buffer);
        };
        cb.userdata1 = capture.release();
        wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, size, cb);
        return true;
#else
        struct MapResult {
            bool done = false;
            bool ok = false;
            std::string message;
        } result;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* user, void*) {
            auto* r = static_cast<MapResult*>(user);
            r->done = true;
            r->ok = status == WGPUMapAsyncStatus_Success;
            r->message = to_string(message);
        };
        cb.userdata1 = &result;
        wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, size, cb);
        wait_for([&] { return result.done; });
        bool written = false;
        if (result.ok) {
            written = capture->write();
        } else {
            LOG_ERROR("capture_frame: mapping the readback failed: {}", result.message);
        }
        wgpuBufferRelease(readback);
        return written;
#endif
    }

private:
    template<typename T>
    static T* find(std::unordered_map<std::uint32_t, T>& map, std::uint32_t id) {
        auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second;
    }

#ifndef __EMSCRIPTEN__
    // Blocks until `done()`: callbacks fire from wgpuInstanceProcessEvents and
    // device polls. Native only: the browser never blocks (see init).
    template<typename F>
    bool wait_for(F&& done) {
        for (int i = 0; !done(); ++i) {
            if (i > 100000) {
                LOG_ERROR("WebGPU: timed out waiting for a callback");
                return false;
            }
            wgpuInstanceProcessEvents(instance_);
            if (done()) break;
            if (device_) wgpuDevicePoll(device_, true, nullptr);
        }
        return true;
    }
#endif

    WGPUSurface create_surface(Window& window) {
        WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
#ifdef __EMSCRIPTEN__
        (void)window;
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
        canvas.selector = sv("#canvas");
        desc.nextInChain = &canvas.chain;
        return wgpuInstanceCreateSurface(instance_, &desc);
#else
        const SDL_PropertiesID props = SDL_GetWindowProperties(window.native());
        const char* driver_name = SDL_GetCurrentVideoDriver();
        const std::string_view driver = driver_name ? driver_name : "";
        if (driver == "wayland") {
            WGPUSurfaceSourceWaylandSurface wl = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
            wl.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
            wl.surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
            if (!wl.display || !wl.surface) return nullptr;
            desc.nextInChain = &wl.chain;
            return wgpuInstanceCreateSurface(instance_, &desc);
        }
        if (driver == "x11") {
            WGPUSurfaceSourceXlibWindow x = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
            x.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            x.window = static_cast<std::uint64_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
            if (!x.display || x.window == 0) return nullptr;
            desc.nextInChain = &x.chain;
            return wgpuInstanceCreateSurface(instance_, &desc);
        }
        if (driver == "windows") {
            WGPUSurfaceSourceWindowsHWND w = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
            w.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
            w.hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
            if (!w.hwnd) return nullptr;
            desc.nextInChain = &w.chain;
            return wgpuInstanceCreateSurface(instance_, &desc);
        }
        return nullptr; // offscreen / dummy video drivers: render offscreen
#endif
    }

#ifndef __EMSCRIPTEN__
    bool request_adapter() {
        struct Result {
            bool done = false;
            WGPUAdapter adapter = nullptr;
            std::string message;
        } result;
        WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
        options.powerPreference = WGPUPowerPreference_HighPerformance;
        options.compatibleSurface = surface_;
        WGPURequestAdapterCallbackInfo cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* user,
                         void*) {
            auto* r = static_cast<Result*>(user);
            r->done = true;
            if (status == WGPURequestAdapterStatus_Success) {
                r->adapter = adapter;
            } else {
                r->message = to_string(message);
            }
        };
        cb.userdata1 = &result;
        wgpuInstanceRequestAdapter(instance_, &options, cb);
        if (!wait_for([&] { return result.done; }) || !result.adapter) {
            LOG_ERROR("WebGPU: no adapter: {}", result.message);
            return false;
        }
        adapter_ = result.adapter;
        return true;
    }

    bool request_device() {
        struct Result {
            bool done = false;
            WGPUDevice device = nullptr;
            std::string message;
        } result;
        WGPUDeviceDescriptor desc = WGPU_DEVICE_DESCRIPTOR_INIT;
        desc.label = sv("cuervo");
        desc.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType type, WGPUStringView message,
                                                       void* user, void*) {
            static_cast<WebGpuDevice*>(user)->report_error(type, to_string(message));
        };
        desc.uncapturedErrorCallbackInfo.userdata1 = this;
        desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        desc.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason,
                                                  WGPUStringView message, void*, void*) {
            if (reason != WGPUDeviceLostReason_Destroyed && reason != WGPUDeviceLostReason_CallbackCancelled) {
                LOG_ERROR("WebGPU device lost: {}", to_string(message));
            }
        };
        WGPURequestDeviceCallbackInfo cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowProcessEvents;
        cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* user,
                         void*) {
            auto* r = static_cast<Result*>(user);
            r->done = true;
            if (status == WGPURequestDeviceStatus_Success) {
                r->device = device;
            } else {
                r->message = to_string(message);
            }
        };
        cb.userdata1 = &result;
        wgpuAdapterRequestDevice(adapter_, &desc, cb);
        if (!wait_for([&] { return result.done; }) || !result.device) {
            LOG_ERROR("WebGPU: device request failed: {}", result.message);
            return false;
        }
        device_ = result.device;
        return true;
    }
#endif

    void report_error(WGPUErrorType type, const std::string& message) {
        if (errors_logged_ >= max_logged_errors) return;
        const char* kind = type == WGPUErrorType_Validation    ? "validation"
                           : type == WGPUErrorType_OutOfMemory ? "out-of-memory"
                                                               : "internal";
        LOG_ERROR("WebGPU {} error: {}", kind, message);
        if (++errors_logged_ == max_logged_errors) LOG_ERROR("WebGPU: further errors are not logged");
    }

    // Non-sRGB 8-bit target (the GL and Vulkan backends write unconverted
    // values too), copyable for capture_frame when the surface allows it.
    bool choose_surface_config(bool vsync, bool& capture) {
        WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
        if (wgpuSurfaceGetCapabilities(surface_, adapter_, &caps) != WGPUStatus_Success || caps.formatCount == 0) {
            LOG_WARN("WebGPU: the window surface reports no formats");
            return false;
        }
        const std::span<const WGPUTextureFormat> formats(caps.formats, caps.formatCount);
        const std::span<const WGPUPresentMode> modes(caps.presentModes, caps.presentModeCount);
        const std::span<const WGPUCompositeAlphaMode> alphas(caps.alphaModes, caps.alphaModeCount);
        auto has = [](auto list, auto value) { return std::find(list.begin(), list.end(), value) != list.end(); };
        bool found = false;
        for (WGPUTextureFormat f : {WGPUTextureFormat_BGRA8Unorm, WGPUTextureFormat_RGBA8Unorm}) {
            if (!found && has(formats, f)) {
                target_format_ = f;
                found = true;
            }
        }
        if (!found) {
            LOG_WARN("WebGPU: the window surface has no 8-bit unorm format");
        }
#ifdef __EMSCRIPTEN__
        // emdawnwebgpu leaves `usages` empty; a canvas accepts COPY_SRC
        // (WebGPU spec, GPUCanvasConfiguration.usage).
        surface_copy_src_ = true;
#else
        surface_copy_src_ = (caps.usages & WGPUTextureUsage_CopySrc) != 0;
#endif
        surface_usage_ = WGPUTextureUsage_RenderAttachment | (surface_copy_src_ ? WGPUTextureUsage_CopySrc : 0);
        capture = surface_copy_src_;
        present_mode_ = WGPUPresentMode_Fifo; // always available
        if (!vsync) {
            for (WGPUPresentMode m : {WGPUPresentMode_Immediate, WGPUPresentMode_Mailbox}) {
                if (present_mode_ == WGPUPresentMode_Fifo && has(modes, m)) present_mode_ = m;
            }
        }
        alpha_mode_ = has(alphas, WGPUCompositeAlphaMode_Opaque) || alphas.empty() ? WGPUCompositeAlphaMode_Opaque
                                                                                  : alphas[0];
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        return found;
    }

    void configure_surface() {
        WGPUSurfaceConfiguration c = WGPU_SURFACE_CONFIGURATION_INIT;
        c.device = device_;
        c.format = target_format_;
        c.usage = surface_usage_;
        c.width = static_cast<std::uint32_t>(fb_size_.x());
        c.height = static_cast<std::uint32_t>(fb_size_.y());
        c.alphaMode = alpha_mode_;
        c.presentMode = present_mode_;
        wgpuSurfaceConfigure(surface_, &c);
        surface_size_ = fb_size_;
    }

    WGPUTexture make_target(WGPUTextureUsage usage, std::uint32_t samples, const char* label) {
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.label = sv(label);
        td.usage = usage;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {static_cast<std::uint32_t>(fb_size_.x()), static_cast<std::uint32_t>(fb_size_.y()), 1};
        td.format = target_format_;
        td.mipLevelCount = 1;
        td.sampleCount = samples;
        return wgpuDeviceCreateTexture(device_, &td);
    }

    // This frame's color target: the surface texture, or the offscreen one
    // when there is no surface (or it can't be acquired this frame).
    void acquire_target() {
        presenting_ = false;
        if (surface_) {
            if (surface_size_ != fb_size_) configure_surface();
            for (int attempt = 0; attempt < 2 && !presenting_; ++attempt) {
                WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
                wgpuSurfaceGetCurrentTexture(surface_, &st);
                if (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
                    st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
                    target_ = st.texture;
                    target_view_ = wgpuTextureCreateView(st.texture, nullptr);
                    presenting_ = true;
                } else {
                    if (st.texture) wgpuTextureRelease(st.texture);
                    configure_surface(); // outdated or lost: try once more
                }
            }
            if (!presenting_ && !acquire_warned_) {
                LOG_WARN("WebGPU: could not acquire the surface texture; rendering offscreen this frame");
                acquire_warned_ = true;
            }
        }
        if (!presenting_) {
            if (!offscreen_ || offscreen_size_ != fb_size_) {
                if (offscreen_view_) wgpuTextureViewRelease(offscreen_view_);
                if (offscreen_) wgpuTextureRelease(offscreen_);
                offscreen_ = make_target(WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc, 1,
                                         "cuervo_offscreen");
                offscreen_view_ = wgpuTextureCreateView(offscreen_, nullptr);
                offscreen_size_ = fb_size_;
            }
            target_ = offscreen_;
            target_view_ = offscreen_view_;
        }
        if (msaa_samples_ > 1 && (!msaa_ || msaa_size_ != fb_size_)) {
            if (msaa_view_) wgpuTextureViewRelease(msaa_view_);
            if (msaa_) wgpuTextureRelease(msaa_);
            msaa_ = make_target(WGPUTextureUsage_RenderAttachment, msaa_samples_, "cuervo_msaa");
            msaa_view_ = wgpuTextureCreateView(msaa_, nullptr);
            msaa_size_ = fb_size_;
        }
    }

    void release_frame_target() {
        if (presenting_) {
            wgpuTextureViewRelease(target_view_);
            wgpuTextureRelease(target_);
        }
        target_ = nullptr;
        target_view_ = nullptr;
        presenting_ = false;
    }

    void begin_encoder() {
        WGPUCommandEncoderDescriptor ed = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
        encoder_ = wgpuDeviceCreateCommandEncoder(device_, &ed);
    }

    // Submits the encoder; a new epoch starts (buffers read by its draws may
    // now be written in place again). `frame_end` submissions bound how many
    // frames may queue, as the GL backend's fence ring does.
    void submit(bool frame_end) {
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder_, nullptr);
        wgpuCommandEncoderRelease(encoder_);
        encoder_ = nullptr;
#ifdef __EMSCRIPTEN__
        (void)frame_end;
        wgpuQueueSubmit(queue_, 1, &commands);
#else
        const WGPUSubmissionIndex index = wgpuQueueSubmitForIndex(queue_, 1, &commands);
        if (frame_end) {
            in_flight_.push_back(index);
            while (in_flight_.size() > max_frames_in_flight) {
                wgpuDevicePoll(device_, true, &in_flight_.front());
                in_flight_.pop_front();
            }
            peak_frames_in_flight_ = std::max(peak_frames_in_flight_, in_flight_.size());
        }
#endif
        wgpuCommandBufferRelease(commands);
        ++epoch_;
    }

    void ensure_pass() {
        if (pass_) return;
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = msaa_samples_ > 1 ? msaa_view_ : target_view_;
        color.resolveTarget = msaa_samples_ > 1 ? target_view_ : nullptr;
        // The first pass of a frame starts from black (the GL and Vulkan
        // targets hold undefined contents there; callers clear first).
        color.loadOp = first_pass_ || pending_clear_ ? WGPULoadOp_Clear : WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        const rhi::Color c = pending_clear_.value_or(rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
        color.clearValue = {c.r, c.g, c.b, c.a};
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &color;
        pass_ = wgpuCommandEncoderBeginRenderPass(encoder_, &rp);
        first_pass_ = false;
        pending_clear_.reset();
        applied_ = {};
    }

    void end_pass() {
        if (!pass_) return;
        wgpuRenderPassEncoderEnd(pass_);
        wgpuRenderPassEncoderRelease(pass_);
        pass_ = nullptr;
    }

    GpuBuffer new_gpu_buffer(WGPUBufferUsage usage, std::size_t size) {
        for (auto it = retired_.begin(); it != retired_.end(); ++it) {
            if (it->usage_flags == usage && it->size == size && it->epoch < epoch_) {
                const GpuBuffer gpu{it->gpu, it->serial};
                retired_.erase(it);
                return gpu;
            }
        }
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.usage = usage;
        bd.size = size;
        return {wgpuDeviceCreateBuffer(device_, &bd), ++buffer_serial_};
    }

    void retire(const Buffer& b) {
        retired_.push_back({b.gpu, b.serial, b.usage_flags, b.shadow.size(), epoch_, frame_});
        if (retired_.size() > max_retired_buffers) {
            wgpuBufferRelease(retired_.front().gpu);
            retired_.pop_front();
        }
    }

    void trim_caches() {
        while (!retired_.empty() && frame_ - retired_.front().frame > retired_keep_frames) {
            wgpuBufferRelease(retired_.front().gpu);
            retired_.pop_front();
        }
        if (bind_groups_.size() > max_cached_bind_groups) {
            for (auto& [key, group] : bind_groups_) wgpuBindGroupRelease(group);
            bind_groups_.clear();
        }
    }

    void write_texture(const Texture& t, rhi::Rect region, const void* data) {
        const std::size_t texel = rhi::texture_format_size(t.format);
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = t.gpu;
        dst.origin = {static_cast<std::uint32_t>(region.x), static_cast<std::uint32_t>(region.y), 0};
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = static_cast<std::uint32_t>(texel * static_cast<std::size_t>(region.width));
        layout.rowsPerImage = static_cast<std::uint32_t>(region.height);
        const WGPUExtent3D extent{static_cast<std::uint32_t>(region.width), static_cast<std::uint32_t>(region.height),
                                  1};
        wgpuQueueWriteTexture(queue_, &dst, data, std::size_t{layout.bytesPerRow} * layout.rowsPerImage, &layout,
                              &extent);
    }

    WGPUShaderModule make_module(std::string_view wgsl, std::string_view label) {
        WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
        source.code = sv(wgsl);
        WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        md.nextInChain = &source.chain;
        md.label = sv(label);
        return wgpuDeviceCreateShaderModule(device_, &md);
    }

    std::array<WGPUBindGroupLayout, 2> make_bind_group_layouts(const Pipeline& p) {
        std::vector<WGPUBindGroupLayoutEntry> uniforms;
        for (std::uint32_t binding : p.uniform_bindings) {
            WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            e.binding = binding;
            e.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            e.buffer.type = WGPUBufferBindingType_Uniform;
            e.buffer.hasDynamicOffset = true;
            uniforms.push_back(e);
        }
        std::vector<WGPUBindGroupLayoutEntry> textures;
        for (std::uint32_t binding : p.texture_bindings) {
            WGPUBindGroupLayoutEntry t = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            t.binding = binding * 2;
            t.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            t.texture.sampleType = WGPUTextureSampleType_Float;
            t.texture.viewDimension = WGPUTextureViewDimension_2D;
            textures.push_back(t);
            WGPUBindGroupLayoutEntry s = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            s.binding = binding * 2 + 1;
            s.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            s.sampler.type = WGPUSamplerBindingType_Filtering;
            textures.push_back(s);
        }
        std::array<WGPUBindGroupLayout, 2> layouts{};
        for (std::size_t g = 0; g < 2; ++g) {
            const std::vector<WGPUBindGroupLayoutEntry>& entries = g == 0 ? uniforms : textures;
            WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            ld.entryCount = entries.size();
            ld.entries = entries.data();
            layouts[g] = wgpuDeviceCreateBindGroupLayout(device_, &ld);
        }
        return layouts;
    }

    void release(Pipeline& p) {
        if (p.gpu) wgpuRenderPipelineRelease(p.gpu);
        for (WGPUBindGroupLayout l : p.layouts) {
            if (l) wgpuBindGroupLayoutRelease(l);
        }
        p = {};
    }

    // `key` names what `entries` bind by identity (see GpuBuffer): the
    // pipeline id and group, then per entry a buffer serial and range or a
    // texture id and filter.
    WGPUBindGroup bind_group(WGPUBindGroupLayout layout, const std::vector<WGPUBindGroupEntry>& entries,
                             std::vector<std::uint64_t> key) {
        auto it = bind_groups_.find(key);
        if (it != bind_groups_.end()) return it->second;
        WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bd.layout = layout;
        bd.entryCount = entries.size();
        bd.entries = entries.data();
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(device_, &bd);
        bind_groups_.emplace(std::move(key), group);
        return group;
    }

    // Opens the pass if needed and applies the pipeline, viewport, scissor,
    // vertex streams and both bind groups for the next draw.
    const Pipeline* prepare_draw() {
        if (!in_frame_) {
            LOG_ERROR("draw outside begin_frame/end_frame");
            return nullptr;
        }
        const Pipeline* p = find(pipelines_, pipeline_);
        if (!p) {
            if (!no_pipeline_reported_) LOG_ERROR("draw: no valid pipeline bound");
            no_pipeline_reported_ = true;
            return nullptr;
        }
        ensure_pass();
        if (applied_.pipeline != pipeline_) {
            wgpuRenderPassEncoderSetPipeline(pass_, p->gpu);
            applied_.pipeline = pipeline_;
        }
        if (applied_.viewport != viewport_) {
            wgpuRenderPassEncoderSetViewport(pass_, static_cast<float>(viewport_.x), static_cast<float>(viewport_.y),
                                             static_cast<float>(viewport_.width),
                                             static_cast<float>(viewport_.height), 0.0f, 1.0f);
            applied_.viewport = viewport_;
        }
        const rhi::Rect scissor = clamp_rect(scissor_, fb_size_);
        if (applied_.scissor != scissor) {
            wgpuRenderPassEncoderSetScissorRect(pass_, static_cast<std::uint32_t>(scissor.x),
                                                static_cast<std::uint32_t>(scissor.y),
                                                static_cast<std::uint32_t>(scissor.width),
                                                static_cast<std::uint32_t>(scissor.height));
            applied_.scissor = scissor;
        }
        for (std::uint32_t s = 0; s < p->stream_count; ++s) {
            Buffer* b = find(buffers_, vertex_[s].buffer);
            if (!b) {
                LOG_ERROR("draw: no vertex buffer bound to stream {}", s);
                return nullptr;
            }
            b->used_epoch = epoch_;
            const std::pair<std::uint64_t, std::size_t> binding{b->serial, vertex_[s].offset};
            if (applied_.vertex[s] != binding) {
                wgpuRenderPassEncoderSetVertexBuffer(pass_, s, b->gpu, vertex_[s].offset, WGPU_WHOLE_SIZE);
                applied_.vertex[s] = binding;
            }
        }

        std::vector<WGPUBindGroupEntry> entries;
        std::vector<std::uint32_t> offsets;
        std::vector<std::uint64_t> key = {pipeline_, 0};
        for (std::uint32_t binding : p->uniform_bindings) {
            WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
            e.binding = binding;
            const UniformBinding& u = uniforms_[binding];
            if (Buffer* b = find(buffers_, u.buffer); b && u.offset < b->size) {
                b->used_epoch = epoch_;
                e.buffer = b->gpu;
                e.size = std::min(u.size != 0 ? u.size : b->size - u.offset, max_uniform_binding_);
                offsets.push_back(static_cast<std::uint32_t>(u.offset));
                key.push_back(b->serial);
            } else {
                e.buffer = dummy_uniform_;
                e.size = dummy_uniform_size;
                offsets.push_back(0);
                key.push_back(0); // the dummy (serials start at 1)
            }
            key.push_back(e.size);
            entries.push_back(e);
        }
        wgpuRenderPassEncoderSetBindGroup(pass_, 0, bind_group(p->layouts[0], entries, std::move(key)),
                                          offsets.size(), offsets.data());

        entries.clear();
        key = {pipeline_, 1};
        for (std::uint32_t binding : p->texture_bindings) {
            const Texture* t = find(textures_by_id_, textures_[binding]);
            key.push_back(t ? textures_[binding] : 0); // 0: the dummy (ids start at 1)
            key.push_back(t && t->filter == rhi::Filter::Linear ? 1 : 0);
            WGPUBindGroupEntry tex = WGPU_BIND_GROUP_ENTRY_INIT;
            tex.binding = binding * 2;
            tex.textureView = t ? t->view : dummy_texture_view_;
            entries.push_back(tex);
            WGPUBindGroupEntry smp = WGPU_BIND_GROUP_ENTRY_INIT;
            smp.binding = binding * 2 + 1;
            smp.sampler = samplers_[t && t->filter == rhi::Filter::Linear ? 1 : 0];
            entries.push_back(smp);
        }
        wgpuRenderPassEncoderSetBindGroup(pass_, 1, bind_group(p->layouts[1], entries, std::move(key)), 0, nullptr);
        return p;
    }

    bool prepare_index() {
        Buffer* b = find(buffers_, index_buffer_);
        if (!b) {
            LOG_ERROR("draw_indexed: no index buffer bound");
            return false;
        }
        b->used_epoch = epoch_;
        const WGPUIndexFormat format =
            index_format_ == rhi::IndexFormat::UInt32 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16;
        if (applied_.index != b->serial || applied_.index_format != format) {
            wgpuRenderPassEncoderSetIndexBuffer(pass_, b->gpu, format, 0, WGPU_WHOLE_SIZE);
            applied_.index = b->serial;
            applied_.index_format = format;
        }
        return true;
    }

    bool create_internal_resources() {
        for (int f = 0; f < 2; ++f) {
            WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
            sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
            sd.magFilter = sd.minFilter = f == 1 ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
            sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            samplers_[static_cast<std::size_t>(f)] = wgpuDeviceCreateSampler(device_, &sd);
        }
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bd.size = dummy_uniform_size;
        dummy_uniform_ = wgpuDeviceCreateBuffer(device_, &bd);
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.usage = WGPUTextureUsage_TextureBinding;
        td.dimension = WGPUTextureDimension_2D;
        td.size = {1, 1, 1};
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = 1;
        td.sampleCount = 1;
        dummy_texture_ = wgpuDeviceCreateTexture(device_, &td);
        dummy_texture_view_ = wgpuTextureCreateView(dummy_texture_, nullptr);

        clear_buffer_ = create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Stream,
                                       clear_slot_size * clear_slots, nullptr});
        rhi::PipelineDesc clear_desc;
        clear_desc.vertex.wgsl = clear_vertex_wgsl;
        clear_desc.fragment.wgsl = clear_fragment_wgsl;
        clear_desc.fragment.uniform_blocks = clear_uniforms;
        clear_desc.debug_name = "cuervo_clear";
        clear_pipeline_ = create_pipeline(clear_desc);
        return samplers_[0] && samplers_[1] && dummy_uniform_ && dummy_texture_view_ && clear_buffer_ &&
               clear_pipeline_;
    }

    void shutdown() {
        if (device_) {
            if (in_frame_) {
                end_pass();
                if (encoder_) wgpuCommandEncoderRelease(encoder_);
                encoder_ = nullptr;
                release_frame_target();
            }
#ifndef __EMSCRIPTEN__
            wgpuDevicePoll(device_, true, nullptr); // let the queue drain
#endif
        }
        for (auto& [key, group] : bind_groups_) wgpuBindGroupRelease(group);
        bind_groups_.clear();
        for (auto& [id, p] : pipelines_) release(p);
        pipelines_.clear();
        for (auto& [id, b] : buffers_) {
            if (b.gpu) wgpuBufferRelease(b.gpu);
        }
        buffers_.clear();
        for (const RetiredBuffer& r : retired_) wgpuBufferRelease(r.gpu);
        retired_.clear();
        for (auto& [id, t] : textures_by_id_) {
            wgpuTextureViewRelease(t.view);
            wgpuTextureRelease(t.gpu);
        }
        textures_by_id_.clear();
        for (WGPUSampler& s : samplers_) {
            if (s) wgpuSamplerRelease(s);
            s = nullptr;
        }
        auto release_texture = [](WGPUTexture& t, WGPUTextureView& v) {
            if (v) wgpuTextureViewRelease(v);
            if (t) wgpuTextureRelease(t);
            t = nullptr;
            v = nullptr;
        };
        release_texture(dummy_texture_, dummy_texture_view_);
        release_texture(offscreen_, offscreen_view_);
        release_texture(msaa_, msaa_view_);
        if (dummy_uniform_) wgpuBufferRelease(dummy_uniform_);
        dummy_uniform_ = nullptr;
        if (surface_) {
            if (surface_size_ != ivec2{0, 0}) wgpuSurfaceUnconfigure(surface_);
            wgpuSurfaceRelease(surface_);
            surface_ = nullptr;
        }
        if (queue_) wgpuQueueRelease(queue_);
        if (device_) wgpuDeviceRelease(device_);
        if (adapter_) wgpuAdapterRelease(adapter_);
        if (instance_) wgpuInstanceRelease(instance_);
        queue_ = nullptr;
        device_ = nullptr;
        adapter_ = nullptr;
        instance_ = nullptr;
    }

    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUSurface surface_ = nullptr;
    WGPUTextureFormat target_format_ = WGPUTextureFormat_RGBA8Unorm;
    WGPUTextureUsage surface_usage_ = WGPUTextureUsage_RenderAttachment;
    WGPUPresentMode present_mode_ = WGPUPresentMode_Fifo;
    WGPUCompositeAlphaMode alpha_mode_ = WGPUCompositeAlphaMode_Opaque;
    bool surface_copy_src_ = false;
    ivec2 surface_size_{0, 0};
    std::uint32_t msaa_samples_ = 1;
    std::size_t max_uniform_binding_ = 65536;

    rhi::DeviceInfo info_;
    std::string api_version_;
    std::string renderer_;

    // The frame.
    ivec2 fb_size_{1, 1};
    bool in_frame_ = false;
    bool presenting_ = false;
    bool first_pass_ = true;
    std::optional<rhi::Color> pending_clear_;
    WGPUTexture target_ = nullptr;
    WGPUTextureView target_view_ = nullptr;
    WGPUTexture offscreen_ = nullptr;
    WGPUTextureView offscreen_view_ = nullptr;
    ivec2 offscreen_size_{0, 0};
    WGPUTexture msaa_ = nullptr;
    WGPUTextureView msaa_view_ = nullptr;
    ivec2 msaa_size_{0, 0};
    WGPUCommandEncoder encoder_ = nullptr;
    WGPURenderPassEncoder pass_ = nullptr;
    std::uint64_t epoch_ = 1; // command buffers submitted so far + 1
    std::uint64_t frame_ = 0;
#ifndef __EMSCRIPTEN__
    std::deque<WGPUSubmissionIndex> in_flight_;
#endif
    std::size_t peak_frames_in_flight_ = 0;

    // Resources.
    std::uint32_t next_id_ = 1;
    std::unordered_map<std::uint32_t, Buffer> buffers_;
    std::unordered_map<std::uint32_t, Texture> textures_by_id_;
    std::unordered_map<std::uint32_t, Pipeline> pipelines_;
    std::deque<RetiredBuffer> retired_;
    std::map<std::vector<std::uint64_t>, WGPUBindGroup> bind_groups_;
    std::uint64_t buffer_serial_ = 0;
    std::array<WGPUSampler, 2> samplers_{}; // nearest, linear (clamp to edge)
    WGPUBuffer dummy_uniform_ = nullptr;
    WGPUTexture dummy_texture_ = nullptr;
    WGPUTextureView dummy_texture_view_ = nullptr;
    rhi::BufferHandle clear_buffer_;
    rhi::PipelineHandle clear_pipeline_;
    std::size_t clear_slot_ = 0;

    // Draw state (reset by begin_frame) and what the pass has applied.
    std::uint32_t pipeline_ = 0;
    std::array<VertexBinding, rhi::max_vertex_streams> vertex_{};
    std::uint32_t index_buffer_ = 0;
    rhi::IndexFormat index_format_ = rhi::IndexFormat::UInt16;
    std::array<UniformBinding, rhi::max_uniform_bindings> uniforms_{};
    std::array<std::uint32_t, rhi::max_texture_bindings> textures_{};
    rhi::Rect viewport_;
    rhi::Rect scissor_;
    AppliedState applied_;

    int errors_logged_ = 0;
    bool acquire_warned_ = false;
    bool no_pipeline_reported_ = false;
    bool base_vertex_reported_ = false;
};

} // namespace

std::unique_ptr<rhi::Device> create_device(Window& window, const rhi::DeviceOptions& options) {
    if (window.graphics() != GraphicsApi::WebGPU) {
        LOG_ERROR("WebGPU device needs a window created for GraphicsApi::WebGPU");
        return nullptr;
    }
    auto device = std::make_unique<WebGpuDevice>();
    if (!device->init(window, options)) return nullptr;
    return device;
}

std::size_t peak_frames_in_flight(const rhi::Device& device) {
    const auto* webgpu_device = dynamic_cast<const WebGpuDevice*>(&device);
    return webgpu_device ? webgpu_device->peak_frames_in_flight() : 0;
}

std::size_t frame_limit() { return max_frames_in_flight; }

#ifdef __EMSCRIPTEN__
// clang-format off
EM_JS(int, cuervo_webgpu_device_ready, (), { return Module['preinitializedWebGPUDevice'] ? 1 : 0; });
// clang-format on

bool browser_device_ready() { return cuervo_webgpu_device_ready() != 0; }
#endif

} // namespace cuervo::webgpu

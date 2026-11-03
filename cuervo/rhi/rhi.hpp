#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/math/vec.hpp"

struct SDL_Window;

namespace cuervo {
class Window;
enum class GraphicsApi; // platform/window.hpp
}

// Render hardware interface (DESIGN.md 4.4). The rest of the engine talks to
// this, never to Vulkan or GL directly. Everything here is `Tier::Compatible`
// (GLES 3.0 / WebGL2-level) unless noted.
//
// Conventions shared by both backends:
//  - Pixel rectangles (viewport, scissor) use a top-left origin, y down.
//  - NDC y = +1 is the top of the render target on every backend (the Vulkan
//    backend flips its viewport), so one projection works everywhere.
//  - Draw state (pipeline, buffers, viewport, scissor) is set between
//    begin_frame and end_frame and resets at every begin_frame.
namespace cuervo::rhi {

// WebGPU (milestone 10, experimental) is a Compatible-tier backend: native
// through wgpu-native, in the browser through Emscripten's emdawnwebgpu.
enum class Backend { OpenGL, Vulkan, WebGPU };

// Project render tier (DESIGN.md 4.4). `Compatible` = features every backend
// has (GLES 3.0 / WebGL2-level); `Full` = adds Vulkan-only features, no web
// export. Ordered: Full includes everything Compatible has.
enum class Tier { Compatible, Full };

std::string_view tier_name(Tier tier);              // "compatible" / "full"
bool parse_tier(std::string_view text, Tier& out);  // the project.toml spelling

// --- capabilities -----------------------------------------------------------------
// Every RHI feature that a device may or may not have, tagged with the tier
// that may use it. DeviceInfo reports which ones the device offers; a device
// gated to a tier (DeviceOptions::tier) refuses features above it even when
// the hardware has them, so a Compatible project tested on Vulkan behaves
// like it will on WebGL2.
enum class Feature : std::uint32_t {
    // Tier::Compatible -- all expressible in GLES 3.0 / WebGL2.
    InstancedDraws,    // draw/draw_indexed with instance_count and first_instance
    IndexUInt32,       // IndexFormat::UInt32
    UniformBuffers,    // bind_uniform_buffer with offsets
    Textures,          // RGBA8 / R8 2D textures, nearest / linear
    IntegerAttributes, // VertexFormat::UInt1
    PointsAndLines,    // Topology::PointList / LineList / LineStrip (1 px wide; shaders
                       // that write gl_PointSize write 1.0: WGSL has no point size)
    Msaa,              // DeviceOptions::msaa_samples > 1 (optional: falls back to 1)
    FrameCapture,      // capture_frame (debug aid; optional)
    TextureMips,       // TextureDesc::mip_levels > 1, per-level data, sRGB RGBA8 (11.2a)
    RenderTargets,     // create_render_target / set_render_target: offscreen passes (11.2b)
    // Tier::Full -- Vulkan only.
    BaseVertexDraws,   // draw_indexed_base_vertex (GLES 3.0 has no base vertex)
    // Compatible, listed last to keep the older entries' positions.
    MultiSurface,      // create_window_surface: more windows than the main one (optional;
                       // desktop only -- WebGPU and the web stay single-window)
    Count
};

Tier feature_tier(Feature feature);
std::string_view feature_name(Feature feature);
// Optional features degrade (no MSAA, no capture) instead of breaking a
// project, so missing them only warns; a missing required feature of the
// project's tier means the project can't run on that device.
bool feature_required(Feature feature);

class FeatureSet {
public:
    constexpr FeatureSet() = default;
    constexpr void set(Feature f, bool on = true) {
        const std::uint64_t bit = std::uint64_t{1} << static_cast<std::uint32_t>(f);
        bits_ = on ? (bits_ | bit) : (bits_ & ~bit);
    }
    constexpr bool has(Feature f) const { return (bits_ >> static_cast<std::uint32_t>(f)) & 1u; }
    constexpr std::uint64_t bits() const { return bits_; }
    friend constexpr bool operator==(FeatureSet, FeatureSet) = default;

private:
    std::uint64_t bits_ = 0;
};

// Numeric limits. `compatible_limits()` are what the Compatible tier may rely
// on everywhere (WebGL2 in practice; the GLES 3.0 spec minimums are lower for
// some, see DESIGN.md 4.4 "Clarified (Phase 5)"); a device gated to
// Compatible enforces them even when the hardware allows more.
struct Limits {
    std::uint32_t max_texture_size = 4096;      // width and height, texels
    std::uint32_t max_msaa_samples = 4;
    std::uint32_t max_uniform_block_size = 16384; // bytes per bind_uniform_buffer range
    std::uint32_t max_vertex_attributes = 16;
    std::uint32_t uniform_offset_alignment = 256;
};
Limits compatible_limits();

struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

struct Rect {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
    friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

// Handles are plain ids; 0 is always invalid.
struct BufferHandle { std::uint32_t id = 0; explicit operator bool() const { return id != 0; } };
struct PipelineHandle { std::uint32_t id = 0; explicit operator bool() const { return id != 0; } };
struct TextureHandle { std::uint32_t id = 0; explicit operator bool() const { return id != 0; } };
struct SurfaceHandle { std::uint32_t id = 0; explicit operator bool() const { return id != 0; } };
struct RenderTargetHandle { std::uint32_t id = 0; explicit operator bool() const { return id != 0; } };

enum class BufferKind { Vertex, Index, Uniform };

// Static: written rarely; updating it while the GPU may still read it is the
//   caller's problem (no renaming).
// Dynamic: updated now and then; contents persist across frames. Updates never
//   race the GPU (the backend renames the storage, copying the rest forward).
// Stream: rewritten every frame. Updates never race the GPU, but bytes not
//   written since the frame's first update are undefined. Updates in one frame
//   to byte ranges beside each other (a batcher's per-layer flush) are cheap on
//   both backends; rewriting bytes a recorded draw already reads is correct
//   but may copy.
// Updates follow GL semantics on both backends: draws recorded before an
// update see the old data, draws after it see the new data.
enum class BufferUsage { Static, Dynamic, Stream };

struct BufferDesc {
    BufferKind kind = BufferKind::Vertex;
    BufferUsage usage = BufferUsage::Static;
    std::size_t size = 0;
    const void* initial_data = nullptr; // optional
};

enum class IndexFormat { UInt16, UInt32 };

// --- textures -------------------------------------------------------------------
// 2D textures, sampled with clamp-to-edge addressing (GLES 3.0 / WebGL2 allow
// nothing else for non-power-of-two sizes without mips). A texture may carry a
// mip chain (Feature::TextureMips); the sampler then filters between levels:
// Filter::Linear is trilinear, Filter::Nearest picks the nearest level.
// The sampler is part of the texture: shaders declare combined samplers.
enum class TextureFormat {
    RGBA8, // 4 x u8 normalized, bytes r, g, b, a in memory
    R8,    // 1 x u8 normalized; samples as (r, 0, 0, 1)
};

enum class Filter { Nearest, Linear };

struct TextureDesc {
    std::int32_t width = 0;
    std::int32_t height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    Filter filter = Filter::Linear;
    // Optional: tightly packed rows, top row first (texture coordinate v = 0
    // is the first row on every backend). Without it the texture starts as
    // transparent black.
    const void* initial_data = nullptr;
    std::string_view debug_name;
    // Mip chain length: 1 = no mips, 0 = the full chain
    // (floor(log2(max(width, height))) + 1), anything else must not exceed it.
    // Level n is max(1, width >> n) x max(1, height >> n). Levels without data
    // start as transparent black; update_texture only writes level 0.
    std::uint32_t mip_levels = 1;
    // RGBA8 only: the sampler decodes the stored bytes from sRGB to linear
    // (filtering happens after decoding). Alpha stays linear.
    bool srgb = false;
    // Optional per-level data, level 0 first, each level tightly packed like
    // initial_data. When non-empty it replaces initial_data (an empty span
    // leaves that level zeroed); extra entries beyond the chain are an error.
    std::span<const std::span<const std::byte>> initial_levels = {};
};

// Number of levels of the full chain for a width x height texture.
std::uint32_t full_mip_chain_length(std::int32_t width, std::int32_t height);

std::size_t texture_format_size(TextureFormat format); // bytes per texel

// --- render targets (Feature::RenderTargets, 11.2b) -----------------------------
// An offscreen color target: a pass renders into it with set_render_target and
// later passes sample it as an ordinary texture (render_target_texture), which
// is how multi-pass post-processing works (DESIGN.md 4.19).
//  - The format is the window's color format, and the target has the device's
//    MSAA sample count (resolved when a pass leaves it), so every pipeline
//    works with the window and with any target unchanged.
//  - It starts transparent black. Its contents persist across passes and
//    frames until cleared; set_render_target(prev) then back resumes drawing.
//  - A target must not be sampled while it is the current render target.
struct RenderTargetDesc {
    std::int32_t width = 0;
    std::int32_t height = 0;
    Filter filter = Filter::Linear; // sampler used when the target is read as a texture
    std::string_view debug_name;
};

// --- shaders ----------------------------------------------------------------
// Shaders are authored once in GLSL 450 under cuervo/render/shaders/ and turned
// into these by the build (cmake/Shaders.cmake); see `shaders/<name>_<stage>.hpp`.
// Uniform blocks must live in descriptor set 0, bindings [0, max_uniform_bindings).
struct UniformBlockBinding {
    std::string_view name; // block name as declared in the generated GLSL
    std::uint32_t binding = 0;
};

// Textures are combined samplers in descriptor set 1:
//   layout(set = 1, binding = N) uniform sampler2D name;   N < max_texture_bindings
// fed by bind_texture(N, ...). GL has no binding layouts in GLSL 330 / ES 300,
// so it sets each sampler uniform to texture unit N after linking.
struct TextureBinding {
    std::string_view name; // sampler uniform name in the generated GLSL
    std::uint32_t binding = 0;
};

struct ShaderSource {
    std::span<const std::uint32_t> spirv; // Vulkan
    std::string_view glsl330;             // desktop GL 3.3 core
    std::string_view glsl_es300;          // GLES 3.0 / WebGL2
    // WebGPU, translated from the SPIR-V by naga; empty in builds without the
    // WebGPU backend. Texture binding N is split into @group(1) @binding(2N)
    // (texture) and @binding(2N + 1) (sampler); uniforms keep @group(0).
    std::string_view wgsl;
    std::span<const UniformBlockBinding> uniform_blocks; // GL applies these after linking
    std::span<const TextureBinding> textures;            // likewise
};

inline constexpr std::uint32_t max_uniform_bindings = 4;
inline constexpr std::uint32_t max_texture_bindings = 4;
inline constexpr std::uint32_t max_vertex_streams = 4;
inline constexpr std::uint32_t max_vertex_attributes = 16;

// --- pipelines ----------------------------------------------------------------
enum class VertexFormat {
    Float1, Float2, Float3, Float4,
    UByte4Norm, // 4 x u8 -> vec4 in [0, 1]
    UInt1,      // u32 -> uint (integer attribute, not normalized)
};

enum class VertexRate { PerVertex, PerInstance };

struct VertexAttribute {
    std::uint32_t location = 0;
    VertexFormat format = VertexFormat::Float2;
    std::uint32_t offset = 0;
};

// One vertex buffer binding. Stream i is fed by bind_vertex_buffer(i, ...).
struct VertexStream {
    std::uint32_t stride = 0;
    VertexRate rate = VertexRate::PerVertex;
    std::span<const VertexAttribute> attributes;
};

enum class BlendMode {
    None,          // opaque
    Alpha,         // straight alpha: src*a + dst*(1-a)
    Premultiplied, // src + dst*(1-a)
    Additive,      // src*a + dst
    // Screen with a premultiplied source: src + dst*(1 - src) per color
    // channel (alpha accumulates like Premultiplied). A glow that never
    // exceeds white and never darkens (11.2b).
    Screen,
    AdditivePremultiplied, // src + dst: glow from a shader that outputs premultiplied color
};

enum class Topology { TriangleList, TriangleStrip, LineList, LineStrip, PointList };

struct PipelineDesc {
    ShaderSource vertex;
    ShaderSource fragment;
    std::span<const VertexStream> streams; // index = stream (binding) number
    BlendMode blend = BlendMode::None;
    Topology topology = Topology::TriangleList;
    std::string_view debug_name;
};

struct DeviceInfo {
    Backend backend = Backend::OpenGL;
    // Highest tier the backend + hardware can serve.
    Tier max_tier = Tier::Compatible;
    // Tier the device is gated to: min(DeviceOptions::tier, max_tier).
    Tier tier = Tier::Compatible;
    std::string_view api_version;
    std::string_view renderer;
    // True on OpenGL ES / WebGL2 contexts (GLSL ES 300 shaders).
    bool gles = false;
    // Features usable now (hardware and gating), and what the hardware has.
    FeatureSet features;
    FeatureSet hardware_features;
    // Effective limits (hardware, clamped to compatible_limits() when gated).
    Limits limits;
    // bind_uniform_buffer offsets must be multiples of this (= limits.uniform_offset_alignment).
    std::uint32_t uniform_offset_alignment = 256;
    // Color samples per pixel actually in use (1 = no MSAA).
    std::uint32_t msaa_samples = 1;
    // True on GL: a render target's rows are stored bottom-up there, so a pass
    // that samples a target flips its texture coordinate v (1 - v). Vulkan and
    // WebGPU store top-down like every other texture. The window itself is
    // unaffected: draws use NDC y = +1 = top everywhere.
    bool render_target_flip_v = false;
    // Linear color (DESIGN.md 4.19, 11.2b): true when DeviceOptions::linear_color
    // was granted. Render targets (and, where `window_srgb`, the window) are then
    // sRGB-encoded storage: shaders write linear values, blending happens in
    // linear space, and sampling a target returns linear values. Textures made
    // with `srgb = true` decode when sampled either way.
    bool linear_color = false;
    // True when the window's own framebuffer encodes linear values on write
    // (Vulkan/WebGPU sRGB surface formats). Where false (GL), the last pass into
    // the window must encode in the shader (PostFx does).
    bool window_srgb = false;

    bool supports(Feature f) const { return features.has(f); }
};

// What a device lacks for running a project of `tier`.
struct TierReport {
    std::vector<Feature> missing_required; // the project can't run (Full) / may break (Compatible)
    std::vector<Feature> missing_optional; // degrades (no MSAA, no capture)
    std::vector<std::string> limit_warnings; // hardware limits below compatible_limits()
    bool gated_down = false;               // device serves a lower tier than asked
    bool ok() const { return missing_required.empty() && !gated_down; }
};
TierReport check_tier(const DeviceInfo& info, Tier tier);

struct DeviceOptions {
    bool vsync = true;
    // Tier gate (see Feature). Requests above what the backend can serve are
    // lowered to DeviceInfo::max_tier; check DeviceInfo::tier.
    Tier tier = Tier::Compatible;
    // Multisample anti-aliasing (Tier::Compatible): the frame is rendered into
    // a multisampled color target and resolved into the window when the frame
    // ends (and before capture_frame reads it). This is the anti-aliasing for
    // tessellated vector meshes (DESIGN.md 4.4); SDF shapes anti-alias
    // analytically and look the same either way. 1 disables it; the backend
    // clamps to what the GPU supports (GLES 3.0 guarantees 4) and reports
    // the result in DeviceInfo::msaa_samples. Every pipeline is created for
    // the device's sample count.
    std::uint32_t msaa_samples = 4;
    // Ask for linear color (see DeviceInfo::linear_color). Backends that can't
    // provide sRGB targets leave DeviceInfo::linear_color false and the caller
    // stays in sRGB blending.
    bool linear_color = false;
};

class Device {
public:
    virtual ~Device() = default;

    virtual const DeviceInfo& info() const = 0;

    // Starts a frame on the window's framebuffer. Resets viewport and scissor
    // to the whole framebuffer and unbinds everything.
    virtual void begin_frame(ivec2 framebuffer_size) = 0;
    // Clears the current scissor rectangle.
    virtual void clear(Color color) = 0;
    virtual void end_frame() = 0; // presents

    // --- secondary window surfaces (Feature::MultiSurface, DESIGN.md 4.4) ----
    // Presents to more windows than the one the device was created for (the
    // editor's undocked panels). The window must have been created for the
    // device's graphics API (create_surface_window does that). Buffers,
    // textures and pipelines are shared with the main window. A surface frame
    // is an ordinary frame (same draw state rules, reset at begin) bracketed
    // by begin_frame_surface / end_frame_surface instead of begin_frame /
    // end_frame; frames never overlap, so finish the main window's first.
    // On GL render targets can't be used during a surface frame (framebuffer
    // objects are per context); on Vulkan the surface must present with the
    // main window's color format. Destroy surfaces before their window.
    // Without the feature create_window_surface logs why and returns {}.
    virtual SurfaceHandle create_window_surface(SDL_Window* window);
    virtual void destroy_window_surface(SurfaceHandle surface);
    virtual void begin_frame_surface(SurfaceHandle surface, ivec2 framebuffer_size);
    virtual void end_frame_surface();

    // Size, in pixels, of the current render target (the window's framebuffer
    // unless set_render_target selected an offscreen target).
    virtual ivec2 framebuffer_size() const = 0;

    // Render targets (Feature::RenderTargets). set_render_target({}) selects the
    // window again. Selecting a target resets viewport and scissor to its full
    // size; pipeline and buffer bindings carry over. clear() and draws apply to
    // the current target. begin_frame selects the window; end_frame presents it
    // (a target still selected is left first). Destroying a target is deferred
    // until the GPU is done with it, and destroys its texture handle too.
    virtual RenderTargetHandle create_render_target(const RenderTargetDesc& desc) = 0;
    virtual TextureHandle render_target_texture(RenderTargetHandle target) const = 0;
    virtual void destroy_render_target(RenderTargetHandle target) = 0;
    virtual void set_render_target(RenderTargetHandle target) = 0;

    virtual BufferHandle create_buffer(const BufferDesc& desc) = 0;
    virtual void update_buffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) = 0;
    virtual void destroy_buffer(BufferHandle buffer) = 0;

    // Textures (Tier::Compatible). update_texture writes `region` (texels) from
    // tightly packed rows, top row first. Ordering: an update is visible to
    // every draw recorded after it. Draws recorded *before* it in the same
    // frame see the old contents on GL but may see the new ones on Vulkan
    // (uploads run before the frame's draws), so update a texture before
    // drawing with it in a frame -- growing an atlas only adds regions that
    // earlier draws don't sample, which is safe either way.
    // Destroying a texture is deferred until the GPU is done with it.
    virtual TextureHandle create_texture(const TextureDesc& desc) = 0;
    virtual void update_texture(TextureHandle texture, Rect region, std::span<const std::byte> data) = 0;
    virtual void destroy_texture(TextureHandle texture) = 0;

    virtual PipelineHandle create_pipeline(const PipelineDesc& desc) = 0;
    virtual void destroy_pipeline(PipelineHandle pipeline) = 0;

    // --- draw state (valid between begin_frame and end_frame) ---------------
    virtual void set_viewport(Rect rect) = 0;
    virtual void set_scissor(Rect rect) = 0;
    virtual void bind_pipeline(PipelineHandle pipeline) = 0;
    virtual void bind_vertex_buffer(std::uint32_t stream, BufferHandle buffer, std::size_t offset = 0) = 0;
    virtual void bind_index_buffer(BufferHandle buffer, IndexFormat format) = 0;
    // `size` 0 = the rest of the buffer.
    virtual void bind_uniform_buffer(std::uint32_t binding, BufferHandle buffer, std::size_t offset = 0,
                                     std::size_t size = 0) = 0;
    // Binds a texture to sampler binding `binding` (< max_texture_bindings) of set 1.
    virtual void bind_texture(std::uint32_t binding, TextureHandle texture) = 0;

    // `first_instance` offsets per-instance streams (emulated on GL, which has no
    // base-instance draws). There is deliberately no base vertex: GLES 3.0 lacks it.
    virtual void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1, std::uint32_t first_vertex = 0,
                      std::uint32_t first_instance = 0) = 0;
    virtual void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count = 1,
                              std::uint32_t first_index = 0, std::uint32_t first_instance = 0) = 0;
    // Tier::Full (Feature::BaseVertexDraws): draw_indexed with `base_vertex`
    // added to every index, so meshes packed into one vertex buffer keep
    // their own 0-based indices. Where unsupported (GL, or a device gated to
    // Compatible) it logs an error once and draws nothing.
    virtual void draw_indexed_base_vertex(std::uint32_t index_count, std::uint32_t instance_count,
                                          std::uint32_t first_index, std::int32_t base_vertex,
                                          std::uint32_t first_instance = 0) = 0;

    // Debug aid: read back the framebuffer of the current frame right before it
    // is presented and write it as a binary PPM (P6). Call between begin_frame
    // and end_frame. Returns false if unsupported; errors are logged.
    virtual bool capture_frame(std::string path) = 0;
};

// Creates a device for the window. The window must have been created for the
// matching GraphicsApi. Returns null (and logs why) on failure.
std::unique_ptr<Device> create_device(Backend backend, Window& window, const DeviceOptions& options = {});

// An SDL window that create_window_surface can present
// to on `backend` (OpenGL / Vulkan flag set, same GL attributes as the main
// window). The caller owns it and destroys it after its surface. Null on failure.
SDL_Window* create_surface_window(Backend backend, const char* title, int width, int height, bool resizable = true);

// Whether this build includes the backend (Vulkan is off for web builds;
// WebGPU only with CUERVO_WEBGPU).
bool backend_compiled_in(Backend backend);
std::string_view backend_name(Backend backend);
// The --backend spelling: gl/opengl, vulkan/vk, webgpu/wgpu.
bool parse_backend(std::string_view text, Backend& out);
// What the window must be created for to host a device of `backend`.
GraphicsApi graphics_api(Backend backend);
// The backend used when none is asked for (DESIGN.md 4.4 "Clarified (Milestone
// 10.3)"): Vulkan when built in; in the browser WebGPU when the page handed
// over a device (web/webgpu_init.js), else GL (WebGL2). Players fall back to
// GL when the default fails to open.
Backend default_backend();

} // namespace cuervo::rhi

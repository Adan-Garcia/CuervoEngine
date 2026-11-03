#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "cuervo/math/vec.hpp"
#include "cuervo/rhi/rhi.hpp"

// Multi-pass post-processing on render targets (DESIGN.md 4.19, 11.2b): a bloom
// pyramid, LUT grading, a CRT look and a vignette, plus custom full-screen
// passes. The game draws the frame into a render target; apply() then runs the
// enabled effects into the window (or another target).
//
// Every pass is one triangle covering the destination with the shared vertex
// shader (post.vert). Passes that read a render target flip the texture
// coordinate when the backend stores target rows bottom-up
// (DeviceInfo::render_target_flip_v), so effects look the same everywhere.
namespace cuervo::render {

// std140 block `PostParams` shared by post.vert and every pass shader (64 bytes):
// the meaning of v0..v2 is the pass's own; v3.w is set by PostFx (flip).
struct PostParams {
    float v0[4]{};
    float v1[4]{};
    float v2[4]{};
    float v3[4]{};
};
static_assert(sizeof(PostParams) == 64);

struct PostSettings {
    // Bloom: the bright part of the scene (above `bloom_threshold`, softened
    // over `bloom_knee`) is blurred through a pyramid of `bloom_levels` half-size
    // targets and added back scaled by `bloom_intensity`.
    bool bloom = false;
    float bloom_threshold = 0.6f;
    float bloom_knee = 0.3f;
    float bloom_intensity = 0.8f;
    int bloom_levels = 5;

    // Vignette: darkens towards the corners; 0 = off. `vignette_radius` (0..1,
    // 0 = center) is where the falloff starts.
    float vignette = 0.0f;
    float vignette_radius = 0.55f;

    // Color grading with a lookup table: a `lut_size`^3 cube stored as a
    // (lut_size^2 x lut_size) RGBA8 strip (red along x, green along y, blue
    // slices side by side). `lut_strength` mixes graded and original color.
    // Create it with make_identity_lut() or from an authored image.
    rhi::TextureHandle lut{};
    int lut_size = 16;
    float lut_strength = 1.0f;

    // CRT: barrel distortion, scanlines and an aperture mask.
    bool crt = false;
    float crt_scanlines = 0.35f;
    float crt_curvature = 0.08f;
    float crt_mask = 0.25f;

    // A user shader pass (user_shader.hpp) run last, over the result of the
    // effects above, into the destination. `custom_params` carries its exports.
    rhi::PipelineHandle custom{};
    PostParams custom_params{};

    bool any() const { return bloom || vignette > 0.0f || lut || crt || custom; }
};

// sRGB-authored color to linear (rgb only), for clear colors and CPU-side shading.
rhi::Color srgb_to_linear(rhi::Color color);

// RGBA8 bytes of the identity grading cube (size^3, strip layout above).
std::vector<std::uint8_t> make_identity_lut(int size);


class PostFx {
public:
    PostFx() = default;
    ~PostFx();
    PostFx(const PostFx&) = delete;
    PostFx& operator=(const PostFx&) = delete;

    bool init(rhi::Device& device);
    void shutdown();

    // Runs the enabled effects over `source` (a render target the frame was
    // drawn into, `source_size` pixels) into `destination` ({} = the window).
    // With nothing enabled it is a plain copy. Call between begin_frame and
    // end_frame; the current render target afterwards is `destination`.
    void apply(rhi::RenderTargetHandle source, ivec2 source_size, rhi::RenderTargetHandle destination,
               const PostSettings& settings);

    // Custom full-screen passes (user shaders, DESIGN.md 4.19). Pipelines made
    // here take the shared vertex shader's varyings: location 0 `v_uv`, 1
    // `v_screen` (post.vert); the fragment shader's uniform block 0 is a
    // PostParams. `vertex()` is that shader, for pipelines built elsewhere.
    const rhi::ShaderSource& vertex() const;
    rhi::PipelineHandle create_pass_pipeline(const rhi::ShaderSource& fragment, rhi::BlendMode blend,
                                             std::string_view debug_name);
    // Draws one pass into the current render target: `textures` bind to samplers
    // 0.., `params` is the uniform block. `flip` samples render targets with v
    // flipped where needed (true when any texture is a render target's).
    void fullscreen(rhi::PipelineHandle pipeline, std::span<const rhi::TextureHandle> textures, PostParams params,
                    bool flip_targets);

    bool ready() const { return device_ != nullptr; }
    // Passes issued since init (tests, profiling).
    std::size_t passes() const { return passes_; }

private:
    void ensure_pyramid(ivec2 source_size, int levels);
    void destroy_pyramid();

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle down_, up_, composite_;
    rhi::BufferHandle triangle_, params_;
    std::size_t slot_ = 0;
    std::size_t slot_size_ = 256;
    std::size_t passes_ = 0;
    std::vector<rhi::RenderTargetHandle> pyramid_;
    std::vector<ivec2> pyramid_sizes_;
    rhi::RenderTargetHandle mid_; // the effects' result while a custom pass follows
    ivec2 mid_size_{0, 0};
    ivec2 pyramid_source_{0, 0};
};

} // namespace cuervo::render

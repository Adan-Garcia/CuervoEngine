#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cuervo/math/affine2.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/rhi/rhi.hpp"

// 2D SDF lighting and sphere-traced (raymarched) shadow batcher
// (DESIGN.md 4.17, milestone 9.7).
//
// Evaluates analytic 2D signed distance functions for circle, rounded-box,
// capsule and polygon-edge occluders, and raymarches soft or hard shadows per
// fragment on both Vulkan and OpenGL / OpenGL ES / WebGL2 in parity. Also
// provides deterministic CPU-side SDF and shadow evaluation (via dmath) so
// gameplay scripts can query illumination and line-of-sight shadows without
// depending on the GPU.
namespace cuervo::render {

inline constexpr std::size_t max_gpu_occluders = 128;
inline constexpr std::size_t max_gpu_lights = 32;
// Occluder indices across all lights' culled lists (LightSceneUniforms::occ_lists).
inline constexpr std::size_t max_gpu_occluder_refs = 512;

enum class LightOccluderKind : std::uint32_t {
    Circle = 0,
    RoundedRect = 1,
    Capsule = 2,
};

enum class LightKind : std::uint32_t {
    Point = 0,
    Directional = 1,
    AmbientComposite = 2,
};

enum class LightBlend : std::uint32_t {
    Add = 0,
    Mix = 1,
    Subtract = 2,
};

struct SdfOccluder {
    LightOccluderKind kind = LightOccluderKind::RoundedRect;
    vec2 center{0.0f, 0.0f};
    vec2 half_size{16.0f, 16.0f}; // Circle: (r, 0); RoundedRect: (hx, hy); Capsule: (half_length, r)
    float rotation = 0.0f;
    float corner_radius = 0.0f;
    std::uint32_t mask = 1u;
    // Normal mapping: the face bends outward over this many units from the
    // edge (0 = flat). Whole units on the GPU.
    float bevel = 8.0f;
};

struct LightDef {
    LightKind kind = LightKind::Point;
    LightBlend blend = LightBlend::Add;
    vec2 position{0.0f, 0.0f};
    float rotation = 0.0f;
    float radius = 200.0f;
    float source_radius = 4.0f;
    float falloff = 1.0f;
    float spot_angle_rad = 6.2831853f; // full cone angle in radians (>= 2*pi = omnidirectional)
    float spot_feather_rad = 0.2617994f;
    float energy = 1.0f;
    rhi::Color color{1.0f, 0.95f, 0.8f, 1.0f};
    bool shadow_enabled = true;
    // The size of the light's glowing source: shadows are sharp at their
    // occluder and this wide at the light (0 = hard).
    float shadow_softness = 16.0f;
    // Height above the plane, for the normal mapping of occluder faces.
    float height = 48.0f;
    rhi::Color shadow_color{0.0f, 0.0f, 0.0f, 0.65f};
    std::uint32_t shadow_mask = 1u;
};

struct AmbientLightDef {
    bool enabled = false;
    rhi::Color color{0.05f, 0.06f, 0.12f, 1.0f};
    float darkness = 0.8f;
    float light_glow = 0.35f;
};

// GPU instance layout; must match cuervo/render/shaders/light.vert (stream 1).
struct LightQuadInstance {
    vec2 center{0.0f, 0.0f};
    vec2 half_size{0.0f, 0.0f};
    vec4 params0{1.0f, 0.0f, 0.0f, 0.0f}; // cos_dir, sin_dir, radius, source_radius
    vec4 params1{-1.0f, -1.0f, 0.0f, 1.0f}; // cos_outer, cos_inner, shadow_softness, falloff
    float energy = 1.0f;
    std::uint32_t flags = 0;
    std::uint32_t mask = 1u;
    std::uint32_t color = 0;
    std::uint32_t shadow_color = 0;
};
static_assert(sizeof(LightQuadInstance) == 68, "LightQuadInstance must match the light.vert instance stream");

struct LightViewUniforms {
    vec4 row0{1.0f, 0.0f, 0.0f, 0.0f};
    vec4 row1{0.0f, 1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(LightViewUniforms) == 32);

struct LightSceneUniforms {
    std::uint32_t counts[4] = {0, 0, 80, 0}; // occluder_count, light_count, max_steps, reserved
    vec4 ambient_color{0.0f, 0.0f, 0.0f, 0.0f};
    vec4 ambient_params{0.35f, 1.0f, 0.0f, 0.0f};
    vec4 occ_geom[max_gpu_occluders]{};
    vec4 occ_meta[max_gpu_occluders]{};
    std::uint32_t occ_masks[max_gpu_occluders]{};
    vec4 light_pos_rad[max_gpu_lights]{};
    vec4 light_color[max_gpu_lights]{};
    vec4 light_shadow[max_gpu_lights]{};
    vec4 light_params0[max_gpu_lights]{};
    vec4 light_params1[max_gpu_lights]{};
    std::uint32_t light_flags[max_gpu_lights]{};
    std::uint32_t light_masks[max_gpu_lights]{};
    std::uint32_t occ_lists[max_gpu_occluder_refs]{};
    vec4 light_params2[max_gpu_lights]{}; // x = height
};
static_assert(sizeof(LightSceneUniforms) == 10032, "LightSceneUniforms must match std140 LightScene in light.frag");

// Deterministic CPU evaluation helpers (using dmath).
float eval_occluder_sdf(const SdfOccluder& occ, vec2 world_p);
float eval_scene_sdf(std::span<const SdfOccluder> occluders, vec2 world_p, std::uint32_t mask = 0xFFFFFFFFu);
float eval_raymarch_shadow(std::span<const SdfOccluder> occluders, vec2 p, vec2 ray_dir, float max_dist,
                           float source_radius, float softness, std::uint32_t mask = 0xFFFFFFFFu,
                           std::uint32_t max_steps = 80);
// Normal mapping (light.frag surface_factor): how much more or less light the
// face of an occluder at `world_p` catches from `to_light` (x, y in the plane,
// z the light's height) than a flat surface; 1 outside occluders.
float eval_surface_factor(std::span<const SdfOccluder> occluders, vec2 world_p, vec3 to_light,
                          std::uint32_t mask = 0xFFFFFFFFu);
// Returns (attenuation including the surface factor, shadow_visibility) for `light` at `world_p`.
vec2 eval_light_at(const LightDef& light, std::span<const SdfOccluder> occluders, vec2 world_p);

class LightBatcher {
public:
    LightBatcher() = default;
    ~LightBatcher();

    LightBatcher(const LightBatcher&) = delete;
    LightBatcher& operator=(const LightBatcher&) = delete;

    bool init(rhi::Device& device, std::size_t initial_capacity = 64);
    void shutdown();

    void clear();

    // Occluder submission
    void add_occluder(const SdfOccluder& occ);
    // `bevel`: SdfOccluder::bevel (normal mapping).
    void add_occluder_circle(vec2 center, float radius, std::uint32_t mask = 1u, float bevel = 8.0f);
    void add_occluder_box(vec2 center, vec2 half_size, float rotation = 0.0f, float corner_radius = 0.0f,
                          std::uint32_t mask = 1u, float bevel = 8.0f);
    void add_occluder_capsule(vec2 center, float half_length, float radius, float rotation = 0.0f,
                              std::uint32_t mask = 1u, float bevel = 8.0f);
    void add_occluder_segment(vec2 a, vec2 b, float thickness = 4.0f, std::uint32_t mask = 1u, float bevel = 8.0f);
    void add_occluder_polygon(std::span<const vec2> points, bool closed, float thickness = 4.0f,
                              std::uint32_t mask = 1u, float bevel = 8.0f);

    // Persistent baked occluders (e.g. from TileMapLayer) that survive per-frame clear() until clear_baked_occluders()
    void add_baked_occluder(const SdfOccluder& occ);
    void clear_baked_occluders();
    std::size_t baked_occluder_count() const { return baked_occluders_.size(); }

    // Light submission
    void add_light(const LightDef& light);
    void set_ambient(const AmbientLightDef& ambient);

    bool has_lights() const { return !lights_.empty() || ambient_.enabled; }
    std::size_t light_count() const { return lights_.size(); }
    std::size_t occluder_count() const { return baked_occluders_.size() + frame_occluders_.size(); }

    std::span<const SdfOccluder> all_occluders() const;
    std::span<const LightDef> lights() const { return lights_; }
    const AmbientLightDef& ambient() const { return ambient_; }

    // Deterministic CPU queries over the currently active occluders and lights.
    float scene_sdf(vec2 world_p, std::uint32_t mask = 0xFFFFFFFFu) const;
    float raymarch(vec2 from, vec2 to, float softness = 0.0f, std::uint32_t mask = 0xFFFFFFFFu) const;
    float sample_light(vec2 world_p, std::uint32_t mask = 0xFFFFFFFFu) const;
    rhi::Color sample_light_color(vec2 world_p, std::uint32_t mask = 0xFFFFFFFFu) const;

    // Uploads active occluders & lights and draws lighting quads over the world view.
    // Each light marches only the occluders that can reach it; past
    // max_gpu_lights lights or max_gpu_occluders occluders the rest are
    // dropped (logged once).
    // `world_to_ndc` maps world coordinates to NDC; `world_bounds_center` and
    // `world_bounds_half` enclose the visible viewport in world space (used for
    // full-screen ambient and directional light quads).
    void flush(const Affine2& world_to_ndc, vec2 world_bounds_center, vec2 world_bounds_half,
               float world_units_per_pixel = 1.0f);

private:
    bool ensure_capacity(std::size_t count);
    void rebuild_combined_occluders() const;

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle quad_vertices_;
    rhi::BufferHandle quad_indices_;
    rhi::BufferHandle instance_buffer_;
    rhi::BufferHandle view_uniforms_;
    rhi::BufferHandle scene_uniforms_;
    std::size_t capacity_ = 0;

    std::vector<SdfOccluder> baked_occluders_;
    std::vector<SdfOccluder> frame_occluders_;
    mutable std::vector<SdfOccluder> combined_occluders_;
    mutable bool combined_dirty_ = true;

    std::vector<LightDef> lights_;
    AmbientLightDef ambient_;
    std::vector<LightQuadInstance> instances_;
    std::size_t flushed_ = 0;
    bool warned_limits_ = false;
};

} // namespace cuervo::render

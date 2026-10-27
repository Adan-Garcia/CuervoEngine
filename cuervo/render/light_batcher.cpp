#include "cuervo/render/light_batcher.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

#include "cuervo/core/log.hpp"
#include "cuervo/math/dmath.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "shaders/light_frag.hpp"
#include "shaders/light_vert.hpp"

namespace cuervo::render {

namespace {

static_assert(offsetof(LightQuadInstance, center) == 0);
static_assert(offsetof(LightQuadInstance, half_size) == 8);
static_assert(offsetof(LightQuadInstance, params0) == 16);
static_assert(offsetof(LightQuadInstance, params1) == 32);
static_assert(offsetof(LightQuadInstance, energy) == 48);
static_assert(offsetof(LightQuadInstance, flags) == 52);
static_assert(offsetof(LightQuadInstance, mask) == 56);
static_assert(offsetof(LightQuadInstance, color) == 60);
static_assert(offsetof(LightQuadInstance, shadow_color) == 64);

constexpr std::array<float, 8> quad_corners{-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
constexpr std::array<std::uint16_t, 6> quad_index_data{0, 1, 2, 0, 2, 3};

constexpr std::array<rhi::VertexAttribute, 1> corner_attributes{{{0, rhi::VertexFormat::Float2, 0}}};
constexpr std::array<rhi::VertexAttribute, 9> instance_attributes{{
    {1, rhi::VertexFormat::Float2, offsetof(LightQuadInstance, center)},
    {2, rhi::VertexFormat::Float2, offsetof(LightQuadInstance, half_size)},
    {3, rhi::VertexFormat::Float4, offsetof(LightQuadInstance, params0)},
    {4, rhi::VertexFormat::Float4, offsetof(LightQuadInstance, params1)},
    {5, rhi::VertexFormat::Float1, offsetof(LightQuadInstance, energy)},
    {6, rhi::VertexFormat::UInt1, offsetof(LightQuadInstance, flags)},
    {7, rhi::VertexFormat::UInt1, offsetof(LightQuadInstance, mask)},
    {8, rhi::VertexFormat::UByte4Norm, offsetof(LightQuadInstance, color)},
    {9, rhi::VertexFormat::UByte4Norm, offsetof(LightQuadInstance, shadow_color)},
}};

std::uint32_t pack_light_flags(const LightDef& l) {
    const std::uint32_t sh = l.shadow_enabled ? 1u : 0u;
    const std::uint32_t k = (static_cast<std::uint32_t>(l.kind) & 3u) << 1u;
    const std::uint32_t b = (static_cast<std::uint32_t>(l.blend) & 3u) << 3u;
    return sh | k | b;
}

void compute_spot_cos(const LightDef& l, float& cos_outer, float& cos_inner) {
    const float half_angle = std::clamp(l.spot_angle_rad * 0.5f, 0.0f, dmath::pi);
    if (half_angle >= dmath::pi - 1e-4f) {
        cos_outer = -2.0f;
        cos_inner = -2.0f;
        return;
    }
    const float feather = std::clamp(l.spot_feather_rad, 1e-3f, half_angle);
    const float inner_angle = std::max(half_angle - feather, 0.0f);
    cos_outer = dmath::cos(half_angle);
    cos_inner = std::max(dmath::cos(inner_angle), cos_outer + 1e-4f);
}

LightQuadInstance make_light_instance(const LightDef& l, vec2 bounds_center, vec2 bounds_half) {
    LightQuadInstance inst;
    const float c = dmath::cos(l.rotation);
    const float s = dmath::sin(l.rotation);
    float cos_outer = -2.0f;
    float cos_inner = -2.0f;
    compute_spot_cos(l, cos_outer, cos_inner);

    const float r = std::max(l.radius, 0.0f);
    if (l.kind == LightKind::Directional) {
        inst.center = bounds_center;
        inst.half_size = bounds_half;
    } else {
        inst.center = l.position;
        inst.half_size = vec2{r, r};
    }
    inst.params0 = vec4{c, s, r, std::max(l.source_radius, 0.0f)};
    inst.params1 = vec4{cos_outer, cos_inner, std::max(l.shadow_softness, 0.0f), std::max(l.falloff, 0.0f)};
    inst.energy = std::max(l.energy, 0.0f);
    inst.flags = pack_light_flags(l);
    inst.mask = l.shadow_mask;
    inst.color = pack_rgba8(l.color);
    inst.shadow_color = pack_rgba8(l.shadow_color);
    return inst;
}

} // namespace

float eval_occluder_sdf(const SdfOccluder& occ, vec2 world_p) {
    const vec2 p = world_p - occ.center;
    if (occ.kind == LightOccluderKind::Circle) {
        return dmath::sqrt(dot(p, p)) - std::max(occ.half_size.x(), 0.0f);
    }
    const float c = dmath::cos(occ.rotation);
    const float s = dmath::sin(occ.rotation);
    vec2 local{c * p.x() + s * p.y(), -s * p.x() + c * p.y()};
    if (occ.kind == LightOccluderKind::RoundedRect) {
        const vec2 h = max(occ.half_size, vec2{0.0f, 0.0f});
        const float r = std::clamp(occ.corner_radius, 0.0f, std::min(h.x(), h.y()));
        const vec2 q{dmath::abs(local.x()) - h.x() + r, dmath::abs(local.y()) - h.y() + r};
        const vec2 q_pos{std::max(q.x(), 0.0f), std::max(q.y(), 0.0f)};
        const float outside = dmath::sqrt(dot(q_pos, q_pos));
        const float inside = std::min(std::max(q.x(), q.y()), 0.0f);
        return outside + inside - r;
    }
    // Capsule
    const float hl = std::max(occ.half_size.x(), 0.0f);
    const float r = std::max(occ.half_size.y(), 0.0f);
    local.x() = std::max(dmath::abs(local.x()) - hl, 0.0f);
    return dmath::sqrt(dot(local, local)) - r;
}

float eval_scene_sdf(std::span<const SdfOccluder> occluders, vec2 world_p, std::uint32_t mask) {
    float min_d = 1e6f;
    for (const SdfOccluder& occ : occluders) {
        if ((occ.mask & mask) == 0u) {
            continue;
        }
        min_d = std::min(min_d, eval_occluder_sdf(occ, world_p));
    }
    return min_d;
}

float eval_raymarch_shadow(std::span<const SdfOccluder> occluders, vec2 p, vec2 ray_dir, float max_dist,
                           float source_radius, float softness, std::uint32_t mask, std::uint32_t max_steps) {
    if (occluders.empty() || max_dist <= 0.5f) {
        return 1.0f;
    }
    const float limit = std::max(max_dist - std::max(source_radius, 0.0f), 0.0f);
    if (limit <= 0.5f) {
        return 1.0f;
    }
    // Mirrors raymarch_shadow in light.frag: escape the occluder holding `p`
    // (its face is lit), count penumbra only while approaching a surface, and
    // center it on the hard shadow's edge, half a round light `softness`
    // across to either side.
    const float d0 = eval_scene_sdf(occluders, p, mask);
    float t = std::max(d0, 0.5f);
    float h_prev = 1e6f;
    if (d0 < 0.25f) { // on the surface too: the march would stop on it
        t = std::max(-d0, 1.0f);
        float h = eval_scene_sdf(occluders, p + ray_dir * t, mask);
        for (std::uint32_t k = 0; k < 32u && h < 0.5f && t < limit; ++k) {
            t += std::max(-h, 1.0f);
            h = eval_scene_sdf(occluders, p + ray_dir * t, mask);
        }
        if (h < 0.5f) {
            return 1.0f;
        }
        h_prev = h;
    }
    const bool soft = softness > 0.001f;
    const float half_width = 0.5f * softness;
    float res = 1.0f; // -1 = the source hidden, 1 = all of it in view
    const float max_step = std::max(limit / 48.0f, 2.0f);

    for (std::uint32_t step = 0; step < max_steps; ++step) {
        if (t >= limit) {
            break;
        }
        const float h = eval_scene_sdf(occluders, p + ray_dir * t, mask);
        if (!soft) {
            if (h < 0.25f) {
                return 0.0f;
            }
        } else if (h < h_prev) {
            float y = 0.0f;
            float d = h;
            if (h > 0.0f && h_prev < 1e5f) {
                y = h * h / (2.0f * h_prev);
                d = dmath::sqrt(std::max(h * h - y * y, 0.0f));
            }
            const float w = half_width * std::max(t - y, 1.0f) / max_dist;
            res = std::min(res, d / std::max(w, 1e-3f));
            if (res <= -1.0f) {
                return 0.0f;
            }
        }
        h_prev = h;
        t += std::min(std::max(h, 0.5f + 0.02f * t), max_step);
    }
    if (!soft) {
        return 1.0f;
    }
    res = std::max(res, -1.0f);
    return 0.25f * (1.0f + res) * (1.0f + res) * (2.0f - res);
}

float eval_surface_factor(std::span<const SdfOccluder> occluders, vec2 world_p, vec3 to_light, std::uint32_t mask) {
    float d = 1e6f;
    float bevel = 0.0f;
    for (const SdfOccluder& occ : occluders) {
        if ((occ.mask & mask) == 0u) continue;
        const float od = eval_occluder_sdf(occ, world_p);
        if (od < d) {
            d = od;
            bevel = dmath::floor(std::max(occ.bevel, 0.0f) + 0.5f); // whole units, as uploaded
        }
    }
    // On the surface (within the march's 0.25 contact) is on the face, as in
    // eval_raymarch_shadow.
    if (d >= 0.25f || bevel <= 0.0f) return 1.0f;
    const auto sdf = [&](vec2 q) { return eval_scene_sdf(occluders, q, mask); };
    const float gx = sdf(world_p + vec2{1.0f, 0.0f}) - sdf(world_p - vec2{1.0f, 0.0f});
    const float gy = sdf(world_p + vec2{0.0f, 1.0f}) - sdf(world_p - vec2{0.0f, 1.0f});
    const float len = dmath::sqrt(gx * gx + gy * gy);
    if (len < 1e-5f) return 1.0f;
    // Mirrors light.frag: a seam between touching occluders is no edge; past
    // one, the union's nearest edge point comes from walks all around, each
    // dropped onto the edge from its first sample outside.
    const auto exit_depth = [&](vec2 dir, float depth) {
        for (std::uint32_t k = 0; k < 8u && depth < bevel; ++k) {
            const float sd = sdf(world_p + dir * (depth + 0.5f));
            if (sd >= 0.0f) break;
            depth += std::max(-sd, 0.5f) + 0.5f;
        }
        return depth;
    };
    // Edge point minus world_p, and the edge's outward normal.
    struct EdgeHit {
        vec2 v{1e6f, 1e6f};
        vec2 n{1.0f, 0.0f};
    };
    const auto edge_along = [&](vec2 dir, float depth) {
        for (std::uint32_t k = 0; k < 24u && depth < 2.0f * bevel; ++k) {
            const vec2 q = world_p + dir * (depth + 0.5f);
            const float sd = sdf(q);
            if (sd >= 0.0f) {
                const float qx = sdf(q + vec2{0.25f, 0.0f}) - sdf(q - vec2{0.25f, 0.0f});
                const float qy = sdf(q + vec2{0.0f, 0.25f}) - sdf(q - vec2{0.0f, 0.25f});
                const float ql = dmath::sqrt(qx * qx + qy * qy);
                const vec2 n = ql > 1e-5f ? vec2{qx / ql, qy / ql} : dir;
                return EdgeHit{q - n * sd - world_p, n};
            }
            depth += std::max(-sd, 0.5f) + 0.5f;
        }
        return EdgeHit{vec2{1e6f, 1e6f}, dir};
    };
    vec2 n2{gx / len, gy / len};
    const float start = std::max(-d, 0.0f);
    float depth = exit_depth(n2, start);
    if (depth > start + 0.75f) {
        std::array<EdgeHit, 17> cands;
        vec2 dir = n2;
        std::size_t best = 0;
        for (std::size_t k = 0; k < cands.size(); ++k) {
            cands[k] = edge_along(dir, start);
            if (length_sq(cands[k].v) < length_sq(cands[best].v)) best = k;
            dir = k == 0 ? vec2{1.0f, 0.0f}
                         : vec2{dir.x() * 0.92387953f - dir.y() * 0.38268343f,
                                dir.x() * 0.38268343f + dir.y() * 0.92387953f};
        }
        vec2 v = cands[best].v;
        // An inside corner: meet the nearest face with the nearest one turned from it.
        std::size_t other = best;
        for (std::size_t k = 0; k < cands.size(); ++k) {
            if (dot(cands[k].n, cands[best].n) < 0.87f &&
                (other == best || length_sq(cands[k].v) < length_sq(cands[other].v))) {
                other = k;
            }
        }
        if (other != best && length_sq(cands[other].v) < 1e10f) {
            const vec2 f1 = cands[best].n;
            const vec2 f2 = cands[other].n;
            const float det = f1.x() * f2.y() - f1.y() * f2.x();
            if (dmath::abs(det) > 0.1f) {
                const float b1 = dot(cands[best].v, f1);
                const float b2 = dot(cands[other].v, f2);
                const vec2 c{(b1 * f2.y() - b2 * f1.y()) / det, (f1.x() * b2 - f2.x() * b1) / det};
                const vec2 out = f1 + f2;
                if (length_sq(c) < length_sq(v) &&
                    sdf(world_p + c + out * (0.75f / dmath::sqrt(dot(out, out)))) > 0.25f) {
                    v = c;
                }
            }
        }
        depth = bevel;
        const float l = dmath::sqrt(dot(v, v));
        if (l < depth && l > 1e-4f) {
            depth = l;
            n2 = v / l;
        }
    }
    const float u = std::clamp(depth / bevel, 0.0f, 1.0f);
    const float tilt = 1.0f - u * u * (3.0f - 2.0f * u);
    vec3 n{n2.x() * 2.0f * tilt, n2.y() * 2.0f * tilt, 1.0f};
    n = n / dmath::sqrt(dot(n, n));
    const vec3 l = to_light / dmath::sqrt(dot(to_light, to_light));
    return std::min(std::max(dot(n, l), 0.0f) / std::max(l.z(), 0.1f), 2.5f);
}

vec2 eval_light_at(const LightDef& light, std::span<const SdfOccluder> occluders, vec2 world_p) {
    const float c = dmath::cos(light.rotation);
    const float s = dmath::sin(light.rotation);
    const vec2 spot_dir{c, s};

    if (light.kind == LightKind::Directional) {
        const float vis = light.shadow_enabled
                              ? eval_raymarch_shadow(occluders, world_p, spot_dir * -1.0f, std::max(light.radius, 0.0f),
                                                     light.source_radius, light.shadow_softness, light.shadow_mask)
                              : 1.0f;
        const float face = eval_surface_factor(occluders, world_p, vec3{-c, -s, 1.0f}, light.shadow_mask);
        return vec2{face, vis};
    }

    const vec2 to_light = light.position - world_p;
    const float dist2 = dot(to_light, to_light);
    const float r = std::max(light.radius, 0.0f);
    if (dist2 >= r * r || r <= 0.001f) {
        return vec2{0.0f, 1.0f};
    }
    const float dist = dmath::sqrt(dist2);
    const vec2 ray_dir = dist > 1e-4f ? (to_light / dist) : vec2{1.0f, 0.0f};

    float cos_outer = -2.0f;
    float cos_inner = -2.0f;
    compute_spot_cos(light, cos_outer, cos_inner);

    float spot_atten = 1.0f;
    if (cos_outer > -0.999f) {
        const float cos_angle = dot(ray_dir * -1.0f, spot_dir);
        if (cos_angle <= cos_outer) {
            return vec2{0.0f, 1.0f};
        }
        const float denom = std::max(cos_inner - cos_outer, 1e-4f);
        const float t_spot = std::clamp((cos_angle - cos_outer) / denom, 0.0f, 1.0f);
        spot_atten = t_spot * t_spot * (3.0f - 2.0f * t_spot);
    }

    const float u = std::clamp(dist / r, 0.0f, 1.0f);
    const float w = 1.0f - u * u;
    const float radial = (w * w) / (1.0f + std::max(light.falloff, 0.0f) * u * u);
    float atten = radial * spot_atten;
    if (atten <= 1e-4f) {
        return vec2{0.0f, 1.0f};
    }
    atten *= eval_surface_factor(occluders, world_p, vec3{to_light.x(), to_light.y(), light.height}, light.shadow_mask);

    const float vis = light.shadow_enabled
                          ? eval_raymarch_shadow(occluders, world_p, ray_dir, dist, light.source_radius,
                                                 light.shadow_softness, light.shadow_mask)
                          : 1.0f;
    return vec2{atten, vis};
}

LightBatcher::~LightBatcher() { shutdown(); }

bool LightBatcher::init(rhi::Device& device, std::size_t initial_capacity) {
    shutdown();
    device_ = &device;

    const std::array<rhi::VertexStream, 2> streams{{
        {sizeof(float) * 2, rhi::VertexRate::PerVertex, corner_attributes},
        {sizeof(LightQuadInstance), rhi::VertexRate::PerInstance, instance_attributes},
    }};
    rhi::PipelineDesc desc;
    desc.vertex = shaders::light_vert;
    desc.fragment = shaders::light_frag;
    desc.streams = streams;
    desc.blend = rhi::BlendMode::Premultiplied;
    desc.debug_name = "light_2d";
    pipeline_ = device.create_pipeline(desc);

    quad_vertices_ = device.create_buffer(
        {rhi::BufferKind::Vertex, rhi::BufferUsage::Static, sizeof(quad_corners), quad_corners.data()});
    quad_indices_ = device.create_buffer(
        {rhi::BufferKind::Index, rhi::BufferUsage::Static, sizeof(quad_index_data), quad_index_data.data()});
    view_uniforms_ =
        device.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, sizeof(LightViewUniforms), nullptr});
    scene_uniforms_ = device.create_buffer(
        {rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, sizeof(LightSceneUniforms), nullptr});

    if (!pipeline_ || !quad_vertices_ || !quad_indices_ || !view_uniforms_ || !scene_uniforms_ ||
        !ensure_capacity(initial_capacity)) {
        LOG_ERROR("LightBatcher: failed to create GPU resources");
        shutdown();
        return false;
    }
    return true;
}

void LightBatcher::shutdown() {
    if (!device_) {
        return;
    }
    device_->destroy_pipeline(pipeline_);
    device_->destroy_buffer(quad_vertices_);
    device_->destroy_buffer(quad_indices_);
    device_->destroy_buffer(instance_buffer_);
    device_->destroy_buffer(view_uniforms_);
    device_->destroy_buffer(scene_uniforms_);
    pipeline_ = {};
    quad_vertices_ = quad_indices_ = instance_buffer_ = view_uniforms_ = scene_uniforms_ = {};
    capacity_ = 0;
    device_ = nullptr;
}

bool LightBatcher::ensure_capacity(std::size_t count) {
    if (instance_buffer_ && count <= capacity_) {
        return true;
    }
    const std::size_t capacity = std::max({count, capacity_ + capacity_ / 4, std::size_t{64}});
    device_->destroy_buffer(instance_buffer_);
    instance_buffer_ = device_->create_buffer(
        {rhi::BufferKind::Vertex, rhi::BufferUsage::Stream, capacity * sizeof(LightQuadInstance), nullptr});
    capacity_ = instance_buffer_ ? capacity : 0;
    return static_cast<bool>(instance_buffer_);
}

void LightBatcher::clear() {
    frame_occluders_.clear();
    lights_.clear();
    ambient_ = AmbientLightDef{};
    instances_.clear();
    flushed_ = 0;
    combined_dirty_ = true;
}

void LightBatcher::add_occluder(const SdfOccluder& occ) {
    frame_occluders_.push_back(occ);
    combined_dirty_ = true;
}

void LightBatcher::add_occluder_circle(vec2 center, float radius, std::uint32_t mask, float bevel) {
    SdfOccluder occ;
    occ.kind = LightOccluderKind::Circle;
    occ.center = center;
    occ.half_size = vec2{std::max(radius, 0.0f), 0.0f};
    occ.mask = mask;
    occ.bevel = std::max(bevel, 0.0f);
    add_occluder(occ);
}

void LightBatcher::add_occluder_box(vec2 center, vec2 half_size, float rotation, float corner_radius,
                                    std::uint32_t mask, float bevel) {
    SdfOccluder occ;
    occ.kind = LightOccluderKind::RoundedRect;
    occ.center = center;
    occ.half_size = max(half_size, vec2{0.0f, 0.0f});
    occ.rotation = rotation;
    occ.corner_radius = std::clamp(corner_radius, 0.0f, std::min(occ.half_size.x(), occ.half_size.y()));
    occ.mask = mask;
    occ.bevel = std::max(bevel, 0.0f);
    add_occluder(occ);
}

void LightBatcher::add_occluder_capsule(vec2 center, float half_length, float radius, float rotation,
                                        std::uint32_t mask, float bevel) {
    SdfOccluder occ;
    occ.kind = LightOccluderKind::Capsule;
    occ.center = center;
    occ.half_size = vec2{std::max(half_length, 0.0f), std::max(radius, 0.0f)};
    occ.rotation = rotation;
    occ.mask = mask;
    occ.bevel = std::max(bevel, 0.0f);
    add_occluder(occ);
}

void LightBatcher::add_occluder_segment(vec2 a, vec2 b, float thickness, std::uint32_t mask, float bevel) {
    const vec2 d = b - a;
    const float len = dmath::sqrt(dot(d, d));
    const float angle = len > 1e-5f ? dmath::atan2(d.y(), d.x()) : 0.0f;
    add_occluder_capsule((a + b) * 0.5f, len * 0.5f, std::max(thickness * 0.5f, 0.5f), angle, mask, bevel);
}

void LightBatcher::add_occluder_polygon(std::span<const vec2> points, bool closed, float thickness,
                                        std::uint32_t mask, float bevel) {
    if (points.size() < 2) {
        return;
    }
    const std::size_t edge_count = closed ? points.size() : (points.size() - 1);
    for (std::size_t i = 0; i < edge_count; ++i) {
        const vec2 a = points[i];
        const vec2 b = points[(i + 1) % points.size()];
        add_occluder_segment(a, b, thickness, mask, bevel);
    }
}

void LightBatcher::add_baked_occluder(const SdfOccluder& occ) {
    baked_occluders_.push_back(occ);
    combined_dirty_ = true;
}

void LightBatcher::clear_baked_occluders() {
    baked_occluders_.clear();
    combined_dirty_ = true;
}

void LightBatcher::add_light(const LightDef& light) {
    lights_.push_back(light);
}

void LightBatcher::set_ambient(const AmbientLightDef& ambient) {
    ambient_ = ambient;
}

void LightBatcher::rebuild_combined_occluders() const {
    if (!combined_dirty_) {
        return;
    }
    combined_occluders_.clear();
    combined_occluders_.reserve(baked_occluders_.size() + frame_occluders_.size());
    combined_occluders_.insert(combined_occluders_.end(), baked_occluders_.begin(), baked_occluders_.end());
    combined_occluders_.insert(combined_occluders_.end(), frame_occluders_.begin(), frame_occluders_.end());
    combined_dirty_ = false;
}

std::span<const SdfOccluder> LightBatcher::all_occluders() const {
    rebuild_combined_occluders();
    return combined_occluders_;
}

float LightBatcher::scene_sdf(vec2 world_p, std::uint32_t mask) const {
    return eval_scene_sdf(all_occluders(), world_p, mask);
}

float LightBatcher::raymarch(vec2 from, vec2 to, float softness, std::uint32_t mask) const {
    const vec2 d = to - from;
    const float dist = dmath::sqrt(dot(d, d));
    if (dist <= 1e-4f) {
        return 1.0f;
    }
    return eval_raymarch_shadow(all_occluders(), from, d / dist, dist, 0.0f, softness, mask);
}

float LightBatcher::sample_light(vec2 world_p, std::uint32_t mask) const {
    const auto occs = all_occluders();
    float total = ambient_.enabled ? std::clamp(1.0f - ambient_.darkness, 0.0f, 1.0f) : 0.0f;
    for (const LightDef& l : lights_) {
        if ((l.shadow_mask & mask) == 0u) {
            continue;
        }
        const vec2 av = eval_light_at(l, occs, world_p);
        const float contrib = av.x() * av.y() * std::max(l.energy, 0.0f) * l.color.a;
        if (l.blend == LightBlend::Subtract) {
            total = std::max(0.0f, total - contrib);
        } else {
            total += contrib;
        }
    }
    return total;
}

rhi::Color LightBatcher::sample_light_color(vec2 world_p, std::uint32_t mask) const {
    const auto occs = all_occluders();
    float r = ambient_.enabled ? ambient_.color.r * (1.0f - std::clamp(ambient_.darkness, 0.0f, 1.0f)) : 0.0f;
    float g = ambient_.enabled ? ambient_.color.g * (1.0f - std::clamp(ambient_.darkness, 0.0f, 1.0f)) : 0.0f;
    float b = ambient_.enabled ? ambient_.color.b * (1.0f - std::clamp(ambient_.darkness, 0.0f, 1.0f)) : 0.0f;
    float a = 0.0f;
    for (const LightDef& l : lights_) {
        if ((l.shadow_mask & mask) == 0u) {
            continue;
        }
        const vec2 av = eval_light_at(l, occs, world_p);
        const float w = av.x() * av.y() * std::max(l.energy, 0.0f) * l.color.a;
        if (l.blend == LightBlend::Subtract) {
            r = std::max(0.0f, r - l.color.r * w);
            g = std::max(0.0f, g - l.color.g * w);
            b = std::max(0.0f, b - l.color.b * w);
        } else {
            r += l.color.r * w;
            g += l.color.g * w;
            b += l.color.b * w;
            a += w;
        }
    }
    return rhi::Color{r, g, b, std::clamp(a, 0.0f, 1.0f)};
}

void LightBatcher::flush(const Affine2& world_to_ndc, vec2 world_bounds_center, vec2 world_bounds_half,
                         float world_units_per_pixel) {
    if (!device_ || (!ambient_.enabled && lights_.empty())) {
        return;
    }

    const auto occs = all_occluders();
    const std::size_t n_lights = std::min(lights_.size(), max_gpu_lights);

    LightViewUniforms view_u;
    view_u.row0 = vec4{world_to_ndc.a, world_to_ndc.c, world_to_ndc.tx, 0.0f};
    view_u.row1 = vec4{world_to_ndc.b, world_to_ndc.d, world_to_ndc.ty, 0.0f};

    LightSceneUniforms scene_u{};
    scene_u.counts[2] = 80u;
    scene_u.ambient_color =
        vec4{ambient_.color.r, ambient_.color.g, ambient_.color.b, std::clamp(ambient_.darkness, 0.0f, 1.0f)};
    scene_u.ambient_params = vec4{std::max(ambient_.light_glow, 0.0f), world_units_per_pixel, 0.0f, 0.0f};

    // Per light, the occluders whose bounds come within its reach (plus the
    // penumbra, at most the source's size); directional lights see those in view.
    // Each occluder is uploaded once, the first time a light needs it.
    std::vector<std::int32_t> slot(occs.size(), -1);
    std::size_t n_occ = 0;
    std::size_t n_refs = 0;
    bool dropped = lights_.size() > max_gpu_lights;
    for (std::size_t i = 0; i < n_lights; ++i) {
        const LightDef& l = lights_[i];
        const auto first = static_cast<std::uint32_t>(n_refs);
        // Lights without shadows still need the occluders they shade (normal mapping).
        {
            const bool directional = l.kind == LightKind::Directional;
            // A penumbra is at most the light's source size wide.
            const float reach = std::max(l.radius, 0.0f) + std::max(l.shadow_softness, 0.0f);
            for (std::size_t o = 0; o < occs.size(); ++o) {
                const SdfOccluder& occ = occs[o];
                if ((occ.mask & l.shadow_mask) == 0u) continue;
                const float bound = occ.kind == LightOccluderKind::Circle  ? occ.half_size.x()
                                    : occ.kind == LightOccluderKind::Capsule ? occ.half_size.x() + occ.half_size.y()
                                                                             : length(occ.half_size);
                if (directional) {
                    const vec2 d = occ.center - world_bounds_center;
                    if (dmath::abs(d.x()) - world_bounds_half.x() > bound ||
                        dmath::abs(d.y()) - world_bounds_half.y() > bound) {
                        continue;
                    }
                } else {
                    const vec2 d = occ.center - l.position;
                    const float r = reach + bound;
                    if (dot(d, d) > r * r) continue;
                }
                if (slot[o] < 0) {
                    if (n_occ == max_gpu_occluders) {
                        dropped = true;
                        continue;
                    }
                    slot[o] = static_cast<std::int32_t>(n_occ);
                    scene_u.occ_geom[n_occ] = vec4{occ.center.x(), occ.center.y(), occ.half_size.x(), occ.half_size.y()};
                    scene_u.occ_meta[n_occ] = vec4{dmath::cos(occ.rotation), dmath::sin(occ.rotation),
                                                   occ.corner_radius,
                                                   static_cast<float>(occ.kind) +
                                                       4.0f * dmath::floor(std::max(occ.bevel, 0.0f) + 0.5f)};
                    scene_u.occ_masks[n_occ] = occ.mask;
                    ++n_occ;
                }
                if (n_refs == max_gpu_occluder_refs) {
                    dropped = true;
                    break;
                }
                scene_u.occ_lists[n_refs++] = static_cast<std::uint32_t>(slot[o]);
            }
        }
        float cos_outer = -2.0f;
        float cos_inner = -2.0f;
        compute_spot_cos(l, cos_outer, cos_inner);
        scene_u.light_pos_rad[i] =
            vec4{l.position.x(), l.position.y(), std::max(l.radius, 0.0f), std::max(l.source_radius, 0.0f)};
        scene_u.light_color[i] =
            vec4{l.color.r * l.color.a, l.color.g * l.color.a, l.color.b * l.color.a, std::max(l.energy, 0.0f)};
        scene_u.light_shadow[i] = vec4{l.shadow_color.r, l.shadow_color.g, l.shadow_color.b, l.shadow_color.a};
        scene_u.light_params0[i] = vec4{dmath::cos(l.rotation), dmath::sin(l.rotation), cos_outer, cos_inner};
        scene_u.light_params1[i] = vec4{std::max(l.falloff, 0.0f), std::max(l.shadow_softness, 0.0f),
                                        static_cast<float>(first), static_cast<float>(n_refs - first)};
        scene_u.light_params2[i] = vec4{l.height, 0.0f, 0.0f, 0.0f};
        scene_u.light_flags[i] = pack_light_flags(l);
        scene_u.light_masks[i] = l.shadow_mask;
    }
    scene_u.counts[0] = static_cast<std::uint32_t>(n_occ);
    scene_u.counts[1] = static_cast<std::uint32_t>(n_lights);
    if (dropped && !warned_limits_) {
        warned_limits_ = true;
        LOG_WARN("2D lighting: more than {} lights, {} occluders or {} occluder-light pairs in one view; the rest "
                 "are skipped",
                 max_gpu_lights, max_gpu_occluders, max_gpu_occluder_refs);
    }

    const std::size_t first = flushed_;
    if (ambient_.enabled) {
        LightQuadInstance amb_inst;
        amb_inst.center = world_bounds_center;
        amb_inst.half_size = world_bounds_half;
        amb_inst.flags = static_cast<std::uint32_t>(LightKind::AmbientComposite) << 1u;
        amb_inst.mask = 0xFFFFFFFFu;
        amb_inst.color = pack_rgba8(ambient_.color);
        instances_.push_back(amb_inst);
    } else {
        for (std::size_t i = 0; i < n_lights; ++i) {
            LightQuadInstance inst = make_light_instance(lights_[i], world_bounds_center, world_bounds_half);
            inst.flags |= static_cast<std::uint32_t>(i) << 8u; // light.frag reads its occluder list
            instances_.push_back(inst);
        }
    }

    flushed_ = instances_.size();
    if (first == instances_.size() || !ensure_capacity(instances_.size())) {
        return;
    }

    device_->update_buffer(view_uniforms_, 0, std::as_bytes(std::span(&view_u, 1)));
    device_->update_buffer(scene_uniforms_, 0, std::as_bytes(std::span(&scene_u, 1)));
    device_->update_buffer(instance_buffer_, first * sizeof(LightQuadInstance),
                           std::as_bytes(std::span(instances_).subspan(first)));

    device_->bind_pipeline(pipeline_);
    device_->bind_vertex_buffer(0, quad_vertices_);
    device_->bind_vertex_buffer(1, instance_buffer_);
    device_->bind_index_buffer(quad_indices_, rhi::IndexFormat::UInt16);
    device_->bind_uniform_buffer(0, view_uniforms_);
    device_->bind_uniform_buffer(1, scene_uniforms_);
    device_->draw_indexed(static_cast<std::uint32_t>(quad_index_data.size()),
                          static_cast<std::uint32_t>(instances_.size() - first), 0, static_cast<std::uint32_t>(first));
}

} // namespace cuervo::render

#pragma once

// Private helpers shared by the physics_*.cpp translation units (not part of the public API).

#include <algorithm>
#include <cmath>

#include "cuervo/math/dmath.hpp"
#include "cuervo/physics/physics_world.hpp"

namespace cuervo::physics::detail {

// Character-controller tuning, in world units (pixels; the world uses 32 units per meter).
inline constexpr float character_slop = 0.05f;           // gap left between a character and a surface
inline constexpr float character_max_step_height = 6.0f; // ledges/seams a character steps over
inline constexpr float floor_leave_speed = 5.0f;         // upward speed above which a character leaves the floor
inline constexpr float floor_probe_dist = 4.0f;          // downward probe/snap distance when airborne or resting
inline constexpr float floor_snap_dist_grounded = 6.0f;  // snap distance when already on the floor last tick
inline constexpr float floor_probe_max_dist = 24.0f;     // cap on the probe when the floor is dropping away
inline constexpr float default_character_mass = 50.0f;   // weight used when the body has no mass set
inline constexpr int solver_substeps = 4;                // Box2D soft-step substeps per tick


inline float dot(vec2 a, vec2 b) { return a.x() * b.x() + a.y() * b.y(); }

inline b2Vec2 to_b2(vec2 v) { return b2Vec2{v.x(), v.y()}; }
inline b2Vec2 to_b2(float x, float y) { return b2Vec2{x, y}; }
inline vec2 to_v2(b2Vec2 v) { return vec2{v.x, v.y}; }
inline bool is_finite(float v) { return std::isfinite(v); }
inline bool is_finite(vec2 v) { return std::isfinite(v.x()) && std::isfinite(v.y()); }

inline std::vector<b2Vec2> subsample_hull_points(const std::vector<b2Vec2>& pts) {
    if (pts.size() <= B2_MAX_POLYGON_VERTICES) return pts;
    std::vector<b2Vec2> out;
    out.reserve(B2_MAX_POLYGON_VERTICES);
    const std::size_t n = pts.size();
    for (int i = 0; i < B2_MAX_POLYGON_VERTICES; ++i) {
        const std::size_t idx = (static_cast<std::size_t>(i) * n) / B2_MAX_POLYGON_VERTICES;
        out.push_back(pts[idx]);
    }
    return out;
}

inline b2ShapeProxy make_proxy_for_shape(const ShapeConfig& config, b2Rot body_rot = b2MakeRot(0.0f)) {
    const bool has_body_rot = (body_rot.c != 1.0f || body_rot.s != 0.0f);
    const b2Vec2 off = has_body_rot ? b2RotateVector(body_rot, to_b2(config.offset)) : to_b2(config.offset);
    const b2Rot rot = has_body_rot ? b2MulRot(body_rot, b2MakeRot(config.rotation)) : b2MakeRot(config.rotation);

    switch (config.kind) {
    case ShapeKind::Circle: {
        b2Vec2 pt = off;
        return b2MakeProxy(&pt, 1, std::max(0.01f, config.radius));
    }
    case ShapeKind::Box: {
        const float hw = std::max(0.01f, config.size.x() * 0.5f);
        const float hh = std::max(0.01f, config.size.y() * 0.5f);
        const b2Vec2 pts[4] = {
            b2Add(off, b2RotateVector(rot, to_b2(-hw, -hh))),
            b2Add(off, b2RotateVector(rot, to_b2(hw, -hh))),
            b2Add(off, b2RotateVector(rot, to_b2(hw, hh))),
            b2Add(off, b2RotateVector(rot, to_b2(-hw, hh))),
        };
        return b2MakeProxy(pts, 4, 0.0f);
    }
    case ShapeKind::Capsule: {
        const float r = std::max(0.01f, config.radius);
        const float half_len = std::max(0.0f, (config.length - 2.0f * r) * 0.5f);
        if (half_len <= 0.05f) {
            b2Vec2 pt = off;
            return b2MakeProxy(&pt, 1, r);
        }
        const b2Vec2 pts[2] = {
            b2Add(off, b2RotateVector(rot, to_b2(0.0f, -half_len))),
            b2Add(off, b2RotateVector(rot, to_b2(0.0f, half_len))),
        };
        return b2MakeProxy(pts, 2, r);
    }
    case ShapeKind::Segment: {
        const bool has_local_tf = has_body_rot || config.offset.x() != 0.0f || config.offset.y() != 0.0f || config.rotation != 0.0f;
        b2Vec2 pts[2] = {
            has_local_tf ? b2Add(off, b2RotateVector(rot, to_b2(config.point_a))) : to_b2(config.point_a),
            has_local_tf ? b2Add(off, b2RotateVector(rot, to_b2(config.point_b))) : to_b2(config.point_b),
        };
        if (b2DistanceSquared(pts[0], pts[1]) < 1e-4f) {
            pts[1].x += 1.0f;
        }
        return b2MakeProxy(pts, 2, 0.0f);
    }
    case ShapeKind::Polygon: {
        std::vector<b2Vec2> pts;
        pts.reserve(config.points.size());
        for (const auto& pt : config.points) {
            pts.push_back(b2Add(off, b2RotateVector(rot, to_b2(pt))));
        }
        if (pts.size() >= 3) {
            const std::vector<b2Vec2> hull_input = subsample_hull_points(pts);
            const b2Hull hull = b2ComputeHull(hull_input.data(), static_cast<int>(hull_input.size()));
            if (hull.count >= 3) {
                return b2MakeProxy(hull.points, hull.count, 0.0f);
            }
        }
        break;
    }
    case ShapeKind::Chain: {
        std::vector<b2Vec2> pts;
        pts.reserve(config.points.size());
        for (const auto& pt : config.points) {
            pts.push_back(b2Add(off, b2RotateVector(rot, to_b2(pt))));
        }
        if (pts.size() >= 2) {
            const int count = std::min(static_cast<int>(pts.size()), static_cast<int>(B2_MAX_POLYGON_VERTICES));
            return b2MakeProxy(pts.data(), count, 0.0f);
        }
        break;
    }
    }
    b2Vec2 pt = to_b2(0.0f, 0.0f);
    return b2MakeProxy(&pt, 1, 16.0f);
}

struct RaycastQueryContext {
    b2WorldId world = b2_nullWorldId;
    b2ShapeId hit_shape = b2_nullShapeId;
    b2Vec2 hit_point = {0.0f, 0.0f};
    b2Vec2 hit_normal = {0.0f, 0.0f};
    float min_fraction = 1.0f;
    uint32_t mask = 1;
    uint32_t exclude_body = 0;
};

inline float raycast_callback(b2ShapeId shape_id, b2Pos point, b2Vec2 normal, float fraction, void* ctx_ptr) {
    auto* ctx = static_cast<RaycastQueryContext*>(ctx_ptr);
    if (b2Shape_IsSensor(shape_id)) return -1.0f;
    const b2Filter filter = b2Shape_GetFilter(shape_id);
    if ((filter.categoryBits & ctx->mask) == 0) return -1.0f;

    const b2BodyId body_id = b2Shape_GetBody(shape_id);
    const auto body_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(body_id)));
    if (ctx->exclude_body != 0 && body_handle == ctx->exclude_body) return -1.0f;

    if (fraction > ctx->min_fraction) return ctx->min_fraction;

    ctx->hit_shape = shape_id;
    ctx->hit_point = point;
    ctx->hit_normal = normal;
    ctx->min_fraction = fraction;
    return fraction;
}

inline bool compute_shape_penetration(b2ShapeId self_shape, b2Transform self_xf,
                               b2ShapeId other_shape, b2Transform other_xf,
                               b2Vec2& out_normal, float& out_depth,
                               bool allow_speculative = false) {
    if (!b2Shape_IsValid(self_shape) || !b2Shape_IsValid(other_shape)) return false;

    const b2ShapeType self_type = b2Shape_GetType(self_shape);
    const b2ShapeType other_type = b2Shape_GetType(other_shape);

    // xf_ab transforms local points in B (self) to local frame of A (other): normal points A -> B (other -> self).
    const b2Transform xf_ab = b2InvMulTransforms(other_xf, self_xf);
    // xf_ba transforms local points in A (other) to local frame of B (self): normal points B -> A (self -> other).
    const b2Transform xf_ba = b2InvMulTransforms(self_xf, other_xf);

    b2LocalManifold m = {};
    bool normal_is_ba = false;

    if (self_type == b2_polygonShape) {
        const b2Polygon poly_self = b2Shape_GetPolygon(self_shape);
        switch (other_type) {
        case b2_polygonShape: {
            const b2Polygon poly_other = b2Shape_GetPolygon(other_shape);
            m = b2CollidePolygons(&poly_other, &poly_self, xf_ab);
            break;
        }
        case b2_circleShape: {
            const b2Circle cir_other = b2Shape_GetCircle(other_shape);
            m = b2CollidePolygonAndCircle(&poly_self, &cir_other, xf_ba);
            normal_is_ba = true;
            break;
        }
        case b2_capsuleShape: {
            const b2Capsule cap_other = b2Shape_GetCapsule(other_shape);
            m = b2CollidePolygonAndCapsule(&poly_self, &cap_other, xf_ba);
            normal_is_ba = true;
            break;
        }
        case b2_segmentShape: {
            const b2Segment seg_other = b2Shape_GetSegment(other_shape);
            m = b2CollideSegmentAndPolygon(&seg_other, &poly_self, xf_ab);
            break;
        }
        case b2_chainSegmentShape: {
            const b2ChainSegment cseg_other = b2Shape_GetChainSegment(other_shape);
            b2SimplexCache cache = b2_emptySimplexCache;
            m = b2CollideChainSegmentAndPolygon(&cseg_other, &poly_self, xf_ab, &cache);
            break;
        }
        default:
            return false;
        }
    } else if (self_type == b2_circleShape) {
        const b2Circle cir_self = b2Shape_GetCircle(self_shape);
        switch (other_type) {
        case b2_circleShape: {
            const b2Circle cir_other = b2Shape_GetCircle(other_shape);
            m = b2CollideCircles(&cir_other, &cir_self, xf_ab);
            break;
        }
        case b2_capsuleShape: {
            const b2Capsule cap_other = b2Shape_GetCapsule(other_shape);
            m = b2CollideCapsuleAndCircle(&cap_other, &cir_self, xf_ab);
            break;
        }
        case b2_polygonShape: {
            const b2Polygon poly_other = b2Shape_GetPolygon(other_shape);
            m = b2CollidePolygonAndCircle(&poly_other, &cir_self, xf_ab);
            break;
        }
        case b2_segmentShape: {
            const b2Segment seg_other = b2Shape_GetSegment(other_shape);
            m = b2CollideSegmentAndCircle(&seg_other, &cir_self, xf_ab);
            break;
        }
        case b2_chainSegmentShape: {
            const b2ChainSegment cseg_other = b2Shape_GetChainSegment(other_shape);
            m = b2CollideChainSegmentAndCircle(&cseg_other, &cir_self, xf_ab);
            break;
        }
        default:
            return false;
        }
    } else if (self_type == b2_capsuleShape) {
        const b2Capsule cap_self = b2Shape_GetCapsule(self_shape);
        switch (other_type) {
        case b2_circleShape: {
            const b2Circle cir_other = b2Shape_GetCircle(other_shape);
            m = b2CollideCapsuleAndCircle(&cap_self, &cir_other, xf_ba);
            normal_is_ba = true;
            break;
        }
        case b2_capsuleShape: {
            const b2Capsule cap_other = b2Shape_GetCapsule(other_shape);
            m = b2CollideCapsules(&cap_other, &cap_self, xf_ab);
            break;
        }
        case b2_polygonShape: {
            const b2Polygon poly_other = b2Shape_GetPolygon(other_shape);
            m = b2CollidePolygonAndCapsule(&poly_other, &cap_self, xf_ab);
            break;
        }
        case b2_segmentShape: {
            const b2Segment seg_other = b2Shape_GetSegment(other_shape);
            m = b2CollideSegmentAndCapsule(&seg_other, &cap_self, xf_ab);
            break;
        }
        case b2_chainSegmentShape: {
            const b2ChainSegment cseg_other = b2Shape_GetChainSegment(other_shape);
            b2SimplexCache cache = b2_emptySimplexCache;
            m = b2CollideChainSegmentAndCapsule(&cseg_other, &cap_self, xf_ab, &cache);
            break;
        }
        default:
            return false;
        }
    } else {
        return false;
    }

    if (m.pointCount <= 0) return false;
    float min_sep = m.points[0].separation;
    for (int i = 1; i < m.pointCount; ++i) {
        if (m.points[i].separation < min_sep) {
            min_sep = m.points[i].separation;
        }
    }
    if (!allow_speculative && min_sep >= 0.0f) return false;

    const b2Vec2 world_norm = normal_is_ba ? b2RotateVector(self_xf.q, b2Vec2{-m.normal.x, -m.normal.y})
                                           : b2RotateVector(other_xf.q, m.normal);
    const float len = b2Length(world_norm);
    if (len < 1e-5f) return false;
    out_normal = b2MulSV(1.0f / len, world_norm);
    out_depth = std::max(0.0f, -min_sep);
    return true;
}

inline b2AABB get_shifted_shape_aabb(b2ShapeId shape_id, b2BodyId body_id, b2Vec2 pos) {
    if (b2Shape_IsValid(shape_id) && b2Body_IsValid(body_id)) {
        b2AABB aabb = b2Shape_GetBounds(shape_id);
        const b2Vec2 body_p = b2Body_GetPosition(body_id);
        const b2Vec2 d = b2Sub(pos, body_p);
        aabb.lowerBound = b2Add(aabb.lowerBound, d);
        aabb.upperBound = b2Add(aabb.upperBound, d);
        return aabb;
    }
    return b2AABB{b2Sub(pos, to_b2(16.0f, 16.0f)), b2Add(pos, to_b2(16.0f, 16.0f))};
}

struct ShapeCastContext {
    b2BodyId self_body = b2_nullBodyId;
    b2ShapeId self_shape = b2_nullShapeId;
    b2Transform self_xf = b2Transform_identity;
    bool has_self_xf = false;
    b2ShapeId hit_shape = b2_nullShapeId;
    b2Vec2 hit_normal = {0.0f, 0.0f};
    b2Pos hit_point = {0.0f, 0.0f};
    float min_fraction = 1.0f;
    uint32_t mask = 1;
    vec2 motion = {0.0f, 0.0f};
};

inline float shape_cast_callback(b2ShapeId shape_id, b2Pos point, b2Vec2 normal, float fraction, void* ctx_ptr) {
    auto* ctx = static_cast<ShapeCastContext*>(ctx_ptr);
    const b2BodyId body_id = b2Shape_GetBody(shape_id);
    if (B2_ID_EQUALS(body_id, ctx->self_body)) return -1.0f;
    if (b2Shape_IsSensor(shape_id)) return -1.0f;

    const b2Filter filter = b2Shape_GetFilter(shape_id);
    if ((filter.categoryBits & ctx->mask) == 0) return -1.0f;

    // If shapes start in overlap, Box2D supplies a zero normal.
    // Derive the contact normal from the manifold or minimal AABB separation axis.
    if (normal.x == 0.0f && normal.y == 0.0f && b2Body_IsValid(ctx->self_body)) {
        const b2Transform self_xf = ctx->has_self_xf ? ctx->self_xf : b2Body_GetTransform(ctx->self_body);
        const b2Vec2 self_p = self_xf.p;
        const b2ShapeType stype = b2Shape_GetType(shape_id);
        if (stype == b2_segmentShape || stype == b2_chainSegmentShape) {
            const b2Segment seg = (stype == b2_segmentShape) ? b2Shape_GetSegment(shape_id)
                                                             : b2Shape_GetChainSegment(shape_id).segment;
            const b2Vec2 a = b2Body_GetWorldPoint(body_id, seg.point1);
            const b2Vec2 b = b2Body_GetWorldPoint(body_id, seg.point2);
            const b2Vec2 ab = b2Sub(b, a);
            const float ab_len_sq = b2Dot(ab, ab);
            if (ab_len_sq > 1e-6f) {
                const float t = std::clamp(b2Dot(b2Sub(self_p, a), ab) / ab_len_sq, 0.0f, 1.0f);
                const b2Vec2 closest = b2Add(a, b2MulSV(t, ab));
                const b2Vec2 diff = b2Sub(self_p, closest);
                const float d_len = b2Length(diff);
                if (d_len > 1e-5f) {
                    normal = b2MulSV(1.0f / d_len, diff);
                } else {
                    const float inv_len = 1.0f / std::sqrt(ab_len_sq);
                    b2Vec2 n = {-ab.y * inv_len, ab.x * inv_len};
                    if (dot(to_v2(n), ctx->motion) > 0.0f) {
                        n = {-n.x, -n.y};
                    }
                    normal = n;
                }
            }
        }
        const b2AABB self_aabb = get_shifted_shape_aabb(ctx->self_shape, ctx->self_body, self_p);
        const b2AABB other_aabb = b2Shape_GetBounds(shape_id);
        const float pen_left = self_aabb.upperBound.x - other_aabb.lowerBound.x;
        const float pen_right = other_aabb.upperBound.x - self_aabb.lowerBound.x;
        const float pen_up = self_aabb.upperBound.y - other_aabb.lowerBound.y;
        const float pen_down = other_aabb.upperBound.y - self_aabb.lowerBound.y;
        const bool is_foot_seam = (pen_up >= -1.0f && pen_up <= 6.0f && self_aabb.lowerBound.y < other_aabb.lowerBound.y);

        if (normal.x == 0.0f && normal.y == 0.0f && b2Shape_IsValid(ctx->self_shape)) {
            b2Vec2 sep_norm = {0.0f, 0.0f};
            float sep_depth = 0.0f;
            if (compute_shape_penetration(ctx->self_shape, self_xf, shape_id, b2Body_GetTransform(body_id), sep_norm, sep_depth, true)) {
                if (is_foot_seam && std::abs(sep_norm.x) > std::abs(sep_norm.y)) {
                    const b2Vec2 rot_up = b2RotateVector(b2Body_GetRotation(body_id), to_b2(0.0f, -1.0f));
                    normal = (rot_up.y < -0.5f) ? rot_up : to_b2(0.0f, -1.0f);
                } else {
                    normal = sep_norm;
                }
            }
        }
        if (normal.x == 0.0f && normal.y == 0.0f) {
            const float min_x = std::min(std::abs(pen_left), std::abs(pen_right));
            const float min_y = std::min(std::abs(pen_up), std::abs(pen_down));

            if (min_y <= min_x || is_foot_seam) {
                normal = to_b2(0.0f, (is_foot_seam || std::abs(pen_up) <= std::abs(pen_down)) ? -1.0f : 1.0f);
            } else {
                normal = to_b2(std::abs(pen_left) <= std::abs(pen_right) ? -1.0f : 1.0f, 0.0f);
            }
        }
    }

    // If motion is moving away from or parallel to the contact normal, this surface does not impede motion.
    const vec2 norm_v = {normal.x, normal.y};
    if (dot(norm_v, ctx->motion) >= -1e-4f) {
        return -1.0f;
    }

    if (fraction > ctx->min_fraction) {
        return ctx->min_fraction;
    }

    ctx->hit_shape = shape_id;
    ctx->hit_normal = normal;
    ctx->hit_point = point;
    ctx->min_fraction = fraction;
    return fraction;
}


// Sweeps `proxy` from `pos` along `motion` against everything `filter` admits except the caster itself,
// returning the closest hit (min_fraction < 1 means blocked).
inline ShapeCastContext sweep_shape(b2WorldId world, const b2ShapeProxy& proxy, const b2QueryFilter& filter,
                                    b2BodyId self_body, b2ShapeId self_shape, b2Vec2 pos, b2Rot rot,
                                    uint32_t mask, b2Vec2 motion) {
    ShapeCastContext ctx;
    ctx.self_body = self_body;
    ctx.self_shape = self_shape;
    ctx.self_xf = {pos, rot};
    ctx.has_self_xf = true;
    ctx.mask = mask;
    ctx.motion = to_v2(motion);
    b2World_CastShape(world, pos, &proxy, motion, filter, shape_cast_callback, &ctx);
    return ctx;
}

} // namespace cuervo::physics::detail

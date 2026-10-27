#include "cuervo/physics/physics_world.hpp"

#include <algorithm>
#include <cmath>

#include "cuervo/core/hash.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/physics/physics_internal.hpp"
#include "pluma/vm.hpp"

namespace cuervo::physics {

using namespace detail;

RaycastHit PhysicsWorld::cast_ray(vec2 origin, vec2 translation, uint32_t mask, uint32_t exclude_body) {
    RaycastHit hit;
    if (!b2World_IsValid(world_id_) || !is_finite(origin) || !is_finite(translation)) return hit;
    if (translation.x() == 0.0f && translation.y() == 0.0f) return hit;

    RaycastQueryContext ctx;
    ctx.world = world_id_;
    ctx.mask = mask;
    ctx.exclude_body = exclude_body;

    // categoryBits is all-ones so a shape's own collision mask never hides it from a ray; the callback
    // filters on the shape's layer against `mask`.
    b2QueryFilter filter = b2DefaultQueryFilter();
    filter.categoryBits = UINT64_MAX;
    filter.maskBits = mask;

    b2World_CastRay(world_id_, to_b2(origin), to_b2(translation), filter, raycast_callback, &ctx);

    if (b2Shape_IsValid(ctx.hit_shape)) {
        hit.hit = true;
        hit.point = {ctx.hit_point.x, ctx.hit_point.y};
        hit.normal = {ctx.hit_normal.x, ctx.hit_normal.y};
        hit.fraction = ctx.min_fraction;
        hit.shape_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Shape_GetUserData(ctx.hit_shape)));
        const b2BodyId body_id = b2Shape_GetBody(ctx.hit_shape);
        hit.body_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(body_id)));
        auto bit = bodies_.find(hit.body_handle);
        if (bit != bodies_.end()) hit.collider = bit->second.owner;
    }
    return hit;
}

ShapeCastHit PhysicsWorld::cast_shape(const ShapeConfig& config, vec2 origin, vec2 translation, uint32_t mask, uint32_t exclude_body) {
    ShapeCastHit hit;
    if (!b2World_IsValid(world_id_) || !is_finite(origin) || !is_finite(translation)) return hit;
    if (translation.x() == 0.0f && translation.y() == 0.0f) return hit;

    const b2ShapeProxy proxy = make_proxy_for_shape(config);
    const b2QueryFilter filter = {.categoryBits = UINT64_MAX, .maskBits = mask};

    ShapeCastContext ctx;
    if (exclude_body != 0) {
        auto eit = bodies_.find(exclude_body);
        if (eit != bodies_.end()) ctx.self_body = eit->second.id;
    }
    ctx.mask = mask;
    ctx.motion = translation;

    b2World_CastShape(world_id_, to_b2(origin), &proxy, to_b2(translation), filter, shape_cast_callback, &ctx);

    if (b2Shape_IsValid(ctx.hit_shape) && ctx.min_fraction < 1.0f) {
        hit.hit = true;
        hit.point = {ctx.hit_point.x, ctx.hit_point.y};
        hit.normal = {ctx.hit_normal.x, ctx.hit_normal.y};
        hit.fraction = ctx.min_fraction;
        hit.shape_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Shape_GetUserData(ctx.hit_shape)));
        const b2BodyId body_id = b2Shape_GetBody(ctx.hit_shape);
        hit.body_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(body_id)));
        auto bit = bodies_.find(hit.body_handle);
        if (bit != bodies_.end()) {
            hit.collider = bit->second.owner;
        }
    }
    return hit;
}

uint32_t PhysicsWorld::query_point(vec2 point, uint32_t mask, pluma::Value* collider_out) {
    if (!b2World_IsValid(world_id_) || !is_finite(point)) return 0;
    struct PointQueryCtx {
        vec2 pt;
        uint32_t mask = 1;
        uint32_t hit_shape_handle = 0;
        uint32_t hit_body_handle = 0;
    } ctx;
    ctx.pt = point;
    ctx.mask = mask;

    b2AABB aabb;
    aabb.lowerBound = to_b2(point.x() - 1.0f, point.y() - 1.0f);
    aabb.upperBound = to_b2(point.x() + 1.0f, point.y() + 1.0f);

    auto overlap_cb = [](b2ShapeId shape_id, void* ctx_ptr) -> bool {
        auto* c = static_cast<PointQueryCtx*>(ctx_ptr);
        if (b2Shape_IsSensor(shape_id)) return true;
        const b2Filter filter = b2Shape_GetFilter(shape_id);
        if ((filter.categoryBits & c->mask) == 0) return true;
        if (b2Shape_TestPoint(shape_id, to_b2(c->pt))) {
            c->hit_shape_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Shape_GetUserData(shape_id)));
            const b2BodyId body_id = b2Shape_GetBody(shape_id);
            c->hit_body_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(body_id)));
            return false;
        }
        return true;
    };

    b2QueryFilter qfilter = b2DefaultQueryFilter();
    qfilter.categoryBits = UINT64_MAX;
    qfilter.maskBits = mask;
    b2World_OverlapAABB(world_id_, to_b2(0.0f, 0.0f), aabb, qfilter, overlap_cb, &ctx);

    if (ctx.hit_body_handle != 0) {
        auto bit = bodies_.find(ctx.hit_body_handle);
        if (bit != bodies_.end() && collider_out != nullptr) {
            *collider_out = bit->second.owner;
        }
        return ctx.hit_body_handle;
    }
    return 0;
}

void PhysicsWorld::explode(vec2 center, float radius, float falloff, float impulse_per_length, uint32_t mask) {
    if (!b2World_IsValid(world_id_) || !is_finite(center) || !is_finite(radius) || radius <= 0.0f ||
        !is_finite(falloff) || !is_finite(impulse_per_length)) {
        return;
    }
    b2ExplosionDef def = b2DefaultExplosionDef();
    def.position = to_b2(center);
    def.radius = radius;
    def.falloff = std::max(0.0f, falloff);
    def.impulsePerLength = impulse_per_length;
    def.maskBits = mask;
    b2World_Explode(world_id_, &def);
}

} // namespace cuervo::physics

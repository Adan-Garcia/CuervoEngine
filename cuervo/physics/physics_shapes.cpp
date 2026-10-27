#include "cuervo/physics/physics_world.hpp"

#include <algorithm>
#include <cmath>

#include "cuervo/core/hash.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/physics/physics_internal.hpp"
#include "pluma/vm.hpp"

namespace cuervo::physics {

using namespace detail;

uint32_t PhysicsWorld::add_shape(uint32_t body_handle, const ShapeConfig& config, const pluma::Value& owner) {
    auto bit = bodies_.find(body_handle);
    if (bit == bodies_.end() || !b2Body_IsValid(bit->second.id)) return 0;

    if (!is_finite(config.offset) || !is_finite(config.rotation) || !is_finite(config.size) ||
        !is_finite(config.point_a) || !is_finite(config.point_b) || !is_finite(config.radius) ||
        !is_finite(config.length)) {
        return 0;
    }

    b2ShapeDef sdef = b2DefaultShapeDef();
    const uint32_t handle = next_shape_handle_++;
    sdef.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
    sdef.isSensor = config.is_sensor;
    sdef.enableSensorEvents = true;
    sdef.enableContactEvents = true;
    if (!config.is_sensor && bit->second.kind == BodyKind::Dynamic) {
        sdef.enablePreSolveEvents = true;
    }
    sdef.filter.categoryBits = config.layer;
    sdef.filter.maskBits = config.mask;
    if (config.is_sensor) {
        sdef.density = 0.0f;
        sdef.updateBodyMass = false;
    } else {
        sdef.density = is_finite(config.density) ? std::max(0.0f, config.density) : 1.0f;
    }
    sdef.material.friction = is_finite(config.friction) ? std::max(0.0f, config.friction) : 0.7f;
    sdef.material.restitution = is_finite(config.restitution) ? std::clamp(config.restitution, 0.0f, 1.0f) : 0.0f;
    if (bit->second.constant_velocity.x() != 0.0f) {
        sdef.material.tangentSpeed = -bit->second.constant_velocity.x();
    }

    b2ShapeId sid = b2_nullShapeId;
    switch (config.kind) {
    case ShapeKind::Circle: {
        const b2Circle circle = {.center = to_b2(config.offset), .radius = std::max(0.01f, config.radius)};
        sid = b2CreateCircleShape(bit->second.id, &sdef, &circle);
        break;
    }
    case ShapeKind::Box: {
        const float hw = std::max(0.01f, config.size.x() * 0.5f);
        const float hh = std::max(0.01f, config.size.y() * 0.5f);
        const b2Polygon box = b2MakeOffsetBox(hw, hh,
                                              to_b2(config.offset),
                                              b2MakeRot(config.rotation));
        sid = b2CreatePolygonShape(bit->second.id, &sdef, &box);
        break;
    }
    case ShapeKind::Capsule: {
        const float r = std::max(0.01f, config.radius);
        const float half_len = std::max(0.0f, (config.length - 2.0f * r) * 0.5f);
        if (half_len <= 0.05f) {
            const b2Circle circle = {.center = to_b2(config.offset), .radius = r};
            sid = b2CreateCircleShape(bit->second.id, &sdef, &circle);
        } else {
            const b2Rot rot = b2MakeRot(config.rotation);
            const b2Vec2 off = to_b2(config.offset);
            const b2Vec2 c1 = b2Add(off, b2RotateVector(rot, to_b2(0.0f, -half_len)));
            const b2Vec2 c2 = b2Add(off, b2RotateVector(rot, to_b2(0.0f, half_len)));
            const b2Capsule cap = {.center1 = c1, .center2 = c2, .radius = r};
            sid = b2CreateCapsuleShape(bit->second.id, &sdef, &cap);
        }
        break;
    }
    case ShapeKind::Segment: {
        const bool has_tf = (config.offset.x() != 0.0f || config.offset.y() != 0.0f || config.rotation != 0.0f);
        const b2Rot rot = b2MakeRot(config.rotation);
        const b2Vec2 off = to_b2(config.offset);
        b2Vec2 p1 = has_tf ? b2Add(off, b2RotateVector(rot, to_b2(config.point_a))) : to_b2(config.point_a);
        b2Vec2 p2 = has_tf ? b2Add(off, b2RotateVector(rot, to_b2(config.point_b))) : to_b2(config.point_b);
        if (b2DistanceSquared(p1, p2) >= 1e-4f) {
            const b2Segment seg = {.point1 = p1, .point2 = p2};
            sid = b2CreateSegmentShape(bit->second.id, &sdef, &seg);
        }
        break;
    }
    case ShapeKind::Polygon: {
        std::vector<b2Vec2> b2_pts;
        b2_pts.reserve(config.points.size());
        for (const auto& pt : config.points) {
            if (is_finite(pt)) {
                b2_pts.push_back(to_b2(pt));
            }
        }
        if (b2_pts.size() >= 3) {
            const std::vector<b2Vec2> hull_input = subsample_hull_points(b2_pts);
            const b2Hull hull = b2ComputeHull(hull_input.data(), static_cast<int>(hull_input.size()));
            if (hull.count >= 3) {
                const b2Polygon poly = b2MakeOffsetPolygon(&hull, to_b2(config.offset), b2MakeRot(config.rotation));
                sid = b2CreatePolygonShape(bit->second.id, &sdef, &poly);
            }
        }
        // Fewer than three usable points, or a collinear hull, is not a polygon: report failure
        // rather than substituting some other shape.
        break;
    }
    case ShapeKind::Chain: {
        std::vector<b2Vec2> b2_pts;
        b2_pts.reserve(config.points.size());
        const b2Rot rot = b2MakeRot(config.rotation);
        const b2Vec2 off = to_b2(config.offset);
        for (const auto& pt : config.points) {
            if (!is_finite(pt)) continue;
            const b2Vec2 world_pt = b2Add(off, b2RotateVector(rot, to_b2(pt)));
            if (!b2_pts.empty() && b2DistanceSquared(b2_pts.back(), world_pt) <= 2.5e-3f) {
                continue;
            }
            b2_pts.push_back(world_pt);
        }
        if (config.is_loop && b2_pts.size() >= 2 && b2DistanceSquared(b2_pts.front(), b2_pts.back()) <= 2.5e-3f) {
            b2_pts.pop_back();
        }
        const int min_pts = config.is_loop ? 3 : 2;
        // Box2D v3 needs at least 4 chain points; pad shorter chains with collinear midpoints.
        std::vector<b2Vec2> chain_pts = b2_pts;
        while (static_cast<int>(b2_pts.size()) >= min_pts && chain_pts.size() < 4) {
            const b2Vec2 last = chain_pts.back();
            const b2Vec2 prev = chain_pts[chain_pts.size() - 2];
            const b2Vec2 mid = {0.5f * (prev.x + last.x), 0.5f * (prev.y + last.y)};
            if (b2DistanceSquared(prev, mid) <= 2.5e-3f || b2DistanceSquared(mid, last) <= 2.5e-3f) break;
            chain_pts.insert(chain_pts.end() - 1, mid);
        }
        if (static_cast<int>(b2_pts.size()) >= min_pts && chain_pts.size() >= 4) {
            b2ChainDef cdef = b2DefaultChainDef();
            cdef.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
            cdef.points = chain_pts.data();
            cdef.pointCount = static_cast<int>(chain_pts.size());
            cdef.isLoop = config.is_loop;
            if (!config.is_loop) {
                cdef.ghost1 = b2Sub(chain_pts[0], b2Sub(chain_pts[1], chain_pts[0]));
                cdef.ghost2 = b2Add(chain_pts.back(), b2Sub(chain_pts.back(), chain_pts[chain_pts.size() - 2]));
            }
            cdef.filter.categoryBits = config.layer;
            cdef.filter.maskBits = config.mask;
            cdef.enableSensorEvents = true;
            b2SurfaceMaterial mat = b2DefaultSurfaceMaterial();
            mat.friction = sdef.material.friction;
            mat.restitution = sdef.material.restitution;
            mat.tangentSpeed = sdef.material.tangentSpeed; // conveyor velocity set before the chain existed
            cdef.materials = &mat;
            cdef.materialCount = 1;

            const b2ChainId cid = b2CreateChain(bit->second.id, &cdef);
            if (b2Chain_IsValid(cid)) {
                ShapeRecord srec;
                srec.handle = handle;
                srec.id = b2_nullShapeId;
                srec.chain_id = cid;
                srec.body_handle = body_handle;
                srec.config = config;
                srec.owner = owner;
                pluma::retain(srec.owner);

                const int seg_count = b2Chain_GetSegmentCount(cid);
                if (seg_count > 0) {
                    std::vector<b2ShapeId> segs(seg_count);
                    b2Chain_GetSegments(cid, segs.data(), seg_count);
                    for (b2ShapeId seg_id : segs) {
                        b2Shape_SetUserData(seg_id, reinterpret_cast<void*>(static_cast<uintptr_t>(handle)));
                    }
                }

                bit->second.shapes.push_back(handle);
                shapes_.emplace(handle, std::move(srec));
                return handle;
            }
        }
        // Too few distinct points (or chain creation failed): fall back to a single segment.
        b2Vec2 p1 = b2_pts.size() >= 1 ? b2_pts.front() : to_b2(config.point_a);
        b2Vec2 p2 = b2_pts.size() >= 2 ? b2_pts.back() : to_b2(config.point_b);
        if (b2DistanceSquared(p1, p2) >= 1e-4f) {
            const b2Segment seg = {.point1 = p1, .point2 = p2};
            sid = b2CreateSegmentShape(bit->second.id, &sdef, &seg);
        }
        break;
    }
    }

    if (!b2Shape_IsValid(sid)) return 0;

    ShapeRecord srec;
    srec.handle = handle;
    srec.id = sid;
    srec.chain_id = b2_nullChainId;
    srec.body_handle = body_handle;
    srec.config = config;
    srec.owner = owner;
    pluma::retain(srec.owner);

    bit->second.shapes.push_back(handle);
    shapes_.emplace(handle, std::move(srec));

    if (!config.is_sensor && bit->second.kind == BodyKind::Dynamic && bit->second.mass > 0.0f) {
        b2MassData md = b2Body_GetMassData(bit->second.id);
        if (md.mass > 0.0f) {
            const float scale = bit->second.mass / md.mass;
            md.rotationalInertia *= scale;
        }
        md.mass = bit->second.mass;
        b2Body_SetMassData(bit->second.id, md);
    }

    return handle;
}

void PhysicsWorld::destroy_shape(uint32_t shape_handle) {
    auto sit = shapes_.find(shape_handle);
    if (sit == shapes_.end()) return;

    const bool was_sensor = sit->second.config.is_sensor;
    queue_sensor_exits(sit->second.body_handle, shape_handle);
    auto bit = bodies_.find(sit->second.body_handle);
    if (bit != bodies_.end()) {
        auto& v = bit->second.shapes;
        v.erase(std::remove(v.begin(), v.end(), shape_handle), v.end());
    }

    if (b2Chain_IsValid(sit->second.chain_id)) {
        b2DestroyChain(sit->second.chain_id);
    } else if (b2Shape_IsValid(sit->second.id)) {
        b2DestroyShape(sit->second.id, !was_sensor);
        if (!was_sensor && bit != bodies_.end() && bit->second.kind == BodyKind::Dynamic && bit->second.mass > 0.0f) {
            b2MassData md = b2Body_GetMassData(bit->second.id);
            if (md.mass > 0.0f) {
                const float scale = bit->second.mass / md.mass;
                md.rotationalInertia *= scale;
                md.mass = bit->second.mass;
                b2Body_SetMassData(bit->second.id, md);
            }
        }
    }
    pluma::release(sit->second.owner);
    shapes_.erase(sit);
}

void PhysicsWorld::set_shape_filter(uint32_t shape_handle, uint32_t layer, uint32_t mask) {
    auto sit = shapes_.find(shape_handle);
    if (sit == shapes_.end()) return;
    sit->second.config.layer = layer;
    sit->second.config.mask = mask;
    // A disabled shape keeps its zeroed filter; the new layer/mask apply when it is re-enabled.
    if (sit->second.disabled) return;

    if (b2Chain_IsValid(sit->second.chain_id)) {
        const int seg_count = b2Chain_GetSegmentCount(sit->second.chain_id);
        if (seg_count > 0) {
            std::vector<b2ShapeId> segs(seg_count);
            b2Chain_GetSegments(sit->second.chain_id, segs.data(), seg_count);
            for (b2ShapeId seg_id : segs) {
                b2Filter filter = b2Shape_GetFilter(seg_id);
                filter.categoryBits = layer;
                filter.maskBits = mask;
                b2Shape_SetFilter(seg_id, filter);
            }
        }
    } else if (b2Shape_IsValid(sit->second.id)) {
        b2Filter filter = b2Shape_GetFilter(sit->second.id);
        filter.categoryBits = layer;
        filter.maskBits = mask;
        b2Shape_SetFilter(sit->second.id, filter);
    }
}

void PhysicsWorld::set_shape_disabled(uint32_t shape_handle, bool disabled) {
    auto sit = shapes_.find(shape_handle);
    if (sit == shapes_.end() || sit->second.disabled == disabled) return;
    sit->second.disabled = disabled;

    if (b2Chain_IsValid(sit->second.chain_id)) {
        const int seg_count = b2Chain_GetSegmentCount(sit->second.chain_id);
        if (seg_count > 0) {
            std::vector<b2ShapeId> segs(seg_count);
            b2Chain_GetSegments(sit->second.chain_id, segs.data(), seg_count);
            for (b2ShapeId seg_id : segs) {
                b2Filter filter = b2Shape_GetFilter(seg_id);
                if (disabled) {
                    filter.categoryBits = 0;
                    filter.maskBits = 0;
                } else {
                    filter.categoryBits = sit->second.config.layer;
                    filter.maskBits = sit->second.config.mask;
                }
                b2Shape_SetFilter(seg_id, filter);
            }
        }
    } else if (b2Shape_IsValid(sit->second.id)) {
        b2Filter filter = b2Shape_GetFilter(sit->second.id);
        if (disabled) {
            filter.categoryBits = 0;
            filter.maskBits = 0;
        } else {
            filter.categoryBits = sit->second.config.layer;
            filter.maskBits = sit->second.config.mask;
        }
        b2Shape_SetFilter(sit->second.id, filter);
    }
}

void PhysicsWorld::set_shape_material(uint32_t shape_handle, float friction, float restitution) {
    if (!is_finite(friction) || !is_finite(restitution)) return;
    auto sit = shapes_.find(shape_handle);
    if (sit == shapes_.end()) return;
    const float f = std::max(0.0f, friction);
    const float r = std::clamp(restitution, 0.0f, 1.0f);
    sit->second.config.friction = f;
    sit->second.config.restitution = r;

    if (b2Chain_IsValid(sit->second.chain_id)) {
        b2SurfaceMaterial mat = b2DefaultSurfaceMaterial();
        mat.friction = f;
        mat.restitution = r;
        b2Chain_SetSurfaceMaterial(sit->second.chain_id, &mat, 0);
    } else if (b2Shape_IsValid(sit->second.id)) {
        b2Shape_SetFriction(sit->second.id, f);
        b2Shape_SetRestitution(sit->second.id, r);
    }
}

} // namespace cuervo::physics

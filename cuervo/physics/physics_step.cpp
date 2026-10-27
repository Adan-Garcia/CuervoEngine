#include "cuervo/physics/physics_world.hpp"

#include <algorithm>
#include <cmath>

#include "cuervo/core/hash.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/physics/physics_internal.hpp"
#include "pluma/vm.hpp"

namespace cuervo::physics {

using namespace detail;

void PhysicsWorld::step(float dt, pluma::VM* vm) {
    if (!b2World_IsValid(world_id_) || !is_finite(dt) || dt < 0.0f) return;

    // 0. Synchronize kinematic bodies (AnimatableBody2D) from scene nodes into Box2D.
    struct SyncedKinematic {
        b2BodyId id;
        b2Vec2 target_pos;
        b2Rot target_rot;
    };
    std::vector<SyncedKinematic> synced_kinematics;
    if (vm != nullptr) {
        // Scripts may create or destroy bodies from these callbacks, so walk a snapshot of the
        // (ordered) handles and re-look each one up rather than iterating the live map.
        std::vector<uint32_t> snapshot;
        snapshot.reserve(bodies_.size());
        for (const auto& entry : bodies_) snapshot.push_back(entry.first);
        for (const uint32_t handle : snapshot) {
            auto body_it = bodies_.find(handle);
            if (body_it == bodies_.end()) continue;
            BodyRecord& body = body_it->second;
            if (body.kind == BodyKind::Kinematic && body.owner.is_obj() && b2Body_IsValid(body.id)) {
                const b2Vec2 old_pos = b2Body_GetPosition(body.id);
                const b2Rot old_rot = b2Body_GetRotation(body.id);
                body.transform_synced = false;
                vm->call_method(body.owner, "_sync_to_physics", {});
                if (body.transform_synced && b2Body_IsValid(body.id)) {
                    const b2Vec2 new_pos = b2Body_GetPosition(body.id);
                    const b2Rot new_rot = b2Body_GetRotation(body.id);
                    if (dt > 1e-6f) {
                        const b2Vec2 vel = {(new_pos.x - old_pos.x) / dt, (new_pos.y - old_pos.y) / dt};
                        const float ang_vel = b2RelativeAngle(new_rot, old_rot) / dt;
                        b2Body_SetTransform(body.id, old_pos, old_rot);
                        b2Body_SetLinearVelocity(body.id, vel);
                        b2Body_SetAngularVelocity(body.id, ang_vel);
                        synced_kinematics.push_back({body.id, new_pos, new_rot});
                    }
                }
            }
        }
    }

    ++step_count_;
    b2World_Step(world_id_, dt, solver_substeps);

    for (const auto& sk : synced_kinematics) {
        if (b2Body_IsValid(sk.id)) {
            b2Body_SetTransform(sk.id, sk.target_pos, sk.target_rot);
        }
    }

    // 1. Synchronize moved dynamic bodies back to scene nodes.
    const b2BodyEvents body_events = b2World_GetBodyEvents(world_id_);
    for (int i = 0; i < body_events.moveCount; ++i) {
        const b2BodyMoveEvent& ev = body_events.moveEvents[i];
        const auto handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ev.userData));
        auto it = bodies_.find(handle);
        if (it != bodies_.end() && it->second.kind == BodyKind::Dynamic && it->second.owner.is_obj() && vm != nullptr) {
            const float x = ev.transform.p.x;
            const float y = ev.transform.p.y;
            const float rot = b2Rot_GetAngle(ev.transform.q);
            const pluma::Value args[2] = {
                pluma::vec2_value(x, y),
                pluma::Value::f32(rot),
            };
            vm->call_method(it->second.owner, "_set_physics_transform", args);
        }
    }

    // 1b. Post-step floor depenetration for CharacterBody2D instances when platforms move upward during step().
    if (vm != nullptr) {
        constexpr float slop = character_slop;
        // Scripts may create or destroy bodies from these callbacks, so walk a snapshot of the
        // (ordered) handles and re-look each one up rather than iterating the live map.
        std::vector<uint32_t> snapshot;
        snapshot.reserve(bodies_.size());
        for (const auto& entry : bodies_) snapshot.push_back(entry.first);
        for (const uint32_t handle : snapshot) {
            auto body_it = bodies_.find(handle);
            if (body_it == bodies_.end()) continue;
            BodyRecord& body = body_it->second;
            if (!body.is_character || !body.owner.is_obj() || !b2Body_IsValid(body.id)) continue;

            b2ShapeProxy proxy;
            b2ShapeId self_shape_id = b2_nullShapeId;
            bool has_proxy = false;
            const b2Rot body_rot = b2Body_GetRotation(body.id);
            for (uint32_t s_handle : body.shapes) {
                auto sit = shapes_.find(s_handle);
                if (sit != shapes_.end() && !sit->second.config.is_sensor && !sit->second.disabled) {
                    proxy = make_proxy_for_shape(sit->second.config, body_rot);
                    self_shape_id = sit->second.id;
                    has_proxy = true;
                    break;
                }
            }
            if (!has_proxy || !b2Shape_IsValid(self_shape_id)) continue;

            b2Vec2 current_pos = b2Body_GetPosition(body.id);
            const b2QueryFilter filter = {.categoryBits = body.layer, .maskBits = body.mask};
            bool moved = false;

            for (int dep_iter = 0; dep_iter < 4; ++dep_iter) {
                struct DepenOverlapCtx {
                    b2BodyId self_body;
                    uint32_t mask;
                    std::vector<b2ShapeId> shapes;
                } ov_ctx{body.id, body.mask, {}};

                auto ov_cb = [](b2ShapeId sid, void* ptr) -> bool {
                    auto* c = static_cast<DepenOverlapCtx*>(ptr);
                    if (b2Shape_IsSensor(sid)) return true;
                    const b2BodyId bid = b2Shape_GetBody(sid);
                    if (B2_ID_EQUALS(bid, c->self_body)) return true;
                    const b2Filter f = b2Shape_GetFilter(sid);
                    if ((f.categoryBits & c->mask) == 0) return true;
                    c->shapes.push_back(sid);
                    return true;
                };

                b2World_OverlapShape(world_id_, current_pos, &proxy, filter, ov_cb, &ov_ctx);
                if (ov_ctx.shapes.empty()) break;

                b2ShapeId best_shape = b2_nullShapeId;
                b2Vec2 best_norm = {0.0f, 0.0f};
                float best_depth = 0.0f;
                const b2Transform self_xf = {current_pos, body_rot};
                const b2AABB self_aabb = get_shifted_shape_aabb(self_shape_id, body.id, current_pos);

                for (b2ShapeId osid : ov_ctx.shapes) {
                    const b2BodyId obid = b2Shape_GetBody(osid);
                    if (!b2Body_IsValid(obid)) continue;
                    b2Vec2 norm = {0.0f, 0.0f};
                    float depth = 0.0f;
                    if (compute_shape_penetration(self_shape_id, self_xf, osid, b2Body_GetTransform(obid), norm, depth)) {
                        if (dot(to_v2(norm), body.up_direction) < body.floor_cos && body.up_direction.y() < -0.5f) {
                            const b2AABB other_aabb = b2Shape_GetBounds(osid);
                            const float pen_up = self_aabb.upperBound.y - other_aabb.lowerBound.y;
                            if (pen_up > 0.0f && pen_up <= character_max_step_height && self_aabb.lowerBound.y < other_aabb.lowerBound.y) {
                                const b2Vec2 rot_up = b2RotateVector(b2Body_GetRotation(obid), to_b2(body.up_direction));
                                norm = (dot(to_v2(rot_up), body.up_direction) >= body.floor_cos) ? rot_up : to_b2(body.up_direction);
                                depth = pen_up;
                            }
                        }
                        if (depth > best_depth && dot(to_v2(norm), body.up_direction) >= body.floor_cos) {
                            best_depth = depth;
                            best_norm = norm;
                            best_shape = osid;
                        }
                    }
                }

                if (!b2Shape_IsValid(best_shape) || best_depth <= 1e-4f) break;

                const float dot_up = dot(to_v2(best_norm), body.up_direction);
                const b2Vec2 push_motion = b2MulSV((best_depth + slop) / std::max(0.5f, dot_up), to_b2(body.up_direction));
                ShapeCastContext push_ctx = sweep_shape(world_id_, proxy, filter, body.id, self_shape_id, current_pos, body_rot, body.mask, push_motion);

                if (b2Shape_IsValid(push_ctx.hit_shape) && push_ctx.min_fraction < 1.0f) {
                    const float frac = std::max(0.0f, push_ctx.min_fraction);
                    current_pos = b2Add(current_pos, b2MulSV(frac, push_motion));
                    const vec2 block_norm = to_v2(push_ctx.hit_normal);
                    current_pos = b2Add(current_pos, to_b2(block_norm.x() * slop, block_norm.y() * slop));
                    moved = true;
                    break;
                } else {
                    current_pos = b2Add(current_pos, push_motion);
                    moved = true;
                }
            }

            // If on floor and not depenetrated upward, snap downward if the platform descended during step().
            if (!moved && body.on_floor) {
                constexpr float snap_dist = floor_probe_dist;
                const b2Vec2 snap_motion = to_b2(-body.up_direction.x() * snap_dist, -body.up_direction.y() * snap_dist);
                ShapeCastContext snap_ctx = sweep_shape(world_id_, proxy, filter, body.id, self_shape_id, current_pos, body_rot, body.mask, snap_motion);
                if (b2Shape_IsValid(snap_ctx.hit_shape) && snap_ctx.min_fraction > 0.0f && snap_ctx.min_fraction < 1.0f) {
                    const vec2 norm = to_v2(snap_ctx.hit_normal);
                    const float dot_up = dot(norm, body.up_direction);
                    if (dot_up >= body.floor_cos) {
                        const float frac = std::max(0.0f, snap_ctx.min_fraction);
                        const float up_slop = slop / std::max(0.5f, dot_up);
                        current_pos = b2Add(current_pos, b2MulSV(frac, snap_motion));
                        current_pos = b2Add(current_pos, to_b2(body.up_direction.x() * up_slop, body.up_direction.y() * up_slop));
                        moved = true;
                    }
                }
            }

            if (moved) {
                b2Body_SetTransform(body.id, current_pos, body_rot);
                const pluma::Value args[2] = {
                    pluma::vec2_value(current_pos.x, current_pos.y),
                    pluma::Value::f32(b2Rot_GetAngle(body_rot)),
                };
                vm->call_method(body.owner, "_set_physics_transform", args);
            }
        }
    }

    // 2. Dispatch sensor events to Area2D instances.
    if (vm != nullptr) {
        flush_sensor_exits(vm);

        // Resolves the Area2D that owns a sensor shape and the body that entered/left it, then calls
        // the matching `_on_{body,area}_{enter,exit}` hook.
        const auto dispatch = [&](b2ShapeId sensor_shape, b2ShapeId visitor_shape, bool entered) {
            if (!b2Shape_IsValid(sensor_shape) || !b2Shape_IsValid(visitor_shape)) return;

            const auto shape_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Shape_GetUserData(sensor_shape)));
            const auto visitor_body = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(visitor_shape))));
            const auto sit = shapes_.find(shape_handle);
            const auto vbit = bodies_.find(visitor_body);
            if (sit == shapes_.end() || vbit == bodies_.end()) return;

            const auto sensor_body = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(sensor_shape))));
            const auto sbit = bodies_.find(sensor_body);
            const pluma::Value area_owner = sbit != bodies_.end() ? sbit->second.owner : sit->second.owner;
            const pluma::Value visitor_owner = vbit->second.owner;
            if (!area_owner.is_obj() || !visitor_owner.is_obj()) return;
            call_sensor_hook(vm, area_owner, visitor_owner, b2Shape_IsSensor(visitor_shape), entered);
        };

        const b2SensorEvents sensor_events = b2World_GetSensorEvents(world_id_);
        for (int i = 0; i < sensor_events.beginCount; ++i) {
            dispatch(sensor_events.beginEvents[i].sensorShapeId, sensor_events.beginEvents[i].visitorShapeId, true);
        }
        for (int i = 0; i < sensor_events.endCount; ++i) {
            dispatch(sensor_events.endEvents[i].sensorShapeId, sensor_events.endEvents[i].visitorShapeId, false);
        }
    }
}

void PhysicsWorld::call_sensor_hook(pluma::VM* vm, const pluma::Value& area, const pluma::Value& visitor,
                                    bool visitor_is_area, bool entered) {
    const char* hook = visitor_is_area ? (entered ? "_on_area_enter" : "_on_area_exit")
                                       : (entered ? "_on_body_enter" : "_on_body_exit");
    vm->call_method(area, hook, std::span<const pluma::Value>(&visitor, 1));
}

void PhysicsWorld::queue_sensor_exits(uint32_t body_handle, uint32_t shape_handle) {
    const auto vbit = bodies_.find(body_handle);
    if (vbit == bodies_.end() || !vbit->second.owner.is_obj()) return;

    for (const auto& [sensor_handle, sensor] : shapes_) {
        if (!sensor.config.is_sensor || sensor.body_handle == body_handle || !b2Shape_IsValid(sensor.id)) continue;
        const int capacity = b2Shape_GetSensorCapacity(sensor.id);
        if (capacity <= 0) continue;
        std::vector<b2ShapeId> visitors(static_cast<std::size_t>(capacity));
        const int count = b2Shape_GetSensorData(sensor.id, visitors.data(), capacity);
        for (int i = 0; i < count; ++i) {
            const b2ShapeId visitor = visitors[static_cast<std::size_t>(i)];
            if (!b2Shape_IsValid(visitor)) continue;
            const auto owner_body = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(b2Body_GetUserData(b2Shape_GetBody(visitor))));
            const auto visitor_shape = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Shape_GetUserData(visitor)));
            if (owner_body != body_handle || (shape_handle != 0 && visitor_shape != shape_handle)) continue;
            PendingSensorExit exit;
            exit.sensor_shape = sensor_handle;
            exit.visitor = vbit->second.owner;
            exit.visitor_is_area = b2Shape_IsSensor(visitor);
            pluma::retain(exit.visitor);
            pending_exits_.push_back(exit);
        }
    }
}

void PhysicsWorld::flush_sensor_exits(pluma::VM* vm) {
    // Exits queued for destroyed visitors: Box2D's own end events skip shapes that no longer exist, so
    // an Area2D would otherwise keep a stale body in its overlap list.
    std::vector<PendingSensorExit> pending;
    pending.swap(pending_exits_);
    for (PendingSensorExit& exit : pending) {
        const auto sit = shapes_.find(exit.sensor_shape);
        if (sit != shapes_.end()) {
            const auto sbit = bodies_.find(sit->second.body_handle);
            const pluma::Value area_owner = sbit != bodies_.end() ? sbit->second.owner : sit->second.owner;
            if (area_owner.is_obj() && exit.visitor.is_obj()) {
                call_sensor_hook(vm, area_owner, exit.visitor, exit.visitor_is_area, false);
            }
        }
        pluma::release(exit.visitor);
    }
}

std::uint64_t PhysicsWorld::state_hash() const {
    StateHasher h;
    if (!b2World_IsValid(world_id_)) return 0;

    // bodies_ is an ordered map, so iteration order (and the hash) is the same on every platform.
    for (const auto& [handle, body] : bodies_) {
        if (!b2Body_IsValid(body.id)) continue;
        const b2Vec2 pos = b2Body_GetPosition(body.id);
        const b2Rot rot = b2Body_GetRotation(body.id);
        const b2Vec2 vel = b2Body_GetLinearVelocity(body.id);
        const float ang_vel = b2Body_GetAngularVelocity(body.id);

        h.value(handle);
        h.value(pos.x);
        h.value(pos.y);
        h.value(rot.c);
        h.value(rot.s);
        h.value(vel.x);
        h.value(vel.y);
        h.value(ang_vel);
    }
    return h.digest();
}

} // namespace cuervo::physics

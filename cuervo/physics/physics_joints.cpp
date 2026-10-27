#include "cuervo/physics/physics_world.hpp"

#include <algorithm>
#include <cmath>

#include "cuervo/core/hash.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/physics/physics_internal.hpp"
#include "pluma/vm.hpp"

namespace cuervo::physics {

using namespace detail;

uint32_t PhysicsWorld::register_joint(b2JointId jid, uint32_t handle, uint32_t body_a, uint32_t body_b,
                                      const pluma::Value& owner) {
    if (!b2Joint_IsValid(jid)) return 0;

    JointRecord rec;
    rec.handle = handle;
    rec.id = jid;
    rec.body_a = body_a;
    rec.body_b = body_b;
    rec.owner = owner;
    pluma::retain(rec.owner);

    joints_.emplace(handle, std::move(rec));
    return handle;
}

uint32_t PhysicsWorld::create_pin_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, float softness,
                                       bool limit_enabled, float lower_limit, float upper_limit,
                                       bool motor_enabled, float motor_speed, float max_motor_torque,
                                       bool collide_connected, const pluma::Value& owner) {
    if (body_a == body_b || !is_finite(anchor)) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;
    def.base.collideConnected = collide_connected;
    def.base.localFrameA.p = b2Body_GetLocalPoint(bit_a->second.id, to_b2(anchor));
    def.base.localFrameB.p = b2Body_GetLocalPoint(bit_b->second.id, to_b2(anchor));
    const b2Rot rot_a = b2Body_GetRotation(bit_a->second.id);
    const b2Rot rot_b = b2Body_GetRotation(bit_b->second.id);
    def.base.localFrameA.q = b2MakeRot(0.0f);
    def.base.localFrameB.q = b2InvMulRot(rot_b, rot_a);

    if (is_finite(softness) && softness > 0.0f) {
        def.enableSpring = true;
        def.hertz = 1.0f / softness;
        def.dampingRatio = 0.7f;
    }
    // A limit with non-finite bounds would pin the joint at 0..0, so it is left off instead.
    def.enableLimit = limit_enabled && is_finite(lower_limit) && is_finite(upper_limit);
    if (def.enableLimit) {
        if (lower_limit > upper_limit) std::swap(lower_limit, upper_limit);
        def.lowerAngle = lower_limit;
        def.upperAngle = upper_limit;
    }
    def.enableMotor = motor_enabled;
    def.motorSpeed = is_finite(motor_speed) ? motor_speed : 0.0f;
    def.maxMotorTorque = is_finite(max_motor_torque) ? std::max(0.0f, max_motor_torque) : 0.0f;

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreateRevoluteJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

uint32_t PhysicsWorld::create_distance_joint(uint32_t body_a, uint32_t body_b, vec2 anchor_a, vec2 anchor_b,
                                            float length, float min_length, float max_length,
                                            float stiffness, float damping,
                                            bool collide_connected, const pluma::Value& owner) {
    if (body_a == body_b || !is_finite(anchor_a) || !is_finite(anchor_b)) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2DistanceJointDef def = b2DefaultDistanceJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;
    def.base.collideConnected = collide_connected;
    def.base.localFrameA.p = b2Body_GetLocalPoint(bit_a->second.id, to_b2(anchor_a));
    def.base.localFrameB.p = b2Body_GetLocalPoint(bit_b->second.id, to_b2(anchor_b));

    if (is_finite(length) && length > 0.0f) {
        def.length = length;
    } else {
        const vec2 d = anchor_b - anchor_a;
        def.length = std::sqrt(d.x() * d.x() + d.y() * d.y());
    }
    def.minLength = (is_finite(min_length) && min_length > 0.0f) ? min_length : 0.0f;
    def.maxLength = (is_finite(max_length) && max_length > 0.0f) ? std::max(def.minLength, max_length) : B2_HUGE;
    def.hertz = is_finite(stiffness) ? std::max(0.0f, stiffness) : 0.0f;
    def.dampingRatio = is_finite(damping) ? std::max(0.0f, damping) : 0.0f;
    def.enableSpring = def.hertz > 0.0f;
    def.enableLimit = (min_length > 0.0f || (max_length > 0.0f && max_length < B2_HUGE));

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreateDistanceJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

uint32_t PhysicsWorld::create_prismatic_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, vec2 axis,
                                             bool enable_spring, float hertz, float damping_ratio, float target_translation,
                                             bool enable_limit, float lower_translation, float upper_translation,
                                             bool enable_motor, float max_motor_force, float motor_speed,
                                             bool collide_connected, const pluma::Value& owner) {
    if (body_a == body_b || !is_finite(anchor) || !is_finite(axis)) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2PrismaticJointDef def = b2DefaultPrismaticJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;
    def.base.collideConnected = collide_connected;

    const b2Vec2 b2_anchor = to_b2(anchor);
    def.base.localFrameA.p = b2Body_GetLocalPoint(bit_a->second.id, b2_anchor);
    def.base.localFrameB.p = b2Body_GetLocalPoint(bit_b->second.id, b2_anchor);

    const float axis_len = std::sqrt(axis.x() * axis.x() + axis.y() * axis.y());
    const vec2 norm_axis = axis_len > 1e-6f ? vec2{axis.x() / axis_len, axis.y() / axis_len} : vec2{1.0f, 0.0f};
    const float world_angle = dmath::atan2(norm_axis.y(), norm_axis.x());
    const b2Rot rot_a = b2Body_GetRotation(bit_a->second.id);
    const b2Rot rot_b = b2Body_GetRotation(bit_b->second.id);
    def.base.localFrameA.q = b2MakeRot(world_angle - b2Rot_GetAngle(rot_a));
    def.base.localFrameB.q = b2MakeRot(world_angle - b2Rot_GetAngle(rot_b));

    def.enableSpring = enable_spring;
    def.hertz = is_finite(hertz) ? std::max(0.0f, hertz) : 0.0f;
    def.dampingRatio = is_finite(damping_ratio) ? std::max(0.0f, damping_ratio) : 0.0f;
    def.targetTranslation = is_finite(target_translation) ? target_translation : 0.0f;
    def.enableLimit = enable_limit;
    if (is_finite(lower_translation) && is_finite(upper_translation)) {
        if (lower_translation > upper_translation) std::swap(lower_translation, upper_translation);
        def.lowerTranslation = lower_translation;
        def.upperTranslation = upper_translation;
    }
    def.enableMotor = enable_motor;
    def.maxMotorForce = is_finite(max_motor_force) ? std::max(0.0f, max_motor_force) : 0.0f;
    def.motorSpeed = is_finite(motor_speed) ? motor_speed : 0.0f;

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreatePrismaticJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

uint32_t PhysicsWorld::create_weld_joint(uint32_t body_a, uint32_t body_b, vec2 anchor,
                                        float linear_hertz, float angular_hertz,
                                        float linear_damping_ratio, float angular_damping_ratio,
                                        bool collide_connected, const pluma::Value& owner) {
    if (body_a == body_b || !is_finite(anchor)) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2WeldJointDef def = b2DefaultWeldJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;
    def.base.collideConnected = collide_connected;

    const b2Vec2 b2_anchor = to_b2(anchor);
    def.base.localFrameA.p = b2Body_GetLocalPoint(bit_a->second.id, b2_anchor);
    def.base.localFrameB.p = b2Body_GetLocalPoint(bit_b->second.id, b2_anchor);

    const b2Rot rot_a = b2Body_GetRotation(bit_a->second.id);
    const b2Rot rot_b = b2Body_GetRotation(bit_b->second.id);
    def.base.localFrameA.q = b2MakeRot(0.0f);
    def.base.localFrameB.q = b2InvMulRot(rot_b, rot_a);

    def.linearHertz = is_finite(linear_hertz) ? std::max(0.0f, linear_hertz) : 0.0f;
    def.angularHertz = is_finite(angular_hertz) ? std::max(0.0f, angular_hertz) : 0.0f;
    def.linearDampingRatio = is_finite(linear_damping_ratio) ? std::max(0.0f, linear_damping_ratio) : 0.0f;
    def.angularDampingRatio = is_finite(angular_damping_ratio) ? std::max(0.0f, angular_damping_ratio) : 0.0f;

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreateWeldJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

uint32_t PhysicsWorld::create_wheel_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, vec2 axis,
                                         bool enable_spring, float hertz, float damping_ratio,
                                         bool enable_limit, float lower_translation, float upper_translation,
                                         bool enable_motor, float max_motor_torque, float motor_speed,
                                         bool collide_connected, const pluma::Value& owner) {
    if (body_a == body_b || !is_finite(anchor) || !is_finite(axis)) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2WheelJointDef def = b2DefaultWheelJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;
    def.base.collideConnected = collide_connected;

    const b2Vec2 b2_anchor = to_b2(anchor);
    def.base.localFrameA.p = b2Body_GetLocalPoint(bit_a->second.id, b2_anchor);
    def.base.localFrameB.p = b2Body_GetLocalPoint(bit_b->second.id, b2_anchor);

    const float axis_len = std::sqrt(axis.x() * axis.x() + axis.y() * axis.y());
    const vec2 norm_axis = axis_len > 1e-6f ? vec2{axis.x() / axis_len, axis.y() / axis_len} : vec2{0.0f, 1.0f};
    const float world_angle = dmath::atan2(norm_axis.y(), norm_axis.x());
    const b2Rot rot_a = b2Body_GetRotation(bit_a->second.id);
    const b2Rot rot_b = b2Body_GetRotation(bit_b->second.id);
    def.base.localFrameA.q = b2MakeRot(world_angle - b2Rot_GetAngle(rot_a));
    def.base.localFrameB.q = b2MakeRot(world_angle - b2Rot_GetAngle(rot_b));

    def.enableSpring = enable_spring;
    def.hertz = is_finite(hertz) ? std::max(0.0f, hertz) : 0.0f;
    def.dampingRatio = is_finite(damping_ratio) ? std::max(0.0f, damping_ratio) : 0.0f;
    def.enableLimit = enable_limit;
    if (is_finite(lower_translation) && is_finite(upper_translation)) {
        if (lower_translation > upper_translation) std::swap(lower_translation, upper_translation);
        def.lowerTranslation = lower_translation;
        def.upperTranslation = upper_translation;
    }
    def.enableMotor = enable_motor;
    def.maxMotorTorque = is_finite(max_motor_torque) ? std::max(0.0f, max_motor_torque) : 0.0f;
    def.motorSpeed = is_finite(motor_speed) ? motor_speed : 0.0f;

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreateWheelJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

uint32_t PhysicsWorld::create_motor_joint(uint32_t body_a, uint32_t body_b,
                                         vec2 linear_offset, float angular_offset,
                                         float max_velocity_force, float max_velocity_torque,
                                         float linear_hertz, float linear_damping_ratio, float max_spring_force,
                                         float angular_hertz, float angular_damping_ratio, float max_spring_torque,
                                         bool collide_connected, const pluma::Value& owner) {
    if (body_a == body_b || !is_finite(linear_offset) || !is_finite(angular_offset)) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2MotorJointDef def = b2DefaultMotorJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;
    def.base.collideConnected = collide_connected;

    def.base.localFrameA.p = to_b2(linear_offset);
    def.base.localFrameA.q = b2MakeRot(angular_offset);
    def.base.localFrameB.p = b2Vec2{0.0f, 0.0f};
    def.base.localFrameB.q = b2MakeRot(0.0f);

    def.maxVelocityForce = is_finite(max_velocity_force) ? std::max(0.0f, max_velocity_force) : 0.0f;
    def.maxVelocityTorque = is_finite(max_velocity_torque) ? std::max(0.0f, max_velocity_torque) : 0.0f;
    def.linearHertz = is_finite(linear_hertz) ? std::max(0.0f, linear_hertz) : 0.0f;
    def.linearDampingRatio = is_finite(linear_damping_ratio) ? std::max(0.0f, linear_damping_ratio) : 0.0f;
    def.maxSpringForce = is_finite(max_spring_force) ? std::max(0.0f, max_spring_force) : 0.0f;
    def.angularHertz = is_finite(angular_hertz) ? std::max(0.0f, angular_hertz) : 0.0f;
    def.angularDampingRatio = is_finite(angular_damping_ratio) ? std::max(0.0f, angular_damping_ratio) : 0.0f;
    def.maxSpringTorque = is_finite(max_spring_torque) ? std::max(0.0f, max_spring_torque) : 0.0f;

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreateMotorJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

uint32_t PhysicsWorld::create_mover_joint(uint32_t body_a, uint32_t body_b,
                                         vec2 linear_velocity, vec2 max_velocity_force,
                                         bool collide_connected, const pluma::Value& owner) {
    if (body_a == body_b || !is_finite(linear_velocity) || !is_finite(max_velocity_force)) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2MoverJointDef def = b2DefaultMoverJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;
    def.base.collideConnected = collide_connected;
    def.linearVelocity = to_b2(linear_velocity);
    def.maxVelocityForce = to_b2(std::max(0.0f, max_velocity_force.x()), std::max(0.0f, max_velocity_force.y()));

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreateMoverJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

uint32_t PhysicsWorld::create_pogo_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, vec2 normal,
                                        float hertz, float damping_ratio, float rest_length,
                                        float max_tension_force, float max_compression_force,
                                        bool collide_connected, const pluma::Value& owner) {
    if (body_a == body_b || !is_finite(anchor) || !is_finite(normal)) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2PogoJointDef def = b2DefaultPogoJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;
    def.base.collideConnected = collide_connected;

    const float norm_len = std::sqrt(normal.x() * normal.x() + normal.y() * normal.y());
    def.normal = norm_len > 1e-6f ? to_b2(normal.x() / norm_len, normal.y() / norm_len) : b2Vec2{0.0f, 1.0f};
    def.hertz = is_finite(hertz) ? std::max(0.0f, hertz) : 0.0f;
    def.dampingRatio = is_finite(damping_ratio) ? std::max(0.0f, damping_ratio) : 0.0f;
    def.restLength = is_finite(rest_length) ? std::max(0.0f, rest_length) : 0.0f;
    def.maxTensionForce = is_finite(max_tension_force) ? std::max(0.0f, max_tension_force) : 0.0f;
    def.maxCompressionForce = is_finite(max_compression_force) ? std::max(0.0f, max_compression_force) : 0.0f;

    def.base.localFrameA.p = b2Vec2{0.0f, 0.0f};
    def.base.localFrameB.p = b2Vec2{0.0f, 0.0f};
    const float world_angle = dmath::atan2(def.normal.y, def.normal.x) - B2_PI * 0.5f;
    const b2Rot rot_b = b2Body_GetRotation(bit_b->second.id);
    def.base.localFrameB.q = b2MakeRot(world_angle - b2Rot_GetAngle(rot_b));

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreatePogoJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

uint32_t PhysicsWorld::create_filter_joint(uint32_t body_a, uint32_t body_b, const pluma::Value& owner) {
    if (body_a == body_b) return 0;
    auto bit_a = bodies_.find(body_a);
    auto bit_b = bodies_.find(body_b);
    if (bit_a == bodies_.end() || bit_b == bodies_.end()) return 0;
    if (!b2Body_IsValid(bit_a->second.id) || !b2Body_IsValid(bit_b->second.id)) return 0;

    b2FilterJointDef def = b2DefaultFilterJointDef();
    def.base.bodyIdA = bit_a->second.id;
    def.base.bodyIdB = bit_b->second.id;

    const uint32_t handle = next_joint_handle_++;
    def.base.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2JointId jid = b2CreateFilterJoint(world_id_, &def);
    return register_joint(jid, handle, body_a, body_b, owner);
}

void PhysicsWorld::destroy_joint(uint32_t handle) {
    auto it = joints_.find(handle);
    if (it == joints_.end()) return;
    if (b2Joint_IsValid(it->second.id)) {
        b2DestroyJoint(it->second.id);
    }
    pluma::release(it->second.owner);
    joints_.erase(it);
}

void PhysicsWorld::joint_set_motor_speed(uint32_t handle, float speed) {
    if (!is_finite(speed)) return;
    auto it = joints_.find(handle);
    if (it == joints_.end() || !b2Joint_IsValid(it->second.id)) return;
    switch (b2Joint_GetType(it->second.id)) {
        case b2_revoluteJoint: b2RevoluteJoint_SetTargetMotorSpeed(it->second.id, speed); break;
        case b2_prismaticJoint: b2PrismaticJoint_SetTargetMotorSpeed(it->second.id, speed); break;
        case b2_wheelJoint: b2WheelJoint_SetTargetMotorSpeed(it->second.id, speed); break;
        case b2_distanceJoint: b2DistanceJoint_SetTargetMotorSpeed(it->second.id, speed); break;
        case b2_motorJoint:
            // A motor joint has no single "speed": drive its angular velocity, switching off the angular spring.
            b2MotorJoint_SetAngularHertz(it->second.id, 0.0f);
            b2MotorJoint_SetAngularVelocity(it->second.id, speed);
            break;
        default: break;
    }
}

void PhysicsWorld::joint_set_motor_enabled(uint32_t handle, bool enabled) {
    auto it = joints_.find(handle);
    if (it == joints_.end() || !b2Joint_IsValid(it->second.id)) return;
    switch (b2Joint_GetType(it->second.id)) {
        case b2_revoluteJoint: b2RevoluteJoint_EnableMotor(it->second.id, enabled); break;
        case b2_prismaticJoint: b2PrismaticJoint_EnableMotor(it->second.id, enabled); break;
        case b2_wheelJoint: b2WheelJoint_EnableMotor(it->second.id, enabled); break;
        case b2_distanceJoint: b2DistanceJoint_EnableMotor(it->second.id, enabled); break;
        default: break;
    }
}

vec2 PhysicsWorld::joint_get_reaction_force(uint32_t handle) const {
    auto it = joints_.find(handle);
    if (it == joints_.end() || !b2Joint_IsValid(it->second.id)) return vec2{0.0f, 0.0f};
    const b2Vec2 f = b2Joint_GetConstraintForce(it->second.id);
    return vec2{f.x, f.y};
}

float PhysicsWorld::joint_get_reaction_torque(uint32_t handle) const {
    auto it = joints_.find(handle);
    if (it == joints_.end() || !b2Joint_IsValid(it->second.id)) return 0.0f;
    return b2Joint_GetConstraintTorque(it->second.id);
}

} // namespace cuervo::physics

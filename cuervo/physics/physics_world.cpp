#include "cuervo/physics/physics_world.hpp"

#include <algorithm>
#include <cmath>

#include "cuervo/core/hash.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/math/dmath.hpp"
#include "cuervo/physics/physics_internal.hpp"
#include "pluma/snapshot.hpp"
#include "pluma/vm.hpp"

namespace cuervo::physics {

using namespace detail;

PhysicsWorld::PhysicsWorld(unsigned worker_count, vec2 gravity)
    : worker_count_(worker_count), gravity_(gravity) {
    init_world();
}

PhysicsWorld::~PhysicsWorld() {
    destroy_world();
}

void PhysicsWorld::init_world() {
    b2SetLengthUnitsPerMeter(32.0f);
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = to_b2(gravity_);
    def.workerCount = static_cast<int>(worker_count_);
    def.enableContinuous = true;
    def.enableSleep = sleep_enabled_;
    world_id_ = b2CreateWorld(&def);
    b2World_SetPreSolveCallback(world_id_, pre_solve_callback, nullptr, this);
}

void PhysicsWorld::pre_solve_callback(b2ShapeId shape_a, b2ShapeId shape_b, b2Manifold* manifold, void* ctx) {
    if (manifold == nullptr || manifold->pointCount <= 0 || ctx == nullptr) return;
    if (!b2Shape_IsValid(shape_a) || !b2Shape_IsValid(shape_b)) return;

    const b2BodyId body_a = b2Shape_GetBody(shape_a);
    const b2BodyId body_b = b2Shape_GetBody(shape_b);
    if (!b2Body_IsValid(body_a) || !b2Body_IsValid(body_b)) return;

    const b2BodyType type_a = b2Body_GetType(body_a);
    const b2BodyType type_b = b2Body_GetType(body_b);

    // Only filter contacts between a CharacterBody2D (Kinematic) and a Dynamic body.
    b2BodyId kin_body = b2_nullBodyId;
    b2Vec2 normal_into_char = {0.0f, 0.0f};
    if (type_a == b2_kinematicBody && type_b == b2_dynamicBody) {
        kin_body = body_a;
        normal_into_char = {-manifold->normal.x, -manifold->normal.y};
    } else if (type_b == b2_kinematicBody && type_a == b2_dynamicBody) {
        kin_body = body_b;
        normal_into_char = manifold->normal;
    } else {
        return;
    }

    auto* self = static_cast<PhysicsWorld*>(ctx);
    const auto k_handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(kin_body)));
    auto it = self->bodies_.find(k_handle);
    if (it == self->bodies_.end() || !it->second.is_character) return;

    // When a dynamic body is a floor beneath the character's feet, move_and_slide() already applies
    // the character's finite weight force; disable the infinite-mass kinematic solver contact so
    // the character does not crush or pinch jointed dynamic floors (bridges, springs, vehicles).
    if (dot(to_v2(normal_into_char), it->second.up_direction) >= it->second.floor_cos) {
        manifold->pointCount = 0;
    }
}

void PhysicsWorld::destroy_world() {
    for (PendingSensorExit& exit : pending_exits_) pluma::release(exit.visitor);
    pending_exits_.clear();

    for (auto& [h, j] : joints_) {
        pluma::release(j.owner);
    }
    joints_.clear();

    for (auto& [h, s] : shapes_) {
        pluma::release(s.owner);
    }
    shapes_.clear();

    for (auto& [h, b] : bodies_) {
        pluma::release(b.owner);
    }
    bodies_.clear();

    if (b2World_IsValid(world_id_)) {
        b2DestroyWorld(world_id_);
    }
    world_id_ = b2_nullWorldId;
}

void PhysicsWorld::set_sleep_enabled(bool enabled) {
    sleep_enabled_ = enabled;
    if (b2World_IsValid(world_id_)) b2World_EnableSleeping(world_id_, enabled);
}

void PhysicsWorld::clear() {
    destroy_world();
    init_world();
}

void PhysicsWorld::remap_owners(const std::function<pluma::Value(const pluma::Value&)>& migrate) {
    const auto remap = [&](pluma::Value& v) {
        if (!v.is_obj()) return;
        const pluma::Value moved = migrate(v);
        pluma::release(v);
        v = moved;
    };
    for (auto& [handle, body] : bodies_) remap(body.owner);
    for (auto& [handle, shape] : shapes_) remap(shape.owner);
    for (auto& [handle, joint] : joints_) remap(joint.owner);
    for (PendingSensorExit& exit : pending_exits_) remap(exit.visitor);
}

void PhysicsWorld::set_gravity(vec2 g) {
    if (!is_finite(g)) return;
    gravity_ = g;
    if (b2World_IsValid(world_id_)) {
        b2World_SetGravity(world_id_, to_b2(g));
    }
}

void PhysicsWorld::set_worker_count(unsigned count) {
    if (worker_count_ == count) return;
    worker_count_ = count;
    clear();
}

uint32_t PhysicsWorld::create_body(const BodyConfig& config, const pluma::Value& owner) {
    if (!b2World_IsValid(world_id_)) return 0;

    b2BodyDef def = b2DefaultBodyDef();
    switch (config.kind) {
    case BodyKind::Static: def.type = b2_staticBody; break;
    case BodyKind::Kinematic: def.type = b2_kinematicBody; break;
    case BodyKind::Dynamic: def.type = b2_dynamicBody; break;
    }

    def.position = is_finite(config.position) ? to_b2(config.position) : b2Vec2{0.0f, 0.0f};
    def.rotation = b2MakeRot(is_finite(config.rotation) ? config.rotation : 0.0f);
    def.linearVelocity = is_finite(config.linear_velocity) ? to_b2(config.linear_velocity) : b2Vec2{0.0f, 0.0f};
    def.angularVelocity = is_finite(config.angular_velocity) ? config.angular_velocity : 0.0f;
    def.linearDamping = is_finite(config.linear_damp) ? std::max(0.0f, config.linear_damp) : 0.0f;
    def.angularDamping = is_finite(config.angular_damp) ? std::max(0.0f, config.angular_damp) : 0.0f;
    def.gravityScale = is_finite(config.gravity_scale) ? config.gravity_scale : 1.0f;
    def.motionLocks.angularZ = config.lock_rotation;
    def.isBullet = config.continuous_cd;
    def.enableSleep = config.can_sleep;

    const uint32_t handle = next_body_handle_++;
    def.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));

    const b2BodyId id = b2CreateBody(world_id_, &def);
    if (!b2Body_IsValid(id)) return 0;

    BodyRecord rec;
    rec.handle = handle;
    rec.id = id;
    rec.kind = config.kind;
    rec.owner = owner;
    rec.mass = (is_finite(config.mass) && config.mass > 0.0f) ? config.mass : 1.0f;
    rec.layer = config.collision_layer;
    rec.mask = config.collision_mask;
    pluma::retain(rec.owner);

    bodies_.emplace(handle, std::move(rec));
    return handle;
}

void PhysicsWorld::destroy_body(uint32_t handle) {
    auto it = bodies_.find(handle);
    if (it == bodies_.end()) return;

    queue_sensor_exits(handle, 0);

    // Clean up joints connected to this body
    std::vector<uint32_t> dead_joints;
    for (const auto& [j_handle, j_rec] : joints_) {
        if (j_rec.body_a == handle || j_rec.body_b == handle) {
            dead_joints.push_back(j_handle);
        }
    }
    for (uint32_t j_handle : dead_joints) {
        destroy_joint(j_handle);
    }

    for (uint32_t s_handle : it->second.shapes) {
        auto sit = shapes_.find(s_handle);
        if (sit != shapes_.end()) {
            pluma::release(sit->second.owner);
            shapes_.erase(sit);
        }
    }

    if (b2Body_IsValid(it->second.id)) {
        b2DestroyBody(it->second.id);
    }
    pluma::release(it->second.owner);
    bodies_.erase(it);
}

bool PhysicsWorld::is_body_valid(uint32_t handle) const {
    auto it = bodies_.find(handle);
    return it != bodies_.end() && b2Body_IsValid(it->second.id);
}

pluma::Value PhysicsWorld::get_body_owner(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end()) return it->second.owner;
    return pluma::Value{};
}

void PhysicsWorld::set_body_transform(uint32_t handle, vec2 position, float rotation) {
    if (!is_finite(position) || !is_finite(rotation)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Body_SetTransform(it->second.id, to_b2(position), b2MakeRot(rotation));
        it->second.transform_synced = true;
    }
}

vec2 PhysicsWorld::get_body_position(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Vec2 p = b2Body_GetPosition(it->second.id);
        return {p.x, p.y};
    }
    return {0.0f, 0.0f};
}

float PhysicsWorld::get_body_rotation(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Rot r = b2Body_GetRotation(it->second.id);
        return b2Rot_GetAngle(r);
    }
    return 0.0f;
}

void PhysicsWorld::set_body_linear_velocity(uint32_t handle, vec2 vel) {
    if (!is_finite(vel)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Body_SetLinearVelocity(it->second.id, to_b2(vel));
    }
}

vec2 PhysicsWorld::get_body_linear_velocity(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Vec2 v = b2Body_GetLinearVelocity(it->second.id);
        return {v.x, v.y};
    }
    return {0.0f, 0.0f};
}

void PhysicsWorld::set_body_angular_velocity(uint32_t handle, float w) {
    if (!is_finite(w)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Body_SetAngularVelocity(it->second.id, w);
    }
}

float PhysicsWorld::get_body_angular_velocity(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        return b2Body_GetAngularVelocity(it->second.id);
    }
    return 0.0f;
}

void PhysicsWorld::apply_force(uint32_t handle, vec2 force) {
    if (!is_finite(force)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id) && it->second.kind == BodyKind::Dynamic) {
        b2Body_ApplyForceToCenter(it->second.id, to_b2(force), true);
    }
}

void PhysicsWorld::apply_impulse(uint32_t handle, vec2 impulse) {
    if (!is_finite(impulse)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id) && it->second.kind == BodyKind::Dynamic) {
        b2Body_ApplyLinearImpulseToCenter(it->second.id, to_b2(impulse), true);
    }
}

void PhysicsWorld::apply_torque(uint32_t handle, float torque) {
    if (!is_finite(torque)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id) && it->second.kind == BodyKind::Dynamic) {
        b2Body_ApplyTorque(it->second.id, torque, true);
    }
}

void PhysicsWorld::apply_angular_impulse(uint32_t handle, float impulse) {
    if (!is_finite(impulse)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id) && it->second.kind == BodyKind::Dynamic) {
        b2Body_ApplyAngularImpulse(it->second.id, impulse, true);
    }
}

void PhysicsWorld::set_body_mass(uint32_t handle, float mass) {
    if (!is_finite(mass) || mass <= 0.0f) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        it->second.mass = mass;
        if (it->second.kind == BodyKind::Dynamic) {
            b2MassData md = b2Body_GetMassData(it->second.id);
            if (md.mass <= 0.0f && !it->second.shapes.empty()) {
                b2Body_UpdateMassFromShapes(it->second.id);
                md = b2Body_GetMassData(it->second.id);
            }
            if (md.mass > 0.0f) {
                const float scale = mass / md.mass;
                md.rotationalInertia *= scale;
                md.mass = mass;
                b2Body_SetMassData(it->second.id, md);
            }
        }
    }
}

void PhysicsWorld::set_body_gravity_scale(uint32_t handle, float scale) {
    if (!is_finite(scale)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Body_SetGravityScale(it->second.id, scale);
    }
}

void PhysicsWorld::set_body_motion_locks(uint32_t handle, bool lock_rotation) {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2MotionLocks locks = {false, false, lock_rotation};
        b2Body_SetMotionLocks(it->second.id, locks);
        if (it->second.kind == BodyKind::Dynamic && it->second.mass > 0.0f) {
            b2MassData md = b2Body_GetMassData(it->second.id);
            if (!lock_rotation && md.rotationalInertia <= 0.0f && !it->second.shapes.empty()) {
                b2Body_UpdateMassFromShapes(it->second.id);
                md = b2Body_GetMassData(it->second.id);
            }
            if (md.mass > 0.0f) {
                const float scale = it->second.mass / md.mass;
                md.rotationalInertia *= scale;
                md.mass = it->second.mass;
                b2Body_SetMassData(it->second.id, md);
            }
        }
    }
}

void PhysicsWorld::set_body_damp(uint32_t handle, float linear_damp, float angular_damp) {
    if (!is_finite(linear_damp) || !is_finite(angular_damp)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Body_SetLinearDamping(it->second.id, std::max(0.0f, linear_damp));
        b2Body_SetAngularDamping(it->second.id, std::max(0.0f, angular_damp));
    }
}

void PhysicsWorld::apply_force_at_point(uint32_t handle, vec2 force, vec2 point) {
    if (!is_finite(force) || !is_finite(point)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id) && it->second.kind == BodyKind::Dynamic) {
        b2Body_ApplyForce(it->second.id, to_b2(force), to_b2(point), true);
    }
}

void PhysicsWorld::apply_impulse_at_point(uint32_t handle, vec2 impulse, vec2 point) {
    if (!is_finite(impulse) || !is_finite(point)) return;
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id) && it->second.kind == BodyKind::Dynamic) {
        b2Body_ApplyLinearImpulse(it->second.id, to_b2(impulse), to_b2(point), true);
    }
}

void PhysicsWorld::set_body_bullet(uint32_t handle, bool bullet) {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Body_SetBullet(it->second.id, bullet);
    }
}

bool PhysicsWorld::is_body_bullet(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        return b2Body_IsBullet(it->second.id);
    }
    return false;
}

void PhysicsWorld::set_body_can_sleep(uint32_t handle, bool can_sleep) {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Body_EnableSleeping(it->second.id, can_sleep);
    }
}

bool PhysicsWorld::is_body_can_sleep(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        return b2Body_IsSleepingEnabled(it->second.id);
    }
    return false;
}

void PhysicsWorld::set_body_awake(uint32_t handle, bool awake) {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        b2Body_SetAwake(it->second.id, awake);
    }
}

bool PhysicsWorld::is_body_awake(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end() && b2Body_IsValid(it->second.id)) {
        return b2Body_IsAwake(it->second.id);
    }
    return false;
}

void PhysicsWorld::set_body_constant_linear_velocity(uint32_t handle, vec2 vel) {
    if (!is_finite(vel)) return;
    auto it = bodies_.find(handle);
    if (it == bodies_.end()) return;
    it->second.constant_velocity = vel;
    for (uint32_t s_handle : it->second.shapes) {
        auto sit = shapes_.find(s_handle);
        if (sit == shapes_.end()) continue;
        if (b2Chain_IsValid(sit->second.chain_id)) {
            b2SurfaceMaterial mat = b2DefaultSurfaceMaterial();
            mat.friction = sit->second.config.friction;
            mat.restitution = sit->second.config.restitution;
            mat.tangentSpeed = -vel.x();
            b2Chain_SetSurfaceMaterial(sit->second.chain_id, &mat, 0);
        } else if (b2Shape_IsValid(sit->second.id)) {
            b2SurfaceMaterial mat = b2Shape_GetSurfaceMaterial(sit->second.id);
            mat.tangentSpeed = -vel.x();
            b2Shape_SetSurfaceMaterial(sit->second.id, &mat);
        }
    }
}

vec2 PhysicsWorld::get_body_constant_linear_velocity(uint32_t handle) const {
    auto it = bodies_.find(handle);
    if (it != bodies_.end()) return it->second.constant_velocity;
    return {0.0f, 0.0f};
}

// ---------------------------------------------------------------- snapshots

struct PhysicsWorld::State {
    std::vector<std::uint8_t> image; // b2World_GetSnapshot
    vec2 gravity{};
    std::uint64_t step_count = 0;
    uint32_t next_body = 1, next_shape = 1, next_joint = 1;
    bool sleep_enabled = false;
    std::map<uint32_t, BodyRecord> bodies;
    std::map<uint32_t, ShapeRecord> shapes;
    std::map<uint32_t, JointRecord> joints;
    std::vector<PendingSensorExit> pending;
};

void PhysicsWorld::StateDeleter::operator()(State* s) const { delete s; }

PhysicsWorld::StatePtr PhysicsWorld::save_state(pluma::SnapshotWriter& w) const {
    const int size = b2World_GetSnapshot(world_id_, nullptr, 0);
    if (size <= 0) {
        w.fail("the physics world cannot be saved (mid-step?)");
        return nullptr;
    }
    StatePtr s(new State);
    s->image.resize(static_cast<std::size_t>(size));
    if (b2World_GetSnapshot(world_id_, s->image.data(), size) != size) {
        w.fail("the physics world snapshot failed");
        return nullptr;
    }
    s->gravity = gravity_;
    s->step_count = step_count_;
    s->next_body = next_body_handle_;
    s->next_shape = next_shape_handle_;
    s->next_joint = next_joint_handle_;
    s->sleep_enabled = sleep_enabled_;
    s->bodies = bodies_;
    s->shapes = shapes_;
    s->joints = joints_;
    s->pending = pending_exits_;
    for (const auto& [h, b] : bodies_) w.value(b.owner);
    for (const auto& [h, sh] : shapes_) w.value(sh.owner);
    for (const auto& [h, j] : joints_) w.value(j.owner);
    for (const PendingSensorExit& e : pending_exits_) w.value(e.visitor);
    return s;
}

void PhysicsWorld::drop_owners() {
    // Releasing an owner can free a node whose finalizer destroys bodies (editing the maps),
    // so the references are collected first and released after the loops.
    std::vector<pluma::Value> held;
    const auto take = [&held](pluma::Value& v) {
        if (v.is_obj()) held.push_back(v);
        v = pluma::Value{};
    };
    for (auto& [h, b] : bodies_) take(b.owner);
    for (auto& [h, sh] : shapes_) take(sh.owner);
    for (auto& [h, j] : joints_) take(j.owner);
    for (PendingSensorExit& e : pending_exits_) take(e.visitor);
    for (const pluma::Value& v : held) pluma::release(v);
}

bool PhysicsWorld::restore_state(const State& s, pluma::SnapshotReader& r) {
    if (!b2World_RestoreFromSnapshot(world_id_, s.image.data(), static_cast<int>(s.image.size()))) {
        r.fail("the physics world could not be restored");
        return false;
    }
    gravity_ = s.gravity;
    step_count_ = s.step_count;
    next_body_handle_ = s.next_body;
    next_shape_handle_ = s.next_shape;
    next_joint_handle_ = s.next_joint;
    set_sleep_enabled(s.sleep_enabled);
    bodies_ = s.bodies;
    shapes_ = s.shapes;
    joints_ = s.joints;
    pending_exits_ = s.pending;
    for (auto& [h, b] : bodies_) b.owner = r.value();
    for (auto& [h, sh] : shapes_) sh.owner = r.value();
    for (auto& [h, j] : joints_) j.owner = r.value();
    for (PendingSensorExit& e : pending_exits_) e.visitor = r.value();
    // The ids name this world by its slot in Box2D's world table, which differs from the world
    // the snapshot came from when it is restored into another host.
    const auto world0 = static_cast<uint16_t>(world_id_.index1 - 1);
    for (auto& [h, b] : bodies_) b.id.world0 = world0;
    for (auto& [h, sh] : shapes_) {
        sh.id.world0 = world0;
        sh.chain_id.world0 = world0;
    }
    for (auto& [h, j] : joints_) j.id.world0 = world0;
    // Box2D clears user data on restore; the handles are how the queries find our records.
    const auto tag = [](uint32_t handle) { return reinterpret_cast<void*>(static_cast<uintptr_t>(handle)); };
    for (const auto& [h, b] : bodies_) b2Body_SetUserData(b.id, tag(h));
    for (const auto& [h, sh] : shapes_) {
        if (b2Shape_IsValid(sh.id)) b2Shape_SetUserData(sh.id, tag(h));
        if (b2Chain_IsValid(sh.chain_id)) {
            const int n = b2Chain_GetSegmentCount(sh.chain_id);
            std::vector<b2ShapeId> segs(static_cast<std::size_t>(std::max(n, 0)));
            if (n > 0) b2Chain_GetSegments(sh.chain_id, segs.data(), n);
            for (b2ShapeId seg : segs) b2Shape_SetUserData(seg, tag(h));
        }
    }
    for (const auto& [h, j] : joints_) b2Joint_SetUserData(j.id, tag(h));
    return r.ok();
}

void PhysicsWorld::hash_deep(StateHasher& h) const {
    h.value(state_hash());
    // Box2D's deep state hash (bodies, contacts with their warm-start impulses, joints, islands).
    // Not the raw snapshot image: that holds array capacities, which differ after a restore.
    h.value(b2World_GetStateHash(world_id_));
    h.value(gravity_);
    h.value(step_count_);
    h.value(next_body_handle_);
    h.value(next_shape_handle_);
    h.value(next_joint_handle_);
    h.value(sleep_enabled_);
    for (const auto& [handle, b] : bodies_) {
        h.value(handle);
        h.value(static_cast<std::uint8_t>(b.kind));
        h.value(b.mass);
        h.value(b.layer);
        h.value(b.mask);
        h.value(b.floor_velocity);
        h.value(b.constant_velocity);
        h.value(b.up_direction);
        h.value(b.floor_max_angle);
        h.value(b.weight_step);
        h.value(b.floor_cos);
        h.value(b.on_floor);
        h.value(b.is_character);
        h.value(b.transform_synced);
        h.span(std::span<const uint32_t>(b.shapes));
    }
    for (const auto& [handle, sh] : shapes_) {
        h.value(handle);
        h.value(sh.body_handle);
        h.value(sh.disabled);
    }
    for (const auto& [handle, j] : joints_) {
        h.value(handle);
        h.value(j.body_a);
        h.value(j.body_b);
    }
    h.value(static_cast<std::uint64_t>(pending_exits_.size()));
    for (const PendingSensorExit& e : pending_exits_) {
        h.value(e.sensor_shape);
        h.value(e.visitor_is_area);
    }
}

} // namespace cuervo::physics

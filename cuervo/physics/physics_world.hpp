#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <map>
#include <vector>

#include <box2d/box2d.h>

#include "cuervo/math/vec.hpp"
#include "pluma/value.hpp"

namespace pluma {
class VM;
}

namespace cuervo::physics {

// The three kinds of physics bodies in Box2D v3 (DESIGN.md 4.17, milestone 9.6).
enum class BodyKind : uint8_t {
    Static,    // Fixed geometry (walls, floors, static platforms)
    Kinematic, // Script/code-driven moving bodies (CharacterBody2D)
    Dynamic,   // Fully simulated rigid bodies with mass, forces, and impulses (RigidBody2D)
};

// Supported primitive 2D collision shapes.
enum class ShapeKind : uint8_t {
    Circle,
    Box,
    Capsule,
    Segment,
    Polygon,
    Chain,
};

// Parameters for creating or updating a collision shape.
struct ShapeConfig {
    ShapeKind kind = ShapeKind::Box;
    vec2 offset = {0.0f, 0.0f};
    float rotation = 0.0f;
    float radius = 16.0f;
    vec2 size = {32.0f, 32.0f}; // width, height (for Box)
    float length = 20.0f;       // height / length (for Capsule)
    vec2 point_a = {-16.0f, 0.0f};
    vec2 point_b = {16.0f, 0.0f};
    std::vector<vec2> points;   // vertices for Polygon or Chain
    bool is_loop = false;       // closed loop for Chain
    float density = 1.0f;
    float friction = 0.7f;
    float restitution = 0.0f;
    bool is_sensor = false;
    uint32_t layer = 1;
    uint32_t mask = 1;
};

// Configuration for creating a physics body.
struct BodyConfig {
    BodyKind kind = BodyKind::Static;
    vec2 position = {0.0f, 0.0f};
    float rotation = 0.0f;
    float mass = 1.0f;
    float gravity_scale = 1.0f;
    vec2 linear_velocity = {0.0f, 0.0f};
    float angular_velocity = 0.0f;
    float linear_damp = 0.0f;
    float angular_damp = 0.0f;
    bool lock_rotation = false;
    bool continuous_cd = false;
    bool can_sleep = true;
    uint32_t collision_layer = 1;
    uint32_t collision_mask = 1;
};

// Result of a 2D raycast query.
struct RaycastHit {
    bool hit = false;
    vec2 point = {0.0f, 0.0f};
    vec2 normal = {0.0f, 0.0f};
    float fraction = 0.0f;
    uint32_t body_handle = 0;
    uint32_t shape_handle = 0;
    pluma::Value collider;
};

// Result of a 2D shape cast query.
struct ShapeCastHit {
    bool hit = false;
    vec2 point = {0.0f, 0.0f};
    vec2 normal = {0.0f, 0.0f};
    float fraction = 0.0f;
    uint32_t body_handle = 0;
    uint32_t shape_handle = 0;
    pluma::Value collider;
};

// Information about an individual collision during move_and_slide.
struct SlideCollision {
    vec2 point = {0.0f, 0.0f};
    vec2 normal = {0.0f, 0.0f};
    vec2 travel = {0.0f, 0.0f};
    vec2 remainder = {0.0f, 0.0f};
    uint32_t collider_body = 0;
    pluma::Value collider;
};

// Return structure for CharacterBody2D::move_and_slide.
struct SlideResult {
    vec2 position = {0.0f, 0.0f};
    vec2 velocity = {0.0f, 0.0f};
    bool on_floor = false;
    bool on_wall = false;
    bool on_ceiling = false;
    vec2 floor_normal = {0.0f, 0.0f};
    vec2 wall_normal = {0.0f, 0.0f};
    vec2 floor_velocity = {0.0f, 0.0f};
    bool collided = false;
    std::vector<SlideCollision> collisions;
};

// Native 2D physics simulation world backed by Box2D v3 (DESIGN.md 4.17, milestone 9.6).
// Handles rigid bodies, kinematic characters, collision shapes, raycasts, joints,
// sensor events (Area2D), and deterministic simulation across worker counts.
class PhysicsWorld {
public:
    explicit PhysicsWorld(unsigned worker_count = 1, vec2 gravity = {0.0f, 980.0f});
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Drops all bodies, shapes, and joints, resetting the simulation world.
    void clear();
    // Hot reload (DESIGN.md 5.13): replaces every script object the world
    // holds (body, shape and joint owners, pending sensor visitors) with
    // `migrate(old)`, an owned reference into the new VM, and releases the
    // old one. Without it the world keeps calling methods on the old VM's
    // objects after the reload.
    void remap_owners(const std::function<pluma::Value(const pluma::Value&)>& migrate);

    b2WorldId world_id() const { return world_id_; }

    vec2 gravity() const { return gravity_; }
    void set_gravity(vec2 g);

    unsigned worker_count() const { return worker_count_; }
    void set_worker_count(unsigned count);

    // --- Body Management
    std::size_t body_count() const { return bodies_.size(); }
    std::size_t shape_count() const { return shapes_.size(); }
    std::size_t joint_count() const { return joints_.size(); }
    uint32_t create_body(const BodyConfig& config, const pluma::Value& owner);
    void destroy_body(uint32_t handle);
    bool is_body_valid(uint32_t handle) const;
    pluma::Value get_body_owner(uint32_t handle) const;

    void set_body_transform(uint32_t handle, vec2 position, float rotation);
    vec2 get_body_position(uint32_t handle) const;
    float get_body_rotation(uint32_t handle) const;

    void set_body_linear_velocity(uint32_t handle, vec2 vel);
    vec2 get_body_linear_velocity(uint32_t handle) const;
    void set_body_angular_velocity(uint32_t handle, float w);
    float get_body_angular_velocity(uint32_t handle) const;

    void apply_force(uint32_t handle, vec2 force);
    void apply_impulse(uint32_t handle, vec2 impulse);
    void apply_force_at_point(uint32_t handle, vec2 force, vec2 point);
    void apply_impulse_at_point(uint32_t handle, vec2 impulse, vec2 point);
    void apply_torque(uint32_t handle, float torque);
    void apply_angular_impulse(uint32_t handle, float impulse);

    void set_body_mass(uint32_t handle, float mass);
    void set_body_gravity_scale(uint32_t handle, float scale);
    void set_body_motion_locks(uint32_t handle, bool lock_rotation);
    void set_body_damp(uint32_t handle, float linear_damp, float angular_damp);
    void set_body_bullet(uint32_t handle, bool bullet);
    bool is_body_bullet(uint32_t handle) const;
    void set_body_can_sleep(uint32_t handle, bool can_sleep);
    bool is_body_can_sleep(uint32_t handle) const;
    void set_body_awake(uint32_t handle, bool awake);
    bool is_body_awake(uint32_t handle) const;
    void set_body_constant_linear_velocity(uint32_t handle, vec2 vel);
    vec2 get_body_constant_linear_velocity(uint32_t handle) const;

    // --- Shape Management
    uint32_t add_shape(uint32_t body_handle, const ShapeConfig& config, const pluma::Value& owner);
    void destroy_shape(uint32_t shape_handle);
    void set_shape_filter(uint32_t shape_handle, uint32_t layer, uint32_t mask);
    void set_shape_disabled(uint32_t shape_handle, bool disabled);
    void set_shape_material(uint32_t shape_handle, float friction, float restitution);

    // --- Joints
    uint32_t create_pin_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, float softness,
                              bool limit_enabled, float lower_limit, float upper_limit,
                              bool motor_enabled, float motor_speed, float max_motor_torque,
                              bool collide_connected, const pluma::Value& owner);
    uint32_t create_pin_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, float softness,
                              bool limit_enabled, float lower_limit, float upper_limit,
                              bool motor_enabled, float motor_speed, float max_motor_torque,
                              const pluma::Value& owner) {
        return create_pin_joint(body_a, body_b, anchor, softness, limit_enabled, lower_limit, upper_limit,
                                motor_enabled, motor_speed, max_motor_torque, false, owner);
    }

    uint32_t create_distance_joint(uint32_t body_a, uint32_t body_b, vec2 anchor_a, vec2 anchor_b,
                                  float length, float min_length, float max_length,
                                  float stiffness, float damping,
                                  bool collide_connected, const pluma::Value& owner);
    uint32_t create_distance_joint(uint32_t body_a, uint32_t body_b, vec2 anchor_a, vec2 anchor_b,
                                  float length, float min_length, float max_length,
                                  float stiffness, float damping, const pluma::Value& owner) {
        return create_distance_joint(body_a, body_b, anchor_a, anchor_b, length, min_length, max_length,
                                     stiffness, damping, false, owner);
    }

    uint32_t create_prismatic_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, vec2 axis,
                                    bool enable_spring, float hertz, float damping_ratio, float target_translation,
                                    bool enable_limit, float lower_translation, float upper_translation,
                                    bool enable_motor, float max_motor_force, float motor_speed,
                                    bool collide_connected, const pluma::Value& owner);

    uint32_t create_weld_joint(uint32_t body_a, uint32_t body_b, vec2 anchor,
                               float linear_hertz, float angular_hertz,
                               float linear_damping_ratio, float angular_damping_ratio,
                               bool collide_connected, const pluma::Value& owner);

    uint32_t create_wheel_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, vec2 axis,
                                bool enable_spring, float hertz, float damping_ratio,
                                bool enable_limit, float lower_translation, float upper_translation,
                                bool enable_motor, float max_motor_torque, float motor_speed,
                                bool collide_connected, const pluma::Value& owner);

    uint32_t create_motor_joint(uint32_t body_a, uint32_t body_b,
                                vec2 linear_offset, float angular_offset,
                                float max_velocity_force, float max_velocity_torque,
                                float linear_hertz, float linear_damping_ratio, float max_spring_force,
                                float angular_hertz, float angular_damping_ratio, float max_spring_torque,
                                bool collide_connected, const pluma::Value& owner);

    uint32_t create_mover_joint(uint32_t body_a, uint32_t body_b,
                                vec2 linear_velocity, vec2 max_velocity_force,
                                bool collide_connected, const pluma::Value& owner);

    uint32_t create_pogo_joint(uint32_t body_a, uint32_t body_b, vec2 anchor, vec2 normal,
                               float hertz, float damping_ratio, float rest_length,
                               float max_tension_force, float max_compression_force,
                               bool collide_connected, const pluma::Value& owner);

    uint32_t create_filter_joint(uint32_t body_a, uint32_t body_b, const pluma::Value& owner);

    void destroy_joint(uint32_t handle);

    void joint_set_motor_speed(uint32_t handle, float speed);
    void joint_set_motor_enabled(uint32_t handle, bool enabled);
    vec2 joint_get_reaction_force(uint32_t handle) const;
    float joint_get_reaction_torque(uint32_t handle) const;

    // Box2D sleeping is a world-wide setting and stays off unless a game opts in, so determinism does not
    // depend on which body happened to ask for it. set_body_can_sleep() only takes effect once it is on.
    void set_sleep_enabled(bool enabled);
    bool sleep_enabled() const { return sleep_enabled_; }

    bool debug_shapes() const { return debug_shapes_; }
    void set_debug_shapes(bool enabled) { debug_shapes_ = enabled; }
    bool editor_debug_shapes() const { return editor_debug_shapes_; }
    void set_editor_debug_shapes(bool enabled) { editor_debug_shapes_ = enabled; }

    // --- Queries & Kinematics
    RaycastHit cast_ray(vec2 origin, vec2 translation, uint32_t mask, uint32_t exclude_body = 0);
    ShapeCastHit cast_shape(const ShapeConfig& config, vec2 origin, vec2 translation, uint32_t mask, uint32_t exclude_body = 0);
    uint32_t query_point(vec2 point, uint32_t mask, pluma::Value* collider_out = nullptr);
    void explode(vec2 center, float radius, float falloff, float impulse_per_length, uint32_t mask);

    SlideResult move_and_slide(uint32_t body_handle, vec2 velocity, float dt,
                               vec2 up_direction, float floor_max_angle, int max_slides);

    // Step simulation by fixed dt, updating dynamic body positions and firing sensor events.
    void step(float dt, pluma::VM* vm = nullptr);

    // Deterministic state digest across worker counts.
    std::uint64_t state_hash() const;

private:
    struct BodyRecord {
        uint32_t handle = 0;
        b2BodyId id = b2_nullBodyId;
        BodyKind kind = BodyKind::Static;
        pluma::Value owner;
        std::vector<uint32_t> shapes;
        float mass = 1.0f;
        uint32_t layer = 1;
        uint32_t mask = 1;
        vec2 floor_velocity = {0.0f, 0.0f};
        vec2 constant_velocity = {0.0f, 0.0f};
        vec2 up_direction = {0.0f, -1.0f};
        float floor_max_angle = 0.785398f;
        std::uint64_t weight_step = ~std::uint64_t{0}; // step_count_ when the character last weighed on a floor
        float floor_cos = 0.70710678f; // cached cos(floor_max_angle); refreshed by move_and_slide
        bool on_floor = false;
        bool is_character = false;
        bool transform_synced = false;
    };

    struct ShapeRecord {
        uint32_t handle = 0;
        b2ShapeId id = b2_nullShapeId;
        b2ChainId chain_id = b2_nullChainId;
        uint32_t body_handle = 0;
        ShapeConfig config;
        pluma::Value owner;
        bool disabled = false;
    };

    struct JointRecord {
        uint32_t handle = 0;
        b2JointId id = b2_nullJointId;
        uint32_t body_a = 0;
        uint32_t body_b = 0;
        pluma::Value owner;
    };

    // An exit owed to an Area2D because the visitor was destroyed while overlapping its sensor.
    struct PendingSensorExit {
        uint32_t sensor_shape = 0;
        pluma::Value visitor; // retained until delivered
        bool visitor_is_area = false;
    };

    uint32_t register_joint(b2JointId jid, uint32_t handle, uint32_t body_a, uint32_t body_b,
                            const pluma::Value& owner);
    void queue_sensor_exits(uint32_t body_handle, uint32_t shape_handle);
    void flush_sensor_exits(pluma::VM* vm);
    static void call_sensor_hook(pluma::VM* vm, const pluma::Value& area, const pluma::Value& visitor,
                                 bool visitor_is_area, bool entered);

    void init_world();
    void destroy_world();
    static void pre_solve_callback(b2ShapeId shape_a, b2ShapeId shape_b, b2Manifold* manifold, void* ctx);

    b2WorldId world_id_ = b2_nullWorldId;
    unsigned worker_count_ = 1;
    vec2 gravity_ = {0.0f, 980.0f};

    std::uint64_t step_count_ = 0;
    uint32_t next_body_handle_ = 1;
    uint32_t next_shape_handle_ = 1;
    uint32_t next_joint_handle_ = 1;

    std::map<uint32_t, BodyRecord> bodies_;
    std::map<uint32_t, ShapeRecord> shapes_;
    std::map<uint32_t, JointRecord> joints_;

    std::vector<PendingSensorExit> pending_exits_;
    bool sleep_enabled_ = false;
    bool debug_shapes_ = false;
    bool editor_debug_shapes_ = true;
};

} // namespace cuervo::physics

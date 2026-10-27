#include "cuervo/script/physics_bindings.hpp"

#include "cuervo/physics/physics_world.hpp"
#include "cuervo/script/script_host.hpp"
#include "pluma/bindings.hpp"
#include "pluma/vm.hpp"

namespace cuervo::script {

namespace {

vec2 to_vec2(const float* d) { return {d[0], d[1]}; }

int get_field_index(const pluma::RecordRT* rt, std::string_view name) {
    if (rt == nullptr) return -1;
    for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
        if (rt->field_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

pluma::Value get_field_value(const pluma::Value& obj, std::string_view name) {
    if (!obj.is_obj()) return pluma::Value{};
    auto* inst = static_cast<pluma::InstanceObj*>(obj.o);
    int idx = get_field_index(inst->type, name);
    if (idx >= 0 && static_cast<std::size_t>(idx) < inst->fields.size()) {
        return inst->fields[idx];
    }
    return pluma::Value{};
}

physics::ShapeKind primitive_shape_kind(int kind) {
    switch (kind) {
    case 0: return physics::ShapeKind::Circle;
    case 1: return physics::ShapeKind::Box;
    case 2: return physics::ShapeKind::Capsule;
    case 3: return physics::ShapeKind::Segment;
    default: return physics::ShapeKind::Box;
    }
}

// A vec2 field of a script object, or `fallback` when the field is missing or not a 2-vector.
vec2 vec2_field(const pluma::Value& obj, std::string_view name, vec2 fallback) {
    const pluma::Value v = get_field_value(obj, name);
    return pluma::is_vec(v) && pluma::vec_size(v) == 2 ? to_vec2(pluma::vec_data(v)) : fallback;
}

} // namespace

void bind_physics(pluma::Bindings& b, ScriptHost& host) {
    namespace nv = pluma::native;
    using pluma::NativeCall;
    using pluma::NumKind;
    using pluma::Value;

    b.add_native("_Physics.body_create", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        physics::BodyConfig config;
        const auto kind_int = static_cast<int>(nv::integer(c, 0));
        switch (kind_int) {
        case 0: config.kind = physics::BodyKind::Static; break;
        case 1: config.kind = physics::BodyKind::Kinematic; break;
        case 2: config.kind = physics::BodyKind::Dynamic; break;
        default: config.kind = physics::BodyKind::Static; break;
        }
        config.position = to_vec2(nv::vec(c, 1));
        config.rotation = nv::f32(c, 2);
        config.collision_layer = static_cast<uint32_t>(nv::integer(c, 3));
        config.collision_mask = static_cast<uint32_t>(nv::integer(c, 4));

        const uint32_t handle = world.create_body(config, c.args[5]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.body_destroy", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.destroy_body(static_cast<uint32_t>(nv::integer(c, 0)));
        return Value{};
    });

    b.add_native("_Physics.body_set_transform", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_transform(static_cast<uint32_t>(nv::integer(c, 0)),
                                 to_vec2(nv::vec(c, 1)), nv::f32(c, 2));
        return Value{};
    });

    b.add_native("_Physics.body_get_position", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const vec2 p = world.get_body_position(static_cast<uint32_t>(nv::integer(c, 0)));
        return nv::vec2_result(p.x(), p.y());
    });

    b.add_native("_Physics.body_get_rotation", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const float r = world.get_body_rotation(static_cast<uint32_t>(nv::integer(c, 0)));
        return Value::f32(r);
    });

    b.add_native("_Physics.body_set_linear_velocity", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_linear_velocity(static_cast<uint32_t>(nv::integer(c, 0)), to_vec2(nv::vec(c, 1)));
        return Value{};
    });

    b.add_native("_Physics.body_get_linear_velocity", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const vec2 v = world.get_body_linear_velocity(static_cast<uint32_t>(nv::integer(c, 0)));
        return nv::vec2_result(v.x(), v.y());
    });

    b.add_native("_Physics.body_set_angular_velocity", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_angular_velocity(static_cast<uint32_t>(nv::integer(c, 0)), nv::f32(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_get_angular_velocity", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const float w = world.get_body_angular_velocity(static_cast<uint32_t>(nv::integer(c, 0)));
        return Value::f32(w);
    });

    b.add_native("_Physics.body_apply_force", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.apply_force(static_cast<uint32_t>(nv::integer(c, 0)), to_vec2(nv::vec(c, 1)));
        return Value{};
    });

    b.add_native("_Physics.body_apply_impulse", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.apply_impulse(static_cast<uint32_t>(nv::integer(c, 0)), to_vec2(nv::vec(c, 1)));
        return Value{};
    });

    b.add_native("_Physics.body_apply_torque", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.apply_torque(static_cast<uint32_t>(nv::integer(c, 0)), nv::f32(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_apply_angular_impulse", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.apply_angular_impulse(static_cast<uint32_t>(nv::integer(c, 0)), nv::f32(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_set_mass", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_mass(static_cast<uint32_t>(nv::integer(c, 0)), nv::f32(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_set_gravity_scale", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_gravity_scale(static_cast<uint32_t>(nv::integer(c, 0)), nv::f32(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_set_motion_locks", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_motion_locks(static_cast<uint32_t>(nv::integer(c, 0)), nv::boolean(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_set_damp", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_damp(static_cast<uint32_t>(nv::integer(c, 0)), nv::f32(c, 1), nv::f32(c, 2));
        return Value{};
    });

    b.add_native("_Physics.body_apply_force_at_point", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.apply_force_at_point(static_cast<uint32_t>(nv::integer(c, 0)),
                                   to_vec2(nv::vec(c, 1)), to_vec2(nv::vec(c, 2)));
        return Value{};
    });

    b.add_native("_Physics.body_apply_impulse_at_point", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.apply_impulse_at_point(static_cast<uint32_t>(nv::integer(c, 0)),
                                     to_vec2(nv::vec(c, 1)), to_vec2(nv::vec(c, 2)));
        return Value{};
    });

    b.add_native("_Physics.body_set_bullet", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_bullet(static_cast<uint32_t>(nv::integer(c, 0)), nv::boolean(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_is_bullet", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        return Value::boolean(world.is_body_bullet(static_cast<uint32_t>(nv::integer(c, 0))));
    });

    b.add_native("_Physics.body_set_can_sleep", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_can_sleep(static_cast<uint32_t>(nv::integer(c, 0)), nv::boolean(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_is_can_sleep", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        return Value::boolean(world.is_body_can_sleep(static_cast<uint32_t>(nv::integer(c, 0))));
    });

    b.add_native("_Physics.body_set_awake", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_awake(static_cast<uint32_t>(nv::integer(c, 0)), nv::boolean(c, 1));
        return Value{};
    });

    b.add_native("_Physics.body_is_awake", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        return Value::boolean(world.is_body_awake(static_cast<uint32_t>(nv::integer(c, 0))));
    });

    b.add_native("_Physics.body_set_constant_velocity", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_body_constant_linear_velocity(static_cast<uint32_t>(nv::integer(c, 0)),
                                                to_vec2(nv::vec(c, 1)));
        return Value{};
    });

    auto read_vec2_list = [](const Value& list, std::vector<vec2>& points) {
        if (!list.is_obj() || list.o->kind != pluma::ObjKind::List) return;
        for (const Value& p : static_cast<const pluma::ListObj*>(list.o)->items) {
            if (pluma::is_vec(p) && pluma::vec_size(p) == 2) {
                points.push_back(to_vec2(pluma::vec_data(p)));
            }
        }
    };

    b.add_native("_Physics.shape_create", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_handle = static_cast<uint32_t>(nv::integer(c, 0));
        physics::ShapeConfig config;
        const auto kind_int = static_cast<int>(nv::integer(c, 1));
        config.kind = primitive_shape_kind(kind_int);
        config.offset = to_vec2(nv::vec(c, 2));
        config.rotation = nv::f32(c, 3);
        config.size = to_vec2(nv::vec(c, 4));
        config.point_a = config.size;
        config.point_b = to_vec2(nv::vec(c, 5));
        config.radius = nv::f32(c, 6);
        config.length = config.size.y();
        config.is_sensor = nv::boolean(c, 7);
        config.layer = static_cast<uint32_t>(nv::integer(c, 8));
        config.mask = static_cast<uint32_t>(nv::integer(c, 9));

        const uint32_t handle = world.add_shape(b_handle, config, c.args[10]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.shape_create_polygon", [&host, read_vec2_list](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_handle = static_cast<uint32_t>(nv::integer(c, 0));
        physics::ShapeConfig config;
        config.kind = physics::ShapeKind::Polygon;
        config.offset = to_vec2(nv::vec(c, 1));
        config.rotation = nv::f32(c, 2);
        read_vec2_list(c.args[3], config.points);
        config.is_sensor = nv::boolean(c, 4);
        config.layer = static_cast<uint32_t>(nv::integer(c, 5));
        config.mask = static_cast<uint32_t>(nv::integer(c, 6));

        const uint32_t handle = world.add_shape(b_handle, config, c.args[7]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.shape_create_chain", [&host, read_vec2_list](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_handle = static_cast<uint32_t>(nv::integer(c, 0));
        physics::ShapeConfig config;
        config.kind = physics::ShapeKind::Chain;
        config.offset = to_vec2(nv::vec(c, 1));
        config.rotation = nv::f32(c, 2);
        read_vec2_list(c.args[3], config.points);
        config.is_loop = nv::boolean(c, 4);
        config.is_sensor = nv::boolean(c, 5);
        config.layer = static_cast<uint32_t>(nv::integer(c, 6));
        config.mask = static_cast<uint32_t>(nv::integer(c, 7));

        const uint32_t handle = world.add_shape(b_handle, config, c.args[8]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.shape_destroy", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.destroy_shape(static_cast<uint32_t>(nv::integer(c, 0)));
        return Value{};
    });

    b.add_native("_Physics.shape_set_filter", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_shape_filter(static_cast<uint32_t>(nv::integer(c, 0)),
                               static_cast<uint32_t>(nv::integer(c, 1)),
                               static_cast<uint32_t>(nv::integer(c, 2)));
        return Value{};
    });

    b.add_native("_Physics.shape_set_disabled", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.set_shape_disabled(static_cast<uint32_t>(nv::integer(c, 0)), nv::boolean(c, 1));
        return Value{};
    });

    b.add_native("_Physics.joint_create_pin", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const vec2 anchor = to_vec2(nv::vec(c, 2));
        const float softness = nv::f32(c, 3);
        const bool limit = nv::boolean(c, 4);
        const float lower = nv::f32(c, 5);
        const float upper = nv::f32(c, 6);
        const bool motor = nv::boolean(c, 7);
        const float speed = nv::f32(c, 8);
        const float max_torque = nv::f32(c, 9);
        const bool collide_connected = nv::boolean(c, 10);

        const uint32_t handle = world.create_pin_joint(b_a, b_b, anchor, softness, limit, lower, upper,
                                                       motor, speed, max_torque, collide_connected, c.args[11]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_create_distance", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const vec2 a_a = to_vec2(nv::vec(c, 2));
        const vec2 a_b = to_vec2(nv::vec(c, 3));
        const float len = nv::f32(c, 4);
        const float min_l = nv::f32(c, 5);
        const float max_l = nv::f32(c, 6);
        const float stiff = nv::f32(c, 7);
        const float damp = nv::f32(c, 8);
        const bool collide_connected = nv::boolean(c, 9);

        const uint32_t handle = world.create_distance_joint(b_a, b_b, a_a, a_b, len, min_l, max_l,
                                                           stiff, damp, collide_connected, c.args[10]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_create_prismatic", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const vec2 anchor = to_vec2(nv::vec(c, 2));
        const vec2 axis = to_vec2(nv::vec(c, 3));
        const bool enable_spring = nv::boolean(c, 4);
        const float hertz = nv::f32(c, 5);
        const float damping = nv::f32(c, 6);
        const float target_trans = nv::f32(c, 7);
        const bool enable_limit = nv::boolean(c, 8);
        const float lower = nv::f32(c, 9);
        const float upper = nv::f32(c, 10);
        const bool enable_motor = nv::boolean(c, 11);
        const float max_force = nv::f32(c, 12);
        const float speed = nv::f32(c, 13);
        const bool collide_connected = nv::boolean(c, 14);

        const uint32_t handle = world.create_prismatic_joint(b_a, b_b, anchor, axis, enable_spring, hertz, damping,
                                                             target_trans, enable_limit, lower, upper,
                                                             enable_motor, max_force, speed, collide_connected, c.args[15]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_create_weld", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const vec2 anchor = to_vec2(nv::vec(c, 2));
        const float linear_hertz = nv::f32(c, 3);
        const float angular_hertz = nv::f32(c, 4);
        const float linear_damp = nv::f32(c, 5);
        const float angular_damp = nv::f32(c, 6);
        const bool collide_connected = nv::boolean(c, 7);

        const uint32_t handle = world.create_weld_joint(b_a, b_b, anchor, linear_hertz, angular_hertz,
                                                        linear_damp, angular_damp, collide_connected, c.args[8]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_create_wheel", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const vec2 anchor = to_vec2(nv::vec(c, 2));
        const vec2 axis = to_vec2(nv::vec(c, 3));
        const bool enable_spring = nv::boolean(c, 4);
        const float hertz = nv::f32(c, 5);
        const float damping = nv::f32(c, 6);
        const bool enable_limit = nv::boolean(c, 7);
        const float lower = nv::f32(c, 8);
        const float upper = nv::f32(c, 9);
        const bool enable_motor = nv::boolean(c, 10);
        const float max_torque = nv::f32(c, 11);
        const float speed = nv::f32(c, 12);
        const bool collide_connected = nv::boolean(c, 13);

        const uint32_t handle = world.create_wheel_joint(b_a, b_b, anchor, axis, enable_spring, hertz, damping,
                                                         enable_limit, lower, upper, enable_motor, max_torque, speed,
                                                         collide_connected, c.args[14]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_create_motor", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const vec2 linear_offset = to_vec2(nv::vec(c, 2));
        const float angular_offset = nv::f32(c, 3);
        const float max_force = nv::f32(c, 4);
        const float max_torque = nv::f32(c, 5);
        const float linear_hertz = nv::f32(c, 6);
        const float linear_damping = nv::f32(c, 7);
        const float max_spring_force = nv::f32(c, 8);
        const float angular_hertz = nv::f32(c, 9);
        const float angular_damping = nv::f32(c, 10);
        const float max_spring_torque = nv::f32(c, 11);
        const bool collide_connected = nv::boolean(c, 12);

        const uint32_t handle = world.create_motor_joint(b_a, b_b, linear_offset, angular_offset, max_force, max_torque,
                                                         linear_hertz, linear_damping, max_spring_force,
                                                         angular_hertz, angular_damping, max_spring_torque,
                                                         collide_connected, c.args[13]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_create_mover", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const vec2 linear_vel = to_vec2(nv::vec(c, 2));
        const vec2 max_force = to_vec2(nv::vec(c, 3));
        const bool collide_connected = nv::boolean(c, 4);

        const uint32_t handle = world.create_mover_joint(b_a, b_b, linear_vel, max_force, collide_connected, c.args[5]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_create_pogo", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const vec2 anchor = to_vec2(nv::vec(c, 2));
        const vec2 normal = to_vec2(nv::vec(c, 3));
        const float hertz = nv::f32(c, 4);
        const float damping = nv::f32(c, 5);
        const float rest_length = nv::f32(c, 6);
        const float max_tension = nv::f32(c, 7);
        const float max_compression = nv::f32(c, 8);
        const bool collide_connected = nv::boolean(c, 9);

        const uint32_t handle = world.create_pogo_joint(b_a, b_b, anchor, normal, hertz, damping, rest_length,
                                                        max_tension, max_compression, collide_connected, c.args[10]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_create_filter", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const auto b_a = static_cast<uint32_t>(nv::integer(c, 0));
        const auto b_b = static_cast<uint32_t>(nv::integer(c, 1));
        const uint32_t handle = world.create_filter_joint(b_a, b_b, c.args[2]);
        return Value::integer(NumKind::I32, handle);
    });

    b.add_native("_Physics.joint_destroy", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.destroy_joint(static_cast<uint32_t>(nv::integer(c, 0)));
        return Value{};
    });

    b.add_native("_Physics.joint_set_motor_speed", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.joint_set_motor_speed(static_cast<uint32_t>(nv::integer(c, 0)), nv::f32(c, 1));
        return Value{};
    });

    b.add_native("_Physics.joint_set_motor_enabled", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        world.joint_set_motor_enabled(static_cast<uint32_t>(nv::integer(c, 0)), nv::boolean(c, 1));
        return Value{};
    });

    b.add_native("_Physics.joint_get_reaction_force", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const vec2 f = world.joint_get_reaction_force(static_cast<uint32_t>(nv::integer(c, 0)));
        return pluma::vec2_value(f.x(), f.y());
    });

    b.add_native("_Physics.joint_get_reaction_torque", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        return Value::f32(world.joint_get_reaction_torque(static_cast<uint32_t>(nv::integer(c, 0))));
    });

    b.add_native("_Physics.set_debug_shapes", [&host](NativeCall& c) {
        host.physics_world().set_debug_shapes(nv::boolean(c, 0));
        return Value{};
    });

    b.add_native("_Physics.get_debug_shapes", [&host](NativeCall&) {
        return Value::boolean(host.physics_world().debug_shapes());
    });

    b.add_native("_Physics.set_editor_debug_shapes", [&host](NativeCall& c) {
        host.physics_world().set_editor_debug_shapes(nv::boolean(c, 0));
        return Value{};
    });

    b.add_native("_Physics.get_editor_debug_shapes", [&host](NativeCall&) {
        return Value::boolean(host.physics_world().editor_debug_shapes());
    });

    b.add_native("_Physics.move_and_slide", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const Value self_obj = c.args[0];
        const float dt = nv::f32(c, 1);
        if (!self_obj.is_obj()) return Value::boolean(false);

        const Value b_val = get_field_value(self_obj, "body_handle");
        const auto b_handle = static_cast<uint32_t>(b_val.i);
        const vec2 vel = vec2_field(self_obj, "velocity", vec2{0.0f, 0.0f});
        const vec2 up = vec2_field(self_obj, "up_direction", vec2{0.0f, -1.0f});
        const Value angle_val = get_field_value(self_obj, "floor_max_angle");
        const float max_angle = pluma::is_num_tag(angle_val.tag) ? angle_val.f : 0.785398f;
        const Value slides_val = get_field_value(self_obj, "max_slides");
        const int max_slides = pluma::is_num_tag(slides_val.tag) ? static_cast<int>(slides_val.i) : 4;
        const physics::SlideResult res = world.move_and_slide(b_handle, vel, dt, up, max_angle, max_slides);

        // Apply results to character node.
        const Value apply_args[8] = {
            pluma::vec2_value(res.position.x(), res.position.y()),
            pluma::vec2_value(res.velocity.x(), res.velocity.y()),
            Value::boolean(res.on_floor),
            Value::boolean(res.on_wall),
            Value::boolean(res.on_ceiling),
            pluma::vec2_value(res.floor_normal.x(), res.floor_normal.y()),
            pluma::vec2_value(res.wall_normal.x(), res.wall_normal.y()),
            pluma::vec2_value(res.floor_velocity.x(), res.floor_velocity.y()),
        };
        c.vm.call_method(self_obj, "_apply_slide_result", apply_args);

        c.vm.call_method(self_obj, "_clear_slide_collisions", {});
        for (const auto& col : res.collisions) {
            const Value col_args[5] = {
                pluma::vec2_value(col.point.x(), col.point.y()),
                pluma::vec2_value(col.normal.x(), col.normal.y()),
                pluma::vec2_value(col.travel.x(), col.travel.y()),
                pluma::vec2_value(col.remainder.x(), col.remainder.y()),
                col.collider.is_obj() ? col.collider : Value{},
            };
            c.vm.call_method(self_obj, "_add_slide_collision", col_args);
        }

        return Value::boolean(res.collided);
    });

    b.add_native("_Physics.raycast_node", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const Value node = c.args[0];
        const vec2 origin = to_vec2(nv::vec(c, 1));
        const vec2 trans = to_vec2(nv::vec(c, 2));
        const auto mask = static_cast<uint32_t>(nv::integer(c, 3));
        const auto exclude = static_cast<uint32_t>(nv::integer(c, 4));

        const physics::RaycastHit hit = world.cast_ray(origin, trans, mask, exclude);
        if (node.is_obj()) {
            const Value args[4] = {
                Value::boolean(hit.hit),
                pluma::vec2_value(hit.point.x(), hit.point.y()),
                pluma::vec2_value(hit.normal.x(), hit.normal.y()),
                hit.collider.is_obj() ? hit.collider : Value{},
            };
            c.vm.call_method(node, "_apply_raycast_result", args);
        }
        return Value{};
    });

    b.add_native("_Physics.raycast_query", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const Value res_obj = c.args[0];
        const vec2 origin = to_vec2(nv::vec(c, 1));
        const vec2 trans = to_vec2(nv::vec(c, 2));
        const auto mask = static_cast<uint32_t>(nv::integer(c, 3));

        const physics::RaycastHit hit = world.cast_ray(origin, trans, mask, 0);
        if (res_obj.is_obj()) {
            const Value args[5] = {
                Value::boolean(hit.hit),
                pluma::vec2_value(hit.point.x(), hit.point.y()),
                pluma::vec2_value(hit.normal.x(), hit.normal.y()),
                Value::f32(hit.fraction),
                hit.collider.is_obj() ? hit.collider : Value{},
            };
            c.vm.call_method(res_obj, "_set_result", args);
        }
        return Value{};
    });

    b.add_native("_Physics.shapecast_node", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const Value node = c.args[0];
        physics::ShapeConfig config;
        const auto kind_int = static_cast<int>(nv::integer(c, 1));
        config.kind = primitive_shape_kind(kind_int);
        config.offset = to_vec2(nv::vec(c, 2));
        config.rotation = nv::f32(c, 3);
        config.size = to_vec2(nv::vec(c, 4));
        config.radius = nv::f32(c, 5);
        config.length = config.size.y();

        const vec2 origin = to_vec2(nv::vec(c, 6));
        const vec2 trans = to_vec2(nv::vec(c, 7));
        const auto mask = static_cast<uint32_t>(nv::integer(c, 8));
        const auto exclude = static_cast<uint32_t>(nv::integer(c, 9));

        const physics::ShapeCastHit hit = world.cast_shape(config, origin, trans, mask, exclude);
        if (node.is_obj()) {
            const Value args[5] = {
                Value::boolean(hit.hit),
                pluma::vec2_value(hit.point.x(), hit.point.y()),
                pluma::vec2_value(hit.normal.x(), hit.normal.y()),
                Value::f32(hit.fraction),
                hit.collider.is_obj() ? hit.collider : Value{},
            };
            c.vm.call_method(node, "_apply_shapecast_result", args);
        }
        return Value{};
    });

    b.add_native("_Physics.shapecast_query", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const Value res_obj = c.args[0];
        physics::ShapeConfig config;
        const auto kind_int = static_cast<int>(nv::integer(c, 1));
        config.kind = primitive_shape_kind(kind_int);
        config.offset = to_vec2(nv::vec(c, 2));
        config.rotation = nv::f32(c, 3);
        config.size = to_vec2(nv::vec(c, 4));
        config.radius = nv::f32(c, 5);
        config.length = config.size.y();

        const vec2 origin = to_vec2(nv::vec(c, 6));
        const vec2 trans = to_vec2(nv::vec(c, 7));
        const auto mask = static_cast<uint32_t>(nv::integer(c, 8));

        const physics::ShapeCastHit hit = world.cast_shape(config, origin, trans, mask, 0);
        if (res_obj.is_obj()) {
            const Value args[5] = {
                Value::boolean(hit.hit),
                pluma::vec2_value(hit.point.x(), hit.point.y()),
                pluma::vec2_value(hit.normal.x(), hit.normal.y()),
                Value::f32(hit.fraction),
                hit.collider.is_obj() ? hit.collider : Value{},
            };
            c.vm.call_method(res_obj, "_set_result", args);
        }
        return Value{};
    });

    b.add_native("_Physics.point_query", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const vec2 pt = to_vec2(nv::vec(c, 0));
        const auto mask = static_cast<uint32_t>(nv::integer(c, 1));
        pluma::Value col;
        world.query_point(pt, mask, &col);
        return col.is_obj() ? nv::owned(col) : Value{};
    });

    b.add_native("_Physics.explode", [&host](NativeCall& c) {
        auto& world = host.physics_world();
        const vec2 center = to_vec2(nv::vec(c, 0));
        const float radius = nv::f32(c, 1);
        const float falloff = nv::f32(c, 2);
        const float impulse = nv::f32(c, 3);
        const auto mask = static_cast<uint32_t>(nv::integer(c, 4));
        world.explode(center, radius, falloff, impulse, mask);
        return Value{};
    });

    b.add_native("_Physics.get_gravity", [&host](NativeCall&) {
        const vec2 g = host.physics_world().gravity();
        return nv::vec2_result(g.x(), g.y());
    });

    b.add_native("_Physics.set_gravity", [&host](NativeCall& c) {
        host.physics_world().set_gravity(to_vec2(nv::vec(c, 0)));
        return Value{};
    });
}

} // namespace cuervo::script

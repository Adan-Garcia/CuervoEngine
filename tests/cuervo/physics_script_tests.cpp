#include "cuervo/physics/physics_world.hpp"

#include <cmath>
#include <doctest.h>

#include "cuervo/input/input_map.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/script/script_host.hpp"
#include "pluma/value.hpp"

using namespace cuervo;
using namespace cuervo::physics;

TEST_CASE("physics: script prelude nodes instantiate and run in scene tree") {
    InputMap map;
    script::HostOptions opt;
    script::ScriptHost host(opt, map);

    const std::string script = R"(
class Main extends Node2D {
    fn ready() {
        var body_a = StaticBody2D()
        body_a.name = "BodyA"
        add_child(body_a)

        var poly = CollisionPolygon2D()
        poly.polygon.push(vec2(-20.0, -20.0))
        poly.polygon.push(vec2(20.0, -20.0))
        poly.polygon.push(vec2(20.0, 20.0))
        poly.polygon.push(vec2(-20.0, 20.0))
        body_a.add_child(poly)

        var body_b = RigidBody2D()
        body_b.name = "BodyB"
        body_b.continuous_cd = true
        body_b.can_sleep = false
        add_child(body_b)

        var shape_b = CollisionShape2D()
        var box_b = RectangleShape2D()
        box_b.size = vec2(16.0, 16.0)
        shape_b.shape = box_b
        body_b.add_child(shape_b)

        var pris = PrismaticJoint2D()
        pris.node_a = "BodyA"
        pris.node_b = "BodyB"
        pris.axis = vec2(1.0, 0.0)
        pris.limit_enabled = true
        pris.lower_limit = -50.0
        pris.upper_limit = 50.0
        add_child(pris)

        var sc = ShapeCast2D()
        sc.target_position = vec2(100.0, 0.0)
        add_child(sc)

        Physics2D.explode(vec2(0.0, 0.0), 50.0, 20.0, 100.0, 1)
        var col = Physics2D.intersect_point(vec2(0.0, 0.0), 1)
        var hit = Physics2D.raycast(vec2(0.0, -50.0), vec2(0.0, 50.0), 1)
    }
}
)";

    REQUIRE_MESSAGE(host.load({{"main.plm", script}}, "Main"), host.error());
    for (int i = 0; i < 5; ++i) {
        host.tick(InputFrame{});
    }
    CHECK(host.physics_world().body_count() >= 2);
}

TEST_CASE("physics: joint path resolution, motor control, and collision previews") {
    InputMap map;
    script::HostOptions opt;
    script::ScriptHost host(opt, map);

    const std::string script = R"(
class Level extends Node2D {
    var pin: PinJoint2D? = null
    var wheel: WheelJoint2D? = null
    var dist: DistanceJoint2D? = null

    fn ready() {
        var a = StaticBody2D()
        a.name = "Anchor"
        a.position = vec2(100.0, 100.0)
        add_child(a)

        var s_a = CollisionShape2D()
        var c_a = CircleShape2D()
        c_a.radius = 10.0
        s_a.shape = c_a
        a.add_child(s_a)

        var b = RigidBody2D()
        b.name = "Plank"
        b.position = vec2(150.0, 100.0)
        b.can_sleep = false
        add_child(b)

        var s_b = CollisionShape2D()
        var r_b = RectangleShape2D()
        r_b.size = vec2(60.0, 10.0)
        s_b.shape = r_b
        b.add_child(s_b)

        // Test path resolution with root-prefix "Level/Anchor" and "Level/Plank"
        var p = PinJoint2D()
        p.name = "Pin"
        p.position = vec2(100.0, 100.0)
        p.node_a = "Level/Anchor"
        p.node_b = "Level/Plank"
        p.motor_enabled = true
        p.motor_speed = 5.0
        add_child(p)
        pin = p

        // Test WheelJoint2D with motor control
        var w = WheelJoint2D()
        w.name = "Wheel"
        w.position = vec2(150.0, 100.0)
        w.node_a = "Anchor"
        w.node_b = "Plank"
        w.motor_enabled = true
        w.motor_speed = 10.0
        add_child(w)
        wheel = w

        // Test DistanceJoint2D
        var d = DistanceJoint2D()
        d.name = "Dist"
        d.node_a = "Anchor"
        d.node_b = "Plank"
        d.length = 50.0
        d.stiffness = 20.0
        d.damping = 0.7
        add_child(d)
        dist = d
    }
}
)";

    REQUIRE_MESSAGE(host.load({{"level.plm", script}}, "Level"), host.error());
    for (int i = 0; i < 5; ++i) {
        host.tick(InputFrame{});
    }

    CHECK(host.physics_world().joint_count() >= 3);
    CHECK_FALSE(host.physics_world().debug_shapes());
    host.physics_world().set_debug_shapes(true);
    CHECK(host.physics_world().debug_shapes());

    // Test draw calls with shapes
    render::SdfBatcher shapes;
    render::TextRenderer text;
    host.draw(shapes, text, render::FontHandle{});
    CHECK(shapes.size() > 0);
}

TEST_CASE("physics: moving platform AnimatableBody2D carries CharacterBody2D with floor velocity") {
    InputMap map;
    script::HostOptions opt;
    script::ScriptHost host(opt, map);

    const std::string script = R"(
class Platform extends AnimatableBody2D {
    var cshape: CollisionShape2D? = null

    fn ready() {
        var cs = CollisionShape2D()
        cs.shape_type = 1
        cs.size = vec2(200.0, 30.0)
        add_child(cs)
        cshape = cs
    }

    fn update(dt: float) {
        position.x += 120.0 * dt
    }
}

class PlayerChar extends CharacterBody2D {
    var cshape: CollisionShape2D? = null

    fn ready() {
        var cs = CollisionShape2D()
        cs.shape_type = 1
        cs.size = vec2(30.0, 40.0)
        add_child(cs)
        cshape = cs
    }

    fn update(dt: float) {
        if !is_on_floor() {
            velocity.y += 980.0 * dt
        }
        move_and_slide()
    }
}

var final_plat_x = 0.0
var final_pl_x = 0.0
var final_on_floor = false
var final_plat_vel = vec2(0.0, 0.0)

class Level extends Node2D {
    var platform: Platform? = null
    var player: PlayerChar? = null

    fn ready() {
        var plat = Platform()
        plat.position = vec2(200.0, 300.0)
        add_child(plat)
        platform = plat

        var pl = PlayerChar()
        pl.position = vec2(200.0, 260.0)
        add_child(pl)
        player = pl
    }

    fn update(dt: float) {
        if platform != null {
            final_plat_x = platform.position.x
        }
        if player != null {
            final_pl_x = player.position.x
            final_on_floor = player.is_on_floor()
            final_plat_vel = player.get_platform_velocity()
        }
    }
}
)";

    REQUIRE_MESSAGE(host.load({{"level.plm", script}}, "Level"), host.error());

    // Run several ticks for player to land on platform and ride it
    for (int i = 0; i < 20; ++i) {
        host.tick(InputFrame{});
    }

    CHECK(host.physics_world().body_count() >= 2);
    CHECK(host.physics_world().get_body_position(1).x() > 220.0f);
    CHECK(host.physics_world().get_body_position(2).x() > 215.0f);
    CHECK(host.physics_world().get_body_linear_velocity(1).x() == doctest::Approx(120.0f).epsilon(0.01f));
}

TEST_CASE("physics: PogoJoint2D stays upright and bounces on floor") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Static floor at y = 500, height 40
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {200.0f, 500.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {600.0f, 40.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // PogoHead at (200, 410)
    BodyConfig head_cfg;
    head_cfg.kind = BodyKind::Dynamic;
    head_cfg.position = {200.0f, 410.0f};
    head_cfg.mass = 2.0f;
    head_cfg.lock_rotation = true;
    uint32_t head_b = world.create_body(head_cfg, pluma::Value{});
    ShapeConfig head_s;
    head_s.kind = ShapeKind::Circle;
    head_s.radius = 20.0f;
    world.add_shape(head_b, head_s, pluma::Value{});

    // PogoLeg at (200, 455), bottom at y = 470
    BodyConfig leg_cfg;
    leg_cfg.kind = BodyKind::Dynamic;
    leg_cfg.position = {200.0f, 455.0f};
    leg_cfg.mass = 0.5f;
    leg_cfg.lock_rotation = true;
    uint32_t leg_b = world.create_body(leg_cfg, pluma::Value{});
    ShapeConfig leg_s;
    leg_s.kind = ShapeKind::Box;
    leg_s.size = {8.0f, 30.0f};
    world.add_shape(leg_b, leg_s, pluma::Value{});

    // Create PogoJoint
    uint32_t joint = world.create_pogo_joint(head_b, leg_b, vec2{200.0f, 410.0f}, vec2{0.0f, 1.0f},
                                             80.0f, 0.3f, 45.0f, 1000.0f, 5000.0f, false, pluma::Value{});
    CHECK(joint != 0);

    // Step physics for half a second
    for (int i = 0; i < 30; ++i) {
        world.step(1.0f / 60.0f);
    }

    // Both bodies must maintain 0 rotation (upright) and head must be above leg
    CHECK(world.get_body_rotation(head_b) == doctest::Approx(0.0f));
    CHECK(world.get_body_rotation(leg_b) == doctest::Approx(0.0f));
    CHECK(world.get_body_position(head_b).y() < world.get_body_position(leg_b).y());
    CHECK(world.get_body_position(leg_b).y() < 480.0f);
}

TEST_CASE("physics: joint isolated - PinJoint2D pendulum, limits, and motor") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Anchor static body at (100, 100)
    BodyConfig anc_cfg;
    anc_cfg.kind = BodyKind::Static;
    anc_cfg.position = {100.0f, 100.0f};
    uint32_t anc = world.create_body(anc_cfg, pluma::Value{});

    // Pendulum bob dynamic body at (150, 100)
    BodyConfig bob_cfg;
    bob_cfg.kind = BodyKind::Dynamic;
    bob_cfg.position = {150.0f, 100.0f};
    bob_cfg.mass = 2.0f;
    uint32_t bob = world.create_body(bob_cfg, pluma::Value{});
    ShapeConfig bob_s;
    bob_s.kind = ShapeKind::Circle;
    bob_s.radius = 10.0f;
    world.add_shape(bob, bob_s, pluma::Value{});

    // 1. Free pin joint swinging as pendulum
    uint32_t pin = world.create_pin_joint(anc, bob, vec2{100.0f, 100.0f}, 0.0f,
                                          false, 0.0f, 0.0f, false, 0.0f, 0.0f, false, pluma::Value{});
    REQUIRE(pin != 0);

    for (int i = 0; i < 30; ++i) {
        world.step(1.0f / 60.0f);
        vec2 p = world.get_body_position(bob);
        float d = std::sqrt((p.x() - 100.0f) * (p.x() - 100.0f) + (p.y() - 100.0f) * (p.y() - 100.0f));
        CHECK(d == doctest::Approx(50.0f).epsilon(0.05f));
    }
    world.destroy_joint(pin);

    // 2. Pin joint with angular limits
    world.set_body_transform(bob, vec2{150.0f, 100.0f}, 0.0f);
    world.set_body_linear_velocity(bob, vec2{0.0f, 0.0f});
    world.set_body_angular_velocity(bob, 0.0f);
    uint32_t pin_limit = world.create_pin_joint(anc, bob, vec2{100.0f, 100.0f}, 0.0f,
                                                true, -0.3f, 0.3f, false, 0.0f, 0.0f, false, pluma::Value{});
    REQUIRE(pin_limit != 0);
    for (int i = 0; i < 30; ++i) {
        world.step(1.0f / 60.0f);
        float rot = world.get_body_rotation(bob);
        CHECK(rot >= -0.35f);
        CHECK(rot <= 0.35f);
    }
    world.destroy_joint(pin_limit);

    // 3. Pin joint with motor
    uint32_t pin_motor = world.create_pin_joint(anc, bob, vec2{100.0f, 100.0f}, 0.0f,
                                                false, 0.0f, 0.0f, true, 4.0f, 5000.0f, false, pluma::Value{});
    REQUIRE(pin_motor != 0);
    for (int i = 0; i < 20; ++i) {
        world.step(1.0f / 60.0f);
    }
    CHECK(world.get_body_angular_velocity(bob) == doctest::Approx(4.0f).epsilon(0.15f));
    world.destroy_joint(pin_motor);
}

TEST_CASE("physics: joint isolated - DistanceJoint2D rigid and spring modes") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    BodyConfig anc_cfg;
    anc_cfg.kind = BodyKind::Static;
    anc_cfg.position = {100.0f, 100.0f};
    uint32_t anc = world.create_body(anc_cfg, pluma::Value{});

    BodyConfig bob_cfg;
    bob_cfg.kind = BodyKind::Dynamic;
    bob_cfg.position = {200.0f, 100.0f};
    bob_cfg.mass = 2.0f;
    uint32_t bob = world.create_body(bob_cfg, pluma::Value{});
    ShapeConfig bob_s;
    bob_s.kind = ShapeKind::Circle;
    bob_s.radius = 10.0f;
    world.add_shape(bob, bob_s, pluma::Value{});

    // Rigid distance joint (stiffness = 0.0 means rigid)
    uint32_t dist = world.create_distance_joint(anc, bob, vec2{100.0f, 100.0f}, vec2{200.0f, 100.0f},
                                                100.0f, 100.0f, 100.0f, 0.0f, 0.0f, false, pluma::Value{});
    REQUIRE(dist != 0);

    for (int i = 0; i < 30; ++i) {
        world.step(1.0f / 60.0f);
        vec2 p = world.get_body_position(bob);
        float d = std::sqrt((p.x() - 100.0f) * (p.x() - 100.0f) + (p.y() - 100.0f) * (p.y() - 100.0f));
        CHECK(d == doctest::Approx(100.0f).epsilon(0.05f));
    }
    world.destroy_joint(dist);
}

TEST_CASE("physics: joint isolated - DampedSpringJoint2D oscillation and rest length") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f}); // Zero gravity

    BodyConfig b1_cfg;
    b1_cfg.kind = BodyKind::Static;
    b1_cfg.position = {100.0f, 100.0f};
    uint32_t b1 = world.create_body(b1_cfg, pluma::Value{});

    BodyConfig b2_cfg;
    b2_cfg.kind = BodyKind::Dynamic;
    b2_cfg.position = {200.0f, 100.0f}; // Displaced by +50 from rest length 50
    b2_cfg.mass = 1.0f;
    uint32_t b2 = world.create_body(b2_cfg, pluma::Value{});
    ShapeConfig s;
    s.kind = ShapeKind::Circle;
    s.radius = 10.0f;
    world.add_shape(b2, s, pluma::Value{});

    // Spring with rest length 50.0, stiffness 8.0, damping 0.5
    uint32_t spring = world.create_distance_joint(b1, b2, vec2{100.0f, 100.0f}, vec2{200.0f, 100.0f},
                                                  50.0f, 0.0f, 1000.0f, 8.0f, 0.5f, false, pluma::Value{});
    REQUIRE(spring != 0);

    // Initial distance is 100. Over 1 second it pulls back toward rest length 50.
    for (int i = 0; i < 60; ++i) {
        world.step(1.0f / 60.0f);
    }

    vec2 p = world.get_body_position(b2);
    float d = std::abs(p.x() - 100.0f);
    CHECK(d < 70.0f);
    CHECK(d > 35.0f);
    world.destroy_joint(spring);
}

TEST_CASE("physics: joint isolated - PrismaticJoint2D 1D translation, limits, and motor") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f}); // Vertical gravity

    BodyConfig guide_cfg;
    guide_cfg.kind = BodyKind::Static;
    guide_cfg.position = {200.0f, 200.0f};
    uint32_t guide = world.create_body(guide_cfg, pluma::Value{});

    BodyConfig car_cfg;
    car_cfg.kind = BodyKind::Dynamic;
    car_cfg.position = {200.0f, 200.0f};
    car_cfg.mass = 3.0f;
    uint32_t car = world.create_body(car_cfg, pluma::Value{});
    ShapeConfig car_s;
    car_s.kind = ShapeKind::Box;
    car_s.size = {30.0f, 30.0f};
    world.add_shape(car, car_s, pluma::Value{});

    // Pure horizontal axis (1, 0) despite vertical gravity (0, 980)
    uint32_t pris = world.create_prismatic_joint(guide, car, vec2{200.0f, 200.0f}, vec2{1.0f, 0.0f},
                                                 false, 0.0f, 0.0f, 0.0f,
                                                 true, -60.0f, 60.0f,
                                                 true, 5000.0f, 120.0f, false, pluma::Value{});
    REQUIRE(pris != 0);

    // Motor drives car along horizontal axis to upper limit 60.0
    for (int i = 0; i < 40; ++i) {
        world.step(1.0f / 60.0f);
        vec2 p = world.get_body_position(car);
        // Vertical position must remain locked at 200.0 despite gravity!
        CHECK(p.y() == doctest::Approx(200.0f).epsilon(0.01f));
        // Horizontal position must not exceed limit 260.0
        CHECK(p.x() <= 260.05f);
    }
    vec2 final_p = world.get_body_position(car);
    CHECK(final_p.x() >= 258.0f);
    world.destroy_joint(pris);
}

TEST_CASE("physics: joint isolated - WeldJoint2D locks relative transform") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    BodyConfig b1_cfg;
    b1_cfg.kind = BodyKind::Dynamic;
    b1_cfg.position = {100.0f, 100.0f};
    b1_cfg.mass = 2.0f;
    uint32_t b1 = world.create_body(b1_cfg, pluma::Value{});

    BodyConfig b2_cfg;
    b2_cfg.kind = BodyKind::Dynamic;
    b2_cfg.position = {140.0f, 100.0f};
    b2_cfg.mass = 2.0f;
    uint32_t b2 = world.create_body(b2_cfg, pluma::Value{});

    uint32_t weld = world.create_weld_joint(b1, b2, vec2{120.0f, 100.0f}, 0.0f, 0.0f, 1.0f, 1.0f, false, pluma::Value{});
    REQUIRE(weld != 0);

    // Apply an asymmetric impulse
    world.apply_impulse(b1, vec2{50.0f, -20.0f});

    for (int i = 0; i < 20; ++i) {
        world.step(1.0f / 60.0f);
        vec2 p1 = world.get_body_position(b1);
        vec2 p2 = world.get_body_position(b2);
        float d = std::sqrt((p2.x() - p1.x()) * (p2.x() - p1.x()) + (p2.y() - p1.y()) * (p2.y() - p1.y()));
        CHECK(d == doctest::Approx(40.0f).epsilon(0.02f));
        CHECK(world.get_body_rotation(b1) == doctest::Approx(world.get_body_rotation(b2)).epsilon(0.02f));
    }
    world.destroy_joint(weld);
}

TEST_CASE("physics: joint isolated - WheelJoint2D suspension, limits, and motor") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Fixed chassis at (200, 200)
    BodyConfig ch_cfg;
    ch_cfg.kind = BodyKind::Static;
    ch_cfg.position = {200.0f, 200.0f};
    uint32_t chassis = world.create_body(ch_cfg, pluma::Value{});

    // Dynamic wheel at (200, 240)
    BodyConfig wh_cfg;
    wh_cfg.kind = BodyKind::Dynamic;
    wh_cfg.position = {200.0f, 240.0f};
    wh_cfg.mass = 3.0f;
    uint32_t wheel = world.create_body(wh_cfg, pluma::Value{});
    ShapeConfig wh_s;
    wh_s.kind = ShapeKind::Circle;
    wh_s.radius = 16.0f;
    world.add_shape(wheel, wh_s, pluma::Value{});

    // Wheel joint with vertical suspension axis (0, 1) and motor.
    // Anchor placed at wheel axle center (200, 240).
    uint32_t wj = world.create_wheel_joint(chassis, wheel, vec2{200.0f, 240.0f}, vec2{0.0f, 1.0f},
                                           true, 15.0f, 0.7f,
                                           true, -10.0f, 20.0f,
                                           true, 10000.0f, 6.0f, false, pluma::Value{});
    REQUIRE(wj != 0);

    for (int i = 0; i < 40; ++i) {
        world.step(1.0f / 60.0f);
    }

    // Wheel should be spinning from motor
    CHECK(world.get_body_angular_velocity(wheel) == doctest::Approx(6.0f).epsilon(0.1f));
    // Wheel translation should be constrained within suspension limits
    vec2 p = world.get_body_position(wheel);
    CHECK(p.y() >= 230.0f);
    CHECK(p.y() <= 265.0f);
    world.destroy_joint(wj);
}

TEST_CASE("physics: joint isolated - MotorJoint2D angular velocity drive") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f}); // Zero gravity

    BodyConfig base_cfg;
    base_cfg.kind = BodyKind::Static;
    base_cfg.position = {100.0f, 100.0f};
    uint32_t base = world.create_body(base_cfg, pluma::Value{});

    BodyConfig rotor_cfg;
    rotor_cfg.kind = BodyKind::Dynamic;
    rotor_cfg.position = {100.0f, 100.0f};
    rotor_cfg.mass = 1.0f;
    uint32_t rotor = world.create_body(rotor_cfg, pluma::Value{});
    ShapeConfig rotor_s;
    rotor_s.kind = ShapeKind::Box;
    rotor_s.size = {60.0f, 16.0f};
    world.add_shape(rotor, rotor_s, pluma::Value{});

    uint32_t mj = world.create_motor_joint(base, rotor, vec2{0.0f, 0.0f}, 0.0f,
                                           5000.0f, 10000.0f, 2.0f, 0.7f, 500.0f,
                                           0.0f, 0.0f, 0.0f, false, pluma::Value{});
    REQUIRE(mj != 0);

    // Set motor speed via API
    world.joint_set_motor_speed(mj, 5.0f);

    for (int i = 0; i < 40; ++i) {
        world.step(1.0f / 60.0f);
    }

    CHECK(world.get_body_angular_velocity(rotor) == doctest::Approx(5.0f).epsilon(0.1f));
    world.destroy_joint(mj);
}

TEST_CASE("physics: joint isolated - MoverJoint2D linear velocity tracking") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    BodyConfig base_cfg;
    base_cfg.kind = BodyKind::Static;
    base_cfg.position = {0.0f, 0.0f};
    uint32_t base = world.create_body(base_cfg, pluma::Value{});

    BodyConfig mover_cfg;
    mover_cfg.kind = BodyKind::Dynamic;
    mover_cfg.position = {100.0f, 100.0f};
    mover_cfg.mass = 2.0f;
    uint32_t mover_b = world.create_body(mover_cfg, pluma::Value{});
    ShapeConfig mover_s;
    mover_s.kind = ShapeKind::Box;
    mover_s.size = {30.0f, 30.0f};
    world.add_shape(mover_b, mover_s, pluma::Value{});

    uint32_t mover_j = world.create_mover_joint(base, mover_b, vec2{60.0f, 0.0f}, vec2{2000.0f, 2000.0f}, false, pluma::Value{});
    REQUIRE(mover_j != 0);

    for (int i = 0; i < 20; ++i) {
        world.step(1.0f / 60.0f);
    }

    vec2 vel = world.get_body_linear_velocity(mover_b);
    CHECK(vel.x() == doctest::Approx(60.0f).epsilon(0.1f));
    world.destroy_joint(mover_j);
}

TEST_CASE("physics: joint isolated - FilterJoint2D collision disabling") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Two dynamic bodies at the exact same position
    BodyConfig b1_cfg;
    b1_cfg.kind = BodyKind::Dynamic;
    b1_cfg.position = {100.0f, 100.0f};
    b1_cfg.mass = 1.0f;
    uint32_t b1 = world.create_body(b1_cfg, pluma::Value{});
    ShapeConfig s1;
    s1.kind = ShapeKind::Box;
    s1.size = {30.0f, 30.0f};
    world.add_shape(b1, s1, pluma::Value{});

    BodyConfig b2_cfg;
    b2_cfg.kind = BodyKind::Dynamic;
    b2_cfg.position = {100.0f, 100.0f};
    b2_cfg.mass = 1.0f;
    uint32_t b2 = world.create_body(b2_cfg, pluma::Value{});
    ShapeConfig s2;
    s2.kind = ShapeKind::Box;
    s2.size = {30.0f, 30.0f};
    world.add_shape(b2, s2, pluma::Value{});

    // Create FilterJoint between b1 and b2
    uint32_t fj = world.create_filter_joint(b1, b2, pluma::Value{});
    REQUIRE(fj != 0);

    // Step physics: without filter joint they would explode apart
    for (int i = 0; i < 20; ++i) {
        world.step(1.0f / 60.0f);
    }

    // Both bodies stay at (100, 100) with 0 velocity
    vec2 p1 = world.get_body_position(b1);
    vec2 p2 = world.get_body_position(b2);
    CHECK(p1.x() == doctest::Approx(100.0f).epsilon(0.01f));
    CHECK(p2.x() == doctest::Approx(100.0f).epsilon(0.01f));
    world.destroy_joint(fj);
}

TEST_CASE("physics: a hot reload moves the world's bodies to the new VM") {
    // The physics world holds its bodies' nodes; a reload replaces every
    // script object, so the world must follow (it used to call methods on the
    // freed objects of the old VM after a reload).
    InputMap map;
    script::HostOptions opt;
    script::ScriptHost host(opt, map);
    const std::string source = R"(
var mover_x = 0.0
var crate_y = 0.0
class Main extends Node2D {
    var mover: CharacterBody2D
    var crate: RigidBody2D
    fn ready() {
        var floor = StaticBody2D()
        floor.position = vec2(0.0, 200.0)
        var fs = CollisionShape2D()
        fs.size = vec2(800.0, 20.0)
        floor.add_child(fs)
        add_child(floor)
        mover = CharacterBody2D()
        var ms = CollisionShape2D()
        ms.size = vec2(20.0, 20.0)
        mover.add_child(ms)
        add_child(mover)
        crate = RigidBody2D()
        crate.position = vec2(100.0, 0.0)
        var cs = CollisionShape2D()
        cs.size = vec2(20.0, 20.0)
        crate.add_child(cs)
        add_child(crate)
    }
    fn update(dt: float) {
        mover.velocity = vec2(60.0, 0.0)
        mover.move_and_slide()
        mover_x = mover.global_position().x
        crate_y = crate.global_position().y
    }
}
)";
    REQUIRE_MESSAGE(host.load({{"main.plm", source}}, "Main"), host.error());
    for (int i = 0; i < 30; ++i) host.tick(InputFrame{});
    REQUIRE(host.running());
    const float x_before = host.session()->global("mover_x").f;
    // Reload with an edit, then keep stepping: callbacks reach the new objects.
    REQUIRE_MESSAGE(host.load({{"main.plm", source + "\n// edited\n"}}, "Main"), host.error());
    for (int i = 0; i < 120; ++i) host.tick(InputFrame{});
    REQUIRE_MESSAGE(host.running(), host.error());
    CHECK(host.session()->global("mover_x").f > x_before + 50.0f);
    // The crate fell onto the floor (top at 190) and rests there.
    CHECK(host.session()->global("crate_y").f == doctest::Approx(180.0f).epsilon(0.02));
}

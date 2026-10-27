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

TEST_CASE("physics: vehicle - acceleration squat and braking dive travel bounds") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Static floor at y = 300, thickness 40 (top edge at y = 280)
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {500.0f, 300.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {1200.0f, 40.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Dynamic vehicle chassis: size 110x26 (half-height 13), mass 40kg
    BodyConfig ch_cfg;
    ch_cfg.kind = BodyKind::Dynamic;
    ch_cfg.position = {300.0f, 235.0f};
    ch_cfg.mass = 40.0f;
    uint32_t chassis = world.create_body(ch_cfg, pluma::Value{});
    ShapeConfig ch_s;
    ch_s.kind = ShapeKind::Box;
    ch_s.size = {110.0f, 26.0f};
    world.add_shape(chassis, ch_s, pluma::Value{});

    // Front wheel: x = 345, y = 265, radius 18, mass 5kg
    BodyConfig fw_cfg;
    fw_cfg.kind = BodyKind::Dynamic;
    fw_cfg.position = {345.0f, 265.0f};
    fw_cfg.mass = 5.0f;
    uint32_t front_wheel = world.create_body(fw_cfg, pluma::Value{});
    ShapeConfig w_s;
    w_s.kind = ShapeKind::Circle;
    w_s.radius = 18.0f;
    w_s.friction = 0.9f;
    world.add_shape(front_wheel, w_s, pluma::Value{});

    // Rear wheel: x = 255, y = 265, radius 18, mass 5kg
    BodyConfig rw_cfg;
    rw_cfg.kind = BodyKind::Dynamic;
    rw_cfg.position = {255.0f, 265.0f};
    rw_cfg.mass = 5.0f;
    uint32_t rear_wheel = world.create_body(rw_cfg, pluma::Value{});
    world.add_shape(rear_wheel, w_s, pluma::Value{});

    // Front suspension: axis (0, 1), limits [-15, 20]
    uint32_t front_j = world.create_wheel_joint(chassis, front_wheel, vec2{345.0f, 265.0f}, vec2{0.0f, 1.0f},
                                                true, 25.0f, 0.7f,
                                                true, -15.0f, 20.0f,
                                                true, 50000.0f, 0.0f, false, pluma::Value{});
    REQUIRE(front_j != 0);

    // Rear suspension: axis (0, 1), limits [-15, 20]
    uint32_t rear_j = world.create_wheel_joint(chassis, rear_wheel, vec2{255.0f, 265.0f}, vec2{0.0f, 1.0f},
                                               true, 25.0f, 0.7f,
                                               true, -15.0f, 20.0f,
                                               true, 50000.0f, 0.0f, false, pluma::Value{});
    REQUIRE(rear_j != 0);

    // Settle under gravity for 30 ticks
    for (int i = 0; i < 30; ++i) {
        world.step(1.0f / 60.0f);
    }

    vec2 settled_ch = world.get_body_position(chassis);
    // Undercarriage (bottom edge y = settled_ch.y + 13) must have clear ground clearance above floor (y = 280)
    CHECK(settled_ch.y() + 13.0f <= 274.0f); // At least 6 px clearance!

    // 1. Acceleration Phase: Drive both wheels forward
    world.joint_set_motor_speed(front_j, 15.0f);
    world.joint_set_motor_speed(rear_j, 15.0f);

    float max_rear_squat_rot = 0.0f;
    for (int i = 0; i < 30; ++i) {
        world.step(1.0f / 60.0f);
        float rot = world.get_body_rotation(chassis);
        if (std::abs(rot) > max_rear_squat_rot) {
            max_rear_squat_rot = std::abs(rot);
        }
        vec2 p = world.get_body_position(chassis);
        // Undercarriage must never scrape the floor during acceleration
        CHECK(p.y() + 13.0f < 280.0f);
    }

    // Pitch angle during acceleration must not exceed 8 degrees (0.14 rad)
    CHECK(max_rear_squat_rot < 0.14f);

    // 2. Braking Phase: Reverse motors to brake
    world.joint_set_motor_speed(front_j, -10.0f);
    world.joint_set_motor_speed(rear_j, -10.0f);

    float max_brake_dive_rot = 0.0f;
    for (int i = 0; i < 20; ++i) {
        world.step(1.0f / 60.0f);
        float rot = world.get_body_rotation(chassis);
        if (std::abs(rot) > max_brake_dive_rot) {
            max_brake_dive_rot = std::abs(rot);
        }
        vec2 p = world.get_body_position(chassis);
        // Front bumper must not scrape the floor
        CHECK(p.y() + 13.0f < 280.0f);
    }
    CHECK(max_brake_dive_rot < 0.14f);

    world.destroy_joint(front_j);
    world.destroy_joint(rear_j);
}

TEST_CASE("physics: vehicle - static payload squat under heavy cargo and player mass") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Floor
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {300.0f, 300.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {600.0f, 40.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Chassis 110x26
    BodyConfig ch_cfg;
    ch_cfg.kind = BodyKind::Dynamic;
    ch_cfg.position = {300.0f, 235.0f};
    ch_cfg.mass = 40.0f;
    uint32_t chassis = world.create_body(ch_cfg, pluma::Value{});
    ShapeConfig ch_s;
    ch_s.kind = ShapeKind::Box;
    ch_s.size = {110.0f, 26.0f};
    world.add_shape(chassis, ch_s, pluma::Value{});

    // Front & rear wheels
    BodyConfig w_cfg;
    w_cfg.kind = BodyKind::Dynamic;
    w_cfg.mass = 5.0f;
    ShapeConfig w_s;
    w_s.kind = ShapeKind::Circle;
    w_s.radius = 18.0f;

    w_cfg.position = {345.0f, 265.0f};
    uint32_t fw = world.create_body(w_cfg, pluma::Value{});
    world.add_shape(fw, w_s, pluma::Value{});

    w_cfg.position = {255.0f, 265.0f};
    uint32_t rw = world.create_body(w_cfg, pluma::Value{});
    world.add_shape(rw, w_s, pluma::Value{});

    uint32_t fj = world.create_wheel_joint(chassis, fw, vec2{345.0f, 265.0f}, vec2{0.0f, 1.0f},
                                           true, 25.0f, 0.7f, true, -15.0f, 20.0f, false, 0.0f, 0.0f, false, pluma::Value{});
    uint32_t rj = world.create_wheel_joint(chassis, rw, vec2{255.0f, 265.0f}, vec2{0.0f, 1.0f},
                                           true, 25.0f, 0.7f, true, -15.0f, 20.0f, false, 0.0f, 0.0f, false, pluma::Value{});

    // Settle unloaded
    for (int i = 0; i < 30; ++i) world.step(1.0f / 60.0f);
    float unloaded_y = world.get_body_position(chassis).y();

    // Place heavy cargo (30 kg) directly on rear bed at x = 270, y = 210
    BodyConfig cargo_cfg;
    cargo_cfg.kind = BodyKind::Dynamic;
    cargo_cfg.position = {270.0f, 210.0f};
    cargo_cfg.mass = 30.0f;
    uint32_t cargo = world.create_body(cargo_cfg, pluma::Value{});
    ShapeConfig cargo_s;
    cargo_s.kind = ShapeKind::Box;
    cargo_s.size = {30.0f, 20.0f};
    world.add_shape(cargo, cargo_s, pluma::Value{});

    // Settle loaded
    for (int i = 0; i < 40; ++i) world.step(1.0f / 60.0f);
    float loaded_y = world.get_body_position(chassis).y();

    // Suspension sag occurs (loaded_y > unloaded_y)
    float sag = loaded_y - unloaded_y;
    CHECK(sag > 0.1f);
    CHECK(sag < 10.0f); // Sag must not exceed 10 px
    // Ground clearance remains >= 6 px (floor top at 280, chassis bottom at loaded_y + 13)
    CHECK(loaded_y + 13.0f <= 274.0f);

    world.destroy_joint(fj);
    world.destroy_joint(rj);
}

TEST_CASE("physics: vehicle - player roof and bed riding with floor velocity tracking") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Static floor at y = 300 (top edge at 280)
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {600.0f, 300.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {1400.0f, 40.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Chassis 110x26 at y = 235 (top edge at y = 222)
    BodyConfig ch_cfg;
    ch_cfg.kind = BodyKind::Dynamic;
    ch_cfg.position = {200.0f, 235.0f};
    ch_cfg.mass = 40.0f;
    uint32_t chassis = world.create_body(ch_cfg, pluma::Value{});
    ShapeConfig ch_s;
    ch_s.kind = ShapeKind::Box;
    ch_s.size = {110.0f, 26.0f};
    world.add_shape(chassis, ch_s, pluma::Value{});

    // Wheels
    BodyConfig w_cfg;
    w_cfg.kind = BodyKind::Dynamic;
    w_cfg.mass = 5.0f;
    ShapeConfig w_s;
    w_s.kind = ShapeKind::Circle;
    w_s.radius = 18.0f;

    w_cfg.position = {245.0f, 265.0f};
    uint32_t fw = world.create_body(w_cfg, pluma::Value{});
    world.add_shape(fw, w_s, pluma::Value{});

    w_cfg.position = {155.0f, 265.0f};
    uint32_t rw = world.create_body(w_cfg, pluma::Value{});
    world.add_shape(rw, w_s, pluma::Value{});

    uint32_t fj = world.create_wheel_joint(chassis, fw, vec2{245.0f, 265.0f}, vec2{0.0f, 1.0f},
                                           true, 25.0f, 0.7f, true, -15.0f, 20.0f, true, 50000.0f, 20.0f, false, pluma::Value{});
    uint32_t rj = world.create_wheel_joint(chassis, rw, vec2{155.0f, 265.0f}, vec2{0.0f, 1.0f},
                                           true, 25.0f, 0.7f, true, -15.0f, 20.0f, true, 50000.0f, 20.0f, false, pluma::Value{});

    // Kinematic player standing on chassis roof
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {200.0f, 200.0f};
    uint32_t player = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {20.0f, 30.0f};
    world.add_shape(player, pl_s, pluma::Value{});

    const float dt = 1.0f / 60.0f;
    // Step simulation and move_and_slide for player
    for (int i = 0; i < 40; ++i) {
        world.step(dt);
        SlideResult res = world.move_and_slide(player, vec2{0.0f, 980.0f * dt}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(player, res.position, 0.0f);

        if (i > 15) {
            // Player must be resting on the chassis
            CHECK(res.on_floor);
            // Floor velocity must be non-negative
            CHECK(res.floor_velocity.x() >= -0.05f);
        }
    }

    // After 40 ticks, vehicle has moved forward
    vec2 ch_p = world.get_body_position(chassis);
    vec2 pl_p = world.get_body_position(player);
    CHECK(ch_p.x() > 205.0f);
    // Player must have traveled with the vehicle! Relative offset must stay small.
    CHECK(std::abs(pl_p.x() - ch_p.x()) < 15.0f);

    world.destroy_joint(fj);
    world.destroy_joint(rj);
}

TEST_CASE("physics: moving platform - high speed vertical ascent anti-penetration sweep") {
    const float speeds[] = {-150.0f, -300.0f, -600.0f, -1000.0f};
    const float dt = 1.0f / 60.0f;

    for (float vy : speeds) {
        PhysicsWorld world(1, vec2{0.0f, 0.0f});

        // Platform starts at y = 300, size 200x20 (top edge at y = 290)
        BodyConfig plat_cfg;
        plat_cfg.kind = BodyKind::Kinematic;
        plat_cfg.position = {100.0f, 300.0f};
        plat_cfg.linear_velocity = {0.0f, vy};
        uint32_t plat = world.create_body(plat_cfg, pluma::Value{});
        ShapeConfig plat_s;
        plat_s.kind = ShapeKind::Box;
        plat_s.size = {200.0f, 20.0f};
        world.add_shape(plat, plat_s, pluma::Value{});

        // Player starting flush on platform at y = 270, size 20x40 (bottom edge at y = 290)
        BodyConfig pl_cfg;
        pl_cfg.kind = BodyKind::Kinematic;
        pl_cfg.position = {100.0f, 270.0f};
        uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
        ShapeConfig pl_s;
        pl_s.kind = ShapeKind::Box;
        pl_s.size = {20.0f, 40.0f};
        world.add_shape(pl, pl_s, pluma::Value{});

        for (int tick = 0; tick < 20; ++tick) {
            world.step(dt);

            SlideResult res = world.move_and_slide(pl, vec2{0.0f, 500.0f * dt}, dt,
                                                  vec2{0.0f, -1.0f}, 0.785f, 4);
            world.set_body_transform(pl, res.position, 0.0f);

            vec2 current_plat = world.get_body_position(plat);
            float feet_y = res.position.y() + 20.0f;
            float plat_top_y = current_plat.y() - 10.0f;

            // Player feet must NEVER penetrate below platform top (within discrete step tolerance)
            CHECK(feet_y <= plat_top_y + 3.5f);
            // Player stays on the ascending platform
            CHECK(std::abs(feet_y - plat_top_y) < 5.0f);
        }
    }
}

TEST_CASE("physics: moving platform - initial overlap depenetration recovery in one tick") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Platform at y = 200, size 200x20 (top edge at 190)
    BodyConfig plat_cfg;
    plat_cfg.kind = BodyKind::Kinematic;
    plat_cfg.position = {100.0f, 200.0f};
    uint32_t plat = world.create_body(plat_cfg, pluma::Value{});
    ShapeConfig plat_s;
    plat_s.kind = ShapeKind::Box;
    plat_s.size = {200.0f, 20.0f};
    world.add_shape(plat, plat_s, pluma::Value{});

    // Player height 40 (half-height 20).
    // Intentionally place player center at y = 174 (bottom edge at 194, which is 4px inside the platform!)
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {100.0f, 174.0f};
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {20.0f, 40.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    world.step(1.0f / 60.0f);

    // Run move_and_slide with downward velocity
    SlideResult res = world.move_and_slide(pl, vec2{0.0f, 60.0f * (1.0f / 60.0f)}, 1.0f / 60.0f,
                                          vec2{0.0f, -1.0f}, 0.785f, 4);

    // Must detect collision, normal, and prevent deeper penetration
    CHECK(res.collided);
    CHECK(res.on_floor);
    CHECK(res.floor_normal.y() < -0.9f);
    CHECK(res.position.y() <= 174.05f);
}

TEST_CASE("physics: moving platform - fast descent sticking and abrupt stop without tunneling") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});
    const float dt = 1.0f / 60.0f;

    const float desc_speed = 480.0f;

    // Platform at y = 100, size 200x20
    BodyConfig plat_cfg;
    plat_cfg.kind = BodyKind::Kinematic;
    plat_cfg.position = {100.0f, 100.0f};
    plat_cfg.linear_velocity = {0.0f, desc_speed};
    uint32_t plat = world.create_body(plat_cfg, pluma::Value{});
    ShapeConfig plat_s;
    plat_s.kind = ShapeKind::Box;
    plat_s.size = {200.0f, 20.0f};
    world.add_shape(plat, plat_s, pluma::Value{});

    // Player resting on platform at y = 70 (bottom at 90)
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {100.0f, 70.0f};
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {20.0f, 40.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    // Establish initial resting contact on platform
    world.move_and_slide(pl, vec2{0.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);

    for (int tick = 0; tick < 25; ++tick) {
        world.step(dt);

        SlideResult res = world.move_and_slide(pl, vec2{0.0f, 980.0f * dt}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);

        vec2 current_plat = world.get_body_position(plat);
        float feet_y = res.position.y() + 20.0f;
        float plat_top = current_plat.y() - 10.0f;
        CHECK(res.on_floor);
        CHECK(std::abs(feet_y - plat_top) < 2.0f);
    }

    // Abrupt stop: halt platform velocity
    world.set_body_linear_velocity(plat, vec2{0.0f, 0.0f});

    for (int tick = 0; tick < 10; ++tick) {
        world.step(dt);
        SlideResult res = world.move_and_slide(pl, vec2{0.0f, 980.0f * dt}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);

        vec2 current_plat = world.get_body_position(plat);
        float feet_y = res.position.y() + 20.0f;
        float plat_top = current_plat.y() - 10.0f;
        // Zero tunneling through the halted platform floor
        CHECK(feet_y <= plat_top + 0.15f);
    }
}

TEST_CASE("physics: moving platform - sinusoidal vertical oscillation apex and trough") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    const float dt = 1.0f / 60.0f;

    BodyConfig plat_cfg;
    plat_cfg.kind = BodyKind::Kinematic;
    plat_cfg.position = {100.0f, 300.0f};
    uint32_t plat = world.create_body(plat_cfg, pluma::Value{});
    ShapeConfig plat_s;
    plat_s.kind = ShapeKind::Box;
    plat_s.size = {200.0f, 20.0f};
    world.add_shape(plat, plat_s, pluma::Value{});

    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {100.0f, 270.0f};
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {20.0f, 40.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    float t = 0.0f;
    bool on_floor_state = true;
    for (int tick = 0; tick < 120; ++tick) {
        t += dt;
        float vy = 100.0f * 3.0f * std::cos(3.0f * t);
        world.set_body_linear_velocity(plat, vec2{0.0f, vy});
        world.step(dt);

        vec2 pl_vel = on_floor_state ? vec2{0.0f, 0.0f} : vec2{0.0f, 980.0f * dt};
        SlideResult res = world.move_and_slide(pl, pl_vel, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);
        on_floor_state = res.on_floor;

        vec2 current_plat = world.get_body_position(plat);
        float feet_y = res.position.y() + 20.0f;
        float plat_top = current_plat.y() - 10.0f;

        // Player feet must never penetrate below platform top (anti-clipping invariant)
        CHECK(feet_y <= plat_top + 1.5f);
    }
}

TEST_CASE("physics: moving platform - vertical ceiling pinch containment without NaNs") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});
    const float dt = 1.0f / 60.0f;

    // Static ceiling at y = 100, height 20 (bottom edge at y = 110)
    BodyConfig ceil_cfg;
    ceil_cfg.kind = BodyKind::Static;
    ceil_cfg.position = {100.0f, 100.0f};
    uint32_t ceil_b = world.create_body(ceil_cfg, pluma::Value{});
    ShapeConfig ceil_s;
    ceil_s.kind = ShapeKind::Box;
    ceil_s.size = {200.0f, 20.0f};
    world.add_shape(ceil_b, ceil_s, pluma::Value{});

    // Ascending platform starting at y = 200, height 20 (top edge at y = 190)
    BodyConfig plat_cfg;
    plat_cfg.kind = BodyKind::Kinematic;
    plat_cfg.position = {100.0f, 200.0f};
    plat_cfg.linear_velocity = {0.0f, -120.0f};
    uint32_t plat = world.create_body(plat_cfg, pluma::Value{});
    ShapeConfig plat_s;
    plat_s.kind = ShapeKind::Box;
    plat_s.size = {200.0f, 20.0f};
    world.add_shape(plat, plat_s, pluma::Value{});

    // Player height 40 (half-height 20) placed between them at y = 160
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {100.0f, 160.0f};
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {20.0f, 40.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    bool hit_ceiling = false;

    // Ascend platform until gap < 40px
    for (int tick = 0; tick < 40; ++tick) {
        world.step(dt);

        SlideResult res = world.move_and_slide(pl, vec2{0.0f, 0.0f}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);

        if (res.on_ceiling) {
            hit_ceiling = true;
        }

        CHECK(!std::isnan(res.position.x()));
        CHECK(!std::isnan(res.position.y()));
        CHECK(!std::isinf(res.position.x()));
        CHECK(!std::isinf(res.position.y()));

        CHECK(res.position.y() - 20.0f >= 109.8f);
    }

    CHECK(hit_ceiling);
}

TEST_CASE("physics: player interaction - suspension bridge plank sag and traversal") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    const float dt = 1.0f / 60.0f;

    // Anchor Left at x = 100, y = 200
    BodyConfig anc_cfg;
    anc_cfg.kind = BodyKind::Static;
    anc_cfg.position = {100.0f, 200.0f};
    uint32_t anc_l = world.create_body(anc_cfg, pluma::Value{});

    // Anchor Right at x = 400, y = 200
    anc_cfg.position = {400.0f, 200.0f};
    uint32_t anc_r = world.create_body(anc_cfg, pluma::Value{});

    // 5 dynamic planks: width 58, height 10, mass 2.0kg each
    std::vector<uint32_t> planks;
    ShapeConfig plank_s;
    plank_s.kind = ShapeKind::Box;
    plank_s.size = {58.0f, 10.0f};

    for (int i = 0; i < 5; ++i) {
        BodyConfig p_cfg;
        p_cfg.kind = BodyKind::Dynamic;
        p_cfg.position = {130.0f + i * 60.0f, 200.0f};
        p_cfg.mass = 2.0f;
        uint32_t pb = world.create_body(p_cfg, pluma::Value{});
        world.add_shape(pb, plank_s, pluma::Value{});
        planks.push_back(pb);
    }

    // Connect with PinJoints with limits [-0.35, 0.35]
    std::vector<uint32_t> joints;
    joints.push_back(world.create_pin_joint(anc_l, planks[0], vec2{100.0f, 200.0f}, 0.0f,
                                           true, -0.35f, 0.35f, false, 0.0f, 0.0f, false, pluma::Value{}));
    for (int i = 0; i < 4; ++i) {
        joints.push_back(world.create_pin_joint(planks[i], planks[i + 1], vec2{160.0f + i * 60.0f, 200.0f}, 0.0f,
                                               true, -0.35f, 0.35f, false, 0.0f, 0.0f, false, pluma::Value{}));
    }
    joints.push_back(world.create_pin_joint(planks[4], anc_r, vec2{400.0f, 200.0f}, 0.0f,
                                           true, -0.35f, 0.35f, false, 0.0f, 0.0f, false, pluma::Value{}));

    // Settle bridge under gravity
    for (int i = 0; i < 30; ++i) world.step(dt);

    // Kinematic player (mass 50kg, height 30, width 16) starts above plank 0
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {130.0f, 175.0f};
    pl_cfg.mass = 50.0f;
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {16.0f, 30.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    // Walk across bridge from left to right (vx = 80 px/s)
    for (int tick = 0; tick < 180; ++tick) {
        world.step(dt);
        SlideResult res = world.move_and_slide(pl, vec2{80.0f, 980.0f * dt}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);
    }

    vec2 final_pl_p = world.get_body_position(pl);
    CHECK(final_pl_p.x() > 300.0f);

    for (uint32_t j : joints) world.destroy_joint(j);
}

TEST_CASE("physics: player interaction - spring pad rebound and vertical launch") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    const float dt = 1.0f / 60.0f;

    // Static base at y = 300
    BodyConfig base_cfg;
    base_cfg.kind = BodyKind::Static;
    base_cfg.position = {200.0f, 300.0f};
    uint32_t base = world.create_body(base_cfg, pluma::Value{});

    // Dynamic pad at y = 250 (rest length 50), mass 2kg, size 80x16
    BodyConfig pad_cfg;
    pad_cfg.kind = BodyKind::Dynamic;
    pad_cfg.position = {200.0f, 250.0f};
    pad_cfg.mass = 2.0f;
    pad_cfg.lock_rotation = true;
    uint32_t pad = world.create_body(pad_cfg, pluma::Value{});
    ShapeConfig pad_s;
    pad_s.kind = ShapeKind::Box;
    pad_s.size = {80.0f, 16.0f};
    world.add_shape(pad, pad_s, pluma::Value{});

    // Damped spring joint
    uint32_t spring = world.create_distance_joint(base, pad, vec2{200.0f, 300.0f}, vec2{200.0f, 250.0f},
                                                  50.0f, 0.0f, 100.0f, 120.0f, 0.3f, false, pluma::Value{});
    REQUIRE(spring != 0);

    for (int i = 0; i < 20; ++i) world.step(dt);

    // Player falls from y = 150 onto pad
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {200.0f, 150.0f};
    pl_cfg.mass = 40.0f;
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {20.0f, 30.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    float max_pad_compression_y = 0.0f;
    float min_pl_rebound_y = 300.0f;

    vec2 pl_vel = {0.0f, 0.0f};
    for (int tick = 0; tick < 60; ++tick) {
        world.step(dt);
        pl_vel = pl_vel + vec2{0.0f, 980.0f * dt};
        SlideResult res = world.move_and_slide(pl, pl_vel, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);
        pl_vel = res.velocity;

        vec2 pad_p = world.get_body_position(pad);
        if (pad_p.y() > max_pad_compression_y) {
            max_pad_compression_y = pad_p.y();
        }
        if (tick > 20 && res.position.y() < min_pl_rebound_y) {
            min_pl_rebound_y = res.position.y();
        }
    }

    CHECK(max_pad_compression_y > 255.0f);
    // Player rebounded upward into the air above the pad
    CHECK(min_pl_rebound_y < 235.0f);

    world.destroy_joint(spring);
}

TEST_CASE("physics: player interaction - turnstile push and angular limit stop") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});
    const float dt = 1.0f / 60.0f;

    // Static anchor at (200, 200)
    BodyConfig anc_cfg;
    anc_cfg.kind = BodyKind::Static;
    anc_cfg.position = {200.0f, 200.0f};
    uint32_t anc = world.create_body(anc_cfg, pluma::Value{});

    // Dynamic bar centered at (200, 160), size 16x80
    BodyConfig bar_cfg;
    bar_cfg.kind = BodyKind::Dynamic;
    bar_cfg.position = {200.0f, 160.0f};
    bar_cfg.mass = 2.0f;
    uint32_t bar = world.create_body(bar_cfg, pluma::Value{});
    ShapeConfig bar_s;
    bar_s.kind = ShapeKind::Box;
    bar_s.size = {16.0f, 80.0f};
    world.add_shape(bar, bar_s, pluma::Value{});

    // PinJoint with limit [0.0, 0.5 rad]
    uint32_t pin = world.create_pin_joint(anc, bar, vec2{200.0f, 200.0f}, 0.0f,
                                          true, 0.0f, 0.5f, false, 0.0f, 0.0f, false, pluma::Value{});
    REQUIRE(pin != 0);

    // Player at x = 160, y = 140
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {160.0f, 140.0f};
    pl_cfg.mass = 50.0f;
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {24.0f, 30.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    for (int tick = 0; tick < 40; ++tick) {
        world.step(dt);
        SlideResult res = world.move_and_slide(pl, vec2{120.0f, 0.0f}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);
    }

    float rot = world.get_body_rotation(bar);
    CHECK(rot >= 0.45f);
    CHECK(rot <= 0.55f);

    vec2 pl_p = world.get_body_position(pl);
    CHECK(pl_p.x() < 225.0f);

    world.destroy_joint(pin);
}

TEST_CASE("physics: player interaction - joint destruction during active ride") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    const float dt = 1.0f / 60.0f;

    // Static guide at (200, 200)
    BodyConfig guide_cfg;
    guide_cfg.kind = BodyKind::Static;
    guide_cfg.position = {200.0f, 200.0f};
    uint32_t guide = world.create_body(guide_cfg, pluma::Value{});

    // Dynamic car at (200, 200), held in place by prismatic joint
    BodyConfig car_cfg;
    car_cfg.kind = BodyKind::Dynamic;
    car_cfg.position = {200.0f, 200.0f};
    car_cfg.mass = 500.0f;
    car_cfg.lock_rotation = true;
    uint32_t car = world.create_body(car_cfg, pluma::Value{});
    ShapeConfig car_s;
    car_s.kind = ShapeKind::Box;
    car_s.size = {80.0f, 20.0f}; // top edge at y = 190
    world.add_shape(car, car_s, pluma::Value{});

    // Prismatic joint locked at y = 200 with zero limits
    uint32_t joint = world.create_prismatic_joint(guide, car, vec2{200.0f, 200.0f}, vec2{0.0f, 1.0f},
                                                  false, 0.0f, 0.0f, 0.0f,
                                                  true, 0.0f, 0.0f, false, 0.0f, 0.0f, false, pluma::Value{});
    REQUIRE(joint != 0);

    // Player standing on car at y = 170 (bottom edge at 190)
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {200.0f, 170.0f};
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {20.0f, 40.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    // Step 20 ticks: car is held firmly in place by joint
    for (int i = 0; i < 20; ++i) {
        world.step(dt);
        SlideResult res = world.move_and_slide(pl, vec2{0.0f, 980.0f * dt}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);
        CHECK(res.on_floor);
    }
    CHECK(world.get_body_position(car).y() == doctest::Approx(200.0f).epsilon(0.01f));

    // Mid-ride: Destroy joint!
    world.destroy_joint(joint);
    world.set_body_awake(car, true);

    // Step another 20 ticks: car and player must now freely fall together under gravity
    for (int i = 0; i < 20; ++i) {
        world.step(dt);
        SlideResult res = world.move_and_slide(pl, vec2{0.0f, 980.0f * dt}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        world.set_body_transform(pl, res.position, 0.0f);
    }

    vec2 final_car_p = world.get_body_position(car);
    vec2 final_pl_p = world.get_body_position(pl);

    // Car and player must both have fallen under gravity after joint was severed
    CHECK(final_car_p.y() > 220.0f);
    CHECK(final_pl_p.y() > 200.0f);
}

TEST_CASE("physics: edge cases - BodyConfig gravity_scale, non-dynamic force guards, and NaN/Inf rejection") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    const float dt = 1.0f / 60.0f;

    // 1. BodyConfig::gravity_scale is respected at creation time
    BodyConfig zero_g_cfg;
    zero_g_cfg.kind = BodyKind::Dynamic;
    zero_g_cfg.position = {100.0f, 100.0f};
    zero_g_cfg.gravity_scale = 0.0f;
    uint32_t zero_g = world.create_body(zero_g_cfg, pluma::Value{});
    ShapeConfig box_s;
    box_s.kind = ShapeKind::Box;
    box_s.size = {20.0f, 20.0f};
    world.add_shape(zero_g, box_s, pluma::Value{});

    // 2. Static and Kinematic bodies safely ignore forces/impulses/torques without asserting
    BodyConfig stat_cfg;
    stat_cfg.kind = BodyKind::Static;
    stat_cfg.position = {200.0f, 100.0f};
    uint32_t stat_b = world.create_body(stat_cfg, pluma::Value{});
    world.add_shape(stat_b, box_s, pluma::Value{});

    BodyConfig kin_cfg;
    kin_cfg.kind = BodyKind::Kinematic;
    kin_cfg.position = {300.0f, 100.0f};
    uint32_t kin_b = world.create_body(kin_cfg, pluma::Value{});
    world.add_shape(kin_b, box_s, pluma::Value{});

    for (uint32_t h : {stat_b, kin_b}) {
        world.apply_force(h, vec2{1000.0f, -1000.0f});
        world.apply_impulse(h, vec2{500.0f, -500.0f});
        world.apply_force_at_point(h, vec2{1000.0f, 0.0f}, vec2{200.0f, 90.0f});
        world.apply_impulse_at_point(h, vec2{500.0f, 0.0f}, vec2{200.0f, 90.0f});
        world.apply_torque(h, 500.0f);
        world.apply_angular_impulse(h, 250.0f);
    }

    // 3. NaN/Inf inputs are rejected safely
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    const float inf_v = std::numeric_limits<float>::infinity();
    world.set_gravity(vec2{nan_v, 980.0f});
    CHECK(world.gravity().y() == doctest::Approx(980.0f));

    world.set_body_transform(zero_g, vec2{nan_v, 100.0f}, 0.0f);
    world.set_body_transform(zero_g, vec2{100.0f, 100.0f}, inf_v);
    world.set_body_linear_velocity(zero_g, vec2{nan_v, 0.0f});
    world.set_body_angular_velocity(zero_g, nan_v);
    world.set_body_mass(zero_g, -5.0f);
    world.set_body_mass(zero_g, 0.0f);
    world.set_body_mass(zero_g, nan_v);
    world.set_body_gravity_scale(zero_g, nan_v);
    world.set_body_damp(zero_g, nan_v, inf_v);
    world.apply_force(zero_g, vec2{nan_v, 0.0f});
    world.apply_impulse(zero_g, vec2{0.0f, inf_v});
    world.apply_torque(zero_g, nan_v);

    for (int i = 0; i < 10; ++i) {
        world.step(dt);
    }

    // Zero-gravity body did not fall, and static/kinematic bodies did not move
    CHECK(world.get_body_position(zero_g).y() == doctest::Approx(100.0f).epsilon(0.01f));
    CHECK(world.get_body_position(stat_b).x() == doctest::Approx(200.0f).epsilon(0.01f));
    CHECK(world.get_body_position(kin_b).x() == doctest::Approx(300.0f).epsilon(0.01f));
}

TEST_CASE("physics: edge cases - sensor mass isolation and unlocking rotation restores inertia") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});
    const float dt = 1.0f / 60.0f;

    // Create dynamic body with locked rotation initially
    BodyConfig cfg;
    cfg.kind = BodyKind::Dynamic;
    cfg.position = {200.0f, 200.0f};
    cfg.mass = 10.0f;
    cfg.lock_rotation = true;
    uint32_t b = world.create_body(cfg, pluma::Value{});

    ShapeConfig solid_s;
    solid_s.kind = ShapeKind::Box;
    solid_s.size = {40.0f, 20.0f};
    world.add_shape(b, solid_s, pluma::Value{});

    // Attach a large offset sensor shape; it must NOT shift center of mass or alter body mass
    ShapeConfig sensor_s;
    sensor_s.kind = ShapeKind::Box;
    sensor_s.size = {200.0f, 200.0f};
    sensor_s.offset = {150.0f, 0.0f};
    sensor_s.is_sensor = true;
    sensor_s.density = 100.0f;
    uint32_t sens_h = world.add_shape(b, sensor_s, pluma::Value{});
    REQUIRE(sens_h != 0);

    // Unlock rotation and apply torque: body should now rotate because inertia was restored from solid shapes
    world.set_body_motion_locks(b, false);
    world.apply_angular_impulse(b, 5000.0f);
    world.step(dt);
    CHECK(world.get_body_angular_velocity(b) > 0.1f);

    // Destroying sensor shape keeps dynamic body intact
    world.destroy_shape(sens_h);
    world.step(dt);
    CHECK(world.get_body_rotation(b) > 0.0f);
}

TEST_CASE("physics: edge cases - degenerate capsule, >8 vertex polygon, offset segment, and self-joint rejection") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    BodyConfig b_cfg;
    b_cfg.kind = BodyKind::Static;
    b_cfg.position = {100.0f, 100.0f};
    uint32_t b = world.create_body(b_cfg, pluma::Value{});

    // 1. Capsule with length <= 2 * radius falls back to circle instead of failing
    ShapeConfig cap_s;
    cap_s.kind = ShapeKind::Capsule;
    cap_s.radius = 20.0f;
    cap_s.length = 10.0f; // smaller than 2*radius (40)
    uint32_t cap_h = world.add_shape(b, cap_s, pluma::Value{});
    CHECK(cap_h != 0);

    // ShapeCast with degenerate capsule also succeeds
    ShapeCastHit sc_hit = world.cast_shape(cap_s, vec2{100.0f, 40.0f}, vec2{0.0f, 60.0f}, 1, 0);
    CHECK(sc_hit.hit);

    // 2. Polygon with > 8 vertices (12-gon) subsamples hull and creates valid polygon shape
    ShapeConfig poly_s;
    poly_s.kind = ShapeKind::Polygon;
    for (int i = 0; i < 12; ++i) {
        const float angle = static_cast<float>(i) * (2.0f * 3.14159265f / 12.0f);
        poly_s.points.push_back(vec2{std::cos(angle) * 25.0f, std::sin(angle) * 25.0f});
    }
    uint32_t poly_h = world.add_shape(b, poly_s, pluma::Value{});
    CHECK(poly_h != 0);

    // 3. Segment with offset and rotation
    ShapeConfig seg_s;
    seg_s.kind = ShapeKind::Segment;
    seg_s.offset = {0.0f, 100.0f};
    seg_s.point_a = {-30.0f, 0.0f};
    seg_s.point_b = {30.0f, 0.0f};
    uint32_t seg_h = world.add_shape(b, seg_s, pluma::Value{});
    CHECK(seg_h != 0);
    RaycastHit rhit = world.cast_ray(vec2{100.0f, 180.0f}, vec2{0.0f, 40.0f}, 1, 0);
    CHECK(rhit.hit);
    CHECK(rhit.point.y() == doctest::Approx(200.0f).epsilon(0.5f));

    // 4. Zero-length raycast/shapecast safely return hit = false
    CHECK_FALSE(world.cast_ray(vec2{100.0f, 100.0f}, vec2{0.0f, 0.0f}, 1, 0).hit);
    CHECK_FALSE(world.cast_shape(cap_s, vec2{100.0f, 100.0f}, vec2{0.0f, 0.0f}, 1, 0).hit);

    // 5. Self-joints (body_a == body_b) are rejected for all joint types
    CHECK(world.create_pin_joint(b, b, vec2{100.0f, 100.0f}, 0.0f, false, 0.0f, 0.0f, false, 0.0f, 0.0f, false, pluma::Value{}) == 0);
    CHECK(world.create_distance_joint(b, b, vec2{100.0f, 100.0f}, vec2{110.0f, 100.0f}, 10.0f, 0.0f, 20.0f, 0.0f, 0.0f, false, pluma::Value{}) == 0);
    CHECK(world.create_prismatic_joint(b, b, vec2{100.0f, 100.0f}, vec2{1.0f, 0.0f}, false, 0.0f, 0.0f, 0.0f, false, 0.0f, 0.0f, false, 0.0f, 0.0f, false, pluma::Value{}) == 0);
    CHECK(world.create_weld_joint(b, b, vec2{100.0f, 100.0f}, 0.0f, 0.0f, 0.0f, 0.0f, false, pluma::Value{}) == 0);
    CHECK(world.create_wheel_joint(b, b, vec2{100.0f, 100.0f}, vec2{0.0f, 1.0f}, false, 0.0f, 0.0f, false, 0.0f, 0.0f, false, 0.0f, 0.0f, false, pluma::Value{}) == 0);
    CHECK(world.create_motor_joint(b, b, vec2{0.0f, 0.0f}, 0.0f, 10.0f, 10.0f, 1.0f, 0.5f, 10.0f, 1.0f, 0.5f, 10.0f, false, pluma::Value{}) == 0);
    CHECK(world.create_mover_joint(b, b, vec2{10.0f, 0.0f}, vec2{10.0f, 10.0f}, false, pluma::Value{}) == 0);
    CHECK(world.create_pogo_joint(b, b, vec2{100.0f, 100.0f}, vec2{0.0f, 1.0f}, 5.0f, 0.5f, 20.0f, 100.0f, 100.0f, false, pluma::Value{}) == 0);
    CHECK(world.create_filter_joint(b, b, pluma::Value{}) == 0);
}

TEST_CASE("physics: conveyor belt StaticBody2D carries both CharacterBody2D and RigidBody2D") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    const float dt = 1.0f / 60.0f;

    // Static conveyor belt at y = 300, width 400, height 20, moving right at +100 px/s
    BodyConfig belt_cfg;
    belt_cfg.kind = BodyKind::Static;
    belt_cfg.position = {200.0f, 300.0f};
    uint32_t belt = world.create_body(belt_cfg, pluma::Value{});
    world.set_body_constant_linear_velocity(belt, vec2{100.0f, 0.0f});

    ShapeConfig belt_s;
    belt_s.kind = ShapeKind::Box;
    belt_s.size = {400.0f, 20.0f};
    belt_s.friction = 0.9f;
    world.add_shape(belt, belt_s, pluma::Value{});

    // 1. Dynamic RigidBody2D box resting on the left half of the belt (x = 100, y = 280)
    BodyConfig box_cfg;
    box_cfg.kind = BodyKind::Dynamic;
    box_cfg.position = {100.0f, 280.0f};
    box_cfg.mass = 10.0f;
    uint32_t box = world.create_body(box_cfg, pluma::Value{});
    ShapeConfig box_s;
    box_s.kind = ShapeKind::Box;
    box_s.size = {20.0f, 20.0f};
    box_s.friction = 0.9f;
    world.add_shape(box, box_s, pluma::Value{});

    // 2. Kinematic CharacterBody2D standing on the right half of the belt (x = 250, y = 275)
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {250.0f, 275.0f};
    uint32_t ch = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig ch_s;
    ch_s.kind = ShapeKind::Box;
    ch_s.size = {20.0f, 30.0f};
    world.add_shape(ch, ch_s, pluma::Value{});

    for (int i = 0; i < 30; ++i) {
        world.step(dt);
        SlideResult res = world.move_and_slide(ch, vec2{0.0f, 980.0f * dt}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        CHECK(res.on_floor);
        CHECK(res.floor_velocity.x() == doctest::Approx(100.0f).epsilon(0.01f));
    }

    // Both the RigidBody2D and the CharacterBody2D should have been carried rightward by the conveyor belt
    CHECK(world.get_body_position(box).x() > 110.0f);
    CHECK(world.get_body_position(ch).x() > 280.0f);
}

TEST_CASE("physics: CharacterBody2D pushes center-pivoted hinged RigidBody2D and stops in acute corner") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});
    const float dt = 1.0f / 60.0f;

    // 1. Center-pivoted turnstile: pivot is AT the bar's center of mass (200, 200)
    BodyConfig anchor_cfg;
    anchor_cfg.kind = BodyKind::Static;
    anchor_cfg.position = {200.0f, 200.0f};
    uint32_t anchor = world.create_body(anchor_cfg, pluma::Value{});

    BodyConfig bar_cfg;
    bar_cfg.kind = BodyKind::Dynamic;
    bar_cfg.position = {200.0f, 200.0f}; // COM is right on the pin anchor!
    bar_cfg.mass = 15.0f;
    bar_cfg.lock_rotation = false;
    uint32_t bar = world.create_body(bar_cfg, pluma::Value{});
    ShapeConfig bar_s;
    bar_s.kind = ShapeKind::Box;
    bar_s.size = {12.0f, 100.0f}; // extends from y = 150 to y = 250
    world.add_shape(bar, bar_s, pluma::Value{});

    uint32_t pin = world.create_pin_joint(anchor, bar, vec2{200.0f, 200.0f}, 0.0f,
                                          false, 0.0f, 0.0f, false, 0.0f, 0.0f, false, pluma::Value{});
    REQUIRE(pin != 0);

    // CharacterBody2D walks right into the top half of the bar (at y = 165, off-center from pivot y = 200)
    BodyConfig pl_cfg;
    pl_cfg.kind = BodyKind::Kinematic;
    pl_cfg.position = {170.0f, 165.0f};
    pl_cfg.mass = 50.0f;
    uint32_t pl = world.create_body(pl_cfg, pluma::Value{});
    ShapeConfig pl_s;
    pl_s.kind = ShapeKind::Box;
    pl_s.size = {20.0f, 20.0f};
    world.add_shape(pl, pl_s, pluma::Value{});

    for (int i = 0; i < 15; ++i) {
        SlideResult res = world.move_and_slide(pl, vec2{150.0f, 0.0f}, dt,
                                              vec2{0.0f, -1.0f}, 0.785f, 4);
        (void)res;
        world.step(dt);
    }

    // Even though the bar's COM is pinned at (200, 200), pushing at y = 165 applies off-center impulse and rotates it!
    CHECK(std::abs(world.get_body_rotation(bar)) > 0.05f);

    // 2. Acute corner / V-wedge stop test
    BodyConfig wedge_cfg;
    wedge_cfg.kind = BodyKind::Static;
    wedge_cfg.position = {500.0f, 200.0f};
    uint32_t wedge = world.create_body(wedge_cfg, pluma::Value{});

    ShapeConfig top_wall;
    top_wall.kind = ShapeKind::Segment;
    top_wall.point_a = {-100.0f, -40.0f};
    top_wall.point_b = {0.0f, 0.0f};
    world.add_shape(wedge, top_wall, pluma::Value{});

    ShapeConfig bot_wall;
    bot_wall.kind = ShapeKind::Segment;
    bot_wall.point_a = {-100.0f, 40.0f};
    bot_wall.point_b = {0.0f, 0.0f};
    world.add_shape(wedge, bot_wall, pluma::Value{});

    BodyConfig runner_cfg;
    runner_cfg.kind = BodyKind::Kinematic;
    runner_cfg.position = {430.0f, 200.0f};
    uint32_t runner = world.create_body(runner_cfg, pluma::Value{});
    ShapeConfig run_s;
    run_s.kind = ShapeKind::Circle;
    run_s.radius = 12.0f;
    world.add_shape(runner, run_s, pluma::Value{});

    for (int i = 0; i < 30; ++i) {
        world.move_and_slide(runner, vec2{200.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
    }
    // Runner must be stopped cleanly inside the V-wedge before reaching the apex at x = 500
    CHECK(world.get_body_position(runner).x() < 480.0f);
}

TEST_CASE("physics: character on vertical moving platform and suspension bridge seams") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    const float dt = 1.0f / 60.0f;

    // 1. Vertical oscillating kinematic platform
    BodyConfig plat_cfg;
    plat_cfg.kind = BodyKind::Kinematic;
    plat_cfg.position = {400.0f, 420.0f};
    uint32_t plat = world.create_body(plat_cfg, pluma::Value{});
    ShapeConfig plat_s;
    plat_s.kind = ShapeKind::Box;
    plat_s.size = {110.0f, 24.0f};
    world.add_shape(plat, plat_s, pluma::Value{});

    BodyConfig ch_cfg;
    ch_cfg.kind = BodyKind::Kinematic;
    ch_cfg.position = {400.0f, 380.0f};
    ch_cfg.mass = 50.0f;
    uint32_t ch = world.create_body(ch_cfg, pluma::Value{});
    ShapeConfig ch_s;
    ch_s.kind = ShapeKind::Box;
    ch_s.size = {32.0f, 56.0f};
    world.add_shape(ch, ch_s, pluma::Value{});

    vec2 ch_vel = {0.0f, 0.0f};
    bool landed = false;
    for (int tick = 1; tick <= 180; ++tick) {
        const float old_y = world.get_body_position(plat).y();
        const float new_y = 420.0f + std::sin(static_cast<float>(tick) * dt * 1.4f) * 120.0f;
        world.set_body_linear_velocity(plat, vec2{0.0f, (new_y - old_y) / dt});

        if (!landed) {
            ch_vel = {0.0f, ch_vel.y() + 980.0f * dt};
        }
        SlideResult res = world.move_and_slide(ch, ch_vel, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
        ch_vel = res.velocity;
        if (res.on_floor) landed = true;

        world.step(dt);
        world.set_body_transform(plat, vec2{400.0f, new_y}, 0.0f);

        if (landed && tick > 30) {
            CHECK(res.on_floor);
            const float feet_y = res.position.y() + 28.0f;
            const float plat_top = new_y - 12.0f;
            CHECK(std::abs(feet_y - plat_top) < 4.0f);
        }
    }
    CHECK(landed);
}

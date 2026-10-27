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

TEST_CASE("physics: rigid body falls under gravity and rests on static floor") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Static floor at y = 100, size 200x20 (top edge at y = 90).
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    uint32_t floor_handle = world.create_body(floor_cfg, pluma::Value{});
    REQUIRE(floor_handle != 0);

    ShapeConfig floor_shape;
    floor_shape.kind = ShapeKind::Box;
    floor_shape.size = {200.0f, 20.0f};
    uint32_t floor_shape_handle = world.add_shape(floor_handle, floor_shape, pluma::Value{});
    REQUIRE(floor_shape_handle != 0);

    // Dynamic box at y = 0, size 20x20 (half-height 10).
    BodyConfig box_cfg;
    box_cfg.kind = BodyKind::Dynamic;
    box_cfg.position = {0.0f, 0.0f};
    box_cfg.mass = 1.0f;
    uint32_t box_handle = world.create_body(box_cfg, pluma::Value{});
    REQUIRE(box_handle != 0);

    ShapeConfig box_shape;
    box_shape.kind = ShapeKind::Box;
    box_shape.size = {20.0f, 20.0f};
    box_shape.density = 1.0f;
    box_shape.friction = 0.3f;
    uint32_t box_shape_handle = world.add_shape(box_handle, box_shape, pluma::Value{});
    REQUIRE(box_shape_handle != 0);

    // Simulate for 1.5 seconds (90 ticks at 60 Hz).
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 90; ++i) {
        world.step(dt);
    }

    vec2 pos = world.get_body_position(box_handle);
    vec2 vel = world.get_body_linear_velocity(box_handle);

    // The box should rest on top of the floor at y ≈ 80 (90 - 10), with small slop tolerance.
    CHECK(pos.y() > 75.0f);
    CHECK(pos.y() < 85.0f);
    CHECK(std::abs(pos.x()) < 1.0f);
    // Vertical velocity should be settled near 0.
    CHECK(std::abs(vel.y()) < 5.0f);
}

TEST_CASE("physics: deterministic state hash across worker counts") {
    auto run_sim = [](unsigned workers) -> std::uint64_t {
        PhysicsWorld world(workers, vec2{0.0f, 980.0f});

        BodyConfig floor_cfg;
        floor_cfg.kind = BodyKind::Static;
        floor_cfg.position = {0.0f, 200.0f};
        uint32_t f = world.create_body(floor_cfg, pluma::Value{});
        ShapeConfig f_shape;
        f_shape.kind = ShapeKind::Box;
        f_shape.size = {400.0f, 20.0f};
        world.add_shape(f, f_shape, pluma::Value{});

        // Add 5 dynamic boxes in a stack.
        for (int i = 0; i < 5; ++i) {
            BodyConfig b_cfg;
            b_cfg.kind = BodyKind::Dynamic;
            b_cfg.position = {static_cast<float>(i * 4 - 8), static_cast<float>(i * 30)};
            b_cfg.mass = 2.0f;
            uint32_t b = world.create_body(b_cfg, pluma::Value{});
            ShapeConfig b_shape;
            b_shape.kind = ShapeKind::Box;
            b_shape.size = {24.0f, 24.0f};
            b_shape.density = 1.0f;
            world.add_shape(b, b_shape, pluma::Value{});
        }

        const float dt = 1.0f / 60.0f;
        for (int tick = 0; tick < 120; ++tick) {
            world.step(dt);
        }
        return world.state_hash();
    };

    const std::uint64_t hash_1_worker = run_sim(1);
    const std::uint64_t hash_4_workers = run_sim(4);

    CHECK(hash_1_worker != 0);
    CHECK(hash_1_worker == hash_4_workers);
}

TEST_CASE("physics: raycast query hit and miss") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    BodyConfig obs_cfg;
    obs_cfg.kind = BodyKind::Static;
    obs_cfg.position = {100.0f, 0.0f};
    uint32_t obs = world.create_body(obs_cfg, pluma::Value{});
    ShapeConfig obs_shape;
    obs_shape.kind = ShapeKind::Box;
    obs_shape.size = {20.0f, 100.0f}; // x from 90 to 110
    world.add_shape(obs, obs_shape, pluma::Value{});

    // Cast ray towards the obstacle.
    RaycastHit hit = world.cast_ray(vec2{0.0f, 0.0f}, vec2{200.0f, 0.0f}, 1, 0);
    CHECK(hit.hit);
    CHECK(hit.point.x() >= 89.0f);
    CHECK(hit.point.x() <= 91.0f);
    CHECK(hit.normal.x() < 0.0f); // Normal points towards origin (-1, 0)
    CHECK(hit.body_handle == obs);

    // Cast ray missing the obstacle.
    RaycastHit miss = world.cast_ray(vec2{0.0f, -100.0f}, vec2{200.0f, 0.0f}, 1, 0);
    CHECK_FALSE(miss.hit);
}

TEST_CASE("physics: CharacterBody2D move_and_slide detection") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Static floor at y = 100, height 20 (top edge at y = 90).
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {300.0f, 20.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Kinematic character body at y = 60, height 40 (bottom edge at y = 80).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {0.0f, 60.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 40.0f}; // half-height 20
    world.add_shape(char_b, char_s, pluma::Value{});

    // Move character down into floor with 1200 px/s downward motion (20 px in 1/60s).
    // Gap to floor top is 10 px (80 -> 90), so it collides halfway and lands with center at y ≈ 70.
    SlideResult res = world.move_and_slide(char_b, vec2{0.0f, 1200.0f}, 1.0f / 60.0f,
                                          vec2{0.0f, -1.0f}, 0.785f, 4);

    CHECK(res.collided);
    CHECK(res.on_floor);
    CHECK_FALSE(res.on_ceiling);
    CHECK(res.floor_normal.y() < -0.9f);
    // Character center lands at y ≈ 70 (floor top 90 - half-height 20).
    CHECK(res.position.y() >= 69.0f);
    CHECK(res.position.y() <= 71.0f);
    // Vertical velocity is stopped.
    CHECK(res.velocity.y() == 0.0f);
}

TEST_CASE("physics: CharacterBody2D pushes dynamic body") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Dynamic box at x = 50, y = 0, size 20x20 (left edge at x = 40).
    BodyConfig box_cfg;
    box_cfg.kind = BodyKind::Dynamic;
    box_cfg.position = {50.0f, 0.0f};
    box_cfg.mass = 1.0f;
    uint32_t box_b = world.create_body(box_cfg, pluma::Value{});
    ShapeConfig box_s;
    box_s.kind = ShapeKind::Box;
    box_s.size = {20.0f, 20.0f};
    world.add_shape(box_b, box_s, pluma::Value{});

    // Character at x = 20, y = 0, size 20x20 (right edge at x = 30).
    // Gap to box left edge is 10 px. Moving right at 1200 px/s (20 px in 1/60s) collides and pushes it.
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {20.0f, 0.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 20.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    SlideResult res = world.move_and_slide(char_b, vec2{1200.0f, 0.0f}, 1.0f / 60.0f,
                                          vec2{0.0f, -1.0f}, 0.785f, 4);

    CHECK(res.collided);
    CHECK(res.on_wall);

    // Verify impulse pushed the dynamic box to the right.
    vec2 box_vel = world.get_body_linear_velocity(box_b);
    CHECK(box_vel.x() > 50.0f);
}

TEST_CASE("physics: PinJoint2D and DistanceJoint2D creation") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    BodyConfig b1_cfg;
    b1_cfg.kind = BodyKind::Static;
    b1_cfg.position = {0.0f, 0.0f};
    uint32_t b1 = world.create_body(b1_cfg, pluma::Value{});

    BodyConfig b2_cfg;
    b2_cfg.kind = BodyKind::Dynamic;
    b2_cfg.position = {0.0f, 50.0f};
    uint32_t b2 = world.create_body(b2_cfg, pluma::Value{});

    uint32_t pin = world.create_pin_joint(b1, b2, vec2{0.0f, 0.0f}, 0.0f, false, 0.0f, 0.0f,
                                         false, 0.0f, 0.0f, pluma::Value{});
    CHECK(pin != 0);

    BodyConfig b3_cfg;
    b3_cfg.kind = BodyKind::Dynamic;
    b3_cfg.position = {50.0f, 50.0f};
    uint32_t b3 = world.create_body(b3_cfg, pluma::Value{});

    uint32_t dist = world.create_distance_joint(b2, b3, vec2{0.0f, 50.0f}, vec2{50.0f, 50.0f},
                                                50.0f, 40.0f, 60.0f, 10.0f, 0.5f, pluma::Value{});
    CHECK(dist != 0);

    // Step simulation and verify no crashes.
    world.step(1.0f / 60.0f);
    CHECK(world.state_hash() != 0);

    world.destroy_joint(pin);
    world.destroy_joint(dist);
}

TEST_CASE("physics: CharacterBody2D jumping on dynamic body") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Static floor at y = 100, height 20 (top edge at y = 90).
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {300.0f, 20.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Dynamic box at x = 0, y = 70, size 40x40 (top edge at y = 50, bottom at y = 90).
    BodyConfig box_cfg;
    box_cfg.kind = BodyKind::Dynamic;
    box_cfg.position = {0.0f, 70.0f};
    box_cfg.mass = 2.0f;
    uint32_t box_b = world.create_body(box_cfg, pluma::Value{});
    ShapeConfig box_s;
    box_s.kind = ShapeKind::Box;
    box_s.size = {40.0f, 40.0f};
    world.add_shape(box_b, box_s, pluma::Value{});

    // Settle dynamic box onto floor with a few physics steps.
    for (int i = 0; i < 10; ++i) world.step(1.0f / 60.0f);

    // Kinematic character body at x = 0, y = 20, size 20x40 (half-height 20, bottom at y = 40).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {0.0f, 20.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 40.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    // Move character down onto the box.
    SlideResult res = world.move_and_slide(char_b, vec2{0.0f, 1200.0f}, 1.0f / 60.0f,
                                          vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(res.collided);
    CHECK(res.on_floor);

    // Step physics, then character rests on box.
    world.step(1.0f / 60.0f);
    res = world.move_and_slide(char_b, vec2{0.0f, 0.0f}, 1.0f / 60.0f,
                               vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(res.on_floor);

    // Step physics, then character moves horizontally on box.
    world.step(1.0f / 60.0f);
    res = world.move_and_slide(char_b, vec2{100.0f, 0.0f}, 1.0f / 60.0f,
                               vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(res.on_floor);

    // Jump from top of box.
    SlideResult jump_res = world.move_and_slide(char_b, vec2{0.0f, -520.0f}, 1.0f / 60.0f,
                                                vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK_FALSE(jump_res.on_floor);
    CHECK(jump_res.velocity.y() < -500.0f);
    CHECK(jump_res.position.y() < res.position.y());
}

TEST_CASE("physics: CharacterBody2D continuous pushing on floor") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Floor at y = 100, height 20 (top at y = 90).
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {600.0f, 20.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Crate at x = 100, y = 70, size 40x40, mass = 2.0, lock_rotation = true
    BodyConfig crate_cfg;
    crate_cfg.kind = BodyKind::Dynamic;
    crate_cfg.position = {100.0f, 70.0f};
    crate_cfg.mass = 2.0f;
    crate_cfg.lock_rotation = true;
    uint32_t crate_b = world.create_body(crate_cfg, pluma::Value{});
    ShapeConfig crate_s;
    crate_s.kind = ShapeKind::Box;
    crate_s.size = {40.0f, 40.0f};
    world.add_shape(crate_b, crate_s, pluma::Value{});
    world.set_body_motion_locks(crate_b, true);
    world.set_body_mass(crate_b, 2.0f);

    // Let crate settle on floor
    for (int i = 0; i < 15; ++i) world.step(1.0f / 60.0f);
    const float initial_crate_x = world.get_body_position(crate_b).x();

    // Player character at x = 40, size 32x56 (right edge at 56, gap to crate is 24 px).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {40.0f, 62.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {32.0f, 56.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    // Run player right at 300 px/s for 30 ticks (0.5 seconds).
    const float dt = 1.0f / 60.0f;
    for (int t = 0; t < 30; ++t) {
        world.move_and_slide(char_b, vec2{300.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
        world.step(dt);
    }

    const float final_crate_x = world.get_body_position(crate_b).x();
    const float final_char_x = world.get_body_position(char_b).x();

    // Crate must have moved significantly right (> 50 pixels).
    CHECK(final_crate_x - initial_crate_x > 50.0f);
    // Player must have moved right alongside crate.
    CHECK(final_char_x > 100.0f);
    // Player and crate must not penetrate: crate center minus player center >= (16 + 20) = 36.
    CHECK(final_crate_x - final_char_x >= 35.5f);
}

TEST_CASE("physics: CharacterBody2D pushing dynamic crate against wall stops cleanly") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Floor at y = 100, height 20 (top at y = 90).
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {600.0f, 20.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Wall at x = 150, y = 50, width 20, height 120 (left edge at x = 140).
    BodyConfig wall_cfg;
    wall_cfg.kind = BodyKind::Static;
    wall_cfg.position = {150.0f, 50.0f};
    uint32_t wall_b = world.create_body(wall_cfg, pluma::Value{});
    ShapeConfig wall_s;
    wall_s.kind = ShapeKind::Box;
    wall_s.size = {20.0f, 120.0f};
    world.add_shape(wall_b, wall_s, pluma::Value{});

    // Crate at x = 100, y = 70, size 40x40 (right edge at 120, 20 px away from wall).
    BodyConfig crate_cfg;
    crate_cfg.kind = BodyKind::Dynamic;
    crate_cfg.position = {100.0f, 70.0f};
    crate_cfg.mass = 2.0f;
    crate_cfg.lock_rotation = true;
    uint32_t crate_b = world.create_body(crate_cfg, pluma::Value{});
    ShapeConfig crate_s;
    crate_s.kind = ShapeKind::Box;
    crate_s.size = {40.0f, 40.0f};
    world.add_shape(crate_b, crate_s, pluma::Value{});
    world.set_body_mass(crate_b, 2.0f);
    world.set_body_motion_locks(crate_b, true);

    // Player at x = 40, size 32x56.
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {40.0f, 62.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {32.0f, 56.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    // Push right for 60 ticks (1.0 second).
    const float dt = 1.0f / 60.0f;
    for (int t = 0; t < 60; ++t) {
        world.move_and_slide(char_b, vec2{300.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
        world.step(dt);
    }

    const float crate_x = world.get_body_position(crate_b).x();
    const float char_x = world.get_body_position(char_b).x();

    // Crate right edge is at 140 (wall left edge), so crate center must be <= 120.5.
    CHECK(crate_x <= 121.0f);
    CHECK(crate_x >= 118.0f);
    // Player right edge is at crate left edge (120 - 20 = 100), so player center <= 85.0 (100 - 16).
    CHECK(char_x <= 85.0f);
    CHECK(char_x >= 82.0f);
}

TEST_CASE("physics: CharacterBody2D ceiling collision and separation") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Ceiling at y = 0, size 200x20 (bottom edge at y = 10).
    BodyConfig ceil_cfg;
    ceil_cfg.kind = BodyKind::Static;
    ceil_cfg.position = {0.0f, 0.0f};
    uint32_t ceil_b = world.create_body(ceil_cfg, pluma::Value{});
    ShapeConfig ceil_s;
    ceil_s.kind = ShapeKind::Box;
    ceil_s.size = {200.0f, 20.0f};
    world.add_shape(ceil_b, ceil_s, pluma::Value{});

    // Character at y = 50, size 20x40 (top edge at y = 30).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {0.0f, 50.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 40.0f}; // half-height 20
    world.add_shape(char_b, char_s, pluma::Value{});

    // Jump up into ceiling at -1800 px/s (30 px in 1/60s). Gap is 20 px (30 -> 10).
    const float dt = 1.0f / 60.0f;
    SlideResult res = world.move_and_slide(char_b, vec2{0.0f, -1800.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);

    CHECK(res.collided);
    CHECK(res.on_ceiling);
    CHECK_FALSE(res.on_floor);
    // Top edge at 10 -> center at 30 (within slop).
    CHECK(res.position.y() >= 29.5f);
    CHECK(res.position.y() <= 30.5f);
    CHECK(res.velocity.y() == 0.0f);

    // Next tick: character starts falling down under gravity (+300 px/s).
    // Because motion is downwards (away from ceiling), ceiling does not impede motion.
    SlideResult fall_res = world.move_and_slide(char_b, vec2{0.0f, 300.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK_FALSE(fall_res.on_ceiling);
    CHECK(fall_res.position.y() > res.position.y());
}

TEST_CASE("physics: CharacterBody2D walkable slope and floor snap") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Create a 30-degree inclined floor using box with rotation.
    // 30 deg = 0.5236 rad < floor_max_angle (0.785 rad).
    BodyConfig slope_cfg;
    slope_cfg.kind = BodyKind::Static;
    slope_cfg.position = {100.0f, 100.0f};
    slope_cfg.rotation = 0.5236f;
    uint32_t slope_b = world.create_body(slope_cfg, pluma::Value{});
    ShapeConfig slope_s;
    slope_s.kind = ShapeKind::Box;
    slope_s.size = {300.0f, 20.0f};
    world.add_shape(slope_b, slope_s, pluma::Value{});

    // Character at y = 50, size 20x40.
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {100.0f, 50.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 40.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    // Drop onto slope with 1800 px/s (30 px in 1/60s). Gap is ~18.5 px.
    const float dt = 1.0f / 60.0f;
    SlideResult res = world.move_and_slide(char_b, vec2{0.0f, 1800.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(res.collided);
    CHECK(res.on_floor);
    CHECK_FALSE(res.on_wall);

    // Move horizontally down-slope. Floor snap keeps on_floor true.
    SlideResult walk_res = world.move_and_slide(char_b, vec2{-100.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(walk_res.on_floor);
}

TEST_CASE("physics: CharacterBody2D steep slope classified as wall") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // 60 degree slope (1.047 rad) > floor_max_angle (0.785 rad).
    BodyConfig slope_cfg;
    slope_cfg.kind = BodyKind::Static;
    slope_cfg.position = {100.0f, 100.0f};
    slope_cfg.rotation = 1.047f;
    uint32_t slope_b = world.create_body(slope_cfg, pluma::Value{});
    ShapeConfig slope_s;
    slope_s.kind = ShapeKind::Box;
    slope_s.size = {300.0f, 20.0f};
    world.add_shape(slope_b, slope_s, pluma::Value{});

    // Character at x = 70, size 20x40 (right edge at 80).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {70.0f, 100.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 40.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    const float dt = 1.0f / 60.0f;
    SlideResult res = world.move_and_slide(char_b, vec2{600.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(res.collided);
    CHECK(res.on_wall);
    CHECK_FALSE(res.on_floor);
}

TEST_CASE("physics: CharacterBody2D corner crevice collision") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Floor at y = 100, height 20 (top at y = 90).
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {200.0f, 20.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Wall at x = 100, y = 50, width 20 (left edge at x = 90).
    BodyConfig wall_cfg;
    wall_cfg.kind = BodyKind::Static;
    wall_cfg.position = {100.0f, 50.0f};
    uint32_t wall_b = world.create_body(wall_cfg, pluma::Value{});
    ShapeConfig wall_s;
    wall_s.kind = ShapeKind::Box;
    wall_s.size = {20.0f, 120.0f};
    world.add_shape(wall_b, wall_s, pluma::Value{});

    // Character at x = 60, y = 60, moving diagonally into the corner (right-down).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {60.0f, 60.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 20.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    const float dt = 1.0f / 60.0f;
    SlideResult res = world.move_and_slide(char_b, vec2{1800.0f, 1800.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(res.collided);
    CHECK(res.on_floor);
    CHECK(res.on_wall);
    // Character right edge at 90 (center at 80), bottom at 90 (center at 80).
    CHECK(res.position.x() <= 80.5f);
    CHECK(res.position.y() <= 80.5f);
}

TEST_CASE("physics: Stacked dynamic crates stability and interaction") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Floor at y = 150, size 300x20 (top edge at 140).
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 150.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {300.0f, 20.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Crate 1 (bottom) at x = 0, y = 115, size 30x30.
    BodyConfig c1_cfg;
    c1_cfg.kind = BodyKind::Dynamic;
    c1_cfg.position = {0.0f, 115.0f};
    c1_cfg.mass = 2.0f;
    c1_cfg.lock_rotation = true;
    uint32_t c1 = world.create_body(c1_cfg, pluma::Value{});
    ShapeConfig c1_s;
    c1_s.kind = ShapeKind::Box;
    c1_s.size = {30.0f, 30.0f};
    world.add_shape(c1, c1_s, pluma::Value{});
    world.set_body_mass(c1, 2.0f);
    world.set_body_motion_locks(c1, true);

    // Crate 2 (top) at x = 0, y = 80, size 30x30.
    BodyConfig c2_cfg;
    c2_cfg.kind = BodyKind::Dynamic;
    c2_cfg.position = {0.0f, 80.0f};
    c2_cfg.mass = 2.0f;
    c2_cfg.lock_rotation = true;
    uint32_t c2 = world.create_body(c2_cfg, pluma::Value{});
    ShapeConfig c2_s;
    c2_s.kind = ShapeKind::Box;
    c2_s.size = {30.0f, 30.0f};
    world.add_shape(c2, c2_s, pluma::Value{});
    world.set_body_mass(c2, 2.0f);
    world.set_body_motion_locks(c2, true);

    // Settle the stack.
    for (int i = 0; i < 30; ++i) world.step(1.0f / 60.0f);

    vec2 p1 = world.get_body_position(c1);
    vec2 p2 = world.get_body_position(c2);

    // Crate 1 resting on floor (top at 140, height 30 -> center at ~125).
    CHECK(p1.y() > 120.0f);
    CHECK(p1.y() < 130.0f);

    // Crate 2 resting on Crate 1 (center at ~95).
    CHECK(p2.y() > 90.0f);
    CHECK(p2.y() < 100.0f);

    // Horizontal drift should be negligible.
    CHECK(std::abs(p1.x()) < 1.0f);
    CHECK(std::abs(p2.x()) < 1.0f);

    // Kinematic character moves down onto Crate 2.
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {0.0f, 50.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 30.0f}; // bottom edge at y = 65
    world.add_shape(char_b, char_s, pluma::Value{});

    SlideResult res = world.move_and_slide(char_b, vec2{0.0f, 1800.0f}, 1.0f / 60.0f,
                                          vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(res.collided);
    CHECK(res.on_floor);
    REQUIRE_FALSE(res.collisions.empty());
    CHECK(res.collisions[0].collider_body == c2);
}

TEST_CASE("physics: mass data integrity with motion locks and shapes") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    BodyConfig cfg;
    cfg.kind = BodyKind::Dynamic;
    cfg.position = {0.0f, 0.0f};
    cfg.mass = 5.0f;
    uint32_t b = world.create_body(cfg, pluma::Value{});

    ShapeConfig s1;
    s1.kind = ShapeKind::Box;
    s1.size = {40.0f, 40.0f}; // default Box2D mass would be 1600.0
    uint32_t sh1 = world.add_shape(b, s1, pluma::Value{});
    REQUIRE(sh1 != 0);

    // Explicitly set mass to 2.5 kg.
    world.set_body_mass(b, 2.5f);

    // Apply linear impulse of 25.0 N*s. delta_v should be 25 / 2.5 = 10.0 px/s!
    world.apply_impulse(b, vec2{25.0f, 0.0f});
    vec2 vel = world.get_body_linear_velocity(b);
    CHECK(std::abs(vel.x() - 10.0f) < 0.1f);

    // Change motion locks (which in Box2D internally triggers mass recalculation).
    world.set_body_motion_locks(b, true);

    // Apply another impulse of 25.0 N*s. Velocity should increase to 20.0 px/s (NOT 10.01 px/s!).
    world.apply_impulse(b, vec2{25.0f, 0.0f});
    vel = world.get_body_linear_velocity(b);
    CHECK(std::abs(vel.x() - 20.0f) < 0.1f);
}

TEST_CASE("physics: collision layer and mask filtering") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Obstacle on layer 4 (mask 6 so it can collide with layer 2 or 4).
    BodyConfig obs_cfg;
    obs_cfg.kind = BodyKind::Static;
    obs_cfg.position = {100.0f, 0.0f};
    obs_cfg.collision_layer = 4;
    obs_cfg.collision_mask = 6;
    uint32_t obs = world.create_body(obs_cfg, pluma::Value{});
    ShapeConfig obs_s;
    obs_s.kind = ShapeKind::Box;
    obs_s.size = {20.0f, 100.0f};
    obs_s.layer = 4;
    obs_s.mask = 6;
    world.add_shape(obs, obs_s, pluma::Value{});

    // Character on layer 2 (mask 2).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {50.0f, 0.0f};
    char_cfg.collision_layer = 2;
    char_cfg.collision_mask = 2;
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 20.0f};
    char_s.layer = 2;
    char_s.mask = 2;
    world.add_shape(char_b, char_s, pluma::Value{});

    // Move character through obstacle (mask 2 does not overlap layer 4).
    const float dt = 1.0f / 60.0f;
    SlideResult res = world.move_and_slide(char_b, vec2{6000.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK_FALSE(res.collided);
    CHECK(res.position.x() >= 140.0f); // Passed cleanly through

    // Now update character mask to include layer 4.
    world.destroy_body(char_b);
    char_cfg.collision_mask = 6; // 2 | 4
    char_b = world.create_body(char_cfg, pluma::Value{});
    char_s.mask = 6;
    world.add_shape(char_b, char_s, pluma::Value{});

    SlideResult hit_res = world.move_and_slide(char_b, vec2{6000.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
    CHECK(hit_res.collided);
    CHECK(hit_res.on_wall);
    CHECK(hit_res.position.x() <= 90.5f); // Stopped by obstacle left edge at 90
}

TEST_CASE("physics: dynamic crate rapid direction reversal") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Floor at y = 100, size 800x20.
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_s;
    floor_s.kind = ShapeKind::Box;
    floor_s.size = {800.0f, 20.0f};
    world.add_shape(floor_b, floor_s, pluma::Value{});

    // Crate at x = 0, y = 70, size 40x40.
    BodyConfig crate_cfg;
    crate_cfg.kind = BodyKind::Dynamic;
    crate_cfg.position = {0.0f, 70.0f};
    crate_cfg.mass = 2.0f;
    crate_cfg.lock_rotation = true;
    uint32_t crate_b = world.create_body(crate_cfg, pluma::Value{});
    ShapeConfig crate_s;
    crate_s.kind = ShapeKind::Box;
    crate_s.size = {40.0f, 40.0f};
    world.add_shape(crate_b, crate_s, pluma::Value{});
    world.set_body_mass(crate_b, 2.0f);
    world.set_body_motion_locks(crate_b, true);

    for (int i = 0; i < 15; ++i) world.step(1.0f / 60.0f);

    // Player character pushing from left at x = -40.
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {-40.0f, 62.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {32.0f, 56.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    const float dt = 1.0f / 60.0f;
    // Push right for 15 ticks.
    for (int t = 0; t < 15; ++t) {
        world.move_and_slide(char_b, vec2{300.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
        world.step(dt);
    }

    const float mid_crate_x = world.get_body_position(crate_b).x();
    CHECK(mid_crate_x > 25.0f);

    // Now teleport player to the right side of the crate (x = mid_crate_x + 50) and push left.
    world.set_body_transform(char_b, vec2{mid_crate_x + 50.0f, 62.0f}, 0.0f);
    for (int t = 0; t < 25; ++t) {
        world.move_and_slide(char_b, vec2{-300.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);
        world.step(dt);
    }

    const float final_crate_x = world.get_body_position(crate_b).x();
    CHECK(final_crate_x < mid_crate_x - 30.0f);
}

TEST_CASE("physics: high velocity move_and_slide prevents thin wall tunneling") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Thin wall: 4 pixels wide at x = 100, y = 0.
    BodyConfig wall_cfg;
    wall_cfg.kind = BodyKind::Static;
    wall_cfg.position = {100.0f, 0.0f};
    uint32_t wall_b = world.create_body(wall_cfg, pluma::Value{});
    ShapeConfig wall_s;
    wall_s.kind = ShapeKind::Box;
    wall_s.size = {4.0f, 200.0f}; // x from 98 to 102
    world.add_shape(wall_b, wall_s, pluma::Value{});

    // Character at x = 0, y = 0, size 20x20 (half-width 10).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {0.0f, 0.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 20.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    // Move right at 12000 px/s (200 px in 1/60s). Gap is only 88 px (10 -> 98).
    // Shape casting must intercept the thin 4px wall and NOT tunnel through to x = 200.
    const float dt = 1.0f / 60.0f;
    SlideResult res = world.move_and_slide(char_b, vec2{12000.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);

    CHECK(res.collided);
    CHECK(res.on_wall);
    // Wall left edge is at 98. Character half-width is 10. Center must be at 88.
    CHECK(res.position.x() <= 88.5f);
    CHECK(res.position.x() >= 87.0f);
}

TEST_CASE("physics: sensor shape does not block kinematic movement") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    // Sensor area at x = 100, y = 0, size 60x60.
    BodyConfig sensor_cfg;
    sensor_cfg.kind = BodyKind::Static;
    sensor_cfg.position = {100.0f, 0.0f};
    uint32_t sensor_b = world.create_body(sensor_cfg, pluma::Value{});
    ShapeConfig sensor_s;
    sensor_s.kind = ShapeKind::Box;
    sensor_s.size = {60.0f, 60.0f};
    sensor_s.is_sensor = true;
    world.add_shape(sensor_b, sensor_s, pluma::Value{});

    // Character at x = 50, moving right at 6000 px/s (100 px in 1/60s).
    BodyConfig char_cfg;
    char_cfg.kind = BodyKind::Kinematic;
    char_cfg.position = {50.0f, 0.0f};
    uint32_t char_b = world.create_body(char_cfg, pluma::Value{});
    ShapeConfig char_s;
    char_s.kind = ShapeKind::Box;
    char_s.size = {20.0f, 20.0f};
    world.add_shape(char_b, char_s, pluma::Value{});

    const float dt = 1.0f / 60.0f;
    SlideResult res = world.move_and_slide(char_b, vec2{6000.0f, 0.0f}, dt, vec2{0.0f, -1.0f}, 0.785f, 4);

    CHECK_FALSE(res.collided);
    CHECK(res.position.x() >= 149.0f); // Unimpeded motion through the sensor
}

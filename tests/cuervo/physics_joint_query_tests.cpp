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

TEST_CASE("physics: all joint types creation, simulation, and destruction") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    BodyConfig b_static;
    b_static.kind = BodyKind::Static;
    b_static.position = {0.0f, 0.0f};
    uint32_t b0 = world.create_body(b_static, pluma::Value{});
    REQUIRE(b0 != 0);

    BodyConfig b_dyn;
    b_dyn.kind = BodyKind::Dynamic;
    b_dyn.position = {0.0f, 50.0f};
    uint32_t b1 = world.create_body(b_dyn, pluma::Value{});
    REQUIRE(b1 != 0);

    // Prismatic joint
    uint32_t prismatic = world.create_prismatic_joint(b0, b1, vec2{0.0f, 0.0f}, vec2{1.0f, 0.0f},
                                                      true, 5.0f, 0.7f, 0.0f,
                                                      true, -50.0f, 50.0f,
                                                      false, 0.0f, 0.0f, false, pluma::Value{});
    CHECK(prismatic != 0);

    // Weld joint
    b_dyn.position = {100.0f, 0.0f};
    uint32_t b2 = world.create_body(b_dyn, pluma::Value{});
    uint32_t weld = world.create_weld_joint(b0, b2, vec2{50.0f, 0.0f}, 0.0f, 0.0f, 1.0f, 1.0f, false, pluma::Value{});
    CHECK(weld != 0);

    // Wheel joint
    b_dyn.position = {200.0f, 50.0f};
    uint32_t b3 = world.create_body(b_dyn, pluma::Value{});
    uint32_t wheel = world.create_wheel_joint(b0, b3, vec2{200.0f, 0.0f}, vec2{0.0f, 1.0f},
                                              true, 8.0f, 0.5f, true, -10.0f, 10.0f,
                                              true, 500.0f, 2.0f, false, pluma::Value{});
    CHECK(wheel != 0);

    // Motor joint
    b_dyn.position = {300.0f, 0.0f};
    uint32_t b4 = world.create_body(b_dyn, pluma::Value{});
    uint32_t motor = world.create_motor_joint(b0, b4, vec2{10.0f, 0.0f}, 0.0f,
                                              500.0f, 500.0f, 2.0f, 0.7f, 500.0f, 2.0f, 0.7f, 500.0f,
                                              false, pluma::Value{});
    CHECK(motor != 0);

    // Mover joint
    b_dyn.position = {400.0f, 0.0f};
    uint32_t b5 = world.create_body(b_dyn, pluma::Value{});
    uint32_t mover = world.create_mover_joint(b0, b5, vec2{10.0f, 0.0f}, vec2{500.0f, 500.0f}, false, pluma::Value{});
    CHECK(mover != 0);

    // Pogo joint
    b_dyn.position = {500.0f, 30.0f};
    uint32_t b6 = world.create_body(b_dyn, pluma::Value{});
    uint32_t pogo = world.create_pogo_joint(b0, b6, vec2{500.0f, 0.0f}, vec2{0.0f, -1.0f},
                                            10.0f, 0.7f, 30.0f, 1000.0f, 1000.0f, false, pluma::Value{});
    CHECK(pogo != 0);

    // Filter joint
    uint32_t filter = world.create_filter_joint(b5, b6, pluma::Value{});
    CHECK(filter != 0);

    // Step simulation across joints
    for (int i = 0; i < 30; ++i) {
        world.step(1.0f / 60.0f);
    }
    CHECK(world.state_hash() != 0);

    // Clean up joints
    world.destroy_joint(prismatic);
    world.destroy_joint(weld);
    world.destroy_joint(wheel);
    world.destroy_joint(motor);
    world.destroy_joint(mover);
    world.destroy_joint(pogo);
    world.destroy_joint(filter);
}

TEST_CASE("physics: PrismaticJoint2D constrains motion to axis within limits") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f}); // Zero gravity

    BodyConfig b0_cfg;
    b0_cfg.kind = BodyKind::Static;
    b0_cfg.position = {0.0f, 0.0f};
    uint32_t b0 = world.create_body(b0_cfg, pluma::Value{});

    BodyConfig b1_cfg;
    b1_cfg.kind = BodyKind::Dynamic;
    b1_cfg.position = {0.0f, 0.0f};
    b1_cfg.mass = 1.0f;
    uint32_t b1 = world.create_body(b1_cfg, pluma::Value{});
    ShapeConfig b1_s;
    b1_s.kind = ShapeKind::Box;
    b1_s.size = {10.0f, 10.0f};
    world.add_shape(b1, b1_s, pluma::Value{});

    // Prismatic joint along X axis, limits [-20, 40]
    uint32_t j = world.create_prismatic_joint(b0, b1, vec2{0.0f, 0.0f}, vec2{1.0f, 0.0f},
                                             false, 0.0f, 0.0f, 0.0f,
                                             true, -20.0f, 40.0f,
                                             false, 0.0f, 0.0f, false, pluma::Value{});
    CHECK(j != 0);

    // Apply impulse diagonally (100 px/s right, 100 px/s down)
    world.apply_impulse(b1, vec2{100.0f, 100.0f});

    for (int i = 0; i < 60; ++i) {
        world.step(1.0f / 60.0f);
    }

    vec2 pos = world.get_body_position(b1);
    // Y position should remain locked near 0 despite downward impulse
    CHECK(std::abs(pos.y()) < 1.0f);
    // X position should be clamped at upper limit 40
    CHECK(pos.x() <= 41.0f);
    CHECK(pos.x() >= 38.0f);

    world.destroy_joint(j);
}

TEST_CASE("physics: WeldJoint2D locks relative transform under gravity") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    BodyConfig b0_cfg;
    b0_cfg.kind = BodyKind::Static;
    b0_cfg.position = {100.0f, 100.0f};
    uint32_t b0 = world.create_body(b0_cfg, pluma::Value{});

    BodyConfig b1_cfg;
    b1_cfg.kind = BodyKind::Dynamic;
    b1_cfg.position = {150.0f, 100.0f};
    b1_cfg.mass = 2.0f;
    uint32_t b1 = world.create_body(b1_cfg, pluma::Value{});
    ShapeConfig b1_s;
    b1_s.kind = ShapeKind::Box;
    b1_s.size = {20.0f, 20.0f};
    world.add_shape(b1, b1_s, pluma::Value{});

    // Weld b1 to b0 rigidly at anchor {100, 100}
    uint32_t weld = world.create_weld_joint(b0, b1, vec2{100.0f, 100.0f}, 0.0f, 0.0f, 1.0f, 1.0f, false, pluma::Value{});
    CHECK(weld != 0);

    for (int i = 0; i < 60; ++i) {
        world.step(1.0f / 60.0f);
    }

    vec2 p1 = world.get_body_position(b1);
    // Dynamic body must remain held up near y = 100 despite gravity
    CHECK(p1.y() >= 98.0f);
    CHECK(p1.y() <= 105.0f);
    CHECK(p1.x() >= 148.0f);
    CHECK(p1.x() <= 152.0f);

    world.destroy_joint(weld);
}

TEST_CASE("physics: FilterJoint2D disables collision between two dynamic bodies") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    BodyConfig b1_cfg;
    b1_cfg.kind = BodyKind::Dynamic;
    b1_cfg.position = {0.0f, 0.0f};
    b1_cfg.mass = 1.0f;
    uint32_t b1 = world.create_body(b1_cfg, pluma::Value{});
    ShapeConfig b1_s;
    b1_s.kind = ShapeKind::Box;
    b1_s.size = {10.0f, 10.0f};
    world.add_shape(b1, b1_s, pluma::Value{});
    ShapeConfig s1;
    s1.kind = ShapeKind::Box;
    s1.size = {20.0f, 20.0f};
    world.add_shape(b1, s1, pluma::Value{});

    BodyConfig b2_cfg;
    b2_cfg.kind = BodyKind::Dynamic;
    b2_cfg.position = {50.0f, 0.0f};
    b2_cfg.mass = 1.0f;
    uint32_t b2 = world.create_body(b2_cfg, pluma::Value{});
    ShapeConfig s2;
    s2.kind = ShapeKind::Box;
    s2.size = {20.0f, 20.0f};
    world.add_shape(b2, s2, pluma::Value{});

    // Disable collisions via filter joint
    uint32_t filter = world.create_filter_joint(b1, b2, pluma::Value{});
    CHECK(filter != 0);

    // Launch b1 right towards b2
    world.set_body_linear_velocity(b1, vec2{100.0f, 0.0f});

    for (int i = 0; i < 60; ++i) {
        world.step(1.0f / 60.0f);
    }

    vec2 p1 = world.get_body_position(b1);
    // b1 passed straight through b2 (at x=50) towards x=100
    CHECK(p1.x() > 60.0f);

    world.destroy_joint(filter);
}

TEST_CASE("physics: Convex polygon and chain shapes collision") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});

    // Static chain floor
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    uint32_t floor_b = world.create_body(floor_cfg, pluma::Value{});

    ShapeConfig chain_s;
    chain_s.kind = ShapeKind::Chain;
    chain_s.points = {vec2{-100.0f, 0.0f}, vec2{0.0f, 10.0f}, vec2{100.0f, 0.0f}};
    chain_s.is_loop = false;
    uint32_t chain_handle = world.add_shape(floor_b, chain_s, pluma::Value{});
    CHECK(chain_handle != 0);

    // Dynamic triangle (convex polygon)
    BodyConfig tri_cfg;
    tri_cfg.kind = BodyKind::Dynamic;
    tri_cfg.position = {0.0f, 20.0f};
    tri_cfg.mass = 1.0f;
    uint32_t tri_b = world.create_body(tri_cfg, pluma::Value{});

    ShapeConfig tri_s;
    tri_s.kind = ShapeKind::Polygon;
    tri_s.points = {vec2{0.0f, -15.0f}, vec2{15.0f, 15.0f}, vec2{-15.0f, 15.0f}};
    uint32_t tri_shape = world.add_shape(tri_b, tri_s, pluma::Value{});
    CHECK(tri_shape != 0);

    // Step simulation: triangle falls and rests on the chain floor
    for (int i = 0; i < 60; ++i) {
        world.step(1.0f / 60.0f);
    }

    vec2 tri_pos = world.get_body_position(tri_b);
    CHECK(tri_pos.y() > 80.0f);
    CHECK(tri_pos.y() < 110.0f);
}

TEST_CASE("physics: ShapeCastHit query detects obstacles") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    BodyConfig wall_cfg;
    wall_cfg.kind = BodyKind::Static;
    wall_cfg.position = {100.0f, 0.0f};
    uint32_t wall = world.create_body(wall_cfg, pluma::Value{});
    ShapeConfig wall_s;
    wall_s.kind = ShapeKind::Box;
    wall_s.size = {20.0f, 100.0f}; // x from 90 to 110
    world.add_shape(wall, wall_s, pluma::Value{});

    // Cast a box shape from x=0 towards x=200
    ShapeConfig cast_cfg;
    cast_cfg.kind = ShapeKind::Box;
    cast_cfg.size = {20.0f, 20.0f}; // half-width 10

    ShapeCastHit hit = world.cast_shape(cast_cfg, vec2{0.0f, 0.0f}, vec2{200.0f, 0.0f}, 1, 0);
    CHECK(hit.hit);
    CHECK(hit.body_handle == wall);
    // Contact occurs when cast box front edge reaches wall left edge (90), so center at 80
    CHECK(hit.fraction < 0.5f);
    CHECK(hit.normal.x() < 0.0f);

    // Cast shape missing the wall
    ShapeCastHit miss = world.cast_shape(cast_cfg, vec2{0.0f, -100.0f}, vec2{200.0f, 0.0f}, 1, 0);
    CHECK_FALSE(miss.hit);
}

TEST_CASE("physics: point query tests hit and miss") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    BodyConfig box_cfg;
    box_cfg.kind = BodyKind::Static;
    box_cfg.position = {50.0f, 50.0f};
    uint32_t box = world.create_body(box_cfg, pluma::Value{});
    ShapeConfig box_s;
    box_s.kind = ShapeKind::Box;
    box_s.size = {40.0f, 40.0f}; // x from 30 to 70, y from 30 to 70
    world.add_shape(box, box_s, pluma::Value{});

    uint32_t hit_b = world.query_point(vec2{50.0f, 50.0f}, 1);
    CHECK(hit_b == box);

    uint32_t miss_b = world.query_point(vec2{100.0f, 100.0f}, 1);
    CHECK(miss_b == 0);
}

TEST_CASE("physics: radial explosion applies impulse to nearby dynamic bodies") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    BodyConfig b_cfg;
    b_cfg.kind = BodyKind::Dynamic;
    b_cfg.position = {40.0f, 0.0f};
    b_cfg.mass = 1.0f;
    uint32_t b = world.create_body(b_cfg, pluma::Value{});
    ShapeConfig b_s;
    b_s.kind = ShapeKind::Circle;
    b_s.radius = 10.0f;
    world.add_shape(b, b_s, pluma::Value{});

    // Detonate explosion at origin with radius 100
    world.explode(vec2{0.0f, 0.0f}, 100.0f, 50.0f, 500.0f, 1);

    world.step(1.0f / 60.0f);

    vec2 vel = world.get_body_linear_velocity(b);
    // Explosion pushes the body radially outward (+x)
    CHECK(vel.x() > 10.0f);
}

TEST_CASE("physics: body continuous collision detection and sleep states") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});

    BodyConfig b_cfg;
    b_cfg.kind = BodyKind::Dynamic;
    b_cfg.position = {0.0f, 0.0f};
    b_cfg.continuous_cd = true;
    b_cfg.can_sleep = true;
    uint32_t b = world.create_body(b_cfg, pluma::Value{});

    CHECK(world.is_body_bullet(b));
    world.set_body_bullet(b, false);
    CHECK_FALSE(world.is_body_bullet(b));

    CHECK(world.is_body_can_sleep(b));
    CHECK(world.is_body_awake(b));

    world.set_body_awake(b, false);
    // In Box2D, putting a body to sleep is honored when enabled
    world.set_body_awake(b, true);
    CHECK(world.is_body_awake(b));
}

TEST_CASE("physics: shape with non-finite geometry is rejected") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    BodyConfig cfg;
    cfg.kind = BodyKind::Static;
    const uint32_t body = world.create_body(cfg, pluma::Value{});
    REQUIRE(body != 0);

    ShapeConfig shape;
    shape.offset = {std::nanf(""), 0.0f};
    CHECK(world.add_shape(body, shape, pluma::Value{}) == 0);
    shape.offset = {0.0f, 0.0f};
    shape.rotation = INFINITY;
    CHECK(world.add_shape(body, shape, pluma::Value{}) == 0);
    shape.rotation = 0.0f;
    CHECK(world.add_shape(body, shape, pluma::Value{}) != 0);
}

TEST_CASE("physics: set_shape_filter does not re-enable a disabled shape") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    BodyConfig floor_cfg;
    floor_cfg.kind = BodyKind::Static;
    floor_cfg.position = {0.0f, 100.0f};
    const uint32_t floor_body = world.create_body(floor_cfg, pluma::Value{});
    ShapeConfig floor_shape;
    floor_shape.size = {200.0f, 20.0f};
    const uint32_t shape = world.add_shape(floor_body, floor_shape, pluma::Value{});
    REQUIRE(shape != 0);

    world.set_shape_disabled(shape, true);
    world.set_shape_filter(shape, 1, 1);
    CHECK_FALSE(world.cast_ray({0.0f, 0.0f}, {0.0f, 200.0f}, 1, 0).hit);

    world.set_shape_disabled(shape, false);
    CHECK(world.cast_ray({0.0f, 0.0f}, {0.0f, 200.0f}, 1, 0).hit);
}

TEST_CASE("physics: raycast sees shapes whose own mask excludes the ray's layer") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    BodyConfig cfg;
    cfg.kind = BodyKind::Static;
    cfg.position = {0.0f, 100.0f};
    const uint32_t body = world.create_body(cfg, pluma::Value{});
    ShapeConfig shape;
    shape.size = {200.0f, 20.0f};
    shape.layer = 4;
    shape.mask = 2; // does not include bit 0
    REQUIRE(world.add_shape(body, shape, pluma::Value{}) != 0);
    CHECK(world.cast_ray({0.0f, 0.0f}, {0.0f, 200.0f}, 4, 0).hit);
    CHECK_FALSE(world.cast_ray({0.0f, 0.0f}, {0.0f, 200.0f}, 1, 0).hit);
}

TEST_CASE("physics: set_body_can_sleep on one body does not enable sleeping world-wide") {
    PhysicsWorld world(1, vec2{0.0f, 0.0f});
    BodyConfig cfg;
    cfg.kind = BodyKind::Dynamic;
    const uint32_t a = world.create_body(cfg, pluma::Value{});
    const uint32_t b = world.create_body(cfg, pluma::Value{});
    REQUIRE(a != 0);
    REQUIRE(b != 0);
    for (uint32_t body : {a, b}) {
        ShapeConfig shape;
        REQUIRE(world.add_shape(body, shape, pluma::Value{}) != 0);
    }
    world.set_body_transform(b, {200.0f, 0.0f}, 0.0f); // apart, so the two boxes do not push each other
    world.set_body_can_sleep(a, true);
    CHECK_FALSE(world.sleep_enabled());
    for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f, nullptr);
    CHECK(world.is_body_awake(a));
    CHECK(world.is_body_awake(b));

    world.set_sleep_enabled(true);
    for (int i = 0; i < 120; ++i) world.step(1.0f / 60.0f, nullptr);
    CHECK(world.is_body_awake(a) == false);
}

TEST_CASE("physics: degenerate polygon and segment shapes are rejected") {
    PhysicsWorld world(1, vec2{0.0f, 980.0f});
    BodyConfig cfg;
    cfg.kind = BodyKind::Static;
    const uint32_t body = world.create_body(cfg, pluma::Value{});
    REQUIRE(body != 0);

    ShapeConfig poly;
    poly.kind = ShapeKind::Polygon;
    poly.points = {{0.0f, 0.0f}, {10.0f, 0.0f}};
    CHECK(world.add_shape(body, poly, pluma::Value{}) == 0);
    poly.points = {{0.0f, 0.0f}, {5.0f, 0.0f}, {10.0f, 0.0f}}; // collinear
    CHECK(world.add_shape(body, poly, pluma::Value{}) == 0);

    ShapeConfig seg;
    seg.kind = ShapeKind::Segment;
    seg.point_a = {3.0f, 3.0f};
    seg.point_b = {3.0f, 3.0f};
    CHECK(world.add_shape(body, seg, pluma::Value{}) == 0);
}

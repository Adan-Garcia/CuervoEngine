#include <doctest.h>

#include <cstring>

#include "cuervo/render/camera.hpp"
#include "cuervo/render/light_batcher.hpp"
#include "recording_device.hpp"

// 2D lighting (DESIGN.md 4.17, milestone 9.7): the CPU mirror of light.frag's
// signed distances and shadow march, and what LightBatcher uploads.

using namespace cuervo;
using namespace cuervo::render;
using cuervo::test::RecordingDevice;

namespace {

SdfOccluder box(vec2 center, vec2 half, float rotation = 0.0f) {
    SdfOccluder o;
    o.kind = LightOccluderKind::RoundedRect;
    o.center = center;
    o.half_size = half;
    o.rotation = rotation;
    return o;
}

SdfOccluder circle(vec2 center, float r) {
    SdfOccluder o;
    o.kind = LightOccluderKind::Circle;
    o.center = center;
    o.half_size = {r, 0.0f};
    return o;
}

LightDef point(vec2 pos, float radius) {
    LightDef l;
    l.position = pos;
    l.radius = radius;
    l.shadow_softness = 0.0f;
    return l;
}

// The scene uniforms LightBatcher last uploaded.
LightSceneUniforms scene_of(const RecordingDevice& d) {
    LightSceneUniforms u{};
    for (const auto& [id, desc] : d.descs) {
        if (desc.kind == rhi::BufferKind::Uniform && desc.size == sizeof(LightSceneUniforms) && d.buffers.contains(id)) {
            std::memcpy(&u, d.buffers.at(id).data(), sizeof(u));
        }
    }
    return u;
}

} // namespace

TEST_CASE("lighting: GPU layouts match light.vert and the std140 LightScene block") {
    CHECK(sizeof(LightQuadInstance) == 68);
    CHECK(sizeof(LightViewUniforms) == 32);
    CHECK(sizeof(LightSceneUniforms) == 10032);
    CHECK(offsetof(LightSceneUniforms, occ_geom) == 48);
    CHECK(offsetof(LightSceneUniforms, occ_masks) == 48 + 2 * 16 * max_gpu_occluders);
    CHECK(offsetof(LightSceneUniforms, light_params2) == sizeof(LightSceneUniforms) - 16 * max_gpu_lights);
}

TEST_CASE("lighting: occluder signed distances") {
    CHECK(eval_occluder_sdf(circle({10.0f, 0.0f}, 5.0f), {20.0f, 0.0f}) == doctest::Approx(5.0f));
    CHECK(eval_occluder_sdf(circle({10.0f, 0.0f}, 5.0f), {10.0f, 0.0f}) == doctest::Approx(-5.0f));
    CHECK(eval_occluder_sdf(box({0.0f, 0.0f}, {10.0f, 4.0f}), {15.0f, 0.0f}) == doctest::Approx(5.0f));
    CHECK(eval_occluder_sdf(box({0.0f, 0.0f}, {10.0f, 4.0f}), {0.0f, 0.0f}) == doctest::Approx(-4.0f));
    // Turned a quarter: the long side is vertical.
    CHECK(eval_occluder_sdf(box({0.0f, 0.0f}, {10.0f, 4.0f}, 1.5707964f), {0.0f, 15.0f}) ==
          doctest::Approx(5.0f).epsilon(1e-4));
    SdfOccluder cap;
    cap.kind = LightOccluderKind::Capsule;
    cap.half_size = {10.0f, 2.0f};
    CHECK(eval_occluder_sdf(cap, {0.0f, 7.0f}) == doctest::Approx(5.0f));
    CHECK(eval_occluder_sdf(cap, {15.0f, 0.0f}) == doctest::Approx(3.0f));
    const std::vector<SdfOccluder> two{circle({0.0f, 0.0f}, 1.0f), circle({10.0f, 0.0f}, 1.0f)};
    CHECK(eval_scene_sdf(two, {7.0f, 0.0f}) == doctest::Approx(2.0f));
    // A mask with no shared bit ignores the occluder.
    std::vector<SdfOccluder> masked{circle({0.0f, 0.0f}, 1.0f)};
    masked[0].mask = 2u;
    CHECK(eval_scene_sdf(masked, {3.0f, 0.0f}, 1u) > 1000.0f);
}

TEST_CASE("lighting: a wall between the point and the light casts a hard shadow") {
    const std::vector<SdfOccluder> wall{box({50.0f, 0.0f}, {4.0f, 40.0f})};
    const LightDef l = point({100.0f, 0.0f}, 200.0f);
    const vec2 behind = eval_light_at(l, wall, {0.0f, 0.0f});
    CHECK(behind.x() > 0.0f);        // in range...
    CHECK(behind.y() == 0.0f);       // ...but shadowed
    const vec2 beside = eval_light_at(l, wall, {0.0f, 130.0f});
    CHECK(beside.y() == 1.0f);       // the ray passes 25 under the wall
    const vec2 far = eval_light_at(l, wall, {-150.0f, 0.0f});
    CHECK(far.x() == 0.0f);          // out of range: no light at all
    LightDef off = l;
    off.shadow_enabled = false;
    CHECK(eval_light_at(off, wall, {0.0f, 0.0f}).y() == 1.0f);
}

TEST_CASE("lighting: occluder faces are normal mapped, and only other occluders shadow them") {
    const std::vector<SdfOccluder> slab{box({0.0f, 0.0f}, {50.0f, 40.0f})};
    const vec3 above{0.0f, -100.0f, 48.0f}; // a light over the slab's top face (y is down)
    // Deep inside the face (past the 8-unit bevel): flat, lit like the floor.
    CHECK(eval_surface_factor(slab, {0.0f, 0.0f}, above) == doctest::Approx(1.0f));
    // On the bevel turned to the light: brighter; turned away: darker.
    CHECK(eval_surface_factor(slab, {0.0f, -38.0f}, vec3{0.0f, -62.0f, 48.0f}) > 1.2f);
    CHECK(eval_surface_factor(slab, {0.0f, 38.0f}, vec3{0.0f, -138.0f, 48.0f}) < 0.6f);
    // Outside every occluder, or on a flat (bevel 0) face: 1.
    CHECK(eval_surface_factor(slab, {0.0f, -60.0f}, above) == 1.0f);
    std::vector<SdfOccluder> flat = slab;
    flat[0].bevel = 0.0f;
    CHECK(eval_surface_factor(flat, {0.0f, -38.0f}, above) == 1.0f);
    // Two occluders touching (tilemap rectangles): the seam between them is flat.
    const std::vector<SdfOccluder> stacked{box({0.0f, 0.0f}, {50.0f, 20.0f}), box({0.0f, 40.0f}, {50.0f, 20.0f})};
    CHECK(eval_surface_factor(stacked, {0.0f, 18.0f}, vec3{0.0f, -118.0f, 48.0f}) == doctest::Approx(1.0f));
    CHECK(eval_surface_factor(stacked, {0.0f, -18.0f}, vec3{0.0f, -82.0f, 48.0f}) > 1.2f); // the real top edge
    // The face isn't shadowed by its own occluder, wherever the light is...
    CHECK(eval_raymarch_shadow(slab, {0.0f, 38.0f}, {0.0f, -1.0f}, 138.0f, 0.0f, 0.0f) == 1.0f);
    // ...but another occluder between them shadows it.
    std::vector<SdfOccluder> two = slab;
    two.push_back(box({0.0f, -70.0f}, {20.0f, 4.0f}));
    CHECK(eval_raymarch_shadow(two, {0.0f, -30.0f}, {0.0f, -1.0f}, 70.0f, 0.0f, 0.0f) == 0.0f);
}

TEST_CASE("lighting: soft shadows fade across the penumbra") {
    const std::vector<SdfOccluder> post{circle({50.0f, 0.0f}, 5.0f)};
    float last = -1.0f;
    bool partial = false;
    // Moving out of the shadow sideways: visibility never decreases.
    for (float y = 0.0f; y <= 60.0f; y += 1.0f) {
        const vec2 to = vec2{100.0f, 0.0f} - vec2{0.0f, y};
        const float d = length(to);
        const float v = eval_raymarch_shadow(post, {0.0f, y}, to / d, d, 0.0f, 24.0f);
        CHECK(v >= last - 1e-4f);
        partial = partial || (v > 0.05f && v < 0.95f);
        last = v;
    }
    CHECK(partial);
    CHECK(last == doctest::Approx(1.0f));
}

TEST_CASE("lighting: a light held low over the floor doesn't darken the floor it grazes") {
    // The floor's top at y = 0, a lantern 20 above it; points just above the
    // floor to the side see the light along rays that skim the floor near it.
    // A penumbra proportional to the distance toward the light (k * h / t)
    // darkened them, leaving a lit wedge above the lantern; a round source
    // `softness` across doesn't.
    const std::vector<SdfOccluder> floor{box({0.0f, 50.0f}, {400.0f, 50.0f})};
    LightDef lantern = point({0.0f, -20.0f}, 300.0f);
    lantern.shadow_softness = 10.0f;
    for (float x = 40.0f; x <= 260.0f; x += 20.0f) {
        CHECK_MESSAGE(eval_light_at(lantern, floor, {x, -6.0f}).y() > 0.95f, x);
        CHECK_MESSAGE(eval_light_at(lantern, floor, {-x, -6.0f}).y() > 0.95f, x);
    }
}

TEST_CASE("lighting: the floor's own surface line isn't shadowed") {
    // Exactly on the floor's top (distance 0), rays toward a low light graze
    // the floor; marching from there hit it at once, a hairline of shadow
    // along the floor wherever the light was far enough.
    const std::vector<SdfOccluder> floor{box({0.0f, 50.0f}, {1000.0f, 50.0f})};
    for (const float softness : {0.0f, 10.0f}) {
        LightDef lantern = point({0.0f, -19.0f}, 300.0f);
        lantern.shadow_softness = softness;
        for (float x = 20.0f; x <= 280.0f; x += 20.0f) {
            for (const float y : {-0.25f, 0.0f, 0.1f, 0.25f}) {
                CHECK_MESSAGE(eval_light_at(lantern, floor, {x, y}).y() > 0.95f, x, " ", y, " ", softness);
            }
        }
    }
}

TEST_CASE("lighting: a wall of stacked rectangles is beveled evenly past its seams") {
    // A tilemap's wall: strips stacked against the right edge (x = 100), lit
    // from the left so the edge's bevel turns away and darkens. Near a seam
    // the nearest edge is the seam, and the bevel used to go flat there: a
    // bright notch at every seam.
    std::vector<SdfOccluder> wall;
    for (int i = 0; i < 4; ++i) {
        wall.push_back(box({0.0f, 32.0f + 64.0f * static_cast<float>(i)}, {100.0f, 32.0f}));
    }
    const vec3 to_light{-300.0f, 0.0f, 48.0f};
    for (float x = 92.5f; x < 100.0f; x += 1.0f) {
        const float away = eval_surface_factor(wall, {x, 32.5f}, to_light); // mid-strip
        for (float y = 56.5f; y <= 71.5f; y += 1.0f) {                      // across the seam at 64
            CHECK_MESSAGE(eval_surface_factor(wall, {x, y}, to_light) == doctest::Approx(away).epsilon(0.02), x,
                          " ", y);
        }
    }
}

TEST_CASE("lighting: an inside corner of a tilemap is beveled round the corner") {
    // Rock wrapped around a corner of open air at the origin (air: x < 0 and
    // y < 0), made of two rectangles touching along y = 0. Past the corner
    // the nearest edge point is the corner itself: the bevel rounds it,
    // instead of stopping flat at the seam.
    const std::vector<SdfOccluder> rock{box({50.0f, -50.0f}, {50.0f, 50.0f}), box({0.0f, 50.0f}, {100.0f, 50.0f})};
    const vec3 to_corner_light{-200.0f, -200.0f, 48.0f}; // up and left, over the open corner
    for (const vec2 p : {vec2{2.5f, 2.5f}, vec2{4.0f, 1.5f}, vec2{1.5f, 4.0f}, vec2{3.5f, 3.5f}}) {
        CHECK_MESSAGE(eval_surface_factor(rock, p, to_corner_light) > 1.2f, p.x(), " ", p.y()); // turned to the light
    }
    CHECK(eval_surface_factor(rock, {9.0f, 9.0f}, to_corner_light) == doctest::Approx(1.0f)); // past the bevel: flat
    // Just outside a face (inside the march's 0.25 contact) shades like the
    // face, not like the open floor beside it.
    CHECK(eval_surface_factor(rock, {-30.0f, -0.1f}, vec3{0.0f, 300.0f, 48.0f}) < 0.5f); // turned away: dark
    CHECK(eval_surface_factor(rock, {-30.0f, -0.5f}, vec3{0.0f, 300.0f, 48.0f}) == 1.0f); // open floor
}

TEST_CASE("lighting: a penumbra is centered on the hard shadow's edge") {
    // A crate the lantern touches: its left face 6 to the right of the light,
    // its top-left corner at (6, -20). The shadow's hard edge runs from the
    // light through that corner. An outer-only penumbra a whole source wide
    // darkened rays passing up to `softness` outside the crate, so the shadow
    // grew far past its outline (and a seam ran up from the light).
    const std::vector<SdfOccluder> crate{box({26.0f, 0.0f}, {20.0f, 20.0f})};
    LightDef lantern = point({0.0f, 0.0f}, 300.0f);
    lantern.shadow_softness = 10.0f;
    // Straight above and below, the rays clear the crate by more than the
    // source's radius: fully lit.
    CHECK(eval_light_at(lantern, crate, {0.0f, -150.0f}).y() == doctest::Approx(1.0f));
    CHECK(eval_light_at(lantern, crate, {-2.0f, 150.0f}).y() == doctest::Approx(1.0f));
    // On the hard edge, half the source is hidden; past it, the shadow is full.
    const float edge = eval_light_at(lantern, crate, {6.0f * 7.0f, -20.0f * 7.0f}).y();
    CHECK(edge > 0.25f);
    CHECK(edge < 0.75f);
    CHECK(eval_light_at(lantern, crate, {150.0f, -60.0f}).y() == 0.0f);
}

TEST_CASE("lighting: a soft light pushed into a wall dims smoothly") {
    // A lantern held against a wall dips into it as it sways: the light fades
    // with the depth instead of flipping between lit wedges and full shadow.
    const std::vector<SdfOccluder> wall{box({50.0f, 0.0f}, {40.0f, 200.0f})};
    LightDef lantern = point({0.0f, 0.0f}, 300.0f);
    lantern.shadow_softness = 10.0f;
    lantern.source_radius = 0.0f;
    for (float y = -120.0f; y <= 120.0f; y += 8.0f) {
        float last = 2.0f;
        for (float x = 0.0f; x <= 20.0f; x += 0.5f) { // the wall's face is at x = 10
            lantern.position = {x, 0.0f};
            const float v = eval_light_at(lantern, wall, {-100.0f, y}).y();
            CHECK_MESSAGE(v <= last + 1e-3f, x, " ", y); // darker the deeper it goes
            last = v;
        }
        CHECK_MESSAGE(last == 0.0f, y); // the whole source inside the wall
    }
    lantern.position = {4.0f, 0.0f}; // clear of the wall by more than its radius
    CHECK(eval_light_at(lantern, wall, {-100.0f, 60.0f}).y() == doctest::Approx(1.0f));
}

TEST_CASE("lighting: spot lights only reach inside their cone") {
    LightDef spot = point({0.0f, 0.0f}, 200.0f);
    spot.rotation = 0.0f; // along +x
    spot.spot_angle_rad = 1.0f;
    CHECK(eval_light_at(spot, {}, {100.0f, 0.0f}).x() > 0.0f);
    CHECK(eval_light_at(spot, {}, {0.0f, 100.0f}).x() == 0.0f);
    CHECK(eval_light_at(spot, {}, {-100.0f, 0.0f}).x() == 0.0f);
}

TEST_CASE("lighting: the batcher keeps baked occluders and forgets the frame's") {
    RecordingDevice d;
    LightBatcher b;
    REQUIRE(b.init(d));
    b.add_baked_occluder(circle({0.0f, 0.0f}, 4.0f));
    b.add_occluder_box({10.0f, 0.0f}, {2.0f, 2.0f});
    b.add_occluder_polygon(std::vector<vec2>{{0, 0}, {10, 0}, {10, 10}}, true, 2.0f);
    CHECK(b.occluder_count() == 5); // 1 baked + 1 box + 3 edges
    b.add_light(point({0.0f, 0.0f}, 50.0f));
    CHECK(b.has_lights());
    b.clear();
    CHECK(b.occluder_count() == 1);
    CHECK_FALSE(b.has_lights());
    // Queries see the same occluders the GPU would.
    CHECK(b.scene_sdf({10.0f, 0.0f}) == doctest::Approx(6.0f));
    CHECK(b.raymarch({-20.0f, 0.0f}, {20.0f, 0.0f}) == 0.0f);
    CHECK(b.raymarch({-20.0f, 10.0f}, {20.0f, 10.0f}) == 1.0f);
}

TEST_CASE("lighting: flush culls each light's occluders and draws one quad per light") {
    RecordingDevice d;
    LightBatcher b;
    REQUIRE(b.init(d));
    b.add_occluder_circle({100.0f, 0.0f}, 5.0f);  // near light 0
    b.add_occluder_circle({900.0f, 0.0f}, 5.0f);  // near light 1
    b.add_occluder_circle({500.0f, 900.0f}, 5.0f); // near neither
    b.add_light(point({0.0f, 0.0f}, 200.0f));
    b.add_light(point({1000.0f, 0.0f}, 200.0f));
    const Affine2 to_ndc = virtual_to_ndc({1280.0f, 720.0f});
    b.flush(to_ndc, {640.0f, 360.0f}, {640.0f, 360.0f});
    REQUIRE(d.draws.size() == 1);
    CHECK(d.draws[0].instance_count == 2); // additive lights: a quad each
    const LightSceneUniforms u = scene_of(d);
    CHECK(u.counts[0] == 2); // the far occluder isn't uploaded
    CHECK(u.counts[1] == 2);
    // Light i's list is light_params1[i].zw = (first, count).
    CHECK(u.light_params1[0].z() == 0.0f);
    CHECK(u.light_params1[0].w() == 1.0f);
    CHECK(u.light_params1[1].z() == 1.0f);
    CHECK(u.light_params1[1].w() == 1.0f);
    CHECK(u.occ_geom[u.occ_lists[0]].x() == 100.0f);
    CHECK(u.occ_geom[u.occ_lists[1]].x() == 900.0f);
}

TEST_CASE("lighting: an ambient light composites every light in one full-view quad") {
    RecordingDevice d;
    LightBatcher b;
    REQUIRE(b.init(d));
    AmbientLightDef amb;
    amb.enabled = true;
    amb.darkness = 0.75f;
    b.set_ambient(amb);
    b.add_light(point({0.0f, 0.0f}, 200.0f));
    b.add_light(point({400.0f, 0.0f}, 200.0f));
    b.flush(virtual_to_ndc({1280.0f, 720.0f}), {640.0f, 360.0f}, {640.0f, 360.0f});
    REQUIRE(d.draws.size() == 1);
    CHECK(d.draws[0].instance_count == 1);
    const LightSceneUniforms u = scene_of(d);
    CHECK(u.counts[1] == 2);
    CHECK(u.ambient_color.w() == doctest::Approx(0.75f));
    // Without lights or ambient, nothing is drawn.
    b.clear();
    d.draws.clear();
    b.flush(virtual_to_ndc({1280.0f, 720.0f}), {640.0f, 360.0f}, {640.0f, 360.0f});
    CHECK(d.draws.empty());
}

#include <doctest.h>

#include "cuervo/math/dmath.hpp"
#include "cuervo/render/camera.hpp"

using namespace cuervo;
using namespace cuervo::render;

namespace {

constexpr float eps = 1e-4f;

void check_near(vec2 actual, vec2 expected, float tolerance = eps) {
    // Relative tolerance with a floor of `tolerance` around zero.
    CHECK(actual.x() == doctest::Approx(expected.x()).scale(1.0).epsilon(tolerance));
    CHECK(actual.y() == doctest::Approx(expected.y()).scale(1.0).epsilon(tolerance));
}

const vec2 virtual_1280x720{1280.0f, 720.0f};

} // namespace

// User decision (DESIGN.md 7.1): world/virtual space is y-down with the origin
// at the top-left; the RHI guarantees NDC +1 is the top of the target.
TEST_CASE("virtual_to_ndc: y-down virtual space, top-left origin") {
    const Affine2 m = virtual_to_ndc(virtual_1280x720);
    check_near(m.apply({0.0f, 0.0f}), {-1.0f, 1.0f});      // top-left
    check_near(m.apply({1280.0f, 0.0f}), {1.0f, 1.0f});    // top-right
    check_near(m.apply({0.0f, 720.0f}), {-1.0f, -1.0f});   // bottom-left
    check_near(m.apply({1280.0f, 720.0f}), {1.0f, -1.0f}); // bottom-right
    check_near(m.apply({640.0f, 360.0f}), {0.0f, 0.0f});
}

TEST_CASE("Affine2: positive rotation is clockwise on a y-down screen") {
    // +x (right) turned by +90 degrees points to +y, which is *down* on screen:
    // right -> down is a clockwise turn.
    const Affine2 r = Affine2::rotate(dmath::half_pi);
    check_near(r.apply({1.0f, 0.0f}), {0.0f, 1.0f});
    check_near(r.apply({0.0f, -1.0f}), {1.0f, 0.0f}); // up -> right, also clockwise

    // A small positive angle moves a point on the +x axis downwards.
    const vec2 p = Affine2::rotate(0.1f).apply({10.0f, 0.0f});
    CHECK(p.y() > 0.0f);
}

TEST_CASE("Affine2 composition and inverse") {
    const Affine2 t = Affine2::translate({5.0f, -3.0f});
    const Affine2 s = Affine2::scale({2.0f, 4.0f});
    const Affine2 r = Affine2::rotate(0.7f);
    const vec2 p{1.5f, -2.25f};

    // (l * r).apply(p) == l.apply(r.apply(p))
    check_near((t * s).apply(p), t.apply(s.apply(p)));
    check_near((t * s * r).apply(p), t.apply(s.apply(r.apply(p))));
    check_near((t * s).apply(p), {8.0f, -12.0f});

    const Affine2 m = t * s * r;
    check_near(m.inverse().apply(m.apply(p)), p);
    check_near((m * m.inverse()).apply(p), p);

    // Singular transforms fall back to the identity.
    const Affine2 singular = Affine2::scale({0.0f, 1.0f});
    check_near(singular.inverse().apply(p), p);
}

TEST_CASE("Camera2D::screen shows world [0, virtual size] like screen space") {
    const Camera2D camera = Camera2D::screen(virtual_1280x720);
    check_near(camera.world_to_virtual(virtual_1280x720).apply({123.0f, 456.0f}), {123.0f, 456.0f});
    const Affine2 vp = camera.view_projection(virtual_1280x720);
    check_near(vp.apply({0.0f, 0.0f}), {-1.0f, 1.0f});
    check_near(vp.apply({1280.0f, 720.0f}), {1.0f, -1.0f});
}

TEST_CASE("Camera2D position, zoom and rotation") {
    Camera2D camera;
    camera.position = {1000.0f, -500.0f};

    // The camera position lands in the center of the view.
    check_near(camera.world_to_virtual(virtual_1280x720).apply(camera.position), {640.0f, 360.0f});
    check_near(camera.view_projection(virtual_1280x720).apply(camera.position), {0.0f, 0.0f});

    // Zoom magnifies around the center.
    camera.zoom = 2.0f;
    check_near(camera.world_to_virtual(virtual_1280x720).apply(camera.position + vec2(10.0f, 5.0f)),
               {660.0f, 370.0f});

    // Turning the camera clockwise makes the world appear to turn
    // counter-clockwise: a point to the camera's right shows up above center.
    camera.zoom = 1.0f;
    camera.rotation = dmath::half_pi;
    check_near(camera.world_to_virtual(virtual_1280x720).apply(camera.position + vec2(100.0f, 0.0f)),
               {640.0f, 260.0f});

    // virtual_to_world inverts world_to_virtual.
    const vec2 world{1234.0f, -321.0f};
    check_near(camera.virtual_to_world(camera.world_to_virtual(virtual_1280x720).apply(world), virtual_1280x720),
               world, 1e-3f);
}

TEST_CASE("fit_viewport: Fit scales uniformly and letterboxes at native resolution") {
    const ivec2 v{1280, 720};

    ViewportFit exact = fit_viewport(ScaleMode::Fit, v, {1920, 1080});
    CHECK(exact.viewport == rhi::Rect{0, 0, 1920, 1080});
    CHECK(exact.scale == vec2(1.5f, 1.5f));

    // Taller framebuffer: bars top and bottom.
    ViewportFit tall = fit_viewport(ScaleMode::Fit, v, {1920, 1200});
    CHECK(tall.viewport == rhi::Rect{0, 60, 1920, 1080});
    CHECK(tall.scale == vec2(1.5f, 1.5f));

    // Wider framebuffer: bars left and right.
    ViewportFit wide = fit_viewport(ScaleMode::Fit, v, {2560, 1080});
    CHECK(wide.viewport == rhi::Rect{320, 0, 1920, 1080});

    // Non-integer scales are fine (vectors render at native resolution).
    ViewportFit small = fit_viewport(ScaleMode::Fit, v, {1000, 1000});
    CHECK(small.viewport.width == 1000);
    CHECK(small.viewport.height == 563); // 720 * 0.78125 = 562.5, rounded
    CHECK(small.viewport.y == (1000 - 563) / 2);
}

TEST_CASE("fit_viewport: Integer uses the largest whole scale") {
    const ivec2 v{320, 180};

    CHECK(fit_viewport(ScaleMode::Integer, v, {1280, 720}).viewport == rhi::Rect{0, 0, 1280, 720});
    CHECK(fit_viewport(ScaleMode::Integer, v, {1280, 720}).scale == vec2(4.0f, 4.0f));

    // 1920x1080 / 320x180 = 6 exactly; 1900x1080 only fits 5x horizontally.
    CHECK(fit_viewport(ScaleMode::Integer, v, {1920, 1080}).scale == vec2(6.0f, 6.0f));
    ViewportFit five = fit_viewport(ScaleMode::Integer, v, {1900, 1080});
    CHECK(five.scale == vec2(5.0f, 5.0f));
    CHECK(five.viewport == rhi::Rect{(1900 - 1600) / 2, (1080 - 900) / 2, 1600, 900});

    // Smaller than 1x: stays at 1x, centered and cropped (never shrinks pixel art).
    ViewportFit crop = fit_viewport(ScaleMode::Integer, v, {200, 100});
    CHECK(crop.scale == vec2(1.0f, 1.0f));
    CHECK(crop.viewport == rhi::Rect{-60, -40, 320, 180});
}

TEST_CASE("fit_viewport: Stretch fills the framebuffer") {
    ViewportFit s = fit_viewport(ScaleMode::Stretch, {1280, 720}, {1000, 1000});
    CHECK(s.viewport == rhi::Rect{0, 0, 1000, 1000});
    CHECK(s.scale.x() == doctest::Approx(1000.0f / 1280.0f));
    CHECK(s.scale.y() == doctest::Approx(1000.0f / 720.0f));
}

TEST_CASE("fit_viewport: degenerate sizes don't divide by zero") {
    for (ScaleMode mode : {ScaleMode::Fit, ScaleMode::Integer, ScaleMode::Stretch}) {
        const ViewportFit zero_fb = fit_viewport(mode, {1280, 720}, {0, 0});
        CHECK(zero_fb.viewport.width >= 0);
        const ViewportFit zero_virtual = fit_viewport(mode, {0, 0}, {800, 600});
        CHECK(zero_virtual.viewport.width >= 0);
    }
}

TEST_CASE("framebuffer_to_virtual maps pixels back through the letterbox") {
    const ViewportFit fit = fit_viewport(ScaleMode::Fit, {1280, 720}, {1920, 1200});
    check_near(framebuffer_to_virtual(fit, {0.0f, 60.0f}), {0.0f, 0.0f});
    check_near(framebuffer_to_virtual(fit, {1920.0f, 1140.0f}), {1280.0f, 720.0f});
    check_near(framebuffer_to_virtual(fit, {960.0f, 600.0f}), {640.0f, 360.0f});
    // Inside the bars maps outside the virtual space.
    CHECK(framebuffer_to_virtual(fit, {0.0f, 0.0f}).y() < 0.0f);
}

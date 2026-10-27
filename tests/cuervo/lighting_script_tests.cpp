// 2D lighting nodes (milestone 9.7) and tilemap occluders/collision
// (milestone 9.8), through the script host (DESIGN.md 4.17).

#include <doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cuervo/render/light_batcher.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/script/script_host.hpp"
#include "recording_device.hpp"

using namespace cuervo;
namespace fs = std::filesystem;
using cuervo::test::RecordingDevice;

namespace {

struct Host {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    explicit Host(const std::string& source) {
        script::HostOptions o;
        o.project_dir = fs::temp_directory_path() / "cuervo_lighting_tests";
        fs::create_directories(o.project_dir);
        o.transform_hierarchy = true;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
        REQUIRE_MESSAGE(host->running(), host->error());
    }

    void run(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            host->tick(InputFrame{});
            REQUIRE_MESSAGE(host->running(), host->error());
        }
    }

    std::int64_t integer(std::string_view n) { return host->session()->global(n).i; }
    float number(std::string_view n) { return host->session()->global(n).f; }
};

// What the lights looked like when Canvas.apply_lights ran.
struct Applied {
    int calls = 0;
    std::size_t lights = 0;
    std::size_t occluders = 0;
    bool ambient = false;
    float zoom = 0.0f;
    std::vector<render::LightDef> defs;
    std::vector<render::SdfOccluder> occs;
};

Applied draw_lit(Host& h) {
    RecordingDevice d;
    render::SdfBatcher shapes;
    render::TextRenderer text;
    render::LightBatcher lights;
    REQUIRE(shapes.init(d));
    REQUIRE(text.init(d));
    REQUIRE(lights.init(d));
    Applied a;
    script::DrawTargets t;
    t.shapes = &shapes;
    t.text = &text;
    t.flush = [](script::DrawFlush, const Affine2&, float) {};
    t.lights = &lights;
    t.apply_lights = [&](const Affine2&, float zoom) {
        ++a.calls;
        a.lights = lights.light_count();
        a.occluders = lights.occluder_count();
        a.ambient = lights.ambient().enabled;
        a.zoom = zoom;
        a.defs.assign(lights.lights().begin(), lights.lights().end());
        const auto occs = lights.all_occluders();
        a.occs.assign(occs.begin(), occs.end());
    };
    h.host->draw(t);
    REQUIRE(h.host->running());
    return a;
}

const char* lit_scene = R"(
class Main extends Node {
    fn ready() {
        var cam = Camera2D()
        cam.zoom = 2.0
        add_child(cam)
        var lamp = PointLight2D()
        lamp.position = vec2(100.0, 50.0)
        lamp.radius = 120.0
        lamp.scale = vec2(1.5, 1.5)
        lamp.spot_angle = 1.0
        lamp.rotation = 0.5
        add_child(lamp)
        var off = PointLight2D()
        off.enabled = false
        add_child(off)
        var sun = DirectionalLight2D()
        sun.rotation = 1.2
        add_child(sun)
        var night = AmbientLight2D()
        night.darkness = 0.7
        add_child(night)
        var holder = Node2D()
        holder.position = vec2(200.0, 0.0)
        add_child(holder)
        var wall = LightOccluder2D()
        wall.position = vec2(10.0, 20.0)
        wall.size = vec2(40.0, 8.0)
        wall.occluder_mask = 3
        holder.add_child(wall)
        var poly = LightOccluder2D()
        poly.shape = OccluderShape.Polygon
        poly.points = [vec2(0.0, 0.0), vec2(10.0, 0.0), vec2(10.0, 10.0)]
        poly.closed = false
        add_child(poly)
        // A HUD light is drawn after the world's lights are applied: it never shades it.
        var hud = CanvasLayer()
        add_child(hud)
        hud.add_child(PointLight2D())
    }
}
)";

} // namespace

TEST_CASE("lighting nodes: the tree's lights and occluders shade the world once, through its camera") {
    Host h(lit_scene);
    h.run();
    const Applied a = draw_lit(h);
    CHECK(a.calls == 1);
    CHECK(a.zoom == 2.0f);
    REQUIRE(a.lights == 2); // the lamp and the sun; the disabled one and the HUD's aren't there
    CHECK(a.ambient);
    REQUIRE(a.occluders == 3); // the wall and the polygon's two edges
    const render::LightDef& lamp = a.defs[0];
    CHECK(lamp.kind == render::LightKind::Point);
    CHECK(lamp.position.x() == doctest::Approx(100.0f));
    CHECK(lamp.radius == doctest::Approx(180.0f)); // times the global scale
    CHECK(lamp.rotation == doctest::Approx(0.5f));
    CHECK(lamp.spot_angle_rad == doctest::Approx(1.0f));
    CHECK(a.defs[1].kind == render::LightKind::Directional);
    CHECK(a.defs[1].rotation == doctest::Approx(1.2f));
    const render::SdfOccluder& wall = a.occs[0];
    CHECK(wall.kind == render::LightOccluderKind::RoundedRect);
    CHECK(wall.center.x() == doctest::Approx(210.0f)); // the parent's transform applies
    CHECK(wall.center.y() == doctest::Approx(20.0f));
    CHECK(wall.half_size.x() == doctest::Approx(20.0f));
    CHECK(wall.mask == 3u);
    CHECK(a.occs[1].kind == render::LightOccluderKind::Capsule);
}

TEST_CASE("lighting nodes: drawing without a light target is unlit and harmless") {
    Host h(lit_scene);
    h.run();
    RecordingDevice d;
    render::SdfBatcher shapes;
    render::TextRenderer text;
    REQUIRE(shapes.init(d));
    REQUIRE(text.init(d));
    h.host->draw(shapes, text, render::FontHandle{});
    CHECK(h.host->running());
}

TEST_CASE("lighting nodes: lighting is render only, the state hash doesn't see it") {
    const char* src = R"(
var t = 0.0
class Main extends Node {
    var lamp: PointLight2D
    fn ready() {
        lamp = PointLight2D()
        add_child(lamp)
        add_child(AmbientLight2D())
    }
    fn update(dt: float) {
        t += dt
        lamp.position = vec2(t * 10.0, 0.0)
    }
}
)";
    Host lit(src);
    Host unlit(src);
    for (int i = 0; i < 30; ++i) {
        lit.run();
        unlit.run();
        draw_lit(lit);
        CHECK(lit.host->state_hash() == unlit.host->state_hash());
    }
}

TEST_CASE("tilemap: solid tiles merge into rectangles and block light") {
    Host h(R"(
var g_rects = 0
var g_r0 = Rect2.of(0.0, 0.0, 0.0, 0.0)
var g_r1 = Rect2.of(0.0, 0.0, 0.0, 0.0)
var g_r2 = Rect2.of(0.0, 0.0, 0.0, 0.0)

class Main extends Node {
    fn ready() {
        var map = TileMapLayer()
        map.tile_size = vec2(16.0, 16.0)
        map.tileset.tiles.push(TileDef { id = 0, walkable = false })
        map.tileset.tiles.push(TileDef { id = 1, walkable = true })
        map.light_occluders = true
        // A 3x2 block, a walkable floor tile and a lone pillar under it.
        for y in 0..2 {
            for x in 0..3 {
                map.set_cell(x, y, 0)
            }
        }
        map.set_cell(3, 0, 1)
        map.set_cell(1, 2, 0)
        map.set_cell(1, 3, 0)
        var rects = map.solid_rects()
        g_rects = rects.len()
        g_r0 = rects[0]
        g_r1 = rects[1]
        add_child(map)
        // Edits rebuild the list.
        map.erase_cell(1, 3)
        g_r2 = map.solid_rects()[1]
    }
}
)");
    h.run();
    CHECK(h.integer("g_rects") == 2);
    const auto rect = [&](std::string_view n) {
        const pluma::Value v = h.host->session()->global(n);
        const auto& f = static_cast<const pluma::InstanceObj*>(v.o)->fields;
        const float* pos = pluma::vec_data(f[0]);
        const float* size = pluma::vec_data(f[1]);
        return std::array<float, 4>{pos[0], pos[1], size[0], size[1]};
    };
    CHECK(rect("g_r0") == std::array<float, 4>{0.0f, 0.0f, 3.0f, 2.0f});
    CHECK(rect("g_r1") == std::array<float, 4>{1.0f, 2.0f, 1.0f, 2.0f});
    CHECK(rect("g_r2") == std::array<float, 4>{1.0f, 2.0f, 1.0f, 1.0f});
    const Applied a = draw_lit(h);
    // No light in the tree: apply_lights skips the GPU, but the occluders were queued.
    CHECK(a.calls == 0);
}

#if CUERVO_FEATURE_PHYSICS
TEST_CASE("tilemap: TileMapCollision2D gives solid tiles collision, one box per rectangle") {
    Host h(R"(
var g_bodies = 0
var g_rest_y = 0.0

class Main extends Node {
    var crate: RigidBody2D
    fn ready() {
        var map = TileMapLayer()
        map.tile_size = vec2(32.0, 32.0)
        map.tileset.tiles.push(TileDef { id = 0, walkable = false })
        for x in 0..10 {
            map.set_cell(x, 5, 0)
            map.set_cell(x, 6, 0)
        }
        map.set_cell(0, 4, 0)
        var col = TileMapCollision2D()
        map.add_child(col)
        add_child(map)
        g_bodies = col.children.len()
        crate = RigidBody2D()
        crate.position = vec2(160.0, 40.0)
        var shape = CollisionShape2D()
        shape.size = vec2(20.0, 20.0)
        crate.add_child(shape)
        add_child(crate)
    }
    fn update(dt: float) {
        g_rest_y = crate.global_position().y
    }
}
)");
    h.run(180);
    CHECK(h.integer("g_bodies") == 2); // the 10x2 floor and the 1x1 bump
    // The floor's top is at y = 160; the crate's center rests 10 above it.
    CHECK(h.number("g_rest_y") == doctest::Approx(150.0f).epsilon(0.02));
}
#endif

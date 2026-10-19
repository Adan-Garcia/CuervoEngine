// Node2D transform hierarchy (milestone 9.1, DESIGN.md 4.17): children follow
// their parents when the hierarchy is on, a switched-off tree behaves as
// world-space, per-node top_level, to_local/to_global, the new nodes, and
// Line2D/Polygon2D drawing. Headless like node_tests.cpp.

#include <doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/script/script_host.hpp"
#include "cuervo/script/vector_cache.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

struct Host {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    explicit Host(const std::string& source, bool hierarchy = true) {
        script::HostOptions o;
        o.project_dir = fs::temp_directory_path() / "cuervo_hierarchy_tests";
        fs::create_directories(o.project_dir);
        o.transform_hierarchy = hierarchy;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
        REQUIRE_MESSAGE(host->running(), host->error());
    }
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            host->tick(InputFrame{});
            REQUIRE_MESSAGE(host->running(), host->error());
        }
    }
    float num(std::string_view n) { return host->session()->global(n).f; }
    std::int64_t integer(std::string_view n) { return host->session()->global(n).i; }
    vec2 v2(std::string_view n) {
        const pluma::Value v = host->session()->global(n);
        const float* d = pluma::vec_data(v);
        return {d[0], d[1]};
    }
};

bool near(float a, float b, float eps = 1e-3f) { return a - b < eps && b - a < eps; }

// parent at (100, 50), rotated a quarter turn, scaled 2; child at local (10, 0).
const char* kTree = R"(
class T {
    static var gp = vec2(0.0, 0.0)
    static var gr = 0.0
    static var gs = vec2(0.0, 0.0)
    static var round_trip = vec2(0.0, 0.0)
    static var top = vec2(0.0, 0.0)
    static var lp = vec2(0.0, 0.0)
}
class Main extends Node {
    var par: Node2D
    var child: Node2D
    var loose: Node2D
    fn ready() {
        par = Node2D()
        par.position = vec2(100.0, 50.0)
        par.rotation = PI / 2.0
        par.scale = vec2(2.0, 2.0)
        add_child(par)
        child = Node2D()
        child.position = vec2(10.0, 0.0)
        child.rotation = 0.25
        par.add_child(child)
        loose = Node2D()
        loose.position = vec2(7.0, 8.0)
        loose.top_level = true
        par.add_child(loose)
    }
    fn update(dt: float) {
        T.gp = child.global_position()
        T.gr = child.global_rotation()
        T.gs = child.global_scale()
        T.round_trip = child.to_local(child.to_global(vec2(3.0, 4.0)))
        T.top = loose.global_position()
        T.lp = child.position
    }
}
)";

} // namespace

TEST_CASE("hierarchy: globals compose parent rotation, scale and position") {
    Host h(kTree);
    h.run(1);
    // (10, 0) scaled 2 -> (20, 0), rotated 90 degrees -> (0, 20), plus (100, 50).
    CHECK(near(h.v2("T.gp").x(), 100.0f));
    CHECK(near(h.v2("T.gp").y(), 70.0f));
    CHECK(near(h.num("T.gr"), 1.5707963f + 0.25f));
    CHECK(near(h.v2("T.gs").x(), 2.0f));
    CHECK(near(h.v2("T.gs").y(), 2.0f));
    CHECK(near(h.v2("T.round_trip").x(), 3.0f));
    CHECK(near(h.v2("T.round_trip").y(), 4.0f));
    CHECK(near(h.v2("T.lp").x(), 10.0f)); // local stays local
    // top_level: ignores the parent.
    CHECK(near(h.v2("T.top").x(), 7.0f));
    CHECK(near(h.v2("T.top").y(), 8.0f));
}

TEST_CASE("hierarchy: moving a parent moves its children (on), not (off)") {
    const char* src = R"(
class T {
    static var gp = vec2(0.0, 0.0)
}
class Main extends Node {
    var par: Node2D
    var child: Node2D
    fn ready() {
        par = Node2D()
        add_child(par)
        child = Node2D()
        child.position = vec2(5.0, 5.0)
        par.add_child(child)
    }
    fn update(dt: float) {
        par.position = par.position + vec2(10.0, 0.0)
        T.gp = child.global_position()
    }
}
)";
    {
        Host h(src, true);
        h.run(3);
        CHECK(near(h.v2("T.gp").x(), 35.0f));
        CHECK(near(h.v2("T.gp").y(), 5.0f));
    }
    {
        Host h(src, false);
        h.run(3);
        CHECK(near(h.v2("T.gp").x(), 5.0f)); // world-space: the par does not matter
        CHECK(near(h.v2("T.gp").y(), 5.0f));
    }
}

TEST_CASE("hierarchy: set_global_position keeps the world position under a parent") {
    Host h(R"(
class T {
    static var gp = vec2(0.0, 0.0)
    static var lp = vec2(0.0, 0.0)
}
class Main extends Node {
    fn ready() {
        var par = Node2D()
        par.position = vec2(100.0, 0.0)
        par.scale = vec2(2.0, 2.0)
        add_child(par)
        var child = Node2D()
        par.add_child(child)
        child.set_global_position(vec2(120.0, 10.0))
        T.gp = child.global_position()
        T.lp = child.position
    }
}
)");
    CHECK(near(h.v2("T.gp").x(), 120.0f));
    CHECK(near(h.v2("T.gp").y(), 10.0f));
    CHECK(near(h.v2("T.lp").x(), 10.0f));
    CHECK(near(h.v2("T.lp").y(), 5.0f));
}

TEST_CASE("hierarchy: VisibleOnScreenNotifier2D follows its parent and signals") {
    Host h(R"(
class T {
    static var entered = 0
    static var exited = 0
}
class Main extends Node {
    var par: Node2D
    var t = 0
    fn ready() {
        par = Node2D()
        par.position = vec2(-1000.0, -1000.0)
        add_child(par)
        var n = VisibleOnScreenNotifier2D()
        n.screen_entered.connect(fn() { T.entered += 1 })
        n.screen_exited.connect(fn() { T.exited += 1 })
        par.add_child(n)
    }
    fn update(dt: float) {
        t += 1
        if t == 3 { par.position = vec2(640.0, 360.0) }
        if t == 6 { par.position = vec2(-1000.0, -1000.0) }
    }
}
)");
    h.run(10);
    CHECK(h.integer("T.entered") == 1);
    CHECK(h.integer("T.exited") == 1);
}

TEST_CASE("hierarchy: Line2D and Polygon2D draw through the vector cache, in global space") {
    Host h(R"(
class Main extends Node {
    fn ready() {
        var par = Node2D()
        par.position = vec2(300.0, 200.0)
        add_child(par)
        var line = Line2D()
        line.points.push(vec2(0.0, 0.0))
        line.points.push(vec2(50.0, 0.0))
        line.points.push(vec2(50.0, 50.0))
        line.width = 6.0
        par.add_child(line)
        var poly = Polygon2D()
        poly.points.push(vec2(0.0, 0.0))
        poly.points.push(vec2(40.0, 0.0))
        poly.points.push(vec2(20.0, 30.0))
        poly.outline_width = 2.0
        par.add_child(poly)
        par.add_child(Marker2D())
    }
}
)");
    render::SdfBatcher shapes;
    render::MeshBatcher meshes;
    script::VectorCache vectors(meshes);
    std::size_t mesh_instances = 0;
    script::DrawTargets t;
    t.shapes = &shapes;
    t.vectors = &vectors;
    t.flush = [&](script::DrawFlush what, const Affine2&, float) {
        if (what != script::DrawFlush::Shapes) {
            mesh_instances += meshes.instance_count();
            meshes.clear();
        }
    };
    h.host->draw(t);
    REQUIRE(h.host->running());
    CHECK(mesh_instances >= 2); // line + polygon (fill) at least
}

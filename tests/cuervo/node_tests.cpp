// The built-in node library (milestone 8.2, DESIGN.md 4.16): Shape, Path,
// Sprite, Text and Camera2D drawing, Timer, Area, Emitter2D and AudioPlayer
// behavior and determinism, a scene-file round trip per node, and the
// examples/node_level project. Headless: batchers without a device collect
// instances, the audio system is the null backend.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/features.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/scene_bridge.hpp"
#include "cuervo/script/script_host.hpp"
#include "cuervo/script/vector_cache.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_node_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Host {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    explicit Host(const std::string& source, fs::path dir = {}) {
        script::HostOptions o;
        o.project_dir = dir.empty() ? temp_dir("default") : dir;
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
    pluma::Value global(std::string_view n) { return host->session()->global(n); }
    std::int64_t integer(std::string_view n) { return global(n).i; }
    std::string str(std::string_view n) {
        pluma::Value v = global(n);
        return v.is_obj() ? static_cast<const pluma::StrObj*>(v.o)->s : std::string{};
    }
};

bool near(float a, float b, float eps = 1e-3f) { return a - b < eps && b - a < eps; }

} // namespace

TEST_CASE("nodes: Timer counts whole ticks: one-shot, repeating, paused, restart") {
    Host h(R"(
class T {
    static var once = 0
    static var once_tick = 0
    static var rep = 0
    static var held_left = 0.0
    static var restarted = 0
}
class Main extends Node {
    var held: Timer
    var late: Timer
    fn ready() {
        var once = Timer()
        once.wait_time = 0.1
        once.one_shot = true
        once.autostart = true
        once.timeout.connect(fn() {
            T.once += 1
            T.once_tick = Time.tick
        })
        add_child(once)
        var rep = Timer()
        rep.wait_time = 0.05
        rep.autostart = true
        rep.timeout.connect(fn() { T.rep += 1 })
        add_child(rep)
        held = Timer()
        held.wait_time = 0.05
        held.autostart = true
        held.paused = true
        add_child(held)
        late = Timer()
        late.wait_time = 0.2
        late.one_shot = true
        late.timeout.connect(fn() { T.restarted += 1 })
        add_child(late)
    }
    fn update(dt: float) {
        T.held_left = held.time_left()
        if Time.tick == 10 {
            late.restart(time = 0.05)
        }
    }
}
)");
    h.run(30);
    CHECK(h.integer("T.once") == 1);
    CHECK(h.integer("T.once_tick") == 6); // 0.1 s = 6 ticks, counted from tick 1
    CHECK(h.integer("T.rep") == 10);      // every 3 ticks
    CHECK(near(h.global("T.held_left").f, 0.05f));
    CHECK(h.integer("T.restarted") == 1); // started at tick 10, 3 ticks
}

TEST_CASE("nodes: Area enter/exit signals, masks, capsules and freed areas") {
#if CUERVO_FEATURE_COLLISION
    const char* source = R"(
class T {
    static var log = ""
    static var b_seen = 0
    static var overlaps_now = false
}
class Main extends Node {
    var a: Area
    var b: Area
    var c: Area
    fn ready() {
        a = Area()
        a.name = "a"
        a.radius = 10.0
        a.layer = 0
        a.mask = 2
        b = Area()
        b.name = "b"
        b.position = vec2(100.0, 0.0)
        b.radius = 10.0
        b.layer = 1
        b.mask = 0
        c = Area()
        c.name = "c"
        c.shape = AreaShape.Capsule
        c.length = 40.0
        c.radius = 2.0
        c.rotation = PI / 2.0
        c.position = vec2(0.0, 100.0)
        c.layer = 1
        c.mask = 0
        a.area_entered.connect(fn(o: Area) { T.log = T.log + "+" + o.name })
        a.area_exited.connect(fn(o: Area) { T.log = T.log + "-" + o.name })
        b.area_entered.connect(fn(o: Area) { T.b_seen += 1 })
        add_child(a)
        add_child(b)
        add_child(c)
    }
    fn update(dt: float) {
        if Time.tick == 2 {
            b.position = vec2(20.0, 0.0) // touching counts
        }
        if Time.tick == 4 {
            c.position = vec2(0.0, 30.0) // the capsule's end reaches a
        }
        if Time.tick == 5 {
            T.overlaps_now = a.overlaps(b) && a.overlaps(c)
        }
        if Time.tick == 6 {
            b.position = vec2(100.0, 0.0)
        }
        if Time.tick == 8 {
            c.queue_free()
        }
    }
}
)";
    Host h(source);
    h.run(12);
    CHECK(h.str("T.log") == "+b+c-b-c");
    CHECK(h.integer("T.b_seen") == 0); // b's mask is empty: detection is one-sided
    CHECK(h.global("T.overlaps_now").b);

    // Same run, same signals and hash.
    Host again(source);
    again.run(12);
    CHECK(again.str("T.log") == h.str("T.log"));
    CHECK(again.host->state_hash() == h.host->state_hash());
#endif
}

TEST_CASE("nodes: Emitter2D fires on schedule, hits areas, one-shots, and is deterministic") {
#if CUERVO_FEATURE_PARTICLES
    const char* source = R"(
class T {
    static var hits = 0
    static var ring_count = 0
    static var ring_shots = 0
    static var ring_emitting = true
    static var gun_shots = 0
}
class Main extends Node {
    var ring: Emitter2D
    var gun: Emitter2D
    fn ready() {
        gun = Emitter2D()
        gun.pattern = EmitPattern.Point
        gun.count = 1
        gun.speed = 600.0
        gun.interval = 0.1
        gun.life = 5.0
        gun.layer = 0
        gun.hits = 2
        gun.tag = #shot
        add_child(gun)
        var target = Area()
        target.position = vec2(100.0, 0.0)
        target.radius = 8.0
        target.layer = 1
        target.mask = 0
        target.hit.connect(fn(tag: name, n: int) {
            if tag == #shot {
                T.hits += n
            }
        })
        add_child(target)
        ring = Emitter2D()
        ring.position = vec2(500.0, 500.0)
        ring.count = 8
        ring.one_shot = true
        ring.life = 10.0
        ring.pattern = EmitPattern.Burst
        ring.spread = TAU
        ring.speed_jitter = 20.0
        add_child(ring)
    }
    fn update(dt: float) {
        T.ring_count = ring.particle_count()
        T.ring_shots = ring.shots
        T.ring_emitting = ring.emitting
        T.gun_shots = gun.shots
    }
}
)";
    Host h(source);
    h.run(60);
    CHECK(h.integer("T.gun_shots") == 10); // ticks 1, 7, ..., 55
    CHECK(h.integer("T.hits") >= 8);       // 10 px per tick: a shot reaches the target ~10 ticks later
    CHECK(h.integer("T.ring_shots") == 1);
    CHECK(h.integer("T.ring_count") == 8);
    CHECK_FALSE(h.global("T.ring_emitting").b);

    Host again(source);
    again.run(60);
    CHECK(again.host->state_hash() == h.host->state_hash());
    CHECK(again.integer("T.hits") == h.integer("T.hits"));
#endif
}

TEST_CASE("nodes: AudioPlayer autoplays from the simulation and stops on exit") {
#if CUERVO_FEATURE_AUDIO
    Host h(R"(
class T {
    static var voice = -1
    static var plays = 0
    static var after_stop = 0
}
class Main extends Node {
    var p: AudioPlayer
    fn ready() {
        p = AudioPlayer()
        p.sound = #missing
        p.autoplay = true
        p.looping = true
        p.bus = #music
        add_child(p)
        T.voice = p.voice
    }
    fn update(dt: float) {
        if Time.tick == 3 {
            T.plays = p.plays
            p.queue_free()
        }
        if Time.tick == 4 {
            T.after_stop = p.voice
        }
    }
}
)");
    h.run(5);
    CHECK(h.integer("T.voice") >= 0); // voice ids are handed out even for unknown sounds
    CHECK(h.integer("T.plays") == 1);
    CHECK(h.integer("T.after_stop") == -1);
#endif
}

TEST_CASE("nodes: Shape, Path, Sprite and Text draw through the camera; CanvasLayers stay on screen") {
    const fs::path dir = temp_dir("draw");
    fs::copy_file(fs::path(CUERVO_ASSETS_DIR) / "demo" / "robot.svg", dir / "robot.svg");
    Host h(R"(
class T {
    static var round_trip = vec2(0.0, 0.0)
}
class Main extends Node {
    fn ready() {
        var cam = Camera2D()
        cam.position = vec2(100.0, 50.0)
        cam.zoom = 2.0
        add_child(cam)
        var s = Shape()
        s.kind = ShapeKind.Rect
        s.position = vec2(10.0, 10.0)
        s.size = vec2(20.0, 10.0)
        add_child(s)
        var robot = Sprite()
        robot.asset = "robot.svg"
        robot.position = vec2(0.0, 0.0)
        add_child(robot)
        var missing = Sprite()
        missing.asset = "nope.svg"
        add_child(missing)
        var p = Path()
        p.points.push(vec2(0.0, 0.0))
        p.points.push(vec2(10.0, 0.0))
        p.points.push(vec2(10.0, 10.0))
        add_child(p)
        var bez = Path()
        bez.curve = PathCurve.Bezier
        bez.segments = 8
        bez.points.push(vec2(0.0, 0.0))
        bez.points.push(vec2(0.0, 10.0))
        bez.points.push(vec2(10.0, 10.0))
        bez.points.push(vec2(10.0, 0.0))
        add_child(bez)
        var label = Text()
        label.text = "Hello"
        add_child(label)
        var hud = CanvasLayer()
        hud.add_child(Shape())
        add_child(hud)
        T.round_trip = cam.screen_to_world(cam.world_to_screen(vec2(3.0, 4.0)))
    }
}
)",
           dir);
    const pluma::Value round_trip = h.global("T.round_trip"); // vec_data points into the value
    const float* rt = pluma::vec_data(round_trip);
    CHECK(near(rt[0], 3.0f));
    CHECK(near(rt[1], 4.0f));

    render::SdfBatcher shapes;
    render::TextRenderer text;
    auto atlas = render::load_font_file(std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf");
    REQUIRE(atlas.has_value());
    const render::FontHandle font = text.add_font(std::move(*atlas));
    render::MeshBatcher meshes;
    script::VectorCache vectors(meshes);

    struct Flush {
        script::DrawFlush what;
        Affine2 view;
        float zoom;
        std::size_t shapes;
        std::size_t meshes;
    };
    std::vector<Flush> log;
    script::DrawTargets t;
    t.shapes = &shapes;
    t.text = &text;
    t.font = font;
    t.vectors = &vectors;
    t.flush = [&](script::DrawFlush what, const Affine2& view, float zoom) {
        log.push_back({what, view, zoom, shapes.size(), meshes.instance_count()});
        if (what != script::DrawFlush::Shapes) meshes.clear();
    };
    h.host->draw(t);
    REQUIRE(h.host->running());

    // Through the camera: its position lands mid-screen, magnified twice.
    const Flush* world = nullptr;
    const Flush* sprite = nullptr;
    for (const Flush& f : log) {
        if (f.zoom == 2.0f && f.what == script::DrawFlush::Layer) world = &f;
        if (f.zoom == 2.0f && f.meshes == 1) sprite = &f;
    }
    REQUIRE(world != nullptr);
    REQUIRE(sprite != nullptr); // the robot, flushed before the shapes drawn after it
    CHECK(sprite->what == script::DrawFlush::Meshes);
    const vec2 mid = world->view.apply({100.0f, 50.0f});
    CHECK(near(mid.x(), 640.0f));
    CHECK(near(mid.y(), 360.0f));
    CHECK(near(world->view.apply({101.0f, 50.0f}).x(), 642.0f));
    // Shapes in the world: the rect, 2 polyline segments, 8 Bezier pieces.
    CHECK(world->shapes == 11);
    // The CanvasLayer's shape goes out last, in screen coordinates.
    const Flush& last = log.back();
    CHECK(last.zoom == 1.0f);
    CHECK(last.view == Affine2{});
    CHECK(last.shapes == 12);
    CHECK(vectors.loaded() == 2); // robot + the missing one (logged once)
    CHECK(!text.instances().empty());

    // The editor's view ignores cameras: everything in world coordinates.
    log.clear();
    t.cameras = false;
    shapes.clear();
    h.host->draw(t);
    for (const Flush& f : log) CHECK(f.zoom == 1.0f);
}

TEST_CASE("nodes: every node round-trips through a scene file with its fields") {
    const fs::path dir = temp_dir("roundtrip");
    std::string nodes = R"(
fn make() -> List<Node> {
    var out = List<Node>()
    var s = Shape()
    s.name = "shape"
    s.kind = ShapeKind.Arc
    s.radius = 33.0
    s.thickness = 7.0
    s.end_angle = 1.25
    s.fill = Color.hex("ff8000")
    s.stroke_width = 2.5
    out.push(s)
    var p = Path()
    p.name = "path"
    p.curve = PathCurve.Bezier
    p.closed = true
    p.points.push(vec2(1.0, 2.0))
    p.points.push(vec2(3.5, -4.0))
    p.width = 3.0
    out.push(p)
    var sp = Sprite()
    sp.name = "sprite"
    sp.asset = "assets/robot.svg"
    sp.centered = false
    sp.flip_h = true
    sp.tint = Color.hex("80ff80c0")
    out.push(sp)
    var tx = Text()
    tx.name = "text"
    tx.text = "hi \"there\""
    tx.align = #center
    tx.max_width = 200.0
    out.push(tx)
    var cam = Camera2D()
    cam.name = "camera"
    cam.zoom = 1.5
    cam.current = false
    cam.position = vec2(5.0, 6.0)
    out.push(cam)
    var tm = Timer()
    tm.name = "timer"
    tm.wait_time = 0.75
    tm.one_shot = true
    tm.autostart = true
    out.push(tm)
)";
#if CUERVO_FEATURE_COLLISION
    nodes += R"(
    var ar = Area()
    ar.name = "area"
    ar.shape = AreaShape.Capsule
    ar.length = 12.0
    ar.layer = 3
    ar.mask = 5
    ar.debug_draw = true
    out.push(ar)
)";
#endif
#if CUERVO_FEATURE_PARTICLES
    nodes += R"(
    var em = Emitter2D()
    em.name = "emitter"
    em.pattern = EmitPattern.Burst
    em.count = 7
    em.gravity = vec2(0.0, 98.0)
    em.particle_shape = #capsule
    em.fade = true
    em.emitting = false
    out.push(em)
)";
#endif
#if CUERVO_FEATURE_AUDIO
    nodes += R"(
    var au = AudioPlayer()
    au.name = "audio"
    au.sound = #boom
    au.bus = #ui
    au.pitch = 1.5
    au.looping = true
    out.push(au)
)";
#endif
    nodes += R"(
    return out
}
class T {
    static var saved = 0
}
class Main extends Node {
    fn ready() {
        for n in make() {
            if Scene.save(n, "out/{n.name}.scene") {
                var back = Scene.load("out/{n.name}.scene")
                if Scene.save(back, "out/{n.name}_again.scene") {
                    T.saved += 1
                }
            }
        }
    }
}
)";
    Host h(nodes, dir);
    struct Expect {
        const char* node;
        std::vector<std::string> lines;
    };
    std::vector<Expect> expects = {
        {"shape", {"node Shape \"shape\"", "kind: ShapeKind = ShapeKind.Arc", "radius: float = 33.0", "stroke_width: float = 2.5"}},
        {"path", {"node Path \"path\"", "curve: PathCurve = PathCurve.Bezier", "closed: bool = true", "(3.5, -4.0)"}},
        {"sprite", {"node Sprite \"sprite\"", "asset: str = \"assets/robot.svg\"", "flip_h: bool = true"}},
        {"text", {"node Text \"text\"", "align: name = #center", "max_width: float = 200.0"}},
        {"camera", {"node Camera2D \"camera\"", "zoom: float = 1.5", "current: bool = false"}},
        {"timer", {"node Timer \"timer\"", "wait_time: float = 0.75", "autostart: bool = true"}},
#if CUERVO_FEATURE_COLLISION
        {"area", {"node Area \"area\"", "shape: AreaShape = AreaShape.Capsule", "mask: int = 5"}},
#endif
#if CUERVO_FEATURE_PARTICLES
        {"emitter", {"node Emitter2D \"emitter\"", "pattern: EmitPattern = EmitPattern.Burst", "gravity: vec2 = (0.0, 98.0)",
                     "particle_shape: name = #capsule"}},
#endif
#if CUERVO_FEATURE_AUDIO
        {"audio", {"node AudioPlayer \"audio\"", "sound: name = #boom", "bus: name = #ui", "looping: bool = true"}},
#endif
    };
    CHECK(h.integer("T.saved") == static_cast<std::int64_t>(expects.size()));
    for (const Expect& e : expects) {
        CAPTURE(e.node);
        const fs::path first = dir / "out" / (std::string(e.node) + ".scene");
        const fs::path again = dir / "out" / (std::string(e.node) + "_again.scene");
        REQUIRE(fs::exists(first));
        CHECK(read_all(first) == read_all(again)); // saved, loaded and saved again: same bytes
        std::string err;
        auto doc = scene::load_file(first, err);
        REQUIRE_MESSAGE(doc.has_value(), err);
        const std::string text = scene::dump_text(*doc);
        for (const std::string& line : e.lines) {
            CAPTURE(line);
            CHECK(text.find(line) != std::string::npos);
        }
    }
}

TEST_CASE("nodes: examples/node_level needs the features of the nodes it uses, and replays") {
    const fs::path d = fs::path(CUERVO_EXAMPLES_DIR) / "node_level";
    std::string err;
    auto p = script::load_project(d, err);
    REQUIRE_MESSAGE(p.has_value(), err);
    const script::FeatureSet f = script::detect_features(*assets::Vfs::dev(d), *p, p->features);
    CHECK(f.collision); // Area
    CHECK(f.particles); // Emitter2D
    CHECK(f.audio);     // AudioPlayer (and a sound under assets/)
    CHECK(f.ui);        // the HUD label

#if CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI
    // The scene runs without scripts; two runs agree tick for tick.
    const auto run = [&](int ticks) {
        script::HostOptions o;
        o.project_dir = d;
        o.main_scene = p->main_scene;
        o.seed = p->seed;
        o.tick_rate = static_cast<float>(p->tick_rate);
        InputMap map;
        script::ScriptHost host(o, map);
        REQUIRE_MESSAGE(host.load({}, ""), host.error());
        REQUIRE_MESSAGE(host.running(), host.error());
        std::vector<std::uint64_t> hashes;
        for (int i = 0; i < ticks; ++i) {
            host.tick(InputFrame{});
            REQUIRE_MESSAGE(host.running(), host.error());
            hashes.push_back(host.state_hash());
        }
#if CUERVO_FEATURE_PARTICLES
        CHECK(host.particles().particle_count() > 0);
#endif
        return hashes;
    };
    CHECK(run(120) == run(120));
#endif
}

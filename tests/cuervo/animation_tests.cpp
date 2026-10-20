// Animation (milestone 9.4, DESIGN.md 4.17): property tracks and their
// interpolation, loop modes, the player, tweens, the state machine and
// determinism. Headless like node_tests.cpp.

#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include <fstream>

#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

std::vector<std::byte> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) out[i] = static_cast<std::byte>(raw[i]);
    return out;
}

struct Host {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    explicit Host(const std::string& source, fs::path dir = {}) {
        script::HostOptions o;
        o.project_dir = dir.empty() ? fs::temp_directory_path() / "cuervo_animation_tests" : dir;
        fs::create_directories(o.project_dir);
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
    bool boolean(std::string_view n) { return host->session()->global(n).b; }
    std::vector<float> vec(std::string_view n) {
        const pluma::Value v = host->session()->global(n);
        const float* d = pluma::vec_data(v);
        return {d, d + pluma::vec_size(v)};
    }
};

// Runs after the animation nodes (children update in order).
constexpr const char* kProbe = R"(
class Probe extends Node {
    var cb: fn() = fn() {}
    fn update(dt: float) {
        cb()
    }
}
)";

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

} // namespace

TEST_CASE("animation: a float track interpolates linearly per tick") {
    Host h(R"(
class T {
    static var v = 0.0
}
class Main extends Node {
    var s: Shape
    var p: AnimationPlayer
    fn ready() {
        s = Shape()
        s.name = "S"
        add_child(s)
        p = AnimationPlayer()
        var a = Animation { name = "a", length = 10 }
        a.key_float("S:radius", 0, 10.0)
        a.key_float("S:radius", 10, 20.0)
        p.animations.push(a)
        add_child(p)
        p.play("a")
        var pr = Probe()
        pr.cb = fn() { T.v = s.radius }
        add_child(pr)
    }
    fn update(dt: float) {}
}
)" + std::string(kProbe));
    h.run(1);
    CHECK(near(h.num("T.v"), 11.0f));
    h.run(4);
    CHECK(near(h.num("T.v"), 15.0f));
    h.run(10);
    CHECK(near(h.num("T.v"), 20.0f));
}

namespace {

// A node with one exported field of every type tracks can drive.
constexpr const char* kRig = R"(
class Rig extends Node2D {
    @export var f = 0.0
    @export var i = 0
    @export var b = false
    @export var v3 = vec3(0.0)
    @export var v4 = vec4(0.0)
    @export var c = Color { r = 0.0, g = 0.0, b = 0.0, a = 1.0 }
    @export var e = ShapeKind.Circle
    @export var n = #a
    var hidden = 0.0
}
class T {
    static var f = 0.0
    static var i = 0
    static var b = false
    static var v2 = vec2(0.0, 0.0)
    static var v3 = vec3(0.0, 0.0, 0.0)
    static var v4 = vec4(0.0, 0.0, 0.0, 0.0)
    static var cr = 0.0
    static var cg = 0.0
    static var e = 0
    static var n_is_b = false
}
)";

} // namespace

TEST_CASE("animation: every exportable type interpolates") {
    Host h(std::string(kRig) + std::string(kProbe) + R"(
class Main extends Node {
    fn ready() {
        var r = Rig()
        r.name = "R"
        add_child(r)
        var p = AnimationPlayer()
        var a = Animation { name = "a", length = 10 }
        a.key_float("R:f", 0, 10.0)
        a.key_float("R:f", 10, 20.0)
        a.key_int("R:i", 0, 0, AnimInterp.Linear)
        a.key_int("R:i", 10, 5, AnimInterp.Linear)
        a.key_bool("R:b", 0, false)
        a.key_bool("R:b", 6, true)
        a.key_vec2("R:position", 0, vec2(0.0, 100.0))
        a.key_vec2("R:position", 10, vec2(50.0, 0.0))
        a.key_vec3("R:v3", 0, vec3(0.0, 0.0, 0.0))
        a.key_vec3("R:v3", 10, vec3(10.0, 20.0, 30.0))
        a.key_vec4("R:v4", 0, vec4(1.0, 1.0, 1.0, 1.0))
        a.key_vec4("R:v4", 10, vec4(0.0, 2.0, 4.0, 8.0))
        a.key_color("R:c", 0, Color.rgb(0.0, 0.0, 0.0, 1.0))
        a.key_color("R:c", 10, Color.rgb(1.0, 0.5, 0.0, 1.0))
        a.key_int("R:e", 0, ShapeKind.Circle as int)
        a.key_int("R:e", 4, ShapeKind.Rect as int)
        a.key_name("R:n", 0, "a")
        a.key_name("R:n", 5, "b")
        p.animations.push(a)
        add_child(p)
        p.play("a")
        var pr = Probe()
        pr.cb = fn() {
            T.f = r.f
            T.i = r.i
            T.b = r.b
            T.v2 = r.position
            T.v3 = r.v3
            T.v4 = r.v4
            T.cr = r.c.r
            T.cg = r.c.g
            T.e = r.e as int
            T.n_is_b = r.n == #b
        }
        add_child(pr)
    }
}
)");
    h.run(5);
    CHECK(near(h.num("T.f"), 15.0f));
    CHECK(h.integer("T.i") == 3); // 2.5 rounds half up
    CHECK(!h.boolean("T.b"));
    auto v2 = h.vec("T.v2");
    CHECK(near(v2[0], 25.0f));
    CHECK(near(v2[1], 50.0f));
    auto v3 = h.vec("T.v3");
    CHECK(near(v3[0], 5.0f));
    CHECK(near(v3[1], 10.0f));
    CHECK(near(v3[2], 15.0f));
    auto v4 = h.vec("T.v4");
    CHECK(near(v4[0], 0.5f));
    CHECK(near(v4[1], 1.5f));
    CHECK(near(v4[2], 2.5f));
    CHECK(near(v4[3], 4.5f));
    CHECK(near(h.num("T.cr"), 0.5f));
    CHECK(near(h.num("T.cg"), 0.25f));
    CHECK(h.integer("T.e") == 1); // ShapeKind.Rect, set at tick 4
    CHECK(h.boolean("T.n_is_b"));
    h.run(1);
    CHECK(h.boolean("T.b")); // changes at its key (tick 6)
    h.run(10); // past the end: holds the last keys
    CHECK(near(h.num("T.f"), 20.0f));
    CHECK(h.integer("T.i") == 5);
    CHECK(near(h.num("T.cr"), 1.0f));
}

TEST_CASE("animation: easing curves and per-key interpolation") {
    Host h(std::string(kRig) + std::string(kProbe) + R"(
class Main extends Node {
    fn ready() {
        var r = Rig()
        r.name = "R"
        add_child(r)
        var p = AnimationPlayer()
        var a = Animation { name = "a", length = 40 }
        // f: step 0..10 holding 1, then ease-in quad 10..20 (1 -> 2), ease-out cubic 20..30 (2 -> 3),
        // ease-in-out quad 30..40 (3 -> 4)
        a.key_float("R:f", 0, 1.0, AnimInterp.Step)
        a.key_float("R:f", 10, 1.0, AnimInterp.EaseIn, Ease.Quad)
        a.key_float("R:f", 20, 2.0, AnimInterp.EaseOut, Ease.Cubic)
        a.key_float("R:f", 30, 3.0, AnimInterp.EaseInOut, Ease.Quad)
        a.key_float("R:f", 40, 4.0)
        // v3.x: Godot-style power ease with exponent 2
        a.key_vec3("R:v3", 0, vec3(0.0, 0.0, 0.0), AnimInterp.Curve, Ease.Sine, 2.0)
        a.key_vec3("R:v3", 40, vec3(8.0, 0.0, 0.0))
        p.animations.push(a)
        add_child(p)
        p.play("a")
        var pr = Probe()
        pr.cb = fn() {
            T.f = r.f
            T.v3 = r.v3
        }
        add_child(pr)
    }
}
)");
    h.run(5);
    CHECK(near(h.num("T.f"), 1.0f)); // step
    CHECK(near(h.vec("T.v3")[0], 8.0f * 0.125f * 0.125f)); // (5/40)^2 * 8
    h.run(10); // tick 15: u = 0.5 of the quad ease-in
    CHECK(near(h.num("T.f"), 1.25f));
    h.run(10); // tick 25: ease-out cubic at 0.5 = 0.875
    CHECK(near(h.num("T.f"), 2.875f));
    h.run(5); // tick 30 exactly: the key
    CHECK(near(h.num("T.f"), 3.0f));
    h.run(2); // tick 32: in-out quad at u = 0.2 -> 2 * 0.04 = 0.08
    CHECK(near(h.num("T.f"), 3.08f));
    h.run(3); // tick 35: in-out at 0.5 = 0.5
    CHECK(near(h.num("T.f"), 3.5f));
    h.run(10);
    CHECK(near(h.num("T.f"), 4.0f));
    CHECK(near(h.vec("T.v3")[0], 8.0f));
}

namespace {

// A program: a Rig "R" animated by player "P" (one float track R:f over 0..10 per loop mode, plus
// a 4-tick "short"), then `extra` in Main.ready after the nodes exist, and a probe copying R.f.
std::string loop_program(std::string_view extra) {
    return std::string(kRig) + kProbe + R"(
class S {
    static var finished = 0
    static var started = 0
    static var events = 0
    static var calls = 0
    static var last_len = 0
}
class Main extends Node {
    var p: AnimationPlayer
    fn ready() {
        var r = Rig()
        r.name = "R"
        add_child(r)
        p = AnimationPlayer()
        p.name = "P"
        for m in 0..3 {
            var a = Animation { name = "m{m}", length = 10 }
            a.mode = m == 0 ? AnimLoop.None : (m == 1 ? AnimLoop.Loop : AnimLoop.PingPong)
            a.key_float("R:f", 0, 0.0)
            a.key_float("R:f", 10, 10.0)
            p.animations.push(a)
        }
        var b = Animation { name = "short", length = 4 }
        b.key_float("R:f", 0, 100.0)
        b.key_float("R:f", 4, 104.0)
        p.animations.push(b)
        p.animation_finished.connect(fn(n: str) { S.finished += 1 })
        p.animation_started.connect(fn(n: str) { S.started += 1 })
        add_child(p)
        var pr = Probe()
        pr.cb = fn() { T.f = r.f }
        add_child(pr)
)" + std::string(extra) + R"(
    }
}
)";
}

} // namespace

TEST_CASE("animation: None stops at the end and finishes once") {
    Host h(loop_program("p.play(\"m0\")"));
    h.run(4);
    CHECK(near(h.num("T.f"), 4.0f));
    h.run(6);
    CHECK(near(h.num("T.f"), 10.0f));
    CHECK(h.integer("S.finished") == 1);
    h.run(5);
    CHECK(near(h.num("T.f"), 10.0f));
    CHECK(h.integer("S.finished") == 1);
    CHECK(h.integer("S.started") == 1);
}

TEST_CASE("animation: Loop wraps and never finishes") {
    Host h(loop_program("p.play(\"m1\")"));
    h.run(13);
    CHECK(near(h.num("T.f"), 3.0f));
    h.run(10);
    CHECK(near(h.num("T.f"), 3.0f));
    CHECK(h.integer("S.finished") == 0);
}

TEST_CASE("animation: PingPong runs back and forth") {
    Host h(loop_program("p.play(\"m2\")"));
    h.run(10);
    CHECK(near(h.num("T.f"), 10.0f));
    h.run(3);
    CHECK(near(h.num("T.f"), 7.0f));
    h.run(7);
    CHECK(near(h.num("T.f"), 0.0f));
    h.run(2);
    CHECK(near(h.num("T.f"), 2.0f));
    CHECK(h.integer("S.finished") == 0);
}

TEST_CASE("animation: speed scales ticks and a negative speed plays backwards") {
    SUBCASE("double speed") {
        Host h(loop_program("p.speed = 2.0\n p.play(\"m0\")"));
        h.run(3);
        CHECK(near(h.num("T.f"), 6.0f));
        h.run(2);
        CHECK(near(h.num("T.f"), 10.0f));
        CHECK(h.integer("S.finished") == 1);
    }
    SUBCASE("half speed") {
        Host h(loop_program("p.play(\"m0\", 0.5)"));
        h.run(4);
        CHECK(near(h.num("T.f"), 2.0f));
    }
    SUBCASE("backwards from the end") {
        Host h(loop_program("p.play_backwards(\"m0\")"));
        h.run(3);
        CHECK(near(h.num("T.f"), 7.0f));
        h.run(7);
        CHECK(near(h.num("T.f"), 0.0f));
        CHECK(h.integer("S.finished") == 1);
    }
}

TEST_CASE("animation: queue plays the next animation after finish") {
    Host h(loop_program("p.play(\"short\")\n p.queue(\"m0\")"));
    h.run(3);
    CHECK(near(h.num("T.f"), 103.0f));
    CHECK(h.integer("S.finished") == 0);
    h.run(1); // reaches 4: short finishes, m0 starts at 0
    CHECK(h.integer("S.finished") == 1);
    CHECK(near(h.num("T.f"), 0.0f));
    h.run(3);
    CHECK(near(h.num("T.f"), 3.0f));
    CHECK(h.integer("S.started") == 2);
}

TEST_CASE("animation: autoplay starts on entering the tree; method and event tracks fire once") {
    Host h(std::string(kRig) + kProbe + R"(
class S {
    static var events = 0
    static var calls = 0
    static var last_event = false
}
class Rig2 extends Node2D {
    @export var f = 0.0
    fn poke() {
        S.calls += 1
    }
}
class Main extends Node {
    fn ready() {
        var r = Rig2()
        r.name = "R"
        add_child(r)
        var p = AnimationPlayer()
        p.name = "P"
        var a = Animation { name = "a", length = 10, mode = AnimLoop.Loop }
        a.key_float("R:f", 0, 0.0)
        a.key_float("R:f", 10, 10.0)
        a.key_call("R:poke", 3)
        a.key_event(5, "step")
        p.animations.push(a)
        p.autoplay = "a"
        p.event.connect(fn(n: str) {
            S.events += 1
            S.last_event = n == "step"
        })
        add_child(p)
        var pr = Probe()
        pr.cb = fn() { T.f = r.f }
        add_child(pr)
    }
}
)");
    h.run(2);
    CHECK(h.integer("S.calls") == 0);
    h.run(1);
    CHECK(h.integer("S.calls") == 1);
    CHECK(h.integer("S.events") == 0);
    h.run(2);
    CHECK(h.integer("S.events") == 1);
    CHECK(h.boolean("S.last_event"));
    CHECK(near(h.num("T.f"), 5.0f));
    h.run(10); // one loop later each fires once more
    CHECK(h.integer("S.calls") == 2);
    CHECK(h.integer("S.events") == 2);
}

namespace {

// A Rig "R" and `body` run in Main.ready after it exists; Main keeps `tw`.
std::string tween_program(std::string_view body) {
    return std::string(kRig) + kProbe + R"(
class S {
    static var done = 0
    static var cb = 0
    static var mv = 0.0
}
class Main extends Node {
    var tw: Tween
    var r: Rig
    fn ready() {
        r = Rig()
        r.name = "R"
        add_child(r)
        var pr = Probe()
        pr.cb = fn() {
            T.f = r.f
            T.v2 = r.position
            T.cr = r.c.r
            T.i = r.i
        }
        add_child(pr)
)" + std::string(body) + R"(
    }
}
)";
}

} // namespace

TEST_CASE("tween: a property tween reaches its target in the given ticks") {
    Host h(tween_program(R"(
        tw = create_tween().tween_float(r, "f", 10.0, 10)
        tw.finished.connect(fn() { S.done += 1 })
)"));
    h.run(1);
    CHECK(near(h.num("T.f"), 1.0f)); // progress k/N after k ticks
    h.run(4);
    CHECK(near(h.num("T.f"), 5.0f));
    CHECK(h.integer("S.done") == 0);
    h.run(5);
    CHECK(near(h.num("T.f"), 10.0f));
    CHECK(h.integer("S.done") == 1);
    h.run(5);
    CHECK(near(h.num("T.f"), 10.0f));
    CHECK(h.integer("S.done") == 1);
}

TEST_CASE("tween: sequence runs steps in order, parallel runs together") {
    SUBCASE("sequence") {
        Host h(tween_program(R"(
        tw = create_tween()
            .tween_float(r, "f", 10.0, 10)
            .tween_vec2(r, "position", vec2(40.0, 0.0), 4)
            .tween_callback(fn() { S.cb += 1 })
)"));
        h.run(10);
        CHECK(near(h.num("T.f"), 10.0f));
        CHECK(near(h.vec("T.v2")[0], 0.0f)); // second step has not begun
        h.run(2);
        CHECK(near(h.vec("T.v2")[0], 20.0f));
        h.run(2);
        CHECK(near(h.vec("T.v2")[0], 40.0f));
        CHECK(h.integer("S.cb") == 1);
    }
    SUBCASE("parallel()") {
        Host h(tween_program(R"(
        tw = create_tween()
            .tween_float(r, "f", 10.0, 10)
            .parallel()
            .tween_vec2(r, "position", vec2(40.0, 0.0), 4)
)"));
        h.run(4);
        CHECK(near(h.num("T.f"), 4.0f));
        CHECK(near(h.vec("T.v2")[0], 40.0f));
        h.run(6);
        CHECK(near(h.num("T.f"), 10.0f));
    }
    SUBCASE("set_parallel(true)") {
        Host h(tween_program(R"(
        tw = create_tween().set_parallel(true)
            .tween_float(r, "f", 10.0, 10)
            .tween_float(r, "position:x", 1.0, 2)
)"));
        h.run(5);
        CHECK(near(h.num("T.f"), 5.0f));
    }
}

TEST_CASE("tween: easing, delay, int and color properties") {
    Host h(tween_program(R"(
        tw = create_tween().set_parallel(true)
        tw.tween_float(r, "f", 8.0, 8).with_ease(AnimInterp.EaseIn, Ease.Quad)
        tw.tween_int(r, "i", 10, 10)
        tw.tween_color(r, "c", Color.rgb(1.0, 0.0, 0.0, 1.0), 10).with_delay(5)
)"));
    h.run(4); // u = 0.5: quad ease-in is 0.25 of 8
    CHECK(near(h.num("T.f"), 2.0f));
    CHECK(h.integer("T.i") == 4);
    CHECK(near(h.num("T.cr"), 0.0f)); // delayed
    h.run(6); // tick 10: color is 5 of 10 ticks in
    CHECK(near(h.num("T.cr"), 0.5f));
    CHECK(h.integer("T.i") == 10);
    h.run(5);
    CHECK(near(h.num("T.cr"), 1.0f));
}

TEST_CASE("tween: tween_method, interval and loops") {
    Host h(tween_program(R"(
        tw = create_tween().set_loops(3)
            .tween_method(fn(v: float) { S.mv = v }, 0.0, 4.0, 4)
            .interval(2)
            .tween_callback(fn() { S.cb += 1 })
)"));
    h.run(2);
    CHECK(near(h.num("S.mv"), 2.0f));
    h.run(2);
    CHECK(near(h.num("S.mv"), 4.0f));
    h.run(30);
    CHECK(h.integer("S.cb") == 3); // three loops of (4 + 2 + callback)
}

TEST_CASE("tween: kill stops it, and it ends with its owner") {
    SUBCASE("kill") {
        Host h(tween_program(R"(
        tw = create_tween()
        tw.finished.connect(fn() { S.done += 1 })
        tw.tween_float(r, "f", 10.0, 10)
        var killer = create_tween().interval(3).tween_callback(fn() { tw.kill() })
)"));
        h.run(10);
        CHECK(near(h.num("T.f"), 3.0f));
        CHECK(h.integer("S.done") == 0);
    }
    SUBCASE("owner leaves the tree") {
        Host h(tween_program(R"(
        var other = Node()
        other.name = "O"
        add_child(other)
        var t2 = other.create_tween().tween_float(r, "f", 10.0, 10)
        var kill_other = create_tween().interval(3).tween_callback(fn() { other.queue_free() })
)"));
        h.run(12);
        CHECK(near(h.num("T.f"), 3.0f)); // frozen where the owner left
    }
}

TEST_CASE("tween: pause holds the tween without ending it") {
    Host h(tween_program(R"(
        tw = create_tween().tween_float(r, "f", 10.0, 10)
        var p = create_tween().interval(3).tween_callback(fn() { tw.pause() })
        var q = create_tween().interval(8).tween_callback(fn() { tw.resume() })
)"));
    h.run(7);
    CHECK(near(h.num("T.f"), 3.0f));
    h.run(10);
    CHECK(near(h.num("T.f"), 10.0f));
}

namespace {

// A state machine over three constant-pose animations of a Rig "R": idle (f = 10, b = false),
// walk (f = 30, b = true), jump (f = 50 -> 60 over 10 ticks, not looping). `Ctl` runs before the tree
// and calls `script` (the body of fn drive(t: int)) with the tick number.
std::string tree_program(std::string_view transitions, std::string_view drive) {
    return std::string(kRig) + kProbe + R"(
class S {
    static var seq = 0
    static var fading = false
    static var in_walk = false
    static var progress = 0.0
}
class Ctl extends Node {
    var tree: AnimationTree
    fn update(dt: float) {
        var t = Time.tick as int
)" + std::string(drive) + R"(
    }
}
class Main extends Node {
    fn ready() {
        var r = Rig()
        r.name = "R"
        add_child(r)
        var p = AnimationPlayer()
        p.name = "P"
        var idle = Animation { name = "idle", length = 20, mode = AnimLoop.Loop }
        idle.key_float("R:f", 0, 10.0)
        idle.key_bool("R:b", 0, false)
        var walk = Animation { name = "walk", length = 20, mode = AnimLoop.Loop }
        walk.key_float("R:f", 0, 30.0)
        walk.key_bool("R:b", 0, true)
        var jump = Animation { name = "jump", length = 10, mode = AnimLoop.None }
        jump.key_float("R:f", 0, 50.0)
        jump.key_float("R:f", 10, 60.0)
        jump.key_bool("R:b", 0, false)
        p.animations.push(idle)
        p.animations.push(walk)
        p.animations.push(jump)
        add_child(p)
        var ctl = Ctl()
        add_child(ctl)
        var tree = AnimationTree()
        tree.name = "Tree"
        tree.states.push(AnimStateDef { name = "idle", animation = "idle" })
        tree.states.push(AnimStateDef { name = "walk", animation = "walk" })
        tree.states.push(AnimStateDef { name = "jump", animation = "jump" })
        tree.state_changed.connect(fn(a: str, b: str) {
            S.seq = S.seq * 10 + (b == "idle" ? 1 : (b == "walk" ? 2 : 3))
        })
)" + std::string(transitions) + R"(
        ctl.tree = tree
        add_child(tree)
        var pr = Probe()
        pr.cb = fn() {
            T.f = r.f
            T.b = r.b
            S.fading = tree.is_fading()
            S.in_walk = tree.current == "walk"
            S.progress = tree.fade_progress()
        }
        add_child(pr)
    }
}
)";
}

constexpr const char* kIdleWalk = R"(
        tree.transitions.push(AnimTransition { from_state = "idle", to_state = "walk", duration = 10,
            conditions = [AnimCondition { param = "move", op = AnimCondOp.IsTrue }] })
        tree.transitions.push(AnimTransition { from_state = "walk", to_state = "idle", duration = 10,
            conditions = [AnimCondition { param = "move", op = AnimCondOp.IsFalse }] })
)";

} // namespace

TEST_CASE("state machine: starts in the first state and holds its pose") {
    Host h(tree_program(kIdleWalk, "if t == 1 { tree.set_param(\"x\", 0.0) }"));
    h.run(3);
    CHECK(near(h.num("T.f"), 10.0f));
    CHECK(!h.boolean("T.b"));
    CHECK(!h.boolean("S.fading"));
}

TEST_CASE("state machine: a condition starts a cross-fade that blends property values") {
    Host h(tree_program(kIdleWalk, "if t == 5 { tree.set_bool(\"move\", true) }"));
    h.run(5); // the transition fires on tick 5, the output is still the old pose
    CHECK(near(h.num("T.f"), 10.0f));
    CHECK(h.boolean("S.fading"));
    h.run(1); // 1 of 10 ticks
    CHECK(near(h.num("T.f"), 12.0f));
    h.run(3); // 4 of 10
    CHECK(near(h.num("T.f"), 18.0f));
    CHECK(!h.boolean("T.b")); // bools switch at half weight
    h.run(1); // 5 of 10: halfway
    CHECK(near(h.num("T.f"), 20.0f));
    CHECK(h.boolean("T.b"));
    h.run(5); // 10 of 10
    CHECK(near(h.num("T.f"), 30.0f));
    CHECK(!h.boolean("S.fading"));
    CHECK(h.integer("S.seq") == 2);
    h.run(10);
    CHECK(near(h.num("T.f"), 30.0f));
}

TEST_CASE("state machine: eased cross-fades and conditions on numbers") {
    Host h(tree_program(R"(
        tree.transitions.push(AnimTransition { from_state = "idle", to_state = "walk", duration = 10,
            interp = AnimInterp.EaseIn, ease_kind = Ease.Quad,
            conditions = [AnimCondition { param = "speed", op = AnimCondOp.Gt, value = 0.5 }] })
)", "if t == 3 { tree.set_param(\"speed\", 0.4) }\n if t == 6 { tree.set_param(\"speed\", 0.6) }"));
    h.run(8);
    CHECK(h.integer("S.seq") == 2); // fired at tick 6, not at 3
    // tick 8 is 2 ticks into the fade: u = 0.2, quad ease-in = 0.04 -> 10 + 20 * 0.04
    CHECK(near(h.num("T.f"), 10.8f));
}

TEST_CASE("state machine: triggers fire one transition and are consumed") {
    Host h(tree_program(R"(
        tree.transitions.push(AnimTransition { from_state = "*", to_state = "jump", duration = 2,
            conditions = [AnimCondition { param = "jump", op = AnimCondOp.Trigger }] })
        tree.transitions.push(AnimTransition { from_state = "jump", to_state = "idle", duration = 4, at_end = true })
)", "if t == 4 { tree.trigger(\"jump\") }"));
    h.run(4);
    CHECK(h.integer("S.seq") == 3); // jump entered on the trigger's tick
    CHECK(near(h.num("T.f"), 10.0f));
    h.run(1); // tick 5: halfway through the 2-tick fade, jump is 1 tick in (51)
    CHECK(near(h.num("T.f"), 30.5f));
    h.run(1); // tick 6: fade done, jump 2 ticks in
    CHECK(near(h.num("T.f"), 52.0f));
    h.run(8); // tick 14: the jump animation ended, at_end fires the way back
    CHECK(near(h.num("T.f"), 60.0f));
    CHECK(h.integer("S.seq") == 31);
    h.run(4); // 4-tick fade to idle
    CHECK(near(h.num("T.f"), 10.0f));
    h.run(30); // the trigger was consumed: no second jump
    CHECK(h.integer("S.seq") == 31);
}

TEST_CASE("state machine: goto cuts or fades, travel follows the transitions") {
    SUBCASE("goto") {
        Host h(tree_program("", R"(
 if t == 3 { tree.goto("walk") }
 if t == 6 { tree.goto("idle", 4) }
)"));
        h.run(3);
        CHECK(near(h.num("T.f"), 30.0f)); // a cut: no fade at all
        h.run(3);
        h.run(2); // tick 8: 2 of 4 ticks into the fade back
        CHECK(near(h.num("T.f"), 20.0f));
        h.run(2);
        CHECK(near(h.num("T.f"), 10.0f));
        CHECK(h.integer("S.seq") == 21);
    }
    SUBCASE("travel walks idle -> walk -> jump") {
        Host h(tree_program(R"(
        tree.transitions.push(AnimTransition { from_state = "idle", to_state = "walk", duration = 4,
            conditions = [AnimCondition { param = "never", op = AnimCondOp.IsTrue }] })
        tree.transitions.push(AnimTransition { from_state = "walk", to_state = "jump", duration = 4,
            conditions = [AnimCondition { param = "never", op = AnimCondOp.IsTrue }] })
)", "if t == 2 { tree.set_param(\"never\", 0.0)\n var ok = tree.travel(\"jump\")\n if !ok { tree.goto(\"idle\") } }"));
        h.run(2);
        // the first hop fires when travel() is called: walk is entered, then jump after its fade
        h.run(1);
        CHECK(h.integer("S.seq") == 2);
        h.run(4);
        CHECK(h.integer("S.seq") == 23);
        h.run(5); // tick 12: jump entered at tick 7, 5 ticks in
        CHECK(near(h.num("T.f"), 55.0f));
    }
}

TEST_CASE("state machine: a transition during a fade continues from the blended pose") {
    Host h(tree_program(kIdleWalk, "if t == 5 { tree.set_bool(\"move\", true) }\n if t == 9 { tree.set_bool(\"move\", false) }"));
    std::vector<float> f;
    for (int i = 0; i < 30; ++i) {
        h.run(1);
        f.push_back(h.num("T.f"));
    }
    // fade out of idle starts at tick 5, is cut at tick 9 (4 ticks in, value 18), and fades
    // back to idle from there: no jump larger than one tick of either fade
    for (std::size_t i = 1; i < f.size(); ++i) {
        INFO("tick " << i + 1 << " f=" << f[i]);
        CHECK(std::fabs(f[i] - f[i - 1]) <= 3.0f);
    }

    CHECK(near(f[3], 10.0f)); // tick 4
    CHECK(near(f[7], 16.0f)); // tick 8: 3 of 10 into the first fade
    CHECK(near(f[8], 16.0f)); // tick 9: interrupted, holds the blended pose for the decision tick
    CHECK(near(f[9], 15.4f)); // then fades back toward idle (10) over 10 ticks
    CHECK(near(f[29], 10.0f)); // settled back in idle
    CHECK(h.integer("S.seq") == 21);
}

TEST_CASE("animation: the state hash follows the animation and replays identically") {
    const std::string drive = "if t == 5 { tree.set_bool(\"move\", true) }";
    auto hashes = [&](const std::string& d) {
        Host h(tree_program(kIdleWalk, d));
        std::vector<std::uint64_t> out;
        for (int i = 0; i < 40; ++i) {
            h.run(1);
            out.push_back(h.host->state_hash());
        }
        return out;
    };
    const auto a = hashes(drive);
    const auto b = hashes(drive);
    CHECK(a == b);
    CHECK(a[3] != a[20]); // the digest moves with the animated values
    const auto c = hashes("if t == 6 { tree.set_bool(\"move\", true) }");
    CHECK(a[3] == c[3]);
    CHECK(a[4] != c[4]);
}

TEST_CASE("animation: animations, player and tree round-trip through a scene file") {
    const fs::path dir = fs::temp_directory_path() / "cuervo_animation_scene";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Host h(std::string(kRig) + kProbe + R"(
class S {
    static var saved = 0
    static var loaded_tracks = 0
    static var loaded_keys = 0
    static var states = 0
    static var transitions = 0
}
class Main extends Node {
    fn ready() {
        var root = Node2D()
        root.name = "Root"
        var r = Rig()
        r.name = "R"
        root.add_child(r)
        var p = AnimationPlayer()
        p.name = "P"
        var a = Animation { name = "a", length = 12, mode = AnimLoop.PingPong }
        a.key_float("R:f", 0, 1.0, AnimInterp.EaseIn, Ease.Cubic)
        a.key_float("R:f", 12, 5.0)
        a.key_color("R:c", 0, Color.rgb(1.0, 0.5, 0.25, 1.0))
        a.key_name("R:n", 3, "b")
        a.key_call("R:poke", 6)
        a.key_event(7, "evt")
        p.animations.push(a)
        p.autoplay = "a"
        p.speed = 1.5
        root.add_child(p)
        var t = AnimationTree()
        t.name = "T"
        t.states.push(AnimStateDef { name = "x", animation = "a", speed = 2.0 })
        t.transitions.push(AnimTransition { from_state = "*", to_state = "x", duration = 3,
            conditions = [AnimCondition { param = "go", op = AnimCondOp.Trigger, value = 0.5 }] })
        root.add_child(t)
        if Scene.save(root, "out/anim.scene") {
            var back = Scene.load("out/anim.scene")
            if Scene.save(back, "out/anim2.scene") {
                S.saved = 1
            }
            for c in back.children {
                if c is AnimationPlayer bp {
                    S.loaded_tracks = bp.animations[0].tracks.len()
                    S.loaded_keys = bp.animations[0].tracks[0].keys.len()
                }
                if c is AnimationTree bt {
                    S.states = bt.states.len()
                    S.transitions = bt.transitions.len()
                }
            }
        }
    }
}
)", dir);
    h.run(1);
    CHECK(h.integer("S.saved") == 1);
    CHECK(h.integer("S.loaded_tracks") == 5);
    CHECK(h.integer("S.loaded_keys") == 2);
    CHECK(h.integer("S.states") == 1);
    CHECK(h.integer("S.transitions") == 1);
    CHECK(read_bytes(dir / "out" / "anim.scene") == read_bytes(dir / "out" / "anim2.scene"));
    std::string err;
    auto doc = scene::load_file(dir / "out" / "anim.scene", err);
    REQUIRE_MESSAGE(doc.has_value(), err);
    const std::string text = scene::dump_text(*doc);
    CHECK(text.find("node AnimationPlayer \"P\"") != std::string::npos);
    CHECK(text.find("animations: List<Animation>") != std::string::npos);
    CHECK(text.find("path = \"R:f\"") != std::string::npos);
    CHECK(text.find("interp = AnimInterp.EaseIn") != std::string::npos);
    CHECK(text.find("node AnimationTree \"T\"") != std::string::npos);
    // The text view parses back to the same document (and so to the same file).
    auto parsed = scene::parse_text(text, err);
    REQUIRE_MESSAGE(parsed.has_value(), err);
    CHECK(*parsed == *doc);
    CHECK(scene::write_binary(*parsed) == scene::write_binary(*doc));
}

TEST_CASE("animation: tracks on missing nodes, hidden fields and unsupported types are skipped") {
    Host h(std::string(kRig) + kProbe + R"(
class Main extends Node {
    fn ready() {
        var r = Rig()
        r.name = "R"
        add_child(r)
        var p = AnimationPlayer()
        var a = Animation { name = "a", length = 10, mode = AnimLoop.Loop }
        a.key_float("R:f", 0, 1.0)
        a.key_float("R:f", 10, 11.0)
        a.key_float("Nope:f", 0, 5.0)
        a.key_float("R:hidden", 0, 5.0)
        a.key_float("R:missing", 0, 5.0)
        a.key_float("R:name", 0, 5.0)
        p.animations.push(a)
        add_child(p)
        p.play("a")
        var pr = Probe()
        pr.cb = fn() { T.f = r.f }
        add_child(pr)
    }
}
)");
    h.run(5);
    CHECK(near(h.num("T.f"), 6.0f)); // the valid track still runs
}

TEST_CASE("animation: the hash covers animated values even when nothing reads them") {
    auto hash_at = [](float to, int ticks) {
        Host h(std::string(kRig) + R"(
class Main extends Node {
    fn ready() {
        var r = Rig()
        r.name = "R"
        add_child(r)
        var p = AnimationPlayer()
        var a = Animation { name = "a", length = 10 }
        a.key_float("R:f", 0, 0.0)
        a.key_float("R:f", 10, )" + std::to_string(to) + R"()
        p.animations.push(a)
        add_child(p)
        p.play("a")
    }
}
)");
        h.run(ticks);
        return h.host->state_hash();
    };
    CHECK(hash_at(10.0f, 5) == hash_at(10.0f, 5));
    CHECK(hash_at(10.0f, 5) != hash_at(11.0f, 5));
    CHECK(hash_at(10.0f, 5) != hash_at(10.0f, 6));
}

TEST_CASE("tween: a coroutine can await finished") {
    Host h(std::string(kRig) + R"(
class S {
    static var woke = 0
    static var tick_woke = 0
}
class Main extends Node {
    fn ready() {
        var r = Rig()
        r.name = "R"
        add_child(r)
        start(sequence(r))
    }
    fn sequence(r: Rig) {
        var tw = create_tween().tween_float(r, "f", 5.0, 5)
        await tw.finished
        S.woke += 1
        S.tick_woke = Time.tick as int
    }
}
)");
    h.run(4);
    CHECK(h.integer("S.woke") == 0);
    h.run(4);
    CHECK(h.integer("S.woke") == 1);
    CHECK(h.integer("S.tick_woke") >= 5);
}

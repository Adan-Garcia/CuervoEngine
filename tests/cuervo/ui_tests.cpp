// UI nodes (cuervo/script/prelude/ui.plm, DESIGN.md 4.10), pausing, and
// scene files through the script host (Scene.save / Scene.load, DESIGN.md
// 4.12), plus the shmup's menu flow. Headless: no font, so UI.measure uses
// its fixed estimate; the tests size controls with `min_size`.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "engine_ui_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

const char* ui_actions[] = {"ui_up", "ui_down", "ui_left", "ui_right", "ui_accept", "ui_cancel", "ui_click"};

struct Host {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    explicit Host(const std::string& source, fs::path dir = {}) {
        Key keys[] = {Key::Up, Key::Down, Key::Left, Key::Right, Key::Enter, Key::Escape, Key::Space};
        for (std::size_t i = 0; i < std::size(ui_actions); ++i) map.set_action(Name(ui_actions[i]), {Binding::key(keys[i])});
        script::HostOptions o;
        o.project_dir = dir;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
    }

    InputFrame frame(std::initializer_list<const char*> held = {}, vec2 pointer = {0.0f, 0.0f}) {
        InputFrame f;
        for (const char* a : held) f.held |= std::uint64_t{1} << *map.action_index(Name(a));
        f.pointer = {static_cast<std::int16_t>(pointer.x()), static_cast<std::int16_t>(pointer.y())};
        return f;
    }
    void tick(const InputFrame& f) {
        host->tick(f);
        REQUIRE_MESSAGE(host->running(), host->error());
    }
    // Presses an action for one tick, then releases it for one.
    void press(const char* action, vec2 pointer = {0.0f, 0.0f}) {
        tick(frame({action}, pointer));
        tick(frame({}, pointer));
    }
    pluma::Value global(std::string_view n) { return host->session()->global(n); }
    std::int64_t integer(std::string_view n) { return global(n).i; }
    std::string str(std::string_view n) {
        pluma::Value v = global(n);
        return v.is_obj() ? static_cast<const pluma::StrObj*>(v.o)->s : std::string{};
    }
};

const pluma::Value& field(const pluma::Value& obj, std::string_view name) {
    const auto* o = static_cast<const pluma::InstanceObj*>(obj.o);
    for (std::size_t i = 0; i < o->type->field_names.size(); ++i) {
        if (o->type->field_names[i] == name) return o->fields[i];
    }
    FAIL("no field " << name);
    static pluma::Value none;
    return none;
}

vec2 field_vec2(const pluma::Value& obj, std::string_view name) {
    const float* d = pluma::vec_data(field(obj, name));
    return {d[0], d[1]};
}

const char* menu_source = R"(
class T {
    static var box: VBox
    static var slider: Slider
    static var presses = 0
    static var cancels = 0
    static var last = ""
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var box = VBox()
        box.set_anchors(#center)
        box.separation = 10.0
        for i in 0..3 {
            var b = Button()
            b.name = "b{i}"
            b.min_size = vec2(200.0, 40.0)
            b.pressed.connect(fn() {
                T.presses += 1
                T.last = b.name
            })
            box.add_child(b)
        }
        var s = Slider()
        s.name = "volume"
        s.min_size = vec2(200.0, 40.0)
        s.value = 0.5
        box.add_child(s)
        box.cancelled.connect(fn() { T.cancels += 1 })
        layer.add_child(box)
        T.box = box
        T.slider = s
    }
}
)";

} // namespace

TEST_CASE("ui: anchors and VBox layout are exact") {
    Host h(menu_source);
    h.tick(h.frame());
    pluma::Value box = h.global("T.box");
    // Slider min height is max(40, estimate); buttons are 200x40 + 10 gaps.
    const vec2 size = field_vec2(box, "size");
    CHECK(size.x() >= 200.0f);
    const float height = 3 * 40.0f + 3 * 10.0f + 40.0f;
    CHECK(size.y() == height);
    // Centered: grows both ways around the anchor.
    CHECK(field_vec2(box, "position") == vec2{640.0f - size.x() * 0.5f, 360.0f - height * 0.5f});
    const auto* kids = static_cast<const pluma::ListObj*>(field(box, "children").o);
    REQUIRE(kids->items.size() == 4);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(field_vec2(kids->items[i], "position").y() == 360.0f - height * 0.5f + 50.0f * static_cast<float>(i));
        CHECK(field_vec2(kids->items[i], "size") == vec2{size.x(), 40.0f}); // fills across
    }
}

TEST_CASE("ui: keyboard focus navigation, wrap-around, accept and cancel") {
    Host h(menu_source);
    h.tick(h.frame());
    auto focused_name = [&] {
        pluma::Value f = h.global("UI.focused");
        if (!f.is_obj()) return std::string("<none>");
        return static_cast<const pluma::StrObj*>(field(f, "name").o)->s;
    };
    CHECK(focused_name() == "<none>");
    h.press("ui_down"); // nothing focused: the first control takes focus
    CHECK(focused_name() == "b0");
    h.press("ui_down");
    CHECK(focused_name() == "b1");
    h.press("ui_down");
    h.press("ui_down");
    CHECK(focused_name() == "volume");
    h.press("ui_down"); // wraps to the top
    CHECK(focused_name() == "b0");
    h.press("ui_up"); // and back to the bottom
    CHECK(focused_name() == "volume");
    // Left/right go to the focused slider; it snaps to its increment.
    h.press("ui_right");
    CHECK(field(h.global("T.slider"), "value").f == doctest::Approx(0.6f));
    h.press("ui_left");
    h.press("ui_left");
    CHECK(field(h.global("T.slider"), "value").f == doctest::Approx(0.4f));
    CHECK(focused_name() == "volume");
    h.press("ui_up");
    h.press("ui_accept");
    CHECK(h.integer("T.presses") == 1);
    CHECK(h.str("T.last") == "b2");
    h.press("ui_cancel");
    CHECK(h.integer("T.cancels") == 1);

    // Key repeat: holding down moves once, then again after 24 ticks and
    // every 6 after that.
    h.press("ui_up");
    h.press("ui_up");
    CHECK(focused_name() == "b0");
    for (int i = 0; i < 31; ++i) h.tick(h.frame({"ui_down"}));
    CHECK(focused_name() == "volume"); // b0 -> b1 (press) -> b2 (tick 25) -> volume (tick 31)
}

TEST_CASE("ui: the pointer focuses on hover and clicks buttons and sliders") {
    Host h(menu_source);
    h.tick(h.frame());
    pluma::Value box = h.global("T.box");
    const vec2 pos = field_vec2(box, "position");
    const vec2 size = field_vec2(box, "size");
    const vec2 b1 = pos + vec2{size.x() * 0.5f, 50.0f + 20.0f};
    h.tick(h.frame({}, b1));
    pluma::Value f = h.global("UI.focused");
    REQUIRE(f.is_obj());
    CHECK(static_cast<const pluma::StrObj*>(field(f, "name").o)->s == "b1");
    h.press("ui_click", b1);
    CHECK(h.integer("T.presses") == 1);
    CHECK(h.str("T.last") == "b1");
    // Clicking outside every control does nothing.
    h.press("ui_click", {5.0f, 5.0f});
    CHECK(h.integer("T.presses") == 1);
    // Dragging a slider's knob to the far right sets it to the maximum.
    const float slider_y = pos.y() + 150.0f + 20.0f;
    h.tick(h.frame({"ui_click"}, {pos.x() + 2.0f, slider_y}));
    h.tick(h.frame({"ui_click"}, {1270.0f, slider_y}));
    h.tick(h.frame({}, {1270.0f, slider_y}));
    CHECK(field(h.global("T.slider"), "value").f == doctest::Approx(1.0f));
}

TEST_CASE("ui: Scene.paused stops updates and coroutines except always_process nodes") {
    Host h(R"(
class Counter extends Node {
    var n = 0
    var waited = 0
    fn ready() {
        start(count())
    }
    fn count() {
        loop {
            await wait_ticks(1)
            waited += 1
        }
    }
    fn update(dt: float) {
        n += 1
    }
}
class T {
    static var game: Counter
    static var menu: Counter
}
class Main extends Node {
    fn ready() {
        T.game = Counter()
        T.menu = Counter()
        T.menu.always_process = true
        add_child(T.game)
        var holder = Node()
        holder.always_process = true
        add_child(holder)
        holder.add_child(T.menu) // inherits always_process through its parent
        T.menu.always_process = false
    }
}
)");
    for (int i = 0; i < 5; ++i) h.tick(h.frame());
    pluma::Value game = h.global("T.game");
    pluma::Value menu = h.global("T.menu");
    CHECK(field(game, "n").i == 5);
    CHECK(field(menu, "n").i == 5);
    const std::int64_t waited = field(game, "waited").i;
    h.host->session()->set_global("Scene.paused", pluma::Value::boolean(true));
    for (int i = 0; i < 7; ++i) h.tick(h.frame());
    CHECK(field(game, "n").i == 5);
    CHECK(field(game, "waited").i == waited);
    CHECK(field(menu, "n").i == 12);
    CHECK(field(menu, "waited").i == waited + 7);
    h.host->session()->set_global("Scene.paused", pluma::Value::boolean(false));
    h.tick(h.frame());
    CHECK(field(game, "n").i == 6);
}

TEST_CASE("scene files: Scene.save and Scene.load round-trip exported fields through the VM") {
    const fs::path dir = temp_dir("roundtrip");
    Host h(R"(
enum Mode { A, B }
struct Stats {
    var speed = 1.0
    var tag = #none
}
class Item extends Node2D {
    @export var hp = 3
    @export var tint = Color.hex("ff0000")
    @export var tags = List<name>()
    @export var mode = Mode.B
    @export var stats = Stats {}
    @export var label = "x"
    @export var big: u64 = 1
    var runtime_only = 7
}
class T {
    static var saved = false
    static var saved_again = false
    static var hp = 0
    static var kids = 0
    static var kid_mode_a = false
    static var tags = 0
    static var speed = 0.0
    static var label = ""
}
class Main extends Node {
    fn ready() {
        var root = Item()
        root.name = "root"
        root.hp = 10
        root.tags.push(#a)
        root.tags.push(#b)
        root.position = vec2(1.5, -2.25)
        root.stats = Stats { speed = 4.5, tag = #fast }
        root.label = "quote \" and\nnewline"
        root.big = 18446744073709551615
        root.runtime_only = 99
        var kid = Item()
        kid.name = "kid"
        kid.mode = Mode.A
        root.add_child(kid)
        var tmp = Item()
        tmp.transient = true
        root.add_child(tmp)
        T.saved = Scene.save(root, "out/item.scene")
        var loaded = Scene.load("out/item.scene") as Item
        T.hp = loaded.hp
        T.kids = loaded.children.len()
        T.kid_mode_a = (loaded.find("kid") as Item).mode == Mode.A
        T.tags = loaded.tags.len()
        T.speed = loaded.stats.speed
        T.label = loaded.label
        T.saved_again = Scene.save(loaded, "out/again.scene")
    }
}
)",
           dir);
    CHECK(h.global("T.saved").b);
    CHECK(h.global("T.saved_again").b);
    CHECK(h.integer("T.hp") == 10);
    CHECK(h.integer("T.kids") == 1); // the transient child is not saved
    CHECK(h.global("T.kid_mode_a").b);
    CHECK(h.integer("T.tags") == 2);
    CHECK(h.global("T.speed").f == 4.5f);
    CHECK(h.str("T.label") == "quote \" and\nnewline");

    // Saving the loaded tree again gives the same bytes.
    auto bytes = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    CHECK(bytes(dir / "out/item.scene") == bytes(dir / "out/again.scene"));

    std::string error;
    auto doc = scene::load_file(dir / "out/item.scene", error);
    REQUIRE_MESSAGE(doc.has_value(), error);
    const std::string text = scene::dump_text(*doc);
    CHECK(text.find("node Item \"root\" {") != std::string::npos);
    CHECK(text.find("    hp: int = 10\n") != std::string::npos);
    CHECK(text.find("    tags: List<name> = [#a, #b]\n") != std::string::npos);
    CHECK(text.find("    mode: Mode = Mode.B\n") != std::string::npos);
    CHECK(text.find("    position: vec2 = (1.5, -2.25)\n") != std::string::npos);
    CHECK(text.find("    stats: Stats = Stats { speed = 4.5, tag = #fast }\n") != std::string::npos);
    CHECK(text.find("    big: u64 = 18446744073709551615u64\n") != std::string::npos);
    CHECK(text.find("tint: Color = Color { r = 1.0, g = 0.0, b = 0.0, a = 1.0 }") != std::string::npos);
    CHECK(text.find("runtime_only") == std::string::npos);
    CHECK(text.find("        mode: Mode = Mode.A\n") != std::string::npos);
}

TEST_CASE("scene files: changed schemas load with defaults and warnings, bad scenes panic") {
    const fs::path dir = temp_dir("schema");
    scene::SceneDoc doc;
    scene::Node n;
    n.type = "Item";
    n.name = "old";
    n.fields.push_back({"hp", "str", scene::Value::str("ten")});     // type changed since
    n.fields.push_back({"gone", "int", scene::Value::integer(1)});   // field removed since
    n.fields.push_back({"secret", "int", scene::Value::integer(5)}); // no longer exported
    n.fields.push_back({"speed", "float", scene::Value::number(9.5)});
    doc.nodes.push_back(n);
    std::string error;
    REQUIRE(scene::save_file(dir / "old.scene", doc, error));
    scene::Node bad;
    bad.type = "NotANode";
    scene::SceneDoc bad_doc;
    bad_doc.nodes.push_back(bad);
    REQUIRE(scene::save_file(dir / "bad.scene", bad_doc, error));

    Host h(R"(
class NotANode {}
class Item extends Node {
    @export var hp = 3
    @export var speed = 1.0
    var secret = 0
}
class T {
    static var hp = 0
    static var speed = 0.0
    static var secret = 0
    static var name = ""
}
class Main extends Node {
    fn ready() {
        var it = Scene.load("old.scene") as Item
        T.hp = it.hp
        T.speed = it.speed
        T.secret = it.secret
        T.name = it.name
    }
    fn update(dt: float) {
        if Time.tick == 2 {
            Scene.load("bad.scene")
        }
    }
}
)",
           dir);
    CHECK(h.integer("T.hp") == 3);
    CHECK(h.global("T.speed").f == 9.5f);
    CHECK(h.integer("T.secret") == 0);
    CHECK(h.str("T.name") == "old");
    h.host->tick(InputFrame{});
    CHECK(h.host->running());
    h.host->tick(InputFrame{});
    CHECK(h.host->panicked());
    CHECK(h.host->error().find("'NotANode' is not a class extending Node") != std::string::npos);
}

TEST_CASE("scene files: paths must stay inside the project") {
    const fs::path dir = temp_dir("paths");
    Host h(R"(
class T {
    static var ok = true
}
class Main extends Node {
    fn ready() {
        T.ok = Scene.save(self, "../escape.scene")
    }
}
)",
           dir);
    CHECK_FALSE(h.global("T.ok").b);
    CHECK_FALSE(fs::exists(dir.parent_path() / "escape.scene"));
}

TEST_CASE("shmup: title -> play -> pause -> resume / quit to title, frozen while paused") {
    const fs::path shmup = fs::path(CUERVO_EXAMPLES_DIR) / "shmup";
    std::string error;
    auto project = script::load_project(shmup, error);
    REQUIRE_MESSAGE(project.has_value(), error);
    InputMap map;
    REQUIRE(script::apply_input(*project, map, error));
    std::vector<pluma::SourceFile::Part> sources;
    REQUIRE(script::read_scripts(*project, sources, nullptr, error));
    script::HostOptions o;
    o.seed = project->seed;
    o.project_dir = project->dir;
    script::ScriptHost host(o, map);
    REQUIRE_MESSAGE(host.load(sources, project->main_class), host.error());

    auto frame = [&](const char* action) {
        InputFrame f;
        if (action != nullptr) f.held = std::uint64_t{1} << *map.action_index(Name(action));
        return f;
    };
    auto run = [&](int n, const char* action = nullptr) {
        for (int i = 0; i < n; ++i) {
            host.tick(frame(i == 0 ? action : nullptr));
            REQUIRE_MESSAGE(host.running(), host.error());
        }
    };
    auto state = [&] {
        pluma::Value g = host.session()->global("Game.main");
        return std::string(host.session()->vm().name_text(static_cast<std::uint32_t>(field(g, "state").i)));
    };
    auto wave = [&] { return field(host.session()->global("Game.main"), "wave").i; };

    run(30);
    CHECK(state() == "title");
    run(2, "ui_accept"); // Start is focused
    CHECK(state() == "playing");
    run(300);
    CHECK(wave() >= 1);
    run(2, "pause");
    CHECK(state() == "paused");
    CHECK(host.session()->global("Scene.paused").b);
    const std::uint64_t particles = host.particles().particle_count();
    const auto hash = host.particles().state_hash();
    run(120);
    CHECK(host.particles().particle_count() == particles);
    CHECK(host.particles().state_hash() == hash);
    run(2, "ui_accept"); // Resume is focused
    CHECK(state() == "playing");
    CHECK_FALSE(host.session()->global("Scene.paused").b);
    run(2, "pause");
    run(2, "ui_up"); // wraps from Resume to Quit to Title
    run(2, "ui_accept");
    CHECK(state() == "title");
    CHECK_FALSE(host.session()->global("Scene.paused").b);
}

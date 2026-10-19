// cuervo/script: project files, the script host (bindings, node tree,
// particles, collision), determinism and hot reload, run headless on the
// shmup example (examples/shmup).

#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "cuervo/math/dmath.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

const fs::path shmup_dir = fs::path(CUERVO_EXAMPLES_DIR) / "shmup";

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
struct Shmup {
    script::Project project;
    InputMap map;
    std::vector<pluma::SourceFile::Part> sources;
    std::unique_ptr<script::ScriptHost> host;

    Shmup() {
        std::string error;
        auto p = script::load_project(shmup_dir, error);
        REQUIRE_MESSAGE(p.has_value(), error);
        project = *p;
        REQUIRE_MESSAGE(script::apply_input(project, map, error), error);
        REQUIRE_MESSAGE(script::read_scripts(project, sources, nullptr, error), error);
        script::HostOptions o;
        o.seed = project.seed;
        o.tick_rate = project.tick_rate;
        o.project_dir = project.dir;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load(sources, project.main_class), host->error());
    }

    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            host->tick(InputFrame{});
            REQUIRE_MESSAGE(host->running(), host->error());
        }
    }

    std::int64_t game_int(std::string_view field) {
        pluma::VM& vm = host->session()->vm();
        pluma::Value g = host->session()->global("Game.main");
        auto* inst = static_cast<pluma::InstanceObj*>(g.o);
        const pluma::RecordRT* rt = inst->type;
        for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
            if (rt->field_names[i] == field) return inst->fields[i].i;
        }
        (void)vm;
        FAIL("no field " << field);
        return 0;
    }
};
#endif

} // namespace

TEST_CASE("script project: project.toml and input bindings") {
    std::string error;
    auto p = script::load_project(shmup_dir, error);
    REQUIRE_MESSAGE(p.has_value(), error);
    CHECK(p->name == "Shmup");
    CHECK(p->main_class == "Game");
    CHECK(p->virtual_size == ivec2{1280, 720});
    CHECK(p->seed == 20261015u);
    InputMap map;
    REQUIRE(script::apply_input(*p, map, error));
    CHECK(map.action_index(Name("shoot")).has_value());
    CHECK(map.axis_index(Name("move.x")).has_value());
    CHECK(map.axis_index(Name("move.y")).has_value());

    DeviceState devices;
    devices.set_key(Key::Z, true);
    devices.set_key(Key::Left, true);
    Input input(map);
    input.push(map.sample(devices));
    CHECK(input.held(Name("shoot")));
    CHECK(input.axis(Name("move.x")) == -1.0f);

    script::Project bad = *p;
    bad.actions = {{"jump", {"Key.Nope"}}};
    InputMap m2;
    CHECK_FALSE(script::apply_input(bad, m2, error));
    CHECK(error == "action 'jump': unknown input 'Key.Nope'");
}

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
TEST_CASE("script host: the shmup plays itself headless, deterministically") {
    Shmup a;
    a.run(1500);
    CHECK(a.game_int("score") > 0);   // the demo pilot shot something
    CHECK(a.game_int("wave") >= 1);
#if CUERVO_FEATURE_PARTICLES
    CHECK(a.host->particles().particle_count() > 0);
#endif

    Shmup b;
    b.run(1500);
    CHECK(a.host->state_hash() == b.host->state_hash());
    CHECK(a.game_int("score") == b.game_int("score"));

    // draw() feeds the canvas onto the batchers (uninitialized batchers
    // collect instances without a device).
    render::SdfBatcher shapes;
    render::TextRenderer text;
    a.host->draw(shapes, text, render::FontHandle{});
    CHECK(shapes.size() > 50);
    CHECK(a.host->running());
}

TEST_CASE("script host: hot reload keeps the game's state and runs the new code") {
    Shmup s;
    s.run(600);
    const std::int64_t score = s.game_int("score");
    const std::int64_t wave = s.game_int("wave");
    // Edit: a new field, and a changed constant in update().
    for (pluma::SourceFile::Part& part : s.sources) {
        if (part.path.ends_with("game.plm")) {
            std::string from = "    fn update(dt: float) {\n";
            auto at = part.text.find(from);
            if (at == std::string::npos) {
                from = "    fn update(dt: float) {\r\n";
                at = part.text.find(from);
            }
            REQUIRE(at != std::string::npos);
            const std::string nl = from.ends_with("\r\n") ? "\r\n" : "\n";
            part.text.replace(at, from.size(),
                              "    var reloaded_ticks = 0" + nl + "    fn update(dt: float) {" + nl +
                                  "        reloaded_ticks += 1" + nl);
        }
    }
    REQUIRE_MESSAGE(s.host->load(s.sources, "Game"), s.host->error());
    CHECK(s.host->reloads() == 1);
    CHECK(s.game_int("score") == score);
    CHECK(s.game_int("wave") == wave);
    s.run(10);
    CHECK(s.game_int("reloaded_ticks") == 10);

    // A broken edit keeps the running game.
    std::vector<pluma::SourceFile::Part> broken = s.sources;
    broken[0].text += "\nvar oops: int = \"no\"\n";
    CHECK_FALSE(s.host->load(broken, "Game"));
    CHECK(s.host->error().find("mismatched types") != std::string::npos);
    s.run(10);
    CHECK(s.game_int("reloaded_ticks") == 20);
}
#endif

#if CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
TEST_CASE("examples/bench: fills to its target, deterministically") {
    const auto run = [](int ticks) {
        std::string error;
        auto project = script::load_project(fs::path(CUERVO_EXAMPLES_DIR) / "bench", error);
        REQUIRE_MESSAGE(project.has_value(), error);
        InputMap map;
        REQUIRE_MESSAGE(script::apply_input(*project, map, error), error);
        std::vector<pluma::SourceFile::Part> sources;
        REQUIRE_MESSAGE(script::read_scripts(*project, sources, nullptr, error), error);
        script::HostOptions o;
        o.seed = project->seed;
        o.tick_rate = project->tick_rate;
        o.project_dir = project->dir;
        script::ScriptHost host(o, map);
        REQUIRE_MESSAGE(host.load(sources, project->main_class), host.error());
        for (int i = 0; i < ticks; ++i) {
            host.tick(InputFrame{});
            REQUIRE_MESSAGE(host.running(), host.error());
        }
        return std::pair{host.particles().particle_count(), host.state_hash()};
    };
    const auto [count, hash] = run(120);
    // 50k by default; a whole ring per shot may overshoot slightly.
    CHECK(count >= 45000);
    CHECK(count <= 50100);
    CHECK(run(120).second == hash);
}
#endif

TEST_CASE("script host: scripts use the engine's deterministic math") {
    InputMap map;
    script::ScriptHost host({}, map);
    REQUIRE_MESSAGE(host.load({{"main.plm", R"(
class Main extends Node {
    var s = 0.0
    var c = 0.0
    var r = vec2(0.0, 0.0)
    fn ready() {
        s = sin(1.25)
        c = cos(1.25)
        r = vec2(1.0, 0.0).rotated(0.5)
    }
}
)"}},
                              "Main"),
                    host.error());
    pluma::Value root = host.session()->global("Scene.root");
    auto* inst = static_cast<pluma::InstanceObj*>(root.o);
    const std::size_t base = inst->fields.size() - 3;
    CHECK(inst->fields[base].f == dmath::sin(1.25f));
    CHECK(inst->fields[base + 1].f == dmath::cos(1.25f));
    CHECK(pluma::vec_data(inst->fields[base + 2])[1] == dmath::sin(0.5f));
}

TEST_CASE("canvas: rect passes its corner radius and rotation to the SDF shape") {
    // Regression: Canvas.rect read `radius` from the `rotation` argument and
    // `rotation` from past the end of the call (found while building Phase
    // 6A's editor example, whose walls are rotated blocks).
    InputMap map;
    script::ScriptHost host(script::HostOptions{}, map);
    const char* source = "class Main extends Node {\n"
                         "    fn draw(g: Canvas) {\n"
                         "        g.rect(vec2(10.0, 20.0), vec2(100.0, 40.0), fill = Color.hex(\"ff0000\"), radius = 6.0,\n"
                         "               rotation = 0.5)\n"
                         "    }\n"
                         "}\n";
    REQUIRE_MESSAGE(host.load({{"main.plm", source}}, "Main"), host.error());
    render::SdfBatcher shapes;
    render::TextRenderer text;
    host.draw(shapes, text, render::FontHandle{});
    REQUIRE(shapes.size() == 1);
    const render::SdfInstance& r = shapes.instances()[0];
    CHECK(r.shape == render::SdfShape::RoundedRect);
    CHECK(r.center == vec2{60.0f, 40.0f});
    CHECK(r.half_size == vec2{50.0f, 20.0f});
    CHECK(r.param == 6.0f);
    CHECK(r.rotation == 0.5f);
}

TEST_CASE("canvas: arc draws stroked arcs and filled sectors") {
    InputMap map;
    script::ScriptHost host(script::HostOptions{}, map);
    const char* source = "class Main extends Node {\n"
                         "    fn draw(g: Canvas) {\n"
                         "        // 1. stroke only\n"
                         "        g.arc(vec2(50.0, 50.0), 30.0, 0.0, 3.14159, stroke = Color.hex(\"00ff00\"), width = 2.0)\n"
                         "        // 2. fill only\n"
                         "        g.arc(vec2(150.0, 50.0), 30.0, 0.0, 1.57079, fill = Color.hex(\"ff0000\"))\n"
                         "        // 3. both fill and stroke\n"
                         "        g.arc(vec2(250.0, 50.0), 30.0, 0.0, 3.14159, fill = Color.hex(\"0000ff\"), stroke = Color.hex(\"ffff00\"), width = 2.0)\n"
                         "    }\n"
                         "}\n";
    REQUIRE_MESSAGE(host.load({{"main.plm", source}}, "Main"), host.error());
    render::SdfBatcher shapes;
    render::TextRenderer text;
    host.draw(shapes, text, render::FontHandle{});
    REQUIRE(shapes.size() == 4);
    // 1st arc: one stroke band
    CHECK(shapes.instances()[0].shape == render::SdfShape::Arc);
    // 2nd arc: 1 sector
    CHECK(shapes.instances()[1].shape == render::SdfShape::Sector);
    CHECK(shapes.instances()[1].center == vec2(150.0f, 50.0f));
    // 3rd arc: 1 sector + its stroke
    CHECK(shapes.instances()[2].shape == render::SdfShape::Sector);
    CHECK(shapes.instances()[2].center == vec2(250.0f, 50.0f));
    CHECK(shapes.instances()[3].shape == render::SdfShape::Arc);
}


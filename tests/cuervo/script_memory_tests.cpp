// cuervo/script and the language's memory model (DESIGN.md 5.10): the cycle
// collector runs at a fixed point at the end of every tick, the frame arena
// is freed at the end of the tick and the scene arena when the root changes.

#include <doctest.h>

#include <algorithm>
#include <filesystem>

#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "pluma/heap.hpp"

using namespace cuervo;

namespace {

struct Game {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    explicit Game(const std::string& src, std::uint64_t budget = 20000) {
        script::HostOptions o;
        o.gc_budget = budget;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"game.plm", src}}, "Game"), host->error());
        REQUIRE_MESSAGE(host->running(), host->error());
    }
    pluma::VM& vm() { return host->session()->vm(); }
    std::int64_t global_int(std::string_view name) { return host->session()->global(name).i; }
};

} // namespace

TEST_CASE("script memory: cycles are collected at the end of each tick, not during it") {
    Game g(R"(
class Pair { var other: Pair }
class Watch { weak var seen: Pair }
var w = Watch()
var nulls = 0
var alive = 0
class Game extends Node {
    fn update(dt: float) {
        // What the previous tick dropped was collected after its scripts.
        if w.seen == null { nulls += 1 }
        var a = Pair()
        a.other = Pair()
        a.other.other = a
        w.seen = a
        // Garbage now, but the collector doesn't run inside the tick.
        a = null
        if w.seen != null { alive += 1 }
    }
}
)");
    const std::size_t before = pluma::live_objects();
    const std::uint64_t collected = pluma::heap_stats().collected;
    for (int i = 0; i < 100; ++i) {
        g.host->tick(InputFrame{});
        REQUIRE_MESSAGE(g.host->running(), g.host->error());
    }
    CHECK(g.global_int("nulls") == 100);
    CHECK(g.global_int("alive") == 100);
    CHECK(pluma::heap_stats().collected - collected == 200);
    CHECK(pluma::live_objects() <= before + 2);
}

TEST_CASE("script memory: the frame arena is freed at the end of the tick") {
    Game g(R"(
class Tmp { var v = 1 }
fn scratch() -> Tmp { return new(frame) Tmp() }
var ticks = 0
class Game extends Node {
    var held: Tmp
    fn update(dt: float) {
        ticks += 1
        var pts = new(frame) List<vec2>()
        pts.push(vec2(1.0, 2.0))
        if ticks == 3 {
            held = scratch()       // escapes through a call: caught at run time
        }
        if ticks == 4 {
            print(held.v)
        }
    }
}
)");
    for (int i = 0; i < 3; ++i) {
        g.host->tick(InputFrame{});
        REQUIRE(g.host->running());
        CHECK(g.vm().arena_objects(pluma::ArenaKind::Frame) == 0);
    }
    g.host->tick(InputFrame{});
    CHECK(g.host->panicked());
    CHECK(g.host->error().find("use after free: 'held'; it was freed by the frame arena at the end of tick") !=
          std::string::npos);
}

TEST_CASE("script memory: the scene arena is freed when the root changes") {
    Game g(R"(
class Boss { var hp = 10 }
class Game extends Node {
    var boss: Boss
    fn ready() { boss = new(scene) Boss() }
    fn update(dt: float) { boss.hp -= 1 }
}
)");
    g.host->tick(InputFrame{});
    REQUIRE(g.host->running());
    CHECK(g.vm().arena_objects(pluma::ArenaKind::Scene) == 1);
    pluma::Value other;
    REQUIRE(g.vm().instantiate(g.vm().find_class("Node"), &other).ok);
    pluma::Value old = g.host->root();
    pluma::retain(old);
    CHECK(g.host->set_root(other, false));
    pluma::release(other);
    CHECK(g.vm().arena_objects(pluma::ArenaKind::Scene) == 0);
    // The old scene's objects are gone: using them panics.
    pluma::RunResult r = g.vm().call_method(old, "update", std::vector<pluma::Value>{pluma::Value::f32(0.1f)});
    CHECK_FALSE(r.ok);
    REQUIRE(r.panic.has_value());
    CHECK(r.panic->message.find("freed by the scene arena when the scene unloaded") != std::string::npos);
    pluma::release(old);
}

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
TEST_CASE("script memory: the shmup's heap stays bounded") {
    namespace fs = std::filesystem;
    std::string error;
    auto p = script::load_project(fs::path(CUERVO_EXAMPLES_DIR) / "shmup", error);
    REQUIRE_MESSAGE(p.has_value(), error);
    InputMap map;
    REQUIRE(script::apply_input(*p, map, error));
    std::vector<pluma::SourceFile::Part> sources;
    REQUIRE(script::read_scripts(*p, sources, nullptr, error));
    script::HostOptions o;
    o.seed = p->seed;
    o.tick_rate = p->tick_rate;
    o.project_dir = p->dir;
    script::ScriptHost host(o, map);
    REQUIRE_MESSAGE(host.load(sources, p->main_class), host.error());
    const pluma::HeapStats start = pluma::heap_stats();
    std::size_t peak = 0;
    for (int i = 0; i < 1200; ++i) {
        host.tick(InputFrame{});
        REQUIRE_MESSAGE(host.running(), host.error());
        peak = std::max(peak, pluma::live_objects());
    }
    const pluma::HeapStats end = pluma::heap_stats();
    MESSAGE("shmup, 1200 ticks: live " << start.live << " -> " << end.live << " (peak " << peak << "), collected "
                                       << end.collected - start.collected << " in " << end.sets - start.sets
                                       << " sets, " << end.work - start.work << " work units");
    CHECK(peak < start.live * 4 + 1000);
}
#endif

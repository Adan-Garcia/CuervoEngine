// Hot reload (DESIGN.md 5.13, pluma/reload.hpp).

#include <doctest.h>

#include "pluma/reload.hpp"
#include "pluma/session.hpp"

namespace {

struct Run {
    std::string out;
    std::unique_ptr<pluma::Session> session;
    explicit Run(const std::string& src) {
        pluma::VmOptions o;
        o.out = [this](std::string_view s) { out += s; };
        session = std::make_unique<pluma::Session>(o);
        pluma::SessionResult r = session->load_files({{"game.plm", src}});
        INFO(r.rendered);
        REQUIRE(r.ok);
    }
};

} // namespace

TEST_CASE("reload: fields by name and type, identity, globals, coroutines") {
    Run v1(R"(
enum Mode { Calm, Angry }
struct Stats { var hp = 3; var tag = "old" }
class Ship {
    var pos = vec2(1.0, 2.0)
    var stats = Stats {}
    var mode = Mode.Calm
    var score = 0
    var buddy: Ship
    var gone = 1
    fn run() {
        loop {
            score += 1
            yield
        }
    }
}
var ship = Ship()
var ticks = 0
var removed = 5
fn setup() {
    ship.pos = vec2(9.0, 8.0)
    ship.stats.hp = 1
    ship.mode = Mode.Angry
    ship.buddy = ship
}
)");
    pluma::VM& old_vm = v1.session->vm();
    REQUIRE(old_vm.call(*old_vm.proto_by_key("setup"), {}).ok);
    pluma::Value ship = v1.session->global("ship");
    pluma::retain(ship);
    REQUIRE(old_vm.call_method(ship, "run").ok == false); // a coroutine can't be called directly from the host...
    // ...so start it from script:
    pluma::release(ship);

    Run v1b(R"(
class Ship {
    var score = 0
    fn run() {
        loop {
            score += 1
            yield
        }
    }
}
var ship = Ship()
fn go() { start(ship.run()) }
)");
    pluma::VM& a = v1b.session->vm();
    REQUIRE(a.call(*a.proto_by_key("go"), {}).ok);
    for (int i = 0; i < 4; ++i) a.tick_coroutines();

    Run v2(R"(
class Ship {
    var score = 100        // kept: same name and type
    var lives = 3          // new: initializer
    fn run() {
        loop {
            score += 10    // the restarted coroutine runs the new code
            yield
        }
    }
}
var ship = Ship()
)");
    pluma::VM& b = v2.session->vm();
    pluma::Migrator m(a, b);
    m.begin();
    m.migrate_globals();
    pluma::RunResult r = m.finish();
    REQUIRE(r.ok);
    CHECK(m.coroutines_restarted() == 1);
    // score was 5 (started + 4 ticks) and the restart added 10.
    pluma::Value s = v2.session->global("ship");
    auto* inst = static_cast<pluma::InstanceObj*>(s.o);
    CHECK(inst->fields[0].i == 15);
    CHECK(inst->fields[1].i == 3);
    b.tick_coroutines();
    CHECK(inst->fields[0].i == 25);
}

TEST_CASE("reload: changed types reset with warnings; nested values migrate") {
    Run v1(R"(
enum Mode { Calm, Angry }
struct Stats { var hp = 3; var tag = "old" }
class Ship {
    var pos = vec2(1.0, 2.0)
    var stats = Stats {}
    var mode = Mode.Calm
    var buddy: Ship
    var level = 1
    var trail = List<vec2>()
}
var ship = Ship()
var other = 2
fn setup() {
    ship.pos = vec2(9.0, 8.0)
    ship.stats.hp = 1
    ship.mode = Mode.Angry
    ship.buddy = ship
    ship.level = 7
    ship.trail.push(vec2(1.0, 1.0))
}
)");
    pluma::VM& a = v1.session->vm();
    REQUIRE(a.call(*a.proto_by_key("setup"), {}).ok);
    Run v2(R"(
enum Mode { Angry, Calm, Sleepy }
struct Stats { var tag = "new"; var hp = 3; var mana = 9 }
class Ship {
    var level = "one"
    var buddy: Ship
    var mode = Mode.Sleepy
    var stats = Stats {}
    var pos = vec2(0.0, 0.0)
    var trail = List<vec2>()
    fn describe() -> str { return "{pos} {stats.hp} {stats.tag} {stats.mana} {mode} {level} {trail} {buddy == self}" }
}
var ship = Ship()
var other = 2.5
)");
    pluma::VM& b = v2.session->vm();
    pluma::Migrator m(a, b);
    m.begin();
    m.migrate_globals();
    REQUIRE(m.finish().ok);
    pluma::Value out;
    REQUIRE(b.call_method(v2.session->global("ship"), "describe", {}, &out).ok);
    CHECK(static_cast<pluma::StrObj*>(out.o)->s == "vec2(9.0, 8.0) 1 old 9 Mode.Angry one [vec2(1.0, 1.0)] true");
    pluma::release(out);
    REQUIRE(m.warnings().size() == 2);
    CHECK(m.warnings()[1] == "'other' changed type from 'int' to 'float'; it was reset");
    CHECK(m.warnings()[0] == "field 'Ship.level' changed type from 'int' to 'str'; it was reset");
}

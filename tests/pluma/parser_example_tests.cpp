// Whole examples from the spec documents, copied verbatim.

#include <doctest.h>

#include "tests/pluma/parse_helpers.hpp"

using test::check_round_trip;
using test::Root;

namespace {

// LANGUAGE.md 8.
constexpr std::string_view player_example = R"(class Player extends Node2D: Damageable {
    signal died

    @export(range = 50.0..400.0)
    var speed = 200.0
    @export
    var focus_speed = 80.0

    var lives = 3 {
        set(v) {
            lives = v
            if lives <= 0 { emit died }
        }
    }

    var _invuln = 0.0

    fn update(dt: float) {
        var s = Input.held(#focus) ? focus_speed : speed
        position += Input.axis(#move) * s * dt
        position = position.clamp(Screen.min, Screen.max)

        if Input.held(#shoot) {
            shots.aimed(at = position + vec2(0.0, -1.0), speed = 600.0)
        }
        _invuln = max(_invuln - dt, 0.0)
    }

    fn hurt(amount: int) {
        if _invuln > 0.0 { return }
        lives -= amount
        _invuln = 2.0
        start(blink())
    }

    fn blink() {
        for i in 0..10 {
            visible = !visible
            await wait(0.1)
        }
        visible = true
    }

    fn draw(g: Canvas) {
        if Input.held(#focus) {
            g.circle(position, 3.0, fill = Color.hex("ffffff"))
        }
    }
}
)";

// DESIGN.md 5.2.
constexpr std::string_view boss_example = R"(// Global class: usable anywhere by name.
class Boss extends Enemy {
    var hp = 400                  // inferred int
    var phase: Phase = Phase.Opening
    signal defeated

    fn ready() {
        start(run_phases())       // start a coroutine
    }

    fn run_phases() {
        while hp > 0 {
            match phase {
                Phase.Opening => {
                    emitter.ring(count = 36, speed = 180.0, every = 0.25)
                    await wait(6.0)
                    phase = Phase.Spiral(arms = 4)
                }
                Phase.Spiral(arms) => {
                    emitter.spiral(arms = arms, spin = 12.0, every = 0.05)
                    await until(fn() { hp < 150 })
                    phase = Phase.Rage
                }
                Phase.Rage => {
                    for i in 0..8 {
                        emitter.aimed(at = Player.main.position, spread = 5)
                        await wait(0.1)
                    }
                }
            }
        }
        emit defeated
    }

    fn draw(g: Canvas) {
        if phase is Phase.Rage {
            g.circle(position, 48.0, stroke = Color.hex("ff4060"), width = 3.0)
        }
    }
}

enum Phase {
    Opening,
    Spiral(arms: int),
    Rage,
}
)";

// LANGUAGE.md 3, run as a script.
constexpr std::string_view statements_example = R"(// assignment (statement only)
x = 1
x += 1;  x -= 1;  x *= 2;  x /= 2;  x %= 3
pos.x = 0.0
list[i] = v

if hp <= 0 {
    die()
} else if hp < 20 {
    flash()
} else {
    heal(1)
}

if target? t { t.hurt(1) }
if phase is Phase.Spiral(arms) { print(arms) }

match phase {
    Phase.Opening => start_opening()
    Phase.Spiral(arms) if arms > 3 => {
        hard_spiral(arms)
    }
    Phase.Spiral(arms) => spiral(arms)
    Phase.Rage => rage()
}

match key {
    0 | 1 => low()
    2..=9 => mid()
    _ => high()
}

for i in 0..8 { }
for i in 0..=8 step 2 { }
for e in enemies { }
for i, e in enemies { }
while hp > 0 { }
loop { if done { break } }
break; continue
outer: for y in 0..h { for x in 0..w { if hit { break outer } } }

foreach p in sparks.pool { p.age += dt }

return value
await wait(0.5)
yield                          // wait one tick
start(run_phases())
emit defeated
defer close(file)              // runs at scope exit, in reverse order
)";

// LANGUAGE.md 5.
constexpr std::string_view foreach_example = R"(fn update(dt: float) {
    foreach p in trail.pool {
        p.vel = p.vel.rotate(sin(time * 4.0) * 0.02)
        p.pos += p.vel * dt
        if p.age >= p.life { trail.pool.kill(p) }
    }
}
)";

} // namespace

TEST_CASE("example: Player (LANGUAGE.md 8)") {
    test::Parsed p = test::parse(player_example, Root::Module);
    CHECK_MESSAGE(p.errors.empty(), p.rendered);
    CHECK(p.dump.rfind("(module (class Player (extends Node2D) (traits Damageable) (signal died) ", 0) == 0);
    CHECK(p.dump.find("(var s (init (?: (call (. Input held) #focus) focus_speed speed)))") != std::string::npos);
    CHECK(p.dump.find("(named at (+ position (call vec2 (float 0) (neg (float 1)))))") != std::string::npos);
    check_round_trip(player_example, Root::Module);
}

TEST_CASE("example: Boss (DESIGN.md 5.2)") {
    test::Parsed p = test::parse(boss_example, Root::Module);
    CHECK_MESSAGE(p.errors.empty(), p.rendered);
    CHECK(p.dump.find("(await (call until (lambda (params) (block (< hp 150)))))") != std::string::npos);
    CHECK(p.dump.find("(= phase (call (. Phase Spiral) (named arms 4)))") != std::string::npos);
    CHECK(p.dump.find("(enum Phase Opening (Spiral (params (arms int))) Rage)") != std::string::npos);
    check_round_trip(boss_example, Root::Module);
}

TEST_CASE("example: statement tour (LANGUAGE.md 3)") {
    test::Parsed p = test::parse(statements_example);
    CHECK_MESSAGE(p.errors.empty(), p.rendered);
    check_round_trip(statements_example);
}

TEST_CASE("example: foreach over a particle pool (LANGUAGE.md 5)") {
    test::Parsed p = test::parse(foreach_example, Root::Module);
    CHECK_MESSAGE(p.errors.empty(), p.rendered);
    check_round_trip(foreach_example, Root::Module);
}

TEST_CASE("snippets from the Decided/Clarified notes in LANGUAGE.md") {
    constexpr std::string_view snippets = R"(var help = """
    Usage:
      move with {key_names}
    """
var shards = Pool<Shard>(capacity = 512)
foreach s in shards { s.ttl -= dt; if s.ttl <= 0.0 { shards.kill(s) } }
if hits(Point { x = 1 }) { }
var a = -90deg
var up = vec2(0.0, -1.0)
var x = a ?? (x => x)
task.start()
)";
    test::Parsed p = test::parse(snippets);
    CHECK_MESSAGE(p.errors.empty(), p.rendered);
    CHECK(p.dump.find(R"((str multiline "Usage:\n  move with " ({} key_names)))") != std::string::npos);
    check_round_trip(snippets);
    test::Parsed attr = test::parse("class A {\n    @export(range = 0..1.0, step = 0.1)\n    var f = 0.5\n}",
                                    Root::Module);
    CHECK_MESSAGE(attr.errors.empty(), attr.rendered);
    CHECK(attr.dump.find("(named step (float 0.1))") != std::string::npos);
}

TEST_CASE("robustness: broken versions of the examples never crash or hang") {
    // Every prefix, and every single-character deletion, in both modes. Error
    // recovery must terminate and still produce a printable tree.
    std::size_t total_errors = 0;
    for (std::string_view example : {player_example, boss_example, statements_example}) {
        for (std::size_t n = 0; n <= example.size(); ++n) {
            std::string prefix(example.substr(0, n));
            test::Parsed a = test::parse(prefix, Root::Module);
            test::Parsed b = test::parse(prefix, Root::Script);
            total_errors += a.error_count + b.error_count;
        }
        for (std::size_t i = 0; i < example.size(); ++i) {
            std::string cut = std::string(example.substr(0, i)) + std::string(example.substr(i + 1));
            test::Parsed p = test::parse(cut, Root::Script);
            total_errors += p.error_count;
            // Deletions that are still valid (e.g. `hp` -> `h`) must round-trip too.
            if (p.error_count == 0) check_round_trip(cut);
        }
    }
    CHECK(total_errors > 0);
}

TEST_CASE("canonical printing follows the style guide (LANGUAGE.md 6)") {
    test::Parsed p = test::parse("class A extends B:C{\n"
                                 "  var x=1\n"
                                 "  fn f(a:int)->int{ if a>0{return a}else{return -a} }\n"
                                 "  fn g(){}\n"
                                 "}\n"
                                 "enum E{X,Y(v:int)}\n",
                                 Root::Module);
    REQUIRE(p.errors.empty());
    CHECK(p.printed == "class A extends B: C {\n"
                       "    var x = 1\n"
                       "\n"
                       "    fn f(a: int) -> int {\n"
                       "        if a > 0 {\n"
                       "            return a\n"
                       "        } else {\n"
                       "            return -a\n"
                       "        }\n"
                       "    }\n"
                       "\n"
                       "    fn g() {}\n"
                       "}\n"
                       "\n"
                       "enum E {\n"
                       "    X,\n"
                       "    Y(v: int),\n"
                       "}\n");
}

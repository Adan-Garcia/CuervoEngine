#include <doctest.h>

#include "pluma/format.hpp"
#include "pluma/parser.hpp"

using namespace pluma;

namespace {

std::string fmt(std::string_view src, bool* ok_out = nullptr) {
    SourceFile sf("test.plm", std::string(src));
    FormatResult res = format_source(sf);
    if (ok_out != nullptr) *ok_out = res.ok;
    REQUIRE(res.ok);
    // Formatting must always be idempotent: format(format(x)) == format(x).
    SourceFile sf2("test.plm", res.formatted);
    FormatResult res2 = format_source(sf2);
    REQUIRE(res2.ok);
    CHECK(res2.formatted == res.formatted);
    return res.formatted;
}

std::vector<Diagnostic> style_diags(std::string_view src) {
    SourceFile sf("test.plm", std::string(src));
    DiagnosticSink sink(sf);
    Arena arena;
    Module mod = parse_module(sf, arena, sink);
    check_style(sf, mod, sink);
    return sink.all();
}

} // namespace

TEST_CASE("formatter: indentation, braces, and operator spacing") {
    std::string input =
        "fn compute(a:int,b:int)->int\n"
        "{\n"
        "  var x=a+b*2\n"
        "  if x>10\n"
        "  {\n"
        "    return x\n"
        "  }\n"
        "  else\n"
        "  {\n"
        "    return 0\n"
        "  }\n"
        "}\n";

    std::string expected =
        "fn compute(a: int, b: int) -> int {\n"
        "    var x = a + b * 2\n"
        "    if x > 10 {\n"
        "        return x\n"
        "    } else {\n"
        "        return 0\n"
        "    }\n"
        "}\n";

    CHECK(fmt(input) == expected);
}

TEST_CASE("formatter: preserves line and nested block comments and intentional blank lines") {
    std::string input =
        "// Header comment\n"
        "/* Outer /* nested */ block comment */\n"
        "\n"
        "\n"
        "fn greet(name: str) {\n"
        "    // Inside function\n"
        "    var msg = \"hi \" + name // trailing comment\n"
        "\n"
        "    echo(msg)\n"
        "}\n";

    std::string expected =
        "// Header comment\n"
        "/* Outer /* nested */ block comment */\n"
        "\n"
        "fn greet(name: str) {\n"
        "    // Inside function\n"
        "    var msg = \"hi \" + name // trailing comment\n"
        "\n"
        "    echo(msg)\n"
        "}\n";

    CHECK(fmt(input) == expected);
}

TEST_CASE("formatter: preserves intentional same-line semicolon statements") {
    std::string input =
        "fn tick() {\n"
        "    var x = 1; var y = 2\n"
        "    x += 1\n"
        "}\n";

    CHECK(fmt(input) == input);
}

TEST_CASE("formatter: multiline lists, arguments, struct literals, maps, and method chains") {
    std::string input =
        "struct Config {\n"
        "    var pos: vec2 = vec2(0.0, 0.0)\n"
        "    var speed: float = 1.0\n"
        "}\n"
        "\n"
        "fn build() {\n"
        "    var items = [\n"
        "        1,\n"
        "        2,\n"
        "        3\n"
        "    ]\n"
        "    var cfg = Config{\n"
        "        pos = vec2(1.0, 2.0),\n"
        "        speed = 5.0\n"
        "    }\n"
        "    var v = vec2(3.0, 4.0)\n"
        "        .normalized()\n"
        "        .rotated(0.5)\n"
        "}\n";

    std::string expected =
        "struct Config {\n"
        "    var pos: vec2 = vec2(0.0, 0.0)\n"
        "    var speed: float = 1.0\n"
        "}\n"
        "\n"
        "fn build() {\n"
        "    var items = [\n"
        "        1,\n"
        "        2,\n"
        "        3,\n"
        "    ]\n"
        "    var cfg = Config {\n"
        "        pos = vec2(1.0, 2.0),\n"
        "        speed = 5.0,\n"
        "    }\n"
        "    var v = vec2(3.0, 4.0)\n"
        "        .normalized()\n"
        "        .rotated(0.5)\n"
        "}\n";

    CHECK(fmt(input) == expected);
}

TEST_CASE("formatter: normalizes recoverable fix-it errors and rejects broken syntax") {
    // `a?b:c` without spaces around `?` and missing enum commas are recoverable fix-its.
    std::string input =
        "enum Phase { Intro Wave1(count: int) Boss }\n"
        "\n"
        "fn pick(flag: bool, a: int, b: int) -> int {\n"
        "    return flag?a:b\n"
        "}\n";

    std::string expected =
        "enum Phase { Intro, Wave1(count: int), Boss }\n"
        "\n"
        "fn pick(flag: bool, a: int, b: int) -> int {\n"
        "    return flag ? a : b\n"
        "}\n";

    CHECK(fmt(input) == expected);

    // Unrecoverable syntax error must return ok == false and leave caller in control.
    SourceFile bad("bad.plm", "fn broken( { return }\n");
    FormatResult bad_res = format_source(bad);
    CHECK_FALSE(bad_res.ok);
    CHECK_FALSE(bad_res.diagnostics.empty());
}

TEST_CASE("formatter: classes, properties, attributes, match, coroutines, and lambdas") {
    std::string input =
        "class Player extends Node2D: Damageable {\n"
        "    signal died\n"
        "\n"
        "    @export(range = 50.0..400.0)\n"
        "    var speed = 200.0\n"
        "\n"
        "    var lives = 3 {\n"
        "        set(v) {\n"
        "            lives = v\n"
        "            if lives <= 0 {\n"
        "                emit died\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "\n"
        "    fn blink() {\n"
        "        for i in 0..10 {\n"
        "            await wait(0.1)\n"
        "        }\n"
        "    }\n"
        "}\n";

    CHECK(fmt(input) == input);
}

TEST_CASE("style checker: PascalCase, snake_case, SCREAMING_SNAKE, and #snake_case") {
    // Compliant code produces zero warnings.
    auto clean = style_diags(
        "const MAX_LIVES = 3\n"
        "enum BossPhase { Intro, SecondWave }\n"
        "class EnemyShip {\n"
        "    signal wave_cleared\n"
        "    var move_speed = 10.0\n"
        "    var _internal_timer = 0.0\n"
        "    fn take_damage(hit_points: int) {\n"
        "        var tag = #player_shot\n"
        "    }\n"
        "}\n");
    CHECK(clean.empty());

    // Non-compliant identifiers produce warnings with fix-it replacements.
    auto bad = style_diags(
        "const maxLives = 3\n"
        "class enemy_ship {\n"
        "    var MoveSpeed = 10.0\n"
        "    fn TakeDamage(HitPoints: int) {\n"
        "        var tag = #PlayerShot\n"
        "    }\n"
        "}\n");
    REQUIRE(bad.size() == 6);
    for (const auto& d : bad) {
        CHECK(d.severity == Severity::Warning);
        REQUIRE(d.fix.has_value());
    }
    CHECK(bad[0].fix->replacement == "MAX_LIVES");
    CHECK(bad[1].fix->replacement == "EnemyShip");
    CHECK(bad[2].fix->replacement == "move_speed");
    CHECK(bad[3].fix->replacement == "take_damage");
    CHECK(bad[4].fix->replacement == "hit_points");
    CHECK(bad[5].fix->replacement == "#player_shot");
}

TEST_CASE("formatter: leading comments stay attached; consecutive consts stay together") {
    // Already formatted: must come back unchanged (doc comments feed hover and
    // docs/API.md, so a blank line between them and their item detaches them).
    const char* src = "// Speed of the player.\n"
                      "const SPEED = 120.0\n"
                      "const LIVES = 3\n"
                      "\n"
                      "// The player ship.\n"
                      "class Player extends Node {\n"
                      "    // Pixels per second.\n"
                      "    var speed = 1.0\n"
                      "\n"
                      "    // Moves the ship.\n"
                      "    fn update(dt: float) {\n"
                      "        var a = 1\n"
                      "        // Why the next line exists.\n"
                      "        position.x += speed * dt\n"
                      "\n"
                      "        // After a blank line.\n"
                      "        a += 1\n"
                      "    }\n"
                      "}\n";
    CHECK(fmt(src) == src);
}

TEST_CASE("formatter: imports and package-qualified types (LANGUAGE.md 9.26)") {
    std::string input = "// header\nimport   tween as tw\nimport util.{ A ,b as  c}\nvar p : tw.Point = tw.Point{x=1}\n"
                        "fn f(m: tw.Mode) -> int { return 1 }\n";
    std::string expected = "// header\nimport tween as tw\nimport util.{A, b as c}\n\nvar p: tw.Point = tw.Point { x = 1 }\n\n"
                           "fn f(m: tw.Mode) -> int {\n    return 1\n}\n";
    CHECK(fmt(input) == expected);
    CHECK(fmt(expected) == expected);
}

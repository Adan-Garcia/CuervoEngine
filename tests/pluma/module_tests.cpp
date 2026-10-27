// Packages and `import` (LANGUAGE.md 9.26, milestone 10.5b): a package's
// names live in their own namespace, only `pub` ones are visible outside it,
// and imports are per file.

#include <doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "pluma/aot.hpp"
#include "pluma/session.hpp"

namespace {

using Part = pluma::SourceFile::Part;

Part pkg(std::string package, std::string path, std::string text) {
    return Part{std::move(path), std::move(text), std::move(package)};
}

std::string show(pluma::Session& s, std::string_view global) { return s.vm().format(s.global(global), false); }

bool has_error(const pluma::SessionResult& r, std::string_view text) {
    for (const auto& d : r.diagnostics) {
        if (d.message.find(text) != std::string::npos) return true;
    }
    return false;
}

const char* tween_src = R"(
pub struct Point {
    var x: int = 0
    var y: int = 0
}
pub enum Mode { Linear, Quad }
pub class Tween {
    var steps: int
    fn init(steps: int) { self.steps = steps }
    fn value(t: int) -> int { return helper(t) * steps }
    static fn twice(n: int) -> int { return n * 2 }
}
pub trait Easing { fn ease(t: int) -> int }
pub fn ease(m: Mode, t: int) -> int {
    match m {
        Mode.Linear => return t
        Mode.Quad => return t * t
    }
}
pub var created: int = 0
pub const MAX: int = 100
fn helper(t: int) -> int { return t + 1 }
class Secret {}
)";

} // namespace

TEST_CASE("packages: a package's names do not clash with the game's") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({
        pkg("tween", "packages/tween/scripts/tween.plm", tween_src),
        {"scripts/game.plm", R"(
import tween
// The game's own `helper`, `Point` and `Tween` live beside the package's.
fn helper(t: int) -> int { return t - 1 }
struct Point { var z: int = 9 }
class Tween {}
var a = tween.Tween(3).value(2)
var b = helper(10)
var c = Point {}.z
var d = tween.Point { x = 4 }.x
var e = tween.ease(tween.Mode.Quad, 5)
var f = tween.Tween.twice(21)
var g = tween.MAX
)"},
    });
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(show(s, "a") == "9");
    CHECK(show(s, "b") == "9");
    CHECK(show(s, "c") == "9");
    CHECK(show(s, "d") == "4");
    CHECK(show(s, "e") == "25");
    CHECK(show(s, "f") == "42");
    CHECK(show(s, "g") == "100");
    // Package globals are qualified in the VM too.
    CHECK(show(s, "tween.created") == "0");
}

TEST_CASE("packages: types, patterns, traits and assignment through an import") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({
        pkg("tween", "packages/tween/scripts/tween.plm", tween_src),
        {"scripts/game.plm", R"(
import tween as tw
class Sq: tw.Easing { fn ease(t: int) -> int { return t * t } }
class Slow extends tw.Tween {}
fn name_of(m: tw.Mode) -> str {
    match m {
        tw.Mode.Linear => return "linear"
        tw.Mode.Quad => return "quad"
    }
}
fn kind(x: tw.Tween) -> str {
    if x is Slow { return "slow" }
    return "tween"
}
var p: tw.Point = tw.Point { y = 2 }
var names = name_of(tw.Mode.Quad)
var k = kind(Slow(1))
var sq = Sq().ease(7)
fn bump() { tw.created += 1 }
var bumped = 0
fn main() {
    bump()
    bumped = tw.created
}
)"},
    });
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(show(s, "names") == "quad");
    CHECK(show(s, "k") == "slow");
    CHECK(show(s, "sq") == "49");
    CHECK(show(s, "p") == "tween.Point { x = 0, y = 2 }");
}

TEST_CASE("packages: imported items and their aliases") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({
        pkg("tween", "packages/tween/scripts/tween.plm", tween_src),
        {"scripts/game.plm", R"(
import tween.{Tween, ease as tween_ease, Mode}
var a = Tween(2).value(1)
var b = tween_ease(Mode.Linear, 6)
)"},
    });
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(show(s, "a") == "4");
    CHECK(show(s, "b") == "6");
}

TEST_CASE("packages: private names, unknown packages and missing imports are errors") {
    auto check = [](std::string game) {
        pluma::Session s;
        return s.load_files({pkg("tween", "packages/tween/scripts/tween.plm", tween_src), {"scripts/game.plm", game}});
    };
    pluma::SessionResult r = check("import tween\nvar x = tween.helper(1)");
    CHECK_FALSE(r.compiled);
    CHECK(has_error(r, "'helper' is not public in package 'tween'"));
    r = check("import tween\nvar x: tween.Secret? = null");
    CHECK(has_error(r, "'Secret' is not public in package 'tween'"));
    r = check("import tween.{Secret}");
    CHECK(has_error(r, "'Secret' is not public in package 'tween'"));
    r = check("import tween\nvar x = tween.nope");
    CHECK(has_error(r, "package 'tween' has no 'nope'"));
    r = check("import physics2");
    CHECK(has_error(r, "unknown package 'physics2'"));
    // Without an import, the package's names are not visible at all.
    r = check("var x = Tween(1)");
    CHECK(has_error(r, "unknown function 'Tween'"));
    r = check("var t = tween.MAX");
    CHECK(has_error(r, "unknown name 'tween'"));
    // An import's name may not hide a declaration.
    r = check("import tween\nvar tween = 1");
    CHECK(has_error(r, "import 'tween' clashes"));
}

TEST_CASE("packages: imports are per file") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({
        pkg("tween", "packages/tween/scripts/tween.plm", tween_src),
        {"scripts/a.plm", "import tween\nvar a = tween.MAX"},
        {"scripts/b.plm", "var b = tween.MAX"},
    });
    CHECK_FALSE(r.compiled);
    REQUIRE(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].file == "scripts/b.plm");
}

TEST_CASE("packages: a package sees preludes and other packages, never the game") {
    pluma::Session s;
    // The prelude part stands in for the engine API.
    pluma::SessionResult r = s.load_files({
        Part{"prelude.plm", "class Node {}\nfn engine_tick() -> int { return 60 }", {}, true},
        pkg("util", "packages/util/scripts/util.plm", "pub fn clamp01(x: int) -> int { return x < 0 ? 0 : 1 }"),
        pkg("fx", "packages/fx/scripts/fx.plm", R"(
import util
pub class Spark extends Node {
    fn rate() -> int { return engine_tick() + util.clamp01(-3) }
}
)"),
        {"scripts/game.plm", "import fx\nvar r = fx.Spark().rate()\nfn game_only() {}"},
    });
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(show(s, "r") == "60");

    pluma::Session bad;
    r = bad.load_files({
        pkg("fx", "packages/fx/scripts/fx.plm", "pub fn f() { game_only() }"),
        {"scripts/game.plm", "fn game_only() {}"},
    });
    CHECK(has_error(r, "unknown function 'game_only'"));
}

TEST_CASE("packages: names are unique per package, and two packages may share one") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({
        pkg("a", "packages/a/scripts/a.plm", "pub fn id() -> int { return 1 }"),
        pkg("b", "packages/b/scripts/b.plm", "pub fn id() -> int { return 2 }"),
        {"scripts/game.plm", "import a\nimport b\nfn id() -> int { return 3 }\nvar s = a.id() * 100 + b.id() * 10 + id()"},
    });
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(show(s, "s") == "123");

    pluma::Session dup;
    r = dup.load_files({
        pkg("a", "packages/a/scripts/x.plm", "pub fn id() -> int { return 1 }"),
        pkg("a", "packages/a/scripts/y.plm", "fn id() -> int { return 2 }"),
    });
    CHECK(has_error(r, "duplicate name 'id' in package 'a'"));
}

TEST_CASE("packages: a package's `main` is not the entry point") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({
        pkg("a", "packages/a/scripts/a.plm", "pub var ran = 0\nfn main() { ran = 1 }"),
        {"scripts/game.plm", "import a\nvar seen = a.ran"},
    });
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(show(s, "seen") == "0");
}

TEST_CASE("packages: AOT modules keep qualified names") {
    pluma::Session vm_sess;
    pluma::SessionResult r = vm_sess.load_files({
        pkg("tween", "packages/tween/scripts/tween.plm", tween_src),
        {"scripts/game.plm", R"(
import tween
class Tween { fn value(t: int) -> int { return -t } }
var a = tween.Tween(3).value(2) + Tween().value(1)
var e = tween.ease(tween.Mode.Quad, 5)
)"},
    });
    REQUIRE_MESSAGE(r.ok, r.rendered);
    std::string err;
    auto mod = pluma::compile_aot_module(vm_sess.program(), vm_sess.vm(), err, 1);
    REQUIRE_MESSAGE(mod != nullptr, err);
    pluma::Session aot_sess({}, nullptr, false);
    r = aot_sess.load_aot(std::move(mod));
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(show(aot_sess, "a") == "8");
    CHECK(show(aot_sess, "e") == "25");
    CHECK(show(aot_sess, "tween.MAX") == show(vm_sess, "tween.MAX"));
}

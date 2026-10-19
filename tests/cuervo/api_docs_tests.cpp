// tools/cuervo_tools/api_docs: the API reference generator (`cuervo-tools
// docs`, docs/API.md) on a small synthetic prelude, and on the real one.

#include <doctest.h>

#include <string>

#include "cuervo/script/script_host.hpp"
#include "tools/cuervo_tools/api_docs.hpp"

namespace {

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* const toy_prelude = R"(// Toy API: one of everything.
// Second header line.

// The answer.
const ANSWER = 42

// Where shots come from.
enum Spread {
    Line(count: int, gap = 8.0),   // side by side
    // All at once.
    Burst,
}

// A thing in the world.
//   aligned  stays aligned
//   columns  too
class Thing extends Node {
    // Fired when it breaks.
    signal broke(by: Thing)
    @export var hp = 3     // hit points
    static var count = 0
    var _secret = 1
    var target: Thing?

    // ---- behavior

    // Hurts it by `amount`; List<Thing> stays prose.
    @native fn hurt(amount: int, color: Color = Color {}, mode = #soft) -> bool {}
    static fn make<T: Add + Default>(at: vec2 = vec2(1.0, 2.0)) -> Thing {
        return Thing()
    }
    fn _hidden() {}
}

struct Pair: Eq {
    var a = 0
}
)";

} // namespace

TEST_CASE("api docs: a synthetic prelude") {
    std::string error;
    auto md = cuervo::tools::generate_api_docs({{"cuervo/toy.plm", toy_prelude}}, error);
    REQUIRE_MESSAGE(md.has_value(), error);
    const std::string& d = *md;

    // Contents link the types; the file's header introduces its section.
    CHECK(has(d, "- [toy.plm](#toyplm): `ANSWER`, [`Spread`](#spread), [`Thing`](#thing), [`Pair`](#pair)\n"));
    CHECK(has(d, "## toy.plm\n\nToy API: one of everything.\nSecond header line.\n"));

    // Constants with their leading comment.
    CHECK(has(d, "- `const ANSWER = 42`\n\n  The answer.\n"));

    // Types: header line, doc, column-aligned comment lines as a code block.
    CHECK(has(d, "### Thing\n\n```pluma\nclass Thing extends Node\n```\n"));
    CHECK(has(d, "A thing in the world.\n\n```\naligned  stays aligned\ncolumns  too\n```\n"));
    CHECK(has(d, "```pluma\nstruct Pair: Eq\n```"));

    // Enum variants: parameters with defaults, trailing and leading comments.
    CHECK(has(d, "- `Line(count: int, gap = 8.0)`\n\n  side by side\n"));
    CHECK(has(d, "- `Burst`\n\n  All at once.\n"));

    // Members grouped signals, fields, methods; attributes kept except @native;
    // full signatures with types, defaults and generics.
    CHECK(has(d, "**Signals**\n\n- `signal broke(by: Thing)`\n\n  Fired when it breaks.\n"));
    CHECK(has(d, "- `@export var hp = 3`\n\n  hit points\n"));
    CHECK(has(d, "- `static var count = 0`\n"));
    CHECK(has(d, "- `var target: Thing?`\n"));
    CHECK(has(d, "- `fn hurt(amount: int, color: Color = Color {}, mode = #soft) -> bool`\n\n"
                 "  Hurts it by `amount`; List&lt;Thing> stays prose.\n"));
    CHECK(has(d, "- `static fn make<T: Add + Default>(at: vec2 = vec2(1.0, 2.0)) -> Thing`\n"));
    CHECK(d.find("**Signals**") < d.find("**Fields**"));
    CHECK(d.find("**Fields**") < d.find("**Methods**"));

    // Section dividers aren't docs; private members are left out.
    CHECK_FALSE(has(d, "behavior"));
    CHECK_FALSE(has(d, "_secret"));
    CHECK_FALSE(has(d, "_hidden"));
    CHECK(d.ends_with("\n"));
    CHECK_FALSE(d.ends_with("\n\n"));
}

TEST_CASE("api docs: a header that runs into the first declaration documents it") {
    std::string error;
    auto md = cuervo::tools::generate_api_docs({{"cuervo/a.plm", "// Plays sounds.\nclass Audio {\n}\n"}}, error);
    REQUIRE_MESSAGE(md.has_value(), error);
    CHECK(has(*md, "## a.plm\n\nSource: `cuervo/script/prelude/a.plm`\n"));
    CHECK(has(*md, "class Audio\n```\n\nPlays sounds.\n"));
}

TEST_CASE("api docs: parse errors are reported, and the engine prelude documents every file") {
    std::string error;
    CHECK_FALSE(cuervo::tools::generate_api_docs({{"cuervo/bad.plm", "class {\n"}}, error).has_value());
    CHECK(has(error, "bad.plm:1:"));

    auto md = cuervo::tools::generate_api_docs(cuervo::script::engine_preludes(), error);
    REQUIRE_MESSAGE(md.has_value(), error);
    CHECK(has(*md, "## core.plm\n"));
    CHECK(has(*md, "## scene.plm\n"));
    CHECK(has(*md, "- `static fn pressed(action: name) -> bool`\n"));
    CHECK(has(*md, "class Node2D extends Node\n"));
}

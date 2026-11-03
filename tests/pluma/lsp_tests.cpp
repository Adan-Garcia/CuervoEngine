#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "pluma/lsp.hpp"

using namespace pluma;
using namespace pluma::lsp;

namespace {

bool has_completion(const std::vector<CompletionItem>& items, std::string_view label) {
    return std::any_of(items.begin(), items.end(),
                       [&](const CompletionItem& item) { return item.label == label; });
}

} // namespace

TEST_CASE("lsp: diagnostics and quickfix code actions") {
    ServerOptions opts;
    opts.check_style = true;
    Server server(opts);

    std::string uri = "file:///workspace/main.plm";
    server.open_document(uri,
                         "enum Phase { Intro Wave }\n"
                         "fn badName(x: int) -> int {\n"
                         "    return x > 0?x:0\n"
                         "}\n");

    auto diags = server.diagnostics(uri);
    REQUIRE(diags.size() >= 3);

    // Quickfix actions should be available for the missing enum comma, ternary spacing, and style warning.
    auto actions = server.code_actions(uri, Range{{0, 0}, {4, 0}});
    CHECK(actions.size() >= 3);
    bool found_rename_fix = false;
    bool found_comma_fix = false;
    for (const auto& act : actions) {
        REQUIRE(!act.edits.empty());
        if (act.edits[0].new_text == "bad_name") found_rename_fix = true;
        if (act.edits[0].new_text == ",") found_comma_fix = true;
    }
    CHECK(found_rename_fix);
    CHECK(found_comma_fix);
}

TEST_CASE("lsp: cross-file definition, hover, references, and rename") {
    ServerOptions opts;
    opts.preludes.push_back(SourceFile::Part{
        "<prelude>",
        "// Base scene node.\n"
        "class Node {\n"
        "    var position: vec2 = vec2(0.0, 0.0)\n"
        "    @native fn move_by(delta: vec2)\n"
        "}\n",
    });
    Server server(opts);

    std::string enemy_uri = "file:///workspace/enemy.plm";
    std::string game_uri = "file:///workspace/game.plm";

    server.open_document(enemy_uri,
                         "// An enemy ship in the arena.\n"
                         "class Enemy extends Node {\n"
                         "    var hp: int = 10\n"
                         "    fn take_hit(dmg: int) {\n"
                         "        hp -= dmg\n"
                         "    }\n"
                         "}\n");

    server.open_document(game_uri,
                         "fn step_enemy(foe: Enemy) {\n"
                         "    foe.take_hit(3)\n"
                         "    foe.move_by(vec2(1.0, 0.0))\n"
                         "}\n");

    CHECK(server.diagnostics(enemy_uri).empty());
    CHECK(server.diagnostics(game_uri).empty());

    // Go to definition on `Enemy` in `game.plm` (line 0, col 20) -> `enemy.plm` line 1.
    auto def_enemy = server.definition(game_uri, Position{0, 20});
    REQUIRE(def_enemy.has_value());
    CHECK(def_enemy->uri == enemy_uri);
    CHECK(def_enemy->range.start.line == 1);

    // Go to definition on `take_hit` in `game.plm` (line 1, col 9) -> `enemy.plm` line 3.
    auto def_hit = server.definition(game_uri, Position{1, 9});
    REQUIRE(def_hit.has_value());
    CHECK(def_hit->uri == enemy_uri);
    CHECK(def_hit->range.start.line == 3);

    // Hover on `Enemy` includes its class signature and doc comment.
    auto hov = server.hover(game_uri, Position{0, 20});
    REQUIRE(hov.has_value());
    CHECK(hov->contents.find("class Enemy extends Node") != std::string::npos);
    CHECK(hov->contents.find("An enemy ship in the arena.") != std::string::npos);

    // Hover on `take_hit` includes `fn take_hit(dmg: int)`.
    auto hov_hit = server.hover(game_uri, Position{1, 9});
    REQUIRE(hov_hit.has_value());
    CHECK(hov_hit->contents.find("fn take_hit(dmg: int)") != std::string::npos);

    // Find references on `take_hit` finds both declaration in `enemy.plm` and call in `game.plm`.
    auto refs_all = server.references(game_uri, Position{1, 9}, true);
    CHECK(refs_all.size() == 2);
    auto refs_uses = server.references(game_uri, Position{1, 9}, false);
    REQUIRE(refs_uses.size() == 1);
    CHECK(refs_uses[0].uri == game_uri);

    // Rename `take_hit` -> `apply_damage` updates both files.
    auto prep = server.prepare_rename(game_uri, Position{1, 9});
    REQUIRE(prep.has_value());
    auto edit = server.rename(game_uri, Position{1, 9}, "apply_damage");
    REQUIRE(edit.has_value());
    CHECK(edit->changes.count(enemy_uri) == 1);
    CHECK(edit->changes.count(game_uri) == 1);

    // Prelude symbols (`Node`, `move_by`) cannot be renamed by user workspace edits.
    CHECK_FALSE(server.prepare_rename(game_uri, Position{2, 9}).has_value());
}

TEST_CASE("lsp: dot completion, #name completion, @attribute completion, and signature help") {
    ServerOptions opts;
    opts.preludes.push_back(SourceFile::Part{
        "<prelude>",
        "class Node {\n"
        "    var position: vec2 = vec2(0.0, 0.0)\n"
        "    @native fn is_held(action: name) -> bool\n"
        "}\n"
        "fn register_default_tags() {\n"
        "    var a = #fire\n"
        "    var b = #bomb\n"
        "}\n",
    });
    Server server(opts);

    std::string uri = "file:///workspace/player.plm";
    server.open_document(uri,
                         "enum State { Idle, Dash(speed: float) }\n"
                         "class Player extends Node {\n"
                         "    @exp\n"
                         "    var hp: int = 5\n"
                         "    fn heal(amount: int, bonus: int = 0) {\n"
                         "        var tag = #player_shot\n"
                         "        var p = position.\n"
                         "        self.\n"
                         "        State.\n"
                         "        var k = #\n"
                         "        heal(1, )\n"
                         "    }\n"
                         "}\n");

    // 1. `@exp` at line 2, col 8 -> completes `export`.
    auto attr_items = server.complete(uri, Position{2, 8});
    CHECK(has_completion(attr_items, "export"));

    // 2. `position.` at line 6, col 25 -> vec2 fields & methods (`x`, `y`, `length`, `normalized`, `rotated`).
    auto vec_items = server.complete(uri, Position{6, 25});
    CHECK(has_completion(vec_items, "x"));
    CHECK(has_completion(vec_items, "y"));
    CHECK(has_completion(vec_items, "normalized"));
    CHECK(has_completion(vec_items, "rotated"));

    // 3. `self.` at line 7, col 13 -> Player and inherited Node members (`hp`, `heal`, `position`, `is_held`).
    auto self_items = server.complete(uri, Position{7, 13});
    CHECK(has_completion(self_items, "hp"));
    CHECK(has_completion(self_items, "heal"));
    CHECK(has_completion(self_items, "position"));
    CHECK(has_completion(self_items, "is_held"));

    // 4. `State.` at line 8, col 14 -> enum variants (`Idle`, `Dash`).
    auto enum_items = server.complete(uri, Position{8, 14});
    CHECK(has_completion(enum_items, "Idle"));
    CHECK(has_completion(enum_items, "Dash"));

    // 5. `#` at line 9, col 17 -> interned names from both prelude and workspace (`fire`, `bomb`, `player_shot`).
    auto name_items = server.complete(uri, Position{9, 17});
    CHECK(has_completion(name_items, "fire"));
    CHECK(has_completion(name_items, "bomb"));
    CHECK(has_completion(name_items, "player_shot"));

    // 6. Signature help inside `heal(1, )` at line 10, col 16 -> active_parameter == 1.
    auto sig = server.signature_help(uri, Position{10, 16});
    REQUIRE(sig.has_value());
    REQUIRE(!sig->signatures.empty());
    CHECK(sig->active_parameter == 1);
    REQUIRE(sig->signatures[0].parameters.size() == 2);
    CHECK(sig->signatures[0].parameters[0].label == "amount: int");
    CHECK(sig->signatures[0].parameters[1].label == "bonus: int = 0");
}

TEST_CASE("lsp: document symbols, workspace symbols, formatting, and JSON-RPC stdio") {
    Server server;
    std::ostringstream out;

    auto send = [&](std::string_view json) {
        std::ostringstream frame;
        frame << "Content-Length: " << json.size() << "\r\n\r\n" << json;
        return frame.str();
    };

    std::string input;
    input += send(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file:///workspace"}})");
    input += send(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    input += send(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///workspace/test.plm","version":1,"text":"fn add(a:int,b:int)->int{\nreturn a+b\n}\n"}}})");
    input += send(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///workspace/test.plm"}}})");
    input += send(R"({"jsonrpc":"2.0","id":3,"method":"textDocument/formatting","params":{"textDocument":{"uri":"file:///workspace/test.plm"}}})");
    input += send(R"({"jsonrpc":"2.0","id":4,"method":"shutdown","params":null})");
    input += send(R"({"jsonrpc":"2.0","method":"exit","params":null})");

    std::istringstream in(input);
    int code = server.run_stdio(in, out);
    CHECK(code == 0);

    std::string responses = out.str();
    CHECK(responses.find("\"pluma-lsp\"") != std::string::npos);
    CHECK(responses.find("\"textDocument/publishDiagnostics\"") != std::string::npos);
    CHECK(responses.find("\"name\":\"add\"") != std::string::npos);
    CHECK(responses.find("fn add(a: int, b: int) -> int {\\n    return a + b\\n}\\n") != std::string::npos);
}

TEST_CASE("lsp: positions are UTF-16 code units, not bytes") {
    Server server;
    const std::string uri = "file:///workspace/utf.plm";
    // `é` is 2 bytes / 1 unit and the emoji 4 bytes / 2 units: `hp` starts at
    // byte column 26 but UTF-16 column 23.
    server.open_document(uri,
                         "fn main() {\n"
                         "    var s = \"\xC3\xA9\xF0\x9F\x98\x80\"; var hp = 3\n"
                         "    print(hp)\n"
                         "}\n");
    auto def = server.definition(uri, Position{2, 10});
    REQUIRE(def.has_value());
    CHECK(def->range.start == Position{1, 23});
    CHECK(def->range.end == Position{1, 25});
    // And back: a UTF-16 position after the emoji resolves to `hp`.
    auto refs = server.references(uri, Position{1, 24}, true);
    CHECK(refs.size() == 2);
}

TEST_CASE("lsp: a malformed Content-Length is skipped, not fatal") {
    Server server;
    std::ostringstream out;
    auto send = [](std::string_view json) {
        return std::format("Content-Length: {}\r\n\r\n{}", json.size(), json);
    };
    std::string input = "Content-Length: not-a-number\r\n\r\n";
    input += "Content-Length: 99999999999999999999999\r\n\r\n";
    input += send(R"({"jsonrpc":"2.0","id":1,"method":"shutdown","params":null})");
    input += send(R"({"jsonrpc":"2.0","method":"exit","params":null})");
    std::istringstream in(input);
    CHECK(server.run_stdio(in, out) == 0);
    CHECK(out.str().find("\"id\":1") != std::string::npos);
}

TEST_CASE("lsp: completion of the 9.2a collections, str methods and Result") {
    Server server(ServerOptions{});
    std::string uri = "file:///workspace/lib.plm";
    server.open_document(uri,
                         "fn f(m: Map<str, int>, s: str, r: Result<int, str>, d: Deque<int>) {\n"
                         "    m.\n"
                         "    s.\n"
                         "    r.\n"
                         "    d.\n"
                         "}\n");
    auto map_items = server.complete(uri, Position{1, 6});
    CHECK(has_completion(map_items, "has"));
    CHECK(has_completion(map_items, "get_or"));
    CHECK(has_completion(map_items, "keys"));
    auto str_items = server.complete(uri, Position{2, 6});
    CHECK(has_completion(str_items, "split"));
    CHECK(has_completion(str_items, "pad_left"));
    auto res_items = server.complete(uri, Position{3, 6});
    CHECK(has_completion(res_items, "unwrap_or"));
    CHECK(has_completion(res_items, "is_ok"));
    auto dq_items = server.complete(uri, Position{4, 6});
    CHECK(has_completion(dq_items, "push_front"));
}

TEST_CASE("lsp: packages are checked in their own namespace and definitions follow imports") {
    ServerOptions opts;
    opts.packages = [](const std::string&) {
        return std::vector<ServerOptions::PackageDir>{{"tween", "/libs/tween"}};
    };
    Server server(opts);
    const std::string lib_uri = "file:///libs/tween/scripts/tween.plm";
    const std::string game_uri = "file:///workspace/game.plm";
    server.open_document(lib_uri, "pub class Tween {\n    fn value() -> int { return 1 }\n}\nfn helper() {}\n");
    // The game's own `Tween` and `helper` don't clash with the package's.
    server.open_document(game_uri, "import tween\nclass Tween {}\nfn helper() {}\nvar v = tween.Tween().value()\n");
    CHECK(server.diagnostics(lib_uri).empty());
    CHECK(server.diagnostics(game_uri).empty());
    auto def = server.definition(game_uri, Position{3, 15}); // the `Tween` in `tween.Tween()`
    REQUIRE(def.has_value());
    CHECK(def->uri == lib_uri);
    CHECK(def->range.start.line == 0);
}

// ---- DocumentSymbol ranges (the editor's outline follows the caret) ----

namespace {

// The text a Range covers (LSP positions: 0-based line, UTF-16 column; the test
// sources are ASCII before the checked ranges).
std::string slice_of(std::string_view text, Range r) {
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') starts.push_back(i + 1);
    auto off = [&](Position p) { return starts[p.line] + p.character; };
    return std::string(text.substr(off(r.start), off(r.end) - off(r.start)));
}

void check_symbol_ranges(std::string_view text, const std::vector<DocumentSymbol>& syms,
                         const Range* parent, std::vector<Range>& flat) {
    Position prev_end{};
    bool first = true;
    for (const DocumentSymbol& s : syms) {
        INFO(s.name);
        // The selection range is the name; it lies inside the full range.
        CHECK(slice_of(text, s.selection_range) == s.name);
        CHECK(s.range.contains(s.selection_range.start));
        CHECK(s.range.contains(s.selection_range.end));
        if (parent) {
            CHECK(parent->contains(s.range.start));
            CHECK(parent->contains(s.range.end));
        }
        // Siblings are in source order and do not overlap.
        if (!first) CHECK_FALSE(s.range.start < prev_end);
        prev_end = s.range.end;
        first = false;
        flat.push_back(s.range);
        check_symbol_ranges(text, s.children, &s.range, flat);
    }
}

} // namespace

TEST_CASE("lsp: document symbol ranges are correct and stable") {
    Server server;
    const std::string uri = "file:///workspace/outline.plm";
    const std::string text =
        "const LIMIT = 10\n"
        "var counter = 0\n"
        "\n"
        "/// Doc comment.\n"
        "pub fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "\n"
        "enum Phase { Intro, Wave }\n"
        "\n"
        "struct Point {\n"
        "    var x: float\n"
        "    var y: float\n"
        "    fn len() -> float { return x }\n"
        "}\n"
        "\n"
        "class Enemy {\n"
        "    var hp: int = 3\n"
        "    signal died\n"
        "    fn init() { }\n"
        "    fn hit() { hp -= 1 }\n"
        "}\n"
        "\n"
        "trait Drawable {\n"
        "    fn draw()\n"
        "}\n";
    server.open_document(uri, text);
    auto syms = server.document_symbols(uri);
    REQUIRE(syms.size() == 7);
    std::vector<Range> flat;
    check_symbol_ranges(text, syms, nullptr, flat);

    auto find = [&](std::string_view n) -> const DocumentSymbol& {
        for (const auto& s : syms) if (s.name == n) return s;
        FAIL("missing symbol");
        return syms[0];
    };
    // A top-level function's full range runs from its first token to the closing brace.
    const DocumentSymbol& add = find("add");
    CHECK(add.range.end.line == 6);
    CHECK(add.selection_range.start.line == 4);
    CHECK(add.kind == SymbolKind::Function);
    const DocumentSymbol& enemy = find("Enemy");
    CHECK(enemy.range.start.line == 16);
    CHECK(enemy.range.end.line == 21);
    REQUIRE(enemy.children.size() == 4);
    CHECK(enemy.children[1].kind == SymbolKind::Event);
    CHECK(enemy.children[2].kind == SymbolKind::Constructor);
    CHECK(enemy.children[3].range.start.line == 20);
    CHECK(find("Phase").children.size() == 2);
    CHECK(find("Point").children.size() == 3);

    // The caret lookup the outline does: the innermost symbol containing a position.
    Position caret{20, 8};
    CHECK(enemy.range.contains(caret));
    CHECK(enemy.children[3].range.contains(caret));
    CHECK_FALSE(enemy.children[2].range.contains(caret));

    // Stable: the same document gives the same outline; an edit above shifts it by lines only.
    auto again = server.document_symbols(uri);
    REQUIRE(again.size() == syms.size());
    for (std::size_t i = 0; i < syms.size(); ++i) CHECK(again[i].range == syms[i].range);
    server.change_document(uri, "var added = 1\n" + text, 2);
    auto shifted = server.document_symbols(uri);
    REQUIRE(shifted.size() == syms.size() + 1);
    for (std::size_t i = 0; i < syms.size(); ++i) {
        CHECK(shifted[i + 1].range.start.line == syms[i].range.start.line + 1);
        CHECK(shifted[i + 1].range.end.line == syms[i].range.end.line + 1);
    }
}

TEST_CASE("lsp: document symbols survive broken code and multibyte text") {
    Server server;
    const std::string uri = "file:///workspace/broken.plm";
    server.open_document(uri, "// héllo ✓\nfn ok() { }\nfn broken( {\nfn after() { }\n");
    auto syms = server.document_symbols(uri); // must not crash; recovered symbols keep their lines
    for (const auto& s : syms) {
        CHECK_FALSE(s.selection_range.start < s.range.start);
    }
    bool ok_found = false;
    for (const auto& s : syms) {
        if (s.name == "ok") {
            ok_found = true;
            CHECK(s.selection_range.start.line == 1);
        }
    }
    CHECK(ok_found);
    CHECK(server.document_symbols("file:///workspace/none.plm").empty());
}

// ---- Warning diagnostics with codes ----

namespace {

std::vector<DiagnosticItem> warnings_for(Server& s, const std::string& uri) {
    std::vector<DiagnosticItem> out;
    for (auto& d : s.diagnostics(uri))
        if (d.severity == Severity::Warning) out.push_back(std::move(d));
    return out;
}

} // namespace

TEST_CASE("lsp: lint warnings carry codes") {
    Server server;
    const std::string uri = "file:///workspace/lint.plm";
    server.open_document(uri,
                         "fn f(flag: bool) -> int {\n"
                         "    var unused = 1\n"
                         "    var _ignored = 2\n"
                         "    var used = 3\n"
                         "    if flag {\n"
                         "        return used\n"
                         "        print(\"dead\")\n"
                         "    }\n"
                         "    for i in 0..3 {\n"
                         "        break\n"
                         "        print(i)\n"
                         "    }\n"
                         "    return 0\n"
                         "}\n");
    auto w = warnings_for(server, uri);
    REQUIRE(w.size() == 3);
    auto has = [&](std::string_view code, std::uint32_t line) {
        for (const auto& d : w) if (d.code == code && d.range.start.line == line) return true;
        return false;
    };
    CHECK(has("unused-variable", 1));
    CHECK(has("unreachable-code", 6));
    CHECK(has("unreachable-code", 10));
    for (const auto& d : w) CHECK_FALSE(d.code.empty());

    // The code reaches the protocol.
    auto msgs = server.handle_message(
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///workspace/lint.plm","version":3},"contentChanges":[{"text":"fn g() { var x = 1 }\n"}]}})");
    bool saw_code = false;
    for (const auto& m : msgs) saw_code = saw_code || m.find("\"code\":\"unused-variable\"") != std::string::npos;
    CHECK(saw_code);

    // Turning lints off removes them; errors are untouched.
    ServerOptions off;
    off.lint_warnings = false;
    Server quiet(off);
    quiet.open_document(uri, "fn g() { var x = 1 }\n");
    CHECK(warnings_for(quiet, uri).empty());
}

TEST_CASE("lsp: style warnings are coded") {
    ServerOptions opts;
    opts.check_style = true;
    Server server(opts);
    const std::string uri = "file:///workspace/style.plm";
    server.open_document(uri, "fn badName() { }\n");
    bool coded = false;
    for (const auto& d : warnings_for(server, uri)) coded = coded || d.code == "style";
    CHECK(coded);
}

TEST_CASE("lsp: used variables, writes, interpolation and captures do not warn") {
    Server server;
    const std::string uri = "file:///workspace/used.plm";
    server.open_document(uri,
                         "fn f() -> int {\n"
                         "    var total = 0\n"
                         "    var label = \"n\"\n"
                         "    var step = 2\n"
                         "    var counter = 0\n"
                         "    for i in 0..3 { total += i * step }\n"
                         "    var bump = fn() { counter += 1 }\n"
                         "    bump()\n"
                         "    print(\"{label}: {total}\")\n"
                         "    for j in 0..2 {\n"
                         "        var x = 3.0\n"
                         "        var base = vec2(x, 2.0)\n"
                         "        var xs = List<float>()\n"
                         "        xs.push(base.x)\n"
                         "        print(xs.len())\n"
                         "    }\n"
                         "    for w in 0..2 {\n"
                         "        var cell = vec2i(w, w)\n"
                         "        if !ok(cell.x, cell.y) {\n"
                         "            panic(\"bad {cell.x}\")\n"
                         "        }\n"
                         "    }\n"
                         "    return total\n"
                         "}\n"
                         "fn ok(a: int, b: int) -> bool { return a < b }\n");
    CHECK(warnings_for(server, uri).empty());
}

TEST_CASE("lsp: unused imports") {
    Server server;
    const std::string uri = "file:///workspace/imports.plm";
    server.open_document(uri, "import tween\nimport audio as snd\nimport ui.{Button, Label}\n"
                              "fn f() { snd.play(Button) }\n");
    auto w = warnings_for(server, uri);
    int unused_imports = 0;
    bool tween = false, label = false, snd = false, button = false;
    for (const auto& d : w) {
        if (d.code != "unused-import") continue;
        ++unused_imports;
        tween = tween || d.message.find("'tween'") != std::string::npos;
        label = label || d.message.find("'Label'") != std::string::npos;
        snd = snd || d.message.find("'snd'") != std::string::npos;
        button = button || d.message.find("'Button'") != std::string::npos;
    }
    CHECK(unused_imports == 2);
    CHECK(tween);
    CHECK(label);
    CHECK_FALSE(snd);
    CHECK_FALSE(button);
}

TEST_CASE("lsp: the example projects produce no lint warnings") {
    int projects = 0;
    for (const auto& e : std::filesystem::directory_iterator(PLUMA_EXAMPLES_DIR)) {
        if (!e.is_directory()) continue;
        ServerOptions opts;
        // The host's prelude order (cuervo/CMakeLists.txt, all features on).
        for (const char* name :
             {"core", "save", "locale", "net", "http", "scene", "nodes", "animation", "tilemap", "lighting",
              "navigation/nodes", "navigation/region", "navigation/dijkstra", "navigation/agent", "collision",
              "particles", "ui/theme", "ui/control", "ui/containers", "ui/widgets", "ui/popups", "ui/ui", "audio",
              "physics/shapes", "physics/collision_polygon", "physics/bodies", "physics/area_raycast",
              "physics/joints", "physics/queries", "physics/tilemap_collision"}) {
            std::ifstream in(std::string(PLUMA_PRELUDE_DIR) + "/" + name + ".plm", std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            opts.preludes.push_back({std::string(name) + ".plm", ss.str()});
        }
        Server server(opts);
        server.set_root_path(e.path().string());
        for (const auto& [uri, items] : server.all_diagnostics()) {
            for (const auto& d : items) {
                if (d.severity != Severity::Warning) continue;
                INFO(uri << ":" << d.range.start.line + 1 << " " << d.message);
                CHECK(d.code.empty()); // fail on any lint warning
            }
        }
        ++projects;
    }
    CHECK(projects > 5);
}

TEST_CASE("lsp: semantic tokens") {
    Server server;
    const std::string uri = "file:///workspace/sem.plm";
    server.open_document(uri,
                         "const LIMIT = 3\n"
                         "enum Mode { Idle, Run }\n"
                         "struct P {\n"
                         "    var x: int\n"
                         "}\n"
                         "fn go(n: int) -> int {\n"
                         "    var p = P { x = n }\n"
                         "    var m = Mode.Run\n"
                         "    return p.x + LIMIT\n"
                         "}\n");
    auto toks = server.semantic_tokens(uri);
    const auto& types = semantic_token_types();
    auto type_at = [&](std::uint32_t line, std::uint32_t ch) -> std::string {
        for (const auto& t : toks)
            if (t.line == line && t.character == ch) return types[t.type];
        return "";
    };
    CHECK(type_at(0, 0) == "keyword");
    CHECK(type_at(0, 6) == "constant");
    CHECK(type_at(1, 5) == "type");
    CHECK(type_at(1, 12) == "enumMember");
    CHECK(type_at(3, 8) == "property");
    CHECK(type_at(5, 3) == "function");
    CHECK(type_at(5, 6) == "parameter");
    CHECK(type_at(6, 8) == "variable");
    CHECK(type_at(8, 13) == "property");
    // Uses of a folded `const` carry no name-resolved token (the checker inlines the value).
    // Sorted and one per position.
    for (std::size_t i = 1; i < toks.size(); ++i) {
        CHECK((toks[i - 1].line < toks[i].line ||
               (toks[i - 1].line == toks[i].line && toks[i - 1].character < toks[i].character)));
    }
    // The protocol answers with delta-encoded data and advertises the legend.
    auto init = server.handle_message(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    REQUIRE(!init.empty());
    CHECK(init[0].find("semanticTokensProvider") != std::string::npos);
    auto resp = server.handle_message(
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///workspace/sem.plm"}}})");
    REQUIRE(!resp.empty());
    CHECK(resp[0].find("\"data\":[0,0,5,6,0") != std::string::npos);
    CHECK(server.semantic_tokens("file:///workspace/none.plm").empty());
}

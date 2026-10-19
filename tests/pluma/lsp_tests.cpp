#include <doctest.h>

#include <algorithm>
#include <format>
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

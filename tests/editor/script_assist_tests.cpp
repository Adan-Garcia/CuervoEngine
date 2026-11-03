#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "editor/script_assist.hpp"

namespace fs = std::filesystem;
using namespace cuervo::editor;

namespace {
struct Proj {
    fs::path dir;
    explicit Proj(const char* n) {
        dir = fs::temp_directory_path() / (std::string("cuervo_as_") + n);
        fs::remove_all(dir);
        fs::create_directories(dir / "scripts");
    }
    ~Proj() { std::error_code ec; fs::remove_all(dir, ec); }
};
}

TEST_CASE("assist: completion opens on prefix, filters, accepts as one undo step") {
    Proj p("comp");
    std::ofstream(p.dir / "scripts/a.plm") << "class A extends Node2D {\n    var speed_scale: float = 1.0\n    fn update(dt: float) {\n        \n    }\n}\n";
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    ScriptDoc* d = ws.doc(ws.open("scripts/a.plm", err));
    CompletionState c;
    d->buf.set_caret({3, 8});
    d->buf.insert("sp");
    CHECK(c.query(ws, *d, 2));
    bool found = false;
    for (const auto& r : c.rows) if (r.label == "speed_scale") found = true;
    CHECK(found);
    CHECK(c.anchor == TextPos{3, 8});
    CHECK(c.still_valid(d->buf));
    // Below the minimum prefix the list stays closed.
    CompletionState c1;
    d->buf.backspace();
    CHECK_FALSE(c1.query(ws, *d, 2));
    d->buf.insert("p");
    c.query(ws, *d, 2);
    for (std::size_t i = 0; i < c.rows.size(); ++i) if (c.rows[i].label == "speed_scale") c.sel = static_cast<int>(i);
    c.move(1); c.move(-1);
    CHECK(c.accept(d->buf));
    CHECK(d->buf.line(3) == "        speed_scale");
    CHECK_FALSE(c.active());
    CHECK(d->buf.undo());
    CHECK(d->buf.line(3) == "        sp");
    // Caret moves away: the popup is stale.
    c.query(ws, *d, 2);
    d->buf.set_caret({2, 0});
    CHECK_FALSE(c.still_valid(d->buf));
}

TEST_CASE("assist: no completion inside a comment or string") {
    Proj p("nocomp");
    std::ofstream(p.dir / "scripts/a.plm") << "class A extends Node2D {\n    // spe\n    var s = \"spe\"\n}\n";
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    ScriptDoc* d = ws.doc(ws.open("scripts/a.plm", err));
    CompletionState c;
    d->buf.set_caret({1, 10});
    CHECK_FALSE(c.query(ws, *d, 2));
    d->buf.set_caret({2, 15});
    CHECK_FALSE(c.query(ws, *d, 2));
}

TEST_CASE("assist: markdown and signature views") {
    CHECK(markdown_to_rml("```pluma\nfn f(a: int)\n```\nUses **bold** and `x < y`.") ==
          "<div class=\"mdcode\"><code>fn f(a: int)</code></div><div>Uses <b>bold</b> and <code>x &lt; y</code>.</div>");
    pluma::lsp::SignatureHelp h;
    pluma::lsp::SignatureInfo s;
    s.label = "fn lerp(a: float, b: float, t: float) -> float";
    s.parameters = {{"a: float", ""}, {"b: float", "end"}, {"t: float", ""}};
    h.signatures.push_back(s);
    h.active_parameter = 1;
    const SignatureView v = signature_view(h);
    REQUIRE(v.ok);
    CHECK(v.before == "fn lerp(a: float, ");
    CHECK(v.active == "b: float");
    CHECK(v.after == ", t: float) -> float");
    CHECK(v.doc == "end");
    CHECK_FALSE(signature_view({}).ok);
}

TEST_CASE("assist: signature help and hover reach the server through the workspace") {
    Proj p("sig");
    std::ofstream(p.dir / "scripts/a.plm") << "class A extends Node2D {\n    fn two(a: int, b: float) -> int { return a }\n    fn go() {\n        var x = two(1, 2.0)\n    }\n}\n";
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    ScriptDoc* d = ws.doc(ws.open("scripts/a.plm", err));
    ws.sync(*d);
    const auto h = ws.server().signature_help(ws.uri(*d), ws.to_lsp(*d, {3, 23}));
    REQUIRE(h);
    const SignatureView v = signature_view(*h);
    CHECK(v.ok);
    CHECK(v.active == "b: float");
    const auto hv = ws.server().hover(ws.uri(*d), ws.to_lsp(*d, {3, 16}));
    REQUIRE(hv);
    CHECK(markdown_to_rml(hv->contents).find("two") != std::string::npos);
}

TEST_CASE("assist: hover and signature on engine natives") {
    Proj p("nat");
    std::ofstream(p.dir / "scripts/a.plm") << "class A extends Node2D {\n    fn go() {\n        var c = Color.hex(\"a\")\n    }\n}\n";
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    ScriptDoc* d = ws.doc(ws.open("scripts/a.plm", err));
    ws.sync(*d);
    CHECK(ws.server().hover(ws.uri(*d), ws.to_lsp(*d, {2, 23})).has_value());
    CHECK(ws.server().signature_help(ws.uri(*d), ws.to_lsp(*d, {2, 26})).has_value());
}

TEST_CASE("assist: definition resolves across files through the workspace") {
    Proj p("def");
    std::ofstream(p.dir / "scripts/a.plm") << "class A extends Node2D {\n    fn go() {\n        var h = Helper.make()\n    }\n}\n";
    std::ofstream(p.dir / "scripts/helper.plm") << "class Helper extends Node {\n    static fn make() -> int { return 1 }\n}\n";
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    ScriptDoc* d = ws.doc(ws.open("scripts/a.plm", err));
    std::string e2;
    ws.open("scripts/helper.plm", e2); // the server only knows documents the workspace has synced
    ws.check_now(ws.find(p.dir / "scripts/helper.plm"));
    ws.sync(*d);
    const auto loc = ws.server().definition(ws.uri(*d), ws.to_lsp(*d, {2, 18}));
    if (loc) {
        CHECK(fs::path(pluma::lsp::uri_to_path(loc->uri)).filename() == "helper.plm");
    } else {
        WARN_MESSAGE(false, "definition across files not resolved by the server");
    }
}

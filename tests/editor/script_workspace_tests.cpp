#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "editor/script_workspace.hpp"

namespace fs = std::filesystem;
using namespace cuervo::editor;

namespace {

struct TempProject {
    fs::path dir;
    explicit TempProject(const char* name) {
        dir = fs::temp_directory_path() / (std::string("cuervo_ws_") + name);
        fs::remove_all(dir);
        fs::create_directories(dir / "scripts");
    }
    ~TempProject() { std::error_code ec; fs::remove_all(dir, ec); }
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel, std::ios::binary) << text;
    }
    std::string read(const std::string& rel) const {
        std::ifstream in(dir / rel, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

const char* kGood = "class Ship extends Node2D {\n    var speed: float = 1.0\n\n    fn update(dt: float) {\n        speed += dt\n    }\n}\n";

} // namespace

TEST_CASE("workspace: open, dedupe, activate, close and MRU cycling") {
    TempProject p("tabs");
    p.write("scripts/a.plm", kGood);
    p.write("scripts/b.plm", kGood);
    p.write("scripts/c.plm", kGood);
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    const int a = ws.open(p.dir / "scripts/a.plm", err);
    const int b = ws.open("scripts/b.plm", err);
    const int c = ws.open(p.dir / "scripts" / ".." / "scripts/c.plm", err);
    REQUIRE((a > 0 && b > 0 && c > 0));
    CHECK(ws.tabs().size() == 3);
    CHECK(ws.open(p.dir / "scripts/a.plm", err) == a);
    CHECK(ws.tabs().size() == 3);
    CHECK(ws.active() == a);
    CHECK(ws.doc(a)->rel == "scripts/a.plm");
    // Ctrl+Tab goes to the previous tab (c), again goes further down the MRU list.
    CHECK(ws.cycle(false) == c);
    CHECK(ws.cycle(false) == b);
    ws.activate(b);
    ws.close(b);
    CHECK(ws.active() == a); // most recent remaining
    CHECK(ws.tabs().size() == 2);
    CHECK(ws.open("scripts/missing.plm", err) == -1);
    CHECK_FALSE(err.empty());
    CHECK(ws.project_scripts() == std::vector<std::string>{"scripts/a.plm", "scripts/b.plm", "scripts/c.plm"});
}

TEST_CASE("workspace: dirty marker, save keeps the line ending, undo returns to clean") {
    TempProject p("dirty");
    p.write("scripts/a.plm", std::string("class A extends Node2D {\r\n}\r\n"));
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    const int a = ws.open("scripts/a.plm", err);
    ScriptDoc* d = ws.doc(a);
    CHECK(d->buf.line_ending() == LineEnding::CrLf);
    CHECK(ws.dirty_count() == 0);
    d->buf.set_caret({1, 0});
    d->buf.insert("    var x: int = 1\n");
    CHECK(d->buf.dirty());
    CHECK(ws.dirty_count() == 1);
    d->buf.undo();
    CHECK(ws.dirty_count() == 0);
    d->buf.redo();
    REQUIRE(ws.save(a, err));
    CHECK(ws.dirty_count() == 0);
    CHECK(p.read("scripts/a.plm") == "class A extends Node2D {\r\n    var x: int = 1\r\n}\r\n");
    d->buf.insert("x");
    p.write("scripts/b.plm", kGood);
    const int b = ws.open("scripts/b.plm", err);
    ws.doc(b)->buf.insert("//");
    CHECK(ws.dirty_count() == 2);
    REQUIRE(ws.save_all(err));
    CHECK(ws.dirty_count() == 0);
}

TEST_CASE("workspace: diagnostics are debounced 150 ms and map to buffer positions") {
    TempProject p("diag");
    p.write("scripts/a.plm", kGood);
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    const int a = ws.open("scripts/a.plm", err);
    ScriptDoc* d = ws.doc(a);
    CHECK(ws.tick(1000)); // first check is immediate
    CHECK(ws.error_count(*d) == 0);
    // Break line 4: "speed += dt" -> "speed += ("
    d->buf.set_selections({{{4, 8}, {4, 19}}});
    d->buf.insert("speed += (");
    CHECK_FALSE(ws.tick(2000)); // debounce starts
    CHECK_FALSE(ws.tick(2149));
    CHECK(d->problems.empty());
    CHECK(ws.tick(2150));
    REQUIRE(ws.error_count(*d) >= 1);
    CHECK(d->problems[0].range.start.line >= 4);
    // Another edit restarts the wait.
    d->buf.insert(")");
    CHECK_FALSE(ws.tick(2200));
    CHECK_FALSE(ws.tick(2340));
    CHECK(ws.tick(2350));
}

TEST_CASE("workspace: LSP positions are UTF-16 and round trip") {
    TempProject p("utf16");
    p.write("scripts/a.plm", "class A extends Node2D {\n}\n");
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    ScriptDoc* d = ws.doc(ws.open("scripts/a.plm", err));
    d->buf.set_caret({0, 0});
    d->buf.insert("// \xF0\x9F\x98\x80 x\n");
    const TextPos after_emoji{0, 3 + 4 + 1};
    CHECK(ws.to_lsp(*d, after_emoji).character == 3 + 2 + 1);
    CHECK(ws.from_lsp(*d, pluma::lsp::Position{0, 6}) == after_emoji);
}

TEST_CASE("workspace: outline lists classes and members with positions") {
    TempProject p("outline");
    p.write("scripts/a.plm", kGood);
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    const int a = ws.open("scripts/a.plm", err);
    ws.check_now(a);
    const ScriptDoc* d = ws.doc(a);
    REQUIRE(d->outline.size() >= 3);
    CHECK(d->outline[0].name == "Ship");
    CHECK(d->outline[0].depth == 0);
    bool found = false;
    for (const OutlineItem& o : d->outline) if (o.name == "update") { found = true; CHECK(o.depth == 1); CHECK(o.pos.line == 3); }
    CHECK(found);
}

TEST_CASE("workspace: new script, external change reload, format on save") {
    TempProject p("misc");
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    const int n = ws.new_script("scripts/enemy.plm", "Enemy", "Node2D", err);
    REQUIRE(n > 0);
    CHECK(ws.doc(n)->buf.text().starts_with("class Enemy extends Node2D"));
    CHECK(ws.new_script("scripts/enemy.plm", "Enemy", "Node2D", err) == -1);
    // A clean tab picks up an on-disk change; a dirty one keeps the user's text.
    p.write("scripts/enemy.plm", "class Enemy extends Node2D {\n}\n");
    fs::last_write_time(p.dir / "scripts/enemy.plm", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    CHECK(ws.refresh_from_disk());
    CHECK(ws.doc(n)->buf.line_count() == 3);
    ws.doc(n)->buf.insert("// edit\n");
    p.write("scripts/enemy.plm", "changed");
    fs::last_write_time(p.dir / "scripts/enemy.plm", fs::file_time_type::clock::now() + std::chrono::seconds(9));
    CHECK_FALSE(ws.refresh_from_disk());
    ws.format_on_save = true;
    ws.doc(n)->buf.set_text("class   Enemy   extends Node2D {   fn   ready( ) {  }   }\n");
    REQUIRE(ws.save(n, err));
    CHECK(p.read("scripts/enemy.plm").find("class Enemy extends Node2D") != std::string::npos);
}

TEST_CASE("workspace: semantic tokens color functions and constants; lint problems carry codes") {
    TempProject p("sem");
    p.write("scripts/a.plm", "class A extends Node2D {\n    const LIMIT: int = 3\n    fn helper() -> int { return LIMIT }\n    fn update(dt: float) {\n        var unused = helper()\n    }\n}\n");
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    ScriptDoc* d = ws.doc(ws.open("scripts/a.plm", err));
    ws.check_now(d->id);
    // `LIMIT` in its declaration is a constant (folded uses carry no token); `helper` in `helper()` is a function.
    const std::string l1 = d->buf.line(1);
    const std::string l4 = d->buf.line(4);
    const auto kind_at = [&](int line, std::size_t col) {
        for (const SynSpan& s : d->syn.line(line).spans) if (col >= s.col && col < s.col + s.len) return s.kind;
        return Syn::Plain;
    };
    CHECK(kind_at(1, l1.find("LIMIT")) == Syn::Constant);
    CHECK(kind_at(4, l4.find("helper")) == Syn::Function);
    bool lint = false;
    for (const Problem& pr : d->problems) if (pr.code == "unused-variable") lint = true;
    CHECK(lint);
    // An edit drops the overlay of its line only; other lines keep theirs.
    d->buf.set_caret({4, 0});
    d->buf.insert(" ");
    d->syn.update(d->buf);
    CHECK(kind_at(1, l1.find("LIMIT")) == Syn::Constant);
    CHECK(kind_at(4, d->buf.line(4).find("helper")) == Syn::Plain);
    ws.check_now(d->id);
    CHECK(kind_at(4, d->buf.line(4).find("helper")) == Syn::Function);
}

TEST_CASE("workspace: format is one undo step, skips broken text; format on save defaults on") {
    ScriptWorkspace probe;
    CHECK(probe.format_on_save);
    TempProject p("fmt");
    p.write("scripts/a.plm", "class A extends Node2D {\nvar   x:int=1\n   fn f( ) {\n  x=2\n }\n}\n");
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    ScriptDoc* d = ws.doc(ws.open("scripts/a.plm", err));
    const std::string before = d->buf.text();
    CHECK(ws.format(d->id));
    CHECK(d->buf.text() != before);
    CHECK_FALSE(ws.format(d->id)); // already formatted
    CHECK(d->buf.undo());
    CHECK(d->buf.text() == before);
    d->buf.set_text("class A {\n  fn f( {\n");
    CHECK_FALSE(ws.format(d->id));
    // Saving formats when the setting is on.
    d->buf.set_text(before);
    CHECK(ws.save(d->id, err));
    CHECK(p.read("scripts/a.plm") != before);
    ws.format_on_save = false;
    d->buf.set_text(before);
    CHECK(ws.save(d->id, err));
    CHECK(p.read("scripts/a.plm") == before);
}

TEST_CASE("workspace: references span files and rename is one undo group per file") {
    TempProject p("rename");
    p.write("scripts/ship.plm", "class Ship extends Node2D {\n    var speed: float = 1.0\n}\n");
    p.write("scripts/user.plm", "class User extends Node2D {\n    var a: Ship\n    var b: Ship\n}\n");
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    const int ship = ws.open(p.dir / "scripts/ship.plm", err);
    REQUIRE(ship > 0);
    const auto refs = ws.references(ship, {0, 8}); // on "Ship"
    REQUIRE(refs.size() == 3);
    int in_user = 0;
    for (const RefHit& r : refs) {
        if (r.rel == "scripts/user.plm") { ++in_user; CHECK_FALSE(r.open); CHECK(r.preview.find("Ship") != std::string::npos); }
    }
    CHECK(in_user == 2);

    CHECK(ws.prepare_rename(ship, {0, 8}).has_value());
    CHECK_FALSE(ws.plan_rename(ship, {0, 8}, "1bad").has_value());
    const auto plan = ws.plan_rename(ship, {0, 8}, "Vessel");
    REQUIRE(plan.has_value());
    CHECK(plan->edits == 3);
    CHECK(plan->files == 2);

    // An unsaved edit in the open tab is respected; the other file is opened as a tab.
    ws.doc(ship)->buf.insert("// note\n");
    const auto plan2 = ws.plan_rename(ship, {1, 8}, "Vessel");
    REQUIRE(plan2.has_value());
    CHECK(plan2->unsaved == 1);
    const std::vector<int> docs = ws.apply_edit(plan2->edit, err);
    CHECK(docs.size() == 2);
    CHECK(ws.active() == ship);
    const int user = ws.find(p.dir / "scripts/user.plm");
    REQUIRE(user > 0);
    CHECK(ws.doc(user)->buf.text() == "class User extends Node2D {\n    var a: Vessel\n    var b: Vessel\n}\n");
    CHECK(ws.doc(ship)->buf.line(1).find("class Vessel") == 0);
    ws.check_now(user);
    CHECK(ws.error_count(*ws.doc(user)) == 0); // sees the renamed class in the other tab
    CHECK(p.read("scripts/user.plm").find("Ship") != std::string::npos); // nothing written to disk
    CHECK(ws.doc(user)->buf.undo()); // one undo group restores the file
    CHECK(ws.doc(user)->buf.text().find("Ship") != std::string::npos);
    CHECK_FALSE(ws.doc(user)->buf.dirty());
}

TEST_CASE("workspace: breakpoints follow their lines through edits") {
    TempProject p("bps");
    p.write("scripts/a.plm", "class A extends Node2D {\n    var a = 1\n    var b = 2\n    var c = 3\n}\n");
    ScriptWorkspace ws;
    ws.reset(p.dir);
    std::string err;
    const int id = ws.open(p.dir / "scripts/a.plm", err);
    REQUIRE(id > 0);
    CHECK(ws.toggle_breakpoint("scripts/a.plm", 3));
    CHECK(ws.toggle_breakpoint("scripts/a.plm", 4));
    const std::uint64_t rev = ws.breakpoint_revision();
    ScriptDoc* d = ws.doc(id);
    d->buf.set_caret({0, 0});
    d->buf.insert_newline(); // a line above: both move down
    d->syn.update(d->buf);
    CHECK(ws.breakpoints().at("scripts/a.plm") == std::set<int>{4, 5});
    CHECK(ws.breakpoint_revision() > rev);
    d->buf.select_range({{3, 0}, {4, 0}}); // delete line 4 (the one holding the old 3rd line's breakpoint)
    d->buf.insert("");
    d->syn.update(d->buf);
    CHECK(ws.breakpoints().at("scripts/a.plm") == std::set<int>{4});
    d->buf.set_caret({3, 3});
    d->buf.insert("x"); // an edit inside a line keeps it
    d->syn.update(d->buf);
    CHECK(ws.breakpoints().at("scripts/a.plm") == std::set<int>{4});
    CHECK_FALSE(ws.toggle_breakpoint("scripts/a.plm", 4));
    CHECK(ws.breakpoints().at("scripts/a.plm").empty());
    ws.set_breakpoints({{"scripts/a.plm", {2}}, {"scripts/other.plm", {9}}});
    CHECK(ws.doc(id)->bps->count(2) == 1); // the open document sees the loaded set
}

TEST_CASE("workspace: a project dir spelled differently from its canonical form (8.3 name, symlink) still maps files") {
    TempProject p("alias");
    p.write("scripts/a.plm", kGood);
    const fs::path alias = fs::temp_directory_path() / "cuervo_ws_alias_link";
    std::error_code ec;
    fs::remove(alias, ec);
    fs::create_directory_symlink(p.dir, alias, ec);
    if (ec) return; // no symlink privilege: nothing to check on this host
    ScriptWorkspace ws;
    ws.reset(alias);
    std::string err;
    const int a = ws.open("scripts/a.plm", err);
    REQUIRE(a >= 0);
    CHECK(ws.doc(a)->rel == "scripts/a.plm");
    fs::remove(alias, ec);
}

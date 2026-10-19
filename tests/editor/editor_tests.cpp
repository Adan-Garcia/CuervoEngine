// The editor's core (editor/, DESIGN.md 4.13), headless: the undo stack,
// node tree operations, inspector edits through the class registry (the
// scene system's @export metadata), byte-identical save round-trips, command
// scripts, and a level built purely from editor operations that then loads
// and runs in the script host like the runtime runs it.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "editor/class_registry.hpp"
#include "editor/command_script.hpp"
#include "editor/commands.hpp"
#include "editor/editor_document.hpp"
#include "editor/project_env.hpp"
#include "editor/scene_model.hpp"
#include "editor/view_camera.hpp"

using namespace cuervo;
using namespace cuervo::editor;
namespace fs = std::filesystem;

namespace {

const fs::path examples = fs::path(CUERVO_EXAMPLES_DIR);

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_editor_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::vector<char> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_text(const fs::path& p) {
    std::vector<char> b = read_bytes(p);
    return {b.begin(), b.end()};
}

// The engine prelude's classes only (Node, Node2D, the UI nodes...).
struct Prelude {
    ProjectEnv env;
    Prelude() {
        std::string error;
        REQUIRE_MESSAGE(env.open_prelude_only(error), error);
    }
};

Prelude& prelude() {
    static Prelude p;
    return p;
}

std::string dump(const EditorDocument& doc) { return scene::dump_text(doc.model().to_doc()); }

} // namespace

// ------------------------------------------------------------ command stack

TEST_CASE("editor: command stack undo, redo, redo-tail drop and dirty tracking") {
    SceneModel m = SceneModel::from_doc({{{"Node2D", "root", -1, {}}}});
    const NodeId root = m.root();
    CommandStack s;
    CHECK_FALSE(s.dirty());
    s.execute(m, std::make_unique<RenameCommand>(root, "root", "a"));
    s.execute(m, std::make_unique<RenameCommand>(root, "a", "b"));
    CHECK(m.get(root)->name == "b");
    CHECK(s.dirty());
    REQUIRE(s.undo(m));
    CHECK(m.get(root)->name == "a");
    REQUIRE(s.undo(m));
    CHECK(m.get(root)->name == "root");
    CHECK_FALSE(s.dirty()); // back at the saved state
    CHECK_FALSE(s.undo(m)); // nothing left
    REQUIRE(s.redo(m));
    CHECK(m.get(root)->name == "a");
    s.mark_saved();
    // A new command drops the redo tail.
    s.execute(m, std::make_unique<RenameCommand>(root, "a", "c"));
    CHECK_FALSE(s.can_redo());
    CHECK(s.size() == 2);
    CHECK(s.dirty());
    s.undo(m);
    CHECK_FALSE(s.dirty());
}

TEST_CASE("editor: mergeable edits fold into one undo step until sealed") {
    SceneModel m = SceneModel::from_doc({{{"Node2D", "root", -1, {}}}});
    const NodeId root = m.root();
    CommandStack s;
    auto set = [&](float x, bool merge) {
        const scene::Field* f = m.get(root)->field("position");
        std::optional<scene::Value> before;
        if (f != nullptr) before = f->value;
        s.execute(m, std::make_unique<SetFieldCommand>(root, "position", "vec2", before, scene::Value::vector({x, 0.0f})),
                  merge);
    };
    // A drag: four frames, one undo step.
    for (float x : {1.0f, 2.0f, 3.0f, 4.0f}) set(x, true);
    CHECK(s.size() == 1);
    CHECK(m.get(root)->field("position")->value.vec[0] == 4.0f);
    s.seal();
    // The next drag is its own step.
    set(10.0f, true);
    set(11.0f, true);
    CHECK(s.size() == 2);
    s.undo(m);
    CHECK(m.get(root)->field("position")->value.vec[0] == 4.0f);
    s.undo(m);
    CHECK(m.get(root)->field("position") == nullptr); // the field wasn't stored before the first drag
    s.redo(m);
    CHECK(m.get(root)->field("position")->value.vec[0] == 4.0f);
    // A different field never merges, even inside the window.
    s.redo(m);
    set(12.0f, true);
    s.execute(m, std::make_unique<RenameCommand>(root, "root", "r"), true);
    CHECK(s.size() == 4);
    // Merging into the saved step makes the document dirty for good.
    s.mark_saved();
    s.execute(m, std::make_unique<RenameCommand>(root, "r", "q"), true);
    CHECK(s.dirty());
}

// ------------------------------------------------------------ node tree

TEST_CASE("editor: add, rename, duplicate, reparent, delete -- and undo all of it exactly") {
    EditorDocument doc(&prelude().env.classes());
    doc.new_scene("Node2D", "level");
    const std::string empty = dump(doc);
    const NodeId root = doc.model().root();

    const NodeId a = doc.add_child(root, "Node2D", "group");
    REQUIRE(a != 0);
    CHECK(doc.selection() == a);
    const NodeId b = doc.add_child(a, "Label");
    REQUIRE(b != 0);
    CHECK(doc.model().get(b)->name == "Label");
    CHECK(doc.model().path_of(b) == "/group/Label");
    CHECK(doc.add_child(root, "NoSuchClass") == 0);
    CHECK(doc.error().find("not a node class") != std::string::npos);

    REQUIRE(doc.set_field(b, "text", scene::Value::str("hi")));
    const NodeId c = doc.duplicate(a);
    REQUIRE(c != 0);
    CHECK(doc.model().get(c)->name == "group2");
    CHECK(doc.model().index_in_parent(c) == 1);
    // The copy is deep and independent.
    const NodeId c_label = doc.model().find_path("/group2/Label");
    REQUIRE(c_label != 0);
    CHECK(c_label != b);
    CHECK(doc.field_value(c_label, "text").text == "hi");

    REQUIRE(doc.rename(c, "copy"));
    CHECK(doc.model().find_path("/copy/Label") == c_label);
    // Reparent: the copy under the original; a node can't go under itself.
    REQUIRE(doc.reparent(c, a));
    CHECK(doc.model().path_of(c_label) == "/group/copy/Label");
    CHECK_FALSE(doc.reparent(a, c));
    CHECK_FALSE(doc.reparent(root, a));
    // Reorder within a parent: copy before Label.
    REQUIRE(doc.reparent(c, a, 0));
    CHECK(doc.model().get(a)->children.front() == c);
    // Delete a subtree; the root can't go.
    CHECK_FALSE(doc.remove(root));
    REQUIRE(doc.remove(a));
    CHECK(doc.model().size() == 1);
    CHECK(doc.selection() == root);

    const std::string full = dump(doc);
    // Undo everything: back to the empty scene, byte for byte.
    while (doc.undo()) {}
    CHECK(dump(doc) == empty);
    // Redo everything: ids survive, so the deleted subtree comes back whole.
    while (doc.redo()) {}
    CHECK(dump(doc) == full);
    doc.undo(); // undo the delete
    CHECK(doc.model().find_path("/group/copy/Label") == c_label);
    CHECK(doc.selection() == a);
}

TEST_CASE("editor: paths address duplicate names by position") {
    EditorDocument doc(&prelude().env.classes());
    doc.new_scene();
    const NodeId root = doc.model().root();
    const NodeId x = doc.add_child(root, "Node2D", "x");
    REQUIRE(doc.rename(doc.add_child(root, "Node2D", "y"), "x")); // two siblings named x
    CHECK(doc.model().path_of(x) == "/#0");
    CHECK(doc.model().find_path("/#1") != 0);
    CHECK(doc.model().find_path("/x") == x); // the first match
    CHECK(doc.model().find_path("/nope") == 0);
    CHECK(doc.model().find_path("/") == root);
}

// ------------------------------------------------------------ inspector edits

TEST_CASE("editor: the class registry reads @export fields and defaults like the scene system") {
    const ClassRegistry& reg = prelude().env.classes();
    const ClassSpec* label = reg.find("Label");
    REQUIRE(label != nullptr);
    CHECK(label->base == "Control");
    CHECK(reg.is_a("Label", "Node2D"));
    CHECK_FALSE(reg.is_a("Label", "CanvasLayer"));
    // Inherited exports first (slot order), `name` excluded.
    REQUIRE(label->field("position") != nullptr);
    CHECK(label->field("name") == nullptr);
    CHECK(label->field("position")->type == "vec2");
    CHECK(label->field("position")->default_value == scene::Value::vector({0.0f, 0.0f}));
    CHECK(label->field("visible")->default_value == scene::Value::boolean(true));
    CHECK(label->field("style")->default_value == scene::Value::name("normal"));
    CHECK(label->field("font_size")->type == "float");
    // Non-exported fields (size, theme) are not shown.
    CHECK(label->field("size") == nullptr);
    CHECK(reg.find("CanvasLayer")->field("layer")->default_value == scene::Value::integer(1));
    // Engine classes that aren't nodes (Emitter, Canvas) are not offered.
    CHECK(reg.find("Emitter") == nullptr);
    CHECK(reg.find("Node") != nullptr);
}

TEST_CASE("editor: inspector edits coerce to the field's type and reset to the default") {
    // Through the editor_level project: an enum field, Color fields, ints.
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open(examples / "editor_level", nullptr, error), error);
    EditorDocument doc(&env.classes());
    doc.new_scene("Level", "level");
    const NodeId t = doc.add_child(doc.model().root(), "Turret", "t");
    REQUIRE(t != 0);

    // Default shown before any edit, not stored.
    bool stored = true;
    CHECK(doc.field_value(t, "count", &stored) == scene::Value::integer(12));
    CHECK_FALSE(stored);
    CHECK(doc.field_value(t, "pattern") == scene::Value::enum_value("FirePattern", "Ring"));

    // int literal into a float field; float field keeps f32.
    REQUIRE(doc.set_field(t, "interval", scene::Value::integer(2)));
    CHECK(doc.field_value(t, "interval", &stored) == scene::Value::number(2.0, scene::NumKind::F32));
    CHECK(stored);
    // An enum by bare variant name, and a wrong variant.
    REQUIRE(doc.set_field(t, "pattern", scene::Value::name("Spiral")));
    CHECK(doc.field_value(t, "pattern") == scene::Value::enum_value("FirePattern", "Spiral"));
    CHECK_FALSE(doc.set_field(t, "pattern", scene::Value::name("Zigzag")));
    CHECK(doc.error().find("no variant 'Zigzag'") != std::string::npos);
    // A Color from hex text; a partial struct keeps the other fields.
    REQUIRE(doc.set_field(t, "bullet_color", scene::Value::str("ff8000")));
    scene::Value c = doc.field_value(t, "bullet_color");
    REQUIRE(c.fields.size() == 4);
    CHECK(c.fields[0].value.f == 1.0);
    CHECK(c.fields[1].value.f == static_cast<double>(128.0f / 255.0f));
    std::string perr;
    auto alpha = scene::parse_value("Color { a = 0.5 }", perr);
    REQUIRE(alpha);
    REQUIRE(doc.set_field(t, "bullet_color", *alpha));
    c = doc.field_value(t, "bullet_color");
    CHECK(c.fields[0].value.f == 1.0);
    CHECK(c.fields[3].value.f == 0.5);
    // Type mismatches and unknown fields are rejected.
    CHECK_FALSE(doc.set_field(t, "count", scene::Value::str("many")));
    CHECK_FALSE(doc.set_field(t, "position", scene::Value::vector({1.0f, 2.0f, 3.0f})));
    CHECK_FALSE(doc.set_field(t, "no_such_field", scene::Value::integer(1)));
    // vecN, bools, strings, names through the same path.
    REQUIRE(doc.set_field(t, "position", scene::Value::vector({10.0f, 20.0f})));
    REQUIRE(doc.set_field(t, "visible", scene::Value::boolean(false)));
    REQUIRE(doc.set_field(doc.model().root(), "title", scene::Value::str("Level 2")));
    // "name" is the node's name.
    REQUIRE(doc.set_field(t, "name", scene::Value::str("gun")));
    CHECK(doc.model().get(t)->name == "gun");

    // Fields are stored in edit order; reset removes one, undo puts it back in place.
    const std::vector<std::string> order = [&] {
        std::vector<std::string> o;
        for (const scene::Field& f : doc.model().get(t)->fields) o.push_back(f.name);
        return o;
    }();
    CHECK(order == std::vector<std::string>{"interval", "pattern", "bullet_color", "position", "visible"});
    const std::string before = dump(doc);
    REQUIRE(doc.reset_field(t, "pattern"));
    CHECK(doc.field_value(t, "pattern", &stored) == scene::Value::enum_value("FirePattern", "Ring"));
    CHECK_FALSE(stored);
    doc.undo();
    CHECK(dump(doc) == before);

    // The move gizmo: Node2D moves `position`; one undo step per drag.
    const std::size_t steps = doc.stack().index();
    for (int i = 0; i < 5; ++i) REQUIRE(doc.move_by(t, {2.0f, -1.0f}, true));
    doc.seal();
    CHECK(doc.stack().index() == steps + 1);
    CHECK(doc.field_value(t, "position") == scene::Value::vector({20.0f, 15.0f}));
    doc.undo();
    CHECK(doc.field_value(t, "position") == scene::Value::vector({10.0f, 20.0f}));
    // A Control moves by its offsets (layout computes its position).
    const NodeId hud = doc.add_child(doc.model().root(), "CanvasLayer", "hud");
    const NodeId label = doc.add_child(hud, "Label", "l");
    REQUIRE(doc.move_by(label, {5.0f, 6.0f}));
    CHECK(doc.field_value(label, "offset_min") == scene::Value::vector({5.0f, 6.0f}));
    CHECK(doc.field_value(label, "offset_max") == scene::Value::vector({5.0f, 6.0f}));
    CHECK(doc.model().get(label)->field("position") == nullptr);
}

// ------------------------------------------------------------ files

TEST_CASE("editor: open and save without edits is byte-identical (every shmup scene)") {
    const fs::path out = temp_dir("roundtrip");
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(examples / "shmup" / "scenes")) {
        if (entry.path().extension() != ".scene") continue;
        EditorDocument doc(&prelude().env.classes());
        std::string error;
        REQUIRE_MESSAGE(doc.open(entry.path(), error), error);
        CHECK_FALSE(doc.dirty());
        const fs::path copy = out / entry.path().filename();
        REQUIRE_MESSAGE(doc.save_as(copy, error), error);
        CHECK_MESSAGE(read_bytes(copy) == read_bytes(entry.path()), entry.path());
        // Edit, undo: still identical.
        const NodeId root = doc.model().root();
        REQUIRE(doc.add_child(root, "Node2D") != 0);
        REQUIRE(doc.set_field(root, "always_process", scene::Value::boolean(false)));
        REQUIRE(doc.rename(root, "renamed"));
        CHECK(doc.dirty());
        while (doc.undo()) {}
        CHECK_FALSE(doc.dirty());
        REQUIRE(doc.save(error));
        CHECK(read_bytes(copy) == read_bytes(entry.path()));
        ++checked;
    }
    CHECK(checked == 4);
}

TEST_CASE("editor: new scene, save as, reopen") {
    const fs::path out = temp_dir("new");
    EditorDocument doc(&prelude().env.classes());
    doc.new_scene("CanvasLayer", "menu");
    std::string error;
    CHECK_FALSE(doc.save(error)); // no path yet
    const NodeId l = doc.add_child(doc.model().root(), "Label", "title");
    REQUIRE(doc.set_field(l, "text", scene::Value::str("Hello")));
    REQUIRE_MESSAGE(doc.save_as(out / "sub" / "menu.scene", error), error);
    CHECK_FALSE(doc.dirty());
    EditorDocument again(&prelude().env.classes());
    REQUIRE_MESSAGE(again.open(out / "sub" / "menu.scene", error), error);
    CHECK(dump(again) == dump(doc));
    CHECK(again.field_value(again.model().find_path("/title"), "text").text == "Hello");
    CHECK_FALSE(again.open(out / "missing.scene", error));
}

// ------------------------------------------------------------ command scripts

TEST_CASE("editor: command scripts apply operations and report the failing line") {
    EditorDocument doc(&prelude().env.classes());
    ProjectEnv& env = prelude().env;
    ScriptResult r = run_command_script("# a menu\n"
                                        "new CanvasLayer menu\n"
                                        "set / layer 5\n"
                                        "add / VBox col\n"
                                        "add /col Label \"the title\"\n"
                                        "set \"/col/the title\" text \"Hi there\"\n"
                                        "set . font_size 30\n"
                                        "duplicate \"/col/the title\"\n"
                                        "move /col 10 -4\n"
                                        "expect /col offset_min (10.0, -4.0)\n"
                                        "expect \"/col/the title2\" text \"Hi there\"\n"
                                        "expect / name menu\n",
                                        doc, env);
    REQUIRE_MESSAGE(r.ok, r.line << ": " << r.error);
    CHECK(r.commands == 8); // `expect` lines check, they don't count as edits
    CHECK(doc.field_value(doc.model().root(), "layer") == scene::Value::integer(5));
    CHECK(doc.field_value(doc.model().find_path("/col/the title"), "font_size") ==
          scene::Value::number(30.0, scene::NumKind::F32));

    r = run_command_script("add / Label ok\nadd /nope Label x\n", doc, env);
    CHECK_FALSE(r.ok);
    CHECK(r.line == 2);
    CHECK(r.error == "no node at '/nope'");
    r = run_command_script("expect / layer 6\n", doc, env);
    CHECK_FALSE(r.ok);
    CHECK(r.error == "expected layer = 6, got 5");
    r = run_command_script("frobnicate /\n", doc, env);
    CHECK(r.error == "unknown command 'frobnicate'");
    r = run_command_script("set / layer (1.0, 2.0)\n", doc, env);
    CHECK_FALSE(r.ok);
    r = run_command_script("undo\nundo\nredo\n", doc, env);
    CHECK(r.ok);
}

// ------------------------------------------------------------ a level without code

TEST_CASE("editor: a level built only from editor operations loads and runs like the runtime runs it") {
    const fs::path project = examples / "editor_level";
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open(project, nullptr, error), error);
    EditorDocument doc(&env.classes());

    // level.cmds, with its final save redirected to a temp file.
    std::string cmds = read_text(project / "level.cmds");
    const std::string save_line = "save scenes/level.scene";
    const auto at = cmds.find(save_line);
    REQUIRE(at != std::string::npos);
    const fs::path out = temp_dir("level") / "level.scene";
    cmds.replace(at, save_line.size(), "save " + out.string());
    const ScriptResult r = run_command_script(cmds, doc, env);
    REQUIRE_MESSAGE(r.ok, "level.cmds:" << r.line << ": " << r.error);
    CHECK(r.saved);
    // The checked-in level is exactly what the operations produce.
    CHECK(read_bytes(out) == read_bytes(project / "scenes" / "level.scene"));

    // The scene view's preview builds it (no ready(), no ticks) and lays out UI.
    env.rebuild_preview(doc.model().to_doc());
    REQUIRE(env.preview_ok());
    const std::vector<NodeId> order = doc.model().preorder();
    REQUIRE(env.bounds().size() == order.size());
    const int ship = doc.model().doc_index(doc.model().find_path("/ship"));
    CHECK(env.bounds()[static_cast<std::size_t>(ship)].position == vec2{640.0f, 610.0f});
    CHECK(pick_node(env.bounds(), {642.0f, 612.0f}, 10.0f) == ship);
    const int caption = doc.model().doc_index(doc.model().find_path("/hud/caption"));
    CHECK(env.bounds()[static_cast<std::size_t>(caption)].has_rect);
    CHECK(env.host().tick_count() == 0); // the preview never ticks
    // ...and the engine's renderer draws it (batchers without a device
    // collect the shapes): backdrop, walls, turrets, ship.
    render::SdfBatcher shapes;
    render::TextRenderer text;
    env.host().draw(shapes, text, render::FontHandle{});
    CHECK_FALSE(env.host().panicked());
    CHECK(shapes.size() >= 12);

    // The game: project.toml's main_scene is the root, as `cuervo --game` runs it.
    auto p = script::load_project(project, error);
    REQUIRE_MESSAGE(p, error);
    CHECK(p->main_scene == "scenes/level.scene");
    InputMap map;
    REQUIRE(script::apply_input(*p, map, error));
    std::vector<pluma::SourceFile::Part> sources;
    REQUIRE(script::read_scripts(*p, sources, nullptr, error));
    auto run = [&] {
        script::HostOptions o;
        o.seed = p->seed;
        o.tick_rate = p->tick_rate;
        o.project_dir = p->dir;
        o.main_scene = p->main_scene;
        auto host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load(sources, p->main_class), host->error());
        REQUIRE_MESSAGE(host->running(), host->error());
        for (int i = 0; i < 300; ++i) {
            host->tick(InputFrame{});
            REQUIRE_MESSAGE(host->running(), host->error());
        }
        return host;
    };
    auto a = run();
#if CUERVO_FEATURE_PARTICLES
    CHECK(a->particles().particle_count() > 20); // the turrets are firing
#endif
    pluma::Value level = a->session()->global("Level.main");
    REQUIRE(level.is_obj()); // ready() ran on the loaded root
    auto b = run();
    CHECK(a->state_hash() == b->state_hash()); // and deterministically
}

TEST_CASE("editor: project.toml main_scene must stay inside the project") {
    const fs::path dir = temp_dir("main_scene");
    std::ofstream(dir / "project.toml") << "name = \"x\"\nmain_scene = \"../outside.scene\"\n";
    std::string error;
    CHECK_FALSE(script::load_project(dir, error));
    CHECK(error.find("main_scene must be a path inside the project") != std::string::npos);
}

// ------------------------------------------------------------ scene view camera

TEST_CASE("editor: the view camera zooms about the cursor and maps to NDC") {
    ViewCamera cam;
    cam.center = {640.0f, 360.0f};
    const vec2 origin{100.0f, 50.0f};
    const vec2 size{800.0f, 600.0f};
    const vec2 cursor{300.0f, 200.0f};
    const vec2 before = cam.screen_to_world(cursor, origin, size);
    cam.zoom_at(cursor, 2.0f, origin, size);
    CHECK(cam.zoom == 2.0f);
    const vec2 after = cam.screen_to_world(cursor, origin, size);
    CHECK(after.x() == doctest::Approx(before.x()));
    CHECK(after.y() == doctest::Approx(before.y()));
    // The view's center maps to NDC (0, 0); its top-left corner to (-1, 1).
    const Affine2 m = cam.world_to_ndc(size);
    const vec2 c = m.apply(cam.center);
    CHECK(c.x() == doctest::Approx(0.0f));
    CHECK(c.y() == doctest::Approx(0.0f));
    const vec2 tl = m.apply(cam.screen_to_world(origin, origin, size));
    CHECK(tl.x() == doctest::Approx(-1.0f));
    CHECK(tl.y() == doctest::Approx(1.0f));
    CHECK(grid_step(1.0f) == 16.0f);
    CHECK(grid_step(0.25f) == 64.0f);
    CHECK(grid_step(4.0f) == 4.0f);
}

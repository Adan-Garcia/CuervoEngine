// Milestone 8.3's editor core, headless (DESIGN.md 4.16 "Clarified (Milestone
// 8.3)"): the command palette's fuzzy search and actions, multi-select with group
// transforms through the undo system, the undo history, saved layouts, recent
// projects and templates, icons, and the user-facing message formula.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "editor/actions.hpp"
#include "editor/command_script.hpp"
#include "editor/editor_document.hpp"
#include "editor/icons.hpp"
#include "editor/layouts.hpp"
#include "editor/messages.hpp"
#include "editor/project_env.hpp"
#include "editor/projects.hpp"
#include "editor/settings.hpp"

using namespace cuervo;
using namespace cuervo::editor;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_editor_83_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

ProjectEnv& prelude_env() {
    static ProjectEnv env;
    static bool opened = false;
    if (!opened) {
        std::string error;
        REQUIRE_MESSAGE(env.open_prelude_only(error), error);
        opened = true;
    }
    return env;
}

// Three Node2Ds under the root: a (0,0), b (100,0), c (0,100).
EditorDocument three_nodes() {
    EditorDocument doc(&prelude_env().classes());
    doc.new_scene("Node2D", "root");
    for (const char* n : {"a", "b", "c"}) REQUIRE(doc.add_child(doc.model().root(), "Node2D", n) != 0);
    CHECK(doc.set_field(doc.model().find_path("/b"), "position", scene::Value::vector({100.0f, 0.0f})));
    CHECK(doc.set_field(doc.model().find_path("/c"), "position", scene::Value::vector({0.0f, 100.0f})));
    return doc;
}

scene::Value field(const EditorDocument& doc, std::string_view path, std::string_view name) {
    return doc.field_value(doc.model().find_path(path), name);
}

} // namespace

// ------------------------------------------------------------ fuzzy matching

TEST_CASE("palette: fuzzy matching is a case-insensitive subsequence per word") {
    FuzzyMatch m;
    CHECK(fuzzy_match("dup sel", "Edit: Duplicate Selection", m));
    CHECK(m.positions.size() == 6);
    CHECK(fuzzy_match("DUPSEL", "Edit: Duplicate Selection", m)); // one word, still a subsequence
    CHECK_FALSE(fuzzy_match("sel dup z", "Edit: Duplicate Selection", m));
    CHECK_FALSE(fuzzy_match("xyz", "Edit: Duplicate Selection", m));
    CHECK(fuzzy_match("", "anything", m)); // empty matches everything
    CHECK(fuzzy_match("   ", "anything", m));
    CHECK_FALSE(fuzzy_match("longer than the text", "short", m));
}

TEST_CASE("palette: word starts and consecutive matches outrank scattered ones") {
    FuzzyMatch tight;
    FuzzyMatch scattered;
    REQUIRE(fuzzy_match("sel", "Select Root", tight));
    REQUIRE(fuzzy_match("sel", "Edit: Duplicate Selection and More Lines", scattered));
    CHECK(tight.score > scattered.score);
    FuzzyMatch word_start;
    FuzzyMatch mid_word;
    REQUIRE(fuzzy_match("gs", "Group Selection", word_start));
    REQUIRE(fuzzy_match("gs", "Edit: Pagination Settings Tweaks", mid_word));
    CHECK(word_start.score > mid_word.score);
}

TEST_CASE("palette: search ranks, lists disabled last and remembers what you ran") {
    EditorDocument doc = three_nodes();
    ActionRegistry reg;
    add_document_actions(reg, doc);
    // Nothing is selected as a target (the root is): selection actions are disabled but findable.
    doc.select(doc.model().root());
    auto hits = reg.search("duplicate selection");
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.front().action->id == "edit.duplicate");
    CHECK_FALSE(hits.front().enabled);
    CHECK_FALSE(reg.run("edit.duplicate"));
    doc.select(doc.model().find_path("/a"));
    hits = reg.search("dup sel");
    CHECK(hits.front().action->title == "Duplicate Selection");
    CHECK(hits.front().enabled);
    CHECK(reg.run("edit.duplicate"));
    CHECK(doc.model().find_path("/a2") != 0);
    // Every node class is an "Add Node" command.
    hits = reg.search("add node node2d");
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.front().action->id == "node.add.Node2D");
    // Empty query: everything, the last one run first.
    reg.run("select.root");
    hits = reg.search("");
    CHECK(hits.front().action->id == "select.root");
    CHECK(hits.size() == reg.all().size());
    // A later registration with the same id replaces the earlier one.
    const std::size_t n = reg.all().size();
    reg.add({"select.root", "Select Root Again", "Select", "", {}, [] { return true; }});
    CHECK(reg.all().size() == n);
    CHECK(reg.find("select.root")->title == "Select Root Again");
}

TEST_CASE("palette: command scripts run the best match, like Enter in the palette") {
    EditorDocument doc(&prelude_env().classes());
    doc.new_scene("Node2D", "root");
    const char* script = R"(
        select /
        expect-palette "add node label" Label
        palette add node label
        rename . title
        palette duplicate selection
        expect-selected 1
        expect /title2 name "title2"
    )";
    ScriptResult r = run_command_script(script, doc, prelude_env());
    CHECK_MESSAGE(r.ok, r.error);
    CHECK(doc.model().find_path("/title2") != 0);
    r = run_command_script("palette qqqqzzzz", doc, prelude_env());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("no enabled command") != std::string::npos);
}

// ------------------------------------------------------------ multi-select and group transforms

TEST_CASE("selection: add, toggle, set and prune after structural edits") {
    EditorDocument doc = three_nodes();
    const NodeId a = doc.model().find_path("/a");
    const NodeId b = doc.model().find_path("/b");
    const NodeId c = doc.model().find_path("/c");
    doc.select(a);
    doc.select_add(b);
    doc.select_toggle(c);
    CHECK(doc.selected().size() == 3);
    CHECK(doc.selection() == c); // the last picked is the primary
    doc.select_toggle(c);
    CHECK(doc.selected().size() == 2);
    CHECK(doc.selection() == b);
    doc.select_set({a, c, 9999});
    CHECK(doc.selected().size() == 2);
    // Deleting a selected node drops it from the selection.
    doc.select_set({a, b});
    CHECK(doc.remove(a));
    CHECK_FALSE(doc.is_selected(a));
    // The root is never a transform target; a selected parent covers its selected child.
    doc.select_set({doc.model().root(), b});
    CHECK(doc.transform_targets() == std::vector<NodeId>{b});
}

TEST_CASE("group move: world deltas become each node's local delta (transform hierarchy)") {
    EditorDocument doc = three_nodes();
    const NodeId a = doc.model().find_path("/a");
    const NodeId b = doc.model().find_path("/b");
    doc.select_set({a, b});
    // As if /b's parent were scaled by 2: its local delta is half the world one.
    const EditorDocument::DeltaToLocal to_local = [b](NodeId id, vec2 d) { return id == b ? d * 0.5f : d; };
    CHECK(doc.move_selection_by({10.0f, 4.0f}, false, to_local));
    CHECK(scene::format_value(field(doc, "/a", "position")) == "(10.0, 4.0)");
    CHECK(scene::format_value(field(doc, "/b", "position")) == "(105.0, 2.0)");
}

TEST_CASE("group move: one undo step for the whole selection, merged while dragging") {
    EditorDocument doc = three_nodes();
    const std::size_t before = doc.stack().size();
    doc.select_set({doc.model().find_path("/a"), doc.model().find_path("/b")});
    // A gizmo drag: many frames, one step.
    CHECK(doc.move_selection_by({5.0f, 0.0f}));
    CHECK(doc.move_selection_by({5.0f, 2.0f}));
    CHECK(doc.move_selection_by({0.0f, 3.0f}));
    doc.seal();
    CHECK(doc.stack().size() == before + 1);
    CHECK(doc.stack().undo_label() == "Move 2 nodes");
    CHECK(scene::format_value(field(doc, "/a", "position")) == "(10.0, 5.0)");
    CHECK(scene::format_value(field(doc, "/b", "position")) == "(110.0, 5.0)");
    CHECK(scene::format_value(field(doc, "/c", "position")) == "(0.0, 100.0)"); // untouched
    REQUIRE(doc.undo());
    CHECK(scene::format_value(field(doc, "/a", "position")) == "(0.0, 0.0)");
    CHECK(scene::format_value(field(doc, "/b", "position")) == "(100.0, 0.0)");
    CHECK(doc.selected().size() == 2); // undo keeps the multi-selection
    REQUIRE(doc.redo());
    CHECK(scene::format_value(field(doc, "/b", "position")) == "(110.0, 5.0)");
}

TEST_CASE("group rotate and scale turn about the centroid and undo exactly") {
    EditorDocument doc = three_nodes();
    const std::string original = scene::dump_text(doc.model().to_doc());
    doc.select_set({doc.model().find_path("/a"), doc.model().find_path("/b")});
    // The pivot is the centroid (50, 0); a quarter turn clockwise (y down) sends (0,0) to (50,-50).
    REQUIRE(doc.selection_pivot());
    CHECK(*doc.selection_pivot() == vec2{50.0f, 0.0f});
    CHECK(doc.rotate_selection_by(1.5707964f, std::nullopt, false));
    const scene::Value pa = field(doc, "/a", "position");
    const scene::Value pb = field(doc, "/b", "position");
    CHECK(pa.vec[0] == doctest::Approx(50.0f).epsilon(0.01));
    CHECK(pa.vec[1] == doctest::Approx(-50.0f).epsilon(0.01));
    CHECK(pb.vec[0] == doctest::Approx(50.0f).epsilon(0.01));
    CHECK(pb.vec[1] == doctest::Approx(50.0f).epsilon(0.01));
    CHECK(field(doc, "/a", "rotation").f == doctest::Approx(1.5707964).epsilon(0.001));
    CHECK(doc.scale_selection_by({2.0f, 2.0f}, std::nullopt, false));
    CHECK(scene::format_value(field(doc, "/a", "scale")) == "(2.0, 2.0)");
    // Two steps, two undos, back to the exact original document.
    CHECK(doc.undo());
    CHECK(doc.undo());
    CHECK(scene::dump_text(doc.model().to_doc()) == original);
    // A single node turns about itself: its position is not even stored.
    doc.select(doc.model().find_path("/c"));
    CHECK(doc.rotate_selection_by(0.5f, std::nullopt, false));
    CHECK(scene::format_value(field(doc, "/c", "position")) == "(0.0, 100.0)");
    CHECK(field(doc, "/c", "rotation").f == doctest::Approx(0.5));
}

TEST_CASE("group transforms skip what cannot take them and say why") {
    EditorDocument doc(&prelude_env().classes());
    doc.new_scene("Node2D", "root");
    REQUIRE(doc.add_child(doc.model().root(), "Button", "ok") != 0);
    doc.select(doc.model().find_path("/ok"));
    CHECK_FALSE(doc.rotate_selection_by(1.0f));
    CHECK(doc.error().find("can rotate") != std::string::npos);
    CHECK(doc.move_selection_by({4.0f, 4.0f})); // a control moves its offsets
    doc.select(doc.model().root());
    CHECK_FALSE(doc.move_selection_by({1.0f, 1.0f}));
}

TEST_CASE("group duplicate, delete and group: each one undo step") {
    EditorDocument doc = three_nodes();
    const std::string original = scene::dump_text(doc.model().to_doc());
    doc.select_set({doc.model().find_path("/a"), doc.model().find_path("/b")});
    CHECK(doc.duplicate_selection());
    CHECK(doc.model().size() == 6);
    CHECK(doc.selected().size() == 2); // the copies are selected
    CHECK(doc.undo());
    CHECK(scene::dump_text(doc.model().to_doc()) == original);
    doc.select_set({doc.model().find_path("/a"), doc.model().find_path("/b")});
    CHECK(doc.remove_selection());
    CHECK(doc.model().size() == 2);
    CHECK(doc.undo());
    CHECK(scene::dump_text(doc.model().to_doc()) == original);
    doc.select_set({doc.model().find_path("/b"), doc.model().find_path("/c")});
    const NodeId group = doc.group_selection();
    REQUIRE(group != 0);
    CHECK(doc.model().find_path("/Group/b") != 0);
    CHECK(doc.model().find_path("/Group/c") != 0);
    CHECK(doc.model().find_path("/a") != 0);
    CHECK(doc.stack().undo_label() == "Group 2 nodes");
    CHECK(doc.undo());
    CHECK(scene::dump_text(doc.model().to_doc()) == original);
}

TEST_CASE("history panel: jump to any step and back") {
    EditorDocument doc = three_nodes();
    const std::size_t steps = doc.stack().size();
    REQUIRE(steps >= 5);
    CHECK(doc.stack().label_at(0) == "Add Node2D");
    CHECK(doc.jump_to_history(2));
    CHECK(doc.stack().index() == 2);
    CHECK(doc.model().size() == 3); // root + a + b
    CHECK(doc.stack().can_redo());
    CHECK(doc.jump_to_history(steps));
    CHECK(doc.model().size() == 4);
    CHECK_FALSE(doc.jump_to_history(steps + 1));
    CHECK(doc.jump_to_history(0));
    CHECK(doc.model().size() == 1);
}

// ------------------------------------------------------------ layouts

TEST_CASE("layouts: built-ins, save as, persistence across runs") {
    LayoutStore store;
    CHECK(store.names().size() == 3);
    CHECK(store.layout("Debugging").bottom > store.layout("Default").bottom);
    Layout mine;
    mine.left = 0.25f;
    mine.right = 0.30f;
    mine.bottom = 0.35f;
    mine.set_hidden("Console", true);
    mine.set_hidden("Profiler", true);
    mine.set_hidden("Profiler", true); // idempotent
    // ImGui's dock text is multi-line and has backslashes in paths of window names: it survives the file.
    mine.dock = "[Window][Scene View]\nPos=0,0\nDockId=0x00000001\n\n[Docking][Data]\nDockSpace ID=0x1 Pos=0,0 Size=10,10 Split=X\n";
    CHECK(mine.hidden.size() == 2);
    std::string error;
    CHECK_FALSE(store.save_as("Default", mine, error)); // a built-in name
    CHECK_FALSE(error.empty());
    CHECK_FALSE(store.save_as("", mine, error));
    CHECK_FALSE(store.save_as("a=b", mine, error));
    REQUIRE(store.save_as("Mine", mine, error));
    CHECK(store.active() == "Mine");
    Layout current = mine;
    current.left = 0.21f;
    store.set_current(current);

    const fs::path file = temp_dir("layouts") / "editor-layouts.txt";
    REQUIRE(store.save(file, error));
    LayoutStore reloaded;
    REQUIRE_MESSAGE(reloaded.load(file, error), error);
    CHECK(reloaded.active() == "Mine");
    CHECK(reloaded.layout("Mine") == store.layout("Mine"));
    CHECK(reloaded.layout("Mine").is_hidden("Console"));
    CHECK(reloaded.layout("Mine").dock == mine.dock);
    CHECK(reloaded.layout("Default").dock.empty()); // built-ins are built from the fractions
    CHECK(reloaded.current().left == doctest::Approx(0.21f));
    CHECK(reloaded.names() == store.names());
    CHECK(reloaded.remove("Mine", error));
    CHECK(reloaded.active() == "Default");
    CHECK_FALSE(reloaded.remove("Minimal", error)); // built-in
}

TEST_CASE("layouts: bad files are rejected whole and out-of-range values are clamped") {
    LayoutStore store;
    std::string error;
    CHECK_FALSE(store.parse("[layout X]\nleft=wide\n", error));
    CHECK(error.find("not a number") != std::string::npos);
    CHECK_FALSE(store.parse("[nonsense]\n", error));
    CHECK_FALSE(store.parse("left=0.2\n", error));
    CHECK(store.names().size() == 3); // unchanged
    REQUIRE(store.parse("active=Nope\n[current]\nleft=0.9\nright=0.9\nbottom=2\n", error));
    CHECK(store.active() == "Default"); // unknown active falls back
    CHECK(store.current().left + store.current().right <= 0.75f + 1e-5f);
    CHECK(store.current().bottom <= 0.70f);
    CHECK(store.load(temp_dir("nolayouts") / "missing.txt", error)); // none saved yet is fine
}

// ------------------------------------------------------------ projects

TEST_CASE("recent projects: newest first, deduplicated, capped, missing ones flagged") {
    const fs::path root = temp_dir("recent");
    RecentProjects recent;
    std::string error;
    for (int i = 0; i < 12; ++i) {
        const fs::path dir = root / ("p" + std::to_string(i));
        REQUIRE_MESSAGE(create_project("empty", dir, "P" + std::to_string(i), error), error);
        recent.add(dir);
    }
    recent.add(root / "p5"); // moves to the front
    CHECK(recent.paths().size() == RecentProjects::max_entries);
    auto list = recent.list();
    CHECK(list.front().name == "P5");
    CHECK(list.front().exists);
    fs::remove_all(root / "p5");
    list = recent.list();
    CHECK_FALSE(list.front().exists);
    // Persistence.
    const fs::path file = root / "recent.txt";
    REQUIRE(recent.save(file, error));
    RecentProjects again;
    REQUIRE(again.load(file));
    CHECK(again.paths() == recent.paths());
    again.remove(again.paths().front());
    CHECK(again.paths().size() == RecentProjects::max_entries - 1);
}

// ------------------------------------------------------------ settings

TEST_CASE("editor settings: scaling factors and per-project last scene persistence") {
    EditorSettings settings;
    CHECK(settings.ui_scale() == doctest::Approx(1.0f));
    CHECK(settings.font_scale() == doctest::Approx(1.0f));

    settings.set_ui_scale(1.25f);
    settings.set_font_scale(1.10f);
    CHECK(settings.ui_scale() == doctest::Approx(1.25f));
    CHECK(settings.font_scale() == doctest::Approx(1.10f));

    const fs::path proj_a = temp_dir("settings_proj_a");
    const fs::path proj_b = temp_dir("settings_proj_b");
    settings.set_last_scene(proj_a, "scenes/level1.scene");
    settings.set_last_scene(proj_b, "scenes/level2.scene");

    CHECK(settings.last_scene(proj_a) == "scenes/level1.scene");
    CHECK(settings.last_scene(proj_b) == "scenes/level2.scene");

    const std::string text = settings.serialize();
    EditorSettings parsed;
    parsed.parse(text);
    CHECK(parsed.ui_scale() == doctest::Approx(1.25f));
    CHECK(parsed.font_scale() == doctest::Approx(1.10f));
    CHECK(parsed.last_scene(proj_a) == "scenes/level1.scene");
    CHECK(parsed.last_scene(proj_b) == "scenes/level2.scene");

    // File save and load
    const fs::path file = temp_dir("settings_dir") / "editor-settings.txt";
    std::string error;
    REQUIRE(settings.save(file, error));

    EditorSettings loaded;
    REQUIRE(loaded.load(file));
    CHECK(loaded.ui_scale() == doctest::Approx(1.25f));
    CHECK(loaded.font_scale() == doctest::Approx(1.10f));
    CHECK(loaded.last_scene(proj_a) == "scenes/level1.scene");
    CHECK(loaded.last_scene(proj_b) == "scenes/level2.scene");
}

TEST_CASE("editor document: change_type supports binding scripts and undo/redo") {
    ProjectEnv& env = prelude_env();
    EditorDocument doc(&env.classes());
    doc.new_scene("Node2D", "root");
    const NodeId child = doc.add_child(doc.model().root(), "Node2D", "player");
    REQUIRE(child != 0);
    CHECK(doc.model().get(child)->type == "Node2D");

    // Changing type to Sprite (or another valid Node class)
    REQUIRE(doc.change_type(child, "Sprite"));
    CHECK(doc.model().get(child)->type == "Sprite");

    // Undo reverts type
    REQUIRE(doc.undo());
    CHECK(doc.model().get(child)->type == "Node2D");

    // Redo re-applies type
    REQUIRE(doc.redo());
    CHECK(doc.model().get(child)->type == "Sprite");

    // Non-existent class is rejected
    CHECK_FALSE(doc.change_type(child, "NonExistentClassXYZ"));
    // Empty class name is rejected
    CHECK_FALSE(doc.change_type(child, ""));
}

TEST_CASE("project templates create projects the editor opens") {
    const fs::path root = temp_dir("templates");
    for (const ProjectTemplate& t : project_templates()) {
        const fs::path dir = root / t.id;
        std::string error;
        REQUIRE_MESSAGE(create_project(t.id, dir, "My " + t.name, error), error);
        CHECK(read_project_name(dir) == "My " + t.name);
        ProjectEnv env;
        REQUIRE_MESSAGE(env.open(dir, nullptr, error), error);
        EditorDocument doc(&env.classes());
        REQUIRE_MESSAGE(doc.open(dir / "scenes" / "main.scene", error), error);
        CHECK(doc.model().size() == 2);
        // Creating again over it is refused and nothing there changes.
        CHECK_FALSE(create_project(t.id, dir, "Other", error));
        CHECK(error.find("already has files") != std::string::npos);
        CHECK(read_project_name(dir) == "My " + t.name);
    }
    std::string error;
    CHECK_FALSE(create_project("nope", root / "x", "X", error));
    CHECK_FALSE(create_project("empty", root / "y", "  ", error));
}

// ------------------------------------------------------------ icons

TEST_CASE("icons: every class has one, unknown classes fall back, all paths parse") {
    for (const std::string& name : icon_names()) {
        const auto* shapes = icon_shapes(name);
        REQUIRE_MESSAGE(shapes != nullptr, name);
        CHECK_MESSAGE(!shapes->empty(), name);
        for (const IconShape& s : *shapes) {
            CHECK_MESSAGE(!s.path.empty(), name);
            const vector::Bounds b = s.path.bounds();
            CHECK_MESSAGE((b.min.x() >= -0.5f && b.min.y() >= -0.5f && b.max.x() <= 24.5f && b.max.y() <= 24.5f), name);
        }
    }
    const ClassRegistry& classes = prelude_env().classes();
    for (const ClassSpec& c : classes.classes()) {
        CHECK_MESSAGE(has_icon(icon_for_class(&classes, c.name)), c.name);
    }
    CHECK(icon_for_class(&classes, "Node2D") == "node2d");
    CHECK(icon_for_class(&classes, "Button") == "button");
    CHECK(icon_for_class(&classes, "VBox") == "vbox");
    CHECK(icon_for_class(&classes, "SomethingNew") == "node");       // not in any registry
    CHECK(icon_for_class(nullptr, "Camera2D") == "camera");          // node library names work without a registry
    // A project class takes its base's icon.
    CHECK(icon_for_class(&classes, "Control") == "control");
    CHECK(icon_for_class(&classes, "Label") == "label");
    // Every toolbar icon exists.
    for (const char* n : {"select", "move", "rotate", "scale", "play", "pause", "stop", "step", "save", "undo", "redo",
                          "search", "history", "warning", "error", "info", "check", "close", "layout", "command"}) {
        CHECK_MESSAGE(has_icon(n), n);
    }
}

// ------------------------------------------------------------ messages

TEST_CASE("messages follow the kit's formula and keep the raw error in Details") {
    const UserMessage m = message_save_failed("scenes/level.scene", "ofstream: permission denied");
    CHECK(m.severity == Severity::Danger);
    CHECK(m.what.find("could not be saved") != std::string::npos);
    CHECK(m.safety.find("nothing was lost") != std::string::npos);
    CHECK_FALSE(m.action.empty());
    CHECK(m.details == "ofstream: permission denied");
    CHECK(m.sentence().find("permission denied") == std::string::npos); // raw text is not in the sentence
    for (const UserMessage& each : {message_edit_rejected("x"), message_open_failed("a", "b", true),
                                    message_project_failed("c", "d"), message_from_log("error", "boom"),
                                    message_from_log("warn", "careful")}) {
        CHECK_FALSE(each.what.empty());
        CHECK_FALSE(each.action.empty());
        CHECK(each.has_details());
    }
}

TEST_CASE("message center: toasts expire, warnings and errors stay, repeats fold") {
    MessageCenter center;
    UserMessage ok;
    ok.severity = Severity::Success;
    ok.what = "Saved.";
    center.push(ok, 0.0);
    center.push(message_save_failed("a", "x"), 0.0);
    center.push(message_save_failed("a", "x"), 1.0); // folded into the previous one
    CHECK(center.items().size() == 2);
    center.update(MessageCenter::toast_seconds + 1.0);
    REQUIRE(center.items().size() == 1);
    CHECK(center.items()[0].message.severity == Severity::Danger);
    center.items()[0].dismissed = true;
    center.update(100.0);
    CHECK(center.empty());
}

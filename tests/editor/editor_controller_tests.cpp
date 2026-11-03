// EditorController: the UI-free editor session both UIs call (DESIGN.md 4.13). Prompts, discard
// handling, tools and messages, with no window, SDL or ImGui.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "editor/editor_controller.hpp"

using namespace cuervo;
using namespace cuervo::editor;

namespace {

void prepare(EditorController& c) {
    std::string error;
    REQUIRE_MESSAGE(c.env().open_prelude_only(error), error);
    c.doc().set_registry(&c.env().classes());
    c.doc().new_scene("Node2D", "root");
}

void make_dirty(EditorController& c) { REQUIRE(c.doc().add_child(c.doc().model().root(), "Node2D", "a") != 0); }

} // namespace

TEST_CASE("controller: leaving a dirty scene asks first, then switches") {
    EditorController c;
    prepare(c);
    c.request_switch_project("other");
    CHECK(c.switch_requested()); // a clean scene leaves without asking
    CHECK(c.prompt().kind == Prompt::None);
    EditorController d;
    prepare(d);
    make_dirty(d);
    d.request_switch_project("other");
    CHECK_FALSE(d.switch_requested());
    CHECK(d.prompt().kind == Prompt::DiscardLeave);
    CHECK(d.discard_is_leave());
    d.confirm_discard();
    CHECK(d.switch_requested());
    CHECK(d.switch_target() == "other");
}

TEST_CASE("controller: opening over unsaved changes prompts, cancel keeps the scene") {
    EditorController c;
    prepare(c);
    make_dirty(c);
    c.request_open_scene("scenes/x.scene");
    PendingPrompt p = c.take_prompt();
    CHECK(p.kind == Prompt::DiscardOpen);
    CHECK(p.path == "scenes/x.scene");
    CHECK(c.discard_path() == "scenes/x.scene");
    CHECK(c.prompt().kind == Prompt::None); // handed over once
    c.cancel_discard();
    CHECK(c.discard_path().empty());
    CHECK(c.doc().dirty());
}

TEST_CASE("controller: save without a path asks for one; add child and attach script are prompts") {
    EditorController c;
    prepare(c);
    c.save_current_scene();
    CHECK(c.take_prompt().kind == Prompt::SaveAs);
    c.request_add_child(7);
    PendingPrompt p = c.take_prompt();
    CHECK(p.kind == Prompt::AddChild);
    CHECK(p.node == 7);
    c.request_attach_script(9);
    CHECK(c.take_prompt().node == 9);
}

TEST_CASE("controller: tools, toggles, messages and actions") {
    EditorController c;
    CHECK(c.tool() == Tool::Move);
    c.set_tool(Tool::Rotate);
    CHECK(c.tool() == Tool::Rotate);
    CHECK(std::string(tool_name(Tool::Paint)) == "Paint");
    c.set_snap(true);
    CHECK(c.snap());
    c.set_preview_lighting(false);
    CHECK_FALSE(c.preview_lighting());
    c.notify_ok("Saved.");
    CHECK(c.messages().items().size() == 1);
    CHECK_FALSE(c.run_action("no.such.action"));
    const auto [cls, file] = EditorController::suggest_script("player ship");
    CHECK(cls == "Player ship");
    CHECK(file == "scripts/player ship.plm");
    CHECK(EditorController::suggest_script("").first == "CustomNode");
}

TEST_CASE("controller: dropping a non-scene asset on the view is refused and changes nothing") {
    EditorController c;
    prepare(c);
    const std::size_t before = c.doc().model().preorder().size();
    CHECK_FALSE(c.drop_asset_on_scene("art/hero.png", vec2{10.0f, 20.0f}));
    CHECK(c.doc().model().preorder().size() == before);
    CHECK_FALSE(c.doc().dirty());
}

TEST_CASE("controller: a scene dropped on the view is one undoable instance at the rounded point") {
    EditorController c;
    prepare(c);
    REQUIRE(c.drop_asset_on_scene("scenes/part.scene", vec2{1.4f, 2.6f}));
    CHECK(c.doc().model().preorder().size() == 2);
    const NodeId id = c.doc().selection();
    REQUIRE(id != 0);
    CHECK(c.doc().undo());
    CHECK(c.doc().model().preorder().size() == 1);
}

TEST_CASE("controller: import copies a valid image into art/, refuses other files, never overwrites") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "cuervo_ctl_import";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "project.toml") << "name = \"Imp\"\nmain_scene = \"scenes/level.scene\"\nvirtual_size = [640, 360]\ntick_rate = 60\nseed = 7\nclear_color = \"10131c\"\n";
    std::string qoi = "qoif";
    for (int v : {1, 1}) { qoi += std::string{0, 0, 0, static_cast<char>(v)}; }
    qoi += std::string{4, 0};
    qoi += std::string{static_cast<char>(0xFF), 10, 20, 30, static_cast<char>(255)};
    qoi += std::string{0, 0, 0, 0, 0, 0, 0, 1};
    std::ofstream(dir / "dot.qoi", std::ios::binary) << qoi;
    std::ofstream(dir / "notes.txt") << "hello";
    EditorController c;
    std::string error;
    REQUIRE_MESSAGE(c.env().open(dir, nullptr, error), error);
    c.assets().open(dir);
    const auto bad = c.import_files({dir / "notes.txt"});
    CHECK(bad.imported == 0);
    CHECK_FALSE(bad.error.empty());
    const auto ok = c.import_files({dir / "dot.qoi"});
    REQUIRE(ok.imported == 1);
    CHECK(ok.first_dest == "art/dot.qoi");
    CHECK(ok.first_image == "art/dot.qoi");
    CHECK(fs::exists(dir / "art/dot.qoi"));
    const auto again = c.import_files({dir / "dot.qoi"});
    CHECK(again.first_dest == "art/dot_2.qoi");
}

TEST_CASE("controller: starting a replay that does not exist fails and nothing plays") {
    EditorController c;
    prepare(c);
    CHECK_FALSE(c.start_replay("replays/missing.replay"));
    CHECK_FALSE(c.play().active());
}

TEST_CASE("controller: the ready() preview starts 0.3 s after an edit settles and stops when toggled off") {
    EditorController c;
    std::string error;
    REQUIRE_MESSAGE(c.env().open(std::filesystem::path(CUERVO_EXAMPLES_DIR) / "editor_level", nullptr, error), error);
    c.doc().set_registry(&c.env().classes());
    REQUIRE_MESSAGE(c.doc().open(c.env().resolve("scenes/level.scene"), error), error);
    CHECK_FALSE(c.ready_preview_on());
    c.toggle_ready_preview();
    CHECK(c.ready_preview_on());
    c.set_clock(1.0);
    c.update_ready_preview(); // notes the revision
    CHECK_FALSE(c.ready_preview().active());
    c.set_clock(1.2);
    c.update_ready_preview();
    CHECK_FALSE(c.ready_preview().active()); // not settled yet
    c.set_clock(1.4);
    c.update_ready_preview();
    CHECK(c.ready_preview().active());
    c.toggle_ready_preview();
    CHECK_FALSE(c.ready_preview().active());
}

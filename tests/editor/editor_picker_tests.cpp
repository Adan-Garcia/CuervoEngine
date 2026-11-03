// The Add Child picker's catalog (editor/class_catalog.hpp), the layout fields that remember recents and
// favorites, and the controller side of the new project-switch and attach-script flows.

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "editor/class_catalog.hpp"
#include "editor/editor_controller.hpp"
#include "editor/settings.hpp"

using namespace cuervo;
using namespace cuervo::editor;

namespace {

struct Fixture {
    EditorController c;
    Fixture() {
        std::string error;
        REQUIRE_MESSAGE(c.env().open_prelude_only(error), error);
        c.doc().set_registry(&c.env().classes());
        c.doc().new_scene("Node2D", "root");
    }
};

const CatalogGroup* group(const std::vector<CatalogGroup>& g, const char* title) {
    for (const CatalogGroup& x : g) if (x.title == title) return &x;
    return nullptr;
}

const auto no_project = [](const std::string&) { return false; };

} // namespace

TEST_CASE("catalog: classes group by family with descriptions") {
    Fixture f;
    const auto groups = build_catalog(f.c.env().classes(), "", {}, no_project);
    const CatalogGroup* ui = group(groups, "UI");
    const CatalogGroup* phys = group(groups, "Physics");
    const CatalogGroup* draw = group(groups, "Drawing");
    REQUIRE((ui && phys && draw));
    CHECK(std::any_of(ui->items.begin(), ui->items.end(), [](const CatalogEntry& e) { return e.name == "Button"; }));
    CHECK(std::any_of(phys->items.begin(), phys->items.end(), [](const CatalogEntry& e) { return e.name == "RigidBody2D"; }));
    CHECK(std::any_of(draw->items.begin(), draw->items.end(), [](const CatalogEntry& e) { return e.name == "Sprite2D"; }));
    for (const CatalogGroup& g : groups) {
        CHECK(std::is_sorted(g.items.begin(), g.items.end(), [](const CatalogEntry& a, const CatalogEntry& b) { return a.name < b.name; }));
        for (const CatalogEntry& e : g.items) CHECK_FALSE(e.description.empty());
    }
}

TEST_CASE("catalog: fuzzy search ranks the best match first and reports matched characters") {
    Fixture f;
    const auto groups = build_catalog(f.c.env().classes(), "rigid", {}, no_project);
    const auto flat = catalog_flat(groups);
    REQUIRE_FALSE(flat.empty());
    CHECK(flat.front()->name == "RigidBody2D");
    CHECK_FALSE(flat.front()->positions.empty());
    CHECK(group(groups, "Favorites") == nullptr);
    CHECK(catalog_flat(build_catalog(f.c.env().classes(), "zzzqqq", {}, no_project)).empty());
}

TEST_CASE("catalog: favorites and recents lead, and survive the layout text") {
    Fixture f;
    CatalogUser u;
    u.toggle_favorite("Camera2D");
    u.note_used("Sprite2D");
    u.note_used("Button");
    u.note_used("Sprite2D");
    CHECK(u.recents == std::vector<std::string>{"Sprite2D", "Button"});
    const CatalogUser round = CatalogUser::parse(u.recents_text(), u.favorites_text());
    CHECK(round.recents == u.recents);
    CHECK(round.is_favorite("Camera2D"));
    const auto groups = build_catalog(f.c.env().classes(), "", round, no_project);
    REQUIRE(groups.size() > 2);
    CHECK(groups[0].title == "Favorites");
    CHECK(groups[1].title == "Recent");
    CHECK(groups[1].items.front().name == "Sprite2D");
    // Starred stays starred inside its family too.
    const CatalogGroup* core = group(groups, "Core");
    REQUIRE(core != nullptr);
    CHECK(std::any_of(core->items.begin(), core->items.end(), [](const CatalogEntry& e) { return e.name == "Camera2D" && e.favorite; }));
    u.toggle_favorite("Camera2D");
    CHECK_FALSE(u.is_favorite("Camera2D"));
    for (int i = 0; i < 10; ++i) u.note_used("N" + std::to_string(i));
    CHECK(u.recents.size() == 6);
}

TEST_CASE("catalog: recents and favorites persist in [rml_layout]") {
    EditorSettings s;
    s.rml_layout().add_recents = "Sprite2D;Button";
    s.rml_layout().add_favorites = "Camera2D";
    EditorSettings back;
    back.parse(s.serialize());
    CHECK(back.rml_layout().add_recents == "Sprite2D;Button");
    CHECK(back.rml_layout().add_favorites == "Camera2D");
}

TEST_CASE("controller: a switch request can be acknowledged and the prompt kinds are queued") {
    Fixture f;
    f.c.request_switch_project("/nowhere");
    CHECK(f.c.switch_requested());
    CHECK(f.c.switch_target() == "/nowhere");
    f.c.acknowledge_switch();
    CHECK_FALSE(f.c.switch_requested());
    f.c.request_add_child(f.c.doc().model().root());
    CHECK(f.c.take_prompt().kind == Prompt::AddChild);
    f.c.request_attach_script(f.c.doc().model().root());
    CHECK(f.c.take_prompt().kind == Prompt::AttachScript);
}

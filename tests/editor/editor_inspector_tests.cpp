// The RmlUi inspector's rules (editor/inspector_model.hpp): sections, search through folded sections,
// number scrubbing and parsing, nested paths, colors, and that edits are the document's undoable commands.

#include <doctest.h>

#include <algorithm>
#include <string>

#include "editor/editor_controller.hpp"
#include "editor/inspector_model.hpp"

using namespace cuervo;
using namespace cuervo::editor;
using scene::NumKind;
using scene::Value;

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

bool has(const ve::Section& s, const char* f) { return std::find(s.fields.begin(), s.fields.end(), f) != s.fields.end(); }

} // namespace

TEST_CASE("inspector: fields group into Transform, own class, ancestors and a closed Advanced") {
    Fixture f;
    const auto sections = ve::group_fields(f.c.env().classes(), "Node2D", {});
    REQUIRE(sections.size() >= 3);
    CHECK(sections[0].key == "Transform");
    CHECK(sections[0].fields == std::vector<std::string>{"position", "rotation", "scale"});
    CHECK(sections[1].key == "Node2D");
    CHECK(has(sections[1], "visible"));
    CHECK(sections[1].default_open);
    const ve::Section& adv = sections.back();
    CHECK(adv.key == "Advanced");
    CHECK_FALSE(adv.default_open);
    CHECK(has(adv, "always_process")); // Node's field
    // Every declared field appears exactly once.
    std::size_t total = 0;
    for (const auto& s : sections) total += s.fields.size();
    CHECK(total == f.c.env().classes().find("Node2D")->fields.size());
    const auto extra = ve::group_fields(f.c.env().classes(), "Node2D", {"mystery", "position"});
    CHECK(extra.back().key == "Stored");
    CHECK(extra.back().fields == std::vector<std::string>{"mystery"});
}

TEST_CASE("inspector: a folded section is still reachable through the filter") {
    Fixture f;
    const auto sections = ve::group_fields(f.c.env().classes(), "Node2D", {});
    const auto hit = ve::filter_sections(sections, "always_pr");
    REQUIRE(hit.size() == 1);
    CHECK(hit[0].key == "Advanced");
    CHECK(hit[0].fields == std::vector<std::string>{"always_process"});
    CHECK(ve::filter_sections(sections, "").size() == sections.size());
    CHECK(ve::filter_sections(sections, "zzzzqq").empty());
}

TEST_CASE("inspector: collapsed state is a list with overrides, defaults included") {
    const ve::Section open{"Turret", "Turret", {}, true}, closed{"Advanced", "Advanced", {}, false};
    CHECK(ve::section_open("", open));
    CHECK_FALSE(ve::section_open("", closed));
    std::string l = ve::toggle_section("", open);
    CHECK(l == "Turret");
    CHECK_FALSE(ve::section_open(l, open));
    CHECK(ve::toggle_section(l, open).empty());
    l = ve::toggle_section("Turret", closed);
    CHECK(l == "Turret;+Advanced");
    CHECK(ve::section_open(l, closed));
    CHECK(ve::toggle_section(l, closed) == "Turret");
    CHECK_FALSE(ve::section_open("Advanced;Signals", closed)); // an older plain token still means closed
}

TEST_CASE("inspector: numbers format with a decimal point and parse with clamping") {
    CHECK(ve::format_number(Value::number(-148.0)) == "-148.0");
    CHECK(ve::format_number(Value::number(6.2)) == "6.2");
    CHECK(ve::format_number(Value::integer(14)) == "14");
    Value i = Value::integer(0, NumKind::U8);
    CHECK(ve::parse_number("300", i));
    CHECK(i.i == 255);
    CHECK(ve::parse_number("-5", i));
    CHECK(i.i == 0);
    CHECK_FALSE(ve::parse_number("abc", i));
    CHECK_FALSE(ve::parse_number("", i));
    Value f = Value::number(0.0);
    CHECK(ve::parse_number(" 1.5 ", f));
    CHECK(f.f == doctest::Approx(1.5));
    CHECK_FALSE(ve::parse_number("1.5x", f));
}

TEST_CASE("inspector: scrub speed uses Shift x10 and Alt x0.01, ints round and clamp") {
    const Value f = Value::number(0.0);
    CHECK(ve::scrub_speed(f, false, false) == doctest::Approx(0.1));
    CHECK(ve::scrub_speed(f, true, false) == doctest::Approx(1.0));
    CHECK(ve::scrub_speed(f, false, true) == doctest::Approx(0.001));
    Value v = f;
    ve::apply_scrub(v, f, 2.5);
    CHECK(v.f == doctest::Approx(2.5));
    Value i = Value::integer(250, NumKind::U8), vi = i;
    ve::apply_scrub(vi, i, 30.4);
    CHECK(vi.i == 255);
    ve::apply_scrub(vi, i, -300.0);
    CHECK(vi.i == 0);
}

TEST_CASE("inspector: nested values are addressed by path and colors round trip through hex") {
    Value list = Value::list({Value::number(1.0), Value::number(2.0)});
    REQUIRE(ve::at_path(list, "1") != nullptr);
    CHECK(ve::at_path(list, "1")->f == 2.0);
    CHECK(ve::at_path(list, "2") == nullptr);
    CHECK(ve::at_path(list, "") == &list);
    Value color;
    color.kind = scene::ValueKind::Struct;
    color.type = "Color";
    for (const char* n : {"r", "g", "b", "a"}) color.fields.push_back({n, "", Value::number(1.0)});
    CHECK(ve::is_color(color));
    CHECK(ve::parse_color_hex("#E8E45A", color));
    CHECK(ve::color_hex(color) == "#E8E45A");
    CHECK(ve::parse_color_hex("11223380", color));
    CHECK(ve::color_hex(color) == "#11223380");
    CHECK_FALSE(ve::parse_color_hex("#12", color));
    CHECK_FALSE(ve::parse_color_hex("#GG0000", color));
    CHECK(ve::at_path(color, "g") == &color.fields[1].value);
}

TEST_CASE("color picker: HSV round trip, primaries, alpha, and the hue swatch") {
    Value c;
    c.kind = scene::ValueKind::Struct;
    c.type = "Color";
    for (const char* n : {"r", "g", "b", "a"}) c.fields.push_back({n, "", Value::number(1.0)});
    ve::set_hsva(c, {0.0f, 1.0f, 1.0f, 0.5f});
    CHECK(ve::color_hex(c) == "#FF000080");
    ve::set_hsva(c, {1.0f / 3.0f, 1.0f, 1.0f, 1.0f});
    CHECK(ve::color_hex(c) == "#00FF00");
    ve::set_hsva(c, {2.0f / 3.0f, 1.0f, 0.5f, 1.0f});
    CHECK(ve::color_hex(c) == "#000080");
    const ve::Hsva back = ve::hsva_of(c);
    CHECK(back.h == doctest::Approx(2.0f / 3.0f).epsilon(0.01));
    CHECK(back.s == doctest::Approx(1.0f));
    CHECK(back.v == doctest::Approx(0.5f).epsilon(0.01));
    ve::set_hsva(c, {0.3f, 0.0f, 0.0f, 1.0f}); // black keeps no hue in RGB
    CHECK(ve::color_hex(c) == "#000000");
    CHECK(ve::hsva_of(c).s == 0.0f);
    CHECK(ve::hue_css(0.0f) == "rgb(255, 0, 0)");
}

TEST_CASE("inspector: an edit through the document is one undo step, a drag merges, reset removes it") {
    Fixture f;
    EditorDocument& d = f.c.doc();
    const NodeId n = d.add_child(d.model().root(), "Node2D", "n");
    d.seal();
    const std::size_t steps = d.stack().size();
    bool stored = true;
    Value pos = d.field_value(n, "position", &stored);
    CHECK_FALSE(stored);
    REQUIRE(pos.kind == scene::ValueKind::Vec);
    for (int i = 1; i <= 5; ++i) { // a scrub drag: merged into one step
        Value* x = &pos;
        x->vec[0] = static_cast<float>(i);
        CHECK(d.set_field(n, "position", pos, true));
    }
    d.seal();
    CHECK(d.stack().size() == steps + 1);
    CHECK(d.field_value(n, "position", &stored).vec[0] == 5.0f);
    CHECK(stored);
    CHECK(d.reset_field(n, "position"));
    d.field_value(n, "position", &stored);
    CHECK_FALSE(stored);
    CHECK(d.undo());
    CHECK(d.field_value(n, "position", &stored).vec[0] == 5.0f);
    CHECK(d.undo());
    CHECK(d.field_value(n, "position", &stored).vec[0] == 0.0f);
}

TEST_CASE("drawer tab follows the node class, scripts fall back to the Script tab") {
    using cuervo::editor::ve::drawer_tab_for;
    CHECK(drawer_tab_for("Emitter2D", false) == "Particles");
    CHECK(drawer_tab_for("AnimatedSprite2D", false) == "Animation");
    CHECK(drawer_tab_for("AnimationPlayer", false) == "Animation");
    CHECK(drawer_tab_for("Sprite2D", true) == "Sprite");
    CHECK(drawer_tab_for("TileMapLayer", false) == "Tiles");
    CHECK(drawer_tab_for("Turret", true) == "Script");
    CHECK(drawer_tab_for("Turret", false).empty());
}

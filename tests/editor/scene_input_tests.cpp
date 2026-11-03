// The scene region's painter rectangle handling and the input routing between the RmlUi shell and
// the scene view (DESIGN.md 4.13): no window, no SDL.

#include <doctest.h>

#include <string>

#include "editor/editor_controller.hpp"
#include "editor/scene_input.hpp"
#include "editor/scene_painter.hpp"

using namespace cuervo;
using namespace cuervo::editor;

TEST_CASE("scene painter: rectangles clamp to the framebuffer") {
    CHECK(clamp_rect({10, 20, 100, 50}, 1600, 900) == rhi::Rect{10, 20, 100, 50});
    CHECK(clamp_rect({-20, -10, 100, 50}, 1600, 900) == rhi::Rect{0, 0, 80, 40});
    CHECK(clamp_rect({1550, 880, 100, 100}, 1600, 900) == rhi::Rect{1550, 880, 50, 20});
    CHECK(clamp_rect({2000, 100, 50, 50}, 1600, 900).width == 0); // wholly outside: empty, never negative
    CHECK(clamp_rect({100, 100, -5, 10}, 1600, 900).width == 0);
}

TEST_CASE("focus router: a gesture belongs to where it started") {
    FocusRouter r;
    r.set_scene_rect(100, 100, 400, 300);
    CHECK(r.pointer({200, 200}) == Route::Scene);
    CHECK(r.pointer({50, 200}) == Route::Ui);
    // Press in the scene, drag out over the UI: still the scene's (camera pan does not leak).
    CHECK(r.press({200, 200}, 3) == Route::Scene);
    CHECK(r.pointer({20, 20}) == Route::Scene);
    r.release(3);
    CHECK(r.pointer({20, 20}) == Route::Ui);
    // Press in the UI (a splitter drag), move over the scene: the UI keeps it.
    CHECK(r.press({50, 200}, 1) == Route::Ui);
    CHECK(r.pointer({200, 200}) == Route::Ui);
    r.release(1);
    CHECK(r.pointer({200, 200}) == Route::Scene);
}

TEST_CASE("focus router: keys follow the last press; text fields and popups win") {
    FocusRouter r;
    r.set_scene_rect(100, 100, 400, 300);
    CHECK(r.key() == Route::Ui);
    r.press({200, 200}, 1);
    r.release(1);
    CHECK(r.key() == Route::Scene);
    r.set_text_editing(true);
    CHECK(r.key() == Route::Ui);
    r.set_text_editing(false);
    r.set_ui_modal(true); // an open menu takes the pointer and the keys
    CHECK(r.pointer({200, 200}) == Route::Ui);
    CHECK(r.key() == Route::Ui);
    r.set_ui_modal(false);
    r.press({20, 20}, 1);
    r.release(1);
    CHECK(r.key() == Route::Ui);
}

namespace {

struct Fixture {
    EditorController ctl;
    ViewCamera cam;
    NodeId a = 0;
    Fixture() {
        std::string error;
        REQUIRE_MESSAGE(ctl.env().open_prelude_only(error), error);
        ctl.doc().set_registry(&ctl.env().classes());
        ctl.doc().new_scene("Node2D", "root");
        a = ctl.doc().add_child(ctl.doc().model().root(), "Node2D", "a");
        REQUIRE(a != 0);
            ctl.env().rebuild_preview(ctl.doc().model().to_doc());
        cam.center = {0.0f, 0.0f};
        cam.zoom = 1.0f;
    }
};

} // namespace

TEST_CASE("scene input: wheel zooms about the pointer, middle drag pans") {
    Fixture f;
    SceneInput in(f.ctl, f.cam);
    in.set_size({800, 600});
    const vec2 before = f.cam.screen_to_world({600, 100}, {0, 0}, {800, 600});
    in.wheel(2.0f, {600, 100});
    CHECK(f.cam.zoom > 1.0f);
    const vec2 after = f.cam.screen_to_world({600, 100}, {0, 0}, {800, 600});
    CHECK(after.x() == doctest::Approx(before.x()).epsilon(0.001));
    CHECK(after.y() == doctest::Approx(before.y()).epsilon(0.001));
    // One notch is 1.2x; a burst arriving as one huge event is capped at 3 notches.
    f.cam.zoom = 1.0f;
    in.wheel(1.0f, {600, 100});
    CHECK(f.cam.zoom == doctest::Approx(1.2f));
    f.cam.zoom = 1.0f;
    in.wheel(150.0f, {600, 100});
    CHECK(f.cam.zoom == doctest::Approx(1.728f).epsilon(0.001));
    const vec2 c0 = f.cam.center;
    in.down(1, {400, 300}, false);
    in.move({440, 300});
    in.up(1, {440, 300});
    CHECK(f.cam.center.x() < c0.x()); // dragging right moves the world right, the camera left
}

TEST_CASE("scene input: a left click on empty space clears, on a node selects") {
    Fixture f;
    SceneInput in(f.ctl, f.cam);
    in.set_size({800, 600});
    f.ctl.set_tool(Tool::Select);
    const vec2 origin = f.cam.world_to_screen({0, 0}, {0, 0}, {800, 600});
    in.down(0, origin, false); // the Node2D `a` sits at the world origin
    in.up(0, origin);
    CHECK(f.ctl.doc().selection() == f.a);
    in.down(0, origin + vec2{300.0f, 200.0f}, false);
    in.up(0, origin + vec2{300.0f, 200.0f});
    CHECK(f.ctl.doc().selection() == 0);
}

TEST_CASE("scene input: dragging a selected node is one undo step") {
    Fixture f;
    SceneInput in(f.ctl, f.cam);
    in.set_size({800, 600});
    f.ctl.set_tool(Tool::Move);
    f.ctl.set_snap(false);
    f.ctl.doc().select(f.a);
    const vec2 origin = f.cam.world_to_screen({0, 0}, {0, 0}, {800, 600});
    const std::uint64_t rev = f.ctl.doc().revision();
    in.down(0, origin, false); // the gizmo's free square
    in.move(origin + vec2{10.0f, 0.0f});
    in.move(origin + vec2{30.0f, 12.0f});
    in.up(0, origin + vec2{30.0f, 12.0f});
    CHECK(f.ctl.doc().revision() != rev);
    CHECK(f.ctl.doc().selection_pivot()->x() == doctest::Approx(30.0f));
    CHECK(f.ctl.doc().undo()); // the whole drag, not one step per move
    CHECK(f.ctl.doc().selection_pivot()->x() == doctest::Approx(0.0f));
    CHECK(f.ctl.doc().selection_pivot()->y() == doctest::Approx(0.0f));
}

namespace {

scene::Value field_of(Fixture& f, NodeId id, const char* name) { return f.ctl.doc().field_value(id, name); }

} // namespace

TEST_CASE("scene input: the rotate gizmo turns the selection by the dragged angle, one undo step") {
    Fixture f;
    SceneInput in(f.ctl, f.cam);
    in.set_size({800, 600});
    f.ctl.set_tool(Tool::Rotate);
    f.ctl.set_snap(true);
    f.ctl.doc().select(f.a);
    const vec2 o = f.cam.world_to_screen({0, 0}, {0, 0}, {800, 600});
    REQUIRE(in.gizmo_pivot().has_value());
    // Grab the ring on its right side, sweep a quarter turn clockwise on screen (down).
    in.down(0, o + vec2{SceneInput::ring, 0.0f}, false);
    CHECK(in.hot() == Handle::Ring);
    in.move(o + vec2{SceneInput::ring * 0.7f, SceneInput::ring * 0.7f});
    in.move(o + vec2{0.0f, SceneInput::ring});
    CHECK(in.readout() == "+90.0 deg");
    in.up(0, o + vec2{0.0f, SceneInput::ring});
    CHECK(field_of(f, f.a, "rotation").f == doctest::Approx(1.5708f).epsilon(0.001));
    CHECK(f.ctl.doc().undo());
    CHECK(field_of(f, f.a, "rotation").f == doctest::Approx(0.0f));
}

TEST_CASE("scene input: the scale gizmo scales along an axis or both, Ctrl inverts snapping") {
    Fixture f;
    SceneInput in(f.ctl, f.cam);
    in.set_size({800, 600});
    f.ctl.set_tool(Tool::Scale);
    f.ctl.set_snap(true);
    f.ctl.doc().select(f.a);
    const vec2 o = f.cam.world_to_screen({0, 0}, {0, 0}, {800, 600});
    in.down(0, o + vec2{SceneInput::arm, 0.0f}, false);
    CHECK(in.hot() == Handle::ScaleX);
    in.move(o + vec2{SceneInput::arm * 2.0f, 0.0f});
    CHECK(in.readout() == "x2.00");
    in.up(0, o + vec2{SceneInput::arm * 2.0f, 0.0f});
    const scene::Value s = field_of(f, f.a, "scale");
    REQUIRE(s.kind == scene::ValueKind::Vec);
    CHECK(s.vec[0] == doctest::Approx(2.0f));
    CHECK(s.vec[1] == doctest::Approx(1.0f)); // x only
    // The centre box scales both axes; with snap on 1.23 rounds to 1.2, Ctrl flips it to 1.23.
    const vec2 grab = o;
    in.set_mods(true, false);
    in.down(0, grab, false);
    CHECK(in.hot() == Handle::ScaleBoth);
    in.move(grab + vec2{8.0f * 1.23f, 0.0f});
    CHECK(in.readout() == "x1.23");
    in.up(0, grab);
}

TEST_CASE("scene input: rotate and scale gizmos hide for a selection that cannot take them") {
    Fixture f;
    SceneInput in(f.ctl, f.cam);
    in.set_size({800, 600});
    f.ctl.set_tool(Tool::Rotate);
    f.ctl.doc().select_clear();
    CHECK_FALSE(in.gizmo_pivot().has_value()); // nothing selected
    f.ctl.doc().select(f.a);
    CHECK(in.gizmo_pivot().has_value());
    f.ctl.set_tool(Tool::Select);
    CHECK_FALSE(in.gizmo_pivot().has_value());
}

TEST_CASE("scene input: dragging on empty space rubber-bands a selection, Shift adds") {
    Fixture f;
    const NodeId b = f.ctl.doc().add_child(f.ctl.doc().model().root(), "Node2D", "b");
    REQUIRE(b != 0);
    f.ctl.doc().set_field(b, "position", scene::Value::vector({200.0f, 0.0f}));
    f.ctl.doc().seal();
    f.ctl.env().rebuild_preview(f.ctl.doc().model().to_doc());
    SceneInput in(f.ctl, f.cam);
    in.set_size({800, 600});
    f.ctl.set_tool(Tool::Select);
    f.ctl.doc().select_clear();
    const vec2 o = f.cam.world_to_screen({0, 0}, {0, 0}, {800, 600});
    in.down(0, o + vec2{-40.0f, 60.0f}, false);
    in.move(o + vec2{-20.0f, 40.0f});
    in.move(o + vec2{60.0f, -60.0f}); // covers `a` at the origin, not `b` at x=200
    in.up(0, o + vec2{60.0f, -60.0f});
    CHECK(f.ctl.doc().selection() == f.a);
    CHECK(f.ctl.doc().selected().size() == 1);
    // Additive: a second box over `b` keeps `a`.
    in.down(0, o + vec2{160.0f, 60.0f}, true);
    in.move(o + vec2{240.0f, -60.0f});
    in.up(0, o + vec2{240.0f, -60.0f});
    CHECK(f.ctl.doc().selected().size() == 2);
    // A box that is just a click (under 3 px) does not select a node range; empty click clears.
    in.down(0, o + vec2{0.0f, 250.0f}, false);
    in.move(o + vec2{1.0f, 251.0f});
    in.up(0, o + vec2{1.0f, 251.0f});
    CHECK(f.ctl.doc().selected().empty());
}

TEST_CASE("scene input: painting a tilemap layer is one undo step per stroke; right button erases") {
    Fixture f;
    const NodeId layer = f.ctl.doc().add_child(f.ctl.doc().model().root(), "TileMapLayer", "ground");
    REQUIRE(layer != 0);
    f.ctl.doc().seal();
    f.ctl.env().rebuild_preview(f.ctl.doc().model().to_doc());
    f.ctl.doc().select(layer);
    f.ctl.set_tool(Tool::Paint);
    f.ctl.set_paint_tile_id(3);
    SceneInput in(f.ctl, f.cam);
    in.set_size({800, 600});
    const vec2 o = f.cam.world_to_screen({0, 0}, {0, 0}, {800, 600});
    const auto count = [&] { return field_of(f, layer, "cells").items.size(); };
    in.down(0, o + vec2{5.0f, 5.0f}, false); // cell (0, 0) at the default 32 px tiles
    in.move(o + vec2{40.0f, 5.0f});          // (1, 0)
    in.move(o + vec2{70.0f, 40.0f});         // (2, 1)
    in.up(0, o + vec2{70.0f, 40.0f});
    CHECK(count() == 3);
    CHECK(f.ctl.doc().undo()); // the whole stroke
    CHECK(count() == 0);
    CHECK(f.ctl.doc().redo());
    CHECK(count() == 3);
    in.down(2, o + vec2{5.0f, 5.0f}, false); // right button erases (0, 0)
    in.up(2, o + vec2{5.0f, 5.0f});
    CHECK(count() == 2);
    // Painting the same tile again is not a change; the cell's tile id is what was selected.
    in.down(0, o + vec2{40.0f, 5.0f}, false);
    in.up(0, o + vec2{40.0f, 5.0f});
    CHECK(count() == 2);
    // Painting is a no-op when the selection is not a TileMapLayer.
    f.ctl.doc().select(f.a);
    in.down(0, o + vec2{500.0f, 5.0f}, false);
    in.up(0, o + vec2{500.0f, 5.0f});
    CHECK(count() == 2);
}

TEST_CASE("turn and scale drags: shared maths") {
    TurnDrag t;
    t.begin({10.0f, 0.0f}, {0.0f, 0.0f});
    CHECK(t.update({0.0f, 10.0f}, {0.0f, 0.0f}, true, false) == doctest::Approx(1.5708f).epsilon(0.001));
    CHECK(t.update({0.0f, 10.0f}, {0.0f, 0.0f}, true, false) == 0.0f); // no movement, no step
    // Crossing the +-pi seam keeps turning the same way instead of jumping 2 pi.
    TurnDrag s;
    s.begin({-10.0f, -1.0f}, {0.0f, 0.0f});
    s.update({-10.0f, 1.0f}, {0.0f, 0.0f}, false, true);
    CHECK(std::abs(s.total) < 0.3f);
    ScaleDrag d;
    d.begin(Handle::ScaleBoth, {20.0f, 0.0f}, {0.0f, 0.0f});
    const auto r = d.update(Handle::ScaleBoth, {40.0f, 0.0f}, {0.0f, 0.0f}, true);
    REQUIRE(r.has_value());
    CHECK(r->x() == doctest::Approx(2.0f));
    CHECK_FALSE(d.update(Handle::ScaleBoth, {40.0f, 0.0f}, {0.0f, 0.0f}, true).has_value());
    // Never below 0.05.
    CHECK(d.update(Handle::ScaleBoth, {0.5f, 0.0f}, {0.0f, 0.0f}, false).has_value());
    CHECK(d.applied == doctest::Approx(0.05f));
}

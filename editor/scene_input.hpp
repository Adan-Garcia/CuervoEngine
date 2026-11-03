#pragma once
// Pointer and key routing between the RmlUi shell and the scene region, and the scene view's own
// navigation/selection/move-gizmo input (UI-free; the ImGui panel keeps its own, richer version).

#include <optional>
#include <string>
#include <vector>

#include "cuervo/brand/brand.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "editor/editor_controller.hpp"
#include "editor/view_camera.hpp"

namespace cuervo::editor {

enum class Route { Ui, Scene };

// Decides who gets an input event. A press is routed by where it lands and then captures the pointer
// until every button is released, so a drag that starts in the scene never leaks into the UI and one
// that starts in the UI never moves the camera. Keys follow the last press. Open popups/menus
// (`set_ui_modal`) take the pointer over the scene.
class FocusRouter {
public:
    void set_scene_rect(float x, float y, float w, float h) { x_ = x; y_ = y; w_ = w; h_ = h; }
    void set_ui_modal(bool on) { modal_ = on; }
    void set_text_editing(bool on) { text_ = on; }
    vec2 origin() const { return {x_, y_}; }
    bool inside(vec2 p) const { return p.x() >= x_ && p.y() >= y_ && p.x() < x_ + w_ && p.y() < y_ + h_; }
    // Where a pointer event at `p` goes right now (move, wheel, hover).
    Route pointer(vec2 p) const;
    Route press(vec2 p, int button);
    void release(int button);
    // Keyboard shortcuts for the scene (tool keys, delete, frame) go to it only when it holds focus
    // and no text field is being edited.
    Route key() const { return (scene_focus_ && !text_ && !modal_) ? Route::Scene : Route::Ui; }
    bool scene_focus() const { return scene_focus_; }
    bool captured() const { return buttons_ != 0; }

private:
    float x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool modal_ = false, text_ = false, scene_focus_ = false;
    int buttons_ = 0;
    Route capture_ = Route::Ui;
};

enum class Handle { None, MoveX, MoveY, MoveFree, Ring, ScaleX, ScaleY, ScaleBoth, Marquee };

// ---- scene-view logic shared by the RmlUi SceneInput and the ImGui scene view panel (UI-free).

// Paints (tile_id) or erases the tile at a cell of a TileMapLayer's `cells`; each change is merged
// into the current undo step (seal it when the stroke ends).
void paint_tile_cell(EditorDocument& doc, NodeId id, int cx, int cy, int tile_id, bool erase);

// The cell of the selected TileMapLayer under a world point.
struct TileCellRef {
    vec2 tile{32.0f, 32.0f};
    vec2 origin{0.0f, 0.0f};
    int x = 0, y = 0;
};
TileCellRef tile_cell_at(const EditorDocument& doc, NodeId layer, const std::vector<NodeId>& order,
                         const std::vector<NodeBounds>& bounds, vec2 world);

// Rubber-band selection: every node whose rect (or position) touches the world rectangle spanned by
// `a` and `b`; `additive` keeps the current selection.
void marquee_select(EditorDocument& doc, const std::vector<NodeId>& order, const std::vector<NodeBounds>& bounds, vec2 a,
                    vec2 b, bool additive);

// Rotate gizmo drag: tracks the turned angle around a screen-space pivot. update() returns the
// angle to apply now (0: nothing changed). Snap rounds to 15 degrees, otherwise to 0.001 rad unless `free`.
struct TurnDrag {
    float last = 0.0f, total = 0.0f, applied = 0.0f;
    void begin(vec2 mouse, vec2 pivot);
    float update(vec2 mouse, vec2 pivot, bool snap, bool free);
    float start_angle() const { return last - total; }
};

// Scale gizmo drag: the factor follows the pointer's distance from the pivot (both axes) or along
// one axis, clamped to 0.05..20 and rounded (0.1 with snap, else 0.01). update() returns the
// per-axis multiplier to apply now.
struct ScaleDrag {
    float start_distance = 8.0f, start_axis = 8.0f, applied = 1.0f;
    void begin(Handle h, vec2 mouse, vec2 pivot);
    std::optional<vec2> update(Handle h, vec2 mouse, vec2 pivot, bool snap);
};

// Camera pan (middle/right drag), zoom (wheel, about the pointer), click selection (Ctrl/Shift toggles),
// rubber-band select, the Move/Rotate/Scale gizmos (one undo step per drag) and tilemap painting
// (left paints, right erases, one undo step per stroke). Positions are in pixels relative to the scene
// region's top-left. Ctrl inverts snapping and Shift drops rounding while dragging (set_mods).
class SceneInput {
public:
    SceneInput(EditorController& ctl, ViewCamera& camera) : ctl_(ctl), cam_(camera) {}
    void set_size(vec2 size) { size_ = size; }
    void set_mods(bool ctrl, bool shift) { ctrl_ = ctrl; shift_ = shift; }
    void down(int button, vec2 p, bool additive);
    void move(vec2 p);
    void up(int button, vec2 p);
    void wheel(float dy, vec2 p);
    void frame_all();

    // Overlay state for the painter.
    std::optional<vec2> gizmo_pivot() const;  // world; set when a gizmo shows (Move, or Rotate/Scale on a node that can take them)
    // Text for the view's readout while a rotate/scale drag runs ("+15.0 deg", "x1.20"); empty otherwise.
    std::string readout() const;
    Handle hot() const { return drag_ != Handle::None ? drag_ : hover_; }
    vec2 world_at(vec2 p) const { return to_world(p); } // view-local pixels to world
    vec2 to_screen(vec2 world) const { return cam_.world_to_screen(world, {0, 0}, size_); }
    static constexpr float arm = 56.0f;       // gizmo arm length, px
    static constexpr float ring = 46.0f;      // rotate ring radius, px
    // Selection outlines and the Move gizmo, in world units (upp: world units per pixel).
    void draw_overlay(render::SdfBatcher& shapes, const brand::ThemeColors& colors, float upp) const;

private:
    Handle hit(vec2 p) const;
    std::optional<vec2> pivot_now() const;    // selection pivot; controls only: the middle of their rects
    bool can_turn() const;                    // a target that takes rotation/scale (a node with a position, no rect)
    bool snap_now() const { return ctl_.snap() != ctrl_; }
    void paint_at(vec2 p, bool erase);
    vec2 screen_pivot() const { return to_screen(pivot_); }
    vec2 to_world(vec2 p) const { return cam_.screen_to_world(p, {0, 0}, size_); }
    EditorController& ctl_;
    ViewCamera& cam_;
    vec2 size_{1, 1};
    Handle drag_ = Handle::None, hover_ = Handle::None;
    bool panning_ = false, ctrl_ = false, shift_ = false;
    bool painting_ = false, erasing_ = false, marquee_moved_ = false, marquee_additive_ = false;
    vec2 last_{0, 0}, start_{0, 0}, applied_{0, 0}, pivot_{0, 0}, marquee_a_{0, 0};
    TurnDrag turn_;
    ScaleDrag scale_;
};

} // namespace cuervo::editor

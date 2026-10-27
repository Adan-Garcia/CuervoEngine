// The scene view (center dock): the open scene drawn by the engine's own
// renderer (EditorApp::render_scene_view) into this panel's rectangle, with a grid,
// the game screen's frame, the selection and the transform gizmos on top.
//
//   wheel                 zoom at the cursor (vector-aware, view_camera.hpp)
//   right / middle drag   pan
//   left click            select the node under the cursor; Ctrl or Shift: add/remove it
//   left drag on empty    rubber-band select (Ctrl or Shift adds)
//   Q W E R               tool: select, move, rotate, scale (the toolbar has them too)
//   move tool             drag the x arrow, the y arrow or the square (free), or a selected node
//   rotate tool           drag the ring around the pivot (the selection's center)
//   scale tool            drag an axis box, or the center square for both axes
//   Ctrl while dragging   inverts snapping (grid, 15 degrees, steps of 0.1); Shift: no rounding
//   F                     frame the game screen
//   drop a .scene from the asset browser   instance it (SceneInstance) there
//
// Every gizmo acts on the whole selection as one undo step (EditorDocument's group
// transforms, DESIGN.md 4.16 "Clarified (Milestone 8.3)"). While playing (F5/F6)
// the view shows the running game letterboxed at its virtual resolution; click the
// view to give the game the keyboard, mouse and gamepad (the main toolbar holds the
// play controls).

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "cuervo/core/log.hpp"
#include "editor/app.hpp"
#include "editor/commands.hpp"
#include "editor/panels.hpp"
#include "editor/theme.hpp"
#include "editor/ui.hpp"

namespace cuervo::editor {

namespace {

constexpr float arm_length = 76.0f;  // pixels, the move and scale arms
constexpr float ring_radius = 60.0f; // pixels, the rotate ring
constexpr float pick_slop = 7.0f;
constexpr float pi = 3.14159265f;

void paint_cell(EditorDocument& doc, NodeId id, int cx, int cy, int tile_id, bool erase) {
    bool stored = false;
    scene::Value before = doc.field_value(id, "cells", &stored);
    scene::Value after = before;
    if (after.kind != scene::ValueKind::List) {
        after.kind = scene::ValueKind::List;
        after.items.clear();
    }
    int found_idx = -1;
    for (std::size_t i = 0; i < after.items.size(); ++i) {
        const auto& item = after.items[i];
        if (item.kind == scene::ValueKind::Struct) {
            int ix = 0, iy = 0;
            for (const auto& f : item.fields) {
                if (f.name == "x" && f.value.kind == scene::ValueKind::Int) ix = static_cast<int>(f.value.i);
                if (f.name == "y" && f.value.kind == scene::ValueKind::Int) iy = static_cast<int>(f.value.i);
            }
            if (ix == cx && iy == cy) {
                found_idx = static_cast<int>(i);
                break;
            }
        }
    }
    if (erase) {
        if (found_idx >= 0) {
            after.items.erase(after.items.begin() + found_idx);
            doc.set_field(id, "cells", after, true);
        }
    } else {
        scene::Value cell_val;
        cell_val.kind = scene::ValueKind::Struct;
        cell_val.type = "TileCell";
        cell_val.fields.push_back({"x", {}, scene::Value::integer(cx)});
        cell_val.fields.push_back({"y", {}, scene::Value::integer(cy)});
        cell_val.fields.push_back({"tile_id", {}, scene::Value::integer(tile_id)});
        cell_val.fields.push_back({"terrain", {}, scene::Value::integer(-1)});

        if (found_idx >= 0) {
            int cur_tid = -1;
            for (const auto& f : after.items[static_cast<std::size_t>(found_idx)].fields) {
                if (f.name == "tile_id" && f.value.kind == scene::ValueKind::Int) cur_tid = static_cast<int>(f.value.i);
            }
            if (cur_tid != tile_id) {
                after.items[static_cast<std::size_t>(found_idx)] = cell_val;
                doc.set_field(id, "cells", after, true);
            }
        } else {
            after.items.push_back(cell_val);
            doc.set_field(id, "cells", after, true);
        }
    }
}

float segment_distance(vec2 p, vec2 a, vec2 b) {
    const vec2 ab = b - a;
    const float t = std::clamp(dot(p - a, ab) / std::max(dot(ab, ab), 1e-6f), 0.0f, 1.0f);
    const vec2 d = p - (a + ab * t);
    return std::sqrt(dot(d, d));
}

ImVec2 im(vec2 v) { return {v.x(), v.y()}; }
vec2 cv(ImVec2 v) { return {v.x, v.y}; }

float wrap_pi(float a) {
    while (a > pi) a -= 2.0f * pi;
    while (a < -pi) a += 2.0f * pi;
    return a;
}

enum class Handle { None, MoveX, MoveY, MoveFree, Ring, ScaleX, ScaleY, ScaleBoth, Marquee };

class SceneViewPanel final : public Panel {
public:
    const char* title() const override { return "Scene View"; }
    Dock dock() const override { return Dock::Center; }
    bool padded() const override { return false; }

    void draw(EditorApp& app) override {
        EditorDocument& doc = app.doc();
        ViewCamera& cam = app.camera();
        const brand::ThemeColors& ui = theme::colors();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const vec2 origin = cv(ImGui::GetCursorScreenPos());
        const vec2 size{std::max(avail.x, 8.0f), std::max(avail.y, 8.0f)};
        if (app.take_frame_request()) {
            cam.frame({0.0f, 0.0f}, app.env().virtual_size(), size);
        }

        ImGui::InvisibleButton("##view", im(size),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImGuiIO& io = ImGui::GetIO();
        const vec2 mouse = cv(io.MousePos);

        // The engine renders the scene at this point of the draw list.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 fb_scale = io.DisplayFramebufferScale;
        const ImVec2 disp = ImGui::GetMainViewport()->Pos;
        const rhi::Rect rect{static_cast<std::int32_t>((origin.x() - disp.x) * fb_scale.x),
                             static_cast<std::int32_t>((origin.y() - disp.y) * fb_scale.y),
                             static_cast<std::int32_t>(size.x() * fb_scale.x),
                             static_cast<std::int32_t>(size.y() * fb_scale.y)};
        // The camera works in framebuffer pixels; ImGui in display points.
        const vec2 px_origin{static_cast<float>(rect.x), static_cast<float>(rect.y)};
        const vec2 px_size{static_cast<float>(rect.width), static_cast<float>(rect.height)};
        const vec2 scale{fb_scale.x, fb_scale.y};
        auto to_screen = [&](vec2 world) { return cam.world_to_screen(world, px_origin, px_size) / scale + cv(disp); };
        auto to_world = [&](vec2 screen) {
            return cam.screen_to_world((screen - cv(disp)) * scale, px_origin, px_size);
        };
        app.queue_scene_view(dl, rect);

        if (app.play().active()) {
            // The game has the input while the view is focused (click it).
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ImGui::SetWindowFocus();
            const bool focused = ImGui::IsWindowFocused();
            app.set_game_view(rect, focused);
            dl->PushClipRect(im(origin), im(origin + size), true);
            const std::string info =
                focused ? std::string("game has the input  |  Shift+F5 stop") : std::string("click to give the game the input");
            chip(dl, origin, info, focused ? ui.success : ui.text);
            if (focused) dl->AddRect(im(origin), im(origin + size), theme::im32(ui.success), 0.0f, 2.0f);
            dl->PopClipRect();
            return;
        }
        app.set_game_view(rect, false);

        // ---- dropping a scene from the asset browser instances it here
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(asset_payload)) {
                const std::string path = static_cast<const char*>(p->Data);
                const vec2 at = to_world(mouse);
                const NodeId parent = doc.selection() != 0 ? doc.selection() : doc.model().root();
                const std::string self = doc.has_path() ? app.env().relative(doc.path()) : std::string();
                if (path.ends_with(".scene")) {
                    app.report(doc.instance_scene(parent, path, vec2{std::round(at.x()), std::round(at.y())}, self) != 0);
                } else {
                    app.report_failure(std::format("Only scenes (.scene) can be dropped into the view; '{}' is not one", path));
                }
            }
            ImGui::EndDragDropTarget();
        }

        // ---- navigation
        if (hovered && io.MouseWheel != 0.0f) {
            cam.zoom_at((mouse - cv(disp)) * scale, std::pow(1.15f, io.MouseWheel), px_origin, px_size);
        }
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) ||
                       ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
            cam.center -= cv(io.MouseDelta) * scale / cam.zoom;
        }

        // ---- the selection's places in the view
        const std::vector<NodeId> order = doc.model().preorder();
        const auto& bounds = app.env().bounds();
        auto bounds_of = [&](NodeId id) -> const NodeBounds* {
            for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
                if (order[i] == id) return &bounds[i];
            }
            return nullptr;
        };
        const std::vector<NodeId> targets = doc.transform_targets();
        // The pivot: the centroid of the targets' positions (a single node: its own).
        std::optional<vec2> pivot = drag_ == Handle::None ? doc.selection_pivot() : std::optional<vec2>(pivot_);
        if (!pivot) {
            // Only controls selected: the middle of their rects.
            vec2 sum{0.0f, 0.0f};
            int n = 0;
            for (NodeId id : targets) {
                if (const NodeBounds* b = bounds_of(id); b != nullptr && b->has_rect) {
                    sum += b->rect_min + b->rect_size * 0.5f;
                    ++n;
                }
            }
            if (n > 0) pivot = sum * (1.0f / static_cast<float>(n));
        }
        const Tool tool = app.tool();
        // Rotate and scale need a node that can take them; the toolbar tool stays, the gizmo hides.
        bool can_turn = false;
        for (NodeId id : targets) {
            const NodeBounds* b = bounds_of(id);
            if (b != nullptr && b->has_position && !b->has_rect) can_turn = true;
        }
        const bool show_gizmo = pivot.has_value() && !targets.empty() && tool != Tool::Paint &&
                                (tool == Tool::Move || ((tool == Tool::Rotate || tool == Tool::Scale) && can_turn));
        const vec2 a = pivot ? to_screen(*pivot) : vec2{0.0f, 0.0f};
        const bool snap = app.snap() != io.KeyCtrl;

        const NodeId sel_id = doc.selection();
        const ModelNode* sel_node = doc.model().get(sel_id);
        const bool is_tilemap = sel_node != nullptr && sel_node->type == "TileMapLayer";

        vec2 tile_sz{32.0f, 32.0f};
        vec2 layer_origin{0.0f, 0.0f};
        if (is_tilemap) {
            bool stored_ts = false;
            scene::Value ts_val = doc.field_value(sel_id, "tile_size", &stored_ts);
            if (ts_val.kind == scene::ValueKind::Vec && ts_val.vec.size() >= 2) {
                tile_sz = vec2{ts_val.vec[0], ts_val.vec[1]};
            }
            if (tile_sz.x() <= 0.0f || tile_sz.y() <= 0.0f) tile_sz = vec2{32.0f, 32.0f};
            if (const NodeBounds* b = bounds_of(sel_id); b != nullptr && b->has_position) {
                layer_origin = b->position;
            }
        }
        const vec2 world_m = to_world(mouse);
        const int cell_x = is_tilemap ? static_cast<int>(std::floor((world_m.x() - layer_origin.x()) / tile_sz.x())) : 0;
        const int cell_y = is_tilemap ? static_cast<int>(std::floor((world_m.y() - layer_origin.y()) / tile_sz.y())) : 0;

        if (tool == Tool::Paint && is_tilemap) {
            if (hovered && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
                paint_cell(doc, sel_id, cell_x, cell_y, app.paint_tile_id(), false);
            } else if (hovered && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
                paint_cell(doc, sel_id, cell_x, cell_y, 0, true);
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                doc.seal();
            }
        } else {
            // ---- pressing the mouse: a handle, a node, or the empty view
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                drag_ = hit_handle(tool, show_gizmo, a, mouse);
                if (drag_ == Handle::None) {
                    const int picked = pick_node(bounds, to_world(mouse), 14.0f / cam.zoom * scale.x());
                    const NodeId id = picked >= 0 && static_cast<std::size_t>(picked) < order.size()
                                          ? order[static_cast<std::size_t>(picked)]
                                          : 0;
                    const bool additive = io.KeyCtrl || io.KeyShift;
                    if (id != 0 && additive) {
                        doc.select_toggle(id);
                    } else if (id != 0) {
                        // Clicking a selected node keeps the selection, so the group can be dragged.
                        if (!doc.is_selected(id)) doc.select(id);
                        if (tool == Tool::Move || tool == Tool::Select) drag_ = Handle::MoveFree;
                    } else {
                        drag_ = Handle::Marquee;
                        marquee_additive_ = additive;
                        marquee_a_ = mouse;
                    }
                }
                drag_start_ = to_world(mouse);
                applied_ = {0.0f, 0.0f};
                applied_angle_ = 0.0f;
                applied_factor_ = 1.0f;
                marquee_moved_ = false;
                if (drag_ != Handle::None && drag_ != Handle::Marquee && pivot) {
                    pivot_ = *pivot;
                    const vec2 p = a;
                    last_mouse_angle_ = std::atan2(mouse.y() - p.y(), mouse.x() - p.x());
                    total_angle_ = 0.0f;
                    start_distance_ = std::max(8.0f, length_of(mouse - p));
                    start_axis_distance_ = std::max(8.0f, drag_ == Handle::ScaleY ? std::abs(mouse.y() - p.y())
                                                                                  : std::abs(mouse.x() - p.x()));
                }
            }
            // ---- dragging
            if (drag_ != Handle::None) {
                if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    // The drag is in world units; each node's `position` is
                    // parent-relative (DESIGN.md 4.17): undo its parent's rotation and scale.
                    to_local_ = [&bounds_of](NodeId id, vec2 d) {
                        const NodeBounds* b = bounds_of(id);
                        if (b == nullptr) return d;
                        const float c = std::cos(-b->parent_rotation);
                        const float s = std::sin(-b->parent_rotation);
                        const float sx = b->parent_scale.x() != 0.0f ? b->parent_scale.x() : 1.0f;
                        const float sy = b->parent_scale.y() != 0.0f ? b->parent_scale.y() : 1.0f;
                        return vec2{(d.x() * c - d.y() * s) / sx, (d.x() * s + d.y() * c) / sy};
                    };
                    drag(app, doc, cam, drag_, mouse, a, snap, io.KeyShift, to_world(mouse));
                    to_local_ = {};
                } else {
                    if (drag_ == Handle::Marquee) finish_marquee(doc, order, bounds, to_world(mouse));
                    drag_ = Handle::None;
                    doc.seal(); // one drag, one undo step
                }
            }
        }

        // ---- overlay (drawn after the scene, so on top)
        dl->PushClipRect(im(origin), im(origin + size), true);
        draw_selection(dl, doc, order, bounds, to_screen);
        if (show_gizmo) draw_gizmo(dl, tool, a, mouse, hovered);
        if (is_tilemap && (tool == Tool::Paint || doc.is_selected(sel_id))) {
            const vec2 cell_min = to_screen(layer_origin + vec2{cell_x * tile_sz.x(), cell_y * tile_sz.y()});
            const vec2 cell_max = to_screen(layer_origin + vec2{(cell_x + 1) * tile_sz.x(), (cell_y + 1) * tile_sz.y()});
            dl->AddRect(im(cell_min), im(cell_max), IM_COL32(80, 200, 255, 220), 0.0f, 2.0f);
        }
        if (drag_ == Handle::Marquee && marquee_moved_) {
            const ImVec2 lo{std::min(marquee_a_.x(), mouse.x()), std::min(marquee_a_.y(), mouse.y())};
            const ImVec2 hi{std::max(marquee_a_.x(), mouse.x()), std::max(marquee_a_.y(), mouse.y())};
            dl->AddRectFilled(lo, hi, theme::im32(brand::with_alpha(ui.accent, 0.12f)));
            dl->AddRect(lo, hi, theme::im32(brand::with_alpha(ui.accent, 0.8f)));
        }
        // Zoom, cursor position and the selection, top-left.
        const vec2 w = to_world(mouse);
        std::string info = hovered ? std::format("{:.0f}%   ({:.1f}, {:.1f})", cam.zoom * 100.0f, w.x(), w.y())
                                   : std::format("{:.0f}%", cam.zoom * 100.0f);
        info += std::format("   {}", tool_name(tool));
        if (doc.selected().size() > 1) info += std::format("   {} selected", doc.selected().size());
        chip(dl, origin, info, ui.text);
        dl->PopClipRect();

        if (drag_ == Handle::Marquee && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) marquee_moved_ = true;
    }

private:
    static float length_of(vec2 v) { return std::sqrt(dot(v, v)); }

    static void chip(ImDrawList* dl, vec2 origin, const std::string& text, rhi::Color ink) {
        const brand::ThemeColors& ui = theme::colors();
        const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
        dl->AddRectFilled(im(origin + vec2{8.0f, 8.0f}), im(origin + vec2{20.0f + sz.x, 12.0f + sz.y + 6.0f}),
                          theme::im32(brand::with_alpha(ui.surface, 0.88f)), theme::radius_md);
        dl->AddText(im(origin + vec2{14.0f, 11.0f}), theme::im32(ink), text.c_str());
    }

    // Which handle (if any) is under the pointer.
    Handle hit_handle(Tool tool, bool show_gizmo, vec2 a, vec2 mouse) const {
        if (!show_gizmo) return Handle::None;
        if (tool == Tool::Move) {
            if (std::abs(mouse.x() - a.x()) <= 8.0f && std::abs(mouse.y() - a.y()) <= 8.0f) return Handle::MoveFree;
            if (segment_distance(mouse, a, a + vec2{arm_length, 0.0f}) <= pick_slop) return Handle::MoveX;
            if (segment_distance(mouse, a, a + vec2{0.0f, arm_length}) <= pick_slop) return Handle::MoveY;
        } else if (tool == Tool::Rotate) {
            const float d = length_of(mouse - a);
            if (std::abs(d - ring_radius) <= 7.0f) return Handle::Ring;
        } else if (tool == Tool::Scale) {
            if (std::abs(mouse.x() - a.x()) <= 8.0f && std::abs(mouse.y() - a.y()) <= 8.0f) return Handle::ScaleBoth;
            if (length_of(mouse - (a + vec2{arm_length, 0.0f})) <= 9.0f ||
                segment_distance(mouse, a, a + vec2{arm_length, 0.0f}) <= 4.0f) {
                return Handle::ScaleX;
            }
            if (length_of(mouse - (a + vec2{0.0f, arm_length})) <= 9.0f ||
                segment_distance(mouse, a, a + vec2{0.0f, arm_length}) <= 4.0f) {
                return Handle::ScaleY;
            }
        }
        return Handle::None;
    }

    void drag(EditorApp& app, EditorDocument& doc, ViewCamera& cam, Handle h, vec2 mouse, vec2 a, bool snap, bool free,
              vec2 world_mouse) {
        switch (h) {
            case Handle::MoveX:
            case Handle::MoveY:
            case Handle::MoveFree: {
                const vec2 axes = h == Handle::MoveX ? vec2{1.0f, 0.0f} : h == Handle::MoveY ? vec2{0.0f, 1.0f} : vec2{1.0f, 1.0f};
                vec2 total = (world_mouse - drag_start_) * axes;
                if (snap) {
                    const float step = grid_step(cam.zoom);
                    total = {std::round(total.x() / step) * step, std::round(total.y() / step) * step};
                } else if (!free) {
                    total = {std::round(total.x()), std::round(total.y())}; // whole virtual pixels
                }
                const vec2 delta = total - applied_;
                if (delta != vec2{0.0f, 0.0f}) {
                    app.report(doc.move_selection_by(delta, true, to_local_));
                    applied_ = total;
                }
                break;
            }
            case Handle::Ring: {
                const float now = std::atan2(mouse.y() - a.y(), mouse.x() - a.x());
                total_angle_ += wrap_pi(now - last_mouse_angle_);
                last_mouse_angle_ = now;
                float target = total_angle_;
                if (snap) {
                    target = std::round(target / (pi / 12.0f)) * (pi / 12.0f); // 15 degrees
                } else if (!free) {
                    target = std::round(target * 1000.0f) / 1000.0f;
                }
                if (target != applied_angle_) {
                    app.report(doc.rotate_selection_by(target - applied_angle_, pivot_, true));
                    applied_angle_ = target;
                }
                break;
            }
            case Handle::ScaleX:
            case Handle::ScaleY:
            case Handle::ScaleBoth: {
                float factor = 1.0f;
                if (h == Handle::ScaleBoth) {
                    factor = length_of(mouse - a) / start_distance_;
                } else {
                    const float d = h == Handle::ScaleX ? std::abs(mouse.x() - a.x()) : std::abs(mouse.y() - a.y());
                    factor = d / start_axis_distance_;
                }
                factor = std::clamp(factor, 0.05f, 20.0f);
                factor = snap ? std::round(factor * 10.0f) / 10.0f : std::round(factor * 100.0f) / 100.0f;
                factor = std::max(factor, 0.05f);
                if (factor != applied_factor_) {
                    const float ratio = factor / applied_factor_;
                    const vec2 f{h == Handle::ScaleY ? 1.0f : ratio, h == Handle::ScaleX ? 1.0f : ratio};
                    app.report(doc.scale_selection_by(f, pivot_, true));
                    applied_factor_ = factor;
                }
                break;
            }
            case Handle::Marquee:
            case Handle::None: break;
        }
    }

    void finish_marquee(EditorDocument& doc, const std::vector<NodeId>& order, const std::vector<NodeBounds>& bounds,
                        vec2 world_end) {
        if (!marquee_moved_) {
            // A plain click on empty space: the root, or nothing when adding.
            if (!marquee_additive_) doc.select(doc.model().root());
            return;
        }
        const vec2 lo = min(drag_start_, world_end);
        const vec2 hi = max(drag_start_, world_end);
        std::vector<NodeId> hit;
        if (marquee_additive_) hit = doc.selected();
        for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
            if (order[i] == doc.model().root()) continue;
            const NodeBounds& b = bounds[i];
            bool inside = false;
            if (b.has_rect) {
                inside = b.rect_min.x() <= hi.x() && b.rect_min.x() + b.rect_size.x() >= lo.x() &&
                         b.rect_min.y() <= hi.y() && b.rect_min.y() + b.rect_size.y() >= lo.y();
            } else if (b.has_position) {
                inside = b.position.x() >= lo.x() && b.position.x() <= hi.x() && b.position.y() >= lo.y() &&
                         b.position.y() <= hi.y();
            }
            if (inside && std::find(hit.begin(), hit.end(), order[i]) == hit.end()) hit.push_back(order[i]);
        }
        doc.select_set(hit);
    }

    template <typename ToScreen>
    void draw_selection(ImDrawList* dl, EditorDocument& doc, const std::vector<NodeId>& order,
                        const std::vector<NodeBounds>& bounds, ToScreen to_screen) {
        const brand::ThemeColors& ui = theme::colors();
        for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
            if (!doc.is_selected(order[i])) continue;
            const bool primary = order[i] == doc.selection();
            const ImU32 color = theme::im32(brand::with_alpha(ui.accent, primary ? 1.0f : 0.75f));
            if (bounds[i].has_rect) {
                dl->AddRect(im(to_screen(bounds[i].rect_min)), im(to_screen(bounds[i].rect_min + bounds[i].rect_size)), color,
                            0.0f, primary ? 1.8f : 1.2f);
            } else if (bounds[i].has_position) {
                dl->AddCircle(im(to_screen(bounds[i].position)), 9.0f, color, 24, primary ? 1.8f : 1.2f);
            }
        }
    }

    void draw_gizmo(ImDrawList* dl, Tool tool, vec2 a, vec2 mouse, bool hovered) {
        const brand::ThemeColors& ui = theme::colors();
        const Handle over = hovered && drag_ == Handle::None ? hit_handle(tool, true, a, mouse) : Handle::None;
        auto lit = [&](Handle h, rhi::Color base) {
            return theme::im32(drag_ == h ? brand::mix(base, ui.text, 0.55f) : over == h ? brand::mix(base, ui.text, 0.3f) : base);
        };
        // Axis colors keep the red x / green y convention, taken from the danger and success tokens.
        const rhi::Color xc = ui.danger;
        const rhi::Color yc = ui.success;
        const rhi::Color both = ui.info;
        // A backing halo keeps the gizmo readable over any scene color.
        const ImU32 halo = theme::im32(brand::with_alpha(ui.surface, 0.55f));
        if (tool == Tool::Move) {
            const vec2 xe = a + vec2{arm_length, 0.0f};
            const vec2 ye = a + vec2{0.0f, arm_length};
            dl->AddLine(im(a), im(xe), halo, 5.0f);
            dl->AddLine(im(a), im(ye), halo, 5.0f);
            dl->AddLine(im(a), im(xe), lit(Handle::MoveX, xc), 2.5f);
            dl->AddTriangleFilled(im(xe + vec2{11.0f, 0.0f}), im(xe + vec2{0.0f, -5.5f}), im(xe + vec2{0.0f, 5.5f}),
                                  lit(Handle::MoveX, xc));
            dl->AddLine(im(a), im(ye), lit(Handle::MoveY, yc), 2.5f);
            dl->AddTriangleFilled(im(ye + vec2{0.0f, 11.0f}), im(ye + vec2{5.5f, 0.0f}), im(ye + vec2{-5.5f, 0.0f}),
                                  lit(Handle::MoveY, yc));
            dl->AddRectFilled(im(a - vec2{5.0f, 5.0f}), im(a + vec2{5.0f, 5.0f}), lit(Handle::MoveFree, both), 2.0f);
        } else if (tool == Tool::Rotate) {
            dl->AddCircle(im(a), ring_radius, halo, 64, 5.0f);
            dl->AddCircle(im(a), ring_radius, lit(Handle::Ring, ui.accent), 64, 2.0f);
            dl->AddCircleFilled(im(a), 3.0f, lit(Handle::Ring, ui.accent));
            if (drag_ == Handle::Ring) {
                // The turned angle as a wedge from the start direction.
                const float start = last_mouse_angle_ - total_angle_;
                dl->PathLineTo(im(a));
                dl->PathArcTo(im(a), ring_radius - 10.0f, start, start + total_angle_, 48);
                dl->PathFillConvex(theme::im32(brand::with_alpha(ui.accent, 0.22f)));
                const ImVec2 tip{a.x() + std::cos(last_mouse_angle_) * ring_radius, a.y() + std::sin(last_mouse_angle_) * ring_radius};
                dl->AddLine(im(a), tip, theme::im32(ui.accent), 1.5f);
                const std::string label = std::format("{:+.1f}\xC2\xB0", applied_angle_ * 180.0f / pi);
                dl->AddText({a.x() + 12.0f, a.y() - ring_radius - 22.0f}, theme::im32(ui.text), label.c_str());
            }
        } else if (tool == Tool::Scale) {
            const vec2 xe = a + vec2{arm_length, 0.0f};
            const vec2 ye = a + vec2{0.0f, arm_length};
            dl->AddLine(im(a), im(xe), halo, 5.0f);
            dl->AddLine(im(a), im(ye), halo, 5.0f);
            dl->AddLine(im(a), im(xe), lit(Handle::ScaleX, xc), 2.5f);
            dl->AddLine(im(a), im(ye), lit(Handle::ScaleY, yc), 2.5f);
            dl->AddRectFilled(im(xe - vec2{6.0f, 6.0f}), im(xe + vec2{6.0f, 6.0f}), lit(Handle::ScaleX, xc), 1.5f);
            dl->AddRectFilled(im(ye - vec2{6.0f, 6.0f}), im(ye + vec2{6.0f, 6.0f}), lit(Handle::ScaleY, yc), 1.5f);
            dl->AddRectFilled(im(a - vec2{6.0f, 6.0f}), im(a + vec2{6.0f, 6.0f}), lit(Handle::ScaleBoth, both), 1.5f);
            if (drag_ == Handle::ScaleX || drag_ == Handle::ScaleY || drag_ == Handle::ScaleBoth) {
                const std::string label = std::format("\xC3\x97{:.2f}", applied_factor_);
                dl->AddText({a.x() + 12.0f, a.y() - 28.0f}, theme::im32(ui.text), label.c_str());
            }
        }
    }

    Handle drag_ = Handle::None;
    vec2 drag_start_{0.0f, 0.0f};
    vec2 applied_{0.0f, 0.0f};
    EditorDocument::DeltaToLocal to_local_; // set while drag() runs (world -> local move deltas)
    vec2 pivot_{0.0f, 0.0f};
    float applied_angle_ = 0.0f;
    float total_angle_ = 0.0f;
    float last_mouse_angle_ = 0.0f;
    float applied_factor_ = 1.0f;
    float start_distance_ = 8.0f;
    float start_axis_distance_ = 8.0f;
    vec2 marquee_a_{0.0f, 0.0f};
    bool marquee_moved_ = false;
    bool marquee_additive_ = false;
};

} // namespace

std::unique_ptr<Panel> make_scene_view_panel() { return std::make_unique<SceneViewPanel>(); }

} // namespace cuervo::editor

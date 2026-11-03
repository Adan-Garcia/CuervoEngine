#include "editor/scene_input.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cuervo::editor {

Route FocusRouter::pointer(vec2 p) const {
    if (buttons_ != 0) return capture_;
    return (!modal_ && inside(p)) ? Route::Scene : Route::Ui;
}

Route FocusRouter::press(vec2 p, int button) {
    const Route target = pointer(p);
    if (buttons_ == 0) capture_ = target;
    buttons_ |= 1 << std::clamp(button, 0, 30);
    // The first press of a gesture decides who holds keyboard focus.
    if (buttons_ == (1 << std::clamp(button, 0, 30))) scene_focus_ = capture_ == Route::Scene;
    return capture_;
}

void FocusRouter::release(int button) { buttons_ &= ~(1 << std::clamp(button, 0, 30)); }

namespace {
// Offsets of the gizmo parts from its pivot, in pixels (the arrows point right and down: screen y is down).
constexpr float free_half = 6.0f;
constexpr float grab = 6.0f;
constexpr float pi = 3.14159265f;

float len_of(vec2 v) { return std::sqrt(dot(v, v)); }

float segment_dist(vec2 p, vec2 a, vec2 b) {
    const vec2 ab = b - a;
    const float t = std::clamp(dot(p - a, ab) / std::max(dot(ab, ab), 1e-6f), 0.0f, 1.0f);
    return len_of(p - (a + ab * t));
}

float wrap_pi(float a) {
    while (a > pi) a -= 2.0f * pi;
    while (a < -pi) a += 2.0f * pi;
    return a;
}
} // namespace

// ---- shared logic

void paint_tile_cell(EditorDocument& doc, NodeId id, int cx, int cy, int tile_id, bool erase) {
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
        return;
    }
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

TileCellRef tile_cell_at(const EditorDocument& doc, NodeId layer, const std::vector<NodeId>& order,
                         const std::vector<NodeBounds>& bounds, vec2 world) {
    TileCellRef r;
    bool stored = false;
    const scene::Value ts = doc.field_value(layer, "tile_size", &stored);
    if (ts.kind == scene::ValueKind::Vec && ts.vec.size() >= 2) r.tile = vec2{ts.vec[0], ts.vec[1]};
    if (r.tile.x() <= 0.0f || r.tile.y() <= 0.0f) r.tile = vec2{32.0f, 32.0f};
    for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
        if (order[i] == layer && bounds[i].has_position) r.origin = bounds[i].position;
    }
    r.x = static_cast<int>(std::floor((world.x() - r.origin.x()) / r.tile.x()));
    r.y = static_cast<int>(std::floor((world.y() - r.origin.y()) / r.tile.y()));
    return r;
}

void marquee_select(EditorDocument& doc, const std::vector<NodeId>& order, const std::vector<NodeBounds>& bounds, vec2 a,
                    vec2 b, bool additive) {
    const vec2 lo = min(a, b);
    const vec2 hi = max(a, b);
    std::vector<NodeId> hit;
    if (additive) hit = doc.selected();
    for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
        if (order[i] == doc.model().root()) continue;
        const NodeBounds& nb = bounds[i];
        bool inside = false;
        if (nb.has_rect) {
            inside = nb.rect_min.x() <= hi.x() && nb.rect_min.x() + nb.rect_size.x() >= lo.x() &&
                     nb.rect_min.y() <= hi.y() && nb.rect_min.y() + nb.rect_size.y() >= lo.y();
        } else if (nb.has_position) {
            inside = nb.position.x() >= lo.x() && nb.position.x() <= hi.x() && nb.position.y() >= lo.y() &&
                     nb.position.y() <= hi.y();
        }
        if (inside && std::find(hit.begin(), hit.end(), order[i]) == hit.end()) hit.push_back(order[i]);
    }
    doc.select_set(hit);
}

void TurnDrag::begin(vec2 mouse, vec2 pivot) {
    last = std::atan2(mouse.y() - pivot.y(), mouse.x() - pivot.x());
    total = 0.0f;
    applied = 0.0f;
}

float TurnDrag::update(vec2 mouse, vec2 pivot, bool snap, bool free) {
    const float now = std::atan2(mouse.y() - pivot.y(), mouse.x() - pivot.x());
    total += wrap_pi(now - last);
    last = now;
    float target = total;
    if (snap) {
        target = std::round(target / (pi / 12.0f)) * (pi / 12.0f); // 15 degrees
    } else if (!free) {
        target = std::round(target * 1000.0f) / 1000.0f;
    }
    if (target == applied) return 0.0f;
    const float step = target - applied;
    applied = target;
    return step;
}

void ScaleDrag::begin(Handle h, vec2 mouse, vec2 pivot) {
    applied = 1.0f;
    start_distance = std::max(8.0f, len_of(mouse - pivot));
    start_axis = std::max(8.0f, h == Handle::ScaleY ? std::abs(mouse.y() - pivot.y()) : std::abs(mouse.x() - pivot.x()));
}

std::optional<vec2> ScaleDrag::update(Handle h, vec2 mouse, vec2 pivot, bool snap) {
    float factor = 1.0f;
    if (h == Handle::ScaleBoth) {
        factor = len_of(mouse - pivot) / start_distance;
    } else {
        const float d = h == Handle::ScaleX ? std::abs(mouse.x() - pivot.x()) : std::abs(mouse.y() - pivot.y());
        factor = d / start_axis;
    }
    factor = std::clamp(factor, 0.05f, 20.0f);
    factor = snap ? std::round(factor * 10.0f) / 10.0f : std::round(factor * 100.0f) / 100.0f;
    factor = std::max(factor, 0.05f);
    if (factor == applied) return std::nullopt;
    const float ratio = factor / applied;
    applied = factor;
    return vec2{h == Handle::ScaleY ? 1.0f : ratio, h == Handle::ScaleX ? 1.0f : ratio};
}

// ---- SceneInput

bool SceneInput::can_turn() const {
    const auto& bounds = ctl_.env().bounds();
    const std::vector<NodeId> order = ctl_.doc().model().preorder();
    for (NodeId id : ctl_.doc().transform_targets()) {
        for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
            if (order[i] == id && bounds[i].has_position && !bounds[i].has_rect) return true;
        }
    }
    return false;
}

std::optional<vec2> SceneInput::pivot_now() const {
    if (const auto pv = ctl_.doc().selection_pivot()) return pv;
    // Only controls selected: the middle of their rects.
    const std::vector<NodeId> order = ctl_.doc().model().preorder();
    const auto& bounds = ctl_.env().bounds();
    vec2 sum{0.0f, 0.0f};
    int n = 0;
    for (NodeId id : ctl_.doc().transform_targets()) {
        for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
            if (order[i] == id && bounds[i].has_rect) {
                sum += bounds[i].rect_min + bounds[i].rect_size * 0.5f;
                ++n;
            }
        }
    }
    if (n == 0) return std::nullopt;
    return sum * (1.0f / static_cast<float>(n));
}

std::optional<vec2> SceneInput::gizmo_pivot() const {
    const Tool tool = ctl_.tool();
    if (tool != Tool::Move && tool != Tool::Rotate && tool != Tool::Scale) return std::nullopt;
    if (ctl_.doc().transform_targets().empty()) return std::nullopt;
    if (tool != Tool::Move && !can_turn()) return std::nullopt;
    if (drag_ != Handle::None && drag_ != Handle::Marquee) return pivot_;
    return pivot_now();
}

Handle SceneInput::hit(vec2 p) const {
    const auto pv = gizmo_pivot();
    if (!pv) return Handle::None;
    const vec2 a = to_screen(*pv);
    const vec2 d = p - a;
    switch (ctl_.tool()) {
    case Tool::Move:
        if (std::abs(d.x()) <= free_half + 2.0f && std::abs(d.y()) <= free_half + 2.0f) return Handle::MoveFree;
        if (std::abs(d.y()) <= grab && d.x() > 0.0f && d.x() <= arm + grab) return Handle::MoveX;
        if (std::abs(d.x()) <= grab && d.y() > 0.0f && d.y() <= arm + grab) return Handle::MoveY;
        return Handle::None;
    case Tool::Rotate: return std::abs(len_of(d) - ring) <= 6.0f ? Handle::Ring : Handle::None;
    case Tool::Scale:
        if (std::abs(d.x()) <= 8.0f && std::abs(d.y()) <= 8.0f) return Handle::ScaleBoth;
        if (len_of(d - vec2{arm, 0.0f}) <= 8.0f || segment_dist(d, {0, 0}, {arm, 0.0f}) <= 4.0f) return Handle::ScaleX;
        if (len_of(d - vec2{0.0f, arm}) <= 8.0f || segment_dist(d, {0, 0}, {0.0f, arm}) <= 4.0f) return Handle::ScaleY;
        return Handle::None;
    default: return Handle::None;
    }
}

void SceneInput::paint_at(vec2 p, bool erase) {
    EditorDocument& doc = ctl_.doc();
    const NodeId layer = doc.selection();
    const ModelNode* n = doc.model().get(layer);
    if (n == nullptr || n->type != "TileMapLayer") return;
    const TileCellRef c = tile_cell_at(doc, layer, doc.model().preorder(), ctl_.env().bounds(), to_world(p));
    paint_tile_cell(doc, layer, c.x, c.y, erase ? 0 : ctl_.paint_tile_id(), erase);
}

std::string SceneInput::readout() const {
    if (drag_ == Handle::Ring) return std::format("{:+.1f} deg", turn_.applied * 180.0f / pi);
    if (drag_ == Handle::ScaleX || drag_ == Handle::ScaleY || drag_ == Handle::ScaleBoth) return std::format("x{:.2f}", scale_.applied);
    return {};
}

void SceneInput::down(int button, vec2 p, bool additive) {
    last_ = p;
    EditorDocument& doc = ctl_.doc();
    const bool tile_mode = ctl_.tool() == Tool::Paint && [&] {
        const ModelNode* n = doc.model().get(doc.selection());
        return n != nullptr && n->type == "TileMapLayer";
    }();
    if (tile_mode && (button == 0 || button == 2)) { // left paints, right erases (middle still pans)
        painting_ = true;
        erasing_ = button == 2;
        paint_at(p, erasing_);
        return;
    }
    if (button == 1 || button == 2) { // middle, right: pan
        panning_ = true;
        return;
    }
    if (button != 0) return;
    drag_ = hit(p);
    marquee_moved_ = false;
    if (drag_ == Handle::None) {
        const std::vector<NodeId> order = doc.model().preorder();
        const auto& bounds = ctl_.env().bounds();
        const int picked = pick_node(bounds, to_world(p), 14.0f / cam_.zoom);
        const NodeId id = picked >= 0 && static_cast<std::size_t>(picked) < order.size() ? order[static_cast<std::size_t>(picked)] : 0;
        if (id != 0 && additive) {
            doc.select_toggle(id);
        } else if (id != 0) {
            // Clicking a selected node keeps the selection, so the group can be dragged.
            if (!doc.is_selected(id)) doc.select(id);
            if (ctl_.tool() == Tool::Move || ctl_.tool() == Tool::Select) drag_ = Handle::MoveFree;
        } else if (ctl_.tool() != Tool::Paint) {
            drag_ = Handle::Marquee;
            marquee_additive_ = additive;
            marquee_a_ = p;
        }
    }
    if (drag_ != Handle::None) {
        start_ = to_world(p);
        applied_ = {0.0f, 0.0f};
        if (drag_ != Handle::Marquee) {
            if (const auto pv = pivot_now()) pivot_ = *pv;
            turn_.begin(p, to_screen(pivot_));
            scale_.begin(drag_, p, to_screen(pivot_));
        }
    }
}

void SceneInput::move(vec2 p) {
    if (painting_) {
        paint_at(p, erasing_);
    } else if (panning_) {
        cam_.center -= (p - last_) / cam_.zoom;
    } else if (drag_ == Handle::Marquee) {
        if (len_of(p - marquee_a_) >= 3.0f) marquee_moved_ = true;
    } else if (drag_ != Handle::None) {
        EditorDocument& doc = ctl_.doc();
        if (drag_ == Handle::Ring) {
            const float step = turn_.update(p, to_screen(pivot_), snap_now(), shift_);
            if (step != 0.0f) ctl_.report(doc.rotate_selection_by(step, pivot_, true));
        } else if (drag_ == Handle::ScaleX || drag_ == Handle::ScaleY || drag_ == Handle::ScaleBoth) {
            if (const auto f = scale_.update(drag_, p, to_screen(pivot_), snap_now())) ctl_.report(doc.scale_selection_by(*f, pivot_, true));
        } else {
            const vec2 axes = drag_ == Handle::MoveX ? vec2{1.0f, 0.0f} : drag_ == Handle::MoveY ? vec2{0.0f, 1.0f} : vec2{1.0f, 1.0f};
            vec2 total = (to_world(p) - start_) * axes;
            if (snap_now()) {
                const float step = grid_step(cam_.zoom);
                total = {std::round(total.x() / step) * step, std::round(total.y() / step) * step};
            } else if (!shift_) {
                total = {std::round(total.x()), std::round(total.y())}; // whole virtual pixels
            }
            const vec2 delta = total - applied_;
            if (delta != vec2{0.0f, 0.0f}) {
                const auto& bounds = ctl_.env().bounds();
                const std::vector<NodeId> order = doc.model().preorder();
                // The drag is in world units; each node's `position` is parent-relative (DESIGN.md 4.17):
                // undo its parent's rotation and scale.
                const auto to_local = [&](NodeId id, vec2 d) {
                    for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
                        if (order[i] != id) continue;
                        const NodeBounds& b = bounds[i];
                        const float c = std::cos(-b.parent_rotation), s = std::sin(-b.parent_rotation);
                        const float sx = b.parent_scale.x() != 0.0f ? b.parent_scale.x() : 1.0f;
                        const float sy = b.parent_scale.y() != 0.0f ? b.parent_scale.y() : 1.0f;
                        return vec2{(d.x() * c - d.y() * s) / sx, (d.x() * s + d.y() * c) / sy};
                    }
                    return d;
                };
                ctl_.report(doc.move_selection_by(delta, true, to_local));
                applied_ = total;
            }
        }
    } else {
        hover_ = hit(p);
    }
    last_ = p;
}

void SceneInput::up(int button, vec2 p) {
    if (button == 1 || button == 2) panning_ = false;
    if (painting_ && ((button == 0 && !erasing_) || (button == 2 && erasing_))) {
        painting_ = false;
        ctl_.doc().seal(); // one stroke, one undo step
    }
    if (button == 0 && drag_ != Handle::None) {
        EditorDocument& doc = ctl_.doc();
        if (drag_ == Handle::Marquee) {
            if (marquee_moved_) {
                marquee_select(doc, doc.model().preorder(), ctl_.env().bounds(), start_, to_world(p), marquee_additive_);
            } else if (!marquee_additive_) {
                doc.select_clear(); // a plain click on empty space
            }
        }
        drag_ = Handle::None;
        marquee_moved_ = false;
        doc.seal(); // one drag, one undo step
    }
    last_ = p;
}

// One notch is wheel.y = 1 (SDL scales high-resolution wheels to that); a touchpad or smooth-scroll
// burst can arrive as one event with a large value, so a single event never zooms more than 3 notches.
void SceneInput::wheel(float dy, vec2 p) { cam_.zoom_at(p, std::pow(1.2f, std::clamp(dy, -3.0f, 3.0f)), {0, 0}, size_); }

void SceneInput::frame_all() { cam_.frame({0.0f, 0.0f}, ctl_.env().virtual_size(), size_); }

void SceneInput::draw_overlay(render::SdfBatcher& shapes, const brand::ThemeColors& ui, float upp) const {
    const EditorDocument& doc = ctl_.doc();
    const std::vector<NodeId> order = doc.model().preorder();
    const auto& bounds = ctl_.env().bounds();
    for (std::size_t i = 0; i < order.size() && i < bounds.size(); ++i) {
        if (!doc.is_selected(order[i])) continue;
        const bool primary = order[i] == doc.selection();
        const rhi::Color color = brand::with_alpha(ui.accent, primary ? 1.0f : 0.75f);
        const float t = (primary ? 0.9f : 0.6f) * upp; // half thickness
        if (bounds[i].has_rect) {
            const vec2 lo = bounds[i].rect_min, sz = bounds[i].rect_size, c = lo + sz * 0.5f, h = sz * 0.5f;
            shapes.rounded_rect({c.x(), lo.y()}, {h.x() + t, t}, 0.0f, 0.0f, color);
            shapes.rounded_rect({c.x(), lo.y() + sz.y()}, {h.x() + t, t}, 0.0f, 0.0f, color);
            shapes.rounded_rect({lo.x(), c.y()}, {t, h.y() + t}, 0.0f, 0.0f, color);
            shapes.rounded_rect({lo.x() + sz.x(), c.y()}, {t, h.y() + t}, 0.0f, 0.0f, color);
        } else if (bounds[i].has_position) {
            shapes.ring(bounds[i].position, 9.0f * upp, 2.0f * t, color);
        }
    }
    const auto frame_rect = [&](vec2 lo, vec2 hi, float t, rhi::Color color) {
        const vec2 c = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f;
        shapes.rounded_rect({c.x(), lo.y()}, {h.x() + t, t}, 0.0f, 0.0f, color);
        shapes.rounded_rect({c.x(), hi.y()}, {h.x() + t, t}, 0.0f, 0.0f, color);
        shapes.rounded_rect({lo.x(), c.y()}, {t, h.y() + t}, 0.0f, 0.0f, color);
        shapes.rounded_rect({hi.x(), c.y()}, {t, h.y() + t}, 0.0f, 0.0f, color);
    };
    // Tilemap: the cell under the pointer (the tile cursor) while painting or with the layer selected.
    if (const ModelNode* n = doc.model().get(doc.selection()); n != nullptr && n->type == "TileMapLayer" && (ctl_.tool() == Tool::Paint || doc.is_selected(doc.selection()))) {
        const TileCellRef c = tile_cell_at(doc, doc.selection(), order, bounds, to_world(last_));
        const vec2 lo = c.origin + vec2{static_cast<float>(c.x) * c.tile.x(), static_cast<float>(c.y) * c.tile.y()};
        frame_rect(lo, lo + c.tile, 1.0f * upp, brand::with_alpha(ui.info, 0.9f));
    }
    if (drag_ == Handle::Marquee && marquee_moved_) {
        const vec2 a = to_world(marquee_a_), b = to_world(last_);
        const vec2 lo = min(a, b), hi = max(a, b), c = (lo + hi) * 0.5f;
        shapes.rounded_rect(c, (hi - lo) * 0.5f, 0.0f, 0.0f, brand::with_alpha(ui.accent, 0.12f));
        frame_rect(lo, hi, 0.5f * upp, brand::with_alpha(ui.accent, 0.8f));
    }
    const auto pv = gizmo_pivot();
    if (!pv) return;
    const auto lit = [&](Handle h, rhi::Color base) { return hot() == h ? brand::mix(base, ui.text, 0.4f) : base; };
    const vec2 a = *pv;
    const float len = arm * upp, w = 1.25f * upp;
    // Axis colors keep the red x / green y convention, taken from the danger and success tokens.
    if (ctl_.tool() == Tool::Move) {
        shapes.rounded_rect({a.x() + len * 0.5f, a.y()}, {len * 0.5f, w}, 0.0f, 0.0f, lit(Handle::MoveX, ui.danger));
        shapes.rounded_rect({a.x(), a.y() + len * 0.5f}, {w, len * 0.5f}, 0.0f, 0.0f, lit(Handle::MoveY, ui.success));
        shapes.rounded_rect({a.x() + len, a.y()}, {4.0f * upp, 4.0f * upp}, 0.0f, 0.0f, lit(Handle::MoveX, ui.danger));
        shapes.rounded_rect({a.x(), a.y() + len}, {4.0f * upp, 4.0f * upp}, 0.0f, 0.0f, lit(Handle::MoveY, ui.success));
        shapes.rounded_rect(a, {5.0f * upp, 5.0f * upp}, 1.0f * upp, 0.0f, lit(Handle::MoveFree, ui.info));
    } else if (ctl_.tool() == Tool::Rotate) {
        shapes.ring(a, ring * upp, 2.0f * w, lit(Handle::Ring, ui.accent));
        shapes.circle(a, 3.0f * upp, lit(Handle::Ring, ui.accent));
        if (drag_ == Handle::Ring) {
            // The pointer's direction: a line from the pivot to the ring.
            const float ang = turn_.last;
            const vec2 dir{std::cos(ang), std::sin(ang)};
            shapes.rounded_rect(a + dir * (ring * upp * 0.5f), {ring * upp * 0.5f, w * 0.6f}, 0.0f, ang, brand::with_alpha(ui.accent, 0.8f));
        }
    } else if (ctl_.tool() == Tool::Scale) {
        shapes.rounded_rect({a.x() + len * 0.5f, a.y()}, {len * 0.5f, w}, 0.0f, 0.0f, lit(Handle::ScaleX, ui.danger));
        shapes.rounded_rect({a.x(), a.y() + len * 0.5f}, {w, len * 0.5f}, 0.0f, 0.0f, lit(Handle::ScaleY, ui.success));
        shapes.rounded_rect({a.x() + len, a.y()}, {5.0f * upp, 5.0f * upp}, 0.0f, 0.0f, lit(Handle::ScaleX, ui.danger));
        shapes.rounded_rect({a.x(), a.y() + len}, {5.0f * upp, 5.0f * upp}, 0.0f, 0.0f, lit(Handle::ScaleY, ui.success));
        shapes.rounded_rect(a, {5.0f * upp, 5.0f * upp}, 0.0f, 0.0f, lit(Handle::ScaleBoth, ui.info));
    }
}

} // namespace cuervo::editor

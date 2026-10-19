// Multi-select and group transforms of EditorDocument (DESIGN.md 4.16 "Clarified
// (Milestone 8.3)"): the gizmos and the palette act on the whole selection, and a
// group transform is ONE undo step that merges while the gizmo is dragged.
#include <algorithm>
#include <cmath>
#include <format>

#include "cuervo/math/dmath.hpp"
#include "editor/editor_document.hpp"

namespace cuervo::editor {

namespace {

// Positions land on a 1/1000 grid so rotating and scaling don't leave float noise in scene files.
float round_milli(float v) { return std::round(v * 1000.0f) / 1000.0f; }

} // namespace

// ------------------------------------------------------------ selection

bool EditorDocument::is_selected(NodeId id) const {
    return std::find(selected_.begin(), selected_.end(), id) != selected_.end();
}

void EditorDocument::select(NodeId id) {
    selected_.clear();
    selection_ = 0;
    if (model_.contains(id)) {
        selected_.push_back(id);
        selection_ = id;
    }
}

void EditorDocument::select_add(NodeId id) {
    if (!model_.contains(id)) return;
    std::erase(selected_, id);
    selected_.push_back(id);
    selection_ = id;
}

void EditorDocument::select_toggle(NodeId id) {
    if (!model_.contains(id)) return;
    if (is_selected(id)) {
        std::erase(selected_, id);
        selection_ = selected_.empty() ? 0 : selected_.back();
    } else {
        select_add(id);
    }
}

void EditorDocument::select_set(const std::vector<NodeId>& ids) {
    selected_.clear();
    for (NodeId id : ids) {
        if (model_.contains(id) && !is_selected(id)) selected_.push_back(id);
    }
    selection_ = selected_.empty() ? 0 : selected_.back();
}

void EditorDocument::select_clear() { select(0); }

void EditorDocument::prune_selection() {
    std::erase_if(selected_, [&](NodeId id) { return !model_.contains(id); });
    if (selected_.empty() && !model_.empty()) selected_.push_back(model_.root());
    if (selected_.empty()) {
        selection_ = 0;
    } else if (!is_selected(selection_)) {
        selection_ = selected_.back();
    }
}

std::vector<NodeId> EditorDocument::transform_targets() const {
    std::vector<NodeId> out;
    for (NodeId id : selected_) {
        if (id == model_.root()) continue;
        bool covered = false;
        for (NodeId other : selected_) {
            if (other != id && other != model_.root() && model_.is_ancestor_or_self(other, id)) covered = true;
        }
        if (!covered) out.push_back(id);
    }
    return out;
}

// ------------------------------------------------------------ group transforms

namespace {

std::optional<scene::Value> stored_value(const EditorDocument& doc, NodeId id, std::string_view field) {
    bool stored = false;
    scene::Value v = doc.field_value(id, field, &stored);
    if (!stored) return std::nullopt;
    return v;
}

bool is_vec2(const scene::Value& v) { return v.kind == scene::ValueKind::Vec && v.vec.size() == 2; }

} // namespace

bool EditorDocument::move_parts(NodeId id, vec2 delta, std::vector<std::unique_ptr<Command>>& parts) {
    const ModelNode* n = model_.get(id);
    if (n == nullptr) return fail("no such node");
    // A Control's position is computed by layout from its anchors and
    // offsets (DESIGN.md 4.10): moving it shifts both offsets.
    const bool control = registry_ != nullptr && registry_->is_a(n->type, "Control");
    for (std::string_view field : control ? std::vector<std::string_view>{"offset_min", "offset_max"}
                                          : std::vector<std::string_view>{"position"}) {
        scene::Value v = field_value(id, field);
        if (!is_vec2(v)) return fail(std::format("'{}' has no vec2 '{}' to move", n->type, field));
        v.vec[0] += delta.x();
        v.vec[1] += delta.y();
        parts.push_back(std::make_unique<SetFieldCommand>(id, std::string(field), field_type(id, field),
                                                          stored_value(*this, id, field), std::move(v)));
    }
    return true;
}

bool EditorDocument::move_by(NodeId id, vec2 delta, bool merge) {
    const ModelNode* n = model_.get(id);
    if (n == nullptr) return fail("no such node");
    std::vector<std::unique_ptr<Command>> parts;
    if (!move_parts(id, delta, parts)) return false;
    apply(std::make_unique<GroupCommand>(std::move(parts), std::format("Move '{}'", n->name)), merge);
    return true;
}

std::optional<vec2> EditorDocument::selection_pivot() const {
    vec2 sum{0.0f, 0.0f};
    int count = 0;
    for (NodeId id : transform_targets()) {
        if (registry_ != nullptr && registry_->is_a(model_.get(id)->type, "Control")) continue;
        const scene::Value p = field_value(id, "position");
        if (!is_vec2(p)) continue;
        sum += vec2{p.vec[0], p.vec[1]};
        ++count;
    }
    if (count == 0) return std::nullopt;
    return sum * (1.0f / static_cast<float>(count));
}

bool EditorDocument::move_selection_by(vec2 delta, bool merge, const DeltaToLocal& to_local) {
    const std::vector<NodeId> targets = transform_targets();
    if (targets.empty()) return fail("select a node to move (the root can't move)");
    std::vector<std::unique_ptr<Command>> parts;
    for (NodeId id : targets) {
        if (!move_parts(id, to_local ? to_local(id, delta) : delta, parts)) return false;
    }
    const std::string label = targets.size() == 1 ? std::format("Move '{}'", model_.get(targets[0])->name)
                                                  : std::format("Move {} nodes", targets.size());
    apply(std::make_unique<GroupCommand>(std::move(parts), label), merge);
    return true;
}

bool EditorDocument::rotate_selection_by(float radians, std::optional<vec2> pivot, bool merge) {
    const std::vector<NodeId> targets = transform_targets();
    std::vector<std::unique_ptr<Command>> parts;
    const std::optional<vec2> center = pivot ? pivot : selection_pivot();
    const float c = dmath::cos(radians);
    const float s = dmath::sin(radians);
    std::size_t count = 0;
    NodeId last = 0;
    for (NodeId id : targets) {
        if (registry_ != nullptr && registry_->is_a(model_.get(id)->type, "Control")) continue;
        scene::Value rot = field_value(id, "rotation");
        const scene::Value pos = field_value(id, "position");
        if (rot.kind != scene::ValueKind::Float || !is_vec2(pos)) continue;
        ++count;
        last = id;
        const scene::Value before = rot;
        rot.f = static_cast<double>(static_cast<float>(before.f + static_cast<double>(radians)));
        parts.push_back(std::make_unique<SetFieldCommand>(id, "rotation", field_type(id, "rotation"),
                                                          stored_value(*this, id, "rotation"), std::move(rot)));
        const vec2 p{pos.vec[0], pos.vec[1]};
        if (center && !(p == *center)) {
            const vec2 rel = p - *center;
            const vec2 q = *center + vec2{rel.x() * c - rel.y() * s, rel.x() * s + rel.y() * c};
            scene::Value np = pos;
            np.vec = {round_milli(q.x()), round_milli(q.y())};
            parts.push_back(std::make_unique<SetFieldCommand>(id, "position", field_type(id, "position"),
                                                              stored_value(*this, id, "position"), std::move(np)));
        }
    }
    if (count == 0) return fail("select a node that can rotate (not a UI control or the root)");
    const std::string label =
        count == 1 ? std::format("Rotate '{}'", model_.get(last)->name) : std::format("Rotate {} nodes", count);
    apply(std::make_unique<GroupCommand>(std::move(parts), label), merge);
    return true;
}

bool EditorDocument::scale_selection_by(vec2 factor, std::optional<vec2> pivot, bool merge) {
    const std::vector<NodeId> targets = transform_targets();
    std::vector<std::unique_ptr<Command>> parts;
    const std::optional<vec2> center = pivot ? pivot : selection_pivot();
    std::size_t count = 0;
    NodeId last = 0;
    for (NodeId id : targets) {
        if (registry_ != nullptr && registry_->is_a(model_.get(id)->type, "Control")) continue;
        scene::Value sc = field_value(id, "scale");
        const scene::Value pos = field_value(id, "position");
        if (!is_vec2(sc) || !is_vec2(pos)) continue;
        ++count;
        last = id;
        sc.vec = {round_milli(sc.vec[0] * factor.x()), round_milli(sc.vec[1] * factor.y())};
        parts.push_back(std::make_unique<SetFieldCommand>(id, "scale", field_type(id, "scale"),
                                                          stored_value(*this, id, "scale"), std::move(sc)));
        const vec2 p{pos.vec[0], pos.vec[1]};
        if (center && !(p == *center)) {
            const vec2 rel = p - *center;
            scene::Value np = pos;
            np.vec = {round_milli(center->x() + rel.x() * factor.x()), round_milli(center->y() + rel.y() * factor.y())};
            parts.push_back(std::make_unique<SetFieldCommand>(id, "position", field_type(id, "position"),
                                                              stored_value(*this, id, "position"), std::move(np)));
        }
    }
    if (count == 0) return fail("select a node that can scale (not a UI control or the root)");
    const std::string label =
        count == 1 ? std::format("Scale '{}'", model_.get(last)->name) : std::format("Scale {} nodes", count);
    apply(std::make_unique<GroupCommand>(std::move(parts), label), merge);
    return true;
}

bool EditorDocument::duplicate_selection() {
    const std::vector<NodeId> targets = transform_targets();
    if (targets.empty()) return fail("select a node to duplicate (the root can't be duplicated)");
    if (targets.size() == 1) return duplicate(targets[0]) != 0;
    std::vector<std::unique_ptr<Command>> parts;
    std::vector<NodeId> copies;
    for (NodeId id : targets) {
        const ModelNode* n = model_.get(id);
        Subtree t = model_.clone(id);
        t.node.name = model_.unique_child_name(n->parent, n->name);
        copies.push_back(t.node.id);
        parts.push_back(std::make_unique<InsertCommand>(std::move(t), n->parent, model_.index_in_parent(id) + 1,
                                                        std::format("Duplicate '{}'", n->name)));
    }
    apply(std::make_unique<GroupCommand>(std::move(parts), std::format("Duplicate {} nodes", targets.size())));
    select_set(copies);
    return true;
}

bool EditorDocument::remove_selection() {
    const std::vector<NodeId> targets = transform_targets();
    if (targets.empty()) return fail("select a node to delete (the root can't be deleted)");
    if (targets.size() == 1) return remove(targets[0]);
    std::vector<std::unique_ptr<Command>> parts;
    for (NodeId id : targets) parts.push_back(std::make_unique<RemoveCommand>(id));
    apply(std::make_unique<GroupCommand>(std::move(parts), std::format("Delete {} nodes", targets.size())));
    return true;
}

NodeId EditorDocument::group_selection(std::string_view class_name) {
    const std::vector<NodeId> targets = transform_targets();
    if (targets.empty()) return fail("select the nodes to group (the root can't be grouped)"), 0;
    const NodeId parent = model_.get(targets[0])->parent;
    for (NodeId id : targets) {
        if (model_.get(id)->parent != parent) return fail("the nodes to group must share one parent"), 0;
    }
    if (registry_ != nullptr && registry_->find(class_name) == nullptr) {
        return fail(std::format("'{}' is not a node class of this project", class_name)), 0;
    }
    std::size_t first = model_.get(parent)->children.size();
    for (NodeId id : targets) first = std::min(first, model_.index_in_parent(id));
    Subtree t;
    t.node.id = model_.allocate_id();
    t.node.type = std::string(class_name);
    t.node.name = model_.unique_child_name(parent, "Group");
    const NodeId group = t.node.id;
    std::vector<std::unique_ptr<Command>> parts;
    parts.push_back(std::make_unique<InsertCommand>(std::move(t), parent, first, "Add Group"));
    std::size_t k = 0;
    for (NodeId id : targets) parts.push_back(std::make_unique<MoveCommand>(id, group, k++));
    apply(std::make_unique<GroupCommand>(std::move(parts), std::format("Group {} nodes", targets.size())));
    select(group);
    return group;
}

bool EditorDocument::jump_to_history(std::size_t steps_applied) {
    if (steps_applied > stack_.size()) return fail("no such history step");
    while (stack_.index() > steps_applied) {
        if (!undo()) return false;
    }
    while (stack_.index() < steps_applied) {
        if (!redo()) return false;
    }
    return true;
}

} // namespace cuervo::editor

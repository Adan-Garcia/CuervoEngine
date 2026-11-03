#include "editor/tree_view.hpp"

#include <algorithm>
#include <cmath>

namespace cuervo::editor {

void TreeState::set_expanded(NodeId id, bool open) {
    if (open) collapsed_.erase(id);
    else collapsed_.insert(id);
}

void TreeState::reveal(const SceneModel& model, NodeId id) {
    const ModelNode* n = model.get(id);
    for (NodeId p = n != nullptr ? n->parent : 0; p != 0;) {
        collapsed_.erase(p);
        const ModelNode* pn = model.get(p);
        p = pn != nullptr ? pn->parent : 0;
    }
}

std::vector<TreeRow> TreeState::flatten(const SceneModel& model) const {
    std::vector<TreeRow> rows;
    rows.reserve(model.size());
    struct Item {
        NodeId id;
        int depth;
    };
    std::vector<Item> stack;
    if (model.root() != 0) stack.push_back({model.root(), 0});
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        const ModelNode* n = model.get(it.id);
        if (n == nullptr) continue;
        const bool open = !n->children.empty() && expanded(it.id);
        rows.push_back({it.id, it.depth, !n->children.empty(), open});
        if (open) for (auto c = n->children.rbegin(); c != n->children.rend(); ++c) stack.push_back({*c, it.depth + 1});
    }
    return rows;
}

void TreeState::prune(const SceneModel& model) {
    for (auto it = collapsed_.begin(); it != collapsed_.end();) it = model.contains(*it) ? std::next(it) : collapsed_.erase(it);
}

std::string TreeState::save(const SceneModel& model) const {
    std::vector<std::string> paths;
    for (const NodeId id : collapsed_) if (model.contains(id)) paths.push_back(model.path_of(id));
    std::sort(paths.begin(), paths.end());
    std::string out;
    for (const std::string& p : paths) out += (out.empty() ? "" : ";") + p;
    return out;
}

void TreeState::load(const SceneModel& model, std::string_view list) {
    collapsed_.clear();
    while (!list.empty()) {
        const std::size_t cut = list.find(';');
        const std::string_view one = list.substr(0, cut);
        if (!one.empty()) if (const NodeId id = model.find_path(one); id != 0) collapsed_.insert(id);
        list = cut == std::string_view::npos ? std::string_view() : list.substr(cut + 1);
    }
}

RowWindow row_window(std::size_t total, float scroll_top, float viewport, float row_height, std::size_t overscan) {
    if (total == 0 || row_height <= 0.0f) return {};
    const float top = std::max(0.0f, scroll_top);
    const auto first_vis = static_cast<std::size_t>(top / row_height);
    const auto last_vis = static_cast<std::size_t>(std::ceil((top + std::max(0.0f, viewport)) / row_height));
    RowWindow w;
    w.first = first_vis > overscan ? first_vis - overscan : 0;
    w.last = std::min(total, last_vis + overscan);
    w.first = std::min(w.first, total);
    return w;
}

std::size_t row_at(std::size_t total, float y, float row_height) {
    if (total == 0 || row_height <= 0.0f) return 0;
    return std::min(total - 1, static_cast<std::size_t>(std::max(0.0f, y) / row_height));
}

DropTarget drop_target(const SceneModel& model, NodeId dragged, NodeId over, float y) {
    DropTarget t;
    const ModelNode* d = model.get(dragged);
    const ModelNode* o = model.get(over);
    if (d == nullptr || o == nullptr || dragged == model.root() || model.is_ancestor_or_self(dragged, over)) return t;
    const bool sibling_ok = over != model.root();
    DropZone zone = DropZone::Into;
    if (sibling_ok && y < 0.25f) zone = DropZone::Before;
    else if (sibling_ok && y > 0.75f) zone = DropZone::After;
    t.row = over;
    if (zone == DropZone::Into) {
        t.zone = zone;
        t.parent = over;
        // Dropping onto the current parent as last child is a no-op unless it is not already last.
        if (d->parent == over && !o->children.empty() && o->children.back() == dragged) return {};
        return t;
    }
    std::size_t index = model.index_in_parent(over) + (zone == DropZone::After ? 1 : 0);
    if (d->parent == o->parent && model.index_in_parent(dragged) < index) --index; // counted after removal
    if (d->parent == o->parent && model.index_in_parent(dragged) == index) return {};
    t.zone = zone;
    t.parent = o->parent;
    t.index = index;
    return t;
}

bool apply_drop(EditorDocument& doc, NodeId dragged, const DropTarget& t) {
    if (!t.valid()) return false;
    const bool ok = doc.reparent(dragged, t.parent, t.index);
    if (ok) doc.seal();
    return ok;
}

bool move_sibling(EditorDocument& doc, NodeId id, int delta) {
    const ModelNode* n = doc.model().get(id);
    if (n == nullptr || n->parent == 0) return false;
    const std::size_t i = doc.model().index_in_parent(id);
    const std::size_t count = doc.model().get(n->parent)->children.size();
    if ((delta < 0 && i == 0) || (delta > 0 && i + 1 >= count)) return false;
    const bool ok = doc.reparent(id, n->parent, delta < 0 ? i - 1 : i + 1);
    if (ok) doc.seal();
    return ok;
}

std::vector<NodeId> range_between(const std::vector<TreeRow>& rows, NodeId a, NodeId b) {
    std::size_t ia = rows.size(), ib = rows.size();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].id == a) ia = i;
        if (rows[i].id == b) ib = i;
    }
    std::vector<NodeId> out;
    if (ia == rows.size() || ib == rows.size()) return out;
    for (std::size_t i = std::min(ia, ib); i <= std::max(ia, ib); ++i) out.push_back(rows[i].id);
    return out;
}

} // namespace cuervo::editor

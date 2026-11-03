#pragma once
// Scene tree logic for the RmlUi editor (DESIGN.md 4.13, mockup EB1): which rows are visible, which
// slice of them is built (virtualization), and where a dragged node would land. No UI types here, so
// the rules are unit-tested; every structural change still goes through EditorDocument (undoable).

#include <cstddef>
#include <optional>
#include <unordered_set>
#include <vector>

#include "editor/editor_document.hpp"

namespace cuervo::editor {

struct TreeRow {
    NodeId id = 0;
    int depth = 0;
    bool has_children = false;
    bool expanded = false;
};

class TreeState {
public:
    bool expanded(NodeId id) const { return !collapsed_.contains(id); }
    void set_expanded(NodeId id, bool open);
    void toggle(NodeId id) { set_expanded(id, !expanded(id)); }
    // Opens every ancestor of `id` so its row is visible.
    void reveal(const SceneModel& model, NodeId id);
    // Visible rows in order (children of collapsed nodes are skipped). Iterative: depth is unbounded.
    std::vector<TreeRow> flatten(const SceneModel& model) const;
    // Drops collapsed ids of nodes that no longer exist.
    void prune(const SceneModel& model);
    // The collapsed nodes as ';'-separated scene paths (sorted), and the reverse. Ids change between
    // sessions, paths do not; paths that no longer exist are ignored.
    std::string save(const SceneModel& model) const;
    void load(const SceneModel& model, std::string_view list);

private:
    std::unordered_set<NodeId> collapsed_;
};

// The rows [first, last) to build for a viewport: visible rows plus `overscan` on both sides.
struct RowWindow {
    std::size_t first = 0, last = 0;
    friend bool operator==(const RowWindow&, const RowWindow&) = default;
};
RowWindow row_window(std::size_t total, float scroll_top, float viewport, float row_height, std::size_t overscan = 8);
// First row index whose top is at or below `y` after clamping; the row under a point inside the list.
std::size_t row_at(std::size_t total, float y, float row_height);

enum class DropZone { None, Before, Into, After };

struct DropTarget {
    DropZone zone = DropZone::None;
    NodeId row = 0;    // the row the pointer is over
    NodeId parent = 0; // where the node would go
    std::optional<std::size_t> index; // among parent's children after the dragged node leaves; nullopt: last
    bool valid() const { return zone != DropZone::None; }
};

// Upper quarter: before `over`; lower quarter: after it; else into it as the last child (same
// rule as the ImGui tree). Not droppable: the root, onto itself, into its own subtree, or a no-op
// (the node would stay exactly where it is).
DropTarget drop_target(const SceneModel& model, NodeId dragged, NodeId over, float y_fraction);

// Applies a drop through EditorDocument::reparent (one undo step).
bool apply_drop(EditorDocument& doc, NodeId dragged, const DropTarget& t);
// Move up / down among the siblings; false at the ends or for the root.
bool move_sibling(EditorDocument& doc, NodeId id, int delta);

// Pre-order ids from `a` to `b` inclusive in the visible rows (shift-click range); empty if either is hidden.
std::vector<NodeId> range_between(const std::vector<TreeRow>& rows, NodeId a, NodeId b);

} // namespace cuervo::editor

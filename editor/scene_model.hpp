#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/scene/scene_file.hpp"

// The editor's view of one scene (DESIGN.md 4.12, 4.13): the node tree of a
// scene file with stable node ids, so selection and undo survive structural
// edits. It converts losslessly to and from scene::SceneDoc: from_doc then
// to_doc gives back an equal document (same node order, names, fields and
// field order), which is what makes "open, save, byte-identical file" hold.
//
// Fields are exactly the ones the file stores. A node created in the editor
// stores none (every field takes its class default when loaded); editing a
// field adds it. This mirrors how hand-written scenes already store only
// what differs (docs/SCENES.md).
namespace cuervo::editor {

using NodeId = std::uint32_t; // 0 = no node

struct ModelNode {
    NodeId id = 0;
    std::string type; // script class
    std::string name;
    std::vector<scene::Field> fields;
    NodeId parent = 0;
    std::vector<NodeId> children;

    const scene::Field* field(std::string_view field_name) const;
    scene::Field* field(std::string_view field_name);
};

// A detached piece of the tree: a node with its descendants, ids kept, so
// undoing a delete restores the very same ids.
struct Subtree {
    ModelNode node; // `parent` and `children` are ignored; `children` below is the structure
    std::vector<Subtree> children;
};

class SceneModel {
public:
    static SceneModel from_doc(const scene::SceneDoc& doc);
    scene::SceneDoc to_doc() const;

    bool empty() const { return root_ == 0; }
    NodeId root() const { return root_; }
    std::size_t size() const { return nodes_.size(); }

    const ModelNode* get(NodeId id) const;
    ModelNode* get(NodeId id);
    bool contains(NodeId id) const { return get(id) != nullptr; }

    // Nodes in file order (pre-order, children in order).
    std::vector<NodeId> preorder() const;
    // Position of the node in to_doc() order, or -1.
    int doc_index(NodeId id) const;

    // Index of `id` among its parent's children (0 for the root).
    std::size_t index_in_parent(NodeId id) const;
    // True when `ancestor` is `id` or one of its ancestors.
    bool is_ancestor_or_self(NodeId ancestor, NodeId id) const;

    // Paths name nodes by their names from the root: "/" is the root,
    // "/menu/start" a grandchild. A segment "#N" picks the N-th child (for
    // unnamed or duplicate names). Returns 0 if nothing matches.
    NodeId find_path(std::string_view path) const;
    std::string path_of(NodeId id) const;

    // `base`, or `base2`, `base3`... so siblings stay distinguishable.
    std::string unique_child_name(NodeId parent, std::string_view base, NodeId ignore = 0) const;

    // ---- structural primitives (the commands in commands.hpp use these)
    NodeId allocate_id() { return next_id_++; }
    // Builds a fresh subtree copy of `id` with new ids.
    Subtree clone(NodeId id);
    // Copy of `id`'s subtree, ids kept, without removing it.
    Subtree snapshot(NodeId id) const;
    // Removes `id`'s subtree from the tree and returns it. The root can be
    // extracted only when it is the last node (the tree becomes empty).
    Subtree extract(NodeId id);
    // Inserts a subtree as child `index` (clamped) of `parent`; parent 0
    // makes it the root of an empty model.
    bool insert(Subtree tree, NodeId parent, std::size_t index);
    // Moves `id` to child `index` of `parent` (index counted after removal).
    bool move(NodeId id, NodeId parent, std::size_t index);

    friend bool operator==(const SceneModel& a, const SceneModel& b) { return a.to_doc() == b.to_doc(); }

private:
    void collect(NodeId id, std::vector<NodeId>& out) const;

    std::map<NodeId, ModelNode> nodes_;
    NodeId root_ = 0;
    NodeId next_id_ = 1;
};

} // namespace cuervo::editor

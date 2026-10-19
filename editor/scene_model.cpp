#include "editor/scene_model.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace cuervo::editor {

const scene::Field* ModelNode::field(std::string_view field_name) const {
    for (const scene::Field& f : fields) {
        if (f.name == field_name) return &f;
    }
    return nullptr;
}

scene::Field* ModelNode::field(std::string_view field_name) {
    for (scene::Field& f : fields) {
        if (f.name == field_name) return &f;
    }
    return nullptr;
}

SceneModel SceneModel::from_doc(const scene::SceneDoc& doc) {
    SceneModel m;
    std::vector<NodeId> ids;
    ids.reserve(doc.nodes.size());
    for (const scene::Node& n : doc.nodes) {
        ModelNode node;
        node.id = m.allocate_id();
        node.type = n.type;
        node.name = n.name;
        node.fields = n.fields;
        if (n.parent >= 0 && static_cast<std::size_t>(n.parent) < ids.size()) {
            node.parent = ids[static_cast<std::size_t>(n.parent)];
            m.nodes_[node.parent].children.push_back(node.id);
        } else if (m.root_ == 0) {
            m.root_ = node.id;
        } else {
            continue; // a second root: validate() rejects such files before we get here
        }
        ids.push_back(node.id);
        m.nodes_.emplace(node.id, std::move(node));
    }
    return m;
}

void SceneModel::collect(NodeId id, std::vector<NodeId>& out) const {
    const ModelNode* n = get(id);
    if (n == nullptr) return;
    out.push_back(id);
    for (NodeId c : n->children) collect(c, out);
}

std::vector<NodeId> SceneModel::preorder() const {
    std::vector<NodeId> out;
    out.reserve(nodes_.size());
    collect(root_, out);
    return out;
}

scene::SceneDoc SceneModel::to_doc() const {
    scene::SceneDoc doc;
    const std::vector<NodeId> order = preorder();
    std::map<NodeId, std::int32_t> index;
    for (NodeId id : order) {
        const ModelNode& n = nodes_.at(id);
        scene::Node out;
        out.type = n.type;
        out.name = n.name;
        out.fields = n.fields;
        out.parent = n.parent == 0 ? -1 : index.at(n.parent);
        index[id] = static_cast<std::int32_t>(doc.nodes.size());
        doc.nodes.push_back(std::move(out));
    }
    return doc;
}

const ModelNode* SceneModel::get(NodeId id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

ModelNode* SceneModel::get(NodeId id) {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

int SceneModel::doc_index(NodeId id) const {
    const std::vector<NodeId> order = preorder();
    auto it = std::find(order.begin(), order.end(), id);
    return it == order.end() ? -1 : static_cast<int>(it - order.begin());
}

std::size_t SceneModel::index_in_parent(NodeId id) const {
    const ModelNode* n = get(id);
    if (n == nullptr || n->parent == 0) return 0;
    const auto& kids = nodes_.at(n->parent).children;
    return static_cast<std::size_t>(std::find(kids.begin(), kids.end(), id) - kids.begin());
}

bool SceneModel::is_ancestor_or_self(NodeId ancestor, NodeId id) const {
    for (const ModelNode* n = get(id); n != nullptr; n = get(n->parent)) {
        if (n->id == ancestor) return true;
    }
    return false;
}

NodeId SceneModel::find_path(std::string_view path) const {
    if (root_ == 0 || path.empty() || path.front() != '/') return 0;
    NodeId at = root_;
    std::size_t pos = 1;
    while (pos < path.size()) {
        std::size_t end = path.find('/', pos);
        if (end == std::string_view::npos) end = path.size();
        const std::string_view segment = path.substr(pos, end - pos);
        pos = end + 1;
        if (segment.empty()) continue;
        const ModelNode& n = nodes_.at(at);
        NodeId next = 0;
        if (segment.front() == '#') {
            std::size_t i = 0;
            auto [p, ec] = std::from_chars(segment.data() + 1, segment.data() + segment.size(), i);
            if (ec == std::errc() && p == segment.data() + segment.size() && i < n.children.size()) {
                next = n.children[i];
            }
        } else {
            for (NodeId c : n.children) {
                if (nodes_.at(c).name == segment) {
                    next = c;
                    break;
                }
            }
        }
        if (next == 0) return 0;
        at = next;
    }
    return at;
}

std::string SceneModel::path_of(NodeId id) const {
    std::vector<std::string> parts;
    for (const ModelNode* n = get(id); n != nullptr && n->parent != 0; n = get(n->parent)) {
        const ModelNode& p = nodes_.at(n->parent);
        // A name that isn't unique among its siblings (or contains '/') is
        // addressed by position instead.
        const bool unique = !n->name.empty() && n->name.front() != '#' &&
                            n->name.find('/') == std::string::npos &&
                            std::count_if(p.children.begin(), p.children.end(),
                                          [&](NodeId c) { return nodes_.at(c).name == n->name; }) == 1;
        parts.push_back(unique ? n->name : std::format("#{}", index_in_parent(n->id)));
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += "/" + *it;
    return out.empty() ? "/" : out;
}

std::string SceneModel::unique_child_name(NodeId parent, std::string_view base, NodeId ignore) const {
    const ModelNode* p = get(parent);
    if (p == nullptr) return std::string(base);
    auto taken = [&](const std::string& name) {
        return std::any_of(p->children.begin(), p->children.end(),
                           [&](NodeId c) { return c != ignore && nodes_.at(c).name == name; });
    };
    // "Turret3" continues from 3 rather than becoming "Turret32".
    std::string stem(base);
    while (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') stem.pop_back();
    if (stem.empty()) stem = std::string(base);
    std::string name(base);
    for (int i = 2; taken(name); ++i) name = std::format("{}{}", stem, i);
    return name;
}

Subtree SceneModel::snapshot(NodeId id) const {
    Subtree t;
    const ModelNode* n = get(id);
    if (n == nullptr) return t;
    t.node = *n;
    for (NodeId c : n->children) t.children.push_back(snapshot(c));
    return t;
}

Subtree SceneModel::clone(NodeId id) {
    Subtree t = snapshot(id);
    auto renumber = [&](auto& self, Subtree& s) -> void {
        s.node.id = allocate_id();
        for (Subtree& c : s.children) self(self, c);
    };
    renumber(renumber, t);
    return t;
}

Subtree SceneModel::extract(NodeId id) {
    Subtree t = snapshot(id);
    ModelNode* n = get(id);
    if (n == nullptr) return t;
    if (n->parent != 0) {
        auto& kids = nodes_.at(n->parent).children;
        kids.erase(std::remove(kids.begin(), kids.end(), id), kids.end());
    } else {
        root_ = 0;
    }
    std::vector<NodeId> gone;
    collect(id, gone);
    for (NodeId g : gone) nodes_.erase(g);
    return t;
}

bool SceneModel::insert(Subtree tree, NodeId parent, std::size_t index) {
    if (parent == 0 ? root_ != 0 : get(parent) == nullptr) return false;
    auto add = [&](auto& self, Subtree& s, NodeId p) -> void {
        ModelNode node = std::move(s.node);
        node.parent = p;
        node.children.clear();
        for (const Subtree& c : s.children) node.children.push_back(c.node.id);
        next_id_ = std::max(next_id_, node.id + 1);
        const NodeId id = node.id;
        nodes_[id] = std::move(node);
        for (Subtree& c : s.children) self(self, c, id);
    };
    const NodeId id = tree.node.id;
    add(add, tree, parent);
    if (parent == 0) {
        root_ = id;
    } else {
        auto& kids = nodes_.at(parent).children;
        kids.insert(kids.begin() + static_cast<std::ptrdiff_t>(std::min(index, kids.size())), id);
    }
    return true;
}

bool SceneModel::move(NodeId id, NodeId parent, std::size_t index) {
    ModelNode* n = get(id);
    ModelNode* p = get(parent);
    if (n == nullptr || p == nullptr || n->parent == 0 || is_ancestor_or_self(id, parent)) return false;
    auto& old_kids = nodes_.at(n->parent).children;
    old_kids.erase(std::remove(old_kids.begin(), old_kids.end(), id), old_kids.end());
    n->parent = parent;
    p->children.insert(p->children.begin() + static_cast<std::ptrdiff_t>(std::min(index, p->children.size())), id);
    return true;
}

} // namespace cuervo::editor

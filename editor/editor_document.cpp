#include "editor/editor_document.hpp"

#include <format>
#include <utility>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"

namespace cuervo::editor {

bool EditorDocument::fail(std::string message) {
    error_ = std::move(message);
    return false;
}

void EditorDocument::apply(std::unique_ptr<Command> command, bool merge) {
    const NodeId focus = stack_.execute(model_, std::move(command), merge);
    focus_on(focus);
}

void EditorDocument::focus_on(NodeId focus) {
    // A node of the current (multi-)selection keeps the selection as it is.
    if (focus != 0 && model_.contains(focus)) {
        if (!is_selected(focus)) select(focus);
    } else {
        prune_selection();
    }
}

// ------------------------------------------------------------ files

void EditorDocument::new_scene(std::string root_class, std::string root_name) {
    scene::SceneDoc doc;
    doc.nodes.push_back({std::move(root_class), std::move(root_name), -1, {}});
    model_ = SceneModel::from_doc(doc);
    stack_.clear();
    stack_.mark_saved();
    select(model_.root());
    path_.clear();
    ++reloads_;
}

bool EditorDocument::open(const std::filesystem::path& path, std::string& error) {
    scene::LoadInfo info;
    auto doc = scene::load_file(path, error, &info);
    if (!doc) return false;
    for (const std::string& m : info.migrated) LOG_INFO("'{}': migrated ({})", platform::utf8(path), m);
    model_ = SceneModel::from_doc(*doc);
    stack_.clear();
    stack_.mark_saved();
    select(model_.root());
    path_ = path;
    ++reloads_;
    return true;
}

bool EditorDocument::save(std::string& error) {
    if (path_.empty()) {
        error = "the scene has no file yet: use Save As";
        return false;
    }
    std::error_code ec;
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), ec);
    if (!scene::save_file(path_, model_.to_doc(), error)) return false;
    stack_.mark_saved();
    return true;
}

bool EditorDocument::save_as(const std::filesystem::path& path, std::string& error) {
    const std::filesystem::path old = path_;
    path_ = path;
    if (save(error)) return true;
    path_ = old;
    return false;
}

// ------------------------------------------------------------ edits

NodeId EditorDocument::add_child(NodeId parent, std::string_view class_name, std::string_view name,
                                 std::vector<scene::Field> fields) {
    if (!model_.contains(parent)) return fail("no such parent node"), 0;
    if (registry_ != nullptr && registry_->find(class_name) == nullptr) {
        return fail(std::format("'{}' is not a node class of this project", class_name)), 0;
    }
    Subtree t;
    t.node.id = model_.allocate_id();
    t.node.type = std::string(class_name);
    t.node.name = model_.unique_child_name(parent, name.empty() ? class_name : name);
    t.node.fields = std::move(fields);
    const std::size_t index = model_.get(parent)->children.size();
    apply(std::make_unique<InsertCommand>(std::move(t), parent, index, std::format("Add {}", class_name)));
    return selection_;
}

NodeId EditorDocument::instance_scene(NodeId parent, std::string_view scene, std::optional<vec2> position,
                                      std::string_view self_path) {
    if (!scene.ends_with(".scene")) return fail(std::format("'{}' is not a scene file", scene)), 0;
    if (!self_path.empty() && scene == self_path) return fail("a scene can't instance itself"), 0;
    // The node is named after the file ("enemy.scene" -> "enemy").
    std::string_view stem = scene.substr(scene.find_last_of('/') + 1);
    stem.remove_suffix(6);
    std::vector<scene::Field> fields;
    fields.push_back({"scene", "str", scene::Value::str(std::string(scene))});
    if (position) fields.push_back({"position", "vec2", scene::Value::vector({position->x(), position->y()})});
    return add_child(parent, "SceneInstance", stem, std::move(fields));
}

bool EditorDocument::remove(NodeId id) {
    if (!model_.contains(id)) return fail("no such node");
    if (id == model_.root()) return fail("the root node can't be deleted");
    apply(std::make_unique<RemoveCommand>(id));
    return true;
}

bool EditorDocument::rename(NodeId id, std::string_view name) {
    const ModelNode* n = model_.get(id);
    if (n == nullptr) return fail("no such node");
    if (name.empty()) return fail("a node needs a name");
    if (n->name == name) return true;
    apply(std::make_unique<RenameCommand>(id, n->name, std::string(name)));
    return true;
}

bool EditorDocument::reparent(NodeId id, NodeId new_parent, std::optional<std::size_t> index) {
    const ModelNode* n = model_.get(id);
    const ModelNode* p = model_.get(new_parent);
    if (n == nullptr || p == nullptr) return fail("no such node");
    if (id == model_.root()) return fail("the root node can't be moved");
    if (model_.is_ancestor_or_self(id, new_parent)) return fail("a node can't move under itself");
    std::size_t at = index.value_or(p->children.size());
    if (n->parent == new_parent && !index) at = p->children.size() - 1;
    if (n->parent == new_parent && at == model_.index_in_parent(id)) return true;
    apply(std::make_unique<MoveCommand>(id, new_parent, at));
    return true;
}

NodeId EditorDocument::duplicate(NodeId id) {
    const ModelNode* n = model_.get(id);
    if (n == nullptr) return fail("no such node"), 0;
    if (id == model_.root()) return fail("the root node can't be duplicated"), 0;
    const NodeId parent = n->parent;
    Subtree t = model_.clone(id);
    t.node.name = model_.unique_child_name(parent, n->name);
    const std::size_t index = model_.index_in_parent(id) + 1;
    apply(std::make_unique<InsertCommand>(std::move(t), parent, index, std::format("Duplicate '{}'", n->name)));
    return selection_;
}

std::string EditorDocument::field_type(NodeId id, std::string_view field) const {
    const ModelNode* n = model_.get(id);
    if (n == nullptr) return {};
    if (registry_ != nullptr) {
        if (const ClassSpec* c = registry_->find(n->type)) {
            if (const FieldSpec* f = c->field(field)) return f->type;
        }
    }
    const scene::Field* stored = n->field(field);
    return stored != nullptr ? stored->type : std::string();
}

scene::Value EditorDocument::field_value(NodeId id, std::string_view field, bool* stored) const {
    if (stored != nullptr) *stored = false;
    const ModelNode* n = model_.get(id);
    if (n == nullptr) return {};
    if (const scene::Field* f = n->field(field)) {
        if (stored != nullptr) *stored = true;
        return f->value;
    }
    if (registry_ != nullptr) {
        if (const ClassSpec* c = registry_->find(n->type)) {
            if (const FieldSpec* f = c->field(field)) return f->default_value;
        }
    }
    return {};
}

bool EditorDocument::set_field(NodeId id, std::string_view field, scene::Value value, bool merge) {
    const ModelNode* n = model_.get(id);
    if (n == nullptr) return fail("no such node");
    if (field == "name") {
        if (value.kind != scene::ValueKind::Str) return fail("name is a string");
        return rename(id, value.text);
    }
    const std::string type = field_type(id, field);
    if (type.empty()) return fail(std::format("'{}' has no exported field '{}'", n->type, field));
    bool stored = false;
    const scene::Value current = field_value(id, field, &stored);
    std::string why;
    if (!coerce_value(value, current, registry_, why)) return fail(std::format("{}: {}", field, why));
    if (stored && current == value) return true;
    std::optional<scene::Value> before;
    if (stored) before = current;
    apply(std::make_unique<SetFieldCommand>(id, std::string(field), type, before, std::move(value)), merge);
    return true;
}

bool EditorDocument::reset_field(NodeId id, std::string_view field) {
    const ModelNode* n = model_.get(id);
    if (n == nullptr) return fail("no such node");
    const scene::Field* f = n->field(field);
    if (f == nullptr) return true; // already the default
    apply(std::make_unique<SetFieldCommand>(id, f->name, f->type, f->value, std::nullopt));
    return true;
}

bool EditorDocument::undo() {
    auto focus = stack_.undo(model_);
    if (!focus) return false;
    focus_on(*focus);
    return true;
}

bool EditorDocument::redo() {
    auto focus = stack_.redo(model_);
    if (!focus) return false;
    focus_on(*focus);
    return true;
}

} // namespace cuervo::editor

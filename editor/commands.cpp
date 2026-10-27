#include "editor/commands.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace cuervo::editor {

// ------------------------------------------------------------ SetField

SetFieldCommand::SetFieldCommand(NodeId node, std::string field, std::string type,
                                 std::optional<scene::Value> before, std::optional<scene::Value> after)
    : node_(node), field_(std::move(field)), type_(std::move(type)), before_(std::move(before)),
      after_(std::move(after)) {}

void SetFieldCommand::set(SceneModel& model, const std::optional<scene::Value>& value) {
    ModelNode* n = model.get(node_);
    if (n == nullptr) return;
    auto it = std::find_if(n->fields.begin(), n->fields.end(), [&](const scene::Field& f) { return f.name == field_; });
    if (it != n->fields.end()) {
        position_ = static_cast<std::size_t>(it - n->fields.begin());
        if (value) {
            it->value = *value;
        } else {
            n->fields.erase(it);
        }
        return;
    }
    if (!value) return;
    // A new field goes last; one restored by undo goes back where it was.
    const std::size_t at = std::min(position_, n->fields.size());
    n->fields.insert(n->fields.begin() + static_cast<std::ptrdiff_t>(at), scene::Field{field_, type_, *value});
}

void SetFieldCommand::apply(SceneModel& model) { set(model, after_); }
void SetFieldCommand::revert(SceneModel& model) { set(model, before_); }

std::string SetFieldCommand::label() const {
    return after_ ? std::format("Set {}", field_) : std::format("Reset {}", field_);
}

bool SetFieldCommand::merge(const Command& next) {
    const auto* other = dynamic_cast<const SetFieldCommand*>(&next);
    if (other == nullptr || other->node_ != node_ || other->field_ != field_) return false;
    after_ = other->after_;
    return true;
}

// ------------------------------------------------------------ Rename

RenameCommand::RenameCommand(NodeId node, std::string before, std::string after)
    : node_(node), before_(std::move(before)), after_(std::move(after)) {}

void RenameCommand::apply(SceneModel& model) {
    if (ModelNode* n = model.get(node_)) n->name = after_;
}
void RenameCommand::revert(SceneModel& model) {
    if (ModelNode* n = model.get(node_)) n->name = before_;
}
std::string RenameCommand::label() const { return std::format("Rename to '{}'", after_); }

ChangeTypeCommand::ChangeTypeCommand(NodeId node, std::string before, std::string after)
    : node_(node), before_(std::move(before)), after_(std::move(after)) {}

void ChangeTypeCommand::apply(SceneModel& model) {
    if (ModelNode* n = model.get(node_)) n->type = after_;
}
void ChangeTypeCommand::revert(SceneModel& model) {
    if (ModelNode* n = model.get(node_)) n->type = before_;
}
std::string ChangeTypeCommand::label() const { return std::format("Change type of node to '{}'", after_); }

// ------------------------------------------------------------ Insert

InsertCommand::InsertCommand(Subtree tree, NodeId parent, std::size_t index, std::string label)
    : tree_(std::move(tree)), id_(tree_.node.id), parent_(parent), index_(index), label_(std::move(label)) {}

void InsertCommand::apply(SceneModel& model) { model.insert(tree_, parent_, index_); }
void InsertCommand::revert(SceneModel& model) { tree_ = model.extract(id_); }

// ------------------------------------------------------------ Remove

RemoveCommand::RemoveCommand(NodeId node) : node_(node) {}

void RemoveCommand::apply(SceneModel& model) {
    const ModelNode* n = model.get(node_);
    if (n == nullptr) return;
    parent_ = n->parent;
    index_ = model.index_in_parent(node_);
    tree_ = model.extract(node_);
}
void RemoveCommand::revert(SceneModel& model) { model.insert(tree_, parent_, index_); }
std::string RemoveCommand::label() const { return std::format("Delete '{}'", tree_.node.name); }

// ------------------------------------------------------------ Move

MoveCommand::MoveCommand(NodeId node, NodeId parent, std::size_t index) : node_(node), parent_(parent), index_(index) {}

void MoveCommand::apply(SceneModel& model) {
    const ModelNode* n = model.get(node_);
    if (n == nullptr) return;
    old_parent_ = n->parent;
    old_index_ = model.index_in_parent(node_);
    model.move(node_, parent_, index_);
}
void MoveCommand::revert(SceneModel& model) { model.move(node_, old_parent_, old_index_); }

// ------------------------------------------------------------ Group

GroupCommand::GroupCommand(std::vector<std::unique_ptr<Command>> parts, std::string label)
    : parts_(std::move(parts)), label_(std::move(label)) {}

void GroupCommand::apply(SceneModel& model) {
    for (auto& p : parts_) p->apply(model);
}

void GroupCommand::revert(SceneModel& model) {
    for (auto it = parts_.rbegin(); it != parts_.rend(); ++it) (*it)->revert(model);
}

bool GroupCommand::merge(const Command& next) {
    const auto* other = dynamic_cast<const GroupCommand*>(&next);
    if (other == nullptr || other->parts_.size() != parts_.size()) return false;
    // Only groups of field edits with the same targets merge (the only
    // groups the editor builds), so either every part folds or none does.
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        const auto* a = dynamic_cast<const SetFieldCommand*>(parts_[i].get());
        const auto* b = dynamic_cast<const SetFieldCommand*>(other->parts_[i].get());
        if (a == nullptr || b == nullptr || !a->same_target(*b)) return false;
    }
    for (std::size_t i = 0; i < parts_.size(); ++i) parts_[i]->merge(*other->parts_[i]);
    return true;
}

NodeId GroupCommand::focus_after_apply() const {
    return parts_.empty() ? 0 : parts_.front()->focus_after_apply();
}

NodeId GroupCommand::focus_after_revert() const {
    return parts_.empty() ? 0 : parts_.front()->focus_after_revert();
}

// ------------------------------------------------------------ stack

NodeId CommandStack::execute(SceneModel& model, std::unique_ptr<Command> command, bool mergeable) {
    command->apply(model);
    ++revision_;
    const NodeId focus = command->focus_after_apply();
    if (mergeable && merge_open_ && index_ == commands_.size() && index_ > 0 &&
        commands_[index_ - 1]->merge(*command)) {
        // The merged step no longer matches what was saved, if it was the save point.
        if (saved_ && *saved_ == index_) saved_.reset();
        return focus;
    }
    commands_.resize(index_); // drop the redo tail
    if (saved_ && *saved_ > index_) saved_.reset();
    commands_.push_back(std::move(command));
    ++index_;
    merge_open_ = mergeable;
    return focus;
}

std::optional<NodeId> CommandStack::undo(SceneModel& model) {
    merge_open_ = false;
    if (!can_undo()) return std::nullopt;
    Command& c = *commands_[--index_];
    c.revert(model);
    ++revision_;
    return c.focus_after_revert();
}

std::optional<NodeId> CommandStack::redo(SceneModel& model) {
    merge_open_ = false;
    if (!can_redo()) return std::nullopt;
    Command& c = *commands_[index_++];
    c.apply(model);
    ++revision_;
    return c.focus_after_apply();
}

void CommandStack::clear() {
    commands_.clear();
    index_ = 0;
    saved_ = 0;
    merge_open_ = false;
    ++revision_;
}

} // namespace cuervo::editor

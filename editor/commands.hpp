#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "editor/scene_model.hpp"

// Undo/redo (DESIGN.md 4.13): every edit of a scene is a Command on a
// CommandStack. A command applies itself to a SceneModel and reverts
// exactly, so undo after redo after undo always lands on the same model.
//
// Merging: continuous edits (dragging a value or the move gizmo) push one
// command per frame with `mergeable` set; while the merge window is open the
// stack folds each into the previous command when that command accepts it
// (same node and field), so one drag is one undo step. `seal()` closes the
// window (mouse released, field deactivated).
namespace cuervo::editor {

class Command {
public:
    virtual ~Command() = default;
    virtual void apply(SceneModel& model) = 0;
    virtual void revert(SceneModel& model) = 0;
    virtual std::string label() const = 0;
    // Folds `next` (already applied) into this command; false = keep separate.
    virtual bool merge(const Command& next) {
        (void)next;
        return false;
    }
    // The node to select after apply / after revert (0 = leave the selection).
    virtual NodeId focus_after_apply() const { return 0; }
    virtual NodeId focus_after_revert() const { return 0; }
};

// Sets (or, with nullopt, removes) one stored field of a node.
class SetFieldCommand final : public Command {
public:
    SetFieldCommand(NodeId node, std::string field, std::string type, std::optional<scene::Value> before,
                    std::optional<scene::Value> after);
    void apply(SceneModel& model) override;
    void revert(SceneModel& model) override;
    std::string label() const override;
    bool merge(const Command& next) override;
    NodeId focus_after_apply() const override { return node_; }
    NodeId focus_after_revert() const override { return node_; }
    bool same_target(const SetFieldCommand& other) const { return node_ == other.node_ && field_ == other.field_; }

private:
    void set(SceneModel& model, const std::optional<scene::Value>& value);

    NodeId node_;
    std::string field_;
    std::string type_;
    std::optional<scene::Value> before_;
    std::optional<scene::Value> after_;
    // Where the field sat in the node's list before (kept on undo, so a
    // revert restores the file's field order exactly).
    std::size_t position_ = static_cast<std::size_t>(-1); // -1: append
};

class RenameCommand final : public Command {
public:
    RenameCommand(NodeId node, std::string before, std::string after);
    void apply(SceneModel& model) override;
    void revert(SceneModel& model) override;
    std::string label() const override;
    NodeId focus_after_apply() const override { return node_; }
    NodeId focus_after_revert() const override { return node_; }

private:
    NodeId node_;
    std::string before_;
    std::string after_;
};

class ChangeTypeCommand final : public Command {
public:
    ChangeTypeCommand(NodeId node, std::string before, std::string after);
    void apply(SceneModel& model) override;
    void revert(SceneModel& model) override;
    std::string label() const override;
    NodeId focus_after_apply() const override { return node_; }
    NodeId focus_after_revert() const override { return node_; }

private:
    NodeId node_;
    std::string before_;
    std::string after_;
};

// Adds a subtree (a new node, or a duplicate) as child `index` of `parent`.
class InsertCommand final : public Command {
public:
    InsertCommand(Subtree tree, NodeId parent, std::size_t index, std::string label);
    void apply(SceneModel& model) override;
    void revert(SceneModel& model) override;
    std::string label() const override { return label_; }
    NodeId focus_after_apply() const override { return id_; }
    NodeId focus_after_revert() const override { return parent_; }

private:
    Subtree tree_;
    NodeId id_;
    NodeId parent_;
    std::size_t index_;
    std::string label_;
};

// Deletes a node and its descendants.
class RemoveCommand final : public Command {
public:
    explicit RemoveCommand(NodeId node);
    void apply(SceneModel& model) override;
    void revert(SceneModel& model) override;
    std::string label() const override;
    NodeId focus_after_apply() const override { return parent_; }
    NodeId focus_after_revert() const override { return node_; }

private:
    NodeId node_;
    NodeId parent_ = 0;
    std::size_t index_ = 0;
    Subtree tree_;
};

// Reparents / reorders: `node` becomes child `index` of `parent`.
class MoveCommand final : public Command {
public:
    MoveCommand(NodeId node, NodeId parent, std::size_t index);
    void apply(SceneModel& model) override;
    void revert(SceneModel& model) override;
    std::string label() const override { return "Reparent"; }
    NodeId focus_after_apply() const override { return node_; }
    NodeId focus_after_revert() const override { return node_; }

private:
    NodeId node_;
    NodeId parent_;
    std::size_t index_;
    NodeId old_parent_ = 0;
    std::size_t old_index_ = 0;
};

// Several commands as one undo step (e.g. moving a Control shifts both of
// its offsets). Merges part by part with a group of the same shape.
class GroupCommand final : public Command {
public:
    GroupCommand(std::vector<std::unique_ptr<Command>> parts, std::string label);
    void apply(SceneModel& model) override;
    void revert(SceneModel& model) override;
    std::string label() const override { return label_; }
    bool merge(const Command& next) override;
    NodeId focus_after_apply() const override;
    NodeId focus_after_revert() const override;

private:
    std::vector<std::unique_ptr<Command>> parts_;
    std::string label_;
};

class CommandStack {
public:
    // Applies `command` and records it. Returns the node the command wants
    // selected (0 = none). A mergeable command folds into the previous one
    // while the merge window is open and that command accepts it.
    NodeId execute(SceneModel& model, std::unique_ptr<Command> command, bool mergeable = false);
    // Closes the merge window: the next command starts a new undo step.
    void seal() { merge_open_ = false; }

    bool can_undo() const { return index_ > 0; }
    bool can_redo() const { return index_ < commands_.size(); }
    // Each returns the node to select (0 = none), or nullopt if nothing to do.
    std::optional<NodeId> undo(SceneModel& model);
    std::optional<NodeId> redo(SceneModel& model);

    void clear();
    std::size_t size() const { return commands_.size(); }
    std::size_t index() const { return index_; } // commands applied
    // The label of the i-th recorded step (0 = oldest); steps [0, index()) are applied.
    std::string label_at(std::size_t i) const { return i < commands_.size() ? commands_[i]->label() : std::string(); }
    std::string undo_label() const { return can_undo() ? commands_[index_ - 1]->label() : std::string(); }
    std::string redo_label() const { return can_redo() ? commands_[index_]->label() : std::string(); }

    // Dirty tracking against the last save.
    void mark_saved() { saved_ = index_; }
    bool dirty() const { return !saved_ || *saved_ != index_; }

    // Changes on every apply/undo/redo/merge (the scene view rebuilds then).
    std::uint64_t revision() const { return revision_; }

private:
    std::vector<std::unique_ptr<Command>> commands_;
    std::size_t index_ = 0;
    std::optional<std::size_t> saved_ = 0; // nullopt: the saved state is unreachable
    bool merge_open_ = false;
    std::uint64_t revision_ = 0;
};

} // namespace cuervo::editor

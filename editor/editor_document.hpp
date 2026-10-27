#pragma once

#include <functional>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/math/vec.hpp"
#include "editor/class_registry.hpp"
#include "editor/commands.hpp"
#include "editor/scene_model.hpp"

// One open scene in the editor: the model, its undo stack, the selection and
// the file it came from. Every edit the UI or a command script makes goes
// through the operations here, so both paths validate the same way and
// every edit is one undoable command (DESIGN.md 4.13).
namespace cuervo::editor {

class EditorDocument {
public:
    explicit EditorDocument(const ClassRegistry* registry = nullptr) : registry_(registry) {}

    void set_registry(const ClassRegistry* registry) { registry_ = registry; }
    const ClassRegistry* registry() const { return registry_; }

    // ---- files (not undoable: they replace the document)
    void new_scene(std::string root_class = "Node2D", std::string root_name = "root");
    bool open(const std::filesystem::path& path, std::string& error);
    // Writes the current format (scene::write_binary). An opened file saved
    // without edits is byte-identical (tests/editor).
    bool save(std::string& error);
    bool save_as(const std::filesystem::path& path, std::string& error);
    const std::filesystem::path& path() const { return path_; }
    bool has_path() const { return !path_.empty(); }
    bool dirty() const { return stack_.dirty(); }

    // ---- state
    const SceneModel& model() const { return model_; }
    const CommandStack& stack() const { return stack_; }
    // The primary selection (the inspector's node; the last one picked), or 0.
    NodeId selection() const { return selection_; }
    // Every selected node, in the order they were picked (the primary last).
    const std::vector<NodeId>& selected() const { return selected_; }
    bool is_selected(NodeId id) const;
    // Selects just `id` (0 or unknown: nothing).
    void select(NodeId id);
    // Ctrl-click: adds `id` to the selection, or removes it when it is in.
    void select_toggle(NodeId id);
    void select_add(NodeId id);
    // Replaces the selection (unknown ids are dropped); the last becomes the primary.
    void select_set(const std::vector<NodeId>& ids);
    void select_clear();
    // What a group transform acts on: the selection without the root and without
    // nodes whose ancestor is selected too (the root can't move, DESIGN.md 4.13).
    std::vector<NodeId> transform_targets() const;
    std::uint64_t revision() const { return stack_.revision() + reloads_; }
    // Last validation error of an operation that returned false/0.
    const std::string& error() const { return error_; }

    // ---- edits (each one undoable command; false/0 = rejected, see error())
    // `fields` (optional) are stored on the new node, in one undo step.
    NodeId add_child(NodeId parent, std::string_view class_name, std::string_view name = {},
                     std::vector<scene::Field> fields = {});
    bool remove(NodeId id);
    bool rename(NodeId id, std::string_view name);
    bool change_type(NodeId id, std::string_view new_type);
    // `index` among the new parent's children (after removal); nullopt = last.
    bool reparent(NodeId id, NodeId new_parent, std::optional<std::size_t> index = std::nullopt);
    NodeId duplicate(NodeId id);
    // Instances a scene file as a child (a SceneInstance node, DESIGN.md
    // 4.13 "Clarified (Phase 6B)") named after the file, at `position` if
    // given. `scene` is project-relative; a scene can't instance itself.
    NodeId instance_scene(NodeId parent, std::string_view scene, std::optional<vec2> position = std::nullopt,
                          std::string_view self_path = {});
    // Sets a field by name; the value is coerced to the field's type
    // (coerce_value). `merge`: part of a continuous drag (seal() ends it).
    bool set_field(NodeId id, std::string_view field, scene::Value value, bool merge = false);
    // Removes the stored field so the class default applies again.
    bool reset_field(NodeId id, std::string_view field);
    // The move gizmo: shifts a Control's offsets or a Node2D's position by
    // `delta` world units. Merged while dragging.
    bool move_by(NodeId id, vec2 delta, bool merge = true);
    // ---- group transforms (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): each acts on
    // transform_targets() as ONE undo step, merged while dragging. Controls move
    // their offsets; rotating and scaling skip them (layout owns their rect).
    // `pivot` (world units) defaults to the targets' centroid; a single node turns
    // and scales about its own position.
    // `delta` is in world units. With the transform hierarchy (DESIGN.md 4.17)
    // a node's `position` is parent-relative: `to_local(id, delta)` turns the
    // world delta into that node's local one (empty: used as is).
    using DeltaToLocal = std::function<vec2(NodeId, vec2)>;
    bool move_selection_by(vec2 delta, bool merge = true, const DeltaToLocal& to_local = {});
    bool rotate_selection_by(float radians, std::optional<vec2> pivot = std::nullopt, bool merge = true);
    bool scale_selection_by(vec2 factor, std::optional<vec2> pivot = std::nullopt, bool merge = true);
    // The centroid of the transformable targets' positions.
    std::optional<vec2> selection_pivot() const;
    // Duplicates / deletes every target as one step; the copies become the selection.
    bool duplicate_selection();
    bool remove_selection();
    // Wraps the targets (they must share a parent) in a new node of `class_name`.
    NodeId group_selection(std::string_view class_name = "Node2D");
    // Undo or redo until `steps_applied` steps are applied (the history panel).
    bool jump_to_history(std::size_t steps_applied);
    void seal() { stack_.seal(); }
    bool undo();
    bool redo();

    // A field's effective value: stored, else the class default (Null if
    // unknown). `stored` tells which.
    scene::Value field_value(NodeId id, std::string_view field, bool* stored = nullptr) const;
    // The declared type of a field (from the class, else from the file).
    std::string field_type(NodeId id, std::string_view field) const;

private:
    bool fail(std::string message);
    void prune_selection();
    void focus_on(NodeId focus);
    // The SetFieldCommands that shift a node (position, or both offsets of a Control).
    bool move_parts(NodeId id, vec2 delta, std::vector<std::unique_ptr<Command>>& parts);
    void apply(std::unique_ptr<Command> command, bool merge = false);

    const ClassRegistry* registry_ = nullptr;
    SceneModel model_;
    CommandStack stack_;
    NodeId selection_ = 0;
    std::vector<NodeId> selected_;
    std::filesystem::path path_;
    std::string error_;
    std::uint64_t reloads_ = 0; // bumps revision() on new/open
};

} // namespace cuervo::editor

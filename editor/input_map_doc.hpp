#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cuervo/input/devices.hpp"
#include "cuervo/script/project_edit.hpp"

// The input-map editor's model (DESIGN.md 4.8, 4.13): the project's
// [input.actions] and [input.axes] as edited in the editor, validated like
// project.toml is when a game loads, with its own undo stack (snapshots:
// the map is small), saved back into project.toml with
// script::write_project_input so the rest of the file -- comments
// included -- is kept.
//
// Note that an action's index is its position in the file and replays
// store held actions by index: removing or adding actions makes older
// replays of the project refuse to play (cuervo/input/replay.hpp).
namespace cuervo::editor {

class InputMapDoc {
public:
    bool load(const std::filesystem::path& project_dir, std::string& error);
    bool save(std::string& error);
    bool loaded() const { return !dir_.empty(); }
    bool dirty() const { return saved_ != index_; }

    const std::vector<script::InputEntry>& actions() const { return current().actions; }
    const std::vector<script::InputEntry>& axes() const { return current().axes; }
    const script::InputEntry* action(std::string_view name) const;
    const script::InputEntry* axis(std::string_view name) const;

    // ---- edits (each one undo step; false = rejected, see error())
    bool add_action(std::string_view name);
    bool remove_action(std::string_view name);
    bool rename_action(std::string_view name, std::string_view new_name);
    // `binding` as project.toml spells it ("Key.Z", "Pad.South", "Mouse.Left").
    bool bind(std::string_view action, std::string_view binding);
    bool unbind(std::string_view action, std::string_view binding);
    bool add_axis(std::string_view name);
    bool remove_axis(std::string_view name);
    // `source`: "Keys.Arrows", "Keys.WASD", "Pad.LeftStick", "Pad.RightStick", "Pad.Dpad".
    bool add_axis_source(std::string_view axis, std::string_view source);
    bool remove_axis_source(std::string_view axis, std::string_view source);

    bool undo();
    bool redo();
    bool can_undo() const { return index_ > 0; }
    bool can_redo() const { return index_ + 1 < history_.size(); }
    const std::string& error() const { return error_; }

private:
    const script::InputSections& current() const { return history_[index_]; }
    bool commit(script::InputSections next);
    bool fail(std::string message);

    std::filesystem::path dir_;
    std::vector<std::pair<std::string, std::string>> file_names_; // entry id -> its name in project.toml
    std::size_t next_id_ = 0;
    std::vector<script::InputSections> history_{script::InputSections{}};
    std::size_t index_ = 0;
    std::size_t saved_ = 0;
    std::string error_;
};

// "Press a key" capture: the first binding pressed in `devices` since the
// last sample (a key, mouse button, pad button, or a stick/trigger pushed
// past half way), as project.toml spells it; nullopt if nothing was.
std::optional<std::string> captured_binding(const DeviceState& devices);

} // namespace cuervo::editor

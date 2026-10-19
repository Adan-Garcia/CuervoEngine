#include "editor/input_map_doc.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

#include "cuervo/input/input_map.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/script/project.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

// In the history, InputEntry::original holds a stable id per entry (so undo
// works across saves); file_names_ maps an id to the name the entry has in
// project.toml now (absent: not in the file).
namespace {

std::vector<script::InputEntry>::const_iterator find_entry(const std::vector<script::InputEntry>& list,
                                                           std::string_view name) {
    return std::find_if(list.begin(), list.end(), [&](const script::InputEntry& e) { return e.name == name; });
}

} // namespace

bool InputMapDoc::fail(std::string message) {
    error_ = std::move(message);
    return false;
}

bool InputMapDoc::load(const fs::path& project_dir, std::string& error) {
    std::ifstream in(project_dir / "project.toml", std::ios::binary);
    if (!in) {
        error = std::format("cannot read '{}'", platform::utf8(project_dir / "project.toml"));
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    script::InputSections s = script::read_input_sections(text);
    file_names_.clear();
    for (auto* list : {&s.actions, &s.axes}) {
        for (script::InputEntry& e : *list) {
            e.original = std::format("f{}", next_id_++);
            file_names_.emplace_back(e.original, e.name);
        }
    }
    dir_ = project_dir;
    history_ = {std::move(s)};
    index_ = 0;
    saved_ = 0;
    return true;
}

bool InputMapDoc::save(std::string& error) {
    // The file's view: ids become the names the entries have in the file.
    script::InputSections out = current();
    auto file_name = [&](const std::string& id) -> std::string {
        for (const auto& [i, n] : file_names_) {
            if (i == id) return n;
        }
        return {};
    };
    for (auto* list : {&out.actions, &out.axes}) {
        for (script::InputEntry& e : *list) e.original = file_name(e.original);
    }
    if (!script::write_project_input(dir_, out, error)) return false;
    file_names_.clear();
    for (const auto* list : {&current().actions, &current().axes}) {
        for (const script::InputEntry& e : *list) file_names_.emplace_back(e.original, e.name);
    }
    saved_ = index_;
    return true;
}

const script::InputEntry* InputMapDoc::action(std::string_view name) const {
    auto it = find_entry(actions(), name);
    return it == actions().end() ? nullptr : &*it;
}

const script::InputEntry* InputMapDoc::axis(std::string_view name) const {
    auto it = find_entry(axes(), name);
    return it == axes().end() ? nullptr : &*it;
}

bool InputMapDoc::commit(script::InputSections next) {
    history_.resize(index_ + 1);
    if (saved_ > index_) saved_ = static_cast<std::size_t>(-1); // the saved state is unreachable now
    history_.push_back(std::move(next));
    ++index_;
    error_.clear();
    return true;
}

bool InputMapDoc::undo() {
    if (!can_undo()) return false;
    --index_;
    return true;
}

bool InputMapDoc::redo() {
    if (!can_redo()) return false;
    ++index_;
    return true;
}

bool InputMapDoc::add_action(std::string_view name) {
    if (!script::is_input_name(name)) return fail(std::format("'{}' is not a valid action name (letters, digits, _)", name));
    if (action(name) != nullptr) return fail(std::format("there is already an action '{}'", name));
    if (actions().size() >= InputFrame::max_actions) return fail("too many input actions (64 at most)");
    script::InputSections next = current();
    next.actions.push_back({std::string(name), {}, std::format("n{}", next_id_++)});
    return commit(std::move(next));
}

bool InputMapDoc::remove_action(std::string_view name) {
    if (action(name) == nullptr) return fail(std::format("no action '{}'", name));
    script::InputSections next = current();
    next.actions.erase(find_entry(next.actions, name));
    return commit(std::move(next));
}

bool InputMapDoc::rename_action(std::string_view name, std::string_view new_name) {
    if (action(name) == nullptr) return fail(std::format("no action '{}'", name));
    if (name == new_name) return true;
    if (!script::is_input_name(new_name)) return fail(std::format("'{}' is not a valid action name", new_name));
    if (action(new_name) != nullptr) return fail(std::format("there is already an action '{}'", new_name));
    script::InputSections next = current();
    for (script::InputEntry& e : next.actions) {
        if (e.name == name) e.name = std::string(new_name);
    }
    return commit(std::move(next));
}

bool InputMapDoc::bind(std::string_view action_name, std::string_view binding) {
    const script::InputEntry* a = action(action_name);
    if (a == nullptr) return fail(std::format("no action '{}'", action_name));
    if (!script::parse_binding(binding)) return fail(std::format("unknown input '{}'", binding));
    if (std::find(a->bindings.begin(), a->bindings.end(), binding) != a->bindings.end()) {
        return fail(std::format("'{}' already has {}", action_name, binding));
    }
    script::InputSections next = current();
    for (script::InputEntry& e : next.actions) {
        if (e.name == action_name) e.bindings.emplace_back(binding);
    }
    return commit(std::move(next));
}

bool InputMapDoc::unbind(std::string_view action_name, std::string_view binding) {
    const script::InputEntry* a = action(action_name);
    if (a == nullptr) return fail(std::format("no action '{}'", action_name));
    if (std::find(a->bindings.begin(), a->bindings.end(), binding) == a->bindings.end()) {
        return fail(std::format("'{}' has no {}", action_name, binding));
    }
    script::InputSections next = current();
    for (script::InputEntry& e : next.actions) {
        if (e.name == action_name) std::erase(e.bindings, std::string(binding));
    }
    return commit(std::move(next));
}

bool InputMapDoc::add_axis(std::string_view name) {
    if (!script::is_input_name(name)) return fail(std::format("'{}' is not a valid axis name (letters, digits, _)", name));
    if (axis(name) != nullptr) return fail(std::format("there is already an axis '{}'", name));
    // Each 2D axis is two InputMap axes (x and y).
    if ((axes().size() + 1) * 2 > InputFrame::max_axes) return fail("too many input axes (4 two-dimensional axes at most)");
    script::InputSections next = current();
    next.axes.push_back({std::string(name), {}, std::format("n{}", next_id_++)});
    return commit(std::move(next));
}

bool InputMapDoc::remove_axis(std::string_view name) {
    if (axis(name) == nullptr) return fail(std::format("no axis '{}'", name));
    script::InputSections next = current();
    next.axes.erase(find_entry(next.axes, name));
    return commit(std::move(next));
}

bool InputMapDoc::add_axis_source(std::string_view axis_name, std::string_view source) {
    const script::InputEntry* a = axis(axis_name);
    if (a == nullptr) return fail(std::format("no axis '{}'", axis_name));
    AxisSource x;
    AxisSource y;
    if (!script::parse_axis2(source, x, y)) return fail(std::format("unknown axis source '{}'", source));
    if (std::find(a->bindings.begin(), a->bindings.end(), source) != a->bindings.end()) {
        return fail(std::format("'{}' already has {}", axis_name, source));
    }
    script::InputSections next = current();
    for (script::InputEntry& e : next.axes) {
        if (e.name == axis_name) e.bindings.emplace_back(source);
    }
    return commit(std::move(next));
}

bool InputMapDoc::remove_axis_source(std::string_view axis_name, std::string_view source) {
    const script::InputEntry* a = axis(axis_name);
    if (a == nullptr) return fail(std::format("no axis '{}'", axis_name));
    if (std::find(a->bindings.begin(), a->bindings.end(), source) == a->bindings.end()) {
        return fail(std::format("'{}' has no {}", axis_name, source));
    }
    script::InputSections next = current();
    for (script::InputEntry& e : next.axes) {
        if (e.name == axis_name) std::erase(e.bindings, std::string(source));
    }
    return commit(std::move(next));
}

std::optional<std::string> captured_binding(const DeviceState& devices) {
    for (std::size_t k = 1; k < key_count; ++k) {
        if (devices.keys_tapped[k]) return script::binding_name(Binding::key(static_cast<Key>(k)));
    }
    for (std::size_t b = 0; b < mouse_button_count; ++b) {
        if (devices.mouse_tapped[b]) return script::binding_name(Binding::mouse(static_cast<MouseButton>(b)));
    }
    for (std::size_t b = 0; b < pad_button_count; ++b) {
        if (devices.buttons_tapped[b]) return script::binding_name(Binding::button(static_cast<PadButton>(b)));
    }
    for (std::size_t a = 0; a < pad_axis_count; ++a) {
        const float v = devices.pad_axes[a];
        const auto axis = static_cast<PadAxis>(a);
        std::string name;
        if (v > 0.5f) name = script::binding_name(Binding::axis_positive(axis));
        if (v < -0.5f) name = script::binding_name(Binding::axis_negative(axis));
        if (!name.empty()) return name;
    }
    return std::nullopt;
}

} // namespace cuervo::editor

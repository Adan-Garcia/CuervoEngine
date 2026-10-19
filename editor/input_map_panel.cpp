// The input-map editor (bottom dock; DESIGN.md 4.8, 4.13): the project's
// actions and 2D axes from project.toml (input_map_doc.hpp).
//
//   + Bind     waits for the next key, mouse button, gamepad button or
//              stick/trigger push and binds it (Esc cancels; bind Escape
//              itself by typing Key.Escape in the field next to it)
//   x          removes a binding; right-click an action to rename/remove it
//   Save       writes [input.actions] / [input.axes] back into project.toml,
//              keeping the rest of the file and its comments
// Every edit is undoable with this panel's Undo/Redo buttons (its own
// history: Ctrl+Z stays the scene's).

#include <imgui.h>
#include <imgui_stdlib.h>

#include <format>
#include <map>
#include <string>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/script/project.hpp"
#include "editor/app.hpp"
#include "editor/theme.hpp"
#include "editor/panels.hpp"

namespace cuervo::editor {

namespace {

class InputMapPanel final : public Panel {
public:
    const char* title() const override { return "Input Map"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        InputMapDoc& map = app.input_map();
        if (!map.loaded()) {
            ImGui::TextDisabled("No project.toml");
            return;
        }
        auto report = [&](bool ok) {
            if (!ok) LOG_WARN("Input map: {}", map.error());
        };
        // ---- toolbar
        ImGui::BeginDisabled(!map.dirty());
        if (ImGui::Button("Save")) {
            std::string error;
            if (map.save(error)) {
                LOG_INFO("Input map saved to project.toml");
            } else {
                LOG_ERROR("{}", error);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!map.can_undo());
        if (ImGui::Button("Undo")) map.undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!map.can_redo());
        if (ImGui::Button("Redo")) map.redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        const bool enter = ImGui::InputTextWithHint("##new", "new action name", &new_name_,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Add Action") || enter) {
            if (map.add_action(new_name_)) {
                new_name_.clear();
            } else {
                report(false);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Axis")) report(map.add_axis(new_name_.empty() ? "axis" : new_name_));
        ImGui::SameLine();
        if (map.dirty()) {
            ImGui::TextColored(theme::im(theme::colors().warning), "unsaved");
        } else {
            ImGui::TextDisabled("saved");
        }
        ImGui::SetItemTooltip("Actions are numbered by their order: adding or removing one makes older replays refuse to play.");
        capture(app, map);

        if (!ImGui::BeginChild("##map")) {
            ImGui::EndChild();
            return;
        }
        if (ImGui::BeginTable("##actions", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("bindings", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("add", ImGuiTableColumnFlags_WidthFixed, 250.0f);
            ImGui::TableHeadersRow();
            // Copy: edits below replace the doc's current snapshot.
            const std::vector<script::InputEntry> actions = map.actions();
            for (const script::InputEntry& a : actions) {
                ImGui::PushID(a.name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Selectable(a.name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
                if (ImGui::BeginPopupContextItem("##action")) {
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::IsWindowAppearing()) rename_ = a.name;
                    if (ImGui::InputText("##rename", &rename_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        report(map.rename_action(a.name, rename_));
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::MenuItem("Remove Action")) report(map.remove_action(a.name));
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                binding_buttons(a, [&](const std::string& b) { report(map.unbind(a.name, b)); });
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("+ Bind")) {
                    capturing_ = a.name;
                    capture_frames_ = 0;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130.0f);
                std::string& typed = typed_[a.name];
                if (ImGui::InputTextWithHint("##typed", "or type: Key.Escape", &typed, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    if (map.bind(a.name, typed)) {
                        typed.clear();
                    } else {
                        report(false);
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!map.axes().empty()) ImGui::SeparatorText("2D axes (Input.axis(#name) -> vec2)");
        if (ImGui::BeginTable("##axes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("axis", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("sources", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("add", ImGuiTableColumnFlags_WidthFixed, 250.0f);
            const std::vector<script::InputEntry> axes = map.axes();
            for (const script::InputEntry& a : axes) {
                ImGui::PushID(("axis:" + a.name).c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Selectable(a.name.c_str());
                if (ImGui::BeginPopupContextItem("##axis")) {
                    if (ImGui::MenuItem("Remove Axis")) report(map.remove_axis(a.name));
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                binding_buttons(a, [&](const std::string& b) { report(map.remove_axis_source(a.name, b)); });
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##source", "+ source")) {
                    for (std::string_view src : script::axis2_source_names()) {
                        if (ImGui::Selectable(std::string(src).c_str())) report(map.add_axis_source(a.name, src));
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

private:
    template <typename Remove>
    static void binding_buttons(const script::InputEntry& e, Remove remove) {
        if (e.bindings.empty()) ImGui::TextDisabled("(unbound)");
        for (std::size_t i = 0; i < e.bindings.size(); ++i) {
            if (i > 0) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(e.bindings[i].c_str());
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::SmallButton("x")) remove(e.bindings[i]);
            ImGui::SetItemTooltip("Remove %s", e.bindings[i].c_str());
            ImGui::PopID();
        }
    }

    // "Press a key": the next input on any device becomes the binding.
    void capture(EditorApp& app, InputMapDoc& map) {
        app.capturing_binding = !capturing_.empty();
        if (capturing_.empty()) return;
        if (!ImGui::IsPopupOpen("Press a key")) ImGui::OpenPopup("Press a key");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Press a key", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Binding '%s': press a key, mouse button or gamepad button,", capturing_.c_str());
            ImGui::TextUnformatted("or push a stick or trigger. Esc cancels.");
            // The click that opened this popup is still in this frame's taps.
            if (++capture_frames_ > 2) {
                const DeviceState& d = app.devices();
                if (d.key(Key::Escape)) {
                    capturing_.clear();
                } else if (auto b = captured_binding(d)) {
                    if (!map.bind(capturing_, *b)) LOG_WARN("Input map: {}", map.error());
                    capturing_.clear();
                }
            }
            if (capturing_.empty()) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    std::string new_name_;
    std::string rename_;
    std::string capturing_;
    int capture_frames_ = 0;
    std::map<std::string, std::string> typed_;
};

} // namespace

std::unique_ptr<Panel> make_input_map_panel() { return std::make_unique<InputMapPanel>(); }

} // namespace cuervo::editor

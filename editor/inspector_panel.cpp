// The inspector (right dock): the selected node's name and every `@export`
// field of its class (LANGUAGE.md 9.13), edited generically by the field's
// value shape -- the same self-describing values scene files store
// (cuervo/scene/scene_file.hpp): bools, every number kind, strings (resource
// paths are text), names, vecN, enums, structs (Color gets a color picker)
// and lists. Fields the file doesn't store show their class default, dimmed;
// editing one stores it, "Reset" removes it again. Drags are merged into one
// undo step; right-click any field to edit its value as text.

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>

#include "cuervo/core/log.hpp"
#include "editor/app.hpp"
#include "editor/theme.hpp"
#include "editor/panels.hpp"

namespace cuervo::editor {

namespace {

struct EditResult {
    bool changed = false;
    bool continuous = false;  // part of a drag: merge with the next change
    bool deactivated = false; // the widget was released: close the merge window
};

void track(EditResult& r) {
    if (ImGui::IsItemActive()) r.continuous = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) r.deactivated = true;
}

ImGuiDataType data_type(scene::NumKind k) {
    switch (k) {
        case scene::NumKind::I8: return ImGuiDataType_S8;
        case scene::NumKind::I16: return ImGuiDataType_S16;
        case scene::NumKind::I32: return ImGuiDataType_S32;
        case scene::NumKind::I64: return ImGuiDataType_S64;
        case scene::NumKind::U8: return ImGuiDataType_U8;
        case scene::NumKind::U16: return ImGuiDataType_U16;
        case scene::NumKind::U32: return ImGuiDataType_U32;
        case scene::NumKind::U64: return ImGuiDataType_U64;
        case scene::NumKind::F32: return ImGuiDataType_Float;
        case scene::NumKind::F64: return ImGuiDataType_Double;
    }
    return ImGuiDataType_S64;
}

bool is_color(const scene::Value& v) {
    if (v.kind != scene::ValueKind::Struct || v.type != "Color" || v.fields.size() != 4) return false;
    return std::all_of(v.fields.begin(), v.fields.end(),
                       [](const scene::Field& f) { return f.value.kind == scene::ValueKind::Float; });
}

EditResult edit_value(const char* label, scene::Value& v, const ClassRegistry& registry, int depth = 0);

// Integers through a 64-bit temporary of the right signedness, then back.
EditResult edit_int(const char* label, scene::Value& v) {
    EditResult r;
    const ImGuiDataType t = data_type(v.num);
    union {
        std::int8_t s8;
        std::int16_t s16;
        std::int32_t s32;
        std::int64_t s64;
        std::uint8_t u8;
        std::uint16_t u16;
        std::uint32_t u32;
        std::uint64_t u64;
    } x{};
    switch (v.num) {
        case scene::NumKind::I8: x.s8 = static_cast<std::int8_t>(v.i); break;
        case scene::NumKind::I16: x.s16 = static_cast<std::int16_t>(v.i); break;
        case scene::NumKind::I32: x.s32 = static_cast<std::int32_t>(v.i); break;
        case scene::NumKind::U8: x.u8 = static_cast<std::uint8_t>(v.i); break;
        case scene::NumKind::U16: x.u16 = static_cast<std::uint16_t>(v.i); break;
        case scene::NumKind::U32: x.u32 = static_cast<std::uint32_t>(v.i); break;
        case scene::NumKind::U64: x.u64 = static_cast<std::uint64_t>(v.i); break;
        default: x.s64 = v.i; break;
    }
    if (ImGui::DragScalar(label, t, &x, 0.2f)) {
        r.changed = true;
        switch (v.num) {
            case scene::NumKind::I8: v.i = x.s8; break;
            case scene::NumKind::I16: v.i = x.s16; break;
            case scene::NumKind::I32: v.i = x.s32; break;
            case scene::NumKind::U8: v.i = x.u8; break;
            case scene::NumKind::U16: v.i = x.u16; break;
            case scene::NumKind::U32: v.i = x.u32; break;
            case scene::NumKind::U64: v.i = static_cast<std::int64_t>(x.u64); break;
            default: v.i = x.s64; break;
        }
    }
    track(r);
    return r;
}

EditResult edit_value(const char* label, scene::Value& v, const ClassRegistry& registry, int depth) {
    EditResult r;
    ImGui::PushID(label);
    switch (v.kind) {
        case scene::ValueKind::Bool:
            r.changed = ImGui::Checkbox(label, &v.b);
            break;
        case scene::ValueKind::Int: r = edit_int(label, v); break;
        case scene::ValueKind::Float:
            if (v.num == scene::NumKind::F64) {
                r.changed = ImGui::DragScalar(label, ImGuiDataType_Double, &v.f, 0.1f, nullptr, nullptr, "%.6g");
            } else {
                float f = static_cast<float>(v.f);
                if (ImGui::DragFloat(label, &f, 0.1f, 0.0f, 0.0f, "%.6g")) {
                    v.f = f;
                    r.changed = true;
                }
            }
            track(r);
            break;
        case scene::ValueKind::Vec:
            r.changed = ImGui::DragScalarN(label, ImGuiDataType_Float, v.vec.data(), static_cast<int>(v.vec.size()), 0.5f,
                                           nullptr, nullptr, "%.6g");
            track(r);
            break;
        case scene::ValueKind::Str:
            // Text (and resource paths): every keystroke is applied, merged
            // into one undo step until the field is left.
            r.changed = ImGui::InputText(label, &v.text);
            track(r);
            // Drop an asset from the browser to store its path.
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(asset_payload)) {
                    v.text = static_cast<const char*>(p->Data);
                    r.changed = true;
                    r.deactivated = true;
                }
                ImGui::EndDragDropTarget();
            }
            break;
        case scene::ValueKind::Name: {
            r.changed = ImGui::InputText(label, &v.text);
            track(r);
            ImGui::SetItemTooltip("an interned name (#%s)", v.text.c_str());
            break;
        }
        case scene::ValueKind::Enum: {
            const EnumSpec* e = registry.find_enum(v.type);
            if (e != nullptr && ImGui::BeginCombo(label, v.text.c_str())) {
                for (const std::string& variant : e->variants) {
                    if (ImGui::Selectable(variant.c_str(), variant == v.text)) {
                        v.text = variant;
                        r.changed = true;
                    }
                }
                ImGui::EndCombo();
            } else if (e == nullptr) {
                ImGui::LabelText(label, "%s.%s", v.type.c_str(), v.text.c_str());
            }
            break;
        }
        case scene::ValueKind::Struct:
            if (is_color(v)) {
                float c[4];
                for (std::size_t i = 0; i < 4; ++i) c[i] = static_cast<float>(v.fields[i].value.f);
                if (ImGui::ColorEdit4(label, c, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
                    for (std::size_t i = 0; i < 4; ++i) v.fields[i].value.f = c[i];
                    r.changed = true;
                    // The picker popup's drags aren't this item: treat every
                    // color change as continuous; the app seals on release.
                    r.continuous = true;
                }
                track(r);
            } else if (depth < 8 && ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth, "%s (%s)", label,
                                                      v.type.c_str())) {
                for (scene::Field& f : v.fields) {
                    EditResult sub = edit_value(f.name.c_str(), f.value, registry, depth + 1);
                    r.changed |= sub.changed;
                    r.continuous |= sub.continuous;
                    r.deactivated |= sub.deactivated;
                }
                ImGui::TreePop();
            }
            break;
        case scene::ValueKind::List: {
            const std::string head = std::format("{} [{}]", label, v.items.size());
            if (depth < 8 && ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth, "%s", head.c_str())) {
                std::optional<std::size_t> remove;
                for (std::size_t i = 0; i < v.items.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("-")) remove = i;
                    ImGui::SameLine();
                    EditResult sub = edit_value(std::format("[{}]", i).c_str(), v.items[i], registry, depth + 1);
                    r.changed |= sub.changed;
                    r.continuous |= sub.continuous;
                    r.deactivated |= sub.deactivated;
                    ImGui::PopID();
                }
                if (remove) {
                    v.items.erase(v.items.begin() + static_cast<std::ptrdiff_t>(*remove));
                    r.changed = true;
                    r.continuous = false;
                }
                // New items copy the last one (an empty list has no shape to copy).
                ImGui::BeginDisabled(v.items.empty());
                if (ImGui::SmallButton("+ item")) {
                    v.items.push_back(v.items.back());
                    r.changed = true;
                    r.continuous = false;
                }
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
            break;
        }
        case scene::ValueKind::Null: ImGui::LabelText(label, "(no default; right-click to type a value)"); break;
    }
    ImGui::PopID();
    return r;
}

class InspectorPanel final : public Panel {
public:
    const char* title() const override { return "Inspector"; }
    Dock dock() const override { return Dock::Right; }

    void draw(EditorApp& app) override {
        EditorDocument& doc = app.doc();
        const NodeId id = doc.selection();
        const ModelNode* n = doc.model().get(id);
        if (n == nullptr) {
            ImGui::TextDisabled("Select a node in the scene tree or the view.");
            return;
        }
        const ClassRegistry& registry = app.env().classes();
        const ClassSpec* cls = registry.find(n->type);

        ImGui::TextColored(theme::im(theme::colors().info), "%s", n->type.c_str());
        if (cls != nullptr && !cls->base.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("extends %s", cls->base.c_str());
        }
        ImGui::TextDisabled("%s", doc.model().path_of(id).c_str());
        if (id != name_node_ || !ImGui::IsAnyItemActive()) {
            name_node_ = id;
            name_text_ = n->name;
        }
        ImGui::SetNextItemWidth(-60.0f);
        ImGui::InputText("name", &name_text_);
        if (ImGui::IsItemDeactivatedAfterEdit()) app.report(doc.rename(id, name_text_));
        ImGui::Separator();

        if (cls == nullptr) {
            ImGui::TextColored(theme::im(theme::colors().warning), "'%s' is not a node class of this project;", n->type.c_str());
            ImGui::TextColored(theme::im(theme::colors().warning), "its stored fields are shown as saved.");
        } else if (!cls->default_error.empty()) {
            ImGui::TextColored(theme::im(theme::colors().warning), "defaults unavailable: %s", cls->default_error.c_str());
        }

        // The class's exported fields, then any stored field it no longer has.
        std::vector<std::string> fields;
        if (cls != nullptr) {
            for (const FieldSpec& f : cls->fields) fields.push_back(f.name);
        }
        for (const scene::Field& f : n->fields) {
            if (std::find(fields.begin(), fields.end(), f.name) == fields.end()) fields.push_back(f.name);
        }
        if (ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed, 118.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            for (const std::string& field : fields) draw_field(app, id, field);
            ImGui::EndTable();
        }
        if (cls != nullptr && cls->fields.empty()) ImGui::TextDisabled("(no exported fields)");
    }

private:
    void draw_field(EditorApp& app, NodeId id, const std::string& field) {
        EditorDocument& doc = app.doc();
        bool stored = false;
        scene::Value value = doc.field_value(id, field, &stored);
        const std::string type = doc.field_type(id, field);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        if (stored) {
            ImGui::TextUnformatted(field.c_str());
        } else {
            ImGui::TextDisabled("%s", field.c_str());
        }
        ImGui::SetItemTooltip("%s: %s%s", field.c_str(), type.c_str(), stored ? "" : " (class default)");
        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(field.c_str());
        ImGui::SetNextItemWidth(stored ? -24.0f : -1.0f);
        if (!stored) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.65f);
        const EditResult r = edit_value("##v", value, app.env().classes());
        if (!stored) ImGui::PopStyleVar();
        if (r.changed) {
            app.report(doc.set_field(id, field, value, r.continuous));
            if (!r.continuous) doc.seal();
        }
        if (r.deactivated) doc.seal();
        if (ImGui::BeginPopupContextItem("##as_text")) {
            if (ImGui::IsWindowAppearing()) text_edit_ = scene::format_value(value);
            ImGui::TextDisabled("%s: %s", field.c_str(), type.c_str());
            ImGui::SetNextItemWidth(320.0f);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##text", &text_edit_, ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Apply") || enter) {
                std::string error;
                if (auto parsed = scene::parse_value(text_edit_, error)) {
                    app.report(doc.set_field(id, field, *parsed));
                    doc.seal();
                    ImGui::CloseCurrentPopup();
                } else {
                    LOG_WARN("{}: {}", field, error);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset to default")) {
                app.report(doc.reset_field(id, field));
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (stored) {
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::SmallButton("R")) app.report(doc.reset_field(id, field));
            ImGui::SetItemTooltip("Reset to the class default (stop storing this field)");
        }
        ImGui::PopID();
    }

    NodeId name_node_ = 0;
    std::string name_text_;
    std::string text_edit_;
};

} // namespace

std::unique_ptr<Panel> make_inspector_panel() { return std::make_unique<InspectorPanel>(); }

} // namespace cuervo::editor

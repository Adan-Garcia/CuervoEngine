// The scene tree (left dock): the hierarchy of the open scene. Select,
// add a child of any node class, rename (double-click or F2), duplicate,
// delete, reorder (context menu) and reparent by drag and drop: drop on a
// node to make it the last child, or on the upper/lower edge of a row to
// place it before/after that node. Ctrl-click adds a node to the selection or removes it,
// Shift-click selects the range from the last picked one; Duplicate, Delete and Group act on
// the whole selection. Every row has the icon of its node class (icons.hpp).

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>

#include "editor/app.hpp"
#include "editor/icons.hpp"
#include "editor/panels.hpp"
#include "editor/theme.hpp"
#include "editor/ui.hpp"

namespace cuervo::editor {

namespace {

constexpr const char* drag_payload = "CUERVO_NODE";

class SceneTreePanel final : public Panel {
public:
    const char* title() const override { return "Scene"; }
    Dock dock() const override { return Dock::Left; }

    void draw(EditorApp& app) override {
        EditorDocument& doc = app.doc();
        const NodeId sel = doc.selection();
        const bool not_root = sel != 0 && sel != doc.model().root();
        if (ui::icon_label_button("Add", "plus")) app.request_add_child(sel != 0 ? sel : doc.model().root());
        ImGui::SetItemTooltip("Add a child node to the selection (Ctrl+A)");
        ImGui::SameLine();
        const bool any = !doc.transform_targets().empty();
        if (ui::icon_button("dup", "duplicate", "Duplicate the selection (Ctrl+D)", false, any)) {
            app.report(doc.duplicate_selection());
        }
        ImGui::SameLine();
        if (ui::icon_button("group", "group", "Group the selection under a new node (Ctrl+G)", false, any)) {
            app.report(doc.group_selection() != 0);
        }
        ImGui::SameLine();
        if (ui::icon_button("del", "trash", "Delete the selection (Del)", false, any)) app.report(doc.remove_selection());
        (void)not_root;
        ImGui::Separator();

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F2, false) &&
            sel != 0) {
            begin_rename(doc, sel);
        }
        if (ImGui::BeginChild("##tree", {0.0f, 0.0f}, ImGuiChildFlags_None)) {
            pending_ = {};
            if (!doc.model().empty()) draw_node(app, doc.model().root());
            // Structural edits wait until the tree is drawn: the model must
            // not change under the loop that walks it.
            apply_pending(app);
        }
        ImGui::EndChild();
    }

private:
    struct Pending {
        enum class Kind { None, Move, Delete, Duplicate, MoveUp, MoveDown, Rename, Group } kind = Kind::None;
        NodeId node = 0;
        NodeId parent = 0;
        std::optional<std::size_t> index;
    };

    // Plain click: just this node. Ctrl: toggle it. Shift: the range from the primary one
    // (in tree order). A right click on a selected node keeps the selection.
    void click_node(EditorDocument& doc, NodeId id, bool right) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            doc.select_toggle(id);
        } else if (io.KeyShift && doc.selection() != 0) {
            const std::vector<NodeId> order = doc.model().preorder();
            const auto from = std::find(order.begin(), order.end(), doc.selection());
            const auto to = std::find(order.begin(), order.end(), id);
            if (from != order.end() && to != order.end()) {
                std::vector<NodeId> range(std::min(from, to), std::max(from, to) + 1);
                if (from > to) std::reverse(range.begin(), range.end()); // the clicked one ends up primary
                doc.select_set(range);
            }
        } else if (!(right && doc.is_selected(id))) {
            doc.select(id);
        }
    }

    void begin_rename(EditorDocument& doc, NodeId id) {
        renaming_ = id;
        rename_text_ = doc.model().get(id)->name;
        focus_rename_ = true;
    }

    void draw_node(EditorApp& app, NodeId id) {
        EditorDocument& doc = app.doc();
        const ModelNode* n = doc.model().get(id);
        if (n == nullptr) return;
        ImGui::PushID(static_cast<int>(id));
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
                                   ImGuiTreeNodeFlags_FramePadding;
        if (n->children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (doc.is_selected(id)) flags |= ImGuiTreeNodeFlags_Selected;

        bool open = false;
        if (renaming_ == id) {
            // Inline rename: Enter commits, Escape or clicking away cancels.
            open = ImGui::TreeNodeEx("##node", flags | ImGuiTreeNodeFlags_AllowOverlap, "%s", "");
            ImGui::SameLine();
            if (focus_rename_) {
                ImGui::SetKeyboardFocusHere();
                focus_rename_ = false;
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##rename", &rename_text_,
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                pending_ = {Pending::Kind::Rename, id, 0, std::nullopt};
                renaming_ = 0;
            } else if (ImGui::IsItemDeactivated() || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                renaming_ = 0;
            }
        } else {
            open = ImGui::TreeNodeEx("##node", flags, "%s", "");
            const ImVec2 row_min = ImGui::GetItemRectMin();
            const ImVec2 row_max = ImGui::GetItemRectMax();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (!ImGui::IsItemToggledOpen()) click_node(doc, id, ImGui::IsItemClicked(ImGuiMouseButton_Right));
            }
            // Icon, name and (dimmed) class, drawn over the row.
            const brand::ThemeColors& t = theme::colors();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (doc.is_selected(id)) {
                dl->AddRectFilled(row_min, {row_min.x + 2.0f, row_max.y}, theme::im32(t.accent));
            }
            const float x = row_min.x + ImGui::GetTreeNodeToLabelSpacing();
            const float cy = (row_min.y + row_max.y) * 0.5f;
            ui::draw_icon(dl, icon_for_class(&app.env().classes(), n->type), {x + 8.0f, cy}, theme::icon_size,
                          theme::im32(doc.is_selected(id) ? t.accent : t.text_secondary));
            const std::string label = n->name.empty() ? std::string("(unnamed)") : n->name;
            const float ty = cy - ImGui::GetTextLineHeight() * 0.5f;
            dl->AddText({x + 22.0f, ty}, theme::im32(t.text), label.c_str());
            const float label_w = ImGui::CalcTextSize(label.c_str()).x;
            dl->AddText({x + 22.0f + label_w + 8.0f, ty}, theme::im32(t.text_muted), n->type.c_str());
            const float type_w = ImGui::CalcTextSize(n->type.c_str()).x;

            const auto [script_path, script_line] = app.find_class_script(n->type);
            const bool has_script = !script_path.empty();
            if (has_script) {
                const float badge_x = x + 22.0f + label_w + 8.0f + type_w + 10.0f;
                ui::draw_icon(dl, "script", {badge_x, cy}, 12.0f, theme::im32(t.accent));
            }

            if (ImGui::IsItemHovered()) {
                if (has_script) {
                    ImGui::SetItemTooltip("%s (%s)\nScript: %s:%d\n(Right-click to manage script)",
                                          label.c_str(), n->type.c_str(), script_path.c_str(), script_line);
                } else {
                    ImGui::SetItemTooltip("%s (%s)\n(Right-click to attach script)", label.c_str(), n->type.c_str());
                }
            }

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && n->children.empty()) {
                begin_rename(doc, id);
            }
        }

        const auto [script_path, script_line] = app.find_class_script(n->type);
        const bool has_script = !script_path.empty();

        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (!doc.is_selected(id)) doc.select(id);
            const bool is_root = id == doc.model().root();
            if (ImGui::MenuItem("Add Child Node...")) app.request_add_child(id);
            if (ImGui::MenuItem("Rename", "F2")) begin_rename(doc, id);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !is_root)) pending_ = {Pending::Kind::Duplicate, id, 0, std::nullopt};
            if (ImGui::MenuItem("Group", "Ctrl+G", false, !is_root)) pending_ = {Pending::Kind::Group, id, 0, std::nullopt};
            if (ImGui::MenuItem("Move Up", nullptr, false, !is_root)) pending_ = {Pending::Kind::MoveUp, id, 0, std::nullopt};
            if (ImGui::MenuItem("Move Down", nullptr, false, !is_root)) pending_ = {Pending::Kind::MoveDown, id, 0, std::nullopt};
            ImGui::Separator();
            if (has_script) {
                if (ImGui::MenuItem("Open Script")) {
                    app.open_in_external_editor(app.env().resolve(script_path), script_line);
                }
                if (ImGui::MenuItem("Change Script...")) {
                    app.request_attach_script(id);
                }
                if (ImGui::MenuItem("Detach Script")) {
                    app.detach_script(id);
                }
            } else {
                if (ImGui::MenuItem("Attach Script...")) {
                    app.request_attach_script(id);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del", false, !is_root)) pending_ = {Pending::Kind::Delete, id, 0, std::nullopt};
            ImGui::EndPopup();
        }

        // Drag and drop: any node but the root can be dragged.
        if (renaming_ != id && id != doc.model().root() && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(drag_payload, &id, sizeof(id));
            ImGui::Text("Move '%s'", n->name.c_str());
            ImGui::EndDragDropSource();
        }
        if (renaming_ != id && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CUERVO_ASSET")) {
                const char* asset_path = static_cast<const char*>(p->Data);
                if (std::string_view(asset_path).ends_with(".plm")) {
                    std::string error;
                    if (app.attach_script_file(id, app.env().resolve(asset_path), error)) {
                        app.notify_ok(std::format("Bound '{}' to '{}'.", asset_path, n->name));
                    } else {
                        app.report_failure(error);
                    }
                }
            }
            // Upper quarter: before this node; lower quarter (if it has no
            // open children): after it; else: into it as the last child.
            const float y = ImGui::GetMousePos().y;
            const float top = ImGui::GetItemRectMin().y;
            const float h = ImGui::GetItemRectSize().y;
            const bool can_sibling = id != doc.model().root();
            int zone = 0;
            if (can_sibling && y < top + h * 0.25f) zone = -1;
            if (can_sibling && y > top + h * 0.75f) zone = 1;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
            if (zone != 0) {
                const float ly = zone < 0 ? top : top + h;
                dl->AddLine({ImGui::GetItemRectMin().x, ly}, {ImGui::GetItemRectMax().x, ly}, color, 2.0f);
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(
                    drag_payload, zone != 0 ? ImGuiDragDropFlags_AcceptNoDrawDefaultRect : 0)) {
                NodeId dragged = 0;
                std::memcpy(&dragged, p->Data, sizeof(dragged));
                if (zone == 0) {
                    pending_ = {Pending::Kind::Move, dragged, id, std::nullopt};
                } else {
                    // Index counted after the dragged node leaves its place.
                    const NodeId parent = n->parent;
                    std::size_t index = doc.model().index_in_parent(id) + (zone > 0 ? 1 : 0);
                    const ModelNode* d = doc.model().get(dragged);
                    if (d != nullptr && d->parent == parent && doc.model().index_in_parent(dragged) < index) --index;
                    pending_ = {Pending::Kind::Move, dragged, parent, index};
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (open) {
            for (NodeId c : n->children) draw_node(app, c);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void apply_pending(EditorApp& app) {
        EditorDocument& doc = app.doc();
        const Pending p = pending_;
        pending_ = {};
        const ModelNode* n = doc.model().get(p.node);
        switch (p.kind) {
            case Pending::Kind::None: break;
            case Pending::Kind::Move: app.report(doc.reparent(p.node, p.parent, p.index)); break;
            case Pending::Kind::Delete: app.report(doc.remove_selection()); break;
            case Pending::Kind::Group: app.report(doc.group_selection() != 0); break;
            case Pending::Kind::Duplicate: app.report(doc.duplicate_selection()); break;
            case Pending::Kind::Rename: app.report(doc.rename(p.node, rename_text_)); break;
            case Pending::Kind::MoveUp:
            case Pending::Kind::MoveDown: {
                if (n == nullptr || n->parent == 0) break;
                const std::size_t i = doc.model().index_in_parent(p.node);
                const std::size_t count = doc.model().get(n->parent)->children.size();
                if (p.kind == Pending::Kind::MoveUp && i > 0) app.report(doc.reparent(p.node, n->parent, i - 1));
                if (p.kind == Pending::Kind::MoveDown && i + 1 < count) app.report(doc.reparent(p.node, n->parent, i + 1));
                break;
            }
        }
    }

    Pending pending_;
    NodeId renaming_ = 0;
    std::string rename_text_;
    bool focus_rename_ = false;
};

} // namespace

std::unique_ptr<Panel> make_scene_tree_panel() { return std::make_unique<SceneTreePanel>(); }

} // namespace cuervo::editor

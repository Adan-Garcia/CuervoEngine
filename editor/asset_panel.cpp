// The asset browser (bottom dock, tabbed with the console; DESIGN.md 4.13):
// the project folder as a tree (asset_library.hpp) with scenes, scripts,
// fonts, sounds, vector art, images and replays.
//
//   double-click a scene     open it (asks first if the open one is unsaved)
//   double-click a replay    play it back in the scene view
//   drag a file              onto an inspector text field (stores its path),
//                            or a .scene into the scene view (instances it)
//   right-click              open, new folder, rename, delete (confirmed),
//                            copy path
// External changes (another editor, git) are picked up by polling once a
// second; Refresh rescans now.

#include <imgui.h>
#include <imgui_stdlib.h>

#include <SDL3/SDL_clipboard.h>

#include <algorithm>
#include <cctype>
#include <format>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "editor/app.hpp"
#include "editor/theme.hpp"
#include "editor/panels.hpp"

namespace cuervo::editor {

namespace {

// The kind tag's color, from the theme's tokens (an icon per kind is 8.3).
ImVec4 kind_color(AssetKind k) {
    const brand::ThemeColors& ui = theme::colors();
    switch (k) {
        case AssetKind::Folder: return theme::im(ui.text_secondary);
        case AssetKind::Scene: return theme::im(ui.accent);
        case AssetKind::Script: return theme::im(ui.success);
        case AssetKind::Font: return theme::im(ui.info);
        case AssetKind::Sound: return theme::im(ui.warning);
        case AssetKind::Vector: return theme::im(ui.link);
        case AssetKind::Image: return theme::im(ui.info);
        case AssetKind::Replay: return theme::im(ui.warning);
        case AssetKind::Project: return theme::im(ui.danger);
        case AssetKind::Other: break;
    }
    return theme::im(ui.text_muted);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool matches(const AssetEntry& e, const std::string& filter) {
    if (filter.empty() || lower(e.name).find(filter) != std::string::npos) return true;
    return std::any_of(e.children.begin(), e.children.end(), [&](const AssetEntry& c) { return matches(c, filter); });
}

class AssetPanel final : public Panel {
public:
    const char* title() const override { return "Assets"; }
    Dock dock() const override { return Dock::Left; }

    void draw(EditorApp& app) override {
        AssetLibrary& lib = app.assets();
        if (std::string reveal = app.take_reveal_request(); !reveal.empty()) {
            selected_ = reveal;
            reveal_ = reveal;
        }
        if (ImGui::Button("New Folder")) begin_new_folder(folder_of(lib, selected_));
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) lib.poll();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("##filter", "filter", &filter_);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", selected_.empty() ? "drag files onto the inspector or the scene view" : selected_.c_str());

        if (ImGui::BeginChild("##tree", {0.0f, 0.0f}, ImGuiChildFlags_None)) {
            const std::string f = lower(filter_);
            for (const AssetEntry& e : lib.root().children) entry(app, e, f);
            // Right-click on empty space: new folder at the top.
            if (ImGui::BeginPopupContextWindow("##empty", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
                if (ImGui::MenuItem("New Folder")) begin_new_folder("");
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
        reveal_.clear();
        dialogs(app);
    }

private:
    static std::string folder_of(const AssetLibrary& lib, const std::string& path) {
        const AssetEntry* e = lib.find(path);
        if (e == nullptr) return {};
        if (e->kind == AssetKind::Folder) return path;
        const std::size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    void begin_new_folder(std::string parent) {
        op_ = Op::NewFolder;
        op_path_ = std::move(parent);
        name_ = "new_folder";
        op_open_ = false;
    }

    void entry(EditorApp& app, const AssetEntry& e, const std::string& filter) {
        if (!matches(e, filter)) return;
        ImGui::PushID(e.path.c_str());
        const bool folder = e.kind == AssetKind::Folder;
        const bool is_selected = selected_ == e.path;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (!folder) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (is_selected) flags |= ImGuiTreeNodeFlags_Selected;
        if (folder && e.path.find('/') == std::string::npos) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        // Open the folders on the way to a revealed file; filtering opens all.
        if (folder && (!filter.empty() || (!reveal_.empty() && reveal_.starts_with(e.path + "/")))) {
            ImGui::SetNextItemOpen(true);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kind_color(e.kind));
        const bool open = ImGui::TreeNodeEx("##node", flags, "%s%s", e.name.c_str(), folder ? "/" : "");
        ImGui::PopStyleColor();
        if (!reveal_.empty() && reveal_ == e.path) ImGui::SetScrollHereY(0.4f);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) selected_ = e.path;
        if (!folder && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) activate(app, e);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(asset_payload, e.path.c_str(), e.path.size() + 1);
            ImGui::Text("%s (%s)", e.path.c_str(), std::string(asset_kind_name(e.kind)).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (e.kind == AssetKind::Scene && ImGui::MenuItem("Open")) app.request_open_scene(e.path);
            if (e.kind == AssetKind::Scene && ImGui::MenuItem("Instance in the Scene")) {
                const EditorDocument& doc = app.doc();
                const NodeId parent = doc.selection() != 0 ? doc.selection() : doc.model().root();
                const std::string self = doc.has_path() ? app.env().relative(doc.path()) : std::string();
                app.report(app.doc().instance_scene(parent, e.path, std::nullopt, self) != 0);
            }
            if (e.kind == AssetKind::Replay && ImGui::MenuItem("Play Replay")) app.start_replay(e.path);
            if (ImGui::MenuItem("New Folder")) begin_new_folder(folder ? e.path : folder_of(app.assets(), e.path));
            if (ImGui::MenuItem("Rename...")) {
                op_ = Op::Rename;
                op_path_ = e.path;
                name_ = e.name;
                op_open_ = false;
            }
            if (ImGui::MenuItem("Delete...")) {
                op_ = Op::Delete;
                op_path_ = e.path;
                op_open_ = false;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Path")) SDL_SetClipboardText(e.path.c_str());
            ImGui::EndPopup();
        }
        // After the context menu, which belongs to the tree node item.
        if (!folder) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", std::string(asset_kind_name(e.kind)).c_str());
        }
        if (folder && open) {
            for (const AssetEntry& c : e.children) entry(app, c, filter);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void activate(EditorApp& app, const AssetEntry& e) {
        switch (e.kind) {
            case AssetKind::Scene: app.request_open_scene(e.path); break;
            case AssetKind::Replay: app.start_replay(e.path); break;
            default: app.open_location(e.path, 0); break;
        }
    }

    void dialogs(EditorApp& app) {
        if (op_ == Op::None) return;
        const char* name = op_ == Op::NewFolder ? "New Folder" : op_ == Op::Rename ? "Rename" : "Delete";
        if (!op_open_) {
            ImGui::OpenPopup(name);
            op_open_ = true;
        }
        bool keep = true;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
        if (!ImGui::BeginPopupModal(name, &keep, ImGuiWindowFlags_AlwaysAutoResize)) {
            op_ = Op::None;
            return;
        }
        AssetLibrary& lib = app.assets();
        std::string error;
        auto done = [&](bool ok) {
            if (!ok) {
                LOG_ERROR("{}", error);
                return;
            }
            ImGui::CloseCurrentPopup();
            op_ = Op::None;
        };
        if (op_ == Op::Delete) {
            const AssetEntry* e = lib.find(op_path_);
            ImGui::Text("Delete '%s'%s?", op_path_.c_str(),
                        e != nullptr && e->kind == AssetKind::Folder ? " and everything in it" : "");
            ImGui::TextColored(theme::im(theme::colors().danger), "This can't be undone.");
            if (ImGui::Button("Delete")) {
                const bool ok = lib.remove(op_path_, error);
                if (ok) LOG_INFO("Deleted '{}'", op_path_);
                done(ok);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) done(true);
        } else {
            ImGui::Text(op_ == Op::NewFolder ? "New folder in '%s/':" : "Rename '%s' to:",
                        op_path_.empty() && op_ == Op::NewFolder ? "." : op_path_.c_str());
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(320.0f);
            const bool enter = ImGui::InputText("##name", &name_, ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("OK") || enter) {
                const bool ok = op_ == Op::NewFolder ? lib.create_folder(op_path_, name_, error)
                                                     : lib.rename(op_path_, name_, error);
                if (ok && op_ == Op::Rename) {
                    const std::size_t slash = op_path_.find_last_of('/');
                    selected_ = (slash == std::string::npos ? std::string() : op_path_.substr(0, slash + 1)) + name_;
                }
                done(ok);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) done(true);
        }
        ImGui::EndPopup();
        if (!keep) op_ = Op::None;
    }

    enum class Op { None, NewFolder, Rename, Delete };
    std::string filter_;
    std::string selected_;
    std::string reveal_;
    Op op_ = Op::None;
    bool op_open_ = false;
    std::string op_path_;
    std::string name_;
};

} // namespace

std::unique_ptr<Panel> make_asset_panel() { return std::make_unique<AssetPanel>(); }

} // namespace cuervo::editor

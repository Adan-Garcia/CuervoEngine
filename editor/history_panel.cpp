// The undo history (right dock): every step of the open scene's undo stack, newest
// first. The applied steps are solid, the ones Undo took back are dimmed (Redo
// brings them back); clicking a row jumps there (undo or redo until that step is
// the last applied one). Whole-scene history, one row per command, so a gizmo drag
// or a group transform is one row (DESIGN.md 4.16 "Clarified (Milestone 8.3)").

#include <imgui.h>

#include <format>

#include "editor/app.hpp"
#include "editor/panels.hpp"
#include "editor/theme.hpp"
#include "editor/ui.hpp"

namespace cuervo::editor {

namespace {

class HistoryPanel final : public Panel {
public:
    const char* title() const override { return "History"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        EditorDocument& doc = app.doc();
        const CommandStack& stack = doc.stack();
        const brand::ThemeColors& t = theme::colors();
        ImGui::BeginDisabled(!stack.can_undo());
        if (ImGui::Button("Undo")) doc.undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!stack.can_redo());
        if (ImGui::Button("Redo")) doc.redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu of %zu steps applied", stack.index(), stack.size());
        ImGui::Separator();
        if (!ImGui::BeginChild("##history")) {
            ImGui::EndChild();
            return;
        }
        std::size_t jump = stack.index();
        bool clicked = false;
        // Newest first; row `i + 1` means "i + 1 steps applied". The last row is the opened state.
        for (std::size_t i = stack.size(); i-- > 0;) {
            const bool applied = i < stack.index();
            const bool current = applied && i + 1 == stack.index();
            ImGui::PushID(static_cast<int>(i));
            const std::string label = stack.label_at(i);
            if (row(label.c_str(), i + 1, current, applied)) {
                jump = i + 1;
                clicked = true;
            }
            ImGui::PopID();
        }
        if (row("Opened state", 0, stack.index() == 0, true)) {
            jump = 0;
            clicked = true;
        }
        if (clicked) app.report(doc.jump_to_history(jump));
        (void)t;
        ImGui::EndChild();
    }

private:
    bool row(const char* label, std::size_t step, bool current, bool applied) {
        const brand::ThemeColors& t = theme::colors();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        const float w = ImGui::GetContentRegionAvail().x;
        const bool hit = ImGui::InvisibleButton("##row", {w, h});
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (current) {
            dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::im32(t.accent_subtle), theme::radius_sm);
            dl->AddRectFilled(p, {p.x + 2.0f, p.y + h}, theme::im32(t.accent));
        } else if (ImGui::IsItemHovered()) {
            dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::im32(theme::control_hover()), theme::radius_sm);
        }
        const rhi::Color ink = applied ? t.text : t.text_muted;
        ui::draw_icon(dl, step == 0 ? "project" : "history", {p.x + 14.0f, p.y + h * 0.5f}, 14.0f,
                      theme::im32(applied ? t.text_secondary : brand::with_alpha(t.text_muted, 0.6f)));
        dl->AddText({p.x + 28.0f, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f}, theme::im32(ink), label);
        return hit && !current;
    }
};

} // namespace

std::unique_ptr<Panel> make_history_panel() { return std::make_unique<HistoryPanel>(); }

} // namespace cuervo::editor

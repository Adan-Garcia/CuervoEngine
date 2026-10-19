// The console (bottom dock): every LOG_* line since the editor started
// (engine, editor, scripts' print), colored by level, with level toggles,
// a text filter and auto-scroll.

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>

#include "editor/app.hpp"
#include "editor/theme.hpp"
#include "editor/panels.hpp"

namespace cuervo::editor {

namespace {

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

class ConsolePanel final : public Panel {
public:
    const char* title() const override { return "Console"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        const bool fresh = app.console().snapshot(seen_, lines_);
        if (ImGui::SmallButton("Clear")) {
            app.console().clear();
            lines_.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("info", &show_info_);
        ImGui::SameLine();
        ImGui::Checkbox("warnings", &show_warn_);
        ImGui::SameLine();
        ImGui::Checkbox("errors", &show_error_);
        ImGui::SameLine();
        ImGui::Checkbox("debug", &show_debug_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("##filter", "filter", &filter_);
        ImGui::SameLine();
        ImGui::Checkbox("auto-scroll", &auto_scroll_);

        // Log lines in the code face (JetBrains Mono, DESIGN.md 4.16).
        ImGui::PushFont(theme::mono_font(), theme::mono_font() != nullptr ? theme::mono_font_size : 0.0f);
        if (ImGui::BeginChild("##log", {0.0f, 0.0f}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
            for (const ConsoleLine& l : lines_) {
                const brand::ThemeColors& ui = theme::colors();
                ImVec4 color = theme::im(ui.text);
                switch (l.level) {
                    case log::Level::Trace:
                    case log::Level::Debug:
                        if (!show_debug_) continue;
                        color = theme::im(ui.text_muted);
                        break;
                    case log::Level::Info:
                        if (!show_info_) continue;
                        break;
                    case log::Level::Warn:
                        if (!show_warn_) continue;
                        color = theme::im(ui.warning);
                        break;
                    case log::Level::Error:
                        if (!show_error_) continue;
                        color = theme::im(ui.danger);
                        break;
                }
                if (!contains_ci(l.text, filter_) && !contains_ci(l.source, filter_)) continue;
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(l.text.c_str());
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s", l.source.c_str());
            }
            if (fresh && auto_scroll_) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::PopFont();
    }

private:
    std::uint64_t seen_ = 0;
    std::vector<ConsoleLine> lines_;
    std::string filter_;
    bool show_info_ = true;
    bool show_warn_ = true;
    bool show_error_ = true;
    bool show_debug_ = false;
    bool auto_scroll_ = true;
};

} // namespace

std::unique_ptr<Panel> make_console_panel() { return std::make_unique<ConsolePanel>(); }

} // namespace cuervo::editor

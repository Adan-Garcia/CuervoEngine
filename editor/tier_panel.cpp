// The render-tier migration report (bottom dock; DESIGN.md 4.4, 4.13): the
// same check `cuervo-tools tier check` runs (cuervo/script/tier_check.hpp),
// for the project's own tier or a target tier, with every entry linked to
// its file (scenes open in the editor; other files are shown in Assets).

#include <imgui.h>

#include <SDL3/SDL_clipboard.h>

#include <format>
#include <optional>

#include "cuervo/core/log.hpp"
#include "cuervo/script/tier_check.hpp"
#include "editor/app.hpp"
#include "editor/theme.hpp"
#include "editor/panels.hpp"

namespace cuervo::editor {

namespace {

class TierPanel final : public Panel {
public:
    const char* title() const override { return "Tier Report"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        if (!ran_) run(app);
        const char* targets[] = {"the project's tier", "compatible", "full"};
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::Combo("target", &target_, targets, 3)) run(app);
        ImGui::SameLine();
        if (ImGui::Button("Run Again")) run(app);
        ImGui::SameLine();
        ImGui::BeginDisabled(!report_);
        if (ImGui::Button("Copy as Text")) SDL_SetClipboardText(text_.c_str());
        ImGui::EndDisabled();
        if (!report_) {
            ImGui::TextColored(theme::im(theme::colors().danger), "%s", error_.c_str());
            return;
        }
        const script::TierCheckReport& r = *report_;
        ImGui::SameLine();
        const bool ok = r.ok();
        ImGui::TextColored(theme::im(ok ? theme::colors().success : theme::colors().danger),
                           "%s: %s  (%zu errors, %zu warnings, %zu notes; declared %s; web download %.1f MB)",
                           std::string(rhi::tier_name(r.target)).c_str(), ok ? "fits" : "blocked",
                           r.count(script::Severity::Error), r.count(script::Severity::Warning),
                           r.count(script::Severity::Note), std::string(rhi::tier_name(r.declared)).c_str(),
                           static_cast<double>(r.download_bytes) / (1024.0 * 1024.0));
        if (r.issues.empty()) {
            ImGui::TextDisabled("Nothing to report.");
            return;
        }
        if (ImGui::BeginTable("##issues", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("severity", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("where", ImGuiTableColumnFlags_WidthFixed, 260.0f);
            ImGui::TableSetupColumn("message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < r.issues.size(); ++i) {
                const script::TierIssue& is = r.issues[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const brand::ThemeColors& ui = theme::colors();
                const ImVec4 color = theme::im(is.severity == script::Severity::Error     ? ui.danger
                                               : is.severity == script::Severity::Warning ? ui.warning
                                                                                          : ui.info);
                ImGui::TextColored(color, "%s", std::string(script::severity_name(is.severity)).c_str());
                ImGui::TableNextColumn();
                const std::string where = is.file.empty() ? std::string("project")
                                          : is.line == 0  ? is.file
                                          : is.column == 0 ? std::format("{}:{}", is.file, is.line)
                                                           : std::format("{}:{}:{}", is.file, is.line, is.column);
                // The link: click to open/show the file.
                if (ImGui::Selectable(where.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    app.open_location(is.file.empty() ? std::string("project.toml") : is.file, is.line);
                }
                ImGui::SetItemTooltip("open / show %s", is.file.empty() ? "project.toml" : is.file.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", is.message.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

private:
    void run(EditorApp& app) {
        ran_ = true;
        std::optional<rhi::Tier> target;
        if (target_ == 1) target = rhi::Tier::Compatible;
        if (target_ == 2) target = rhi::Tier::Full;
        error_.clear();
        report_ = script::check_project_tier(app.env().project_dir(), target, error_);
        text_ = report_ ? script::format_tier_report(*report_) : error_;
        if (report_) {
            LOG_INFO("Tier report ({}): {} error(s), {} warning(s)", rhi::tier_name(report_->target),
                     report_->count(script::Severity::Error), report_->count(script::Severity::Warning));
        }
    }

    bool ran_ = false;
    int target_ = 0;
    std::optional<script::TierCheckReport> report_;
    std::string error_;
    std::string text_;
};

} // namespace

std::unique_ptr<Panel> make_tier_panel() { return std::make_unique<TierPanel>(); }

} // namespace cuervo::editor

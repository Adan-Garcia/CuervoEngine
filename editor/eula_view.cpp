#include "editor/eula_view.hpp"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "cuervo/platform/eula.hpp"
#include "editor/theme.hpp"

namespace cuervo::editor {

namespace {

// The summary is Markdown; the box shows it as plain text (no ** markers, no heading line).
std::string plain_text(std::string_view md) {
    std::string out;
    for (std::size_t i = 0; i < md.size(); ++i) {
        if (md[i] == '*' && i + 1 < md.size() && md[i + 1] == '*') {
            ++i;
        } else {
            out += md[i];
        }
    }
    const std::size_t b = out.find_first_not_of(" \t\r\n");
    out.erase(0, b == std::string::npos ? out.size() : b);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

} // namespace

EulaChoice draw_eula(EulaView& view, bool interactive) {
    using namespace theme;
    EulaChoice choice = EulaChoice::Pending;
    const brand::ThemeColors& t = colors();

    // Title and the version being agreed to.
    if (ImFont* font = display_font()) ImGui::PushFont(font, 22.0f);
    ImGui::TextUnformatted("Cuervo Engine License Agreement");
    if (display_font() != nullptr) ImGui::PopFont();
    ImGui::TextColored(im(t.text_muted), "Version %s", std::string(eula::version()).c_str());
    ImGui::Dummy({0.0f, space_1});

    // The plain-English summary, in a raised box above the legal text.
    const std::string summary = plain_text(eula::summary());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, im(t.surface_raised));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {space_3, space_2});
    const float wrap_w = ImGui::GetContentRegionAvail().x - 2.0f * space_3;
    const float summary_h = ImGui::CalcTextSize(summary.c_str(), nullptr, false, wrap_w).y + 2.0f * space_2 + 2.0f;
    if (ImGui::BeginChild("##summary", {0.0f, std::min(summary_h, 190.0f)}, ImGuiChildFlags_Borders)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(summary.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Agreement / third-party notices.
    ImGui::Dummy({0.0f, space_1});
    if (ImGui::BeginTabBar("##eula_tabs")) {
        if (ImGui::BeginTabItem("Agreement")) {
            view.notices = false;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Third-party notices")) {
            view.notices = true;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    const float footer = interactive ? 30.0f + 22.0f + 4.0f * space_2 : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, im(t.bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {space_3, space_2});
    if (ImGui::BeginChild("##agreement", {0.0f, -footer}, ImGuiChildFlags_Borders)) {
        if (ImFont* mono = mono_font()) ImGui::PushFont(mono, mono_font_size);
        ImGui::PushTextWrapPos(0.0f);
        const std::string_view text = view.notices ? eula::notices_text() : eula::license_text();
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        if (mono_font() != nullptr) ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (!interactive) return choice;

    ImGui::Dummy({0.0f, space_1});
    ImGui::Checkbox("I have read and agree to the Cuervo Engine License Agreement", &view.agreed);
    ImGui::Dummy({0.0f, space_1});
    const float w = (std::min(ImGui::GetContentRegionAvail().x, 420.0f) - space_2) * 0.5f;
    ImGui::BeginDisabled(!view.agreed);
    push_primary_button();
    if (ImGui::Button("Accept and continue", {w, 30.0f})) choice = EulaChoice::Accepted;
    pop_primary_button();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Decline and quit", {w, 30.0f})) choice = EulaChoice::Declined;
    return choice;
}

} // namespace cuervo::editor

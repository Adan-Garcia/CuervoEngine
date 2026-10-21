#include "editor/license_view.hpp"

#include <imgui.h>

#include <string_view>

#include "cuervo/platform/license.hpp"
#include "editor/theme.hpp"

namespace cuervo::editor {

void draw_license_dialog(LicenseView& view) {
    using namespace theme;
    const brand::ThemeColors& t = colors();

    if (ImFont* font = display_font()) ImGui::PushFont(font, 22.0f);
    ImGui::TextUnformatted("Cuervo Engine License");
    if (display_font() != nullptr) ImGui::PopFont();
    ImGui::Dummy({0.0f, space_1});

    if (ImGui::BeginTabBar("##license_tabs")) {
        if (ImGui::BeginTabItem("License")) {
            view.notices = false;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Third-party notices")) {
            view.notices = true;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    const float footer = 30.0f + 2.0f * space_2;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, im(t.bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {space_3, space_2});
    if (ImGui::BeginChild("##license_content", {0.0f, -footer}, ImGuiChildFlags_Borders)) {
        if (ImFont* mono = mono_font()) ImGui::PushFont(mono, mono_font_size);
        ImGui::PushTextWrapPos(0.0f);
        const std::string_view text = view.notices ? license::notices_text() : license::license_text();
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        if (mono_font() != nullptr) ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace cuervo::editor

// The editor's look (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): no raw ImGui-default
// styling is left. Every color and the metrics are compared with ImGui's own light
// and dark defaults.

#include <doctest.h>
#include <imgui.h>

#include <string>

#include "editor/theme.hpp"

namespace {

using cuervo::brand::Theme;

struct Context {
    Context() {
        ctx = ImGui::CreateContext();
        ImGui::GetIO().Fonts->AddFontDefault();
    }
    ~Context() { ImGui::DestroyContext(ctx); }
    ImGuiContext* ctx;
};

bool same(const ImVec4& a, const ImVec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

} // namespace

TEST_CASE("editor style: every ImGui color is a brand token, not an ImGui default") {
    for (const Theme theme : {Theme::Light, Theme::Dark}) {
        Context c;
        cuervo::editor::theme::apply(theme);
        const ImGuiStyle styled = ImGui::GetStyle();

        ImGuiStyle dark;
        ImGui::StyleColorsDark(&dark);
        ImGuiStyle light;
        ImGui::StyleColorsLight(&light);
        ImGuiStyle classic;
        ImGui::StyleColorsClassic(&classic);

        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            const std::string name = ImGui::GetStyleColorName(i);
            INFO("color ", name, theme == Theme::Light ? " (light)" : " (dark)");
            CHECK_FALSE(same(styled.Colors[i], dark.Colors[i]));
            CHECK_FALSE(same(styled.Colors[i], light.Colors[i]));
            CHECK_FALSE(same(styled.Colors[i], classic.Colors[i]));
        }
    }
}

TEST_CASE("editor style: metrics follow the kit's scales") {
    Context c;
    cuervo::editor::theme::apply(Theme::Dark);
    const ImGuiStyle& s = ImGui::GetStyle();
    const ImGuiStyle defaults;
    // Compact density, machined radii: none of these is ImGui's default.
    CHECK(s.ItemSpacing.y != defaults.ItemSpacing.y);
    CHECK(s.FramePadding.x == doctest::Approx(cuervo::editor::theme::space_2)); // kit spacing, equal to ImGui's by design
    CHECK(s.WindowPadding.x == doctest::Approx(cuervo::editor::theme::space_2));
    CHECK(s.FrameRounding != defaults.FrameRounding);
    CHECK(s.PopupRounding != defaults.PopupRounding);
    CHECK(s.ScrollbarSize != defaults.ScrollbarSize);
    CHECK(s.GrabRounding != defaults.GrabRounding);
    CHECK(s.TabRounding != defaults.TabRounding);
    // The kit's radius scale: 2 px small, 4 px controls, docks square.
    CHECK(s.FrameRounding == doctest::Approx(cuervo::editor::theme::radius_md));
    CHECK(s.GrabRounding == doctest::Approx(cuervo::editor::theme::radius_sm));
    CHECK(s.WindowRounding == doctest::Approx(0.0f));
    // The two themes differ and the window background is the surface token.
    const ImVec4 dark_bg = s.Colors[ImGuiCol_WindowBg];
    cuervo::editor::theme::apply(Theme::Light);
    CHECK_FALSE(same(dark_bg, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]));
    CHECK(cuervo::editor::theme::current() == Theme::Light);
}

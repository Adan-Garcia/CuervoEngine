#include "editor/theme.hpp"

#include <SDL3/SDL.h>

#include <filesystem>

#include "cuervo/core/log.hpp"

namespace cuervo::editor::theme {

namespace {

brand::Theme current_theme = brand::Theme::Dark;
ImFont* mono = nullptr;
ImFont* display = nullptr;

using brand::mix;
using brand::with_alpha;

} // namespace

brand::Theme current() { return current_theme; }

const brand::ThemeColors& colors() { return brand::colors(current_theme); }

brand::Theme system_theme() {
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT ? brand::Theme::Light : brand::Theme::Dark;
}

ImU32 im32(rhi::Color c) { return ImGui::ColorConvertFloat4ToU32(im(c)); }

rhi::Color control_fill() { return colors().surface; }
rhi::Color control_hover() { return mix(colors().surface, colors().text, 0.07f); }
rhi::Color control_active() { return mix(colors().surface, colors().text, 0.14f); }

rhi::Color severity_color(Severity s) {
    const brand::ThemeColors& t = colors();
    switch (s) {
        case Severity::Success: return t.success;
        case Severity::Warning: return t.warning;
        case Severity::Danger: return t.danger;
        case Severity::Info: break;
    }
    return t.info;
}

rhi::Color severity_subtle(Severity s) {
    const brand::ThemeColors& t = colors();
    switch (s) {
        case Severity::Success: return t.success_subtle;
        case Severity::Warning: return t.warning_subtle;
        case Severity::Danger: return t.danger_subtle;
        case Severity::Info: break;
    }
    return t.info_subtle;
}

void push_primary_button() {
    const brand::ThemeColors& t = colors();
    ImGui::PushStyleColor(ImGuiCol_Button, im(t.accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, im(t.accent_hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, im(mix(t.accent_hover, t.text, 0.12f)));
    ImGui::PushStyleColor(ImGuiCol_Text, im(t.on_accent));
    ImGui::PushStyleColor(ImGuiCol_Border, im(t.accent));
}
void pop_primary_button() { ImGui::PopStyleColor(5); }

void push_danger_button() {
    const brand::ThemeColors& t = colors();
    ImGui::PushStyleColor(ImGuiCol_Button, im(t.danger));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, im(mix(t.danger, t.text, 0.15f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, im(mix(t.danger, t.text, 0.28f)));
    ImGui::PushStyleColor(ImGuiCol_Text, im(current_theme == brand::Theme::Light ? t.on_accent : t.text_inverse));
    ImGui::PushStyleColor(ImGuiCol_Border, im(t.danger));
}
void pop_danger_button() { ImGui::PopStyleColor(5); }

void apply(brand::Theme theme) {
    current_theme = theme;
    const brand::ThemeColors& t = brand::colors(theme);
    ImGuiStyle& s = ImGui::GetStyle();

    // ---- metrics: the kit's 4 px spacing scale, compact density, machined edges
    // (radius-md 4 px for controls, radius-sm 2 px for small grabs, radius-lg 8 px
    // for dialogs; docks are square and divided by hairlines).
    s.Alpha = 1.0f;
    s.DisabledAlpha = 0.45f;
    s.WindowPadding = {space_2, space_2};
    s.WindowRounding = 0.0f;
    s.WindowBorderSize = 0.0f;
    s.WindowMinSize = {96.0f, 48.0f};
    s.WindowTitleAlign = {0.0f, 0.5f};
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ChildRounding = radius_md;
    s.ChildBorderSize = 1.0f;
    s.PopupRounding = radius_md;
    s.PopupBorderSize = 1.0f;
    s.FramePadding = {space_2, 3.0f};
    s.FrameRounding = radius_md;
    s.FrameBorderSize = 1.0f;
    s.ItemSpacing = {space_2, space_1 + 1.0f};
    s.ItemInnerSpacing = {space_1 + 2.0f, space_1};
    s.CellPadding = {space_2 - 2.0f, space_1 - 1.0f};
    s.TouchExtraPadding = {0.0f, 0.0f};
    s.IndentSpacing = space_4;
    s.ColumnsMinSpacing = space_2;
    s.ScrollbarSize = 11.0f;
    s.ScrollbarRounding = radius_md;
    s.ScrollbarPadding = 2.0f;
    s.GrabMinSize = 10.0f;
    s.GrabRounding = radius_sm;
    s.LogSliderDeadzone = 4.0f;
    s.ImageBorderSize = 0.0f;
    s.TabRounding = radius_md;
    s.TabBorderSize = 0.0f;
    s.TabMinWidthBase = 1.0f;
    s.TabBarBorderSize = 1.0f;
    s.TabBarOverlineSize = 2.0f;
    s.DockingSeparatorSize = 2.0f;
    s.TableAngledHeadersAngle = 0.35f;
    s.TableAngledHeadersTextAlign = {0.5f, 0.0f};
    s.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesToNodes;
    s.TreeLinesSize = 1.0f;
    s.TreeLinesRounding = 0.0f;
    s.ColorButtonPosition = ImGuiDir_Right;
    s.ButtonTextAlign = {0.5f, 0.5f};
    s.SelectableTextAlign = {0.0f, 0.0f};
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign = {0.0f, 0.5f};
    s.SeparatorTextPadding = {space_2, 3.0f};
    s.DisplayWindowPadding = {space_3, space_3};
    s.DisplaySafeAreaPadding = {0.0f, 0.0f};
    s.MouseCursorScale = 1.0f;
    s.AntiAliasedLines = true;
    s.AntiAliasedLinesUseTex = true;
    s.AntiAliasedFill = true;
    s.CurveTessellationTol = 1.25f;
    s.CircleTessellationMaxError = 0.30f;

    // ---- colors: every ImGuiCol is assigned below; the loop first gives them a
    // visible token so a color added by a future ImGui can't fall back to a default.
    ImVec4* c = s.Colors;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) c[i] = im(t.text_muted);

    // Surfaces: docks and panels sit on `surface`; the page behind them, the menu
    // bar and the toolbar on `bg`; inputs are sunk into the panel (`bg`).
    const rhi::Color fill = control_fill();
    const rhi::Color hover = control_hover();
    const rhi::Color active = control_active();
    const rhi::Color field = t.bg;
    const rhi::Color field_hover = mix(t.bg, t.text, 0.05f);
    const rhi::Color field_active = mix(t.bg, t.text, 0.09f);
    const rhi::Color row_selected = t.accent_subtle;
    const rhi::Color row_hover = mix(t.surface, t.text, 0.06f);

    c[ImGuiCol_Text] = im(t.text);
    c[ImGuiCol_TextDisabled] = im(t.text_muted);
    c[ImGuiCol_WindowBg] = im(t.surface);
    c[ImGuiCol_ChildBg] = im(with_alpha(t.surface, 0.0f));
    c[ImGuiCol_PopupBg] = im(t.surface_raised);
    c[ImGuiCol_Border] = im(t.border);
    c[ImGuiCol_BorderShadow] = im(with_alpha(t.bg, 0.0f));
    c[ImGuiCol_FrameBg] = im(field);
    c[ImGuiCol_FrameBgHovered] = im(field_hover);
    c[ImGuiCol_FrameBgActive] = im(field_active);
    c[ImGuiCol_TitleBg] = im(t.bg);
    c[ImGuiCol_TitleBgActive] = im(t.bg);
    c[ImGuiCol_TitleBgCollapsed] = im(t.bg);
    c[ImGuiCol_MenuBarBg] = im(t.bg);
    c[ImGuiCol_ScrollbarBg] = im(with_alpha(t.surface, 0.0f));
    c[ImGuiCol_ScrollbarGrab] = im(mix(t.border, t.border_strong, 0.5f));
    c[ImGuiCol_ScrollbarGrabHovered] = im(t.border_strong);
    c[ImGuiCol_ScrollbarGrabActive] = im(t.text_muted);
    c[ImGuiCol_CheckMark] = im(t.accent);
    c[ImGuiCol_CheckboxSelectedBg] = im(field);
    c[ImGuiCol_SliderGrab] = im(t.accent);
    c[ImGuiCol_SliderGrabActive] = im(t.accent_hover);
    c[ImGuiCol_Button] = im(fill);
    c[ImGuiCol_ButtonHovered] = im(hover);
    c[ImGuiCol_ButtonActive] = im(active);
    c[ImGuiCol_Header] = im(row_selected);
    c[ImGuiCol_HeaderHovered] = im(row_hover);
    c[ImGuiCol_HeaderActive] = im(mix(row_selected, t.accent, 0.25f));
    c[ImGuiCol_Separator] = im(t.border);
    c[ImGuiCol_SeparatorHovered] = im(t.border_strong);
    c[ImGuiCol_SeparatorActive] = im(t.accent);
    c[ImGuiCol_ResizeGrip] = im(with_alpha(t.border_strong, 0.30f));
    c[ImGuiCol_ResizeGripHovered] = im(t.border_strong);
    c[ImGuiCol_ResizeGripActive] = im(t.accent);
    c[ImGuiCol_InputTextCursor] = im(t.text);
    c[ImGuiCol_Tab] = im(t.bg);
    c[ImGuiCol_TabHovered] = im(row_hover);
    c[ImGuiCol_TabSelected] = im(t.surface);
    c[ImGuiCol_TabSelectedOverline] = im(t.accent);
    c[ImGuiCol_TabDimmed] = im(t.bg);
    c[ImGuiCol_TabDimmedSelected] = im(t.surface);
    c[ImGuiCol_TabDimmedSelectedOverline] = im(t.border_strong);
    c[ImGuiCol_PlotLines] = im(t.info);
    c[ImGuiCol_PlotLinesHovered] = im(t.accent);
    c[ImGuiCol_PlotHistogram] = im(t.accent);
    c[ImGuiCol_PlotHistogramHovered] = im(t.accent_hover);
    c[ImGuiCol_TableHeaderBg] = im(t.bg);
    c[ImGuiCol_TableBorderStrong] = im(t.border_strong);
    c[ImGuiCol_TableBorderLight] = im(t.border);
    c[ImGuiCol_TableRowBg] = im(with_alpha(t.text, 0.0f));
    c[ImGuiCol_TableRowBgAlt] = im(with_alpha(t.text, 0.035f));
    c[ImGuiCol_TextLink] = im(t.link);
    c[ImGuiCol_TextSelectedBg] = im(with_alpha(t.focus, 0.35f));
    c[ImGuiCol_TreeLines] = im(t.border);
    c[ImGuiCol_DragDropTarget] = im(t.accent);
    c[ImGuiCol_DragDropTargetBg] = im(with_alpha(t.accent, 0.10f));
    // The docking branch (DESIGN.md 4.16): drop-target preview and the empty-dock backdrop.
    c[ImGuiCol_DockingPreview] = im(with_alpha(t.accent, 0.30f));
    c[ImGuiCol_DockingEmptyBg] = im(t.bg);
    c[ImGuiCol_UnsavedMarker] = im(t.text);
    c[ImGuiCol_NavCursor] = im(t.focus);
    c[ImGuiCol_NavWindowingHighlight] = im(with_alpha(t.focus, 0.7f));
    c[ImGuiCol_NavWindowingDimBg] = im(with_alpha(brand::palette::raven, 0.35f));
    c[ImGuiCol_ModalWindowDimBg] = im(with_alpha(brand::palette::raven, 0.45f));
}

void load_fonts(const std::string& fallback_ttf) {
    ImGuiIO& io = ImGui::GetIO();
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_regular_file(CUERVO_FONT_UI, ec)) {
        io.Fonts->AddFontFromFileTTF(CUERVO_FONT_UI, ui_font_size);
    } else if (!fallback_ttf.empty() && fs::is_regular_file(fallback_ttf, ec) && fallback_ttf.ends_with(".ttf")) {
        LOG_WARN("Brand UI font missing ('{}'), using '{}'", CUERVO_FONT_UI, fallback_ttf);
        io.Fonts->AddFontFromFileTTF(fallback_ttf.c_str(), ui_font_size);
    }
    mono = fs::is_regular_file(CUERVO_FONT_MONO, ec) ? io.Fonts->AddFontFromFileTTF(CUERVO_FONT_MONO, mono_font_size)
                                                     : nullptr;
    display = fs::is_regular_file(CUERVO_FONT_DISPLAY, ec) ? io.Fonts->AddFontFromFileTTF(CUERVO_FONT_DISPLAY, 22.0f)
                                                           : nullptr;
}

ImFont* mono_font() { return mono; }
ImFont* display_font() { return display; }

} // namespace cuervo::editor::theme

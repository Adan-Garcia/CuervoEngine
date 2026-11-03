#pragma once

#include <string>

#include "cuervo/brand/brand.hpp"
#include "editor/messages.hpp"

// The editor's design tokens with no UI library behind them (DESIGN.md 4.13, 4.16): the active
// theme's 25 semantic colors, the kit's spacing/radius/density scales and the interface scale.
// The RmlUi editor reads these directly; the ImGui editor's styling (imgui_theme.hpp) is
// derived from the same values. Nothing in this header or theme.cpp includes ImGui.
namespace cuervo::editor::theme {

// The editor's current theme from now on. Does not call the restyle hook (a UI library that
// restyles on a theme change does so itself, as imgui_theme.cpp's apply() does).
void set_current(brand::Theme theme);
brand::Theme current();
const brand::ThemeColors& colors();
// The OS preference (SDL_GetSystemTheme), dark when it doesn't say.
brand::Theme system_theme();

// UI and Font scaling factors (1.0 = 100%).
float ui_scale();
void set_ui_scale(float scale);
float font_scale();
void set_font_scale(float scale);

// Called after a scale changes, so a UI library can restyle (the ImGui editor
// registers its style writer; the RmlUi editor needs none).
void set_restyle_hook(void (*hook)());

// The kit's scales (design kit tokens.css): spacing in px on a 4 px base, radii.
inline constexpr float space_1 = 4.0f;
inline constexpr float space_2 = 8.0f;
inline constexpr float space_3 = 12.0f;
inline constexpr float space_4 = 16.0f;
inline constexpr float radius_sm = 2.0f;
inline constexpr float radius_md = 4.0f;
inline constexpr float radius_lg = 8.0f;
// Compact density (kit 03 "Density": 14 px base text, space-2/space-3 gaps).
inline constexpr float ui_font_size = 14.0f;
inline constexpr float mono_font_size = 13.0f;
inline constexpr float display_font_size = 22.0f;
inline constexpr float toolbar_height = 36.0f;
inline constexpr float icon_size = 16.0f;

// ---- derived colors (mixes of tokens, never new hues)
// A control's resting, hovered and pressed fill on the panel surface.
rhi::Color control_fill();
rhi::Color control_hover();
rhi::Color control_active();
// A severity's accent color and its subtle background.
rhi::Color severity_color(Severity severity);
rhi::Color severity_subtle(Severity severity);

} // namespace cuervo::editor::theme

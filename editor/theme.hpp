#pragma once

#include <imgui.h>

#include <string>

#include "cuervo/brand/brand.hpp"
#include "editor/messages.hpp"

// The editor's look, entirely from the brand tokens (DESIGN.md 4.16, milestones
// 8.1 and 8.3): every ImGui color and every style metric is set here from the
// 25 semantic colors of the active theme (Bone or Raven) and the kit's spacing
// (4 px base), radius (2/4/8 px) and compact density (14 px text) scales, so no
// ImGui default is left in use (tests/editor/editor_ui_tests.cpp compares the
// whole style with ImGui's own light and dark defaults). Panels take their
// status colors from colors() instead of spelling them.
namespace cuervo::editor::theme {

// Restyles ImGui with `theme`'s tokens; the editor's current theme from now on.
void apply(brand::Theme theme);
brand::Theme current();
const brand::ThemeColors& colors();
// The OS preference (SDL_GetSystemTheme), dark when it doesn't say.
brand::Theme system_theme();

// UI and Font scaling factors (1.0 = 100%).
float ui_scale();
void set_ui_scale(float scale);
float font_scale();
void set_font_scale(float scale);

constexpr ImVec4 im(rhi::Color c) { return {c.r, c.g, c.b, c.a}; }
ImU32 im32(rhi::Color c);

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
inline constexpr float toolbar_height = 36.0f;
inline constexpr float icon_size = 16.0f;

// Loads the brand fonts into ImGui's atlas (the UI font becomes the default).
// `fallback_ttf` is used for the UI when a brand font file is missing.
void load_fonts(const std::string& fallback_ttf);
// JetBrains Mono, or the default font when it isn't loaded.
ImFont* mono_font();
// Outfit (the display face), or the default font when it isn't loaded.
ImFont* display_font();

// ---- derived colors (mixes of tokens, never new hues)
// A control's resting, hovered and pressed fill on the panel surface.
rhi::Color control_fill();
rhi::Color control_hover();
rhi::Color control_active();
// A severity's accent color and its subtle background.
rhi::Color severity_color(Severity severity);
rhi::Color severity_subtle(Severity severity);

// A filled Ember button (the one primary action of a view) / a danger one.
// Push before the Button, pop after.
void push_primary_button();
void pop_primary_button();
void push_danger_button();
void pop_danger_button();

} // namespace cuervo::editor::theme

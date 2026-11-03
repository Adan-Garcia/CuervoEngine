#pragma once

#include <imgui.h>

#include <string>

#include "cuervo/brand/brand.hpp"
#include "editor/messages.hpp"
#include "editor/theme.hpp"

// The ImGui editor's look, entirely from the brand tokens (DESIGN.md 4.16, milestones
// 8.1 and 8.3): every ImGui color and every style metric is set here from the
// 25 semantic colors of the active theme (Bone or Raven) and the kit's spacing
// (4 px base), radius (2/4/8 px) and compact density (14 px text) scales, so no
// ImGui default is left in use (tests/editor/editor_ui_tests.cpp compares the
// whole style with ImGui's own light and dark defaults). Panels take their
// status colors from colors() instead of spelling them.
namespace cuervo::editor::theme {

// Restyles ImGui with `theme`'s tokens and makes it the current theme (editor/theme.hpp holds the
// UI-library-independent tokens; a scale change restyles through the hook registered here).
void apply(brand::Theme theme);
constexpr ImVec4 im(rhi::Color c) { return {c.r, c.g, c.b, c.a}; }
ImU32 im32(rhi::Color c);

// Loads the brand fonts into ImGui's atlas (the UI font becomes the default).
// `fallback_ttf` is used for the UI when a brand font file is missing.
void load_fonts(const std::string& fallback_ttf);
// JetBrains Mono, or the default font when it isn't loaded.
ImFont* mono_font();
// Outfit (the display face), or the default font when it isn't loaded.
ImFont* display_font();

// A filled Ember button (the one primary action of a view) / a danger one.
// Push before the Button, pop after.
void push_primary_button();
void pop_primary_button();
void push_danger_button();
void pop_danger_button();

} // namespace cuervo::editor::theme

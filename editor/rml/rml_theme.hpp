#pragma once

#include <string>

#include "cuervo/brand/brand.hpp"
#include "cuervo/rhi/rhi.hpp"

// Token set of the RmlUi editor (DESIGN.md 4.13). Independent of editor/theme.cpp (the ImGui
// editor's): text, accent and semantic colors come from brand::ThemeColors, the surface steps and
// field borders are the mockup palette (.worktrees/plans/ui-overhaul/mockups, eb1.html :root).
namespace cuervo::editor::rml_theme {

struct Surfaces {
    rhi::Color page, layer, card, overlay, edge, border, field, scene;
};

// Surface steps for a theme: page (behind everything), layer (panels), card (raised rows,
// controls), overlay (menus, popups), edge (1 px dividers), border (field outlines).
Surfaces surfaces(brand::Theme theme);

// body { --name: value; ... } for tokens.rcss, plus the syntax colors for the script editor.
std::string export_tokens(brand::Theme theme);

} // namespace cuervo::editor::rml_theme

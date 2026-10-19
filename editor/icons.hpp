#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cuervo/vector/path.hpp"
#include "editor/class_registry.hpp"

// The editor's icons (DESIGN.md 4.16 "Clarified (Milestone 8.3)"). They are
// vectors, not an icon font: 24x24 line art written as SVG path data and read
// by the engine's own path parser (cuervo/vector/path.hpp), so they stay sharp
// at any size and any display scale and add no font to ship or license. The
// editor draws them as stroked polylines on ImGui's draw lists (icon_draw.cpp).
//
// Node classes map to an icon by name, walking up the base classes, so a
// project's `Ship extends Node2D` gets Node2D's icon and a class from the node
// library that has no entry yet still gets its base class's, ending at the
// plain "node" icon. Nothing ever has no icon.
namespace cuervo::editor {

struct IconShape {
    vector::Path path; // in a 24x24 box
    bool fill = false; // a small filled dot rather than a stroked outline
};

// nullptr for a name that is not an icon.
const std::vector<IconShape>* icon_shapes(std::string_view name);
bool has_icon(std::string_view name);
// Every icon name, in the table's order (the icon gallery, the tests).
std::vector<std::string> icon_names();

// The icon of a node class: its own entry, else its nearest base class's, else "node".
// `registry` may be null (then only the class's own name is looked up).
std::string_view icon_for_class(const ClassRegistry* registry, std::string_view class_name);

} // namespace cuervo::editor

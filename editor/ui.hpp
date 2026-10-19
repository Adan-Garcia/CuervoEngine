#pragma once

#include <imgui.h>

#include <string_view>
#include <vector>

#include "editor/messages.hpp"

// Small ImGui widgets shared by the panels (DESIGN.md 4.16 "Clarified (Milestone
// 8.3)"): vector icons, icon buttons for the toolbar, and the message banner.
// Colors come from theme.hpp only.
namespace cuervo::editor::ui {

// Draws icon `name` (icons.hpp; unknown names draw the plain node icon) inside the
// square of side `size` centered on `center`, stroked in `color`.
void draw_icon(ImDrawList* list, std::string_view name, ImVec2 center, float size, ImU32 color);

// An icon at the cursor, advancing it like a small square item; for inline use before a label.
void icon(std::string_view name, ImU32 color, float size = 16.0f);

// A square toolbar button with an icon. `selected` draws the pressed (tool chosen) state in
// the accent; `enabled` false greys it out. The tooltip reads "Label (Shortcut)".
bool icon_button(const char* id, std::string_view icon_name, const char* tooltip, bool selected = false,
                 bool enabled = true, float size = 28.0f);

// A button with an icon before its label.
bool icon_label_button(const char* label, std::string_view icon_name, bool enabled = true);

// A vertical 1 px divider between toolbar groups, at the cursor on the current line.
void toolbar_separator();

// A section header in a panel: muted small caps style text with a hairline.
void section_label(const char* text);

// A message (banner or toast row) drawn at the cursor, `width` wide: severity icon and
// bar, the sentence, a Details toggle and a dismiss button. Returns true when dismissed.
bool message_row(MessageCenter::Item& item, float width);

// Draws `text` with the characters at `positions` (ascending) in `highlight`.
void highlighted_text(ImDrawList* list, ImVec2 pos, std::string_view text, const std::vector<std::size_t>& positions,
                      ImU32 color, ImU32 highlight);

} // namespace cuervo::editor::ui

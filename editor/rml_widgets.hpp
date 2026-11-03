#pragma once
// The RmlUi editor's widget kit (DESIGN.md 4.13, mockups EB1/E4): each widget is one RML snippet built
// here and styled once in editor/rml/widgets.rcss from the theme tokens. String-only, so the markup
// (states, data attributes) is unit-tested without a window. Panels never write widget markup by hand.
//
// State classes, shared by every widget: .hover/.active come from the pseudo-classes, the rest are
// explicit: .selected, .disabled, .mod (the value is stored/modified), .focus (the keyboard focus row).

#include <string>
#include <string_view>
#include <vector>

namespace cuervo::editor::w {

std::string esc(std::string_view v);
std::string icon(std::string_view name, int size = 16, std::string_view cls = {});

// "Save scene (Ctrl+S)" -> text "Save scene", keys "Ctrl+S"; no trailing parenthesis -> keys empty.
struct Tip {
    std::string text, keys;
};
Tip split_tip(std::string_view title);
// Markup of the tooltip body.
std::string tip_body(const Tip& t);

struct RowOpts {
    bool selected = false, disabled = false;
    int indent = 0; // dp
    int top = -1;   // dp: absolute row in a virtualized list (>= 0), else flow layout
};
// A 22dp row: [lead][icon][label][secondary]. `data` is extra attributes ("data-id=\"4\"").
std::string row(std::string_view data, std::string_view label, std::string_view secondary, const RowOpts& o = {}, std::string_view icon_name = {}, std::string_view lead = {});

// Collapsible section header; `key` is what data-sect carries and what the persisted collapsed list stores.
std::string section(std::string_view key, std::string_view title, bool open, std::string_view count = {}, std::string_view tip = {});

std::string toggle(std::string_view key, bool on, bool disabled = false);
// A draggable number: `axis` "x"/"y"/"z"/"w" draws the colored axis tag (color plus the letter).
std::string scrub(std::string_view key, std::string_view text, std::string_view axis = {}, bool modified = false, bool disabled = false);
std::string text_field(std::string_view key, std::string_view value, std::string_view placeholder = {}, bool clearable = false, bool mono = true);
std::string dropdown(std::string_view key, std::string_view value, bool disabled = false);
struct Seg {
    std::string key, label;
};
std::string segmented(std::string_view group, const std::vector<Seg>& items, std::string_view active);
// Label badge ("Advanced 5 hidden", "2 errors"): kind "", "warn", "err", "info".
std::string badge(std::string_view text, std::string_view kind = {});
struct MenuItem {
    std::string key, label, keys; // key "-" is a separator
    bool disabled = false, checked = false;
};
std::string popup_menu(std::string_view data_attr, const std::vector<MenuItem>& items);

// Inspector property row: label column (tooltip carries name, type, default state), marker, control, reset.
struct PropOpts {
    bool modified = false, dim = false, disabled = false, reset = true;
    std::string tip = {}; // title text
};
std::string prop(std::string_view key, std::string_view label, std::string_view control, const PropOpts& o = {});

} // namespace cuervo::editor::w

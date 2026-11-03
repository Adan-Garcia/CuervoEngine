#pragma once
// Inspector rules for the RmlUi editor (mockup E4): how fields are grouped into sections, how numbers
// scrub and parse, how a nested value is addressed. No UI types, so it is unit-tested. Every edit
// still goes through EditorDocument::set_field / reset_field (the commands the ImGui inspector uses).

#include <string>
#include <string_view>
#include <vector>

#include "cuervo/scene/scene_file.hpp"
#include "editor/class_registry.hpp"

namespace cuervo::editor::ve {

// ---- sections
struct Section {
    std::string key;   // stable id, what the collapsed list stores ("Transform", "Turret", "Advanced")
    std::string title;
    std::vector<std::string> fields;
    bool default_open = true;
};
// Transform (position, rotation, scale), then the node's own class, then ancestor classes (closed by
// default), then Advanced (what Node and Node2D leave: always_process, top_level), then the fields a scene
// stores that the class no longer declares. `stored_extra`: field names stored on the node.
std::vector<Section> group_fields(const ClassRegistry& classes, std::string_view class_name, const std::vector<std::string>& stored_extra);

// Section state with overrides: a token "Key" in `list` closes a default-open section, "+Key" opens a
// default-closed one. Returns the new list after toggling `key`.
bool section_open(std::string_view list, const Section& s);
std::string toggle_section(std::string_view list, const Section& s);

// The sections that contain a field matching `query` (fuzzy on the name), with only matching fields.
// An empty query returns the input unchanged. Folded sections are searched too.
std::vector<Section> filter_sections(const std::vector<Section>& sections, std::string_view query);

// ---- values
// "-148.0", "6.2", "14"; floats always show a decimal point.
std::string format_number(const scene::Value& v);
// Reads text into the number (kind kept; ints clamped to their width). false: not a number.
bool parse_number(std::string_view text, scene::Value& v);
// Drag speed in units per pixel, times 10 with Shift, times 0.01 with Alt.
double scrub_speed(const scene::Value& v, bool shift, bool alt);
// v = start + delta (ints rounded and clamped to their width, F32 rounded to float).
void apply_scrub(scene::Value& v, const scene::Value& start, double delta);
std::pair<std::int64_t, std::int64_t> int_range(scene::NumKind k); // unsigned 64-bit saturates at INT64_MAX

// A nested value by path: "" is the root; segments are struct field names, or indices for lists and vecs.
scene::Value* at_path(scene::Value& root, std::string_view path);
// A vec component as a Value::number so it scrubs like a float.
std::string join_path(std::string_view base, std::string_view segment);

bool is_color(const scene::Value& v);
std::string color_hex(const scene::Value& v);               // "#E8E45A" or "#E8E45AFF" when alpha < 1
bool parse_color_hex(std::string_view text, scene::Value& v); // 6 or 8 digits, '#' optional
std::string color_css(const scene::Value& v);               // rgba(...) for a swatch

// HSV view of a Color value for the picker. Hue/saturation are kept by the caller while value or
// saturation is 0 (the RGB color alone cannot say them).
struct Hsva { float h = 0, s = 0, v = 0, a = 1; }; // all 0..1
Hsva hsva_of(const scene::Value& color);
void set_hsva(scene::Value& color, const Hsva& c);            // writes r, g, b, a as floats
void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b);
std::string hue_css(float h);                                  // "rgb(...)" of the pure hue

// The selection-following drawer tab for a node class: "Particles", "Animation", "Sprite", "Tiles", or
// "Script" for a class with a script and no tools of its own; empty when none.
std::string drawer_tab_for(std::string_view type, bool has_script);

} // namespace cuervo::editor::ve

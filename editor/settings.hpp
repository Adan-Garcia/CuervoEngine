#pragma once

#include <filesystem>
#include <map>
#include <vector>
#include <string>
#include <string_view>

namespace cuervo::editor {

// Layout of the RmlUi shell (DESIGN.md 4.13), persisted under [rml_layout]: splitter sizes in dp,
// collapsed regions and the active tab of each tab group. The ImGui editor keeps its own layouts.
// The part of the layout that "Save layout" captures: sizes, collapsed regions, active tabs.
struct RmlPanels {
    int left_width = 280;
    int right_width = 340;
    int drawer_height = 220;
    bool left_collapsed = false;
    bool right_collapsed = false;
    bool drawer_open = false;
    int tree_height = 300;          // scene tree share of the right column, dp
    std::string rail = "scene";     // scene | assets | search | settings
    std::string center = "scene";   // scene | script
    std::string drawer = "console"; // console | history | profiler | node
    // Reads one "key", "value" pair of the persisted form; false when the key is not a panel key.
    bool set(std::string_view key, std::string_view val);
    std::string serialize() const;                    // "left_width=280;right_width=340;..."
    static RmlPanels parse(std::string_view text);
    bool operator==(const RmlPanels&) const = default;
};

struct RmlLayout : RmlPanels {
    std::string theme;              // dark | light; empty: follow the system
    float ui_scale = 0.0f;          // 0: unset
    std::string closed_sections;    // inspector sections the user folded, ';'-separated
    std::string tree_scene;         // scene the saved tree state belongs to
    std::string tree_collapsed;     // collapsed scene tree nodes (paths), ;-separated
    std::string add_recents;        // Add Child picker: recently used classes, newest first, ;-separated
    std::string add_favorites;      // Add Child picker: starred classes
    bool operator==(const RmlLayout&) const = default;
    const RmlPanels& panels() const { return *this; }
    void set_panels(const RmlPanels& p) { static_cast<RmlPanels&>(*this) = p; }
};

// Persistent user preferences across editor runs:
// - ui_scale: spacing, padding, grab and control metrics scale
// - font_scale: font size scale factor
// - last_scenes: per-project path to the scene that was last opened or saved
class EditorSettings {
public:
    static constexpr float default_ui_scale = 1.0f;
    static constexpr float default_font_scale = 1.0f;

    float ui_scale() const { return ui_scale_; }
    void set_ui_scale(float scale) { ui_scale_ = scale; }

    float font_scale() const { return font_scale_; }
    void set_font_scale(float scale) { font_scale_ = scale; }

    // Returns the relative scene path last opened for `project_dir`, or empty if none recorded.
    std::string last_scene(const std::filesystem::path& project_dir) const;

    // Records the relative scene path last opened for `project_dir`.
    void set_last_scene(const std::filesystem::path& project_dir, std::string_view scene_path);

    // Named layouts (View > Save layout): panel sizes and tabs only, never theme, scale or user lists.
    static constexpr std::size_t max_layout_name = 40;
    static std::string clean_layout_name(std::string_view name); // trimmed, no '=', '[', ']', '#', line breaks; empty when nothing is left
    const std::map<std::string, RmlPanels>& layouts() const { return layouts_; }
    bool save_layout_as(std::string_view name, const RmlPanels& panels); // false: unusable name
    bool delete_layout(std::string_view name);

    // Script editor: format the document on save (palette toggle), and line breakpoints per project.
    bool format_on_save() const { return format_on_save_; }
    void set_format_on_save(bool on) { format_on_save_ = on; }
    using BreakpointList = std::vector<std::pair<std::string, int>>; // project relative file, 1-based line
    BreakpointList breakpoints(const std::filesystem::path& project_dir) const;
    void set_breakpoints(const std::filesystem::path& project_dir, const BreakpointList& list);

    RmlLayout& rml_layout() { return rml_layout_; }
    const RmlLayout& rml_layout() const { return rml_layout_; }

    std::string serialize() const;
    void parse(std::string_view text);

    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file, std::string& error) const;

    // <user data dir>/editor-settings.txt
    static std::filesystem::path user_file();

private:
    float ui_scale_ = default_ui_scale;
    float font_scale_ = default_font_scale;
    std::map<std::string, std::string> last_scenes_;
    std::map<std::string, std::string> breakpoints_; // project -> "file:line|file:line"
    bool format_on_save_ = true;
    RmlLayout rml_layout_;
    std::map<std::string, RmlPanels> layouts_;
};

} // namespace cuervo::editor

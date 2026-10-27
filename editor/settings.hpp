#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace cuervo::editor {

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
};

} // namespace cuervo::editor

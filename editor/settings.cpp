#include "editor/settings.hpp"

#include <charconv>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

#include "cuervo/platform/platform.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

namespace {

std::string normalize_path(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::canonical(p, ec);
    if (ec) c = fs::absolute(p, ec);
    return platform::utf8(c);
}

} // namespace

std::string EditorSettings::last_scene(const fs::path& project_dir) const {
    const std::string key = normalize_path(project_dir);
    const auto it = last_scenes_.find(key);
    if (it != last_scenes_.end()) return it->second;
    return {};
}

void EditorSettings::set_last_scene(const fs::path& project_dir, std::string_view scene_path) {
    if (project_dir.empty() || scene_path.empty()) return;
    const std::string key = normalize_path(project_dir);
    last_scenes_[key] = std::string(scene_path);
}

std::string EditorSettings::serialize() const {
    std::ostringstream out;
    out << "ui_scale = " << ui_scale_ << "\n";
    out << "font_scale = " << font_scale_ << "\n";
    out << "[last_scenes]\n";
    for (const auto& [proj, scene] : last_scenes_) {
        out << proj << " = " << scene << "\n";
    }
    return out.str();
}

void EditorSettings::parse(std::string_view text) {
    std::string_view remaining = text;
    bool in_last_scenes = false;

    while (!remaining.empty()) {
        const std::size_t nl = remaining.find('\n');
        std::string_view cur = remaining.substr(0, nl);
        remaining = (nl == std::string_view::npos) ? std::string_view{} : remaining.substr(nl + 1);

        while (!cur.empty() && (cur.front() == ' ' || cur.front() == '\t' || cur.front() == '\r')) cur.remove_prefix(1);
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t' || cur.back() == '\r')) cur.remove_suffix(1);
        if (cur.empty() || cur.front() == '#') continue;

        if (cur == "[last_scenes]") {
            in_last_scenes = true;
            continue;
        }

        const std::size_t eq = cur.find('=');
        if (eq == std::string_view::npos) continue;

        std::string_view key = cur.substr(0, eq);
        std::string_view val = cur.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);

        if (!in_last_scenes) {
            if (key == "ui_scale") {
                float v = 1.0f;
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc() && v > 0.1f && v < 10.0f) {
                    ui_scale_ = v;
                }
            } else if (key == "font_scale") {
                float v = 1.0f;
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc() && v > 0.1f && v < 10.0f) {
                    font_scale_ = v;
                }
            }
        } else {
            last_scenes_[std::string(key)] = std::string(val);
        }
    }
}

bool EditorSettings::load(const fs::path& file) {
    if (file.empty()) return false;
    std::ifstream in(file);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    parse(ss.str());
    return true;
}

bool EditorSettings::save(const fs::path& file, std::string& error) const {
    if (file.empty()) {
        error = "There is no per-user folder to keep editor settings in.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        error = std::format("Could not write '{}': {}", platform::utf8(file), std::error_code(errno, std::generic_category()).message());
        return false;
    }
    out << serialize();
    return true;
}

fs::path EditorSettings::user_file() {
    const fs::path dir = platform::user_data_dir("Cuervo Engine");
    return dir.empty() ? fs::path{} : dir / "editor-settings.txt";
}

} // namespace cuervo::editor

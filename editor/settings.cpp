#include "editor/settings.hpp"

#include <algorithm>
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

EditorSettings::BreakpointList EditorSettings::breakpoints(const std::filesystem::path& project_dir) const {
    BreakpointList out;
    const auto it = breakpoints_.find(normalize_path(project_dir));
    if (it == breakpoints_.end()) return out;
    std::string_view rest = it->second;
    while (!rest.empty()) {
        const std::size_t bar = rest.find('|');
        const std::string_view item = rest.substr(0, bar);
        rest = bar == std::string_view::npos ? std::string_view{} : rest.substr(bar + 1);
        const std::size_t colon = item.rfind(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        int line = 0;
        const std::string_view num = item.substr(colon + 1);
        if (std::from_chars(num.data(), num.data() + num.size(), line).ec != std::errc() || line < 1) continue;
        out.emplace_back(std::string(item.substr(0, colon)), line);
    }
    return out;
}

void EditorSettings::set_breakpoints(const std::filesystem::path& project_dir, const BreakpointList& list) {
    std::string text;
    for (const auto& [file, line] : list) {
        if (file.find_first_of("|\r\n") != std::string::npos) continue;
        if (!text.empty()) text += '|';
        text += file + ":" + std::to_string(line);
    }
    const std::string key = normalize_path(project_dir);
    if (text.empty()) breakpoints_.erase(key);
    else breakpoints_[key] = text;
}

std::string EditorSettings::serialize() const {
    std::ostringstream out;
    out << "ui_scale = " << ui_scale_ << "\n";
    out << "font_scale = " << font_scale_ << "\n";
    out << "format_on_save = " << (format_on_save_ ? 1 : 0) << "\n";
    out << "[last_scenes]\n";
    for (const auto& [proj, scene] : last_scenes_) {
        out << proj << " = " << scene << "\n";
    }
    out << "[breakpoints]\n";
    for (const auto& [proj, list] : breakpoints_) out << proj << " = " << list << "\n";
    const RmlLayout& l = rml_layout_;
    out << "[rml_layout]\n";
    out << "left_width = " << l.left_width << "\nright_width = " << l.right_width << "\ndrawer_height = " << l.drawer_height
        << "\ntree_height = " << l.tree_height << "\nleft_collapsed = " << l.left_collapsed << "\nright_collapsed = " << l.right_collapsed
        << "\ndrawer_open = " << l.drawer_open << "\nrail = " << l.rail << "\ncenter = " << l.center << "\ndrawer = " << l.drawer
        << "\ntheme = " << l.theme << "\nui_scale = " << l.ui_scale << "\nclosed_sections = " << l.closed_sections << "\ntree_scene = " << l.tree_scene << "\ntree_collapsed = " << l.tree_collapsed << "\nadd_recents = " << l.add_recents << "\nadd_favorites = " << l.add_favorites << "\n";
    out << "[rml_layouts]\n";
    for (const auto& [name, panels] : layouts_) out << name << " = " << panels.serialize() << "\n";
    return out.str();
}

bool RmlPanels::set(std::string_view key, std::string_view val) {
    const auto num = [&](int& out, int lo, int hi) {
        int v = 0;
        if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc()) out = std::clamp(v, lo, hi);
    };
    const auto flag = [&](bool& out) { out = val == "1" || val == "true"; };
    const auto word = [&](std::string& out, std::initializer_list<std::string_view> allowed) {
        for (const std::string_view a : allowed) if (val == a) out = std::string(val);
    };
    if (key == "left_width") num(left_width, 160, 800);
    else if (key == "right_width") num(right_width, 200, 900);
    else if (key == "drawer_height") num(drawer_height, 80, 900);
    else if (key == "tree_height") num(tree_height, 80, 1200);
    else if (key == "left_collapsed") flag(left_collapsed);
    else if (key == "right_collapsed") flag(right_collapsed);
    else if (key == "drawer_open") flag(drawer_open);
    else if (key == "rail") word(rail, {"scene", "assets", "search", "settings"});
    else if (key == "center") word(center, {"scene", "script"});
    else if (key == "drawer") word(drawer, {"console", "history", "profiler", "node"});
    else return false;
    return true;
}

std::string RmlPanels::serialize() const {
    std::ostringstream out;
    out << "left_width=" << left_width << ";right_width=" << right_width << ";drawer_height=" << drawer_height << ";tree_height=" << tree_height
        << ";left_collapsed=" << left_collapsed << ";right_collapsed=" << right_collapsed << ";drawer_open=" << drawer_open
        << ";rail=" << rail << ";center=" << center << ";drawer=" << drawer;
    return out.str();
}

RmlPanels RmlPanels::parse(std::string_view text) {
    RmlPanels p;
    while (!text.empty()) {
        const std::size_t semi = text.find(';');
        const std::string_view item = text.substr(0, semi);
        text = semi == std::string_view::npos ? std::string_view{} : text.substr(semi + 1);
        const std::size_t eq = item.find('=');
        if (eq != std::string_view::npos) p.set(item.substr(0, eq), item.substr(eq + 1));
    }
    return p;
}

std::string EditorSettings::clean_layout_name(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if (c == '=' || c == '[' || c == ']' || c == '#' || c == '\n' || c == '\r' || c == '\t') continue;
        out += c;
    }
    const std::size_t a = out.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    out = out.substr(a, out.find_last_not_of(' ') - a + 1);
    if (out.size() > max_layout_name) out.resize(max_layout_name);
    return out;
}

bool EditorSettings::save_layout_as(std::string_view name, const RmlPanels& panels) {
    const std::string clean = clean_layout_name(name);
    if (clean.empty()) return false;
    layouts_[clean] = panels;
    return true;
}

bool EditorSettings::delete_layout(std::string_view name) { return layouts_.erase(std::string(name)) > 0; }

void EditorSettings::parse(std::string_view text) {
    std::string_view remaining = text;
    enum class Section { Top, LastScenes, Breakpoints, RmlLayout, Layouts } section = Section::Top;

    while (!remaining.empty()) {
        const std::size_t nl = remaining.find('\n');
        std::string_view cur = remaining.substr(0, nl);
        remaining = (nl == std::string_view::npos) ? std::string_view{} : remaining.substr(nl + 1);

        while (!cur.empty() && (cur.front() == ' ' || cur.front() == '\t' || cur.front() == '\r')) cur.remove_prefix(1);
        while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t' || cur.back() == '\r')) cur.remove_suffix(1);
        if (cur.empty() || cur.front() == '#') continue;

        if (cur == "[last_scenes]") {
            section = Section::LastScenes;
            continue;
        }
        if (cur == "[breakpoints]") {
            section = Section::Breakpoints;
            continue;
        }
        if (cur == "[rml_layout]") {
            section = Section::RmlLayout;
            continue;
        }
        if (cur == "[rml_layouts]") {
            section = Section::Layouts;
            continue;
        }

        const std::size_t eq = cur.find('=');
        if (eq == std::string_view::npos) continue;

        std::string_view key = cur.substr(0, eq);
        std::string_view val = cur.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);

        if (section == Section::Top) {
            if (key == "ui_scale") {
                float v = 1.0f;
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc() && v > 0.1f && v < 10.0f) {
                    ui_scale_ = v;
                }
            } else if (key == "format_on_save") {
                format_on_save_ = val != "0";
            } else if (key == "font_scale") {
                float v = 1.0f;
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc() && v > 0.1f && v < 10.0f) {
                    font_scale_ = v;
                }
            }
        } else if (section == Section::RmlLayout) {
            RmlLayout& l = rml_layout_;
            const auto word = [&](std::string& out, std::initializer_list<std::string_view> allowed) {
                for (const std::string_view a : allowed) if (val == a) out = std::string(val);
            };
            if (l.set(key, val)) {}
            else if (key == "theme") word(l.theme, {"dark", "light"});
            else if (key == "ui_scale") {
                float v = 0.0f;
                if (std::from_chars(val.data(), val.data() + val.size(), v).ec == std::errc() && v >= 0.5f && v <= 4.0f) l.ui_scale = v;
            } else if (key == "closed_sections") l.closed_sections = std::string(val);
            else if (key == "tree_scene") l.tree_scene = std::string(val);
            else if (key == "tree_collapsed") l.tree_collapsed = std::string(val);
            else if (key == "add_recents") l.add_recents = std::string(val);
            else if (key == "add_favorites") l.add_favorites = std::string(val);
        } else if (section == Section::Layouts) {
            const std::string name = clean_layout_name(key);
            if (!name.empty()) layouts_[name] = RmlPanels::parse(val);
        } else if (section == Section::Breakpoints) {
            breakpoints_[normalize_path(fs::path(std::string(key)))] = std::string(val);
        } else {
            // Keys are normalized like set/lookup so a file written elsewhere (or a literal
            // POSIX path on Windows, where absolute() adds a drive) still matches.
            last_scenes_[normalize_path(fs::path(std::string(key)))] = std::string(val);
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

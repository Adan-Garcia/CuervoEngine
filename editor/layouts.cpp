#include "editor/layouts.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

#include "cuervo/platform/platform.hpp"

namespace cuervo::editor {

namespace {

constexpr std::string_view builtin_names[] = {"Default", "Debugging", "Minimal"};

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    return std::string(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
}

bool parse_float(std::string_view s, float& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

void write_layout(std::ostream& out, const Layout& l) {
    out << std::format("left={:.4f}\nright={:.4f}\nbottom={:.4f}\n", l.left, l.right, l.bottom);
    out << "hidden=";
    for (std::size_t i = 0; i < l.hidden.size(); ++i) out << (i != 0 ? "," : "") << l.hidden[i];
    out << '\n';
    // ImGui's ini text is multi-line: kept on one line with \n and \\ escaped.
    if (!l.dock.empty()) {
        out << "dock=";
        for (const char ch : l.dock) {
            if (ch == '\n') out << "\\n";
            else if (ch == '\\') out << "\\\\";
            else if (ch != '\r') out << ch;
        }
        out << '\n';
    }
}

std::string unescape_dock(std::string_view v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            ++i;
            out += v[i] == 'n' ? '\n' : v[i];
        } else {
            out += v[i];
        }
    }
    return out;
}

} // namespace

bool Layout::is_hidden(std::string_view title) const {
    return std::find(hidden.begin(), hidden.end(), title) != hidden.end();
}

void Layout::set_hidden(std::string_view title, bool hide) {
    const auto it = std::find(hidden.begin(), hidden.end(), title);
    if (hide && it == hidden.end()) hidden.emplace_back(title);
    if (!hide && it != hidden.end()) hidden.erase(it);
}

void Layout::clamp() {
    left = std::clamp(left, 0.08f, 0.40f);
    right = std::clamp(right, 0.08f, 0.45f);
    bottom = std::clamp(bottom, 0.08f, 0.70f);
    if (left + right > 0.75f) {
        const float k = 0.75f / (left + right);
        left *= k;
        right *= k;
    }
}

LayoutStore::LayoutStore() {
    layouts_["Default"] = {};
    Layout debugging;
    debugging.left = 0.17f;
    debugging.right = 0.26f;
    debugging.bottom = 0.40f;
    layouts_["Debugging"] = debugging;
    Layout minimal;
    minimal.left = 0.16f;
    minimal.right = 0.20f;
    minimal.bottom = 0.14f;
    minimal.hidden = {"Console", "Replay", "Input Map", "Tier Report", "Profiler", "History"};
    layouts_["Minimal"] = minimal;
    current_ = layouts_["Default"];
}

bool LayoutStore::is_builtin(std::string_view name) {
    return std::find(std::begin(builtin_names), std::end(builtin_names), name) != std::end(builtin_names);
}

void LayoutStore::set_active(std::string name) { active_ = contains(name) ? std::move(name) : std::string(default_name); }

const Layout& LayoutStore::layout(std::string_view name) const {
    const auto it = layouts_.find(std::string(name));
    return it != layouts_.end() ? it->second : layouts_.at(default_name);
}

std::vector<std::string> LayoutStore::names() const {
    std::vector<std::string> out(std::begin(builtin_names), std::end(builtin_names));
    for (const auto& [name, layout] : layouts_) {
        if (!is_builtin(name)) out.push_back(name);
    }
    return out;
}

bool LayoutStore::save_as(std::string name, const Layout& layout, std::string& error) {
    name = trim(name);
    if (name.empty()) {
        error = "A layout needs a name.";
        return false;
    }
    if (is_builtin(name)) {
        error = std::format("'{}' is a built-in layout and can't be replaced. Pick another name.", name);
        return false;
    }
    if (name.find_first_of("[]\n=,") != std::string::npos) {
        error = "A layout name can't contain [ ] = , or line breaks.";
        return false;
    }
    Layout l = layout;
    l.clamp();
    layouts_[name] = l;
    active_ = name;
    return true;
}

bool LayoutStore::remove(std::string_view name, std::string& error) {
    if (is_builtin(name)) {
        error = std::format("'{}' is a built-in layout and can't be deleted.", name);
        return false;
    }
    if (layouts_.erase(std::string(name)) == 0) {
        error = std::format("There is no layout named '{}'.", name);
        return false;
    }
    if (active_ == name) active_ = default_name;
    return true;
}

std::string LayoutStore::serialize() const {
    std::ostringstream out;
    out << "# Cuervo editor layouts (per user). Edited by the editor; safe to delete.\n";
    out << "active=" << active_ << "\n[current]\n";
    write_layout(out, current_);
    for (const auto& [name, layout] : layouts_) {
        if (is_builtin(name)) continue;
        out << "[layout " << name << "]\n";
        write_layout(out, layout);
    }
    return out.str();
}

bool LayoutStore::parse(std::string_view text, std::string& error) {
    LayoutStore fresh;
    Layout* target = nullptr;
    std::string active = default_name;
    std::istringstream in{std::string(text)};
    std::string raw;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        const std::string l = trim(raw);
        if (l.empty() || l[0] == '#') continue;
        if (l.front() == '[') {
            if (l.back() != ']') {
                error = std::format("line {}: unterminated section header", line);
                return false;
            }
            const std::string head = l.substr(1, l.size() - 2);
            if (head == "current") {
                target = &fresh.current_;
            } else if (head.starts_with("layout ")) {
                const std::string name = trim(head.substr(7));
                if (name.empty() || is_builtin(name)) {
                    error = std::format("line {}: bad layout name '{}'", line, name);
                    return false;
                }
                target = &fresh.layouts_[name];
            } else {
                error = std::format("line {}: unknown section '{}'", line, head);
                return false;
            }
            continue;
        }
        const std::size_t eq = l.find('=');
        if (eq == std::string::npos) {
            error = std::format("line {}: expected key=value", line);
            return false;
        }
        const std::string key = trim(l.substr(0, eq));
        const std::string value = trim(l.substr(eq + 1));
        if (key == "active" && target == nullptr) {
            active = value;
            continue;
        }
        if (target == nullptr) {
            error = std::format("line {}: '{}' outside a section", line, key);
            return false;
        }
        float f = 0.0f;
        if (key == "left" || key == "right" || key == "bottom") {
            if (!parse_float(value, f)) {
                error = std::format("line {}: '{}' is not a number", line, value);
                return false;
            }
            (key == "left" ? target->left : key == "right" ? target->right : target->bottom) = f;
        } else if (key == "dock") {
            target->dock = unescape_dock(value);
        } else if (key == "hidden") {
            target->hidden.clear();
            std::istringstream names(value);
            std::string n;
            while (std::getline(names, n, ',')) {
                if (!trim(n).empty()) target->hidden.push_back(trim(n));
            }
        }
    }
    fresh.current_.clamp();
    for (auto& [name, layout] : fresh.layouts_) layout.clamp();
    fresh.set_active(active);
    *this = std::move(fresh);
    return true;
}

std::filesystem::path LayoutStore::user_file() {
    const std::filesystem::path dir = platform::user_data_dir("Cuervo Engine");
    return dir.empty() ? std::filesystem::path{} : dir / "editor-layouts.txt";
}

bool LayoutStore::load(const std::filesystem::path& file, std::string& error) {
    std::ifstream in(file);
    if (!in) return true; // none saved yet
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), error);
}

bool LayoutStore::save(const std::filesystem::path& file, std::string& error) const {
    if (file.empty()) {
        error = "There is no per-user folder to keep layouts in.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << serialize();
    out.flush();
    if (!out) {
        error = std::format("Could not write '{}'.", platform::utf8(file));
        return false;
    }
    return true;
}

} // namespace cuervo::editor

#include "cuervo/script/project.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/name.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/plugin/cuervo_plugin.h"

namespace cuervo::script {

namespace fs = std::filesystem;

namespace {

// Spellings of cuervo::Key, in enum order.
constexpr std::string_view key_names[] = {
    "Unknown", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U",
    "V", "W", "X", "Y", "Z", "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9", "Space",
    "Enter", "Escape", "Tab", "Backspace", "Left", "Right", "Up", "Down", "LeftShift", "RightShift", "LeftCtrl",
    "RightCtrl", "LeftAlt", "RightAlt", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Minus", "Equals", "LeftBracket", "RightBracket", "Backslash", "Semicolon", "Apostrophe", "Grave", "Comma",
    "Period", "Slash", "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "Kp0", "Kp1", "Kp2", "Kp3", "Kp4",
    "Kp5", "Kp6", "Kp7", "Kp8", "Kp9", "KpPlus", "KpMinus", "KpMultiply", "KpDivide", "KpPeriod"};
static_assert(std::size(key_names) == static_cast<std::size_t>(Key::Count));

constexpr std::string_view pad_names[] = {"South", "East", "West", "North", "Back", "Guide", "Start", "LeftStick",
                                          "RightStick", "LeftShoulder", "RightShoulder", "DpadUp", "DpadDown",
                                          "DpadLeft", "DpadRight"};
static_assert(std::size(pad_names) == static_cast<std::size_t>(PadButton::Count));

// A 2D axis source: its x and y parts.
bool parse_axis2_impl(std::string_view s, AxisSource& x, AxisSource& y) {
    auto keys = [&](Key l, Key r, Key u, Key d) {
        x = AxisSource::digital(Binding::key(l), Binding::key(r));
        y = AxisSource::digital(Binding::key(u), Binding::key(d)); // y down
        return true;
    };
    if (s == "Keys.Arrows") return keys(Key::Left, Key::Right, Key::Up, Key::Down);
    if (s == "Keys.WASD") return keys(Key::A, Key::D, Key::W, Key::S);
    if (s == "Pad.LeftStick" || s == "Pad.RightStick") {
        bool left = s == "Pad.LeftStick";
        x = AxisSource::stick(left ? PadAxis::LeftX : PadAxis::RightX);
        y = AxisSource::stick(left ? PadAxis::LeftY : PadAxis::RightY);
        return true;
    }
    if (s == "Pad.Dpad") {
        x = AxisSource::digital(Binding::button(PadButton::DpadLeft), Binding::button(PadButton::DpadRight));
        y = AxisSource::digital(Binding::button(PadButton::DpadUp), Binding::button(PadButton::DpadDown));
        return true;
    }
    return false;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Strips a trailing `# comment` that is not inside a string.
std::string_view strip_comment(std::string_view s) {
    bool in_str = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') in_str = !in_str;
        if (s[i] == '#' && !in_str) return s.substr(0, i);
    }
    return s;
}

struct TomlValue {
    enum class Kind { String, Number, Bool, Array } kind = Kind::String;
    std::string str;
    double num = 0.0;
    bool b = false;
    std::vector<TomlValue> items;
};

bool parse_value(std::string_view s, TomlValue& out) {
    s = trim(s);
    if (s.empty()) return false;
    if (s.front() == '"') {
        if (s.size() < 2 || s.back() != '"') return false;
        out.kind = TomlValue::Kind::String;
        out.str = std::string(s.substr(1, s.size() - 2));
        return true;
    }
    if (s == "true" || s == "false") {
        out.kind = TomlValue::Kind::Bool;
        out.b = s == "true";
        return true;
    }
    if (s.front() == '[') {
        if (s.back() != ']') return false;
        out.kind = TomlValue::Kind::Array;
        std::string_view body = trim(s.substr(1, s.size() - 2));
        while (!body.empty()) {
            std::size_t comma = std::string_view::npos;
            bool in_str = false;
            for (std::size_t i = 0; i < body.size(); ++i) {
                if (body[i] == '"') in_str = !in_str;
                if (body[i] == ',' && !in_str) {
                    comma = i;
                    break;
                }
            }
            TomlValue item;
            std::string_view part = trim(body.substr(0, comma));
            if (!part.empty()) {
                if (!parse_value(part, item)) return false;
                out.items.push_back(std::move(item));
            }
            if (comma == std::string_view::npos) break;
            body = trim(body.substr(comma + 1));
        }
        return true;
    }
    out.kind = TomlValue::Kind::Number;
    std::string text(s);
    std::erase(text, '_');
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out.num);
    return ec == std::errc() && ptr == text.data() + text.size();
}

// `[dependencies]` (`name = "1.2.0"`) and `[dependencies.name]` (`path`,
// `version`) in project.toml and package.toml. Returns false and sets
// `message` on a bad entry; `handled` is false for other sections.
bool dependency_entry(const std::string& section, const std::string& key, const TomlValue& v, int line,
                      std::vector<Dependency>& deps, bool& handled, std::string& message) {
    handled = false;
    if (section == "dependencies") {
        handled = true;
        if (!valid_package_name(key)) {
            message = std::format("'{}' is not a valid package name (letters, digits and '_', not starting with a "
                                  "digit)",
                                  key);
            return false;
        }
        if (v.kind != TomlValue::Kind::String || !valid_version(v.str)) {
            message = std::format("dependency '{}' needs a pinned version, e.g. {} = \"1.0.0\"", key, key);
            return false;
        }
        for (const Dependency& d : deps) {
            if (d.name == key) {
                message = std::format("dependency '{}' is listed twice", key);
                return false;
            }
        }
        deps.push_back(Dependency{key, v.str, {}, line});
        return true;
    }
    if (!section.starts_with("dependencies.")) return true;
    handled = true;
    const std::string name = section.substr(std::string_view("dependencies.").size());
    if (!valid_package_name(name)) {
        message = std::format("'{}' is not a valid package name", name);
        return false;
    }
    auto it = std::find_if(deps.begin(), deps.end(), [&](const Dependency& d) { return d.name == name; });
    if (it == deps.end()) {
        deps.push_back(Dependency{name, {}, {}, line});
        it = deps.end() - 1;
    }
    if (v.kind != TomlValue::Kind::String) {
        message = std::format("'{}' in [{}] must be a string", key, section);
        return false;
    }
    if (key == "version") {
        if (!valid_version(v.str)) {
            message = std::format("version \"{}\" of '{}' must be MAJOR.MINOR.PATCH", v.str, name);
            return false;
        }
        it->version = v.str;
    } else if (key == "path") {
        it->path = v.str;
    } else {
        message = std::format("unknown key '{}' in [{}] (path, version)", key, section);
        return false;
    }
    return true;
}

bool check_dependencies(const std::vector<Dependency>& deps, const std::string& file, std::string& error) {
    for (const Dependency& d : deps) {
        if (d.version.empty()) {
            error = std::format("{}:{}: dependency '{}' needs a pinned 'version'", file, d.line, d.name);
            return false;
        }
    }
    return true;
}

} // namespace

bool valid_package_name(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::optional<Version> parse_version(std::string_view text, bool partial) {
    if (!partial && !valid_version(text)) return std::nullopt;
    int parts[3] = {0, 0, 0};
    int n = 0;
    while (true) {
        const std::size_t dot = text.find('.');
        const std::string_view digits = text.substr(0, dot);
        if (n == 3 || digits.empty() || digits.size() > 9) return std::nullopt;
        auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), parts[n]);
        if (ec != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
        ++n;
        if (dot == std::string_view::npos) break;
        text.remove_prefix(dot + 1);
    }
    return Version{parts[0], parts[1], parts[2]};
}

bool VersionRange::contains(const Version& v) const {
    for (const Clause& c : clauses) {
        switch (c.op) {
            case Op::Eq: if (v != c.v) return false; break;
            case Op::Ge: if (v < c.v) return false; break;
            case Op::Gt: if (v <= c.v) return false; break;
            case Op::Le: if (v > c.v) return false; break;
            case Op::Lt: if (v >= c.v) return false; break;
            case Op::Caret: {
                const Version upper = c.v.major > 0 ? Version{c.v.major + 1, 0, 0} : Version{0, c.v.minor + 1, 0};
                if (v < c.v || v >= upper) return false;
                break;
            }
        }
    }
    return true;
}

std::optional<VersionRange> parse_version_range(std::string_view text, std::string& error) {
    VersionRange r;
    r.text = std::string(trim(text));
    if (r.text.empty() || r.text == "*") {
        r.text = "*";
        return r;
    }
    std::string_view rest = r.text;
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        std::string_view clause = trim(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        VersionRange::Clause c;
        if (clause.starts_with(">=")) {
            c.op = VersionRange::Op::Ge;
            clause.remove_prefix(2);
        } else if (clause.starts_with("<=")) {
            c.op = VersionRange::Op::Le;
            clause.remove_prefix(2);
        } else if (clause.starts_with(">")) {
            c.op = VersionRange::Op::Gt;
            clause.remove_prefix(1);
        } else if (clause.starts_with("<")) {
            c.op = VersionRange::Op::Lt;
            clause.remove_prefix(1);
        } else if (clause.starts_with("^")) {
            c.op = VersionRange::Op::Caret;
            clause.remove_prefix(1);
        } else if (clause.starts_with("=")) {
            clause.remove_prefix(1);
        }
        auto v = parse_version(trim(clause), true);
        if (!v) {
            error = std::format("invalid version range \"{}\" (e.g. \">=1.2, <2\", \"^1.2.0\", \"1.4.0\")", r.text);
            return std::nullopt;
        }
        c.v = *v;
        r.clauses.push_back(c);
    }
    return r;
}

std::string_view engine_version() { return CUERVO_ENGINE_VERSION; }

int plugin_abi_version() { return CUERVO_PLUGIN_ABI_VERSION; }

bool valid_version(std::string_view version) {
    int parts = 0;
    while (!version.empty()) {
        std::size_t dot = version.find('.');
        std::string_view n = version.substr(0, dot);
        if (n.empty() || n.size() > 9 || (n.size() > 1 && n.front() == '0')) return false;
        for (char c : n) {
            if (c < '0' || c > '9') return false;
        }
        ++parts;
        if (dot == std::string_view::npos) break;
        version.remove_prefix(dot + 1);
        if (version.empty()) return false;
    }
    return parts == 3;
}

// ------------------------------------------------------------ binding names

namespace {

struct NamedAxisBinding {
    std::string_view name;
    Binding binding;
};

// Sticks and triggers pushed past the digital threshold, as buttons (menus).
constexpr NamedAxisBinding axis_bindings[] = {
    {"Pad.LeftStickUp", Binding::axis_negative(PadAxis::LeftY)},
    {"Pad.LeftStickDown", Binding::axis_positive(PadAxis::LeftY)},
    {"Pad.LeftStickLeft", Binding::axis_negative(PadAxis::LeftX)},
    {"Pad.LeftStickRight", Binding::axis_positive(PadAxis::LeftX)},
    {"Pad.RightStickUp", Binding::axis_negative(PadAxis::RightY)},
    {"Pad.RightStickDown", Binding::axis_positive(PadAxis::RightY)},
    {"Pad.RightStickLeft", Binding::axis_negative(PadAxis::RightX)},
    {"Pad.RightStickRight", Binding::axis_positive(PadAxis::RightX)},
    {"Pad.LeftTrigger", Binding::axis_positive(PadAxis::LeftTrigger)},
    {"Pad.RightTrigger", Binding::axis_positive(PadAxis::RightTrigger)},
};

constexpr std::string_view mouse_names[] = {"Left", "Middle", "Right", "X1", "X2"};
static_assert(std::size(mouse_names) == static_cast<std::size_t>(MouseButton::Count));

constexpr std::string_view axis2_names[] = {"Keys.Arrows", "Keys.WASD", "Pad.LeftStick", "Pad.RightStick", "Pad.Dpad"};

} // namespace

std::optional<Binding> parse_binding(std::string_view s) {
    if (s.starts_with("Key.")) {
        for (std::size_t i = 1; i < std::size(key_names); ++i) {
            if (key_names[i] == s.substr(4)) return Binding::key(static_cast<Key>(i));
        }
        return std::nullopt;
    }
    for (const NamedAxisBinding& a : axis_bindings) {
        if (a.name == s) return a.binding;
    }
    if (s.starts_with("Pad.")) {
        for (std::size_t i = 0; i < std::size(pad_names); ++i) {
            if (pad_names[i] == s.substr(4)) return Binding::button(static_cast<PadButton>(i));
        }
    } else if (s.starts_with("Mouse.")) {
        for (std::size_t i = 0; i < std::size(mouse_names); ++i) {
            if (mouse_names[i] == s.substr(6)) return Binding::mouse(static_cast<MouseButton>(i));
        }
    } else if (s == "Wheel.Up") {
        return Binding::wheel_up();
    } else if (s == "Wheel.Down") {
        return Binding::wheel_down();
    }
    return std::nullopt;
}

std::string binding_name(Binding b) {
    switch (b.source) {
        case Binding::Source::Key:
            if (b.code > 0 && b.code < std::size(key_names)) return "Key." + std::string(key_names[b.code]);
            return {};
        case Binding::Source::PadButton:
            if (b.code < std::size(pad_names)) return "Pad." + std::string(pad_names[b.code]);
            return {};
        case Binding::Source::MouseButton:
            if (b.code < std::size(mouse_names)) return "Mouse." + std::string(mouse_names[b.code]);
            return {};
        case Binding::Source::WheelUp: return "Wheel.Up";
        case Binding::Source::WheelDown: return "Wheel.Down";
        case Binding::Source::PadAxisPositive:
        case Binding::Source::PadAxisNegative:
            for (const NamedAxisBinding& a : axis_bindings) {
                if (a.binding.source == b.source && a.binding.code == b.code) return std::string(a.name);
            }
            return {};
    }
    return {};
}

bool parse_axis2(std::string_view s, AxisSource& x, AxisSource& y) { return parse_axis2_impl(s, x, y); }

std::span<const std::string_view> axis2_source_names() { return axis2_names; }

std::optional<Project> load_project(const fs::path& dir, std::string& error) {
    return load_project(*assets::Vfs::dev(dir), error);
}

std::optional<Project> load_project(const assets::Vfs& vfs, std::string& error) {
    std::string text;
    if (!vfs.read_text("project.toml", text, error)) {
        error = std::format("cannot read '{}'", vfs.describe("project.toml"));
        return std::nullopt;
    }
    const std::string file = vfs.describe("project.toml");
    std::istringstream in(text);
    Project p;
    p.dir = vfs.root_dir();
    std::string section;
    std::string line;
    int number = 0;
    auto fail = [&](std::string message) {
        error = std::format("{}:{}: {}", file, number, message);
        return std::nullopt;
    };
    while (std::getline(in, line)) {
        ++number;
        std::string_view s = trim(strip_comment(line));
        if (s.empty()) continue;
        if (s.front() == '[') {
            if (s.back() != ']') return fail("expected ']'");
            section = std::string(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        std::size_t eq = s.find('=');
        if (eq == std::string_view::npos) return fail("expected 'key = value'");
        std::string key(trim(s.substr(0, eq)));
        TomlValue v;
        if (!parse_value(s.substr(eq + 1), v)) return fail(std::format("invalid value for '{}'", key));
        auto strings = [&](std::vector<std::string>& out) {
            if (v.kind != TomlValue::Kind::Array) return false;
            for (const TomlValue& item : v.items) {
                if (item.kind != TomlValue::Kind::String) return false;
                out.push_back(item.str);
            }
            return true;
        };
        if (section == "input.actions" || section == "input.axes") {
            std::vector<std::string> list;
            if (!strings(list)) return fail(std::format("'{}' must be an array of strings", key));
            (section == "input.actions" ? p.actions : p.axes).emplace_back(key, std::move(list));
            continue;
        }
        if (section == "features") {
            if (v.kind != TomlValue::Kind::Bool) return fail(std::format("'{}' in [features] must be a boolean", key));
            if (key == "audio") p.features.audio = v.b;
            else if (key == "ui") p.features.ui = v.b;
            else if (key == "particles") p.features.particles = v.b;
            else if (key == "collision") p.features.collision = v.b;
            else if (key == "vulkan") p.features.vulkan = v.b;
            else if (key == "svg_import") p.features.svg_import = v.b;
            else if (key == "font_import") p.features.font_import = v.b;
            else if (key == "dev") p.features.dev = v.b;
            else return fail(std::format("unknown feature '{}' in [features]", key));
            continue;
        }
        {
            bool handled = false;
            std::string message;
            if (!dependency_entry(section, key, v, number, p.dependencies, handled, message)) return fail(message);
            if (handled) continue;
        }
        if (section == "scene") {
            if (key == "transform_hierarchy") {
                if (v.kind != TomlValue::Kind::Bool) return fail("'transform_hierarchy' in [scene] must be a boolean");
                p.transform_hierarchy = v.b;
            } else {
                return fail(std::format("unknown key '{}' in [scene]", key));
            }
            continue;
        }
        if (!section.empty()) continue; // other sections are for later tools
        if (key == "name" && v.kind == TomlValue::Kind::String) {
            p.name = v.str;
        } else if (key == "main" && v.kind == TomlValue::Kind::String) {
            p.main_class = v.str;
        } else if (key == "main_scene" && v.kind == TomlValue::Kind::String) {
            const std::filesystem::path rel(v.str);
            bool escapes = false;
            for (const auto& part : rel) escapes = escapes || part == "..";
            if (v.str.empty() || rel.is_absolute() || escapes) {
                return fail("main_scene must be a path inside the project, e.g. \"scenes/level.scene\"");
            }
            p.main_scene = v.str;
        } else if (key == "tick_rate" && v.kind == TomlValue::Kind::Number && v.num > 0.0) {
            p.tick_rate = static_cast<float>(v.num);
        } else if (key == "seed" && v.kind == TomlValue::Kind::Number) {
            p.seed = static_cast<std::uint64_t>(v.num);
        } else if (key == "jobs" && v.kind == TomlValue::Kind::Number && v.num >= 0.0 && v.num <= 256.0) {
            p.jobs = static_cast<unsigned>(v.num);
        } else if (key == "virtual_size" && v.kind == TomlValue::Kind::Array && v.items.size() == 2) {
            p.virtual_size = {static_cast<int>(v.items[0].num), static_cast<int>(v.items[1].num)};
        } else if (key == "clear_color" && v.kind == TomlValue::Kind::String) {
            unsigned rgb = 0;
            auto [ptr, ec] = std::from_chars(v.str.data(), v.str.data() + v.str.size(), rgb, 16);
            if (ec != std::errc() || v.str.size() != 6) return fail("clear_color must be 'rrggbb'");
            p.clear_color = {static_cast<float>((rgb >> 16) & 255) / 255.0f, static_cast<float>((rgb >> 8) & 255) / 255.0f,
                             static_cast<float>(rgb & 255) / 255.0f, 1.0f};
            (void)ptr;
        } else if (key == "render_tier" && v.kind == TomlValue::Kind::String) {
            if (!rhi::parse_tier(v.str, p.render_tier)) {
                return fail(std::format("render_tier must be \"compatible\" or \"full\", not \"{}\"", v.str));
            }
            p.render_tier_line = number;
        } else if (key == "tier") {
            return fail("unknown setting 'tier' (the render tier is 'render_tier = \"compatible\"' or \"full\")");
        } else {
            return fail(std::format("unknown or invalid setting '{}'", key));
        }
    }
    if (!check_dependencies(p.dependencies, file, error)) return std::nullopt;
    return p;
}

std::optional<PackageManifest> load_package_manifest(const assets::Vfs& vfs, std::string_view prefix,
                                                     std::string& error) {
    const std::string path = std::string(prefix) + "package.toml";
    std::string text;
    if (!vfs.read_text(path, text, error)) {
        error = std::format("cannot read '{}'", vfs.describe(path));
        return std::nullopt;
    }
    const std::string file = vfs.describe(path);
    std::istringstream in(text);
    PackageManifest m;
    std::string section;
    std::string line;
    int number = 0;
    auto fail = [&](std::string message) {
        error = std::format("{}:{}: {}", file, number, message);
        return std::nullopt;
    };
    while (std::getline(in, line)) {
        ++number;
        std::string_view s = trim(strip_comment(line));
        if (s.empty()) continue;
        if (s.front() == '[') {
            if (s.back() != ']') return fail("expected ']'");
            section = std::string(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        std::size_t eq = s.find('=');
        if (eq == std::string_view::npos) return fail("expected 'key = value'");
        std::string key(trim(s.substr(0, eq)));
        TomlValue v;
        if (!parse_value(s.substr(eq + 1), v)) return fail(std::format("invalid value for '{}'", key));
        bool handled = false;
        std::string message;
        if (!dependency_entry(section, key, v, number, m.dependencies, handled, message)) return fail(message);
        if (handled) continue;
        if (section == "plugin") {
            m.plugin.present = true;
            if (key == "abi") {
                std::string why;
                std::optional<VersionRange> range;
                if (v.kind == TomlValue::Kind::Number && v.num >= 1) {
                    range = parse_version_range(std::format("{}", static_cast<int>(v.num)), why);
                } else if (v.kind == TomlValue::Kind::String) {
                    range = parse_version_range(v.str, why);
                }
                if (!range) return fail("'abi' must be a positive integer or a range of them (\">=1, <3\")");
                m.plugin.abi = *range;
                m.plugin.has_abi = true;
            } else if (key == "platforms") {
                if (v.kind != TomlValue::Kind::Array) return fail("'platforms' must be a list, e.g. [\"linux\"]");
                for (const TomlValue& item : v.items) {
                    if (item.kind != TomlValue::Kind::String || (item.str != "linux" && item.str != "windows")) {
                        return fail("'platforms' lists \"linux\" and \"windows\" (the web has no plugins yet)");
                    }
                    m.plugin.platforms.push_back(item.str);
                }
            } else if (key == "sim_safe") {
                if (v.kind != TomlValue::Kind::Bool) return fail("'sim_safe' must be true or false");
                m.plugin.sim_safe = v.b;
            } else if (v.kind != TomlValue::Kind::String) {
                return fail(std::format("'{}' in [plugin] must be a string", key));
            } else if (key == "linux") {
                m.plugin.lib_linux = v.str;
            } else if (key == "linux_static") {
                m.plugin.static_linux = v.str;
            } else if (key == "windows") {
                m.plugin.lib_windows = v.str;
            } else if (key == "windows_static") {
                m.plugin.static_windows = v.str;
            } else if (key == "windows_gnu_static") {
                m.plugin.static_windows_gnu = v.str;
            } else if (key == "dotnet") {
                m.plugin.dotnet = v.str;
            } else if (key == "dotnet_type") {
                m.plugin.dotnet_type = v.str;
            } else {
                return fail(std::format("unknown key '{}' in [plugin] (abi, sim_safe, platforms, linux, linux_static, "
                                        "windows, windows_static, windows_gnu_static, dotnet, dotnet_type)",
                                        key));
            }
            continue;
        }
        if (!section.empty()) continue; // other sections are for later tools
        if (v.kind != TomlValue::Kind::String) return fail(std::format("'{}' must be a string", key));
        if (key == "name") {
            if (!valid_package_name(v.str)) return fail(std::format("'{}' is not a valid package name", v.str));
            m.name = v.str;
        } else if (key == "version") {
            if (!valid_version(v.str)) return fail(std::format("version \"{}\" must be MAJOR.MINOR.PATCH", v.str));
            m.version = v.str;
        } else if (key == "description") {
            m.description = v.str;
        } else if (key == "license") {
            m.license = v.str;
        } else if (key == "engine") {
            std::string why;
            auto range = parse_version_range(v.str, why);
            if (!range) return fail(why);
            m.engine = *range;
        } else {
            return fail(std::format("unknown setting '{}'", key));
        }
    }
    number = 0;
    if (m.name.empty()) return fail("missing 'name'");
    if (m.version.empty()) return fail("missing 'version'");
    if (m.plugin.present && !m.plugin.has_abi) return fail("[plugin] needs 'abi' (the plugin ABI version it was built for)");
    if (m.plugin.present && m.plugin.platforms.empty()) {
        if (!m.plugin.lib_linux.empty() || !m.plugin.dotnet.empty()) m.plugin.platforms.push_back("linux");
        if (!m.plugin.lib_windows.empty() || !m.plugin.dotnet.empty()) m.plugin.platforms.push_back("windows");
    }
    if (!check_dependencies(m.dependencies, file, error)) return std::nullopt;
    return m;
}

namespace {

struct Resolver {
    const assets::Vfs& root;
    std::vector<ResolvedPackage>& out;
    std::string& error;
    // name -> (version, who asked first) and the DFS stack, for conflicts
    // and cycles.
    std::vector<std::pair<std::string, std::string>> seen; // name, version
    std::vector<std::string> stack;

    // `from` names the requirer in errors; `base` is its directory on disk.
    bool visit(const Dependency& dep, const std::string& from, const fs::path& base) {
        for (const auto& [name, version] : seen) {
            if (name != dep.name) continue;
            if (version != dep.version) {
                error = std::format("{} needs {} {}, but {} {} is already required (one version per package)", from,
                                    dep.name, dep.version, dep.name, version);
                return false;
            }
            if (std::find(stack.begin(), stack.end(), dep.name) != stack.end()) {
                error = std::format("dependency cycle: {} -> {}", from, dep.name);
                return false;
            }
            return true;
        }
        seen.emplace_back(dep.name, dep.version);
        ResolvedPackage pkg;
        if (root.packed() || dep.path.empty()) {
            // Vendored, or any package inside a pack (export bundles them there).
            pkg.vfs = std::shared_ptr<const assets::Vfs>(std::shared_ptr<const assets::Vfs>{}, &root);
            pkg.prefix = std::format("packages/{}/", dep.name);
            if (!root.packed()) pkg.dir = root.root_dir() / "packages" / dep.name;
            if (!root.exists(pkg.prefix + "package.toml")) {
                error = std::format("{}: package '{}' not found (expected {}package.toml{})", from, dep.name,
                                    pkg.prefix, root.packed() ? " in the pack" : ", or give it a 'path'");
                return false;
            }
        } else {
            fs::path dir = platform::path_from_utf8(dep.path);
            if (dir.is_relative()) dir = base / dir;
            pkg.dir = dir.lexically_normal();
            std::error_code ec;
            if (!fs::exists(pkg.dir / "package.toml", ec)) {
                error = std::format("{}: package '{}' not found at '{}'", from, dep.name, platform::utf8(pkg.dir));
                return false;
            }
            pkg.vfs = assets::Vfs::dev(pkg.dir);
        }
        auto manifest = load_package_manifest(*pkg.vfs, pkg.prefix, error);
        if (!manifest) return false;
        if (manifest->name != dep.name) {
            error = std::format("{}: dependency '{}' points at package '{}'", from, dep.name, manifest->name);
            return false;
        }
        if (!manifest->engine.contains(*parse_version(engine_version(), true))) {
            error = std::format("{} {} needs engine {}; this is engine {}", dep.name, manifest->version,
                                manifest->engine.text, engine_version());
            return false;
        }
        if (manifest->version != dep.version) {
            error = std::format("{} pins {} {}, but the package is version {}", from, dep.name, dep.version,
                                manifest->version);
            return false;
        }
        pkg.manifest = std::move(*manifest);
        stack.push_back(dep.name);
        const std::string me = std::format("package {} {}", pkg.manifest.name, pkg.manifest.version);
        for (const Dependency& d : pkg.manifest.dependencies) {
            if (!visit(d, me, pkg.dir)) return false;
        }
        stack.pop_back();
        out.push_back(std::move(pkg));
        return true;
    }
};

} // namespace

bool resolve_packages(const Project& project, const assets::Vfs& vfs, std::vector<ResolvedPackage>& out,
                      std::string& error) {
    out.clear();
    Resolver r{vfs, out, error, {}, {}};
    for (const Dependency& d : project.dependencies) {
        if (!r.visit(d, "project.toml", vfs.packed() ? fs::path() : vfs.root_dir())) return false;
    }
    return true;
}

bool apply_input(const Project& project, InputMap& map, std::string& error) {
    for (const auto& [action, sources] : project.actions) {
        std::vector<Binding> bindings;
        for (const std::string& s : sources) {
            auto b = parse_binding(s);
            if (!b) {
                error = std::format("action '{}': unknown input '{}'", action, s);
                return false;
            }
            bindings.push_back(*b);
        }
        if (!map.set_action(Name(action), std::span<const Binding>(bindings))) {
            error = "too many input actions (64 at most)";
            return false;
        }
    }
    for (const auto& [axis, sources] : project.axes) {
        std::vector<AxisSource> xs;
        std::vector<AxisSource> ys;
        for (const std::string& s : sources) {
            AxisSource x;
            AxisSource y;
            if (!parse_axis2(s, x, y)) {
                error = std::format("axis '{}': unknown input '{}' (Keys.Arrows, Keys.WASD, Pad.LeftStick, "
                                    "Pad.RightStick, Pad.Dpad)",
                                    axis, s);
                return false;
            }
            xs.push_back(x);
            ys.push_back(y);
        }
        if (!map.set_axis(Name(axis + ".x"), std::span<const AxisSource>(xs)) ||
            !map.set_axis(Name(axis + ".y"), std::span<const AxisSource>(ys))) {
            error = "too many input axes (4 two-dimensional axes at most)";
            return false;
        }
    }
    return true;
}

bool read_scripts(const Project& project, std::vector<pluma::SourceFile::Part>& out, std::vector<fs::path>* files,
                  std::string& error) {
    return read_scripts(*assets::Vfs::dev(project.dir), out, files, error);
}

bool read_scripts(const assets::Vfs& vfs, std::vector<pluma::SourceFile::Part>& out, std::vector<fs::path>* files,
                  std::string& error) {
    // Unit names are the project-relative paths ("scripts/game.plm") in dev
    // and release alike, so diagnostics and closure keys match.
    out.clear();
    std::vector<fs::path> real;
    // Packages first (LANGUAGE.md 9.26). A VFS without a project.toml (tests,
    // a bare directory of scripts) has none.
    if (vfs.exists("project.toml")) {
        auto project = load_project(vfs, error);
        if (!project) return false;
        std::vector<ResolvedPackage> packages;
        if (!resolve_packages(*project, vfs, packages, error)) return false;
        for (const ResolvedPackage& pkg : packages) {
            const std::string unit_prefix = std::format("packages/{}/", pkg.manifest.name);
            for (const std::string& path : pkg.vfs->list(pkg.prefix + "scripts", ".plm")) {
                std::string text;
                if (!pkg.vfs->read_text(path, text, error)) return false;
                out.push_back({unit_prefix + path.substr(pkg.prefix.size()), std::move(text), pkg.manifest.name});
                if (auto r = pkg.vfs->real_path(path)) real.push_back(std::move(*r));
            }
        }
    }
    const std::vector<std::string> paths = vfs.list("scripts", ".plm");
    for (const std::string& path : paths) {
        std::string text;
        if (!vfs.read_text(path, text, error)) return false;
        out.push_back({path, std::move(text)});
        if (auto r = vfs.real_path(path)) real.push_back(std::move(*r));
    }
    if (files != nullptr) *files = std::move(real);
    // No scripts is fine: a project whose `main_scene` uses only prelude
    // nodes has no code (examples/node_level, milestone 8.2). Without a
    // main_scene, the missing main class is reported when the game loads.
    return true;
}

} // namespace cuervo::script

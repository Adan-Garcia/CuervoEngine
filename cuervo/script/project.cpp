#include "cuervo/script/project.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/name.hpp"
#include "cuervo/platform/platform.hpp"

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

} // namespace

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
    return p;
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
    const std::vector<std::string> paths = vfs.list("scripts", ".plm");
    out.clear();
    std::vector<fs::path> real;
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

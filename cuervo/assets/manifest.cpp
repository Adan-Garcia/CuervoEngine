#include "cuervo/assets/manifest.hpp"

#include <charconv>
#include <format>

namespace cuervo::assets {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// TOML basic-string escaping for the few characters a game name may hold.
std::string toml_string(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

bool unquote(std::string_view v, std::string& out) {
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return false;
    out.clear();
    for (std::size_t i = 1; i + 1 < v.size(); ++i) {
        if (v[i] == '\\' && i + 2 < v.size()) {
            ++i;
            out += v[i] == 'n' ? '\n' : v[i];
            continue;
        }
        out += v[i];
    }
    return true;
}

bool parse_string_array(std::string_view v, std::vector<std::string>& out) {
    v = trim(v);
    if (v.size() < 2 || v.front() != '[' || v.back() != ']') return false;
    v = trim(v.substr(1, v.size() - 2));
    out.clear();
    while (!v.empty()) {
        if (v.front() == '"') {
            std::size_t end = v.find('"', 1);
            if (end == std::string_view::npos) return false;
            std::string item;
            if (!unquote(v.substr(0, end + 1), item)) return false;
            out.push_back(std::move(item));
            v = trim(v.substr(end + 1));
            if (!v.empty() && v.front() == ',') v = trim(v.substr(1));
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view script_form_name(ScriptForm f) {
    switch (f) {
        case ScriptForm::Source:   return "source";
        case ScriptForm::Bytecode: return "bytecode";
        case ScriptForm::Native:   return "native";
    }
    return "unknown";
}

std::optional<ScriptForm> parse_script_form(std::string_view text) {
    for (ScriptForm f : {ScriptForm::Source, ScriptForm::Bytecode, ScriptForm::Native}) {
        if (script_form_name(f) == text) return f;
    }
    return std::nullopt;
}

std::string write_manifest(const ExportManifest& m) {
    std::string out = "# Written by cuervo-tools pack (docs/EXPORT.md). Do not edit.\n";
    out += std::format("format = {}\n", m.format);
    out += std::format("game = {}\n", toml_string(m.game));
    out += std::format("engine = {}\n", toml_string(m.engine));
    out += std::format("platform = {}\n", toml_string(platform_name(m.platform)));
    out += std::format("scripts = {}\n", toml_string(script_form_name(m.scripts)));
    out += std::format("stripped = {}\n", m.stripped ? "true" : "false");
    if (!m.features.empty()) {
        out += "features = [";
        for (std::size_t i = 0; i < m.features.size(); ++i) {
            if (i > 0) out += ", ";
            out += toml_string(m.features[i]);
        }
        out += "]\n";
    }
    if (!m.stripped_features.empty()) {
        out += "stripped_features = [";
        for (std::size_t i = 0; i < m.stripped_features.size(); ++i) {
            if (i > 0) out += ", ";
            out += toml_string(m.stripped_features[i]);
        }
        out += "]\n";
    }
    return out;
}

std::optional<ExportManifest> parse_manifest(std::string_view text, std::string& error) {
    ExportManifest m;
    m.format = 0;
    int number = 0;
    for (std::size_t pos = 0; pos < text.size();) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++number;
        if (line.empty() || line.front() == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            error = std::format("{}:{}: expected 'key = value'", manifest_path, number);
            return std::nullopt;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        std::string str;
        auto fail = [&](std::string_view what) {
            error = std::format("{}:{}: invalid {}", manifest_path, number, what);
            return std::nullopt;
        };
        if (key == "format") {
            auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), m.format);
            if (ec != std::errc() || p != value.data() + value.size()) return fail("format");
        } else if (key == "game" || key == "engine") {
            if (!unquote(value, str)) return fail(key);
            (key == "game" ? m.game : m.engine) = str;
        } else if (key == "platform") {
            auto p = unquote(value, str) ? parse_platform(str) : std::nullopt;
            if (!p) return fail("platform");
            m.platform = *p;
        } else if (key == "scripts") {
            auto f = unquote(value, str) ? parse_script_form(str) : std::nullopt;
            if (!f) return fail("scripts form");
            m.scripts = *f;
        } else if (key == "stripped") {
            if (value != "true" && value != "false") return fail("stripped");
            m.stripped = value == "true";
        } else if (key == "features") {
            if (!parse_string_array(value, m.features)) return fail("features");
        } else if (key == "stripped_features") {
            if (!parse_string_array(value, m.stripped_features)) return fail("stripped_features");
        }
    }
    if (m.format <= 0) {
        error = std::format("{}: no format", manifest_path);
        return std::nullopt;
    }
    if (m.format > manifest_format) {
        error = std::format("{}: format {} is newer than this runtime reads ({})", manifest_path, m.format,
                            manifest_format);
        return std::nullopt;
    }
    return m;
}

} // namespace cuervo::assets

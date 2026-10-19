#include "cuervo/script/project_edit.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

#include "cuervo/platform/platform.hpp"

namespace cuervo::script {

namespace fs = std::filesystem;

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Where a `#` comment starts outside strings, else npos.
std::size_t comment_start(std::string_view s) {
    bool in_str = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') in_str = !in_str;
        if (s[i] == '#' && !in_str) return i;
    }
    return std::string_view::npos;
}

struct Line {
    std::string_view text; // without the line ending
    std::string_view eol;  // "\n", "\r\n" or "" (last line)
};

std::vector<Line> split_lines(std::string_view toml) {
    std::vector<Line> out;
    std::size_t pos = 0;
    while (pos < toml.size()) {
        std::size_t nl = toml.find('\n', pos);
        if (nl == std::string_view::npos) {
            out.push_back({toml.substr(pos), {}});
            break;
        }
        std::size_t end = nl;
        if (end > pos && toml[end - 1] == '\r') --end;
        out.push_back({toml.substr(pos, end - pos), toml.substr(end, nl + 1 - end)});
        pos = nl + 1;
    }
    return out;
}

// A `[section]` header's name.
std::optional<std::string> header(std::string_view line) {
    std::string_view s = line;
    if (std::size_t c = comment_start(s); c != std::string_view::npos) s = s.substr(0, c);
    s = trim(s);
    if (s.size() < 2 || s.front() != '[' || s.back() != ']') return std::nullopt;
    return std::string(trim(s.substr(1, s.size() - 2)));
}

struct KeyLine {
    std::string key;
    std::vector<std::string> strings; // the array's string items
    std::string_view comment;         // from the whitespace before '#' to the end, or empty
};

std::optional<KeyLine> key_line(std::string_view line) {
    std::string_view body = line;
    KeyLine out;
    if (std::size_t c = comment_start(line); c != std::string_view::npos) {
        std::size_t start = c;
        while (start > 0 && (line[start - 1] == ' ' || line[start - 1] == '\t')) --start;
        out.comment = line.substr(start);
        body = line.substr(0, c);
    }
    body = trim(body);
    const std::size_t eq = body.find('=');
    if (body.empty() || eq == std::string_view::npos) return std::nullopt;
    out.key = std::string(trim(body.substr(0, eq)));
    std::string_view value = body.substr(eq + 1);
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '"') continue;
        const std::size_t close = value.find('"', i + 1);
        if (close == std::string_view::npos) break;
        out.strings.emplace_back(value.substr(i + 1, close - i - 1));
        i = close;
    }
    return out;
}

std::string format_entry(const InputEntry& e) {
    std::string s = e.name + " = [";
    for (std::size_t i = 0; i < e.bindings.size(); ++i) {
        if (i > 0) s += ", ";
        s += "\"" + e.bindings[i] + "\"";
    }
    return s + "]";
}

} // namespace

bool is_input_name(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

InputSections read_input_sections(std::string_view toml) {
    InputSections out;
    std::string section;
    for (const Line& l : split_lines(toml)) {
        if (auto h = header(l.text)) {
            section = *h;
            continue;
        }
        if (section != "input.actions" && section != "input.axes") continue;
        auto k = key_line(l.text);
        if (!k) continue;
        (section == "input.actions" ? out.actions : out.axes).push_back({k->key, k->strings, k->key});
    }
    return out;
}

std::string rewrite_input_sections(std::string_view toml, const InputSections& input) {
    const std::vector<Line> lines = split_lines(toml);
    std::string eol = "\n";
    for (const Line& l : lines) {
        if (!l.eol.empty()) {
            eol = std::string(l.eol);
            break;
        }
    }
    struct Section {
        const char* name;
        const std::vector<InputEntry>* entries;
        std::vector<bool> written;
        std::optional<std::size_t> insert_at; // index into `out` after the section's last entry
    };
    Section sections[2] = {{"input.actions", &input.actions, std::vector<bool>(input.actions.size()), std::nullopt},
                           {"input.axes", &input.axes, std::vector<bool>(input.axes.size()), std::nullopt}};
    // Lines of the result (text + ending), so new entries can be inserted.
    std::vector<std::string> out;
    Section* current = nullptr;
    for (const Line& l : lines) {
        if (auto h = header(l.text)) {
            current = nullptr;
            for (Section& s : sections) {
                if (*h == s.name) current = &s;
            }
            out.push_back(std::string(l.text) + std::string(l.eol));
            if (current != nullptr) current->insert_at = out.size();
            continue;
        }
        std::optional<KeyLine> k = current != nullptr ? key_line(l.text) : std::nullopt;
        if (!k) {
            out.push_back(std::string(l.text) + std::string(l.eol));
            continue;
        }
        const std::vector<InputEntry>& entries = *current->entries;
        std::size_t found = entries.size();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!current->written[i] && !entries[i].original.empty() && entries[i].original == k->key) {
                found = i;
                break;
            }
        }
        if (found == entries.size()) continue; // removed: the line goes
        current->written[found] = true;
        const InputEntry& e = entries[found];
        const std::string_view line_eol = l.eol.empty() ? std::string_view(eol) : l.eol;
        if (e.name == k->key && e.bindings == k->strings) {
            out.push_back(std::string(l.text) + std::string(l.eol)); // unchanged: verbatim
        } else {
            out.push_back(format_entry(e) + std::string(k->comment) + std::string(line_eol));
        }
        current->insert_at = out.size();
    }
    // New entries after their section's last entry; missing sections at the end.
    for (int si = 1; si >= 0; --si) {
        Section& s = sections[si];
        if (!s.insert_at) continue;
        std::vector<std::string> added;
        for (std::size_t i = 0; i < s.entries->size(); ++i) {
            if (!s.written[i]) added.push_back(format_entry((*s.entries)[i]) + eol);
        }
        // An entry inserted after a last line without a newline needs one.
        if (!added.empty() && *s.insert_at > 0 && !out[*s.insert_at - 1].ends_with('\n')) out[*s.insert_at - 1] += eol;
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(*s.insert_at), added.begin(), added.end());
    }
    for (Section& s : sections) {
        if (s.insert_at || s.entries->empty()) continue;
        if (!out.empty() && !out.back().ends_with('\n')) out.back() += eol;
        if (!out.empty() && out.back().find_first_not_of(" \t\r\n") != std::string::npos) out.push_back(eol);
        out.push_back(std::string("[") + s.name + "]" + eol);
        for (const InputEntry& e : *s.entries) out.push_back(format_entry(e) + eol);
    }
    std::string result;
    for (const std::string& l : out) result += l;
    return result;
}

bool write_project_input(const fs::path& dir, const InputSections& input, std::string& error) {
    const fs::path file = dir / "project.toml";
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            error = std::format("cannot read '{}'", platform::utf8(file));
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string updated = rewrite_input_sections(text, input);
    if (updated == text) return true;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << updated;
    if (!out) {
        error = std::format("cannot write '{}'", platform::utf8(file));
        return false;
    }
    return true;
}

} // namespace cuervo::script

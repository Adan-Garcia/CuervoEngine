#pragma once

// The flag table type shared by `cuervo` (runtime/cli_flags.hpp) and `cuervo-tools`
// (tools/cuervo_tools/cli_flags.hpp). One table per program drives its `--help`
// text, docs/CLI.md (`cuervo-tools cli-docs`) and a test that every flag the
// parsers handle is listed (tests/cuervo/cli_table_tests.cpp), so the three
// can't drift apart. Header-only and dependency-free on purpose.

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cuervo::cli {

struct Flag {
    std::string_view name;    // "--press", "-o" style long name, or an environment variable (CUERVO_PRESS)
    std::string_view alias;   // short form ("-o") or empty
    std::string_view arg;     // "<file>" or "on|off"; empty for a switch
    std::string_view help;
    std::string_view section; // group heading (a mode for `cuervo`, a command for `cuervo-tools`)
};

inline const Flag* find(std::span<const Flag> table, std::string_view name) {
    for (const Flag& f : table) {
        if (f.name == name || (!f.alias.empty() && f.alias == name)) return &f;
    }
    return nullptr;
}

// "--press <ticks:actions>" or "-o, --out <dir>".
inline std::string signature(const Flag& f) {
    std::string s;
    if (!f.alias.empty()) s += std::string(f.alias) + ", ";
    s += f.name;
    if (!f.arg.empty()) s += " " + std::string(f.arg);
    return s;
}

// Sections in first-use order.
inline std::vector<std::string_view> sections(std::span<const Flag> table) {
    std::vector<std::string_view> out;
    for (const Flag& f : table) {
        if (std::find(out.begin(), out.end(), f.section) == out.end()) out.push_back(f.section);
    }
    return out;
}

// The flags of a table as `--help` text: a heading per section, the signature
// padded to a column, then the help.
inline std::string help_text(std::span<const Flag> table, std::string_view only_section = {}) {
    std::size_t width = 0;
    for (const Flag& f : table) width = std::max(width, signature(f).size());
    std::string out;
    for (std::string_view section : sections(table)) {
        if (!only_section.empty() && section != only_section) continue;
        out += "\n" + std::string(section) + ":\n";
        for (const Flag& f : table) {
            if (f.section != section) continue;
            std::string sig = signature(f);
            out += "  " + sig + std::string(width - sig.size() + 2, ' ') + std::string(f.help) + "\n";
        }
    }
    return out;
}

// The same table as Markdown for docs/CLI.md.
inline std::string markdown(std::span<const Flag> table, std::string_view heading_prefix) {
    std::string out;
    for (std::string_view section : sections(table)) {
        out += std::string(heading_prefix) + " " + std::string(section) + "\n\n| Flag | Description |\n| --- | --- |\n";
        for (const Flag& f : table) {
            if (f.section != section) continue;
            std::string help(f.help);
            for (std::size_t pos = 0; (pos = help.find('<', pos)) != std::string::npos; pos += 4) help.replace(pos, 1, "&lt;");
            for (std::size_t pos = 0; (pos = help.find('>', pos)) != std::string::npos; pos += 4) help.replace(pos, 1, "&gt;");
            for (std::size_t pos = 0; (pos = help.find('|', pos)) != std::string::npos; pos += 2) help.insert(pos, "\\");
            std::string sig = signature(f);
            for (std::size_t pos = 0; (pos = sig.find('|', pos)) != std::string::npos; pos += 2) sig.insert(pos, "\\");
            out += "| `" + sig + "` | " + help + " |\n";
        }
        out += "\n";
    }
    return out;
}

} // namespace cuervo::cli

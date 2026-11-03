#pragma once

// Every command and flag of `cuervo-tools`: the table behind `cuervo-tools --help`,
// docs/CLI.md (`cuervo-tools cli-docs`) and tests/cuervo/cli_table_tests.cpp, which
// checks that every flag main.cpp's parsers handle is listed here. Add a flag to
// a parser and to this table together. The flag sections are the command names.

#include <span>
#include <string>
#include <string_view>

#include "runtime/cli_table.hpp"

namespace cuervo::tools {

struct Command {
    std::string_view name;
    std::string_view usage; // the line after "cuervo-tools"
    std::string_view help;
};

inline constexpr Command command_list[] = {
    {"scene", "scene dump <file.scene> | build <file.txt> <file.scene> | load-text <file.txt> <file.scene> | migrate <dir> --text|--binary [--dry-run] | upgrade <file.scene> [<out.scene>]",
     "Dump a scene file as text, build binary from text, normalize text, convert a folder, or upgrade a scene."},
    {"tier", "tier check <project> [--to compatible|full] [--platform linux|windows|web] | set <project> compatible|full [--force]",
     "Report whether a project fits a render tier on a platform, or change its tier."},
    {"shader", "shader build <project|file.glsl> [-o <out.cshader>]",
     "Precompile user shaders (.glsl) for the game: SPIR-V, GLSL ES and WGSL (docs/DESIGN.md 4.19)."},
    {"pack", "pack <project> -o <game.pak> [options]", "Pack a project into a game.pak."},
    {"export", "export <project> --platform linux|web|windows -o <dir> [options]",
     "Export a runnable game directory (docs/EXPORT.md)."},
    {"pack-info", "pack-info <game.pak>", "List the entries of a pack."},
    {"fmt", "fmt <file-or-project> [--check] [--write|-w]", "Format Pluma scripts."},
    {"lsp", "lsp [<project>] [--check-style|--no-check-style]", "Run the Pluma language server on stdio."},
    {"docs", "docs [-o <file.md>] [--check <file.md>] [--check-coverage]", "Generate docs/API.md from the engine prelude."},
    {"cli-docs", "cli-docs [-o <file.md>] [--check <file.md>]", "Generate docs/CLI.md from the flag tables."},
    {"package", "package new <dir> --name <name> [--version <x.y.z>] | check <dir>", "Create or check a script package."},
    {"test", "test <project-or-package> [--filter <substring>]", "Run the Pluma tests of a project or package."},
    {"plugin", "plugin verify <dir> | publish <dir> | install <name>[@<range>] | list [options]",
     "Verify, publish, install and list plugins (--registry defaults to $CUERVO_REGISTRY)."},
    {"license", "license report <project>", "List the licenses of the packages a project uses."},
};

inline constexpr cli::Flag tools_flag_list[] = {
    {"--text", "", "", "scene migrate: convert the scenes to text.", "scene"},
    {"--binary", "", "", "scene migrate: convert the scenes to binary.", "scene"},
    {"--dry-run", "", "", "scene migrate: list what would change without writing.", "scene"},
    {"--to", "", "compatible|full", "Check against this tier instead of the project's own.", "tier"},
    {"--platform", "", "linux|windows|web", "Check against this platform.", "tier"},
    {"--force", "", "", "tier set: change the tier even if the project does not fit it.", "tier"},

    {"--out", "-o", "<game.pak>", "The pack to write (required).", "pack"},
    {"--platform", "", "linux|web|windows", "Target platform of the pack.", "pack"},
    {"--scripts", "", "source|native", "Ship scripts as source or as native code (see --aot).", "pack"},
    {"--aot", "", "", "Compile the scripts to native code (same as --scripts native).", "pack"},
    {"--strip", "", "", "Leave out what the project does not use.", "pack"},
    {"--font", "", "<file>", "Font to pack (default: the engine's).", "pack"},

    {"--out", "-o", "<dir>", "The directory to export into (required).", "export"},
    {"--platform", "", "linux|web|windows", "Target platform (required).", "export"},
    {"--scripts", "", "source|native", "Ship scripts as source or as native code.", "export"},
    {"--aot", "", "", "Compile the scripts to native code.", "export"},
    {"--strip", "", "", "Leave out what the project does not use.", "export"},
    {"--font", "", "<file>", "Font to pack (default: the engine's).", "export"},
    {"--runtime", "", "<path>", "The player executable to ship.", "export"},
    {"--name", "", "<launcher>", "Name of the launcher.", "export"},
    {"--engine", "", "auto|full|stripped", "Which engine build to ship (stripped players are built on demand).", "export"},
    {"--cmake", "", "<path>", "cmake used to build a stripped player.", "export"},
    {"--player-cache", "", "<dir>", "Where stripped players are cached.", "export"},
    {"--no-verify", "", "", "Skip running the exported game once to compare its state hash.", "export"},
    {"--emsdk", "", "<dir>", "Emscripten SDK for web stripped players.", "export"},
    {"--host-build", "", "<dir>", "Host build directory for web stripped players.", "export"},

    {"--check", "", "", "Report files that need formatting; write nothing (exit 1 if any).", "fmt"},
    {"--write", "-w", "", "Rewrite the files in place.", "fmt"},

    {"--check-style", "", "", "Report style problems as diagnostics (default).", "lsp"},
    {"--no-check-style", "", "", "Do not report style problems.", "lsp"},
    {"--stdio", "", "", "Accepted for editors that pass it; stdio is the only transport.", "lsp"},
    {"--clientProcessId", "", "<pid>", "Accepted for editors that pass it (--clientProcessId=<pid>); ignored.", "lsp"},

    {"--out", "-o", "<file.md>", "Write the API reference to a file instead of stdout.", "docs"},
    {"--check", "", "<file.md>", "Fail if the file differs from the generated reference.", "docs"},
    {"--check-coverage", "", "", "Fail if a public prelude symbol has no doc comment.", "docs"},

    {"--out", "-o", "<file.md>", "Write docs/CLI.md to a file instead of stdout.", "cli-docs"},
    {"--check", "", "<file.md>", "Fail if the file differs from the generated reference.", "cli-docs"},

    {"--name", "", "<name>", "Package name (required).", "package"},
    {"--version", "", "<x.y.z>", "Package version (default 0.1.0).", "package"},

    {"--filter", "", "<substring>", "Only run tests whose name contains the text.", "test"},

    {"--registry", "", "<dir>", "The plugin registry (default $CUERVO_REGISTRY).", "plugin"},
    {"--project", "", "<dir>", "plugin install: the project to add the plugin to.", "plugin"},

    {"--help", "-h", "", "Print this reference and exit.", "General"},
};

inline std::span<const Command> command_table() { return command_list; }
inline std::span<const cli::Flag> tools_table() { return tools_flag_list; }

inline constexpr std::string_view tools_exit_codes_text =
    "0 ok, 1 the command failed or found a problem (a stale file, an unformatted script), 2 wrong usage";

// `cuervo-tools --help`.
inline std::string tools_help() {
    std::string out = "cuervo-tools: command-line tools for projects\n\nusage:\n";
    for (const Command& c : command_table()) out += "  cuervo-tools " + std::string(c.usage) + "\n";
    out += "\nCommands:\n";
    for (const Command& c : command_table()) {
        out += "  " + std::string(c.name) + std::string(c.name.size() < 10 ? 10 - c.name.size() : 1, ' ') + std::string(c.help) + "\n";
    }
    out += "\nFlags per command:\n";
    out += cli::help_text(tools_table());
    out += "\nExit codes: " + std::string(tools_exit_codes_text) + ".\n";
    out += "docs/CLI.md has the same reference.\n";
    return out;
}

} // namespace cuervo::tools

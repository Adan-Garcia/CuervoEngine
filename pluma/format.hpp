#pragma once

// Source code formatter and style checker for Pluma (LANGUAGE.md 6, DESIGN.md 5.14).
//
// Unlike `pluma::print` (printer.hpp), which prints a bare AST without trivia,
// `format_source` works on the original `SourceFile` + AST to preserve `//` and
// `/* ... */` comments, intentional blank lines, intentional same-line `;`
// statements, multiline method chains, and multiline lists/arguments/parameters
// with trailing commas, while normalizing indentation (4 spaces), brace
// placement (`} else {`), and operator/`?` spacing.

#include <string>
#include <string_view>
#include <vector>

#include "pluma/ast.hpp"
#include "pluma/diagnostic.hpp"
#include "pluma/source.hpp"

namespace pluma {

struct FormatOptions {
    // Parse as a script (statements + declarations at top level) instead of a
    // module (declarations only).
    bool script_mode = false;
    // Run the naming-convention checker (LANGUAGE.md 6) and include its
    // warnings in `FormatResult::diagnostics`.
    bool check_style = true;
};

struct FormatResult {
    // True when the input parsed without unrecoverable syntax errors.
    bool ok = false;
    // Canonical formatted source text (equals the original text on failure).
    std::string formatted;
    // Syntax errors (when !ok) and style warnings (when check_style is true).
    std::vector<Diagnostic> diagnostics;
    // Clang-style rendered diagnostics.
    std::string rendered;
    // True when `formatted != source.text()`.
    bool changed = false;
};

FormatResult format_source(const SourceFile& source, FormatOptions options = {});
FormatResult format_source(std::string_view path, std::string_view text, FormatOptions options = {});

// Checks the naming rules of LANGUAGE.md 6 on a parsed AST and reports
// `Severity::Warning` diagnostics into `diags`:
//   - PascalCase for classes, structs, enums, traits, generic params, variants
//   - snake_case for functions, methods, signals, fields, locals, parameters
//   - SCREAMING_SNAKE for `const` (also allowed on top-level/static `var`)
//   - #snake_case for interned `#name` literals
void check_style(const SourceFile& source, const Module& module, DiagnosticSink& diags);
void check_style(const SourceFile& source, const Script& script, DiagnosticSink& diags);

} // namespace pluma

#pragma once

// Warnings that need only the syntax tree (the language server adds the ones
// that need name resolution). Each warning carries a stable code string.
// Decision: warnings never change what compiles; they are editor and
// `plm check` hints, and valid code in examples/ must produce none.

#include "pluma/ast.hpp"
#include "pluma/diagnostic.hpp"

namespace pluma {

// Warning codes (Diagnostic::code).
inline constexpr const char* warn_unreachable_code = "unreachable-code";
inline constexpr const char* warn_unused_variable = "unused-variable";
inline constexpr const char* warn_unused_import = "unused-import";

// "unreachable-code": the first statement after a `return`, `break` or
// `continue` in the same block. Lambda bodies are not inspected.
void lint_unreachable(const Module& module, DiagnosticSink& diags);

} // namespace pluma

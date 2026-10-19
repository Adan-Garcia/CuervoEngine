#pragma once

#include "pluma/arena.hpp"
#include "pluma/ast.hpp"
#include "pluma/diagnostic.hpp"
#include "pluma/source.hpp"

namespace pluma {

// Recursive-descent parser with a precedence-climbing (Pratt) expression core,
// implementing docs/LANGUAGE.md. It never stops at the first error: after a
// syntax error it resynchronizes at the next statement or declaration boundary
// and keeps going, so one run reports every independent error in a file.
//
// All nodes are allocated in `arena`; names point into `source`. Both must
// outlive the returned tree. Diagnostics are sorted by position.

// A whole file: declarations only (LANGUAGE.md 7, `file = { decl }`).
Module parse_module(const SourceFile& source, Arena& arena, DiagnosticSink& diagnostics);

// REPL/script input: declarations and statements mixed at top level.
Script parse_script(const SourceFile& source, Arena& arena, DiagnosticSink& diagnostics);

// A single expression (tests and tools). Trailing tokens are an error.
Expr* parse_expression(const SourceFile& source, Arena& arena, DiagnosticSink& diagnostics);

} // namespace pluma

#pragma once

// Name resolution and static type checking (DESIGN.md 5.4-5.8, LANGUAGE.md).
// Lowers a parsed unit into the typed IR (tir.hpp) inside a Program.
//
// A Program accumulates units: a file is one unit; the REPL adds one per
// input, and later inputs see earlier declarations. A unit with errors is
// rolled back (its names are unregistered) so the session stays consistent.
// Like the parser, the checker reports every independent error it finds.
// Semantics decided while implementing it are listed in LANGUAGE.md §9.
//
// Not implemented yet (reported as "not supported yet" diagnostics):
// Coroutines, signals and vectors are implemented (LANGUAGE.md 9.10-9.11).
// TODO(phase 4): `foreach` over pools (particle emitters; LANGUAGE.md 5).
// TODO(phase 7): manual memory (`new`, `alloc`, `free`, `manual class`, `weak`).
// TODO: property accessors (get/set), map literals and Map/Set, fixed arrays `[T; N]`, ivecN,
//   Result and `?` propagation, method values, generic traits and trait
//   inheritance, `Iterable`/`IndexSet`, static variables in generic types,
//   attribute semantics other than @native and @export (@deprecated, ...).

#include <cstdint>

#include "pluma/ast.hpp"
#include "pluma/diagnostic.hpp"
#include "pluma/sema.hpp"
#include "pluma/tir.hpp"

namespace pluma {

struct CheckedUnit {
    bool ok = false;
    // Runs the unit's top-level statements (script mode) or its global
    // initializers (module mode). Always set when ok.
    Function* chunk = nullptr;
    // `fn main()` declared by this unit (module mode), if any.
    Function* main = nullptr;
};

struct CheckOptions {
    // REPL: top-level expression statements print their value.
    bool echo = false;
    // Tooling / LSP: keep resolved declarations and typed IR in `Program` even
    // when errors were reported, so completion, hover and go-to-definition work
    // on incomplete or broken files.
    bool keep_on_error = false;
    // `@test` functions (LANGUAGE.md 9.27) are checked and listed in
    // Program::tests; otherwise they are left out entirely, as in release builds.
    bool tests = false;
};

CheckedUnit check_script(Program& program, const SourceFile& source, const Script& script, DiagnosticSink& diags,
                         CheckOptions options = {});
CheckedUnit check_module(Program& program, const SourceFile& source, const Module& module, DiagnosticSink& diags,
                         CheckOptions options = {});

} // namespace pluma

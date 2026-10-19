#pragma once

// Helpers for parser tests: parse text, then compare the S-expression dump,
// the diagnostics ("line:col: message"), and the print -> parse round trip.

#include <doctest.h>

#include <format>
#include <string>
#include <string_view>

#include "pluma/arena.hpp"
#include "pluma/ast_dump.hpp"
#include "pluma/diagnostic.hpp"
#include "pluma/parser.hpp"
#include "pluma/printer.hpp"
#include "pluma/source.hpp"

namespace test {

enum class Root { Script, Module, Expr };

struct Parsed {
    std::string dump;    // whole tree
    std::string first;   // dump of the first statement / declaration / the expression
    std::string printed; // canonical source
    std::string errors;  // "line:col: message\n" per diagnostic
    std::string rendered;
    std::size_t error_count = 0;
};

inline Parsed parse(std::string_view text, Root root = Root::Script) {
    pluma::SourceFile source("test.plm", std::string(text));
    pluma::Arena arena;
    pluma::DiagnosticSink diags(source);
    Parsed out;
    switch (root) {
        case Root::Script: {
            pluma::Script s = pluma::parse_script(source, arena, diags);
            out.dump = pluma::dump(s);
            if (!s.stmts.empty()) out.first = pluma::dump(s.stmts[0]);
            out.printed = pluma::print(s);
            break;
        }
        case Root::Module: {
            pluma::Module m = pluma::parse_module(source, arena, diags);
            out.dump = pluma::dump(m);
            if (!m.decls.empty()) out.first = pluma::dump(m.decls[0]);
            out.printed = pluma::print(m);
            break;
        }
        case Root::Expr: {
            pluma::Expr* e = pluma::parse_expression(source, arena, diags);
            out.dump = pluma::dump(e);
            out.first = out.dump;
            out.printed = pluma::print(e);
            break;
        }
    }
    for (const pluma::Diagnostic& d : diags.all()) {
        out.errors += std::format("{}:{}: {}\n", d.line, d.column, d.message);
    }
    out.rendered = pluma::render_all(diags.all(), source);
    out.error_count = diags.error_count();
    return out;
}

// parse -> print -> parse gives the same AST, and printing is idempotent.
inline void check_round_trip(std::string_view text, Root root = Root::Script) {
    Parsed a = parse(text, root);
    REQUIRE_MESSAGE(a.errors.empty(), a.rendered);
    Parsed b = parse(a.printed, root);
    INFO("printed:\n" << a.printed);
    CHECK_MESSAGE(b.errors.empty(), b.rendered);
    CHECK(b.dump == a.dump);
    CHECK(b.printed == a.printed);
}

// Valid source: no diagnostics, the first item dumps as expected, and it
// round-trips.
inline void accepts(std::string_view text, std::string_view expected, Root root = Root::Script) {
    INFO("source: " << text);
    Parsed p = parse(text, root);
    CHECK_MESSAGE(p.errors.empty(), p.rendered);
    CHECK(p.first == expected);
    if (p.errors.empty()) check_round_trip(text, root);
}

inline void accepts_expr(std::string_view text, std::string_view expected) { accepts(text, expected, Root::Expr); }

// Invalid source: exactly these diagnostics.
inline void rejects(std::string_view text, std::string_view expected_errors, Root root = Root::Script) {
    INFO("source: " << text);
    Parsed p = parse(text, root);
    CHECK(p.errors == expected_errors);
}

} // namespace test

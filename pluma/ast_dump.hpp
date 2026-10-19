#pragma once

#include <string>

#include "pluma/ast.hpp"

namespace pluma {

// S-expression dump of an AST, without spans: `(+ a (* b 2))`. Two trees that
// dump the same are the same AST, which is what the round-trip tests compare.
// `pretty` puts nested nodes on indented lines (used by the REPL's `:ast`).
std::string dump(const Module& module, bool pretty = false);
std::string dump(const Script& script, bool pretty = false);
std::string dump(const Expr* expr, bool pretty = false);
std::string dump(const Stmt* stmt, bool pretty = false);
std::string dump(const Decl* decl, bool pretty = false);

} // namespace pluma

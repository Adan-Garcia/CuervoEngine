#pragma once

#include <string>

#include "pluma/ast.hpp"

namespace pluma {

// Emits canonical source for an AST, following the style guide (LANGUAGE.md 6):
// 4-space indents, braces on the same line, `} else {`, spaces around binary
// operators and the ternary, none before postfix `?`. Parentheses are
// re-inserted from the precedence table (4.1) only where needed, so
// parse(print(ast)) yields the same AST. Comments and blank lines inside
// blocks are not part of the AST and are not preserved.
std::string print(const Module& module);
std::string print(const Script& script);
std::string print(const Expr* expr);

} // namespace pluma

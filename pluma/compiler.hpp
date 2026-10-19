#pragma once

// Typed IR -> register bytecode (DESIGN.md 5.12).
//
// Incremental: `sync` brings a VM up to date with everything the Program has
// gained since the last call (new classes, structs, enums, globals and
// functions). The REPL calls it after each checked input.

#include "pluma/sema.hpp"

namespace pluma {

class VM;

void compile_program(Program& program, VM& vm);

} // namespace pluma

#pragma once

// The language's own library written in Pluma (pluma/std/*.plm), embedded at
// build time: the math library of Milestone 9.2b (LANGUAGE.md 9.20 - 9.24).
// A host adds it to its Bindings so it is compiled before user code; the
// engine embeds the same file in its prelude (cuervo/CMakeLists.txt).

#include <string_view>

namespace pluma {

class Bindings;

// "pluma/math.plm": the name diagnostics and the debugger show for it.
std::string_view math_prelude_path();
std::string_view math_prelude_source();

// Adds the math library as the first prelude of `bindings`.
void add_math_prelude(Bindings& bindings);

} // namespace pluma

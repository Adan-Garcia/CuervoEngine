#pragma once

// Runs script text in a session that has the math library (pluma/std/math.plm)
// compiled first, like the engine's prelude.

#include <string>
#include <string_view>

#include "pluma/bindings.hpp"
#include "pluma/stdlib.hpp"
#include "tests/pluma/run_helpers.hpp"

namespace test {

inline const pluma::Bindings& math_bindings() {
    static const pluma::Bindings b = [] {
        pluma::Bindings x;
        pluma::add_math_prelude(x);
        return x;
    }();
    return b;
}

inline Ran run_math(std::string_view text, pluma::VmOptions options = {}) {
    std::string out;
    options.out = [&out](std::string_view s) { out += s; };
    pluma::Session session(options, &math_bindings());
    REQUIRE_MESSAGE(session.prelude_result().ok, session.prelude_result().rendered);
    pluma::SessionResult r = session.run_script("test.plm", std::string(text));
    return collect(r, out);
}

inline void math_prints(std::string_view text, std::string_view expected) {
    INFO("source:\n" << text);
    Ran r = run_math(text);
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    CHECK_MESSAGE(r.panic.empty(), r.panic);
    CHECK(r.out == expected);
}

// The panic's message (a panic inside the library reports the library's line,
// so only the text after "line:col: " is compared).
inline void math_panics(std::string_view text, std::string_view expected_message) {
    INFO("source:\n" << text);
    Ran r = run_math(text);
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    CHECK_FALSE(r.ok);
    std::string message = r.panic.substr(r.panic.find(": ") == std::string::npos ? 0 : r.panic.find(": ") + 2);
    CHECK(message == expected_message);
}

} // namespace test

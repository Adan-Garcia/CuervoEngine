#pragma once

// Helpers for checker and VM tests: run source through a Session and collect
// printed output, diagnostics ("line:col: message") and panics.

#include <doctest.h>

#include <format>
#include <string>
#include <string_view>

#include "pluma/session.hpp"
#include "pluma/value.hpp"

namespace test {

struct Ran {
    bool ok = false;
    bool compiled = false;
    std::string out;     // everything printed
    std::string errors;  // "line:col: message\n" per diagnostic
    std::string rendered;
    std::string panic;   // "line:col: message" or empty
};

inline Ran collect(const pluma::SessionResult& r, std::string out) {
    Ran ran;
    ran.ok = r.ok;
    ran.compiled = r.compiled;
    ran.out = std::move(out);
    for (const pluma::Diagnostic& d : r.diagnostics) {
        ran.errors += std::format("{}:{}: {}\n", d.line, d.column, d.message);
    }
    ran.rendered = r.rendered;
    if (r.panic) ran.panic = std::format("{}:{}: {}", r.panic->line, r.panic->column, r.panic->message);
    return ran;
}

// Runs `text` in script mode in a fresh session.
inline Ran run(std::string_view text, bool wrap_overflow = false) {
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    options.wrap_overflow = wrap_overflow;
    pluma::Session session(options);
    pluma::SessionResult r = session.run_script("test.plm", std::string(text));
    return collect(r, out);
}

// Valid program: no diagnostics, no panic, prints exactly `expected`.
inline void prints(std::string_view text, std::string_view expected) {
    INFO("source:\n" << text);
    Ran r = run(text);
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    CHECK_MESSAGE(r.panic.empty(), r.panic);
    CHECK(r.out == expected);
}

// Invalid program: exactly these diagnostics.
inline void rejects(std::string_view text, std::string_view expected_errors) {
    INFO("source:\n" << text);
    Ran r = run(text);
    CHECK_FALSE(r.compiled);
    CHECK(r.errors == expected_errors);
}

// Compiles, then panics with this "line:col: message".
inline void panics(std::string_view text, std::string_view expected_panic, std::string_view expected_out = "") {
    INFO("source:\n" << text);
    Ran r = run(text);
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    CHECK_FALSE(r.ok);
    CHECK(r.panic == expected_panic);
    CHECK(r.out == expected_out);
}

} // namespace test

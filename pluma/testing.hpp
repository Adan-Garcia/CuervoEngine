#pragma once

// Pluma unit tests (LANGUAGE.md 9.27, milestone 10.5b): `@test fn name() {...}`
// declarations, run by `plm test` and `cuervo-tools test`. Each test runs in
// a fresh session (globals initialized anew), so a test's outcome never
// depends on the tests before it; a panic (a failed `assert`, an index out
// of bounds, ...) fails the test.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/session.hpp"
#include "pluma/source.hpp"

namespace pluma {

struct TestOutcome {
    std::string name; // qualified: "test_ease", "tween.test_ease" in a package
    std::string file;
    std::uint32_t line = 0;
    bool passed = false;
    std::string message; // the panic, for a failure
};

struct TestReport {
    bool compiled = false;
    std::string rendered; // diagnostics when it did not compile, or a failed initialization
    std::vector<TestOutcome> results;
    std::size_t passed() const;
    std::size_t failed() const { return results.size() - passed(); }
    bool ok() const { return compiled && failed() == 0; }
};

// Makes the session a test runs in (with the host's bindings). Default: a
// plain Session.
using SessionFactory = std::function<std::unique_ptr<Session>()>;

// Which tests run: those of one namespace (the root, a game's own scripts,
// or a package's: testing a game doesn't run its packages' tests) whose
// name contains `filter`.
struct TestSelection {
    std::string_view module;
    std::string_view filter;
};

// Checks `files` with their tests and runs the selected ones, in source order.
TestReport run_tests(const std::vector<SourceFile::Part>& files, const SessionFactory& make_session = {},
                     TestSelection selection = {});

// `ok   test_name (file:line)` lines, failures with their message, and a
// `N passed, M failed` summary.
std::string format_test_report(const TestReport& report);

} // namespace pluma

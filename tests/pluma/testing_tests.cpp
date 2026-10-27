// `@test` functions and the test runner (LANGUAGE.md 9.27, milestone 10.5b).

#include <doctest.h>

#include <string>
#include <vector>

#include "pluma/session.hpp"
#include "pluma/testing.hpp"

namespace {

using Part = pluma::SourceFile::Part;

const char* sample = R"(
var calls = 0
fn add(a: int, b: int) -> int { return a + b }

@test fn adds() {
    calls += 1
    assert(add(2, 2) == 4)
    // Each test starts from freshly initialized globals.
    assert(calls == 1)
}

@test fn fails() {
    calls += 1
    assert(add(2, 2) == 5, "2 + 2 is not 5")
}

@test fn indexes_out_of_bounds() {
    var xs = [1, 2]
    assert(xs[calls + 5] == 0)
}

@test fn still_fresh() { assert(calls == 0) }
)";

} // namespace

TEST_CASE("tests: each @test runs on its own; a panic fails it") {
    pluma::TestReport r = pluma::run_tests({{"t.plm", sample}});
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    REQUIRE(r.results.size() == 4);
    CHECK(r.results[0].name == "adds");
    CHECK(r.results[0].passed);
    CHECK(r.results[0].file == "t.plm");
    CHECK(r.results[0].line == 5);
    CHECK_FALSE(r.results[1].passed);
    CHECK(r.results[1].message.find("assertion failed: 2 + 2 is not 5") != std::string::npos);
    CHECK_FALSE(r.results[2].passed);
    CHECK(r.results[3].passed);
    CHECK(r.passed() == 2);
    CHECK_FALSE(r.ok());
    const std::string text = pluma::format_test_report(r);
    CHECK(text.find("ok   adds (t.plm:5)") != std::string::npos);
    CHECK(text.find("FAIL fails (t.plm:12)") != std::string::npos);
    CHECK(text.find("2 passed, 2 failed") != std::string::npos);

    r = pluma::run_tests({{"t.plm", sample}}, {}, pluma::TestSelection{{}, "add"});
    REQUIRE(r.results.size() == 1);
    CHECK(r.ok());
}

TEST_CASE("tests: outside test mode @test functions don't exist") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({{"t.plm", "@test fn t() { assert(false) }\n"
                                                    "@test fn broken() { var x: int = \"not checked\" }\n"}});
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(s.program().tests.empty());
    // Other code can't call them.
    pluma::Session calls;
    r = calls.load_files({{"t.plm", "@test fn t() {}\nfn f() { t() }"}});
    CHECK_FALSE(r.compiled);
}

TEST_CASE("tests: @test must be a plain top-level function") {
    auto first_error = [](std::string src) {
        pluma::TestReport r = pluma::run_tests({{"t.plm", std::move(src)}});
        return r.compiled ? std::string() : r.rendered;
    };
    CHECK(first_error("@test fn t(x: int) {}").find("must take no parameters and return nothing") !=
          std::string::npos);
    CHECK(first_error("@test fn t() -> int { return 1 }").find("must take no parameters") != std::string::npos);
    CHECK(first_error("class C {\n    @test fn t() {}\n}").find("applies only to top-level functions") !=
          std::string::npos);
    CHECK(first_error("@test var x = 1").find("applies only to top-level functions") != std::string::npos);
    CHECK(first_error("@test fn t() {\n    var x: int = \"s\"\n}").find("t.plm:2") != std::string::npos);
}

TEST_CASE("tests: a package's tests run in its namespace and are selected by it") {
    std::vector<Part> files{
        Part{"packages/m/scripts/m.plm", "pub fn twice(x: int) -> int { return helper(x) }\n"
                                         "fn helper(x: int) -> int { return x * 2 }\n"
                                         "@test fn private_names_are_visible() { assert(helper(2) == 4) }\n",
             "m"},
        Part{"scripts/game.plm", "import m\n@test fn uses_the_package() { assert(m.twice(3) == 6) }\n"},
    };
    pluma::TestReport game = pluma::run_tests(files);
    REQUIRE_MESSAGE(game.compiled, game.rendered);
    REQUIRE(game.results.size() == 1);
    CHECK(game.results[0].name == "uses_the_package");
    CHECK(game.ok());
    pluma::TestReport pkg = pluma::run_tests(files, {}, pluma::TestSelection{"m", {}});
    REQUIRE(pkg.results.size() == 1);
    CHECK(pkg.results[0].name == "m.private_names_are_visible");
    CHECK(pkg.ok());
}

#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cuervo/platform/eula.hpp"
#include "cuervo/platform/platform.hpp"
#include "eula_gate.hpp" // pluma/repl: plm's own copy of the gate

// The EULA gate (DESIGN.md 4.16 "EULA agreement"): storage, versioning, the
// non-interactive and interactive paths, and that plm's copy agrees with it.
// The pack exemption and the programs' refusals run through the shipped
// binaries (tests/eula.cmake).
namespace {

namespace fs = std::filesystem;
using namespace cuervo;

fs::path temp_file(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "cuervo_eula_tests";
    fs::create_directories(dir);
    const fs::path file = dir / name;
    fs::remove(file);
    return file;
}

eula::GateInput input(const fs::path& file) {
    eula::GateInput g;
    g.program = "cuervo";
    g.version = "0.1";
    g.file = file;
    return g;
}

} // namespace

TEST_CASE("the EULA version and text come from LICENSE.md") {
    CHECK(eula::version() == CUERVO_EULA_VERSION);
    CHECK(eula::license_text().find("Version " + std::string(eula::version())) != std::string_view::npos);
    CHECK(eula::license_text().find("## 19. General") != std::string_view::npos);
    CHECK(eula::notices_text().find("Third") != std::string_view::npos);
    const std::string_view summary = eula::summary();
    CHECK(summary.find("free for hobbyists") != std::string_view::npos);
    CHECK(summary.find("## 1. Definitions") == std::string_view::npos); // only the plain-English part
}

TEST_CASE("acceptance is stored with the version and the date") {
    const fs::path file = temp_file("a.txt");
    CHECK_FALSE(eula::read_acceptance(file));
    std::string error;
    REQUIRE(eula::write_acceptance(file, {"0.1", "2026-10-18"}, error));
    const auto a = eula::read_acceptance(file);
    REQUIRE(a);
    CHECK(a->version == "0.1");
    CHECK(a->date == "2026-10-18");
    CHECK(eula::accepted(file, "0.1"));
    CHECK_FALSE(eula::accepted(file, "0.2")); // a new version asks again (EULA 19.2)
    CHECK(eula::today().size() == 10);
}

TEST_CASE("non-interactive runs without acceptance are refused with how to accept") {
    eula::GateInput g = input(temp_file("b.txt"));
    std::string message;
    CHECK(eula::run_gate(g, message) == eula::GateResult::Refused);
    CHECK(message.find("--accept-eula") != std::string::npos);
    CHECK(message.find("CUERVO_ACCEPT_EULA=0.1") != std::string::npos);
    CHECK(message.find("no work was lost") != std::string::npos);
    CHECK_FALSE(fs::exists(g.file));
}

TEST_CASE("--accept-eula stores the acceptance; the environment accepts one run only") {
    eula::GateInput g = input(temp_file("c.txt"));
    std::string message;
    g.env_version = "0.1";
    CHECK(eula::run_gate(g, message) == eula::GateResult::Accepted);
    CHECK_FALSE(fs::exists(g.file));
    g.env_version = "0.0";
    CHECK(eula::run_gate(g, message) == eula::GateResult::Refused);
    CHECK(message.find("does not match") != std::string::npos);
    g.env_version.reset();
    g.accept_flag = true;
    CHECK(eula::run_gate(g, message) == eula::GateResult::Accepted);
    CHECK(eula::accepted(g.file, "0.1"));
    g.accept_flag = false;
    CHECK(eula::run_gate(g, message) == eula::GateResult::Accepted); // remembered
    g.version = "0.2";
    CHECK(eula::run_gate(g, message) == eula::GateResult::Refused); // new version
}

TEST_CASE("the terminal prompt accepts on yes and declines otherwise") {
    for (const char* answer : {"yes\n", "no\n", "\n", ""}) {
        eula::GateInput g = input(temp_file("d.txt"));
        std::istringstream in(answer);
        std::ostringstream out;
        g.interactive = true;
        g.in = &in;
        g.out = &out;
        std::string message;
        const bool yes = std::string(answer) == "yes\n";
        CHECK((eula::run_gate(g, message) == eula::GateResult::Accepted) == yes);
        CHECK(eula::accepted(g.file, "0.1") == yes);
        CHECK(out.str().find("hobbyists") != std::string::npos); // the plain-English summary is shown first
        if (!yes) CHECK(message.find("nothing was run") != std::string::npos);
    }
    eula::GateInput g = input(temp_file("e.txt"));
    std::istringstream in("view\nyes\n");
    std::ostringstream out;
    g.interactive = true;
    g.in = &in;
    g.out = &out;
    std::string message;
    CHECK(eula::run_gate(g, message) == eula::GateResult::Accepted);
    CHECK(out.str().find("## 19. General") != std::string::npos); // 'view' prints the whole agreement
}

TEST_CASE("--accept-eula is removed from argv for positional parsers") {
    char a0[] = "tool", a1[] = "--accept-eula", a2[] = "tier", a3[] = "--accept-eula";
    char* argv[] = {a0, a1, a2, a3, nullptr};
    int argc = 4;
    eula::strip_accept_flag(argc, argv);
    CHECK(argc == 2);
    CHECK(std::string(argv[1]) == "tier");
}

TEST_CASE("plm's copy of the gate agrees with the engine's") {
    // Same folder and file name for the same user (SDL_GetPrefPath's convention).
    CHECK(plm_eula::acceptance_file() == eula::acceptance_file());
    // An acceptance written by either is read by the other.
    const fs::path file = temp_file("f.txt");
    std::string error;
    REQUIRE(eula::write_acceptance(file, {CUERVO_EULA_VERSION, eula::today()}, error));
    CHECK(plm_eula::accepted(file, CUERVO_EULA_VERSION));
    CHECK_FALSE(plm_eula::accepted(file, "99.9"));

    plm_eula::Input g;
    g.version = CUERVO_EULA_VERSION;
    g.file = temp_file("g.txt");
    std::string message;
    CHECK(plm_eula::decide(g, message) == plm_eula::Result::Refused);
    CHECK(message.find("--accept-eula") != std::string::npos);
    g.accept_flag = true;
    CHECK(plm_eula::decide(g, message) == plm_eula::Result::Accepted);
    CHECK(eula::accepted(g.file, eula::version())); // stored in the engine's format
    CHECK(std::string_view(CUERVO_EULA_VERSION) == eula::version());
}

// The flag tables behind `--help` and docs/CLI.md (runtime/cli_flags.hpp,
// tools/cuervo_tools/cli_flags.hpp): every flag the parsers handle is listed,
// shows up in --help and in the committed reference.

#include <doctest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "tools/cuervo_tools/cli_docs.hpp"

namespace {

std::string read_text(const std::string& relative) {
    std::ifstream in(std::string(CUERVO_SOURCE_DIR) + "/" + relative, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "can't open " << relative);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// "--flag" / "-x" string literals a parser compares against.
std::set<std::string> flag_literals(const std::string& relative) {
    const std::string text = read_text(relative);
    std::set<std::string> out;
    const std::regex re(R"re("(--?[A-Za-z][A-Za-z-]*)")re");
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) out.insert((*it)[1].str());
    return out;
}

} // namespace

TEST_CASE("cli: every flag the runtime parsers handle is in the table") {
    for (const char* file : {"runtime/main.cpp", "runtime/game_cli.cpp", "runtime/dap_cli.cpp", "runtime/particles_demo.cpp",
                             "runtime/vector_demo.cpp", "runtime/svg_viewer.cpp"}) {
        for (const std::string& flag : flag_literals(file)) {
            CHECK_MESSAGE(cuervo::cli::find(cuervo::cli::runtime_table(), flag) != nullptr,
                          file << " handles " << flag << " but runtime/cli_flags.hpp does not list it");
        }
    }
}

TEST_CASE("cli: every flag cuervo-tools handles is in the table") {
    for (const std::string& flag : flag_literals("tools/cuervo_tools/main.cpp")) {
        CHECK_MESSAGE(cuervo::cli::find(cuervo::tools::tools_table(), flag) != nullptr,
                      "main.cpp handles " << flag << " but cuervo_tools/cli_flags.hpp does not list it");
    }
}

TEST_CASE("cli: --help and the reference list every flag") {
    const std::string runtime_help = cuervo::cli::runtime_help();
    const std::string tools_help = cuervo::tools::tools_help();
    const std::string docs = read_text("docs/CLI.md");
    for (const cuervo::cli::Flag& f : cuervo::cli::runtime_table()) {
        CHECK_MESSAGE(runtime_help.find(f.name) != std::string::npos, "cuervo --help lacks " << f.name);
        CHECK_MESSAGE(docs.find(std::string(f.name)) != std::string::npos, "docs/CLI.md lacks " << f.name);
    }
    for (const cuervo::cli::Flag& f : cuervo::tools::tools_table()) {
        CHECK_MESSAGE(tools_help.find(f.name) != std::string::npos, "cuervo-tools --help lacks " << f.name);
        CHECK_MESSAGE(docs.find(std::string(f.name)) != std::string::npos, "docs/CLI.md lacks " << f.name);
    }
    for (const cuervo::tools::Command& c : cuervo::tools::command_table()) {
        CHECK_MESSAGE(tools_help.find(c.usage) != std::string::npos, "cuervo-tools --help lacks " << c.name);
    }
}

TEST_CASE("cli: the screenshot tick default is documented") {
    CHECK(cuervo::cli::default_screenshot_tick == 240);
    CHECK(cuervo::cli::runtime_help().find("defaults to 240") != std::string::npos);
    CHECK(cuervo::tools::generate_cli_docs().find("defaults to **240**") != std::string::npos);
    CHECK(cuervo::tools::generate_cli_docs().find("CUERVO_SCREENSHOT_TICK") != std::string::npos);
}

TEST_CASE("cli: flag names are unique within a section") {
    for (auto table : {cuervo::cli::runtime_table(), cuervo::tools::tools_table()}) {
        std::set<std::string> seen;
        for (const cuervo::cli::Flag& f : table) {
            CHECK_MESSAGE(seen.insert(std::string(f.section) + " " + std::string(f.name)).second,
                          f.name << " is listed twice in " << f.section);
        }
    }
}

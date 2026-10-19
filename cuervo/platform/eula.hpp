#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

// The Cuervo Engine License Agreement gate (DESIGN.md 4.16 "EULA agreement",
// LICENSE.md). Using the compiled engine requires accepting the EULA once per
// user and EULA version. The version and text come from LICENSE.md at build time
// (cmake/Eula.cmake). Acceptance is a small file under the per-user data dir
// (platform::user_data_dir("Cuervo Engine")) holding the version and date; a new
// version asks again (EULA 19.2).
//
// The command-line programs (`cuervo`, `cuervo-tools`) call require_acceptance();
// the editor shows the agreement in its first-launch window instead. Running a
// pack (an exported game) never calls it: the players of an exported game are
// the game's customers, not Engine licensees. pluma/repl carries its own small
// copy of the same rules because pluma/ never depends on cuervo/; a test keeps
// the two in agreement (tests/cuervo/eula_tests.cpp).
namespace cuervo::eula {

// "0.1": the version in LICENSE.md.
std::string_view version();
// LICENSE.md and THIRD_PARTY_NOTICES.md, embedded.
std::string_view license_text();
std::string_view notices_text();
// The plain-English section at the top of the license ("A word before the legal text").
std::string_view summary();

struct Acceptance {
    std::string version;
    std::string date; // YYYY-MM-DD (UTC)
    friend bool operator==(const Acceptance&, const Acceptance&) = default;
};

// <user data dir>/eula-accepted.txt (CUERVO_USER_DIR overrides the directory).
std::filesystem::path acceptance_file();
std::optional<Acceptance> read_acceptance(const std::filesystem::path& file);
bool write_acceptance(const std::filesystem::path& file, const Acceptance& acceptance, std::string& error);
// Today's date, UTC.
std::string today();

// True when `file` holds an acceptance of `current`.
bool accepted(const std::filesystem::path& file, std::string_view current);
// True when this user accepted this build's EULA version.
bool accepted();
// Records acceptance of this version, dated today.
bool record_acceptance(std::string& error);

struct GateInput {
    std::string_view program;                // "cuervo", for messages
    std::string_view version;                // the version being asked about
    std::filesystem::path file;              // where acceptance is stored
    bool accept_flag = false;                // --accept-eula
    std::optional<std::string> env_version;  // CUERVO_ACCEPT_EULA
    bool interactive = false;                // a person at a terminal
    std::istream* in = nullptr;              // the prompt's answers
    std::ostream* out = nullptr;             // the prompt (stderr)
};

enum class GateResult { Accepted, Declined, Refused };

// The decision, without touching the process: stored acceptance of this version,
// --accept-eula (stored), CUERVO_ACCEPT_EULA=<this version> (this run only), a
// prompt when interactive, else Refused. `message` explains Declined/Refused in
// the kit's formula: what happened, whether work is lost, what to do.
GateResult run_gate(const GateInput& input, std::string& message);

// The entry points' one call: reads argv, the environment and the terminal.
// Returns false (after printing why to stderr) when the program must exit.
bool require_acceptance(int argc, char** argv, std::string_view program);

// Removes every --accept-eula from argv (argc shrinks), for programs whose argument parsing is positional.
void strip_accept_flag(int& argc, char** argv);

} // namespace cuervo::eula

#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

// plm's copy of the Cuervo Engine License Agreement gate (DESIGN.md 4.16 "EULA
// agreement"). The shared implementation is cuervo/platform/eula.*, but pluma/
// never depends on cuervo/, so the `plm` program carries this small copy of the
// same rules: the same acceptance file in the same per-user folder (the one
// SDL_GetPrefPath("Cuervo", "Cuervo Engine") names), the same version from
// LICENSE.md (CUERVO_EULA_VERSION, set by CMake), the same flag and environment
// variable. tests/cuervo/eula_tests.cpp checks the two agree. It lives in the
// program, not in the pluma library: embedding the language in another host
// (the engine, a test) never asks.
namespace plm_eula {

std::filesystem::path acceptance_file();
// True when `file` holds an acceptance of `version`.
bool accepted(const std::filesystem::path& file, std::string_view version);

struct Input {
    std::string_view version;
    std::filesystem::path file;
    bool accept_flag = false;
    std::optional<std::string> env_version;
    bool interactive = false;
    std::istream* in = nullptr;
    std::ostream* out = nullptr;
};
enum class Result { Accepted, Declined, Refused };
Result decide(const Input& input, std::string& message);

// Reads argv, the environment and the terminal; false = print and exit.
bool require_acceptance(int argc, char** argv);

} // namespace plm_eula

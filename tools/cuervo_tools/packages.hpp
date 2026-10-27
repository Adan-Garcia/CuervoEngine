#pragma once

// `cuervo-tools package new|check` (milestone 10.5b, docs/PACKAGES.md): make
// a Pluma package and check it on its own, outside any game.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/testing.hpp"

namespace cuervo::tools {

// Writes `<dir>/package.toml` and `<dir>/scripts/<name>.plm`. Fails if the
// directory already holds a package.
bool new_package(const std::filesystem::path& dir, std::string_view name, std::string_view version,
                 std::string& error);

struct PackageCheck {
    std::string name;
    std::string version;
    std::size_t files = 0;             // the package's own .plm files
    std::vector<std::string> packages; // its dependencies, "name version", in load order
};

// Resolves the package's dependencies (as project.toml's are, relative to
// the package) and checks its scripts in its own namespace against the
// engine preludes. On failure `error` holds the diagnostics.
std::optional<PackageCheck> check_package(const std::filesystem::path& dir, std::string& error);

// `cuervo-tools test`: runs the `@test` functions (LANGUAGE.md 9.27) of a
// project (its scripts and packages) or of a package directory, against the
// engine API. nullopt (and `error`) when the project can't be read.
std::optional<pluma::TestReport> test_scripts(const std::filesystem::path& dir, std::string_view filter,
                                              std::string& error);

} // namespace cuervo::tools

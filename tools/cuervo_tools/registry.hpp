#pragma once

// The package registry and `cuervo-tools plugin install|publish|verify`
// (milestone 10.5d, docs/PLUGINS.md "Sharing plugins", DESIGN.md 4.18).
//
// A registry is a directory (a local path, a shared drive, or a checkout
// served as static files):
//
//   index.json                         every published version, with checksums
//   packages/<name>/<name>-<ver>.cpkg  one archive per version
//
// An archive is a pack (cuervo/assets/pack.hpp, platform "any") holding the
// package's manifest, scripts, license and notices, its plugin binaries and
// its conformance project. Publishing verifies the package first; installing
// checks every checksum, vendors the package into the project's
// `packages/<name>/`, pins it in project.toml and verifies it again on this
// machine, including the conformance hash recorded at publish.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cuervo::tools {

struct RegistryEntry {
    std::string name;
    std::string version;
    std::string description;
    std::string license;
    std::string engine; // version range ("*": any)
    bool plugin = false;
    std::string abi;    // plugins: ABI range
    bool sim_safe = false;
    std::vector<std::string> platforms;
    std::vector<std::pair<std::string, std::string>> dependencies; // name, exact version
    std::string archive; // registry-relative path
    std::string sha256;  // of the archive
    std::map<std::string, std::string> files; // archive path -> sha256
    std::string conformance_hash; // sim-safe plugins: state hash, 16 hex digits
};

class Registry {
public:
    // Reads `<root>/index.json`; a directory without one is an empty registry.
    static std::optional<Registry> open(const std::filesystem::path& root, std::string& error);
    // Writes index.json (sorted, so the same registry gives the same file).
    bool save(std::string& error) const;

    const std::filesystem::path& root() const { return root_; }
    const std::vector<RegistryEntry>& entries() const { return entries_; }
    const RegistryEntry* find(std::string_view name, std::string_view version) const;
    // Every version of `name`, newest first.
    std::vector<const RegistryEntry*> versions(std::string_view name) const;
    void put(RegistryEntry entry);

private:
    std::filesystem::path root_;
    std::vector<RegistryEntry> entries_;
};

struct VerifyOptions {
    // The game font the conformance project is packed with.
    std::string font_path;
    // The published entry to compare with (checksums, conformance hash).
    const RegistryEntry* expected = nullptr;
};

struct VerifyReport {
    std::string name;
    std::string version;
    std::vector<std::string> passed; // one line per check
    std::vector<std::string> failed;
    std::vector<std::string> notes;
    std::string conformance_hash; // when the conformance project ran
    bool ok() const { return failed.empty(); }
};

// `cuervo-tools plugin verify`: checks a package directory (a plugin or a
// plain script package). nullopt + `error` when it has no readable manifest;
// otherwise the report lists every check.
std::optional<VerifyReport> verify_package(const std::filesystem::path& dir, const VerifyOptions& options,
                                           std::string& error);
std::string format_verify_report(const VerifyReport& report);

struct PublishResult {
    RegistryEntry entry;
    VerifyReport report;
    bool already = false; // the same archive was published before
};

// `cuervo-tools plugin publish`: verifies `dir`, archives it into
// `registry` and records it in the index. A version can't be published
// twice with different contents.
bool publish_package(const std::filesystem::path& dir, const std::filesystem::path& registry,
                     const std::string& font_path, PublishResult& out, std::string& error);

struct InstallResult {
    std::vector<std::string> installed; // "name version", dependencies first
    std::vector<VerifyReport> reports;
};

// `cuervo-tools plugin install`: `spec` is "name" (the newest version that
// works with this engine and platform) or "name@range" ("orbit@^1.0").
// Installs its dependencies from the same registry.
bool install_package(const std::filesystem::path& project, std::string_view spec,
                     const std::filesystem::path& registry, const std::string& font_path, InstallResult& out,
                     std::string& error);

// An SPDX license expression ("MIT", "MIT OR Apache-2.0",
// "LicenseRef-Studio-EULA"); false + `why` otherwise.
bool valid_license_expression(std::string_view license, std::string& why);
// The license file of a package directory (LICENSE, LICENSE.md, ...), or empty.
std::filesystem::path find_license_file(const std::filesystem::path& dir);

// `cuervo-tools license report`: the packages a project uses, with their
// licenses, as Markdown (export writes it into licenses/PACKAGES.md).
std::optional<std::string> license_report(const std::filesystem::path& project, std::string& error);
// Writes licenses/PACKAGES.md and each package's license text
// (licenses/packages/<name>.txt) into `out_dir`. Nothing for a project
// without packages.
bool write_package_notices(const std::filesystem::path& project, const std::filesystem::path& out_dir,
                           std::vector<std::filesystem::path>& written, std::string& error);

} // namespace cuervo::tools

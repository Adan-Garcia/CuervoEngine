#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/rhi/rhi.hpp"
#include "cuervo/script/support.hpp"

// Render-tier migration report (DESIGN.md 4.4: "Changing tier is a migration.
// The editor lists every asset, shader or API use that the new tier doesn't
// support before applying the change"). `cuervo-tools tier check` prints it;
// the editor will show the same data.
//
// What is scanned, relative to the project directory:
//   project.toml   render_tier
//   shaders/**     custom GLSL (.vert .frag .comp .geom .tesc .tese .glsl):
//                  stages and constructs GLES 3.0 / WebGL2 can't express
//   scripts/**     .plm: uses of Full-only script API (TierRules::script_api),
//                  and web-export caveats (e.g. Scene.save doesn't persist)
//   scenes/**      .scene: node classes / enum values from TierRules::scene_api
//   assets/**      textures above the Compatible texture limit, formats the
//                  engine or the web export can't load, total download size
//
// Severity: an *error* blocks the target tier (the project would break), a
// *warning* is a portability problem that doesn't block, a *note* is
// information (e.g. what the web export will do).
namespace cuervo::script {

enum class Severity { Error, Warning, Note };

std::string_view severity_name(Severity severity);

struct TierIssue {
    Severity severity = Severity::Error;
    std::string file;      // project-relative, '/' separators; empty = the project
    std::uint32_t line = 0;   // 1-based; 0 = whole file
    std::uint32_t column = 0; // 1-based; 0 = whole line
    std::string message;
    friend bool operator==(const TierIssue&, const TierIssue&) = default;
};

// A script/scene API that only exists on (or behaves differently below) a tier.
struct ApiRule {
    // Script: a dotted path matched on tokens, e.g. "Scene.save" or "Gpu"
    // (one identifier). Scene: a node class name, or "Enum.Variant" for an
    // enum value stored in a field.
    std::string pattern;
    rhi::Tier tier = rhi::Tier::Full; // lowest tier that has it
    Severity severity = Severity::Error; // when the target tier is below `tier`
    std::string message;
};

struct TierRules {
    std::vector<ApiRule> script_api;
    std::vector<ApiRule> scene_api;
};

// The engine's rules. No script API is Full-only yet (the Full tier's GPU
// features are RHI-level); the table is where they go when they land.
const TierRules& default_tier_rules();

struct TierCheckReport {
    std::filesystem::path project;
    rhi::Tier declared = rhi::Tier::Compatible; // render_tier in project.toml
    rhi::Tier target = rhi::Tier::Compatible;
    std::vector<TierIssue> issues; // sorted by file, line, column
    std::uint64_t download_bytes = 0; // what a web export would preload

    std::size_t count(Severity s) const;
    bool ok() const { return count(Severity::Error) == 0; }
};

// Scans `dir` for the target tier (default: the declared one). nullopt +
// `error` if the project can't be read at all (no project.toml). With a
// `platform`, the report also refuses claims the platform does not provide
// (docs/support.json via support::check_claims): a Full tier on the web, AOT
// scripts off Linux. `native_scripts` says the export compiles scripts to native code.
std::optional<TierCheckReport> check_project_tier(const std::filesystem::path& dir, std::optional<rhi::Tier> target,
                                                  std::string& error,
                                                  const TierRules& rules = default_tier_rules(),
                                                  std::optional<support::Target> platform = std::nullopt,
                                                  bool native_scripts = false);

// The human-readable report (`cuervo-tools tier check`).
std::string format_tier_report(const TierCheckReport& report);

// Rewrites `render_tier` in project.toml (adding it to the top-level keys if
// absent), keeping every other line as it is.
bool write_project_tier(const std::filesystem::path& dir, rhi::Tier tier, std::string& error);

// Building blocks, exposed for tests.
std::vector<TierIssue> scan_shader_source(std::string_view file, std::string_view text, rhi::Tier target);
std::vector<TierIssue> scan_script_source(std::string_view file, std::string_view text, rhi::Tier target,
                                          std::span<const ApiRule> rules);
// Width and height of a PNG or JPEG from its header, without decoding.
std::optional<std::pair<std::uint32_t, std::uint32_t>> image_size(std::span<const std::byte> data);

} // namespace cuervo::script

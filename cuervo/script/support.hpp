#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/rhi/rhi.hpp"

// The supported-features contract (milestone 8.8, DESIGN.md 4.4 "Clarified (Milestone 8.8)").
// docs/support.json is the one source of truth for what the engine supports: every feature
// with its status, platforms, desktop-only / web-compatible flags and render tier. The
// documentation tables (docs/STATUS.md, README.md) are generated from it by
// tools/support_matrix.py, and this header exposes the same data, compiled in by
// cmake/SupportData.cmake, so the tier check and the exporter refuse configurations that
// claim support the selected target does not provide.
namespace cuervo::script {
struct TierIssue;
}

namespace cuervo::script::support {

enum class Status : std::uint8_t { stable, experimental, planned, unsupported };
// The tier a feature needs; `none` = independent of the render tier.
enum class TierNeed : std::uint8_t { none, compatible, full };
// Bit positions in Entry::platforms.
enum class Target : std::uint8_t { Linux = 0, Windows = 1, Web = 2 };

std::string_view status_name(Status status);
std::string_view target_name(Target target); // "linux", "windows", "web"
std::optional<Target> parse_target(std::string_view text);
// The platform this binary runs on (the export host).
Target host_target();

struct TargetEntry {
    std::string_view id;
    std::string_view name;
    Status status;
};

struct Entry {
    std::string_view id;
    std::string_view name;
    Status status;
    unsigned platforms; // bit mask over Target
    bool desktop_only;
    bool web_compatible;
    TierNeed tier;
    std::string_view note;
};

struct RhiFeatureDoc {
    std::string_view name;
    TierNeed tier;
    bool required;
};

struct CompatibleLimitsDoc {
    std::uint32_t max_texture_size, max_msaa_samples, max_uniform_block_size, max_vertex_attributes,
        uniform_offset_alignment;
};

std::span<const Entry> entries();
std::span<const TargetEntry> targets();
// The render-tier table as documented, to be compared with rhi::feature_* in tests.
std::span<const RhiFeatureDoc> documented_rhi_features();
CompatibleLimitsDoc documented_compatible_limits();
// Platforms a tier is documented for.
unsigned tier_platforms(rhi::Tier tier);

const Entry* find(std::string_view id);
const TargetEntry& target_entry(Target target);
// Shipped (stable or experimental) and listed for the target.
bool available_on(const Entry& entry, Target target);

// The claims a project makes that the target must provide (the `tier check --platform`
// and export rule): the render tier, AOT-compiled scripts, and the target's own status.
// Errors block; an experimental target or feature is a warning.
struct ProjectClaims {
    rhi::Tier render_tier = rhi::Tier::Compatible;
    bool native_scripts = false; // --aot / --scripts native
    // AOT compiles with the host's C compiler, so it only works when the host is the target.
    Target host = host_target();
};
std::vector<TierIssue> check_claims(const ProjectClaims& claims, Target target);

} // namespace cuervo::script::support

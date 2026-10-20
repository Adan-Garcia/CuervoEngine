#include "cuervo/script/support.hpp"

#include <format>

#include "cuervo/script/tier_check.hpp"

namespace cuervo::script::support {

namespace {

#include "support_data.inc"

std::string platform_list(unsigned mask) {
    std::string out;
    for (const TargetEntry& t : target_table) {
        const auto bit = 1u << static_cast<unsigned>(&t - target_table);
        if ((mask & bit) == 0) continue;
        if (!out.empty()) out += ", ";
        out += t.name;
    }
    return out.empty() ? std::string("no platform") : out;
}

const Entry& entry_for_tier(rhi::Tier tier) {
    return *find(tier == rhi::Tier::Full ? "tier-full" : "tier-compatible");
}

TierIssue claim_issue(Severity severity, std::string message) {
    return {severity, "project.toml", 0, 0, std::move(message)};
}

// One claim: the entry must be shipped and listed for the target.
void check_entry(const Entry& entry, std::string_view claim, Target target, std::vector<TierIssue>& out) {
    const TargetEntry& t = target_entry(target);
    if (!available_on(entry, target)) {
        const bool shipped = entry.status == Status::stable || entry.status == Status::experimental;
        out.push_back(claim_issue(
            Severity::Error,
            shipped ? std::format("{} is not supported on {}: docs/STATUS.md lists it for {}{}", claim, t.name,
                                  platform_list(entry.platforms), entry.desktop_only ? " (desktop only)" : "")
                    : std::format("{} is {}: docs/STATUS.md does not offer it on any platform", claim,
                                  status_name(entry.status))));
    } else if (entry.status == Status::experimental) {
        out.push_back(claim_issue(Severity::Warning, std::format("{} is experimental (docs/STATUS.md)", claim)));
    }
}

} // namespace

std::string_view status_name(Status status) {
    switch (status) {
        case Status::stable: return "stable";
        case Status::experimental: return "experimental";
        case Status::planned: return "planned";
        case Status::unsupported: return "unsupported";
    }
    return "?";
}

Target host_target() {
#ifdef _WIN32
    return Target::Windows;
#else
    return Target::Linux;
#endif
}

std::string_view target_name(Target target) { return target_table[static_cast<std::size_t>(target)].id; }

std::optional<Target> parse_target(std::string_view text) {
    for (const Target t : {Target::Linux, Target::Windows, Target::Web}) {
        if (text == target_name(t)) return t;
    }
    return std::nullopt;
}

std::span<const Entry> entries() { return entry_table; }
std::span<const TargetEntry> targets() { return target_table; }
std::span<const RhiFeatureDoc> documented_rhi_features() { return rhi_feature_table; }
CompatibleLimitsDoc documented_compatible_limits() { return compatible_limits_doc; }
unsigned tier_platforms(rhi::Tier tier) { return tier == rhi::Tier::Full ? full_tier_platforms : compatible_tier_platforms; }

const Entry* find(std::string_view id) {
    for (const Entry& e : entry_table) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const TargetEntry& target_entry(Target target) { return target_table[static_cast<std::size_t>(target)]; }

bool available_on(const Entry& entry, Target target) {
    const bool shipped = entry.status == Status::stable || entry.status == Status::experimental;
    return shipped && (entry.platforms & (1u << static_cast<unsigned>(target))) != 0;
}

std::vector<TierIssue> check_claims(const ProjectClaims& claims, Target target) {
    std::vector<TierIssue> out;
    const TargetEntry& t = target_entry(target);
    if (t.status != Status::stable) {
        out.push_back(claim_issue(Severity::Warning, std::format("{} is {} (docs/STATUS.md)", t.name, status_name(t.status))));
    }
    check_entry(entry_for_tier(claims.render_tier),
                std::format("render_tier = \"{}\"", rhi::tier_name(claims.render_tier)), target, out);
    if (claims.native_scripts) {
        if (claims.host != target) {
            out.push_back(claim_issue(Severity::Error,
                                      std::format("AOT-compiled scripts need the export host to match the target "
                                                  "(host {}, target {}): they are built with the host C compiler",
                                                  target_entry(claims.host).name, t.name)));
        } else if (const Entry* aot = find("aot")) check_entry(*aot, "AOT-compiled scripts", target, out);
    }
    return out;
}

} // namespace cuervo::script::support

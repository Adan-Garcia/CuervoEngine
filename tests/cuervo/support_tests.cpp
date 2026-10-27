// The supported-features contract (milestone 8.8, DESIGN.md 4.4 "Clarified (Milestone 8.8)"):
// docs/support.json compiled in, its render-tier table checked against the RHI feature table
// in code, and the claims checks that refuse a configuration its target cannot provide.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "cuervo/platform/platform.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/script/support.hpp"
#include "cuervo/script/tier_check.hpp"

using namespace cuervo;
using script::Severity;
namespace sup = script::support;
namespace fs = std::filesystem;

namespace {

rhi::Tier as_tier(sup::TierNeed need) { return need == sup::TierNeed::full ? rhi::Tier::Full : rhi::Tier::Compatible; }

bool has(const std::vector<script::TierIssue>& issues, Severity s, std::string_view text) {
    for (const script::TierIssue& i : issues) {
        if (i.severity == s && i.message.find(text) != std::string::npos) return true;
    }
    return false;
}

struct Project {
    fs::path dir;
    Project(const char* name, const char* tier) {
        dir = fs::temp_directory_path() / (std::string("engine_support_test_") + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::ofstream(dir / "project.toml")
            << "name = \"T\"\nmain = \"Main\"\nrender_tier = \"" << tier << "\"\n\n[input.actions]\n";
    }
    ~Project() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("support.json: the documented render-tier table is the RHI feature table in code") {
    const auto doc = sup::documented_rhi_features();
    REQUIRE(doc.size() == static_cast<std::size_t>(rhi::Feature::Count));
    for (std::size_t i = 0; i < doc.size(); ++i) {
        const auto f = static_cast<rhi::Feature>(i);
        CHECK_MESSAGE(doc[i].name == rhi::feature_name(f), "feature ", i);
        CHECK_MESSAGE(as_tier(doc[i].tier) == rhi::feature_tier(f), rhi::feature_name(f));
        CHECK_MESSAGE(doc[i].required == rhi::feature_required(f), rhi::feature_name(f));
    }
}

TEST_CASE("support.json: the documented compatible limits are the ones the tier gate enforces") {
    const rhi::Limits real = rhi::compatible_limits();
    const sup::CompatibleLimitsDoc doc = sup::documented_compatible_limits();
    CHECK(doc.max_texture_size == real.max_texture_size);
    CHECK(doc.max_msaa_samples == real.max_msaa_samples);
    CHECK(doc.max_uniform_block_size == real.max_uniform_block_size);
    CHECK(doc.max_vertex_attributes == real.max_vertex_attributes);
    CHECK(doc.uniform_offset_alignment == real.uniform_offset_alignment);
}

TEST_CASE("support.json: entries are consistent") {
    std::set<std::string_view> ids;
    for (const sup::Entry& e : sup::entries()) {
        CHECK_MESSAGE(ids.insert(e.id).second, "duplicate id ", e.id);
        CHECK_MESSAGE(e.web_compatible == ((e.platforms & (1u << static_cast<unsigned>(sup::Target::Web))) != 0), e.id);
        CHECK_MESSAGE(e.desktop_only == (e.platforms != 0 && !e.web_compatible), e.id);
        if (e.status == sup::Status::planned || e.status == sup::Status::unsupported) {
            CHECK_MESSAGE(e.platforms == 0, e.id);
        } else {
            CHECK_MESSAGE(e.platforms != 0, e.id);
        }
    }
    // The page states the same facts the checks rely on.
    const sup::Entry* full = sup::find("tier-full");
    const sup::Entry* compat = sup::find("tier-compatible");
    REQUIRE(full != nullptr);
    REQUIRE(compat != nullptr);
    CHECK(full->desktop_only);
    CHECK_FALSE(full->web_compatible);
    CHECK(full->tier == sup::TierNeed::full);
    CHECK(compat->web_compatible);
    CHECK(full->platforms == sup::tier_platforms(rhi::Tier::Full));
    CHECK(compat->platforms == sup::tier_platforms(rhi::Tier::Compatible));
    // Windows is stable since milestone 10.5a (docs/WINDOWS.md "Validation results").
    CHECK(sup::target_entry(sup::Target::Windows).status == sup::Status::stable);
    CHECK(sup::target_entry(sup::Target::Linux).status == sup::Status::stable);
    CHECK(sup::find("macos") == nullptr);
    CHECK(sup::parse_target("web") == sup::Target::Web);
    CHECK_FALSE(sup::parse_target("macos"));
}

TEST_CASE("claims: a Full tier is refused on the web and accepted on the desktop") {
    const auto web = sup::check_claims({rhi::Tier::Full, false}, sup::Target::Web);
    CHECK(has(web, Severity::Error, "render_tier = \"full\" is not supported on Web"));
    CHECK(has(web, Severity::Error, "desktop only"));
    const auto desktop = sup::check_claims({rhi::Tier::Full, false}, sup::Target::Linux);
    for (const auto& i : desktop) CHECK(i.severity != Severity::Error);
    CHECK(has(desktop, Severity::Warning, "experimental")); // the full tier is experimental
    // Compatible runs everywhere.
    for (const auto t : {sup::Target::Linux, sup::Target::Windows, sup::Target::Web}) {
        for (const auto& i : sup::check_claims({rhi::Tier::Compatible, false}, t)) CHECK(i.severity != Severity::Error);
    }
}

TEST_CASE("claims: native (AOT) scripts need a host that matches the target, and a target the matrix lists") {
    using T = sup::Target;
    // Never on the web, whatever the host.
    CHECK(has(sup::check_claims({rhi::Tier::Compatible, true, T::Linux}, T::Web), Severity::Error, "AOT"));
    CHECK(has(sup::check_claims({rhi::Tier::Compatible, true, T::Web}, T::Web), Severity::Error, "AOT-compiled scripts"));
    // Cross-host: compiled with the host's C compiler, so host != target is refused.
    CHECK(has(sup::check_claims({rhi::Tier::Compatible, true, T::Linux}, T::Windows), Severity::Error, "host to match"));
    CHECK(has(sup::check_claims({rhi::Tier::Compatible, true, T::Windows}, T::Linux), Severity::Error, "host to match"));
    // Same host and target: accepted on Linux and Windows.
    for (const auto& i : sup::check_claims({rhi::Tier::Compatible, true, T::Linux}, T::Linux)) {
        CHECK(i.severity != Severity::Error);
    }
    const auto win = sup::check_claims({rhi::Tier::Compatible, true, T::Windows}, T::Windows);
    for (const auto& i : win) CHECK(i.severity != Severity::Error);
}

TEST_CASE("claims: a stable target needs no warning; experimental features still warn") {
    // The experimental-warning path itself is covered by the Full tier above.
    const auto win = sup::check_claims({rhi::Tier::Compatible, false}, sup::Target::Windows);
    for (const auto& i : win) CHECK((i.severity != Severity::Error && i.severity != Severity::Warning));
}

TEST_CASE("tier check --platform: a Full project is refused for the web and accepted for Linux") {
    Project full("full", "full");
    std::string error;
    auto web = script::check_project_tier(full.dir, std::nullopt, error, script::default_tier_rules(), sup::Target::Web);
    REQUIRE_MESSAGE(web, error);
    CHECK_FALSE(web->ok());
    CHECK(has(web->issues, Severity::Error, "not supported on Web"));
    const std::string text = script::format_tier_report(*web);
    CHECK(text.find("render_tier = \"full\"") != std::string::npos);

    auto lin = script::check_project_tier(full.dir, std::nullopt, error, script::default_tier_rules(), sup::Target::Linux);
    REQUIRE_MESSAGE(lin, error);
    CHECK(lin->ok());

    // Migrating to compatible first (--to compatible) makes the web claim valid.
    auto migrated =
        script::check_project_tier(full.dir, rhi::Tier::Compatible, error, script::default_tier_rules(), sup::Target::Web);
    REQUIRE_MESSAGE(migrated, error);
    CHECK(migrated->ok());

    // Without a platform nothing changes (the old behavior).
    auto plain = script::check_project_tier(full.dir, std::nullopt, error);
    REQUIRE_MESSAGE(plain, error);
    CHECK(plain->ok());
}

TEST_CASE("tier check --platform: a Compatible project is accepted everywhere") {
    Project compat("compat", "compatible");
    std::string error;
    for (const auto t : {sup::Target::Linux, sup::Target::Windows, sup::Target::Web}) {
        auto r = script::check_project_tier(compat.dir, std::nullopt, error, script::default_tier_rules(), t);
        REQUIRE_MESSAGE(r, error);
        CHECK(r->ok());
    }
}

TEST_CASE("platform: executable_dir strips trailing separator and find_asset resolves brand") {
    const auto exe = platform::executable_dir();
    if (!exe.empty()) {
        CHECK_FALSE(exe.filename().empty());
        CHECK(exe.parent_path() != exe);
    }
    const auto brand = platform::find_asset("brand", CUERVO_ASSETS_DIR "/brand"); // a build tree finds it through the fallback
    CHECK(fs::exists(brand));
    CHECK(fs::exists(brand / "logo" / "mark-ember.svg"));
}

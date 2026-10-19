// Render tiers (DESIGN.md 4.4, Phase 5): RHI capability reporting and tier
// gating, project.toml's render_tier, the runtime's refuse/warn decision and
// the migration report (cuervo-tools tier check / set).

#include <doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cuervo/platform/file_watcher.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/rhi/rhi_internal.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/game_runner.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/tier_check.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

// A device that reports what the GL backend reports (everything Compatible).
rhi::DeviceInfo gl_like(rhi::Tier requested) {
    rhi::DeviceInfo info;
    info.backend = rhi::Backend::OpenGL;
    info.max_tier = rhi::Tier::Compatible;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(rhi::Feature::Count); ++i) {
        const auto f = static_cast<rhi::Feature>(i);
        if (rhi::feature_tier(f) == rhi::Tier::Compatible) info.hardware_features.set(f);
    }
    info.limits = {32768, 32, 65536, 16, 256};
    rhi::detail::apply_tier_gate(info, requested);
    return info;
}

// What the Vulkan backend reports on a desktop GPU.
rhi::DeviceInfo vulkan_like(rhi::Tier requested) {
    rhi::DeviceInfo info;
    info.backend = rhi::Backend::Vulkan;
    info.max_tier = rhi::Tier::Full;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(rhi::Feature::Count); ++i) {
        info.hardware_features.set(static_cast<rhi::Feature>(i));
    }
    info.limits = {32768, 8, 65536, 16, 64};
    rhi::detail::apply_tier_gate(info, requested);
    return info;
}

// A scratch project directory, removed afterwards.
struct TempProject {
    fs::path dir;
    explicit TempProject(const char* name) {
        dir = fs::temp_directory_path() / (std::string("engine_tier_test_") + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const fs::path& rel, std::string_view text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream out(dir / rel, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    void write_bytes(const fs::path& rel, const std::vector<std::uint8_t>& bytes) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream out(dir / rel, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::string read(const fs::path& rel) const {
        std::ifstream in(dir / rel, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
};

std::vector<std::uint8_t> png_header(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> b = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (std::uint32_t v : {w, h}) {
        for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
    }
    for (int i = 0; i < 9; ++i) b.push_back(0);
    return b;
}

bool has_issue(const script::TierCheckReport& r, script::Severity s, std::string_view file, std::uint32_t line,
               std::string_view text) {
    return std::any_of(r.issues.begin(), r.issues.end(), [&](const script::TierIssue& i) {
        return i.severity == s && i.file == file && i.line == line && i.message.find(text) != std::string::npos;
    });
}

} // namespace

TEST_CASE("Tier names and the feature table") {
    rhi::Tier t{};
    CHECK(rhi::parse_tier("compatible", t));
    CHECK(t == rhi::Tier::Compatible);
    CHECK(rhi::parse_tier("full", t));
    CHECK(t == rhi::Tier::Full);
    CHECK_FALSE(rhi::parse_tier("Full", t));
    CHECK(rhi::tier_name(rhi::Tier::Compatible) == "compatible");
    // Every Full feature is something GLES 3.0 / WebGL2 lacks; base-vertex is the first.
    CHECK(rhi::feature_tier(rhi::Feature::BaseVertexDraws) == rhi::Tier::Full);
    CHECK(rhi::feature_tier(rhi::Feature::InstancedDraws) == rhi::Tier::Compatible);
    CHECK_FALSE(rhi::feature_required(rhi::Feature::Msaa));
    CHECK_FALSE(rhi::feature_required(rhi::Feature::FrameCapture));
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(rhi::Feature::Count); ++i) {
        CHECK_FALSE(rhi::feature_name(static_cast<rhi::Feature>(i)).empty());
    }
}

TEST_CASE("Tier gating: devices serve min(requested, max) and clamp limits") {
    SUBCASE("GL never serves Full") {
        const rhi::DeviceInfo gl = gl_like(rhi::Tier::Full);
        CHECK(gl.tier == rhi::Tier::Compatible);
        CHECK_FALSE(gl.supports(rhi::Feature::BaseVertexDraws));
        CHECK(gl.supports(rhi::Feature::InstancedDraws));
        const rhi::TierReport full = rhi::check_tier(gl, rhi::Tier::Full);
        CHECK(full.gated_down);
        CHECK(full.missing_required == std::vector{rhi::Feature::BaseVertexDraws});
        CHECK_FALSE(full.ok());
        CHECK(rhi::check_tier(gl, rhi::Tier::Compatible).ok());
    }
    SUBCASE("Vulkan gated to Compatible refuses Full features, keeps the hardware report") {
        const rhi::DeviceInfo vk = vulkan_like(rhi::Tier::Compatible);
        CHECK(vk.tier == rhi::Tier::Compatible);
        CHECK(vk.hardware_features.has(rhi::Feature::BaseVertexDraws));
        CHECK_FALSE(vk.supports(rhi::Feature::BaseVertexDraws));
        // Limits behave like the guaranteed web.
        CHECK(vk.limits.max_texture_size == rhi::compatible_limits().max_texture_size);
        CHECK(vk.limits.max_msaa_samples == 4);
        CHECK(vk.limits.uniform_offset_alignment == 256); // stricter of hardware (64) and tier (256)
        CHECK(vk.uniform_offset_alignment == 256);
        bool reported = false;
        CHECK_FALSE(rhi::detail::feature_allowed(vk, rhi::Feature::BaseVertexDraws, reported));
        CHECK(reported);
        rhi::TextureDesc big;
        big.width = 8192;
        big.height = 16;
        CHECK_FALSE(rhi::detail::texture_size_allowed(vk, big));
    }
    SUBCASE("Vulkan at Full") {
        const rhi::DeviceInfo vk = vulkan_like(rhi::Tier::Full);
        CHECK(vk.tier == rhi::Tier::Full);
        CHECK(vk.supports(rhi::Feature::BaseVertexDraws));
        CHECK(vk.limits.max_texture_size == 32768);
        CHECK(vk.limits.max_msaa_samples == 8);
        CHECK(rhi::check_tier(vk, rhi::Tier::Full).ok());
        rhi::TextureDesc big;
        big.width = 8192;
        big.height = 16;
        CHECK(rhi::detail::texture_size_allowed(vk, big));
    }
    SUBCASE("Missing optional features and low limits only warn") {
        rhi::DeviceInfo weak = gl_like(rhi::Tier::Compatible);
        weak.hardware_features.set(rhi::Feature::Msaa, false);
        weak.limits.max_texture_size = 2048; // the GLES 3.0 spec minimum
        rhi::detail::apply_tier_gate(weak, rhi::Tier::Compatible);
        const rhi::TierReport r = rhi::check_tier(weak, rhi::Tier::Compatible);
        CHECK(r.ok());
        CHECK(r.missing_optional == std::vector{rhi::Feature::Msaa});
        REQUIRE(r.limit_warnings.size() == 1);
        CHECK(r.limit_warnings[0].find("texture") != std::string::npos);
    }
    CHECK(rhi::detail::msaa_sample_count(8, 4) == 4);
    CHECK(rhi::detail::msaa_sample_count(3, 16) == 2);
    CHECK(rhi::detail::msaa_sample_count(0, 16) == 1);
}

TEST_CASE("The runtime refuses Full projects on Compatible devices and warns otherwise") {
    CHECK_FALSE(script::check_device_tier(gl_like(rhi::Tier::Full), rhi::Tier::Full));
    CHECK(script::check_device_tier(gl_like(rhi::Tier::Compatible), rhi::Tier::Compatible));
    CHECK(script::check_device_tier(vulkan_like(rhi::Tier::Full), rhi::Tier::Full));
    CHECK(script::check_device_tier(vulkan_like(rhi::Tier::Compatible), rhi::Tier::Compatible));
    rhi::DeviceInfo odd = vulkan_like(rhi::Tier::Full);
    odd.hardware_features.set(rhi::Feature::BaseVertexDraws, false);
    rhi::detail::apply_tier_gate(odd, rhi::Tier::Full);
    CHECK_FALSE(script::check_device_tier(odd, rhi::Tier::Full));
}

TEST_CASE("project.toml render_tier") {
    TempProject p("toml");
    std::string error;
    p.write("project.toml", "name = \"T\"\n");
    auto project = script::load_project(p.dir, error);
    REQUIRE(project);
    CHECK(project->render_tier == rhi::Tier::Compatible); // the default
    CHECK(project->render_tier_line == 0);

    p.write("project.toml", "name = \"T\"\nrender_tier = \"full\"  # desktop only\n");
    project = script::load_project(p.dir, error);
    REQUIRE(project);
    CHECK(project->render_tier == rhi::Tier::Full);
    CHECK(project->render_tier_line == 2);

    p.write("project.toml", "render_tier = \"vulkan\"\n");
    CHECK_FALSE(script::load_project(p.dir, error));
    CHECK(error.find("render_tier must be") != std::string::npos);

    p.write("project.toml", "tier = \"full\"\n");
    CHECK_FALSE(script::load_project(p.dir, error));
    CHECK(error.find("render_tier") != std::string::npos);

    // The shipped example is portable.
    auto shmup = script::load_project(fs::path(CUERVO_EXAMPLES_DIR) / "shmup", error);
    REQUIRE(shmup);
    CHECK(shmup->render_tier == rhi::Tier::Compatible);
}

TEST_CASE("Shader scan: GLES 3.0-inexpressible constructs, with locations") {
    const std::string src = "#version 450\n"
                            "// imageLoad in a comment is fine\n"
                            "layout(std430, binding = 0) buffer Particles { vec4 p[]; };\n"
                            "/* double\n"
                            "   dvec2 */ void main() {\n"
                            "    uint i = gl_GlobalInvocationID.x; float d = 1.0;\n"
                            "}\n";
    const auto issues = script::scan_shader_source("shaders/sim.glsl", src, rhi::Tier::Compatible);
    // `std430` and `buffer` are one construct on one line: reported once.
    // Comments (line and block) are skipped.
    REQUIRE(issues.size() == 2);
    CHECK(issues[0].line == 3);
    CHECK(issues[0].column == 8); // std430
    CHECK(issues[0].message.find("storage buffers") != std::string::npos);
    CHECK(issues[1].line == 6);
    CHECK(issues[1].column == 14);
    CHECK(issues[1].message.find("compute") != std::string::npos);
    CHECK(script::scan_shader_source("shaders/sim.glsl", src, rhi::Tier::Full).empty());
}

TEST_CASE("Script scan: dotted API paths, skipping comments and strings") {
    const std::array<script::ApiRule, 2> rules = {{
        {"Gpu.compute", rhi::Tier::Full, script::Severity::Error, "GPU compute is Full-only"},
        {"Scene.save", rhi::Tier::Full, script::Severity::Note, "web: not persistent"},
    }};
    const std::string src = "// Gpu.compute(1) in a comment\n"
                            "fn f() {\n"
                            "    let s = \"Gpu.compute\"\n"
                            "    Gpu.compute(64)\n"
                            "    x.Gpu.compute(1)\n"
                            "    Scene.save(self, \"a.scene\")\n"
                            "    Scene.saved = 1\n"
                            "}\n";
    const auto issues = script::scan_script_source("scripts/a.plm", src, rhi::Tier::Compatible, rules);
    REQUIRE(issues.size() == 2);
    CHECK(issues[0] == script::TierIssue{script::Severity::Error, "scripts/a.plm", 4, 5, "Gpu.compute: GPU compute is Full-only"});
    CHECK(issues[1].severity == script::Severity::Note);
    CHECK(issues[1].line == 6);
    CHECK(script::scan_script_source("scripts/a.plm", src, rhi::Tier::Full, rules).empty());
}

TEST_CASE("Image headers") {
    const auto png = png_header(8192, 16);
    auto size = script::image_size(std::as_bytes(std::span(png)));
    REQUIRE(size);
    CHECK(size->first == 8192);
    CHECK(size->second == 16);
    // A minimal JPEG: SOI, an APP0 segment, then SOF0 with height 300, width 5000.
    const std::vector<std::uint8_t> jpg = {0xff, 0xd8, 0xff, 0xe0, 0, 4, 0, 0, 0xff, 0xc0, 0, 11, 8, 0x01, 0x2c,
                                           0x13, 0x88, 3, 0, 0, 0};
    size = script::image_size(std::as_bytes(std::span(jpg)));
    REQUIRE(size);
    CHECK(size->first == 5000);
    CHECK(size->second == 300);
    const std::vector<std::uint8_t> junk = {1, 2, 3, 4};
    CHECK_FALSE(script::image_size(std::as_bytes(std::span(junk))));
}

TEST_CASE("Migration report: a Full project checked for Compatible") {
    TempProject p("report");
    p.write("project.toml", "name = \"T\"\nmain = \"Main\"\nrender_tier = \"full\"\n\n[input.actions]\n");
    p.write("shaders/sim.comp", "#version 450\nvoid main() {}\n");
    p.write("shaders/fx.frag", "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(gl_DrawID); }\n");
    p.write("scripts/main.plm", "class Main extends Node {\n    fn ready() {\n        Scene.save(self, \"x.scene\")\n    }\n}\n");
    p.write_bytes("assets/huge.png", png_header(8192, 512));
    p.write_bytes("assets/ok.png", png_header(1024, 1024));
    p.write("assets/fonts/ui.ttf", "not really a font");
    p.write("assets/music/theme.opus", "opus");
    p.write("scenes/broken.scene", "not a scene");
    scene::SceneDoc doc;
    scene::Node root;
    root.type = "GpuSwarm";
    root.name = "swarm";
    scene::Node child;
    child.type = "Node2D";
    child.name = "fx";
    child.parent = 0;
    child.fields.push_back({"mode", "Mode", scene::Value::enum_value("Mode", "Compute")});
    doc.nodes = {root, child};
    std::string error;
    REQUIRE(scene::save_file(p.dir / "scenes/level.scene", doc, error));

    script::TierRules rules = script::default_tier_rules();
    rules.scene_api.push_back({"GpuSwarm", rhi::Tier::Full, script::Severity::Error, "GPU swarms are Full-only"});
    rules.scene_api.push_back({"Mode.Compute", rhi::Tier::Full, script::Severity::Error, "compute mode is Full-only"});

    auto report = script::check_project_tier(p.dir, rhi::Tier::Compatible, error, rules);
    REQUIRE_MESSAGE(report, error);
    CHECK(report->declared == rhi::Tier::Full);
    CHECK(report->target == rhi::Tier::Compatible);
    CHECK_FALSE(report->ok());
    const std::string text = script::format_tier_report(*report);
    INFO(text);
    CHECK(has_issue(*report, script::Severity::Note, "project.toml", 3, "changes from \"full\" to \"compatible\""));
    CHECK(has_issue(*report, script::Severity::Error, "shaders/sim.comp", 0, "compute shaders"));
    CHECK(has_issue(*report, script::Severity::Error, "shaders/fx.frag", 3, "draw parameters"));
    CHECK(has_issue(*report, script::Severity::Note, "scripts/main.plm", 3, "Scene.save"));
    CHECK(has_issue(*report, script::Severity::Error, "assets/huge.png", 0, "8192x512"));
    CHECK_FALSE(has_issue(*report, script::Severity::Error, "assets/ok.png", 0, ""));
    CHECK(has_issue(*report, script::Severity::Note, "assets/fonts/ui.ttf", 0, ".efont"));
    CHECK(has_issue(*report, script::Severity::Warning, "assets/music/theme.opus", 0, "not decoded"));
    CHECK(has_issue(*report, script::Severity::Error, "scenes/broken.scene", 0, "can't be loaded"));
    CHECK(has_issue(*report, script::Severity::Error, "scenes/level.scene", 0, "GPU swarms"));
    CHECK(has_issue(*report, script::Severity::Error, "scenes/level.scene", 0, "Mode.Compute"));
    CHECK(report->count(script::Severity::Error) == 6);
    CHECK(text.find("does NOT fit the compatible tier") != std::string::npos);
    CHECK(text.find("shaders/fx.frag:3:") != std::string::npos);
    // Sorted by file, then line.
    CHECK(std::is_sorted(report->issues.begin(), report->issues.end(),
                         [](const script::TierIssue& a, const script::TierIssue& b) { return a.file < b.file; }));

    // The same project at its own (Full) tier: only notes.
    report = script::check_project_tier(p.dir, std::nullopt, error, rules);
    REQUIRE(report);
    CHECK(report->target == rhi::Tier::Full);
    CHECK(report->count(script::Severity::Error) == 1); // the unreadable scene blocks every tier
    CHECK(report->count(script::Severity::Warning) == 1); // .opus
}

TEST_CASE("The shmup fits the compatible tier") {
    std::string error;
    auto report = script::check_project_tier(fs::path(CUERVO_EXAMPLES_DIR) / "shmup", std::nullopt, error);
    REQUIRE_MESSAGE(report, error);
    INFO(script::format_tier_report(*report));
    CHECK(report->ok());
    CHECK(report->count(script::Severity::Warning) == 0);
    CHECK(report->download_bytes > 0);
}

TEST_CASE("tier set rewrites only render_tier") {
    TempProject p("set");
    std::string error;
    p.write("project.toml", "# header\nname = \"T\"\nrender_tier = \"full\"   # keep me\n\n[input.actions]\nx = []\n");
    REQUIRE(script::write_project_tier(p.dir, rhi::Tier::Compatible, error));
    CHECK(p.read("project.toml") ==
          "# header\nname = \"T\"\nrender_tier = \"compatible\"  # keep me\n\n[input.actions]\nx = []\n");
    // Absent: added after the last top-level key, before the sections.
    p.write("project.toml", "name = \"T\"\r\nseed = 3\r\n\r\n[input.actions]\r\n");
    REQUIRE(script::write_project_tier(p.dir, rhi::Tier::Full, error));
    CHECK(p.read("project.toml") == "name = \"T\"\r\nseed = 3\r\nrender_tier = \"full\"\r\n\r\n[input.actions]\r\n");
    auto project = script::load_project(p.dir, error);
    REQUIRE(project);
    CHECK(project->render_tier == rhi::Tier::Full);
}

TEST_CASE("Platform helpers") {
    CHECK(platform::utf8(fs::path("a") / "b.plm") == "a/b.plm");
    CHECK(platform::path_from_utf8("caf\xc3\xa9.scene").filename() == fs::path(u8"café.scene"));
    CHECK(platform::hardware_threads() >= 1);
    CHECK_FALSE(platform::env("CUERVO_SURELY_UNSET_VARIABLE_42"));
    CHECK(platform::has_file_watching);

    TempProject p("watch");
    p.write("scripts/a.plm", "1");
    platform::FileWatcher w;
    w.reset({p.dir / "scripts/a.plm"}, p.dir / "scripts", ".plm");
    CHECK_FALSE(w.changed());
    p.write("scripts/b.plm", "2"); // a new file counts
    CHECK(w.changed());
    w.reset({p.dir / "scripts/a.plm", p.dir / "scripts/b.plm"}, p.dir / "scripts", ".plm");
    CHECK_FALSE(w.changed());
    fs::last_write_time(p.dir / "scripts/a.plm", fs::last_write_time(p.dir / "scripts/a.plm") + std::chrono::seconds(5));
    CHECK(w.changed());
    p.write("scripts/notes.txt", "other extensions are ignored");
    w.reset({}, p.dir / "scripts", ".plm");
    CHECK_FALSE(w.changed());
}

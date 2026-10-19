#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "cuervo/assets/manifest.hpp"
#include "cuervo/assets/pack.hpp"
#include "cuervo/assets/vfs.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/features.hpp"
#include "cuervo/script/project.hpp"
#include "tools/cuervo_tools/export.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

const fs::path shmup_dir = fs::path(CUERVO_EXAMPLES_DIR) / "shmup";

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_feature_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_file(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

} // namespace

TEST_CASE("features: detect features in examples/shmup") {
    std::string err;
    auto proj = script::load_project(shmup_dir, err);
    REQUIRE(proj.has_value());

    auto vfs = assets::Vfs::dev(shmup_dir);
    script::FeatureSet f = script::detect_features(*vfs, *proj, proj->features);

    // Shmup has audio files in assets/, emitters in enemies.plm/player.plm, hitboxes, and HUD UI.
    CHECK(f.audio == true);
    CHECK(f.particles == true);
    CHECK(f.collision == true);
    CHECK(f.ui == true);

    auto active = f.active_list();
    CHECK(std::find(active.begin(), active.end(), "audio") != active.end());
    CHECK(std::find(active.begin(), active.end(), "particles") != active.end());
    CHECK(std::find(active.begin(), active.end(), "collision") != active.end());
    CHECK(std::find(active.begin(), active.end(), "ui") != active.end());
}

TEST_CASE("features: minimal project without audio, particles, or ui") {
    const fs::path dir = temp_dir("minimal");
    write_file(dir / "project.toml",
               "name = \"Minimal\"\n"
               "main = \"Main\"\n"
               "render_tier = \"compatible\"\n");
    write_file(dir / "scripts" / "main.plm",
               "class Main extends Node {\n"
               "    fn ready() {}\n"
               "    fn update(dt: float) {}\n"
               "}\n");

    std::string err;
    auto proj = script::load_project(dir, err);
    REQUIRE(proj.has_value());

    auto vfs = assets::Vfs::dev(dir);
    script::FeatureSet f = script::detect_features(*vfs, *proj, proj->features);

    CHECK(f.audio == false);
    CHECK(f.particles == false);
    CHECK(f.collision == false);
    CHECK(f.ui == false);
    CHECK(f.vulkan == false);
    CHECK(f.dev == false);

    auto stripped = f.stripped_list();
    CHECK(std::find(stripped.begin(), stripped.end(), "audio") != stripped.end());
    CHECK(std::find(stripped.begin(), stripped.end(), "particles") != stripped.end());
    CHECK(std::find(stripped.begin(), stripped.end(), "collision") != stripped.end());
    CHECK(std::find(stripped.begin(), stripped.end(), "ui") != stripped.end());
}

TEST_CASE("features: project.toml [features] overrides") {
    const fs::path dir = temp_dir("overrides");
    write_file(dir / "project.toml",
               "name = \"Override\"\n"
               "main = \"Main\"\n"
               "[features]\n"
               "audio = false\n"
               "ui = true\n"
               "particles = false\n");
    write_file(dir / "scripts" / "main.plm",
               "class Main extends Node {\n"
               "    fn ready() {\n"
               "        Audio.play(#boom)\n" // Code mentions Audio, but [features] overrides audio = false
               "    }\n"
               "}\n");

    std::string err;
    auto proj = script::load_project(dir, err);
    REQUIRE(proj.has_value());
    CHECK(proj->features.audio == false);
    CHECK(proj->features.ui == true);
    CHECK(proj->features.particles == false);

    auto vfs = assets::Vfs::dev(dir);
    script::FeatureSet f = script::detect_features(*vfs, *proj, proj->features);

    // Explicit override audio = false overrides detection
    CHECK(f.audio == false);
    CHECK(f.ui == true);
    CHECK(f.particles == false);
}

TEST_CASE("features: particle dependency forces collision") {
    const fs::path dir = temp_dir("dep");
    write_file(dir / "project.toml",
               "name = \"Dep\"\n"
               "main = \"Main\"\n"
               "[features]\n"
               "particles = true\n"
               "collision = false\n"); // invalid combination: particles require collision
    write_file(dir / "scripts" / "main.plm", "class Main extends Node {}\n");

    std::string err;
    auto proj = script::load_project(dir, err);
    REQUIRE(proj.has_value());

    auto vfs = assets::Vfs::dev(dir);
    script::FeatureSet f = script::detect_features(*vfs, *proj, proj->features);

    // particles = true forces collision = true
    CHECK(f.particles == true);
    CHECK(f.collision == true);
}

TEST_CASE("features: manifest round trip with features") {
    assets::ExportManifest m;
    m.format = assets::manifest_format;
    m.game = "TestGame";
    m.engine = "0.1.0";
    m.platform = assets::PackPlatform::Linux;
    m.scripts = assets::ScriptForm::Source;
    m.stripped = true;
    m.features = {"audio", "collision", "particles"};
    m.stripped_features = {"dev", "font_import", "svg_import", "ui", "vulkan"};

    const std::string text = assets::write_manifest(m);
    CHECK(text.find("features = [\"audio\", \"collision\", \"particles\"]") != std::string::npos);
    CHECK(text.find("stripped_features = [\"dev\", \"font_import\", \"svg_import\", \"ui\", \"vulkan\"]") !=
          std::string::npos);

    std::string err;
    auto parsed = assets::parse_manifest(text, err);
    REQUIRE(parsed.has_value());
    CHECK(parsed->game == "TestGame");
    CHECK(parsed->stripped == true);
    CHECK(parsed->features == m.features);
    CHECK(parsed->stripped_features == m.stripped_features);
}

TEST_CASE("features: packed project records features in manifest") {
    tools::PackOptions opts;
    opts.platform = assets::PackPlatform::Linux;
    opts.engine_version = "test-7.5";
    opts.strip = true;

    tools::PackSummary summary;
    std::string error;
    auto bytes = tools::pack_project(shmup_dir, opts, summary, error);
    REQUIRE(bytes.has_value());

    // Verify notes contain features
    bool has_feat_active_note = false;
    for (const auto& note : summary.notes) {
        if (note.find("features active:") != std::string::npos) has_feat_active_note = true;
    }
    CHECK(has_feat_active_note);

    auto pack = assets::Pack::from_bytes(*bytes, error);
    REQUIRE(pack != nullptr);
    auto entry = pack->find(assets::manifest_path);
    REQUIRE(entry.has_value());

    std::string manifest_text(reinterpret_cast<const char*>(entry->data()), entry->size());
    auto manifest = assets::parse_manifest(manifest_text, error);
    REQUIRE(manifest.has_value());
    CHECK(!manifest->features.empty());
    CHECK(std::find(manifest->features.begin(), manifest->features.end(), "audio") != manifest->features.end());
    CHECK(std::find(manifest->features.begin(), manifest->features.end(), "particles") != manifest->features.end());
}

TEST_CASE("features: comments and strings don't count, interpolations and scenes do") {
    const fs::path dir = temp_dir("false_positives");
    write_file(dir / "project.toml", "name = \"Words\"\nmain = \"Main\"\n");
    write_file(dir / "scripts" / "main.plm",
               "// Label the Button, then Emitter a Shot (just words in a comment)\n"
               "class Main extends Node {\n"
               "    var title = \"Press the Button / Audio off\"\n"
               "    var cancelled = false // a field named like a UI signal\n"
               "    fn draw(g: Canvas) {\n"
               "        g.text(vec2(0.0), title)\n"
               "    }\n"
               "}\n");
    std::string err;
    auto proj = script::load_project(dir, err);
    REQUIRE_MESSAGE(proj.has_value(), err);
    auto vfs = assets::Vfs::dev(dir);
    script::FeatureSet f = script::detect_features(*vfs, *proj, proj->features);
    CHECK_FALSE(f.audio);
    CHECK_FALSE(f.ui);
    CHECK_FALSE(f.particles);
    CHECK_FALSE(f.collision);

    // A name used only inside an interpolation still counts.
    write_file(dir / "scripts" / "main.plm",
               "class Main extends Node {\n"
               "    fn ready() { print(\"{Audio.volume(#master)}\") }\n"
               "}\n");
    auto vfs2 = assets::Vfs::dev(dir);
    CHECK(script::detect_features(*vfs2, *proj, proj->features).audio);

    // UI used only by a scene file's node types.
    write_file(dir / "scripts" / "main.plm", "class Main extends Node {}\n");
    scene::SceneDoc doc;
    doc.nodes.push_back({"Node", "root", -1, {}});
    doc.nodes.push_back({"Button", "start", 0, {}});
    fs::create_directories(dir / "scenes");
    REQUIRE_MESSAGE(scene::save_file(dir / "scenes" / "menu.scene", doc, err), err);
    auto vfs3 = assets::Vfs::dev(dir);
    script::FeatureSet g = script::detect_features(*vfs3, *proj, proj->features);
    CHECK(g.ui);
    CHECK_FALSE(g.particles);
}

TEST_CASE("features: hitboxes alone need collision, not particles; the examples") {
    const fs::path dir = temp_dir("hitboxes");
    write_file(dir / "project.toml", "name = \"Hits\"\nmain = \"Main\"\n");
    write_file(dir / "scripts" / "main.plm",
               "class Main extends Node {\n"
               "    fn update(dt: float) { Collision.hitbox(self, vec2(0.0), 4.0, 0) }\n"
               "}\n");
    std::string err;
    auto proj = script::load_project(dir, err);
    REQUIRE_MESSAGE(proj.has_value(), err);
    auto vfs = assets::Vfs::dev(dir);
    script::FeatureSet f = script::detect_features(*vfs, *proj, proj->features);
    CHECK(f.collision);
    CHECK_FALSE(f.particles);

    const auto detect = [&](std::string_view example) {
        const fs::path d = fs::path(CUERVO_EXAMPLES_DIR) / example;
        auto p = script::load_project(d, err);
        REQUIRE_MESSAGE(p.has_value(), err);
        return script::detect_features(*assets::Vfs::dev(d), *p, p->features);
    };
    const script::FeatureSet bench = detect("bench");
    CHECK(bench.particles);
    CHECK_FALSE(bench.ui);
    CHECK_FALSE(bench.audio);
    const script::FeatureSet tictak = detect("tictak");
    CHECK_FALSE(tictak.particles);
    CHECK_FALSE(tictak.ui);
}

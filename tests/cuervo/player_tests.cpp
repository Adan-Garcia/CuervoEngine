// Per-export players (tools/cuervo_tools/player.hpp, milestone 7.5): the
// backend-independent parts (feature set -> CMake definitions, cache keys,
// which player ships). The build itself runs in the export_stripped_tictak
// ctest (CUERVO_SLOW_TESTS, tests/export_stripped.cmake).

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cuervo/platform/platform.hpp"
#include "tools/cuervo_tools/export.hpp"
#include "tools/cuervo_tools/player.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_player_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_text(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

script::FeatureSet tictak_features() {
    script::FeatureSet f; // compatible tier, no audio/ui/particles/collision
    f.audio = f.ui = f.particles = f.collision = f.vulkan = false;
    return f;
}

tools::PlayerToolchain toolchain() {
    tools::PlayerToolchain t;
    t.source_dir = "/src/cuervo";
    t.cmake = "/usr/bin/cmake";
    t.generator = "Ninja";
    t.make_program = "/usr/bin/ninja";
    t.c_compiler = "/usr/bin/cc";
    t.cxx_compiler = "/usr/bin/c++";
    t.engine_version = "0.1.0";
    return t;
}

bool has(const std::vector<std::string>& v, std::string_view s) { return std::find(v.begin(), v.end(), s) != v.end(); }

} // namespace

TEST_CASE("player: --engine choices") {
    CHECK(tools::parse_engine_choice("auto") == tools::EngineChoice::Auto);
    CHECK(tools::parse_engine_choice("full") == tools::EngineChoice::Full);
    CHECK(tools::parse_engine_choice("stripped") == tools::EngineChoice::Stripped);
    CHECK_FALSE(tools::parse_engine_choice("tiny"));
    for (auto c : {tools::EngineChoice::Auto, tools::EngineChoice::Full, tools::EngineChoice::Stripped}) {
        CHECK(tools::parse_engine_choice(tools::engine_choice_name(c)) == c);
    }
}

TEST_CASE("player: feature set -> cmake definitions") {
    const auto defs = tools::player_cmake_definitions(tictak_features());
    CHECK(std::is_sorted(defs.begin(), defs.end()));
    CHECK(defs.size() == 12);
    for (std::string_view d : {"CMAKE_BUILD_TYPE=Release", "CUERVO_BUILD_TESTS=OFF", "CUERVO_BUILD_EDITOR=OFF",
                               "CUERVO_FEATURE_AUDIO=OFF", "CUERVO_FEATURE_UI=OFF", "CUERVO_FEATURE_PARTICLES=OFF",
                               "CUERVO_FEATURE_COLLISION=OFF", "CUERVO_FEATURE_DEV=OFF", "CUERVO_VULKAN=OFF",
                               "CUERVO_SVG_IMPORT=OFF", "CUERVO_FONT_IMPORT=OFF", "CUERVO_PROFILING=OFF"}) {
        CHECK_MESSAGE(has(defs, d), d);
    }
    // examples/bench: particles + collision on, the rest off.
    script::FeatureSet bench = tictak_features();
    bench.particles = bench.collision = true;
    const auto bdefs = tools::player_cmake_definitions(bench);
    CHECK(has(bdefs, "CUERVO_FEATURE_PARTICLES=ON"));
    CHECK(has(bdefs, "CUERVO_FEATURE_COLLISION=ON"));
    CHECK(has(bdefs, "CUERVO_FEATURE_AUDIO=OFF"));
    // A full-tier project keeps Vulkan; project.toml overrides reach the build.
    script::FeatureSet full;
    full.dev = full.svg_import = true;
    const auto fdefs = tools::player_cmake_definitions(full);
    CHECK(has(fdefs, "CUERVO_VULKAN=ON"));
    CHECK(has(fdefs, "CUERVO_FEATURE_DEV=ON"));
    CHECK(has(fdefs, "CUERVO_SVG_IMPORT=ON"));
    CHECK(has(fdefs, "CUERVO_FONT_IMPORT=OFF"));
}

TEST_CASE("player: what can be stripped, option dependencies") {
    CHECK(tools::can_strip(tictak_features()));
    script::FeatureSet everything;
    everything.dev = everything.svg_import = everything.font_import = true;
    CHECK_FALSE(tools::can_strip(everything));
    // The default set (all subsystems, no dev tools or importers) still drops those.
    CHECK(tools::can_strip(script::FeatureSet{}));

    CHECK(tools::check_feature_dependencies(tictak_features()).empty());
    script::FeatureSet bad = tictak_features();
    bad.particles = true; // without collision
    CHECK(tools::check_feature_dependencies(bad).find("collision") != std::string::npos);
    bad = script::FeatureSet{};
    bad.particles = false;
    bad.dev = true;
    CHECK(tools::check_feature_dependencies(bad).find("particles") != std::string::npos);
}

TEST_CASE("player: cache keys are stable and change with every input") {
    const auto t = toolchain();
    const auto lin = assets::PackPlatform::Linux;
    const std::string key = tools::player_cache_key(tictak_features(), t, lin);
    CHECK(key.starts_with("linux_core_"));
    CHECK(key.size() == std::string("linux_core_").size() + 16);
    CHECK(tools::player_cache_key(tictak_features(), t, lin) == key);

    // The same set built in another order gives the same key.
    script::FeatureSet a = tictak_features();
    a.particles = true;
    a.collision = true;
    script::FeatureSet b;
    b.set("ui", false);
    b.set("collision", true);
    b.set("vulkan", false);
    b.set("audio", false);
    b.set("particles", true);
    const std::string ka = tools::player_cache_key(a, t, lin);
    CHECK(ka == tools::player_cache_key(b, t, lin));
    CHECK(ka.starts_with("linux_collision+particles_"));
    CHECK(ka != key);

    auto changed = [&](auto mutate) {
        auto t2 = t;
        mutate(t2);
        return tools::player_cache_key(tictak_features(), t2, lin) != key;
    };
    CHECK(changed([](tools::PlayerToolchain& x) { x.engine_version = "0.2.0"; }));
    CHECK(changed([](tools::PlayerToolchain& x) { x.source_dir = "/other/cuervo"; }));
    CHECK(changed([](tools::PlayerToolchain& x) { x.cxx_compiler = "/usr/bin/clang++"; }));
    CHECK(changed([](tools::PlayerToolchain& x) { x.generator = "Unix Makefiles"; }));
    // Not part of the binary: where cmake lives and where the dependency
    // checkouts come from.
    CHECK_FALSE(changed([](tools::PlayerToolchain& x) { x.cmake = "/opt/cmake/bin/cmake"; }));
    CHECK_FALSE(changed([](tools::PlayerToolchain& x) { x.deps_dir = "/tmp/deps"; }));
    CHECK(tools::player_cache_key(tictak_features(), t, assets::PackPlatform::Web) != key);
}

TEST_CASE("player: the configure command") {
    const fs::path deps = temp_dir("deps");
    fs::create_directories(deps / "volk-src");
    fs::create_directories(deps / "msdf_atlas_gen-src");
    fs::create_directories(deps / "volk-build"); // not a checkout
    auto t = toolchain();
    t.deps_dir = deps;
    const auto argv = tools::player_configure_command(tictak_features(), t, "/cache/key");
    REQUIRE(argv.size() > 7);
    CHECK(argv[0] == "/usr/bin/cmake");
    CHECK(argv[1] == "-S");
    CHECK(argv[2] == "/src/cuervo");
    CHECK(argv[3] == "-B");
    CHECK(argv[4] == "/cache/key");
    CHECK(has(argv, "-G"));
    CHECK(has(argv, "Ninja"));
    CHECK(has(argv, "-DCMAKE_CXX_COMPILER=/usr/bin/c++"));
    CHECK(has(argv, "-DCUERVO_VULKAN=OFF"));
    const std::string msdf = "-DFETCHCONTENT_SOURCE_DIR_MSDF_ATLAS_GEN=" + platform::utf8(deps / "msdf_atlas_gen-src");
    const std::string volk = "-DFETCHCONTENT_SOURCE_DIR_VOLK=" + platform::utf8(deps / "volk-src");
    const auto im = std::find(argv.begin(), argv.end(), msdf);
    const auto iv = std::find(argv.begin(), argv.end(), volk);
    CHECK(im != argv.end());
    CHECK(iv != argv.end());
    CHECK(im < iv); // sorted: the same command every time
    CHECK(std::count_if(argv.begin(), argv.end(),
                        [](const std::string& s) { return s.starts_with("-DFETCHCONTENT_SOURCE_DIR_"); }) == 2);
}

TEST_CASE("player: toolchain checks") {
    tools::PlayerToolchain t;
    CHECK(t.missing().find("engine sources") != std::string::npos);
    const fs::path dir = temp_dir("toolchain");
    write_text(dir / "src/CMakeLists.txt", "project(x)\n");
    t.source_dir = dir / "src";
    t.cmake = dir / "no-cmake";
    CHECK(t.missing().find("cmake") != std::string::npos);
    write_text(dir / "cmake", "");
    t.cmake = dir / "cmake";
    CHECK(t.missing().empty());
}

#if !defined(_WIN32)
TEST_CASE("player: run_process captures output, exit codes and timeouts") {
    const fs::path dir = temp_dir("process");
    std::string error;
    auto r = tools::run_process({"/bin/sh", "-c", "echo hi $CUERVO_T; pwd; exit 3"}, dir, {{"CUERVO_T", "there"}},
                                dir / "log.txt", std::chrono::seconds(30), error);
    REQUIRE_MESSAGE(r, error);
    CHECK(r->exit_code == 3);
    CHECK_FALSE(r->timed_out);
    CHECK(r->output.find("hi there") != std::string::npos);
    CHECK(r->output.find("process") != std::string::npos);
    auto slow =
        tools::run_process({"/bin/sh", "-c", "sleep 10"}, dir, {}, dir / "slow.txt", std::chrono::seconds(1), error);
    REQUIRE(slow);
    CHECK(slow->timed_out);
    CHECK_FALSE(
        tools::run_process({"/nonexistent/program"}, dir, {}, dir / "none.txt", std::chrono::seconds(5), error));
}
#endif

TEST_CASE("export: which player ships") {
    const fs::path dir = temp_dir("choice");
    const fs::path tictak = fs::path(CUERVO_EXAMPLES_DIR) / "tictak";
    write_text(dir / "full-player", "#!/bin/sh\n");
    tools::ExportOptions o;
    o.pack.platform = assets::PackPlatform::Linux;
    o.pack.font_path = std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf";
    o.pack.engine_version = "test";
    o.full_runtime = dir / "full-player";
    std::string error;

    SUBCASE("auto without a toolchain ships the full player, and says why") {
        o.engine = tools::EngineChoice::Auto;
        o.out_dir = dir / "auto";
        tools::ExportSummary s;
        REQUIRE_MESSAGE(tools::export_project(tictak, o, s, error), error);
        CHECK(s.player.kind == tools::ShippedPlayer::Kind::Full);
        CHECK(s.player.full_bytes == s.player.bytes);
        CHECK_FALSE(s.pack.features.audio);
        CHECK_FALSE(s.pack.features.vulkan);
        const bool noted = std::any_of(s.pack.notes.begin(), s.pack.notes.end(),
                                       [](const std::string& n) { return n.find("no toolchain") != std::string::npos; });
        CHECK(noted);
        CHECK(fs::exists(dir / "auto/tictak"));
    }
    SUBCASE("stripped without a toolchain is an error, not a silent full player") {
        o.engine = tools::EngineChoice::Stripped;
        o.out_dir = dir / "stripped";
        tools::ExportSummary s;
        CHECK_FALSE(tools::export_project(tictak, o, s, error));
        CHECK(error.find("can't build a stripped player") != std::string::npos);
        CHECK_FALSE(fs::exists(dir / "stripped/tictak"));
    }
    SUBCASE("an explicit runtime wins") {
        o.engine = tools::EngineChoice::Stripped;
        o.runtime = dir / "full-player";
        o.out_dir = dir / "explicit";
        tools::ExportSummary s;
        REQUIRE_MESSAGE(tools::export_project(tictak, o, s, error), error);
        CHECK(s.player.kind == tools::ShippedPlayer::Kind::Explicit);
    }
    SUBCASE("web stripped players need an emsdk; Windows ones aren't supported yet") {
        o.engine = tools::EngineChoice::Stripped;
        o.out_dir = dir / "web";
        tools::ExportSummary s;
        o.pack.platform = assets::PackPlatform::Web;
        o.toolchain.emsdk = dir / "no-emsdk";
        CHECK_FALSE(tools::export_project(tictak, o, s, error));
        CHECK(error.find("no emsdk") != std::string::npos);
        o.pack.platform = assets::PackPlatform::Windows;
        CHECK_FALSE(tools::export_project(tictak, o, s, error));
        CHECK(error.find("not supported for windows yet") != std::string::npos);
    }
}

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
TEST_CASE("player: whose state hash and AOT module match this engine") {
    CHECK(tools::script_surface_matches(script::FeatureSet{})); // Vulkan/dev/importers don't count
    script::FeatureSet shmup;
    shmup.vulkan = false;
    CHECK(tools::script_surface_matches(shmup));
    CHECK_FALSE(tools::script_surface_matches(tictak_features()));
}
#endif

TEST_CASE("player: web players (emsdk toolchain, pack-fetching runtime)") {
    // Definitions: the pack-loading runtime, never Vulkan.
    script::FeatureSet everything = tictak_features();
    everything.vulkan = true;
    const auto defs = tools::player_cmake_definitions(everything, assets::PackPlatform::Web);
    CHECK(std::is_sorted(defs.begin(), defs.end()));
    CHECK(has(defs, "CUERVO_WEB_PACK=fetch"));
    CHECK(has(defs, "CUERVO_VULKAN=OFF"));
    CHECK_FALSE(has(tools::player_cmake_definitions(everything), "CUERVO_WEB_PACK=fetch"));

    // The configure command: emscripten's toolchain file and the host tools,
    // not the host compilers.
    auto t = toolchain();
    t.emsdk = "/opt/emsdk";
    t.host_build = "/src/cuervo/build/host-tools";
    const auto argv = tools::player_configure_command(tictak_features(), t, "/cache/web", assets::PackPlatform::Web);
    CHECK(has(argv, "-DCMAKE_TOOLCHAIN_FILE=/opt/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"));
    CHECK(has(argv, "-DCUERVO_HOST_BUILD=/src/cuervo/build/host-tools"));
    CHECK_FALSE(has(argv, "-DCMAKE_CXX_COMPILER=/usr/bin/c++"));

    // Missing pieces are reported, not guessed at.
    CHECK(t.missing(assets::PackPlatform::Web).find("no emsdk") != std::string::npos);
    const fs::path emsdk = temp_dir("emsdk");
    fs::create_directories(emsdk / "upstream" / "emscripten");
    std::ofstream(emsdk / "upstream" / "emscripten" / "emcc") << "";
    t.emsdk = emsdk;
    CHECK(t.missing(assets::PackPlatform::Web).find("no host-tools build") != std::string::npos);
    CHECK(t.missing(assets::PackPlatform::Windows).find("isn't supported") != std::string::npos);

    // The build environment puts the emsdk tools first.
    const auto env = tools::web_build_environment(emsdk);
    bool has_emsdk = false;
    bool path_first = false;
    for (const auto& [k, v] : env) {
        if (k == "EMSDK") has_emsdk = (v == platform::utf8(emsdk));
        if (k == "PATH") path_first = v.starts_with(platform::utf8(emsdk));
    }
    CHECK(has_emsdk);
    CHECK(path_first);
}

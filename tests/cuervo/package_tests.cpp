// Packages (milestone 10.5b, LANGUAGE.md 9.26, docs/PACKAGES.md): project.toml
// [dependencies], package.toml, resolution (paths, vendoring, versions,
// cycles), tagged script parts, hot reload and examples/garden.

#include <doctest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "tools/cuervo_tools/packages.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

// A scratch directory tree, removed afterwards.
struct Tree {
    fs::path root;
    explicit Tree(std::string_view name) {
        root = fs::temp_directory_path() / std::format("cuervo_pkg_{}_{}", name, std::random_device{}());
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << text;
    }
};

std::string manifest(std::string_view name, std::string_view version, std::string_view deps = {}) {
    return std::format("name = \"{}\"\nversion = \"{}\"\n{}", name, version, deps);
}

const char* game_toml = "name = \"g\"\nmain = \"Main\"\n";

struct Resolved {
    bool ok = false;
    std::string error;
    std::vector<std::string> order; // "name version"
};

Resolved resolve(const fs::path& project_dir) {
    Resolved r;
    auto vfs = assets::Vfs::dev(project_dir);
    auto project = script::load_project(*vfs, r.error);
    if (!project) return r;
    std::vector<script::ResolvedPackage> packages;
    r.ok = script::resolve_packages(*project, *vfs, packages, r.error);
    for (const auto& p : packages) r.order.push_back(p.manifest.name + " " + p.manifest.version);
    return r;
}

} // namespace

TEST_CASE("packages: versions and names") {
    CHECK(script::valid_version("1.2.3"));
    CHECK(script::valid_version("0.0.10"));
    CHECK_FALSE(script::valid_version("1.2"));
    CHECK_FALSE(script::valid_version("1.02.3"));
    CHECK_FALSE(script::valid_version("1.2.3-beta"));
    CHECK_FALSE(script::valid_version("^1.2.3"));
    CHECK(script::valid_package_name("tween"));
    CHECK(script::valid_package_name("ui_kit2"));
    CHECK_FALSE(script::valid_package_name("2d"));
    CHECK_FALSE(script::valid_package_name("my-lib"));
}

TEST_CASE("packages: path and vendored dependencies resolve dependencies first") {
    Tree t("resolve");
    t.write("game/project.toml", std::string(game_toml) +
                                     "[dependencies]\nvend = \"0.1.0\"\n[dependencies.fx]\npath = \"../libs/fx\"\n"
                                     "version = \"2.0.0\"\n");
    t.write("game/packages/vend/package.toml", manifest("vend", "0.1.0"));
    // fx needs util, from a path relative to fx's own directory.
    t.write("libs/fx/package.toml", manifest("fx", "2.0.0", "[dependencies.util]\npath = \"../util\"\nversion = \"1.0.0\"\n"));
    t.write("libs/util/package.toml", manifest("util", "1.0.0"));
    Resolved r = resolve(t.root / "game");
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.order == std::vector<std::string>{"vend 0.1.0", "util 1.0.0", "fx 2.0.0"});
}

TEST_CASE("packages: pinned versions, conflicts, cycles and missing packages are errors") {
    Tree t("errors");
    t.write("libs/a/package.toml", manifest("a", "1.0.0", "[dependencies.b]\npath = \"../b\"\nversion = \"1.0.0\"\n"));
    t.write("libs/b/package.toml", manifest("b", "1.0.0", "[dependencies.a]\npath = \"../a\"\nversion = \"1.0.0\"\n"));
    t.write("libs/c/package.toml", manifest("c", "1.1.0"));
    t.write("libs/d/package.toml", manifest("d", "1.0.0", "[dependencies.c]\npath = \"../c\"\nversion = \"1.0.0\"\n"));

    auto game = [&](std::string deps) {
        t.write("game/project.toml", std::string(game_toml) + deps);
        return resolve(t.root / "game");
    };
    Resolved r = game("[dependencies.a]\npath = \"../libs/a\"\nversion = \"1.0.0\"\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("dependency cycle") != std::string::npos);

    r = game("[dependencies.c]\npath = \"../libs/c\"\nversion = \"1.0.0\"\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("pins c 1.0.0, but the package is version 1.1.0") != std::string::npos);

    r = game("[dependencies.c]\npath = \"../libs/c\"\nversion = \"1.1.0\"\n[dependencies.d]\npath = \"../libs/d\"\n"
             "version = \"1.0.0\"\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("one version per package") != std::string::npos);

    r = game("[dependencies]\nnope = \"1.0.0\"\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("package 'nope' not found") != std::string::npos);

    r = game("[dependencies]\nc = \"latest\"\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("needs a pinned version") != std::string::npos);

    r = game("[dependencies.c]\npath = \"../libs/c\"\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("needs a pinned 'version'") != std::string::npos);

    // The directory's package must be the one named.
    r = game("[dependencies.zed]\npath = \"../libs/c\"\nversion = \"1.1.0\"\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("points at package 'c'") != std::string::npos);
}

TEST_CASE("packages: read_scripts tags package files and puts them first") {
    Tree t("parts");
    t.write("game/project.toml", std::string(game_toml) + "[dependencies.fx]\npath = \"../fx\"\nversion = \"1.0.0\"\n");
    t.write("game/scripts/main.plm", "import fx\nclass Main extends Node {}\n");
    t.write("fx/package.toml", manifest("fx", "1.0.0"));
    t.write("fx/scripts/a.plm", "pub fn a() {}\n");
    t.write("fx/scripts/sub/b.plm", "pub fn b() {}\n");
    std::vector<pluma::SourceFile::Part> parts;
    std::vector<fs::path> files;
    std::string error;
    REQUIRE_MESSAGE(script::read_scripts(*assets::Vfs::dev(t.root / "game"), parts, &files, error), error);
    REQUIRE(parts.size() == 3);
    CHECK(parts[0].path == "packages/fx/scripts/a.plm");
    CHECK(parts[0].module == "fx");
    CHECK(parts[1].path == "packages/fx/scripts/sub/b.plm");
    CHECK(parts[2].path == "scripts/main.plm");
    CHECK(parts[2].module.empty());
    // Package files are watched for hot reload too.
    CHECK(files.size() == 3);
}

TEST_CASE("packages: hot reload picks up a package's new code and keeps its state") {
    Tree t("reload");
    t.write("game/project.toml", std::string(game_toml) + "[dependencies.counter]\npath = \"../counter\"\n"
                                                          "version = \"1.0.0\"\n");
    t.write("game/scripts/main.plm", "import counter\nclass Main extends Node {\n    fn update(dt: float) { "
                                     "counter.bump() }\n}\n");
    t.write("counter/package.toml", manifest("counter", "1.0.0"));
    t.write("counter/scripts/counter.plm", "pub var n = 0\npub fn bump() { n += 1 }\n");
    std::vector<pluma::SourceFile::Part> parts;
    std::string error;
    auto vfs = assets::Vfs::dev(t.root / "game");
    REQUIRE_MESSAGE(script::read_scripts(*vfs, parts, nullptr, error), error);
    InputMap map;
    script::ScriptHost host({}, map);
    REQUIRE_MESSAGE(host.load(parts, "Main"), host.error());
    for (int i = 0; i < 5; ++i) host.tick(InputFrame{});
    CHECK(host.session()->global("counter.n").i == 5);

    t.write("counter/scripts/counter.plm", "pub var n = 0\npub fn bump() { n += 10 }\n");
    REQUIRE_MESSAGE(script::read_scripts(*vfs, parts, nullptr, error), error);
    REQUIRE_MESSAGE(host.load(parts, "Main"), host.error());
    CHECK(host.reloads() == 1);
    CHECK(host.session()->global("counter.n").i == 5);
    host.tick(InputFrame{});
    CHECK(host.session()->global("counter.n").i == 15);
}

TEST_CASE("examples/garden: imports two packages without name clashes, deterministically") {
    const auto run = [](int ticks) {
        std::string error;
        auto project = script::load_project(fs::path(CUERVO_EXAMPLES_DIR) / "garden", error);
        REQUIRE_MESSAGE(project.has_value(), error);
        CHECK(project->dependencies.size() == 2);
        InputMap map;
        REQUIRE_MESSAGE(script::apply_input(*project, map, error), error);
        std::vector<pluma::SourceFile::Part> sources;
        REQUIRE_MESSAGE(script::read_scripts(*project, sources, nullptr, error), error);
        script::HostOptions o;
        o.seed = project->seed;
        o.tick_rate = project->tick_rate;
        o.project_dir = project->dir;
        script::ScriptHost host(o, map);
        REQUIRE_MESSAGE(host.load(sources, project->main_class), host.error());
        for (int i = 0; i < ticks; ++i) {
            host.tick(InputFrame{});
            REQUIRE_MESSAGE(host.running(), host.error());
        }
        // The game's `petals` and the palette package's are different globals.
        CHECK(host.session()->vm().format(host.session()->global("petals"), false) == "[5, 6, 8]");
        CHECK(host.session()->vm().format(host.session()->global("palette.petals"), false).starts_with("[\"ff5973\""));
        return host.state_hash();
    };
    const std::uint64_t hash = run(90);
    CHECK(run(90) == hash);
}

TEST_CASE("packages: cuervo-tools package new and check") {
    Tree t("tools");
    std::string error;
    REQUIRE_MESSAGE(tools::new_package(t.root / "sparkle", "sparkle", "0.1.0", error), error);
    CHECK_FALSE(tools::new_package(t.root / "sparkle", "sparkle", "0.1.0", error)); // already there
    CHECK_FALSE(tools::new_package(t.root / "bad", "my-lib", "0.1.0", error));
    auto report = tools::check_package(t.root / "sparkle", error);
    REQUIRE_MESSAGE(report.has_value(), error);
    CHECK(report->name == "sparkle");
    CHECK(report->files == 1);

    // A package checks against the engine API and its own dependencies.
    t.write("fx/package.toml", manifest("fx", "1.0.0", "[dependencies.sparkle]\npath = \"../sparkle\"\n"
                                                       "version = \"0.1.0\"\n"));
    t.write("fx/scripts/fx.plm", "import sparkle\npub class Burst extends Node2D {\n"
                                 "    var label = sparkle.greeting()\n}\n");
    report = tools::check_package(t.root / "fx", error);
    REQUIRE_MESSAGE(report.has_value(), error);
    CHECK(report->packages == std::vector<std::string>{"sparkle 0.1.0"});

    t.write("fx/scripts/fx.plm", "pub fn f() -> int { return missing() }\n");
    CHECK_FALSE(tools::check_package(t.root / "fx", error).has_value());
    CHECK(error.find("missing") != std::string::npos);
}

TEST_CASE("packages: cuervo-tools test runs a game's or a package's own @test functions") {
    std::string error;
    auto game = tools::test_scripts(fs::path(CUERVO_EXAMPLES_DIR) / "garden", {}, error);
    REQUIRE_MESSAGE(game.has_value(), error);
    CHECK_MESSAGE(game->ok(), pluma::format_test_report(*game));
    CHECK(game->results.size() == 3); // not the tween package's
    auto pkg = tools::test_scripts(fs::path(CUERVO_EXAMPLES_DIR) / "libs" / "tween", {}, error);
    REQUIRE_MESSAGE(pkg.has_value(), error);
    CHECK_MESSAGE(pkg->ok(), pluma::format_test_report(*pkg));
    REQUIRE(pkg->results.size() == 3);
    CHECK(pkg->results[0].name == "tween.curves_start_at_zero_and_end_at_one");
    CHECK_FALSE(tools::test_scripts(fs::path(CUERVO_EXAMPLES_DIR), {}, error).has_value());
}

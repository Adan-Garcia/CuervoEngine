// The package registry (milestone 10.5d, tools/cuervo_tools/registry.hpp,
// docs/PLUGINS.md "Sharing plugins"): version ranges, checksums, license
// checks, and publish -> install -> verify into a fresh project.

#include <doctest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/sha256.hpp"
#include "cuervo/script/project.hpp"
#include "tools/cuervo_tools/registry.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

const fs::path examples = CUERVO_EXAMPLES_DIR;
const std::string font = std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf";

struct TempDir {
    fs::path path;
    explicit TempDir(std::string_view name)
        : path(fs::temp_directory_path() / std::format("cuervo_registry_{}_{}", name, std::random_device{}())) {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool in_range(std::string_view range, std::string_view version) {
    std::string error;
    auto r = script::parse_version_range(range, error);
    REQUIRE_MESSAGE(r.has_value(), error);
    return r->contains(*script::parse_version(version, true));
}

// A fresh game with one script that imports `pkg` and calls `call`.
fs::path fresh_project(const fs::path& root, std::string_view pkg, std::string_view call) {
    const fs::path p = root / "game";
    write_text(p / "project.toml", "name = \"fresh\"\nmain = \"Main\"\nrender_tier = \"compatible\"\n");
    write_text(p / "scripts/main.plm", std::format("import {}\nvar v = 0.0\nclass Main extends Node {{\n"
                                                   "    fn update(dt: float) {{ v = {} }}\n}}\n",
                                                   pkg, call));
    return p;
}

void make_package(const fs::path& dir, std::string_view name, std::string_view version, std::string_view extra,
                  std::string_view script) {
    write_text(dir / "package.toml", std::format("name = \"{}\"\nversion = \"{}\"\nlicense = \"MIT\"\n{}", name,
                                                 version, extra));
    write_text(dir / "LICENSE", "MIT License\n");
    write_text(dir / "scripts" / std::format("{}.plm", name), script);
}

} // namespace

TEST_CASE("registry: versions and version ranges") {
    CHECK(script::parse_version("1.2.3") == script::Version{1, 2, 3});
    CHECK_FALSE(script::parse_version("1.2").has_value());
    CHECK(script::parse_version("1.2", true) == script::Version{1, 2, 0});
    CHECK_FALSE(script::parse_version("1.x", true).has_value());
    CHECK(in_range("*", "9.9.9"));
    CHECK(in_range("1.4.0", "1.4.0"));
    CHECK_FALSE(in_range("1.4.0", "1.4.1"));
    CHECK(in_range(">=1.2, <2", "1.9.0"));
    CHECK_FALSE(in_range(">=1.2, <2", "2.0.0"));
    CHECK_FALSE(in_range(">=1.2, <2", "1.1.9"));
    CHECK(in_range("^1.2", "1.7.0"));
    CHECK_FALSE(in_range("^1.2", "2.0.0"));
    CHECK(in_range("^0.2", "0.2.5"));
    CHECK_FALSE(in_range("^0.2", "0.3.0"));
    CHECK(in_range(">0.1, <=0.1.4", "0.1.4"));
    std::string error;
    CHECK_FALSE(script::parse_version_range(">=one", error).has_value());
    CHECK(error.find("invalid version range") != std::string::npos);
    CHECK(script::parse_version(script::engine_version(), true).has_value());
}

TEST_CASE("registry: SHA-256 matches the FIPS 180-4 examples") {
    CHECK(sha256_hex(std::string_view("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(std::string_view("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 split;
    const std::string million(1000000, 'a');
    split.update(std::string_view(million).substr(0, 333333));
    split.update(std::string_view(million).substr(333333));
    std::string hex;
    for (std::uint8_t b : split.finish()) hex += std::format("{:02x}", b);
    CHECK(hex == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("registry: licenses are SPDX expressions") {
    std::string why;
    CHECK(tools::valid_license_expression("MIT", why));
    CHECK(tools::valid_license_expression("MIT OR Apache-2.0", why));
    CHECK(tools::valid_license_expression("(MIT AND Zlib) OR BSD-3-Clause", why));
    CHECK(tools::valid_license_expression("GPL-2.0-or-later WITH Classpath-exception-2.0", why));
    CHECK(tools::valid_license_expression("LicenseRef-Studio-EULA", why));
    CHECK_FALSE(tools::valid_license_expression("", why));
    CHECK(why.find("no license") != std::string::npos);
    CHECK_FALSE(tools::valid_license_expression("MIT-ish", why));
    CHECK(why.find("not an SPDX") != std::string::npos);
    CHECK_FALSE(tools::valid_license_expression("MIT OR", why));
}

TEST_CASE("registry: package.toml engine ranges, ABI ranges and platforms") {
    TempDir t("manifest");
    auto load = [&](const std::string& text) {
        write_text(t.path / "p/package.toml", text);
        auto vfs = assets::Vfs::dev(t.path / "p");
        std::string error;
        auto m = script::load_package_manifest(*vfs, "", error);
        return m ? std::string() : error;
    };
    CHECK(load("name = \"p\"\nversion = \"1.0.0\"\nengine = \">=0.1\"\n").empty());
    CHECK(load("name = \"p\"\nversion = \"1.0.0\"\nengine = \"soon\"\n").find("invalid version range") !=
          std::string::npos);
    CHECK(load("name = \"p\"\nversion = \"1.0.0\"\n[plugin]\nabi = \">=1, <3\"\nlinux = \"x.so\"\n").empty());
    CHECK(load("name = \"p\"\nversion = \"1.0.0\"\n[plugin]\nabi = 1\nplatforms = [\"web\"]\n")
              .find("'platforms' lists") != std::string::npos);

    // A game can't use a package made for another engine version.
    write_text(t.path / "game/project.toml",
               "name = \"g\"\nmain = \"Main\"\n[dependencies.p]\npath = \"../p\"\nversion = \"1.0.0\"\n");
    write_text(t.path / "p/package.toml", "name = \"p\"\nversion = \"1.0.0\"\nengine = \">=99\"\n");
    auto vfs = assets::Vfs::dev(t.path / "game");
    std::string error;
    auto project = script::load_project(*vfs, error);
    REQUIRE_MESSAGE(project.has_value(), error);
    std::vector<script::ResolvedPackage> pkgs;
    CHECK_FALSE(script::resolve_packages(*project, *vfs, pkgs, error));
    CHECK(error.find("needs engine >=99") != std::string::npos);
}

TEST_CASE("registry: script packages publish and install with their dependencies") {
    TempDir t("scripts");
    const fs::path reg = t.path / "registry";
    make_package(t.path / "src/util", "util", "1.0.0", "", "pub fn twice(x: float) -> float { return x * 2.0 }\n");
    make_package(t.path / "src/shapes", "shapes", "2.1.0",
                 "[dependencies.util]\npath = \"../util\"\nversion = \"1.0.0\"\n",
                 "import util\npub fn area(r: float) -> float { return util.twice(r) * r }\n");
    std::string error;
    tools::PublishResult pub;
    // Dependencies are published first.
    CHECK_FALSE(tools::publish_package(t.path / "src/shapes", reg, font, pub, error));
    CHECK(error.find("util 1.0.0 is not in the registry") != std::string::npos);
    REQUIRE_MESSAGE(tools::publish_package(t.path / "src/util", reg, font, pub, error), error);
    REQUIRE_MESSAGE(tools::publish_package(t.path / "src/shapes", reg, font, pub, error), error);
    CHECK_FALSE(pub.already);
    REQUIRE_MESSAGE(tools::publish_package(t.path / "src/shapes", reg, font, pub, error), error);
    CHECK(pub.already); // archives are deterministic

    // The same version with other contents is refused.
    write_text(t.path / "src/util/scripts/extra.plm", "pub fn thrice(x: float) -> float { return x * 3.0 }\n");
    CHECK_FALSE(tools::publish_package(t.path / "src/util", reg, font, pub, error));
    CHECK(error.find("already published with different contents") != std::string::npos);

    auto registry = tools::Registry::open(reg, error);
    REQUIRE_MESSAGE(registry.has_value(), error);
    CHECK(registry->entries().size() == 2);
    const tools::RegistryEntry* shapes = registry->find("shapes", "2.1.0");
    REQUIRE(shapes != nullptr);
    CHECK(shapes->dependencies == std::vector<std::pair<std::string, std::string>>{{"util", "1.0.0"}});

    // Installing into a fresh game brings the dependency too and pins both.
    const fs::path game = fresh_project(t.path, "shapes", "shapes.area(3.0)");
    tools::InstallResult inst;
    REQUIRE_MESSAGE(tools::install_package(game, "shapes@^2", reg, font, inst, error), error);
    CHECK(inst.installed == std::vector<std::string>{"util 1.0.0", "shapes 2.1.0"});
    const std::string toml = read_text(game / "project.toml");
    CHECK(toml.find("[dependencies]\nutil = \"1.0.0\"\nshapes = \"2.1.0\"\n") != std::string::npos);
    CHECK(read_text(game / "packages/shapes/package.toml").find("path") == std::string::npos);
    CHECK(tools::install_package(game, "shapes", reg, font, inst, error)); // already there
    CHECK_FALSE(tools::install_package(game, "shapes@^3", reg, font, inst, error));
    CHECK(error.find("no version of 'shapes' matches") != std::string::npos);
    CHECK_FALSE(tools::install_package(game, "nothing", reg, font, inst, error));

    auto report = tools::license_report(game, error);
    REQUIRE_MESSAGE(report.has_value(), error);
    CHECK(report->find("| shapes | 2.1.0 | MIT | vendored |") != std::string::npos);
    std::vector<fs::path> written;
    REQUIRE_MESSAGE(tools::write_package_notices(game, t.path / "out", written, error), error);
    CHECK(fs::exists(t.path / "out/licenses/PACKAGES.md"));
    CHECK(fs::exists(t.path / "out/licenses/packages/util.txt"));

    // A damaged archive is caught by its checksum.
    const fs::path game2 = fresh_project(t.path / "second", "util", "util.twice(1.0)");
    {
        std::fstream f(reg / "packages/util/util-1.0.0.cpkg", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(40);
        f.put('!');
    }
    CHECK_FALSE(tools::install_package(game2, "util", reg, font, inst, error));
    CHECK(error.find("doesn't match its checksum") != std::string::npos);
    CHECK_FALSE(fs::exists(game2 / "packages/util"));
}

#if CUERVO_TEST_PLUGIN_ORBIT
TEST_CASE("registry: a plugin published to the registry installs into a fresh project and passes the checks") {
    TempDir t("plugin");
    const fs::path reg = t.path / "registry";
    std::string error;
    tools::PublishResult pub;
    REQUIRE_MESSAGE(tools::publish_package(examples / "plugins" / "orbit", reg, font, pub, error), error);
    CHECK(pub.report.ok());
    CHECK(pub.entry.plugin);
    CHECK(pub.entry.sim_safe);
#if defined(_WIN32)
    const char* host_platform = "windows";
    const char* host_lib = "lib/orbit.dll";
#else
    const char* host_platform = "linux";
    const char* host_lib = "lib/liborbit.so";
#endif
    CHECK(pub.entry.platforms == std::vector<std::string>{host_platform}); // only this platform is built here
    REQUIRE(pub.entry.conformance_hash.size() == 16);
    CHECK(pub.entry.files.contains(host_lib));
    CHECK_FALSE(pub.entry.files.contains("src/orbit.cpp")); // sources stay out

    const fs::path game = fresh_project(t.path, "orbit", "orbit.advance(v, 2.0, dt)");
    tools::InstallResult inst;
    REQUIRE_MESSAGE(tools::install_package(game, "orbit", reg, font, inst, error), error);
    REQUIRE(inst.reports.size() == 1);
    const tools::VerifyReport& r = inst.reports[0];
    CHECK(r.ok());
    CHECK(r.conformance_hash == pub.entry.conformance_hash);
    const std::string text = tools::format_verify_report(r);
    CHECK(text.find("matches the published one") != std::string::npos);
    CHECK(text.find("match their checksums") != std::string::npos);
    CHECK(fs::exists(game / "packages/orbit" / host_lib));

    // The game resolves the installed plugin and checks against its natives.
    auto vfs = assets::Vfs::dev(game);
    auto project = script::load_project(*vfs, error);
    REQUIRE_MESSAGE(project.has_value(), error);
    std::vector<script::ResolvedPackage> pkgs;
    REQUIRE_MESSAGE(script::resolve_packages(*project, *vfs, pkgs, error), error);
    REQUIRE(pkgs.size() == 1);
    CHECK(pkgs[0].manifest.plugin.present);

    // A conformance hash that differs from the published one fails verification.
    auto registry = tools::Registry::open(reg, error);
    REQUIRE(registry.has_value());
    tools::RegistryEntry tampered = *registry->find("orbit", "1.0.0");
    tampered.conformance_hash = "0000000000000000";
    tools::VerifyOptions vo;
    vo.font_path = font;
    vo.expected = &tampered;
    auto again = tools::verify_package(game / "packages/orbit", vo, error);
    REQUIRE_MESSAGE(again.has_value(), error);
    CHECK_FALSE(again->ok());
    CHECK(tools::format_verify_report(*again).find("not deterministic here") != std::string::npos);
}
#endif

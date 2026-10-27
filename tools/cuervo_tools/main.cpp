// cuervo-tools: command-line tools for project files (DESIGN.md 3, 4.4, 4.12).
//
//   cuervo-tools scene dump <file.scene>               stable text view on stdout
//   cuervo-tools scene build <file.txt> <file.scene>   text view -> binary
//   cuervo-tools scene upgrade <file.scene> [<out>]    rewrite in the current version
//   cuervo-tools tier check <project> [--to compatible|full] [--platform linux|windows|web]
//   cuervo-tools tier set <project> compatible|full [--force]
//   cuervo-tools pack <project> -o <game.pak> [--platform linux|web|windows] [--strip] [--font <file>]
//   cuervo-tools export <project> --platform linux|web|windows -o <dir> [--strip] [--runtime <path>]
//                [--name <launcher>] [--font <file>] [--engine auto|full|stripped] [--cmake <path>]
//                [--player-cache <dir>] [--no-verify] [--emsdk <dir>] [--host-build <dir>]
//   cuervo-tools pack-info <game.pak>                  list a pack's entries and check its hash
//   cuervo-tools docs [-o <file>] [--check <file>] [--check-coverage]   engine API reference (docs/API.md)
//   cuervo-tools package new <dir> --name <name> [--version 0.1.0]    a Pluma package (docs/PACKAGES.md)
//   cuervo-tools package check <dir>                   check a package on its own
//   cuervo-tools test <project-or-package> [--filter s] run the @test functions (LANGUAGE.md 9.27)
//
// `pack` and `export` are the release path (DESIGN.md 4.11, docs/EXPORT.md):
// the tier check, the importers and a deterministic archive; `export` adds the
// runtime so the output directory runs on its own. By default (`--engine auto`)
// that runtime is a player built with only the features the game uses
// (milestone 7.5, tools/cuervo_tools/player.hpp, docs/EXPORT.md "Stripped players").
//
// `scene dump` is meant as a git textconv driver, so `git diff` shows scene
// changes as text (docs/SCENES.md):
//   .gitattributes:  *.scene diff=scene
//   git config diff.scene.textconv "<build>/tools/cuervo_tools/cuervo-tools scene dump"
//
// `tier check` prints the render-tier migration report (DESIGN.md 4.4,
// cuervo/script/tier_check.hpp): every shader, script, scene and asset use the
// target tier (default: the project's own render_tier) doesn't support, with
// file:line:column. Exit code 0 when nothing blocks the tier, 1 otherwise.
// `--platform` also refuses claims the platform doesn't provide (a Full tier on the web;
// docs/support.json, milestone 8.8); `export` runs the same check for its target.
// `tier set` is the migration itself: it runs the same check and rewrites
// render_tier in project.toml only if there are no errors (or with --force).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/assets/pack.hpp"
#include "cuervo/assets/vfs.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "cuervo/script/tier_check.hpp"
#include "pluma/format.hpp"
#include "pluma/lsp.hpp"
#include "tools/cuervo_tools/api_docs.hpp"
#include "tools/cuervo_tools/export.hpp"
#include "tools/cuervo_tools/packages.hpp"
#include "tools/cuervo_tools/registry.hpp"

namespace {

int usage() {
    std::fputs("usage:\n"
               "  cuervo-tools scene dump <file.scene>\n"
               "  cuervo-tools scene build <file.txt> <file.scene>\n"
               "  cuervo-tools scene upgrade <file.scene> [<out.scene>]\n"
               "  cuervo-tools tier check <project> [--to compatible|full] [--platform linux|windows|web]\n"
               "  cuervo-tools tier set <project> compatible|full [--force]\n"
               "  cuervo-tools pack <project> -o <game.pak> [--platform linux|web|windows] [--scripts source|native]\n"
               "               [--aot] [--strip] [--font <file>]\n"
               "  cuervo-tools export <project> --platform linux|web|windows -o <dir> [--scripts source|native] [--aot]\n"
               "               [--strip] [--runtime <path>] [--name <launcher>] [--font <file>]\n"
               "               [--engine auto|full|stripped] [--cmake <path>] [--player-cache <dir>] [--no-verify]\n"
               "               [--emsdk <dir>] [--host-build <dir>]   (web stripped players)\n"
               "  cuervo-tools pack-info <game.pak>\n"
               "  cuervo-tools fmt <file-or-project> [--check] [--write|-w]\n"
               "  cuervo-tools lsp [<project>] [--check-style|--no-check-style]\n"
               "  cuervo-tools docs [-o <file.md>] [--check <file.md>] [--check-coverage]\n"
               "  cuervo-tools package new <dir> --name <name> [--version <x.y.z>]\n"
               "  cuervo-tools package check <dir>\n"
               "  cuervo-tools test <project-or-package> [--filter <substring>]\n"
               "  cuervo-tools plugin verify <package-dir> [--registry <dir>]\n"
               "  cuervo-tools plugin publish <package-dir> [--registry <dir>]\n"
               "  cuervo-tools plugin install <name>[@<range>] [--project <dir>] [--registry <dir>]\n"
               "  cuervo-tools plugin list [--registry <dir>]\n"
               "  cuervo-tools license report <project>\n"
               "  (--registry defaults to $CUERVO_REGISTRY)\n",
               stderr);
    return 2;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "cuervo-tools: %s\n", message.c_str());
    return 1;
}

int scene_command(int argc, char** argv) {
    using namespace cuervo;
    const std::string_view cmd = argv[2];
    std::string error;
    if (cmd == "dump" && argc == 4) {
        scene::LoadInfo info;
        auto doc = scene::load_file(argv[3], error, &info);
        if (!doc) return fail(error);
        std::string text = scene::dump_text(*doc, &info);
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    if (cmd == "build" && argc == 5) {
        std::ifstream in(argv[3], std::ios::binary);
        if (!in) return fail(std::string("can't open ") + argv[3]);
        std::stringstream ss;
        ss << in.rdbuf();
        auto doc = scene::parse_text(ss.str(), error);
        if (!doc) return fail(std::string(argv[3]) + ": " + error);
        if (!scene::save_file(argv[4], *doc, error)) return fail(error);
        return 0;
    }
    if (cmd == "upgrade" && (argc == 4 || argc == 5)) {
        scene::LoadInfo info;
        auto doc = scene::load_file(argv[3], error, &info);
        if (!doc) return fail(error);
        if (!scene::save_file(argc == 5 ? argv[4] : argv[3], *doc, error)) return fail(error);
        for (const std::string& m : info.migrated) std::fprintf(stderr, "migrated: %s\n", m.c_str());
        return 0;
    }
    return usage();
}

int package_command(int argc, char** argv) {
    const std::string_view cmd = argv[2];
    const std::filesystem::path dir = cuervo::platform::path_from_utf8(argv[3]);
    std::string error;
    if (cmd == "new") {
        std::string name;
        std::string version = "0.1.0";
        for (int i = 4; i + 1 < argc; i += 2) {
            const std::string_view flag = argv[i];
            if (flag == "--name") {
                name = argv[i + 1];
            } else if (flag == "--version") {
                version = argv[i + 1];
            } else {
                return usage();
            }
        }
        if (argc % 2 != 0 || name.empty()) return usage();
        if (!cuervo::tools::new_package(dir, name, version, error)) return fail(error);
        std::printf("created package %s %s in %s\n", name.c_str(), version.c_str(), argv[3]);
        return 0;
    }
    if (cmd == "check" && argc == 4) {
        auto report = cuervo::tools::check_package(dir, error);
        if (!report) return fail(error);
        std::printf("package %s %s: %zu script(s) ok", report->name.c_str(), report->version.c_str(), report->files);
        if (!report->packages.empty()) {
            std::printf(", with");
            for (const std::string& p : report->packages) std::printf(" %s", p.c_str());
        }
        std::printf("\n");
        return 0;
    }
    return usage();
}

int test_command(int argc, char** argv) {
    std::string filter;
    if (argc == 5 && std::string_view(argv[3]) == "--filter") {
        filter = argv[4];
    } else if (argc != 3) {
        return usage();
    }
    std::string error;
    auto report = cuervo::tools::test_scripts(cuervo::platform::path_from_utf8(argv[2]), filter, error);
    if (!report) return fail(error);
    const std::string text = pluma::format_test_report(*report);
    std::fwrite(text.data(), 1, text.size(), stdout);
    return report->ok() ? 0 : 1;
}

// The font conformance projects are packed with, as `pack` uses.
std::string default_font() {
    return cuervo::platform::utf8(
        cuervo::platform::find_asset("fonts/LiberationSans-Regular.ttf", CUERVO_TOOLS_DEFAULT_FONT));
}

// The full player `export` ships for a platform when no --runtime or
// CUERVO_EXPORT_RUNTIME names one (docs/EXPORT.md "Players"): this build
// tree's when it exists (a source checkout), else the export template in a
// prebuilt package, `<package>/templates/<platform>/` beside `bin/` (the
// release archives, .github/workflows/release.yml). Empty when there is none.
std::filesystem::path default_player(cuervo::assets::PackPlatform target) {
    using cuervo::assets::PackPlatform;
    std::error_code ec;
    std::filesystem::path built;
    const char* file = "";
    switch (target) {
        case PackPlatform::Web:
            built = cuervo::platform::path_from_utf8(CUERVO_TOOLS_WEB_RUNTIME); // a directory
            break;
        case PackPlatform::Linux:
            if (cuervo::assets::host_platform() == PackPlatform::Linux) {
                built = cuervo::platform::path_from_utf8(CUERVO_TOOLS_RUNTIME);
            }
            file = "cuervo";
            break;
        case PackPlatform::Windows:
#if defined(CUERVO_TOOLS_PLAYER)
            built = cuervo::platform::path_from_utf8(CUERVO_TOOLS_PLAYER); // the GUI-subsystem player
#endif
            file = "cuervo-player.exe";
            break;
        case PackPlatform::Any: return {};
    }
    if (!built.empty() && std::filesystem::exists(built, ec)) return built;
    const std::filesystem::path templates = cuervo::platform::executable_dir() / ".." / "templates" /
                                            std::string(cuervo::assets::platform_name(target)) / file;
    if (std::filesystem::exists(templates, ec)) return templates.lexically_normal();
    return built;
}

int plugin_command(int argc, char** argv) {
    namespace tools = cuervo::tools;
    using cuervo::platform::path_from_utf8;
    const std::string_view cmd = argv[2];
    std::string target;
    std::string registry;
    std::string project = ".";
    for (int i = 3; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--registry" && i + 1 < argc) {
            registry = argv[++i];
        } else if (a == "--project" && i + 1 < argc) {
            project = argv[++i];
        } else if (!a.starts_with("--") && target.empty()) {
            target = argv[i];
        } else {
            return usage();
        }
    }
    if (registry.empty()) {
        if (const auto env = cuervo::platform::env("CUERVO_REGISTRY")) registry = *env;
    }
    std::string error;
    if (cmd == "verify" && !target.empty()) {
        std::optional<tools::Registry> reg;
        if (!registry.empty()) {
            reg = tools::Registry::open(path_from_utf8(registry), error);
            if (!reg) return fail(error);
        }
        tools::VerifyOptions o;
        o.font_path = default_font();
        // With a registry, compare with the published version of this package.
        auto vfs = cuervo::assets::Vfs::dev(path_from_utf8(target));
        auto m = cuervo::script::load_package_manifest(*vfs, "", error);
        if (!m) return fail(error);
        if (reg) o.expected = reg->find(m->name, m->version);
        auto report = tools::verify_package(path_from_utf8(target), o, error);
        if (!report) return fail(error);
        const std::string text = tools::format_verify_report(*report);
        std::fwrite(text.data(), 1, text.size(), stdout);
        return report->ok() ? 0 : 1;
    }
    if (registry.empty() && (cmd == "publish" || cmd == "install" || cmd == "list")) {
        return fail("no registry: pass --registry <dir> or set CUERVO_REGISTRY");
    }
    if (cmd == "publish" && !target.empty()) {
        tools::PublishResult r;
        if (!tools::publish_package(path_from_utf8(target), path_from_utf8(registry), default_font(), r, error)) {
            return fail(error);
        }
        const std::string text = tools::format_verify_report(r.report);
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::printf("%s %s %s %s (sha256 %s)\n", r.already ? "already published:" : "published", r.entry.name.c_str(),
                    r.entry.version.c_str(), r.entry.archive.c_str(), r.entry.sha256.c_str());
        return 0;
    }
    if (cmd == "install" && !target.empty()) {
        tools::InstallResult r;
        if (!tools::install_package(path_from_utf8(project), target, path_from_utf8(registry), default_font(), r,
                                    error)) {
            return fail(error);
        }
        for (const tools::VerifyReport& v : r.reports) {
            const std::string text = tools::format_verify_report(v);
            std::fwrite(text.data(), 1, text.size(), stdout);
        }
        if (r.installed.empty()) std::printf("%s is already installed\n", target.c_str());
        for (const std::string& s : r.installed) std::printf("installed %s into packages/\n", s.c_str());
        return 0;
    }
    if (cmd == "list" && target.empty()) {
        auto reg = tools::Registry::open(path_from_utf8(registry), error);
        if (!reg) return fail(error);
        for (const tools::RegistryEntry& e : reg->entries()) {
            std::printf("%s %s  %s  %s%s\n", e.name.c_str(), e.version.c_str(), e.license.c_str(),
                        e.plugin ? (e.sim_safe ? "plugin (sim-safe) " : "plugin ") : "", e.description.c_str());
        }
        return 0;
    }
    return usage();
}

int license_command(int argc, char** argv) {
    if (std::string_view(argv[2]) != "report" || argc != 4) return usage();
    std::string error;
    auto report = cuervo::tools::license_report(cuervo::platform::path_from_utf8(argv[3]), error);
    if (!report) return fail(error);
    std::fwrite(report->data(), 1, report->size(), stdout);
    return 0;
}

int tier_command(int argc, char** argv) {
    using namespace cuervo;
    const std::string_view cmd = argv[2];
    const std::string project = argv[3];
    std::string error;
    if (cmd == "check") {
        std::optional<rhi::Tier> target;
        std::optional<script::support::Target> platform;
        for (int i = 4; i < argc; i += 2) {
            const std::string_view flag = argv[i];
            if (i + 1 >= argc) return usage();
            if (flag == "--to") {
                rhi::Tier t{};
                if (!rhi::parse_tier(argv[i + 1], t)) return usage();
                target = t;
            } else if (flag == "--platform") {
                platform = script::support::parse_target(argv[i + 1]);
                if (!platform) return usage();
            } else {
                return usage();
            }
        }
        auto report = script::check_project_tier(project, target, error, script::default_tier_rules(), platform);
        if (!report) return fail(error);
        const std::string text = script::format_tier_report(*report);
        std::fwrite(text.data(), 1, text.size(), stdout);
        return report->ok() ? 0 : 1;
    }
    if (cmd == "set" && (argc == 5 || argc == 6)) {
        rhi::Tier t{};
        if (!rhi::parse_tier(argv[4], t)) return usage();
        const bool force = argc == 6 && std::string_view(argv[5]) == "--force";
        if (argc == 6 && !force) return usage();
        auto report = script::check_project_tier(project, t, error);
        if (!report) return fail(error);
        const std::string text = script::format_tier_report(*report);
        std::fwrite(text.data(), 1, text.size(), stdout);
        if (!report->ok() && !force) {
            return fail("not changing render_tier: fix the errors above or pass --force");
        }
        if (!script::write_project_tier(project, t, error)) return fail(error);
        std::printf("render_tier = \"%s\" written to %s/project.toml\n", std::string(rhi::tier_name(t)).c_str(),
                    project.c_str());
        return 0;
    }
    return usage();
}

// --flag value options after the project argument; false on an unknown flag.
struct Flags {
    std::string out, platform, scripts, font, runtime, name;
    std::string engine, cmake, player_cache, emsdk, host_build;
    bool strip = false;
    bool aot = false;
    bool no_verify = false;
};

bool parse_flags(int argc, char** argv, int first, Flags& f) {
    for (int i = first; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&](std::string& into) {
            if (i + 1 >= argc) return false;
            into = argv[++i];
            return true;
        };
        if (a == "--strip") {
            f.strip = true;
        } else if (a == "--aot") {
            f.aot = true;
        } else if (a == "--no-verify") {
            f.no_verify = true;
        } else if ((a == "-o" || a == "--out") && value(f.out)) {
        } else if (a == "--platform" && value(f.platform)) {
        } else if (a == "--scripts" && value(f.scripts)) {
        } else if (a == "--font" && value(f.font)) {
        } else if (a == "--runtime" && value(f.runtime)) {
        } else if (a == "--name" && value(f.name)) {
        } else if (a == "--engine" && value(f.engine)) {
        } else if (a == "--cmake" && value(f.cmake)) {
        } else if (a == "--player-cache" && value(f.player_cache)) {
        } else if (a == "--emsdk" && value(f.emsdk)) {
        } else if (a == "--host-build" && value(f.host_build)) {
        } else {
            return false;
        }
    }
    return true;
}

bool pack_options(const Flags& f, cuervo::tools::PackOptions& o, std::string& error) {
    using namespace cuervo;
    if (!f.platform.empty()) {
        auto p = assets::parse_platform(f.platform);
        if (!p || *p == assets::PackPlatform::Any) {
            error = "--platform must be linux, web or windows, not '" + f.platform + "'";
            return false;
        }
        o.platform = *p;
    }
    if (f.aot) {
        o.scripts = assets::ScriptForm::Native;
    }
    if (!f.scripts.empty()) {
        auto s = assets::parse_script_form(f.scripts);
        if (!s || (*s != assets::ScriptForm::Source && *s != assets::ScriptForm::Native)) {
            error = "--scripts must be source or native, not '" + f.scripts + "'";
            return false;
        }
        o.scripts = *s;
    }
    o.strip = f.strip;
    o.font_path = f.font.empty()
                      ? platform::utf8(platform::find_asset("fonts/LiberationSans-Regular.ttf", CUERVO_TOOLS_DEFAULT_FONT))
                      : f.font;
    o.engine_version = CUERVO_TOOLS_ENGINE_VERSION;
    return true;
}

void print_summary(const cuervo::tools::PackSummary& s) {
    for (const std::string& n : s.notes) std::printf("  %s\n", n.c_str());
    std::printf("'%s': %zu entries (%zu imported, %zu skipped), %llu bytes, content hash %016llx\n", s.game.c_str(),
                s.files, s.imported, s.skipped, static_cast<unsigned long long>(s.bytes),
                static_cast<unsigned long long>(s.content_hash));
}

int pack_command(int argc, char** argv) {
    using namespace cuervo;
    Flags f;
    if (!parse_flags(argc, argv, 3, f) || f.out.empty() || !f.runtime.empty() || !f.name.empty() ||
        !f.engine.empty() || !f.cmake.empty() || !f.player_cache.empty() || f.no_verify || !f.emsdk.empty() ||
        !f.host_build.empty()) {
        return usage();
    }
    tools::PackOptions o;
    std::string error;
    if (!pack_options(f, o, error)) return fail(error);
    tools::PackSummary summary;
    auto bytes = tools::pack_project(platform::path_from_utf8(argv[2]), o, summary, error);
    if (!bytes) return fail(error);
    std::ofstream out(platform::path_from_utf8(f.out), std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()))) {
        return fail("can't write " + f.out);
    }
    print_summary(summary);
    std::printf("wrote %s (%s)\n", f.out.c_str(), std::string(assets::platform_name(o.platform)).c_str());
    return 0;
}

int export_command(int argc, char** argv) {
    using namespace cuervo;
    Flags f;
    if (!parse_flags(argc, argv, 3, f) || f.out.empty() || f.platform.empty()) return usage();
    tools::ExportOptions o;
    std::string error;
    if (!pack_options(f, o.pack, error)) return fail(error);
    o.out_dir = platform::path_from_utf8(f.out);
    o.name = f.name;
    o.engine = tools::EngineChoice::Auto;
    if (!f.engine.empty()) {
        auto e = tools::parse_engine_choice(f.engine);
        if (!e) return fail("--engine must be auto, full or stripped, not '" + f.engine + "'");
        o.engine = *e;
    }
    // Stripped players are built with this cuervo-tools' own configure
    // (tools/cuervo_tools/CMakeLists.txt): same sources, compilers, generator.
    o.toolchain.source_dir = platform::path_from_utf8(CUERVO_TOOLS_SOURCE_DIR);
    // Shipped as licenses/ with the game: from the source checkout, or from a
    // prebuilt package (THIRD_PARTY_NOTICES.md beside bin/).
    o.notices_root = o.toolchain.source_dir;
    if (std::error_code ec; !std::filesystem::exists(o.notices_root / "THIRD_PARTY_NOTICES.md", ec)) {
        o.notices_root = (platform::executable_dir() / "..").lexically_normal();
    }
    o.toolchain.cmake = platform::path_from_utf8(f.cmake.empty() ? std::string(CUERVO_TOOLS_CMAKE) : f.cmake);
    o.toolchain.generator = CUERVO_TOOLS_GENERATOR;
    o.toolchain.make_program = platform::path_from_utf8(CUERVO_TOOLS_MAKE_PROGRAM);
    o.toolchain.c_compiler = platform::path_from_utf8(CUERVO_TOOLS_C_COMPILER);
    o.toolchain.cxx_compiler = platform::path_from_utf8(CUERVO_TOOLS_CXX_COMPILER);
    o.toolchain.deps_dir = platform::path_from_utf8(CUERVO_TOOLS_DEPS_DIR);
    o.toolchain.engine_version = CUERVO_TOOLS_ENGINE_VERSION;
    if (!f.player_cache.empty()) o.player_cache = platform::path_from_utf8(f.player_cache);
    // Web players: emsdk from --emsdk, $EMSDK or ~/emsdk; the host tools from
    // --host-build or the source tree's build/host-tools (docs/WEB.md).
    if (!f.emsdk.empty()) {
        o.toolchain.emsdk = platform::path_from_utf8(f.emsdk);
    } else if (auto e = platform::env("EMSDK"); e && !e->empty()) {
        o.toolchain.emsdk = platform::path_from_utf8(*e);
    } else if (auto home = platform::env("HOME"); home && !home->empty()) {
        o.toolchain.emsdk = platform::path_from_utf8(*home) / "emsdk";
    }
    o.toolchain.host_build = f.host_build.empty() ? o.toolchain.source_dir / "build" / "host-tools"
                                                  : platform::path_from_utf8(f.host_build);
    o.verify = !f.no_verify;
    o.progress = [](std::string_view m) {
        std::fprintf(stderr, "cuervo-tools: %.*s\n", static_cast<int>(m.size()), m.data());
    };
    if (!f.runtime.empty()) o.runtime = platform::path_from_utf8(f.runtime);
    bool env_runtime = false;
    if (auto env = platform::env(o.pack.platform == assets::PackPlatform::Web ? "CUERVO_EXPORT_WEB_RUNTIME"
                                                                             : "CUERVO_EXPORT_RUNTIME");
        env && !env->empty()) {
        env_runtime = true;
        o.full_runtime = platform::path_from_utf8(*env);
    } else {
        o.full_runtime = default_player(o.pack.platform);
    }
    // The prebuilt player's build type, unless it came from elsewhere.
    const std::string full_label =
        env_runtime || std::string_view(CUERVO_TOOLS_BUILD_TYPE) == "Release" ? "" : CUERVO_TOOLS_BUILD_TYPE;
    tools::ExportSummary summary;
    if (!tools::export_project(platform::path_from_utf8(argv[2]), o, summary, error)) return fail(error);
    print_summary(summary.pack);
    const tools::ShippedPlayer& shipped = summary.player;
    if (shipped.kind == tools::ShippedPlayer::Kind::Full && o.pack.platform != assets::PackPlatform::Web &&
        !full_label.empty()) {
        std::fprintf(stderr,
                     "cuervo-tools: note: shipped the %s runtime of this build tree; export from a Release build, "
                     "set CUERVO_EXPORT_RUNTIME or use --engine stripped for a release\n",
                     full_label.c_str());
    }
    std::printf("player: %s, %llu bytes", std::string(tools::player_kind_name(shipped.kind)).c_str(),
                static_cast<unsigned long long>(shipped.bytes));
    if (shipped.full_bytes && shipped.kind != tools::ShippedPlayer::Kind::Full) {
        // The build type is this build tree's: the web runtime (web-pack preset) is always Release.
        const std::string label = o.pack.platform == assets::PackPlatform::Web ? std::string() : full_label;
        std::printf(" (full prebuilt player%s%s: %llu bytes)", label.empty() ? "" : ", ", label.c_str(),
                    static_cast<unsigned long long>(*shipped.full_bytes));
    }
    if (shipped.verified && shipped.state_hash) {
        std::printf(", verified: state hash %016llx at tick %llu", static_cast<unsigned long long>(*shipped.state_hash),
                    static_cast<unsigned long long>(tools::verify_tick));
        if (!shipped.reference.empty()) std::printf(" (equal to the %s)", shipped.reference.c_str());
    }
    std::printf("\n");
    std::error_code ec;
    for (const auto& p : summary.outputs) {
        std::printf("wrote %s (%llu bytes)\n", platform::utf8(p).c_str(),
                    static_cast<unsigned long long>(std::filesystem::file_size(p, ec)));
    }
    std::printf("run: %s\n", platform::utf8(summary.launcher).c_str());
    return 0;
}

int pack_info_command(int argc, char** argv) {
    using namespace cuervo;
    if (argc != 3) return usage();
    std::string error;
    auto pack = assets::Pack::open(platform::path_from_utf8(argv[2]), error);
    if (!pack) return fail(error);
    std::printf("pack v%u for %s, %zu entries, %zu bytes, content hash %016llx (ok)\n", pack->version(),
                std::string(assets::platform_name(pack->platform())).c_str(), pack->entries().size(),
                pack->size_bytes(), static_cast<unsigned long long>(pack->content_hash()));
    for (const assets::PackEntry& e : pack->entries()) {
        std::printf("%10zu  %.*s\n", e.data.size(), static_cast<int>(e.path.size()), e.path.data());
    }
    return 0;
}

int fmt_command(int argc, char** argv) {
    namespace fs = std::filesystem;
    bool check_only = false;
    bool write_in_place = false;
    std::vector<fs::path> targets;
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--check") {
            check_only = true;
        } else if (arg == "--write" || arg == "-w") {
            write_in_place = true;
        } else if (!arg.empty() && arg[0] == '-') {
            return usage();
        } else {
            targets.push_back(cuervo::platform::path_from_utf8(arg));
        }
    }
    if (targets.empty()) return usage();

    std::vector<fs::path> files;
    bool had_directory = false;
    for (const auto& t : targets) {
        std::error_code ec;
        if (fs::is_directory(t, ec)) {
            had_directory = true;
            for (const auto& entry : fs::recursive_directory_iterator(t, ec)) {
                if (!ec && entry.is_regular_file(ec) && entry.path().extension() == ".plm") {
                    files.push_back(entry.path());
                }
            }
        } else {
            files.push_back(t);
        }
    }
    std::sort(files.begin(), files.end());
    if (had_directory && !check_only) write_in_place = true;

    int status = 0;
    for (const auto& path : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail("can't open " + cuervo::platform::utf8(path));
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();

        pluma::SourceFile sf(cuervo::platform::utf8(path), text);
        pluma::FormatResult res = pluma::format_source(sf);
        if (!res.ok) {
            std::string rendered = pluma::render_all(res.diagnostics, sf);
            std::fwrite(rendered.data(), 1, rendered.size(), stderr);
            status = 1;
            continue;
        }
        if (check_only) {
            if (res.formatted != text) {
                std::fprintf(stderr, "%s: needs formatting\n", cuervo::platform::utf8(path).c_str());
                status = 1;
            }
        } else if (write_in_place) {
            if (res.formatted != text) {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(res.formatted.data(), static_cast<std::streamsize>(res.formatted.size()))) {
                    return fail("can't write " + cuervo::platform::utf8(path));
                }
            }
        } else {
            std::fwrite(res.formatted.data(), 1, res.formatted.size(), stdout);
        }
    }
    return status;
}

int lsp_command(int argc, char** argv) {
    std::string root;
    bool check_style = true;
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--check-style") {
            check_style = true;
        } else if (arg == "--no-check-style") {
            check_style = false;
        } else if (arg == "--stdio" || arg.starts_with("--clientProcessId")) {
            // What language clients append (vscode-languageclient with stdio
            // transport): stdio is the only transport, so nothing to do.
        } else if (!arg.empty() && arg[0] == '-') {
            return usage();
        } else if (root.empty()) {
            root = std::string(arg);
        } else {
            return usage();
        }
    }
    pluma::lsp::Server server(cuervo::script::default_lsp_options(std::move(root), check_style));
    return server.run_stdio(std::cin, std::cout);
}

// The API reference from the prelude this build embeds (tools/cuervo_tools/
// api_docs.hpp). `--check` is for ctest: docs/API.md must match the prelude.
int docs_command(int argc, char** argv) {
    std::string out_path, check_path;
    bool coverage = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--check-coverage") {
            coverage = true;
        } else if ((a == "-o" || a == "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (a == "--check" && i + 1 < argc) {
            check_path = argv[++i];
        } else {
            return usage();
        }
    }
    std::string error;
    if (coverage) {
        // Every public prelude symbol needs a doc comment (DESIGN.md 4.16, 8.4).
        auto cov = cuervo::tools::check_api_coverage(cuervo::script::engine_preludes(), error);
        if (!cov) return fail("the prelude doesn't parse:\n" + error);
        for (const std::string& m : cov->missing) std::fprintf(stderr, "undocumented: %s\n", m.c_str());
        std::printf("prelude doc coverage: %zu of %zu public symbols documented\n", cov->documented, cov->total);
        return cov->missing.empty() ? 0 : 1;
    }
    auto docs = cuervo::tools::generate_api_docs(cuervo::script::engine_preludes(), error);
    if (!docs) return fail("the prelude doesn't parse:\n" + error);
    if (!check_path.empty()) {
        std::ifstream in(cuervo::platform::path_from_utf8(check_path), std::ios::binary);
        if (!in) return fail("can't open " + check_path);
        std::stringstream ss;
        ss << in.rdbuf();
        if (ss.str() != *docs) {
            return fail(check_path + " is out of date with the prelude; regenerate it with `cuervo-tools docs -o " +
                        check_path + "`");
        }
        return 0;
    }
    if (out_path.empty()) {
        std::fwrite(docs->data(), 1, docs->size(), stdout);
        return 0;
    }
    std::ofstream out(cuervo::platform::path_from_utf8(out_path), std::ios::binary | std::ios::trunc);
    if (!out || !out.write(docs->data(), static_cast<std::streamsize>(docs->size()))) {
        return fail("can't write " + out_path);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string_view group = argv[1];
    if (group == "lsp") return lsp_command(argc, argv);
    if (group == "docs") return docs_command(argc, argv);
    if (argc < 3) return usage();
    if (group == "fmt") return fmt_command(argc, argv);
    if (group == "test") return test_command(argc, argv);
    if (group == "plugin") return plugin_command(argc, argv);
    if (group == "license") return license_command(argc, argv);
    if (group == "pack") return pack_command(argc, argv);
    if (group == "export") return export_command(argc, argv);
    if (group == "pack-info") return pack_info_command(argc, argv);
    if (argc < 4) return usage();
    if (group == "scene") return scene_command(argc, argv);
    if (group == "tier") return tier_command(argc, argv);
    if (group == "package") return package_command(argc, argv);
    return usage();
}

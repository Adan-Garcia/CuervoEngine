#include "tools/cuervo_tools/export.hpp"
#include "tools/cuervo_tools/registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <system_error>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/input/devices.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/script/features.hpp"
#include "cuervo/script/game_session.hpp"
#include "cuervo/script/plugin_host.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "cuervo/script/tier_check.hpp"
#include "cuervo/vector/imported_vector.hpp"

namespace cuervo::tools {

namespace fs = std::filesystem;

namespace {

bool read_bytes(const fs::path& file, std::vector<std::byte>& out) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Step 2's filter. Hidden files never ship (.import/ caches are rebuilt by
// step 3, .git and editor state are not game data). Stripping drops what
// only development uses.
enum class Keep { Yes, Hidden, Stripped };
Keep keep(const std::string& rel, bool strip) {
    std::size_t pos = 0;
    bool in_replays = false;
    while (pos <= rel.size()) {
        std::size_t end = rel.find('/', pos);
        if (end == std::string::npos) end = rel.size();
        const std::string_view part = std::string_view(rel).substr(pos, end - pos);
        if (!part.empty() && part.front() == '.') return Keep::Hidden;
        if (part == "replays" && end != rel.size()) in_replays = true;
        pos = end + 1;
    }
    if (!strip) return Keep::Yes;
    const std::string ext = lower(fs::path(rel).extension().string());
    // Replays (docs/REPLAYS.md) and editor command scripts (editor/README.md)
    // are development artifacts.
    if (in_replays || ext == ".replay" || ext == ".cmds") return Keep::Stripped;
    return Keep::Yes;
}

std::string replace_extension(const std::string& rel, std::string_view ext) {
    const std::size_t dot = rel.rfind('.');
    const std::size_t slash = rel.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return rel + std::string(ext);
    return rel.substr(0, dot) + std::string(ext);
}

std::optional<std::vector<std::byte>> import_font_bytes(std::span<const std::byte> data, const std::string& label,
                                                        std::string& error) {
    if (render::is_font_atlas(data)) return std::vector<std::byte>(data.begin(), data.end());
    if (!render::font_import_available()) {
        error = std::format("'{}': this cuervo-tools was built without font import (CUERVO_FONT_IMPORT)", label);
        return std::nullopt;
    }
    auto font = render::load_font_data(data, label);
    if (!font) {
        error = std::format("'{}': font import failed", label);
        return std::nullopt;
    }
    return render::serialize_font_atlas(*font);
}

bool copy_file(const fs::path& from, const fs::path& to, bool executable, std::string& error) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = std::format("can't copy '{}' to '{}': {}", platform::utf8(from), platform::utf8(to), ec.message());
        return false;
    }
    if (executable) {
        fs::permissions(to,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec,
                        fs::perm_options::replace, ec);
    }
    return true;
}

} // namespace

std::string launcher_name(std::string_view game) {
    std::string out;
    for (char c : game) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0) {
            out += static_cast<char>(std::tolower(u));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string("game") : out;
}

std::optional<std::vector<std::byte>> pack_project(const fs::path& project, const PackOptions& options,
                                                   PackSummary& summary, std::string& error) {
    if (options.scripts == assets::ScriptForm::Bytecode) {
        error = "packing scripts as 'bytecode' is not supported (use 'source' or 'native')";
        return std::nullopt;
    }
    if (options.scripts == assets::ScriptForm::Native && options.platform != assets::PackPlatform::Any &&
        options.platform != assets::host_platform()) {
        error = std::format("native AOT script compilation is only supported for the host platform ({}), not {}",
                            assets::platform_name(assets::host_platform()), assets::platform_name(options.platform));
        return std::nullopt;
    }

    // 1. check
    auto proj = script::load_project(project, error);
    if (!proj) return std::nullopt;
    summary.game = proj->name;
    if (options.tier_gate) {
        // The project's own tier is checked, and the claims it makes (a Full
        // tier on the web, native scripts when the host is not the target) must be ones the target
        // provides per docs/support.json (milestone 8.8, DESIGN.md 4.4).
        std::optional<script::support::Target> platform;
        switch (options.platform) {
            case assets::PackPlatform::Linux: platform = script::support::Target::Linux; break;
            case assets::PackPlatform::Windows: platform = script::support::Target::Windows; break;
            case assets::PackPlatform::Web: platform = script::support::Target::Web; break;
            case assets::PackPlatform::Any: break;
        }
        auto report = script::check_project_tier(project, std::nullopt, error, script::default_tier_rules(), platform,
                                                 options.scripts == assets::ScriptForm::Native);
        if (!report) return std::nullopt;
        if (!report->ok()) {
            error = "the tier check failed (cuervo-tools tier check):\n" + script::format_tier_report(*report);
            return std::nullopt;
        }
        summary.notes.push_back(std::format("tier check: {} for {} ({} warning(s))", rhi::tier_name(report->target),
                                            assets::platform_name(options.platform),
                                            report->count(script::Severity::Warning)));
    }

    // Packages and plugins (LANGUAGE.md 9.26, docs/PLUGINS.md). Plugin
    // importers convert their sources in step 3 like the engine's own; the
    // plugins load on this machine, whatever the target platform.
    std::vector<script::ResolvedPackage> packages;
    std::shared_ptr<script::PluginSet> plugins;
    {
        auto dev_vfs = assets::Vfs::dev(project);
        auto with_deps = script::load_project(*dev_vfs, error);
        if (!with_deps) return std::nullopt;
        if (!script::resolve_packages(*with_deps, *dev_vfs, packages, error)) return std::nullopt;
        bool any_plugin = false;
        for (const script::ResolvedPackage& pkg : packages) {
            if (!pkg.manifest.plugin.present) continue;
            any_plugin = true;
            if (options.platform == assets::PackPlatform::Web) {
                error = std::format("plugin '{}': native plugins are not available on the web yet (docs/PLUGINS.md)",
                                    pkg.manifest.name);
                return std::nullopt;
            }
        }
        if (any_plugin) {
            plugins = script::PluginSet::load(packages, error);
            if (!plugins && options.scripts == assets::ScriptForm::Native) return std::nullopt;
            if (!plugins) {
                // A source pack runs the plugins on the player; only their importers are lost.
                summary.notes.push_back(std::format("plugins: importers not run ({})", error));
                error.clear();
            }
        }
    }

    // 2. collect (sorted, so the import log reads the same every time)
    std::vector<std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(project, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) files.push_back(platform::utf8(it->path().lexically_relative(project)));
    }
    if (ec) {
        error = std::format("can't list '{}': {}", platform::utf8(project), ec.message());
        return std::nullopt;
    }
    std::sort(files.begin(), files.end(), assets::path_component_less);

    assets::PackBuilder pack(options.platform);
    for (const std::string& rel : files) {
        const Keep k = keep(rel, options.strip);
        if (k != Keep::Yes) {
            ++summary.skipped;
            if (k == Keep::Stripped) summary.notes.push_back(std::format("strip: {}", rel));
            continue;
        }
        const std::string ext = lower(fs::path(rel).extension().string());
        if (options.scripts == assets::ScriptForm::Native && ext == ".plm") {
            // 4. scripts (Native): .plm sources are compiled into .cuervo/scripts.so
            // and omitted from the shipped pack.
            ++summary.skipped;
            summary.notes.push_back(std::format("aot: {} -> {}", rel, assets::native_scripts_entry));
            continue;
        }
        std::vector<std::byte> data;
        if (!read_bytes(project / platform::path_from_utf8(rel), data)) {
            error = std::format("can't read '{}'", platform::utf8(project / platform::path_from_utf8(rel)));
            return std::nullopt;
        }
        // 3. import: the engine format replaces the source in the pack.
        std::string path = rel;
        if (ext == ".ttf" || ext == ".otf") {
            auto efont = import_font_bytes(data, rel, error);
            if (!efont) return std::nullopt;
            path = replace_extension(rel, ".efont");
            data = std::move(*efont);
            ++summary.imported;
            summary.notes.push_back(std::format("import: {} -> {}", rel, path));
        } else if (ext == ".svg") {
            const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
            auto vec = vector::import_svg(text, rel);
            if (!vec) {
                error = std::format("'{}': SVG import failed", rel);
                return std::nullopt;
            }
            path = replace_extension(rel, ".evec");
            data = vector::serialize_imported_vector(*vec);
            ++summary.imported;
            summary.notes.push_back(std::format("import: {} -> {}", rel, path));
        } else if (const script::PluginImporter* imp = plugins ? plugins->importer_for(rel) : nullptr) {
            std::vector<std::byte> out;
            if (!script::PluginSet::run_importer(*imp, rel, data, out, error)) return std::nullopt;
            path = rel.substr(0, rel.size() - imp->extension.size()) + imp->output_extension;
            data = std::move(out);
            ++summary.imported;
            summary.notes.push_back(std::format("import: {} -> {} (plugin {})", rel, path, imp->plugin));
        }
        if (pack.contains(path)) {
            // e.g. both font.ttf and font.efont: the hand-made engine file wins.
            summary.notes.push_back(std::format("skip: {} (already packed as {})", rel, path));
            ++summary.skipped;
            continue;
        }
        if (!pack.add(path, std::move(data), error)) return std::nullopt;
    }

    // 3b. packages from other directories (`path` dependencies, LANGUAGE.md
    // 9.26) go under packages/<name>/, where a packed game finds them, with
    // a plugin's library for the target (milestone 10.5c, docs/PLUGINS.md).
    // Vendored packages are already inside the project.
    {
        for (const script::ResolvedPackage& pkg : packages) {
            const script::PluginSpec& plugin = pkg.manifest.plugin;
            if (!pkg.prefix.empty()) continue; // vendored
            const std::string dest = std::format("packages/{}/", pkg.manifest.name);
            std::vector<std::string> entries{"package.toml"};
            for (const std::string& f : pkg.vfs->list("scripts", ".plm")) entries.push_back(f);
            for (const std::string& f : {std::string("LICENSE"), std::string("LICENSE.md")}) {
                if (pkg.vfs->exists(f)) entries.push_back(f);
            }
            if (plugin.present) {
                const std::string& lib = options.platform == assets::PackPlatform::Windows ? plugin.lib_windows
                                                                                           : plugin.lib_linux;
                if (!lib.empty()) {
                    entries.push_back(lib);
                } else if (!plugin.dotnet.empty()) {
                    // A .NET assembly ships with its directory (deps, runtimeconfig, bindings).
                    const std::string dir = fs::path(plugin.dotnet).parent_path().generic_string();
                    for (const std::string& f : pkg.vfs->list(dir)) {
                        if (!f.ends_with(".pdb")) entries.push_back(f);
                    }
                }
            }
            for (const std::string& rel : entries) {
                if (options.scripts == assets::ScriptForm::Native && rel.ends_with(".plm")) continue;
                std::vector<std::byte> data;
                const fs::path src = pkg.dir / platform::path_from_utf8(rel);
                if (!read_bytes(src, data)) {
                    error = std::format("can't read '{}'", platform::utf8(src));
                    return std::nullopt;
                }
                if (pack.contains(dest + rel)) continue; // a vendored copy wins
                if (!pack.add(dest + rel, std::move(data), error)) return std::nullopt;
            }
            summary.notes.push_back(std::format("package: {} {} ({}) -> {}", pkg.manifest.name, pkg.manifest.version,
                                                platform::utf8(pkg.dir), dest));
        }
    }

    // 4. scripts: ScriptForm::Source (the .plm files collected above) or
    // ScriptForm::Native (ahead-of-time C compilation into .cuervo/scripts.so,
    // DESIGN.md 5.12).
    const assets::ScriptForm scripts = options.scripts;
    if (scripts == assets::ScriptForm::Native) {
        auto dev_vfs = assets::Vfs::dev(project);
        std::vector<pluma::SourceFile::Part> sources;
        if (!script::read_scripts(*dev_vfs, sources, nullptr, error)) return std::nullopt;
        // Plugins: their natives for the checker, and the static libraries
        // linked into the module (DESIGN.md 4.18).
        auto statics = script::static_plugins(packages, error);
        if (!statics) return std::nullopt;
        if (!plugins) plugins = std::make_shared<script::PluginSet>();
        for (const script::StaticPlugin& p : *statics) {
            summary.notes.push_back(std::format("aot: links plugin {} {} ({})", p.name, p.version,
                                                platform::utf8(p.library)));
        }
        auto so = script::compile_aot_scripts(sources, error, 2, plugins, *statics);
        if (!so) return std::nullopt;
        ++summary.imported;
        summary.notes.push_back(
            std::format("aot: compiled {} script(s) -> {} ({} bytes)", sources.size(), assets::native_scripts_entry,
                        so->size()));
        if (!pack.add(std::string(assets::native_scripts_entry), std::move(*so), error)) return std::nullopt;
    }

    // 5. engine entries
    if (!options.font_path.empty()) {
        std::vector<std::byte> data;
        if (!read_bytes(options.font_path, data)) {
            error = std::format("can't read the game font '{}'", options.font_path);
            return std::nullopt;
        }
        auto efont = import_font_bytes(data, options.font_path, error);
        if (!efont) return std::nullopt;
        if (!pack.add(std::string(assets::default_font_entry), std::move(*efont), error)) return std::nullopt;
    }
    auto dev_vfs_feat = assets::Vfs::dev(project);
    script::FeatureSet detected_features = script::detect_features(*dev_vfs_feat, *proj, proj->features);

    std::string feat_active;
    for (const auto& f : detected_features.active_list()) {
        if (!feat_active.empty()) feat_active += ", ";
        feat_active += f;
    }
    std::string feat_stripped;
    for (const auto& f : detected_features.stripped_list()) {
        if (!feat_stripped.empty()) feat_stripped += ", ";
        feat_stripped += f;
    }
    summary.notes.push_back(std::format("features active: [{}]", feat_active));
    if (!feat_stripped.empty()) {
        summary.notes.push_back(std::format("features stripped: [{}]", feat_stripped));
    }

    assets::ExportManifest manifest;
    manifest.game = proj->name;
    manifest.engine = options.engine_version;
    manifest.platform = options.platform;
    manifest.scripts = scripts;
    manifest.stripped = options.strip;
    summary.features = detected_features;
    manifest.features = detected_features.active_list();
    manifest.stripped_features = detected_features.stripped_list();
    const std::string text = assets::write_manifest(manifest);
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    if (!pack.add(std::string(assets::manifest_path), std::vector<std::byte>(begin, begin + text.size()), error)) {
        return std::nullopt;
    }

    // 6. pack, then read it back the way the runtime will.
    std::vector<std::byte> bytes = pack.build();
    auto check = assets::Pack::from_bytes(bytes, error);
    if (!check) return std::nullopt;
    auto vfs = assets::Vfs::release(check, {});
    if (!script::load_project(*vfs, error)) return std::nullopt;
    if (scripts == assets::ScriptForm::Native) {
        if (!vfs->exists(assets::native_scripts_entry)) {
            error = std::format("packed archive is missing '{}'", assets::native_scripts_entry);
            return std::nullopt;
        }
    } else {
        std::vector<pluma::SourceFile::Part> sources;
        if (!script::read_scripts(*vfs, sources, nullptr, error)) return std::nullopt;
    }
    summary.files = check->entries().size();
    summary.bytes = bytes.size();
    summary.content_hash = check->content_hash();
    return bytes;
}

namespace {

bool check_runtime(assets::PackPlatform target, const fs::path& runtime, std::string& error) {
    std::error_code ec;
    if (target == assets::PackPlatform::Web) {
        for (const char* f : {"index.html", "index.js", "index.wasm"}) {
            if (!fs::is_regular_file(runtime / f, ec)) {
                error = std::format("'{}' has no {}: build the pack-loading web runtime first (cmake --preset "
                                    "web-pack && cmake --build --preset web-pack; docs/EXPORT.md \"Web\")",
                                    platform::utf8(runtime), f);
                return false;
            }
        }
        // A web runtime built around a raw project (CUERVO_WEB_GAME) would
        // ignore game.pak; the pack-loading build has the loader.
        std::ifstream js(runtime / "index.js", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(js)), std::istreambuf_iterator<char>());
        if (text.find("cuervo-pack") == std::string::npos) {
            error = std::format("'{}' is not a pack-loading web runtime (configure the web build with "
                                "-DCUERVO_WEB_PACK=fetch, or use the web-pack preset)",
                                platform::utf8(runtime));
            return false;
        }
    } else if (!fs::is_regular_file(runtime, ec)) {
        error = std::format("runtime '{}' not found", platform::utf8(runtime));
        return false;
    }
    return true;
}

// The size a player adds to a download: the executable, or the web build's .wasm.
std::optional<std::uint64_t> player_bytes(assets::PackPlatform target, const fs::path& runtime) {
    std::error_code ec;
    const fs::path file = target == assets::PackPlatform::Web ? runtime / "index.wasm" : runtime;
    const auto size = fs::file_size(file, ec);
    if (ec) return std::nullopt;
    return static_cast<std::uint64_t>(size);
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& s : items) {
        if (!out.empty()) out += ", ";
        out += s;
    }
    return out;
}

} // namespace

std::string_view player_kind_name(ShippedPlayer::Kind kind) {
    switch (kind) {
    case ShippedPlayer::Kind::Explicit: return "explicit";
    case ShippedPlayer::Kind::Full: return "full";
    case ShippedPlayer::Kind::Stripped: return "stripped";
    }
    return "full";
}

std::optional<std::uint64_t> reference_state_hash(const fs::path& pack, std::string& error) {
    std::error_code ec;
    const fs::path user = fs::temp_directory_path(ec) / std::format("cuervo-verify-ref-{}", launcher_name(
                                                                                               platform::utf8(pack)));
    fs::remove_all(user, ec);
    script::SessionOptions o;
    o.pack = pack;
    o.user_dir = user;
    o.jobs = 1;
    script::GameSession session;
    if (!session.init(o, error)) return std::nullopt;
    if (session.host().panicked()) {
        error = "the game panicked while starting";
        return std::nullopt;
    }
    DeviceState devices;
    for (std::uint64_t i = 0; i < verify_tick; ++i) session.tick(session.sample(devices, {0.0f, 0.0f}));
    fs::remove_all(user, ec);
    if (session.host().panicked()) {
        error = "the game panicked";
        return std::nullopt;
    }
    return session.host().state_hash();
}

bool script_surface_matches(const script::FeatureSet& f) {
    // This process's engine (cuervo_export links the configured cuervo library).
    constexpr bool audio = CUERVO_FEATURE_AUDIO != 0;
    constexpr bool ui = CUERVO_FEATURE_UI != 0;
    constexpr bool particles = CUERVO_FEATURE_PARTICLES != 0;
    constexpr bool collision = CUERVO_FEATURE_COLLISION != 0;
    constexpr bool physics = CUERVO_FEATURE_PHYSICS != 0;
    return f.audio == audio && f.ui == ui && f.particles == particles && f.collision == collision &&
           f.physics == physics;
}

std::optional<std::uint64_t> run_player_headless(const fs::path& launcher, const std::vector<std::string>& args,
                                                 bool from_pack, std::string& error) {
    std::error_code ec;
    const fs::path work = fs::temp_directory_path(ec) /
                          std::format("cuervo-verify-{}{}", launcher_name(platform::utf8(launcher)), from_pack ? "" : "-dev");
    fs::remove_all(work, ec);
    fs::create_directories(work / "user", ec);
    if (ec) {
        error = std::format("can't create '{}': {}", platform::utf8(work), ec.message());
        return std::nullopt;
    }
    // Headless the way CI runs the shmup export (tests/export_shmup.cmake):
    // offscreen SDL works for GL only (and not on Windows, which has no EGL
    // for it: a window opens there), and a private user directory keeps real
    // saves out. `--screenshot` simulates exactly verify_tick ticks.
    std::vector<std::pair<std::string, std::string>> env = {
        {"CUERVO_USER_DIR", platform::utf8(work / "user")},
        {"CUERVO_SCREENSHOT_TICK", std::to_string(verify_tick)},
    };
    if constexpr (platform::os != platform::Os::Windows) env.emplace_back("SDL_VIDEO_DRIVER", "offscreen");
    std::vector<std::string> argv = {platform::utf8(fs::absolute(launcher, ec))};
    argv.insert(argv.end(), args.begin(), args.end());
    for (const char* a : {"--backend", "gl", "--no-audio", "--screenshot"}) argv.emplace_back(a);
    argv.push_back(platform::utf8(work / "verify.ppm"));
    auto run = run_process(argv, work, env, work / "verify.log", std::chrono::seconds(300), error);
    fs::remove_all(work, ec);
    if (!run) return std::nullopt;
    const std::string& log = run->output;
    auto fail = [&](std::string_view why) -> std::optional<std::uint64_t> {
        error = std::format("the stripped player failed to run the {}: {}\n{}", from_pack ? "exported game" : "project",
                            why, log);
        return std::nullopt;
    };
    if (run->timed_out) return fail("timed out");
    if (run->exit_code != 0) return fail(std::format("exit code {}", run->exit_code));
    if (from_pack && log.find("Pack '") == std::string::npos) return fail("it did not run from game.pak");
    // A script compile error (e.g. a feature detection missed, so a prelude
    // the game names is not in this player) is logged as an error or panic.
    if (log.find("[error]") != std::string::npos || log.find("panic") != std::string::npos) {
        return fail("errors in the log");
    }
    const std::size_t at = log.rfind("state hash ");
    if (at == std::string::npos || at + 11 + 16 > log.size()) return fail("no state hash in the log");
    const std::string hex = log.substr(at + 11, 16);
    char* end = nullptr;
    const std::uint64_t hash = std::strtoull(hex.c_str(), &end, 16);
    if (end != hex.c_str() + hex.size()) return fail("unreadable state hash");
    return hash;
}

namespace {

// The web counterpart of run_player_headless: the page in `page_dir` (the
// exported dist/) in headless Chrome through tools/web/smoke_test.mjs, which
// serves it, runs `--screenshot` at verify_tick and echoes the log.
std::optional<std::uint64_t> run_web_player_headless(const fs::path& page_dir, const PlayerToolchain& t,
                                                     std::string& error) {
    std::error_code ec;
    const fs::path work = fs::temp_directory_path(ec) / "cuervo-verify-web";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const fs::path script = t.source_dir / "tools" / "web" / "smoke_test.mjs";
    // --seconds sets the smoke test's own timeout (seconds + 60): SwiftShader
    // needs about 66 s for examples/bench's 240 ticks, so allow what
    // run_process allows.
    const std::vector<std::string> argv = {"node", platform::utf8(script), "--dir", platform::utf8(fs::absolute(page_dir, ec)),
                                           "--seconds", "240", "--args", "--screenshot /tmp/verify.ppm",
                                           "--expect", "state hash"};
    auto run = run_process(argv, work, web_build_environment(t.emsdk), work / "verify.log", std::chrono::seconds(300),
                           error);
    fs::remove_all(work, ec);
    if (!run) return std::nullopt;
    const std::string& log = run->output;
    auto fail = [&](std::string_view why) -> std::optional<std::uint64_t> {
        error = std::format("the stripped web player failed to run the exported game: {}\n{}", why, log);
        return std::nullopt;
    };
    if (run->exit_code == 77) return fail("no Chrome/Chromium to run it in (install one, or pass --no-verify)");
    if (run->timed_out) return fail("timed out");
    if (run->exit_code != 0) return fail(std::format("exit code {}", run->exit_code));
    const std::size_t at = log.rfind("state hash ");
    if (at == std::string::npos || at + 11 + 16 > log.size()) return fail("no state hash in the log");
    const std::string hex = log.substr(at + 11, 16);
    char* end = nullptr;
    const std::uint64_t hash = std::strtoull(hex.c_str(), &end, 16);
    if (end != hex.c_str() + hex.size()) return fail("unreadable state hash");
    return hash;
}

// Whether the dev run of a stripped player can load `project` as is: it has
// no runtime importers, so sources that export converts (SVG, TTF/OTF) would
// be errors there.
bool needs_import(const fs::path& project) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(project, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = platform::utf8(it->path().lexically_relative(project));
        if (keep(rel, false) != Keep::Yes) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext == ".svg" || ext == ".ttf" || ext == ".otf") return true;
    }
    return false;
}

// The stripped player's dev run (`--game`) of a copy of the project (a dev
// run saves into its project), with the pack's imported font so text
// layout matches the pack run.
std::optional<std::uint64_t> dev_run_hash(const fs::path& project, const fs::path& pak, const fs::path& exe,
                                          std::string& error) {
    std::error_code ec;
    const fs::path work = fs::temp_directory_path(ec) / std::format("cuervo-verify-dev-{}",
                                                                    launcher_name(platform::utf8(exe)));
    fs::remove_all(work, ec);
    fs::create_directories(work / "project", ec);
    for (fs::recursive_directory_iterator it(project, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string rel = platform::utf8(it->path().lexically_relative(project));
        if (keep(rel, false) != Keep::Yes) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const fs::path to = work / "project" / it->path().lexically_relative(project);
        if (it->is_directory(ec)) {
            fs::create_directories(to, ec);
        } else if (it->is_regular_file(ec)) {
            fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) break;
    }
    std::vector<std::string> args = {"--game", platform::utf8(work / "project")};
    if (auto pack = assets::Pack::open(pak, error)) {
        if (auto font = pack->find(assets::default_font_entry)) {
            std::ofstream(work / "default.efont", std::ios::binary)
                .write(reinterpret_cast<const char*>(font->data()), static_cast<std::streamsize>(font->size()));
            args.push_back("--font");
            args.push_back(platform::utf8(work / "default.efont"));
        }
    }
    if (ec) {
        error = std::format("can't copy the project for the dev run: {}", ec.message());
        fs::remove_all(work, ec);
        return std::nullopt;
    }
    auto hash = run_player_headless(exe, args, false, error);
    fs::remove_all(work, ec);
    return hash;
}

// The pack run must be clean, and its state hash must equal a reference: the
// full engine in this process (a stripped player's hash doesn't depend on its
// features, ScriptHost::state_hash), else, when this engine can't run the
// pack, the same player's dev run of the project, as tests/export_shmup.cmake
// compares the full player's export with the dev run.
bool verify_stripped(const fs::path& project, const fs::path& pak, const fs::path& exe, ShippedPlayer& player,
                     std::vector<std::string>& notes, std::string& error) {
    auto hash = run_player_headless(exe, {}, true, error);
    if (!hash) return false;
    player.state_hash = hash;
    std::string why_none;
    std::string ref_error;
    player.reference_hash = reference_state_hash(pak, ref_error);
    if (player.reference_hash) {
        player.reference = "full engine";
    } else if (!needs_import(project)) {
        player.reference_hash = dev_run_hash(project, pak, exe, error);
        if (!player.reference_hash) return false;
        player.reference = "dev run";
    } else {
        why_none = "the full engine can't run it here (" + ref_error + "), and the project needs importers for a dev run";
    }
    if (player.reference_hash && *player.reference_hash != *hash) {
        error = std::format("the stripped player reaches state hash {:016x} at tick {} on the exported game, the {} "
                            "reaches {:016x}",
                            *hash, verify_tick, player.reference, *player.reference_hash);
        return false;
    }
    player.verified = true;
    if (player.reference_hash) {
        notes.push_back(std::format("verify: the stripped player reaches state hash {:016x} at tick {}, equal to the {}",
                                    *hash, verify_tick, player.reference));
    } else {
        notes.push_back(std::format("verify: the stripped player runs the game cleanly (state hash {:016x} at tick {}); "
                                    "not compared: {}",
                                    *hash, verify_tick, why_none));
    }
    return true;
}

// A player built against the MSVC runtime imports the Visual C++ runtime DLLs,
// which a player's machine may lack; Microsoft allows shipping them next to the
// program (app-local). They come from beside the player (the export templates
// carry them) or, on a Windows host, from the system directory. An import name
// is plain ASCII in the PE import table, so a byte search finds it.
bool copy_msvc_runtime(const fs::path& player, const fs::path& out_dir, std::vector<fs::path>& written,
                       std::vector<std::string>& notes, std::string& error) {
    std::ifstream in(player, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::transform(bytes.begin(), bytes.end(), bytes.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::error_code ec;
    for (const char* dll : {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"}) {
        if (bytes.find(dll) == std::string::npos) continue;
        fs::path from = player.parent_path() / dll;
        if (!fs::is_regular_file(from, ec)) {
            from.clear();
            if (auto root = platform::env("SystemRoot"); root && !root->empty()) {
                const fs::path sys = platform::path_from_utf8(*root) / "System32" / dll;
                if (fs::is_regular_file(sys, ec)) from = sys;
            }
        }
        if (from.empty()) {
            notes.push_back(std::format("player: it needs {} (the Visual C++ runtime) and none was found to ship; "
                                        "machines without the runtime installed can't start the game",
                                        dll));
            continue;
        }
        if (!copy_file(from, out_dir / dll, false, error)) return false;
        written.push_back(out_dir / dll);
    }
    return true;
}

} // namespace

bool export_project(const fs::path& project, const ExportOptions& options, ExportSummary& summary,
                    std::string& error) {
    const assets::PackPlatform target = options.pack.platform;
    if (target == assets::PackPlatform::Any) {
        error = "export needs a platform (linux, web or windows)";
        return false;
    }

    // Which player ships (docs/EXPORT.md "Stripped players"): an explicit
    // --runtime wins; otherwise Full ships the prebuilt player and Stripped
    // builds one. Auto builds one only where it is possible and useful.
    ShippedPlayer& player = summary.player;
    std::string why_full;
    if (!options.runtime.empty()) {
        player.kind = ShippedPlayer::Kind::Explicit;
        player.source = options.runtime;
    } else if (options.engine == EngineChoice::Full) {
        player.kind = ShippedPlayer::Kind::Full;
        player.source = options.full_runtime;
    } else if (options.engine == EngineChoice::Stripped) {
        player.kind = ShippedPlayer::Kind::Stripped;
    } else if (std::string missing = options.toolchain.missing(target); !missing.empty()) {
        why_full = "no toolchain to build a stripped player (" + missing + "), shipping the full player";
    } else {
        player.kind = ShippedPlayer::Kind::Stripped;
    }
    if (!why_full.empty()) {
        player.kind = ShippedPlayer::Kind::Full;
        player.source = options.full_runtime;
    }
    if (player.kind != ShippedPlayer::Kind::Stripped) {
        if (player.source.empty()) {
            error = target == assets::PackPlatform::Windows
                        ? "no Windows player to ship: export from a Windows build or a prebuilt package (its "
                          "templates/windows/cuervo-player.exe), or pass --runtime <cuervo-player.exe> "
                          "(docs/EXPORT.md \"Windows\")"
                        : "no runtime to ship (--runtime)";
            return false;
        }
        if (!check_runtime(target, player.source, error)) return false;
    }

    auto bytes = pack_project(project, options.pack, summary.pack, error);
    if (!bytes) return false;
    if (!why_full.empty()) summary.pack.notes.push_back("player: " + why_full);
    const script::FeatureSet& features = summary.pack.features;
    if (player.kind == ShippedPlayer::Kind::Stripped && options.engine == EngineChoice::Auto && !can_strip(features)) {
        // The project uses everything the prebuilt player has.
        player.kind = ShippedPlayer::Kind::Full;
        player.source = options.full_runtime;
        summary.pack.notes.push_back("player: the game needs every feature, shipping the full player");
        if (!check_runtime(target, player.source, error)) return false;
    }
    if (player.kind == ShippedPlayer::Kind::Stripped && options.pack.scripts == assets::ScriptForm::Native &&
        !script_surface_matches(features)) {
        // The AOT module carries this engine's prelude (script_surface_matches);
        // a player without some of it would run a module built for another engine.
        std::vector<std::string> missing;
        for (const char* f : {"audio", "collision", "particles", "physics", "ui"}) {
            if (!features.enabled(f)) missing.emplace_back(f);
        }
        const std::string why = std::format(
            "AOT scripts are compiled against this engine's script prelude, so a player without [{}] can't run "
            "them yet",
            join(missing));
        if (options.engine == EngineChoice::Stripped) {
            error = "--engine stripped with --aot: " + why + " (use source scripts, or --engine full)";
            return false;
        }
        player.kind = ShippedPlayer::Kind::Full;
        player.source = options.full_runtime;
        summary.pack.notes.push_back("player: " + why + "; shipping the full player");
        if (!check_runtime(target, player.source, error)) return false;
    }
    if (player.kind == ShippedPlayer::Kind::Stripped) {
        const fs::path cache = options.player_cache.empty() ? default_player_cache_dir() : options.player_cache;
        auto built = build_player(features, options.toolchain, cache, target, options.progress, error);
        if (!built) return false;
        player.source = built->executable;
        player.cache_key = built->key;
        player.configured = built->configured;
        summary.pack.notes.push_back(std::format("player: stripped build {} ({}; features [{}], stripped [{}])",
                                                 built->key, built->configured ? "configured" : "cached",
                                                 join(features.active_list()), join(features.stripped_list())));
    }
    if (auto b = player_bytes(target, player.source)) player.bytes = *b;
    if (!options.full_runtime.empty()) player.full_bytes = player_bytes(target, options.full_runtime);

    std::error_code ec;
    fs::create_directories(options.out_dir, ec);
    if (ec) {
        error = std::format("can't create '{}': {}", platform::utf8(options.out_dir), ec.message());
        return false;
    }
    const fs::path pak = options.out_dir / "game.pak";
    {
        std::ofstream out(pak, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()))) {
            error = std::format("can't write '{}'", platform::utf8(pak));
            return false;
        }
    }
    summary.outputs.push_back(pak);

    if (!options.notices_root.empty() &&
        !copy_third_party_notices(options.notices_root, options.out_dir, summary.outputs, error)) {
        return false;
    }
    // The packages' licenses next to the engine's (milestone 10.5d).
    if (!write_package_notices(project, options.out_dir, summary.outputs, error)) return false;

    const std::string name = options.name.empty() ? launcher_name(summary.pack.game) : options.name;
    if (target == assets::PackPlatform::Web) {
        // The page fetches game.pak next to itself into MEMFS before main()
        // (web/pack_loader.js); the runtime then runs `--pack /game.pak`.
        for (const char* f : {"index.html", "index.js", "index.wasm"}) {
            if (!copy_file(player.source / f, options.out_dir / f, false, error)) return false;
            summary.outputs.push_back(options.out_dir / f);
        }
        summary.launcher = options.out_dir / "index.html";
        if (player.kind == ShippedPlayer::Kind::Stripped && options.verify) {
            // The same check as a native player, in a browser: the page must
            // run this pack and match the full engine (the sim is the same
            // code on every platform, the hash the same for every feature set).
            if (options.progress) options.progress("verifying the stripped web player in headless Chrome");
            auto hash = run_web_player_headless(options.out_dir, options.toolchain, error);
            if (!hash) {
                for (const char* f : {"index.html", "index.js", "index.wasm"}) fs::remove(options.out_dir / f, ec);
                return false;
            }
            player.state_hash = hash;
            {
                std::string ref_error;
                player.reference_hash = reference_state_hash(pak, ref_error);
                if (player.reference_hash) player.reference = "full engine";
            }
            if (player.reference_hash && *player.reference_hash != *hash) {
                error = std::format("the stripped web player reaches state hash {:016x} at tick {}, the {} reaches "
                                    "{:016x}",
                                    *hash, verify_tick, player.reference, *player.reference_hash);
                for (const char* f : {"index.html", "index.js", "index.wasm"}) fs::remove(options.out_dir / f, ec);
                return false;
            }
            player.verified = true;
            summary.pack.notes.push_back(
                player.reference_hash
                    ? std::format("verify: the stripped web player reaches state hash {:016x} at tick {}, equal to the "
                                  "full engine",
                                  *hash, verify_tick)
                    : std::format("verify: the stripped web player runs the game cleanly in headless Chrome (state "
                                  "hash {:016x} at tick {})",
                                  *hash, verify_tick));
        }
        return true;
    }
    // Linux / Windows: the player renamed after the game; it finds game.pak
    // next to itself (runtime/main.cpp), `--pack <file>` overrides.
    const fs::path exe = options.out_dir / (target == assets::PackPlatform::Windows ? name + ".exe" : name);
    if (!copy_file(player.source, exe, true, error)) return false;
    summary.outputs.push_back(exe);
    summary.launcher = exe;
    if (target == assets::PackPlatform::Windows &&
        !copy_msvc_runtime(player.source, options.out_dir, summary.outputs, summary.pack.notes, error)) {
        return false;
    }

    // A built player is checked before it ships: it must run this pack to the
    // state the full engine reaches, so a feature the detection missed fails
    // the export instead of the player (DESIGN.md 4.11 "Clarified (Milestone 7.5)").
    if (player.kind == ShippedPlayer::Kind::Stripped && options.verify) {
        if (options.progress) options.progress("verifying the stripped player on the exported game");
        if (!verify_stripped(project, pak, exe, player, summary.pack.notes, error)) {
            // Don't leave a player that can't run the game in the output.
            fs::remove(exe, ec);
            return false;
        }
    }
    return true;
}

bool copy_third_party_notices(const fs::path& root, const fs::path& out_dir, std::vector<fs::path>& written,
                              std::string& error) {
    const fs::path notices = root / "THIRD_PARTY_NOTICES.md";
    std::error_code ec;
    // A source checkout keeps the texts in third_party/licenses/, a prebuilt package in licenses/.
    fs::path texts = root / "third_party" / "licenses";
    if (!fs::is_directory(texts, ec)) texts = root / "licenses";
    if (!fs::is_regular_file(notices, ec) || !fs::is_directory(texts, ec)) {
        error = std::format("third-party notices not found under '{}' (THIRD_PARTY_NOTICES.md, third_party/licenses/)",
                            platform::utf8(root));
        return false;
    }
    const fs::path dest = out_dir / "licenses";
    fs::create_directories(dest, ec);
    if (!copy_file(notices, dest / "THIRD_PARTY_NOTICES.md", false, error)) return false;
    written.push_back(dest / "THIRD_PARTY_NOTICES.md");
    // Sorted, so the output listing is the same on every machine.
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(texts, ec)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        if (!copy_file(f, dest / f.filename(), false, error)) return false;
        written.push_back(dest / f.filename());
    }
    return true;
}

} // namespace cuervo::tools

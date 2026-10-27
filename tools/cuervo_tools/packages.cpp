#include "tools/cuervo_tools/packages.hpp"

#include <format>
#include <fstream>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/input/input_map.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/script/plugin_host.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "pluma/session.hpp"

namespace cuervo::tools {

namespace fs = std::filesystem;

bool new_package(const fs::path& dir, std::string_view name, std::string_view version, std::string& error) {
    if (!script::valid_package_name(name)) {
        error = std::format("'{}' is not a valid package name (letters, digits and '_', not starting with a digit)",
                            name);
        return false;
    }
    if (!script::valid_version(version)) {
        error = std::format("version '{}' must be MAJOR.MINOR.PATCH", version);
        return false;
    }
    std::error_code ec;
    if (fs::exists(dir / "package.toml", ec)) {
        error = std::format("'{}' already has a package.toml", platform::utf8(dir));
        return false;
    }
    fs::create_directories(dir / "scripts", ec);
    if (ec) {
        error = std::format("can't create '{}': {}", platform::utf8(dir / "scripts"), ec.message());
        return false;
    }
    std::ofstream manifest(dir / "package.toml", std::ios::binary);
    manifest << std::format("# A Pluma package (docs/PACKAGES.md). Games add it to project.toml:\n"
                            "#   [dependencies.{0}]\n#   path = \"<this directory>\"\n#   version = \"{1}\"\n"
                            "name = \"{0}\"\nversion = \"{1}\"\ndescription = \"\"\nlicense = \"MIT\"\n\n"
                            "[dependencies]\n",
                            name, version);
    std::ofstream script(dir / "scripts" / std::format("{}.plm", name), std::ios::binary);
    script << std::format("// Package '{0}'. Only `pub` declarations are visible to the code that\n"
                          "// imports it: `import {0}`, then `{0}.greeting()` (LANGUAGE.md 9.26).\n\n"
                          "pub fn greeting() -> str {{\n    return \"hello from {0}\"\n}}\n",
                          name);
    if (!manifest || !script) {
        error = std::format("can't write the package files in '{}'", platform::utf8(dir));
        return false;
    }
    return true;
}

namespace {

// A package's scripts in its namespace, after its dependencies'. `plugins`
// receives the native plugins among them, the package itself included.
std::optional<PackageCheck> package_parts(const fs::path& dir, std::vector<pluma::SourceFile::Part>& parts,
                                          std::string& error, std::shared_ptr<script::PluginSet>* plugins = nullptr) {
    auto vfs = assets::Vfs::dev(dir);
    auto manifest = script::load_package_manifest(*vfs, "", error);
    if (!manifest) return std::nullopt;
    // The package stands in for a project: its dependencies resolve the same
    // way (paths relative to it, or vendored under its packages/). A package
    // vendored in a game (`<game>/packages/<name>/`, as `plugin install`
    // leaves it) finds its dependencies beside it, as the game does.
    script::Project as_project;
    as_project.dir = dir;
    as_project.dependencies = manifest->dependencies;
    std::shared_ptr<assets::Vfs> root = vfs;
    std::error_code ec;
    const fs::path game = dir.lexically_normal().parent_path().parent_path();
    if (dir.lexically_normal().parent_path().filename() == "packages" && fs::exists(game / "project.toml", ec)) {
        as_project.dir = game;
        root = assets::Vfs::dev(game);
    }
    std::vector<script::ResolvedPackage> deps;
    if (!script::resolve_packages(as_project, *root, deps, error)) return std::nullopt;

    PackageCheck out;
    out.name = manifest->name;
    out.version = manifest->version;
    parts.clear();
    for (const script::ResolvedPackage& d : deps) {
        if (d.manifest.name == manifest->name) {
            error = std::format("package '{}' depends on itself", manifest->name);
            return std::nullopt;
        }
        out.packages.push_back(d.manifest.name + " " + d.manifest.version);
        for (const std::string& path : d.vfs->list(d.prefix + "scripts", ".plm")) {
            std::string text;
            if (!d.vfs->read_text(path, text, error)) return std::nullopt;
            parts.push_back({std::format("packages/{}/{}", d.manifest.name, path.substr(d.prefix.size())),
                             std::move(text), d.manifest.name});
        }
    }
    for (const std::string& path : vfs->list("scripts", ".plm")) {
        std::string text;
        if (!vfs->read_text(path, text, error)) return std::nullopt;
        parts.push_back({path, std::move(text), manifest->name});
        ++out.files;
    }
    if (out.files == 0) {
        error = std::format("package '{}' has no scripts/*.plm", manifest->name);
        return std::nullopt;
    }
    if (plugins != nullptr) {
        script::ResolvedPackage self{*manifest, vfs, "", dir};
        deps.push_back(std::move(self));
        *plugins = script::PluginSet::load(deps, error);
        if (!*plugins) return std::nullopt;
    }
    return out;
}

} // namespace

std::optional<PackageCheck> check_package(const fs::path& dir, std::string& error) {
    std::vector<pluma::SourceFile::Part> parts;
    std::shared_ptr<script::PluginSet> plugins;
    auto out = package_parts(dir, parts, error, &plugins);
    if (!out) return std::nullopt;
    // Checked against the engine API, as a game would see it.
    InputMap map;
    script::HostOptions options;
    options.plugins = plugins;
    script::ScriptHost host(options, map);
    pluma::Session session({}, &host.bindings());
    if (!session.prelude_result().ok) {
        error = "engine prelude failed to compile:\n" + session.prelude_result().rendered;
        return std::nullopt;
    }
    pluma::SessionResult r = session.load_files(parts);
    if (!r.compiled) {
        error = r.rendered;
        return std::nullopt;
    }
    if (!r.ok) {
        error = r.panic ? r.panic->render() : r.rendered;
        return std::nullopt;
    }
    return out;
}

std::optional<pluma::TestReport> test_scripts(const fs::path& dir, std::string_view filter, std::string& error) {
    std::vector<pluma::SourceFile::Part> parts;
    std::string module; // a package's own tests, not its dependencies'
    std::shared_ptr<script::PluginSet> plugins;
    std::error_code ec;
    if (fs::exists(dir / "project.toml", ec)) {
        auto vfs = assets::Vfs::dev(dir);
        if (!script::read_scripts(*vfs, parts, nullptr, error)) return std::nullopt;
        plugins = script::load_project_plugins(*vfs, error);
        if (!plugins) return std::nullopt;
    } else if (fs::exists(dir / "package.toml", ec)) {
        auto pkg = package_parts(dir, parts, error, &plugins);
        if (!pkg) return std::nullopt;
        module = pkg->name;
    } else {
        error = std::format("'{}' has no project.toml or package.toml", platform::utf8(dir));
        return std::nullopt;
    }
    InputMap map;
    script::HostOptions options;
    options.plugins = plugins;
    script::ScriptHost host(options, map);
    const pluma::Bindings* bindings = &host.bindings();
    return pluma::run_tests(parts, [bindings] { return std::make_unique<pluma::Session>(pluma::VmOptions{}, bindings); },
                            pluma::TestSelection{module, filter});
}

} // namespace cuervo::tools

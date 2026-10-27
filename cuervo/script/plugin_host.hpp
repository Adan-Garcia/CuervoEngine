#pragma once

// The engine side of the plugin ABI (cuervo/plugin/cuervo_plugin.h, milestone
// 10.5c, docs/PLUGINS.md): loads the native libraries of a project's plugin
// packages, calls their entry points and binds what they register (natives,
// finalizers, importers) into the script host.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/plugin/cuervo_plugin.h"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"

namespace pluma {
class AotModule;
class Bindings;
}

namespace cuervo::script {

struct PluginImporter {
    std::string plugin;
    std::string extension;        // ".csv"
    std::string output_extension; // ".scene"
    cuervo_import_fn fn = nullptr;
    void* user = nullptr;
};

struct LoadedPlugin {
    std::string name;
    std::string version;
    bool sim_safe = false;
    std::string kind; // "native", "static" (linked in) or "dotnet"
    struct Native {
        std::string name;
        cuervo_native_fn fn = nullptr;
        void* user = nullptr;
    };
    std::vector<Native> natives;
    std::vector<std::pair<std::string, cuervo_finalizer_fn>> finalizers;
};

class PluginSet {
public:
    PluginSet();
    ~PluginSet();
    PluginSet(const PluginSet&) = delete;
    PluginSet& operator=(const PluginSet&) = delete;

    // Loads the plugins among `packages` for this platform: the library a
    // package's [plugin] names, from its directory or (in a pack) from
    // `vfs`. A plugin built for another ABI, a missing library or an entry
    // that fails is an error. Packages without [plugin] are skipped.
    static std::shared_ptr<PluginSet> load(const std::vector<ResolvedPackage>& packages, std::string& error);

    // A plugin whose code is linked into this program (AOT modules, tests):
    // `entry` is its cuervo_plugin_entry_<name>.
    bool add_entry(std::string name, std::string version, bool sim_safe, cuervo_plugin_entry_fn entry,
                   std::string& error, std::string kind = "static");

    // The plugins an AOT module was linked with (its `cuervo_aot_plugins`
    // table, compile_aot_scripts). The module must outlive the set's use.
    bool add_aot_plugins(const pluma::AotModule& module, std::string& error);

    // Loads the plugins among `packages` not already in the set (after
    // add_aot_plugins: the ones without a static library).
    bool load_more(const std::vector<ResolvedPackage>& packages, std::string& error);

    // Adds every registered native and finalizer to `bindings`.
    void bind(pluma::Bindings& bindings) const;

    const std::vector<LoadedPlugin>& plugins() const { return plugins_; }
    const std::vector<PluginImporter>& importers() const { return importers_; }
    bool empty() const { return plugins_.empty(); }
    // Names of the plugins that are not sim-safe (replays may not verify).
    std::vector<std::string> unsafe_plugins() const;

    // The importer for `path`'s extension, or null.
    const PluginImporter* importer_for(std::string_view path) const;
    // Runs `importer` on `data`; false (with the importer's message) on failure.
    static bool run_importer(const PluginImporter& importer, std::string_view path, std::span<const std::byte> data,
                             std::vector<std::byte>& out, std::string& error);

private:
    struct Library;
    bool load_library(const ResolvedPackage& pkg, std::string& error);
    bool load_dotnet(const ResolvedPackage& pkg, std::string& error);
    // A file or directory of a package on disk: itself, or extracted from a pack.
    bool package_file(const ResolvedPackage& pkg, const std::string& rel, bool whole_dir, std::filesystem::path& out,
                      std::string& error);
    std::vector<std::unique_ptr<Library>> libraries_;
    std::vector<LoadedPlugin> plugins_;
    std::vector<PluginImporter> importers_;
};

// Development: reads through `vfs` convert importer sources on demand
// (Vfs::add_derived). A packed VFS is left alone: export already converted.
void add_importers(const std::shared_ptr<const PluginSet>& set, assets::Vfs& vfs);

// The function table every plugin is given.
const cuervo_host& plugin_host_api();

// Plugins of a project: resolves its packages (project.toml through `vfs`)
// and loads those with [plugin]. An empty set when there are none. With
// `aot`, the plugins linked into that module come from it.
std::shared_ptr<PluginSet> load_project_plugins(const assets::Vfs& vfs, std::string& error,
                                                const pluma::AotModule* aot = nullptr);

// The project's plugins that have a static library for this platform, for
// compile_aot_scripts; a .NET plugin is an error (C# is not available in AOT
// exports, DESIGN.md 4.18).
std::optional<std::vector<StaticPlugin>> static_plugins(const std::vector<ResolvedPackage>& packages,
                                                        std::string& error);

} // namespace cuervo::script

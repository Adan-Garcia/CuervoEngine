#pragma once

// Hosting .NET for C# plugins (DESIGN.md 4.18 "Decision", milestone 10.5c,
// docs/PLUGINS.md): the desktop player and editor load the installed .NET
// runtime through hostfxr and call a plugin assembly's
// `[UnmanagedCallersOnly]` entry method, whose signature is the C plugin
// entry's. Not available on the web or in AOT exports.

#include <filesystem>
#include <string>
#include <string_view>

#include "cuervo/plugin/cuervo_plugin.h"

namespace cuervo::script {

// The .NET installation used: $DOTNET_ROOT, the `dotnet` on PATH, or the
// usual install directories. Empty when there is none.
std::filesystem::path dotnet_root();

// Loads `assembly` (next to its .runtimeconfig.json and .deps.json) and
// returns its `type_name` ("Hues.Plugin, Hues") static `Entry` method.
// The runtime starts on the first call and stays for the process.
bool load_dotnet_entry(const std::filesystem::path& assembly, std::string_view type_name,
                       cuervo_plugin_entry_fn& entry, std::string& error);

} // namespace cuervo::script

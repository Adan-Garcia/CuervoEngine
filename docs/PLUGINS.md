# Plugins, FFI and embedding

Native code reaches Pluma in three ways:

- **Plugins.** A package that carries a native library. Its natives implement the
  package's `@native` declarations, and it can add nodes, classes, functions and asset
  importers. Write plugins in C++, Rust or C#; all three use one C ABI.
- **The FFI.** `@extern` declares a C function of a shared library and calls it with
  explicit marshaling ([LANGUAGE.md](LANGUAGE.md) §9.28). Use it for a few calls into an
  existing library without writing a plugin.
- **The embedding API.** A C interface for hosting the Pluma VM in another program,
  without the engine (`pluma/embed/pluma_embed.h`, [below](#embedding-pluma)).

The examples are in `examples/plugins/` (`orbit` in C++, `noise` in Rust, `hues` in C#),
and `examples/plugin_demo` is a game that uses all three. Build them with:

```sh
cmake --build build/debug --target example_plugins
cuervo --game examples/plugin_demo
```

## A plugin is a package

A plugin is an ordinary package ([PACKAGES.md](PACKAGES.md)) whose `package.toml` has a
`[plugin]` table. Its scripts declare the API in Pluma, and the library implements the
`@native` parts:

```
orbit/
  package.toml
  scripts/orbit.plm     # the API: classes, nodes, @native declarations
  src/orbit.cpp         # the natives
  lib/liborbit.so       # built: the shared library (lib/orbit.dll on Windows)
  lib/liborbit.a        # built: the static library, for AOT exports
```

```toml
name = "orbit"
version = "1.0.0"
license = "MIT"

[plugin]
abi = 1                          # CUERVO_PLUGIN_ABI_VERSION it was built against
sim_safe = true                  # see "Determinism" below
linux = "lib/liborbit.so"
linux_static = "lib/liborbit.a"  # linked into AOT exports
windows = "lib/orbit.dll"
windows_static = "lib/orbit_static.lib"      # MSVC-runtime builds
windows_gnu_static = "lib/liborbit_gnu.a"    # MinGW-w64 builds
```

A .NET plugin names its assembly instead: `dotnet = "bin/Hues.dll"`. Its entry type
defaults to `<Assembly>.Plugin, <Assembly>`, and `dotnet_type` overrides it.

The script side is plain Pluma. Types, defaults, named arguments, classes and
inheritance all come from the checker, and the library supplies only the bodies:

```pluma
// orbit/scripts/orbit.plm
pub class Orbiter extends Node2D {
    var radius = 40.0
    var speed = 1.0
    fn update(dt: float) { ... }
    @native fn energy() -> float {}           // implemented in C++
}
@native pub fn advance(angle: float, speed: float, dt: float) -> float {}
```

A game depends on the plugin like any package and uses it through `import`:

```toml
[dependencies.orbit]
path = "../plugins/orbit"
version = "1.0.0"
```

```pluma
import orbit
var o = orbit.Orbiter()
```

**Namespaces.** A plugin registers names only inside its package: `advance` is
`orbit.advance`, and `Orbiter.energy` is `orbit.Orbiter.energy`. A registration outside
the namespace is refused when the plugin loads, so two plugins never collide.

## The C ABI

`cuervo/plugin/cuervo_plugin.h` is the one extension boundary. The other bindings are
built on it. A plugin exports one function:

```c
int cuervo_plugin_entry_orbit(const cuervo_host* host, cuervo_registrar* reg);
```

During this call the plugin registers its natives, finalizers and importers through
`host`. The table also reads arguments, sets results, raises panics, reads and writes
object fields, and logs. A native receives a call handle:

```c
static const cuervo_host* host; /* kept from the entry call */

static void advance(cuervo_call* call, void* user) {
    double a = host->arg_float(call, 0) + host->arg_float(call, 1) * host->arg_float(call, 2);
    host->ret_f32(call, (float)a);
}

int cuervo_plugin_entry_orbit(const cuervo_host* h, cuervo_registrar* reg) {
    host = h;
    return host->register_native(reg, "advance", advance, NULL);
}
```

**Versioning.** `CUERVO_PLUGIN_ABI_VERSION` is 1. The host table starts with the
engine's ABI version and the table's `size`. New entries are only appended, so a plugin
built against an older header keeps working. The engine refuses a plugin whose
`abi` is not its own ABI version, and the message names both versions.

**Values.** Numbers convert from any numeric argument. A result setter must match the
declared return type: `ret_i32` for `int`, `ret_i64` for `i64`, `ret_f32` for `float`,
`ret_f64` for `f64`. Strings are UTF-8, and vectors are `vec2` to `vec4`. A native that
calls `panic` ends the script call with a stack trace, as a failed `assert` does.

**Host objects.** A class with native methods can attach host data to an instance
(`object_set_native`). The class's finalizer frees it when the instance dies.

## C++

`cuervo/plugin/cuervo_plugin.hpp` is a header-only wrapper. Natives are typed lambdas:
the wrapper converts arguments and the result from the lambda's signature, and turns a
C++ exception into a script panic.

```cpp
#include "cuervo/plugin/cuervo_plugin.hpp"

CUERVO_PLUGIN(orbit) {
    plugin.function("advance", [](float angle, float speed, float dt) { ... });
    plugin.function("describe", [] { return std::string("orbit 1.0.0 (C++)"); });
    plugin.function("Orbiter.energy", [](cuervo::plugin::Object self) {
        const double r = self.get_float("radius");
        ...
    });
    plugin.finalizer<Counter>("Box");     // frees a Counter attached with set_native
}
```

Build it as a shared library and as a static one. `examples/plugins/CMakeLists.txt`
shows how.

## Rust

The `cuervo-plugin` crate (`bindings/rust/cuervo-plugin`) is a safe layer.
`tools/gen_plugin_abi.py` generates its raw `sys` module from the C header. Typed
closures work as in C++, and a Rust panic becomes a script panic:

```rust
use cuervo_plugin::{cuervo_plugin, Object};

cuervo_plugin!(noise, |plugin| {
    plugin.function("value", |x: f32, y: f32, seed: i32| value(x, y, seed));
    plugin.function("Wisp.offset", |wisp: Object| -> [f32; 2] { ... });
});
```

Build with `crate-type = ["cdylib", "staticlib"]`. The `noise_plugin` CMake target runs
cargo and copies both libraries to `lib/`.

## C#

`Cuervo.Plugin` (`bindings/dotnet/Cuervo.Plugin`) runs plugins on .NET, hosted by the
desktop player and editor through hostfxr. The same script generates its raw layer
(`Abi.g.cs`). The entry is an `[UnmanagedCallersOnly]` method:

```csharp
public static class Plugin
{
    [UnmanagedCallersOnly]
    public static unsafe int Entry(cuervo_host* host, IntPtr reg) =>
        Cuervo.Plugin.Entry.Run(host, reg, "hues", plugin =>
        {
            plugin.Function("hue", (float t) => Hue(t));
            plugin.Function("Pulse.scale", (ScriptObject self) => ...);
        });
}
```

**Decision:** C# is available on desktop only. It is not available on the web or in AOT
exports until a NativeAOT/WASM path is proven. An AOT export of a game with a .NET
plugin is refused and the message names the plugin. A source export ships the
assembly's directory.

## Determinism

The simulation must be deterministic (DESIGN.md §4.1), and natives run inside it. A
plugin declares `sim_safe = true` only when its natives return the same results on every
machine. In practice that means:

- integer math and IEEE arithmetic only;
- no libm transcendental functions (`sin`, `exp`, ...);
- no clocks, threads or unordered iteration;
- no randomness unless it is seeded.

The engine logs a warning for each plugin that is not sim-safe, because replays and
state hashes of a game that uses its natives in the simulation may differ. Such plugins
belong in drawing, tools and platform services. `cuervo-tools plugin verify` runs a
sim-safe plugin's conformance project to check the flag ([Sharing plugins](#sharing-plugins)).

## AOT exports

`cuervo-tools export --aot` compiles scripts to native code (DESIGN.md §5.12) and links
every plugin's static library (`linux_static`; on Windows `windows_static` from an MSVC-runtime
build or `windows_gnu_static` from a MinGW-w64 one, since a static library only links
with its own toolchain's runtime) into the script
module. The module carries a table of the plugins it contains, and the player registers
them from that table, so those plugins need no library file at run time. A plugin with
only a shared library ships it in the pack and loads it from there. A .NET plugin makes
the AOT export fail, and the message names it.

## Importers

A plugin can convert asset sources into engine formats:

```cpp
plugin.importer(".upper", ".txt", [](std::string_view path, std::span<const std::uint8_t> data) {
    ... return converted bytes ...
});
```

Game code always names the **output** path. Here that is `notes/readme.txt`, made from
`notes/readme.upper`. This keeps loaders unchanged:

- **In development,** the VFS converts on read: when no file has that path, it finds the
  source and runs the importer. A real `notes/readme.txt` wins over the derived one.
- **At export,** the pack stores the converted file under the output path and leaves out
  the source, so a release game never runs an importer.

Export runs the plugins on the developer's machine, whatever the target platform. If a
plugin can't load there, a source export notes that its importers didn't run, and an
AOT export fails.

## Sharing plugins

Plugins, like any package, are shared through a **registry**: a directory holding
`index.json` and one archive per version. It can be a local folder, a shared drive, or a
checkout served as static files. `--registry <dir>` names one, and `CUERVO_REGISTRY`
gives the default.

```sh
cuervo-tools plugin verify  examples/plugins/orbit              # check a package
cuervo-tools plugin publish examples/plugins/orbit --registry R # verify, archive, index
cuervo-tools plugin install orbit@^1.0 --registry R             # into the game in .
cuervo-tools plugin list --registry R
```

**The manifest.** `package.toml` declares what the checks need:

```toml
name = "orbit"
version = "1.0.0"
license = "MIT"                  # an SPDX expression: "MIT OR Apache-2.0", "LicenseRef-..."
engine = ">=0.1, <0.3"           # engine versions it works with (default: any)

[plugin]
abi = 1                          # or a range of ABI versions: ">=1, <3"
sim_safe = true
platforms = ["linux", "windows"] # default: those the library fields name
linux = "lib/liborbit.so"
```

**Verification.** `verify` runs these checks, and so do `publish` (before it archives)
and `install` (on the installed copy):

- the license is a valid SPDX expression, and a license text (`LICENSE`, `LICENSE.md`,
  `COPYING`) is in the package; a copyleft license is noted;
- this engine is in the `engine` range, and its plugin ABI is in the `abi` range;
- every listed platform has a library, and the library for this machine exists, loads
  and registers its natives;
- the package's scripts check against the engine API and the plugin's natives;
- a **sim-safe** plugin has a conformance project (`conformance/project.toml`, a small
  game that depends on the plugin with `path = ".."` and exercises its natives in the
  simulation, keeping its results in globals). `verify` packs it, runs it headless for
  240 ticks and takes the state hash. `publish` records the hash, and `install` requires
  the same hash on the installing machine, so a plugin that isn't deterministic there
  fails to install.

**Archives and checksums.** An archive (`packages/<name>/<name>-<version>.cpkg`) is a
pack holding the manifest, scripts, license and notices, README, the conformance
project and the plugin's built libraries. Sources and build trees are left out. The
index records the SHA-256 of the archive and of every file in it, and the platforms
that have a built library. `install` refuses an archive or file that doesn't match its
checksum, and a version is never published twice with different contents.
Dependencies must be in the registry before their dependents; `path` lines in an
archived manifest are dropped, because installed packages find their dependencies
vendored beside them.

## Platforms

| | Linux | Windows | Web |
|---|---|---|---|
| C++ / Rust plugins | yes, and in AOT | yes, and in AOT | not yet |
| C# plugins | yes (.NET) | yes (.NET) | no |
| `@extern` FFI | yes | yes | no |
| Embedding API | yes | yes | yes |

A web export of a game with a plugin is refused. Web plugins would be WASM side modules,
which are not built yet.

## Embedding Pluma

`pluma/embed/pluma_embed.h` hosts the VM in any C or C++ program, without the engine. It
is part of the `pluma` library. The host declares its own API as a prelude of `@native`
declarations, binds C callbacks to them, loads modules, and calls into scripts:

```c
#include "pluma/embed/pluma_embed.h"

static pluma_value host_log(pluma_vm* vm, const pluma_value* args, uint32_t n, void* user) {
    puts(args[0].as.str);
    return pluma_none();
}

pluma_vm* vm = pluma_new();
pluma_add_prelude(vm, "host.plm", "@native fn host_log(s: str) {}");
pluma_add_native(vm, "host_log", host_log, NULL);
if (!pluma_load(vm, "game.plm", source)) fputs(pluma_error(vm), stderr);

pluma_value args[2] = {pluma_int(4), pluma_float(1.5)}, out;
if (pluma_call(vm, "score", args, 2, &out)) printf("%g\n", out.as.number);
pluma_free(vm);
```

- **Types.** Values convert by the declared Pluma types. `PLUMA_INT` fills any integer
  type and `PLUMA_FLOAT` any float type, so the host never names a width.
- **Objects.** Class instances, lists and maps come back as `pluma_object` handles. The
  host releases them (`pluma_release`) and passes them back with `pluma_call_method`.
- **Errors.** Diagnostics, panics and a native's `pluma_panic` come back as text from
  `pluma_error`. A panic never unwinds through C frames.
- **Time and memory.** `pluma_tick` advances coroutines one tick, and `pluma_collect`
  runs a step of the cycle collector.
- **Versioning.** The API is `PLUMA_EMBED_API_VERSION` 1, and later versions only add
  functions.

`tests/pluma/embed_test.c` is a complete example in plain C.

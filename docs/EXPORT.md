# Shipping a game (packing and export)

A release is a **player executable plus one archive**, `game.pak`, that holds
every file the game reads. Nothing reads the project directory any more: the
runtime loads project.toml, scripts, scenes, sounds and the font from the pack
through the engine's virtual file system (VFS). DESIGN.md 4.11: a release export
packs everything into a single archive per platform.

Status: **Linux, Windows and web exports built and verified**: the exported shmup
reaches the same state hash at tick 240 as `cuervo --game examples/shmup`
(`a3e57e79964421d3`), natively from an unrelated directory with the project
deleted, and in headless Chrome. Linux exports ship a **stripped player** by default: an engine built
with only the subsystems the game uses ("Stripped players" below), 20-40%
smaller than the full Release player.

## Quick start

```sh
# Linux: dist/shmup (the player, renamed) + dist/game.pak
cuervo-tools export examples/shmup --platform linux -o dist/ --strip
cuervo-tools export examples/shmup --platform linux --aot -o dist-aot/ --strip  # AOT native scripts (.cuervo/scripts.so)
./dist/shmup                      # finds game.pak next to itself
./dist/shmup --pack other.pak     # or any pack; all `cuervo --game` flags work

# Web: index.html/.js/.wasm (a game-independent runtime) + game.pak
cmake --preset host-tools && cmake --build --preset host-tools
cmake --preset web-pack && cmake --build --preset web-pack     # build/web-pack/runtime
cuervo-tools export examples/shmup --platform web -o dist-web/ --strip
python3 -m http.server 8000 --directory dist-web

# Just the archive, or a look inside one
cuervo-tools pack examples/shmup -o game.pak [--platform linux|web|windows] [--scripts source|native] [--aot] [--strip]
cuervo-tools pack-info game.pak   # entries, sizes, platform; verifies the hash
```

Options of `pack` and `export`:

| Option | Meaning |
|---|---|
| `--platform linux\|web\|windows` | the target (one archive per platform). `pack` defaults to the host |
| `-o` | the `.pak` file (`pack`) or the output directory (`export`) |
| `--scripts source\|native` | how scripts ship: `source` (`.plm` files, default) or `native` (AOT-compiled to C and built into `.cuervo/scripts.so`, omitting `.plm` sources) |
| `--aot` | shorthand for `--scripts native` |
| `--strip` | leave development-only files out (below) |
| `--font <file>` | the game font (TTF/OTF imported to `.efont`, or an `.efont`); default `assets/fonts/LiberationSans-Regular.ttf` |
| `--runtime <path>` | the player to ship: an executable (Linux/Windows) or a web runtime directory. Wins over `--engine` |
| `--engine auto\|full\|stripped` | (`export`) which player ships when `--runtime` isn't given: `auto` (default) builds a stripped player when the target is Linux, a toolchain is available and the game doesn't need every feature, else ships the full one (with a note saying why); `full` ships the prebuilt player; `stripped` always builds one and fails if it can't ("Stripped players") |
| `--cmake <path>` | (`export`) the cmake that builds stripped players; default the one that configured cuervo-tools |
| `--player-cache <dir>` | (`export`) where stripped players are built and kept; default `$CUERVO_PLAYER_CACHE`, else `$XDG_CACHE_HOME/cuervo/players`, else `~/.cache/cuervo/players` |
| `--no-verify` | (`export`) don't run a freshly built player on the exported game before shipping it (for cross builds whose player can't run here) |
| `--name <launcher>` | the executable's name; default the project name, lowercased (`Shmup` → `shmup`) |

## Players

The full player (`--engine full`, or `auto` when no stripped player can be built) is,
in this order:

1. `--runtime <path>`;
2. `CUERVO_EXPORT_RUNTIME` (Linux and Windows) or `CUERVO_EXPORT_WEB_RUNTIME` (web);
3. the player of the build tree that cuervo-tools comes from: `cuervo` on Linux,
   `cuervo-player.exe` on Windows (the GUI-subsystem build of the same runtime, which
   opens no console window; docs/WINDOWS.md 5), `build/web-pack/runtime` for the web;
4. the **export template** of a prebuilt package: `<package>/templates/<platform>/`
   beside its `bin/` (`linux/cuervo`, `windows/cuervo-player.exe`).

A build tree only has its own platform's player, so exporting to the other desktop
platform takes a template. Each release ships one in its archive, and
`cuervo-export-templates.zip` holds both (unpack it next to `bin/`). The Windows
templates are Authenticode-signed when the release has a signing certificate. Every
release file is listed in `SHA256SUMS.txt` and, while the repository is public, has a
Sigstore build attestation (`gh attestation verify <file> --repo <owner>/<repo>`; GitHub
offers none for private repositories owned by a user; `.github/workflows/release.yml`).

Shipping a Debug full player prints a note: export from a Release build, or ship a
stripped player (always Release), for a real release.

## Pipeline

`tools/cuervo_tools/export.{hpp,cpp}`:

1. **check**: project.toml must load, and the **tier check** must pass
   (`cuervo-tools tier check`, DESIGN.md 4.4) against the project's tier, and the
   project must not claim support the target platform lacks ([STATUS.md](STATUS.md)):
   a `render_tier = "full"` project is refused for `--platform web`, and `--aot` is
   refused unless the export host is the target. Errors stop the export; experimental
   claims warn.
2. **collect**: every file under the project. Hidden files and directories
   (`.import/`, `.git`, `.gitignore`...) are never packed. `--strip` also drops
   development-only files: replays (`*.replay`, `replays/`, docs/REPLAYS.md) and
   editor command scripts (`*.cmds`). With `--aot` / `--scripts native`, `.plm`
   sources are omitted from the pack and compiled in step 4.
3. **import**: sources become the engine formats every target loads, and the
   engine file replaces the source in the pack: TTF/OTF → `.efont`, SVG →
   `.evec` (ImportedVector). Sounds ship unchanged because miniaudio decodes
   WAV/OGG/FLAC/MP3 on Linux, Windows and the web. A hand-made `x.efont` next to
   `x.ttf` wins. The `.import/` cache is not used: export always re-imports.
4. **scripts**: the form is recorded in the manifest (`ScriptForm`).
   - `source` (`ScriptForm::Source`): the `.plm` files, compiled by the VM at startup.
   - `native` (`ScriptForm::Native`, `--aot`): `pluma::emit_c` translates the engine
     prelude and project scripts to C11 and compiles them into `.cuervo/scripts.so`
     with `-std=c11 -O2 -ffp-contract=off -fno-fast-math -g0 -fno-ident -Wl,--build-id=none`,
     leaving `.plm` sources out of the archive while keeping panic stack traces,
     live-local inspection, and use-after-free diagnostics.
   - A runtime refuses a pack whose script form it can't run.
5. **engine entries**: `.cuervo/default.efont` (the game font),
   `.cuervo/scripts.so` (when `scripts = "native"`), and `.cuervo/export.toml`
   (the manifest: format, game, engine version, platform, script form, stripped).
6. **pack**: a deterministic archive. It is then opened again the way the
   runtime will open it (project.toml and scripts/native module checked through
   the VFS) as a self-check.
7. **assemble** (`export` only): the player + `game.pak` (Linux/Windows), or
   the web runtime + `game.pak`. The player is a stripped build (next
   section) or the full one; a stripped one is run on the exported game
   before the export succeeds. Every export also gets `licenses/`: the
   engine's `THIRD_PARTY_NOTICES.md` and the license texts of
   `third_party/licenses/` (SDL3, Vulkan headers, ...), because those licenses
   ask for their notices to travel with the binary. A game with packages also
   gets `licenses/PACKAGES.md` (each package's license, as `cuervo-tools license
   report` prints it) and their license texts in `licenses/packages/`. The
   game's own license is the developer's to add.

## Stripped players

`tools/cuervo_tools/player.{hpp,cpp}` (DESIGN.md 4.11 "Clarified (Milestone 7.5)").
The pack's manifest records the features the game
needs (`detect_features`, `[features]` in project.toml overrides it). Export
builds a player with exactly those:

1. **configure** (once per cache entry) the engine sources cuervo-tools was
   built from, with the cmake, generator and C/C++ compilers of its own
   configure, into `<player cache>/<key>`:
   `CMAKE_BUILD_TYPE=Release`, `CUERVO_BUILD_TESTS=OFF`,
   `CUERVO_BUILD_EDITOR=OFF`, `CUERVO_FEATURE_AUDIO/UI/PARTICLES/COLLISION`
   from the feature set, `CUERVO_FEATURE_DEV=OFF`, `CUERVO_VULKAN=OFF` unless
   the game needs Vulkan (compatible-tier games run on GL),
   `CUERVO_SVG_IMPORT=OFF` and `CUERVO_FONT_IMPORT=OFF` (a pack carries imported
   assets). The tools tree's FetchContent checkouts are passed as
   `FETCHCONTENT_SOURCE_DIR_<NAME>`, so nothing is downloaded again.
2. **build** the `runtime` target only (`cmake --build <dir> --target runtime`).
   A cached entry is rebuilt incrementally, so it follows source changes. The
   output of both steps is in `<dir>/player-build.log`; a failure fails the
   export with the log's tail. A lock file serializes exports sharing an entry.
3. **verify** (skip with `--no-verify`): the shipped launcher runs headless
   (`SDL_VIDEO_DRIVER=offscreen --backend gl --no-audio --screenshot`, tick
   240, a private `CUERVO_USER_DIR`) next to its `game.pak`. It must exit 0,
   run from the pack, log no `[error]` or panic (a script naming a feature the
   player lacks is a compile error, so a missed detection fails here) and its
   state hash must equal the same pack run in cuervo-tools' own (full)
   engine. The hash doesn't depend on the features a player was built with:
   a player without particles or the UI hashes what those hold in a game that
   doesn't use them (an idle particle system, the UI's initial statics;
   `ScriptHost::state_hash`, checked by `tests/cuervo/state_hash_tests.cpp`),
   so a stripped player, native or web, reaches the full engine's hash and a
   replay recorded on one plays on the other.
   - Should the full engine fail to run the pack here, the reference is the
     same player's dev run (`--game` on a copy of the project, with the pack's
     font); a project with SVG/TTF/OTF sources can't have that dev run (no
     importers) and is then only checked for a clean run.
   On failure the launcher is removed from the output.

The **cache key** is `<platform>_<features>_<hash>` (e.g.
`linux_collision+particles_327a6e73ddd1f1fa`, `linux_core_...` with no optional
feature). The hash (FNV-1a 64) covers the CMake definitions, the platform, the
engine version and source directory, the generator and the compilers; not the
cmake path or the dependency checkouts. Equal feature sets share one entry
whichever project they come from.

The export prints which player shipped, its size and the full player's:

```
player: stripped, 2480896 bytes (full prebuilt player, Debug: 45279648 bytes), verified: state hash 0b1d5dd886e8c072 at tick 240 (equal to the full engine)
```

Limits:

- **AOT** (`--aot`): `.cuervo/scripts.so` is compiled against cuervo-tools'
  own engine prelude, so it only matches a player with the same script
  surface. `auto` ships the full player otherwise (with a note), `stripped`
  fails. The shmup's AOT export ships a stripped player. Lifting this would need the
  AOT emitter to take the target feature set (not implemented).
- **Web**: the same definitions plus `CUERVO_WEB_PACK=fetch` (the runtime that
  fetches the `game.pak` next to its page) and never Vulkan, configured with
  emscripten's toolchain file and `CUERVO_HOST_BUILD` like the `web-pack`
  preset. It needs an emsdk (`--emsdk <dir>`, else `$EMSDK`, else `~/emsdk`)
  and a host-tools build (`--host-build <dir>`, else the source tree's
  `build/host-tools`; docs/WEB.md "Building"); without them `auto` ships the
  prebuilt web runtime. The cache key adds the emscripten version. The
  shipped player is the page directory (`index.html`, `.js`, `.wasm`) with both
  render backends (`CUERVO_WEBGPU=ON`: WebGPU where the browser has it, WebGL2
  otherwise; docs/WEB.md "WebGPU"), and it
  is verified in headless Chrome (`tools/web/smoke_test.mjs`): it must run
  the pack and match the full engine's state hash. No Chrome: the export
  fails unless `--no-verify`.
- **Windows** (on a Windows host, milestone 10.5a): the same definitions, built with the
  tools' own generator and compilers into `%LOCALAPPDATA%\cuervo\players` (the `player`
  target: GUI subsystem). MSVC builds need the developer prompt's environment (`INCLUDE`,
  `LIB`); without it the toolchain counts as missing. The verification run opens a real
  window (Windows has no offscreen GL). tictak's MSVC player is 31 % smaller than the full
  Release player and reaches the same state hash. Building Windows players from Linux, or
  Linux players from Windows, is refused: a native player builds on its own platform.

### Sizes

Measured on Arch Linux x86_64, GCC, Release, unstripped
executables (`--gc-sections`, no `-s`). The full player is a Release `runtime`
with every option at its default (every subsystem, Vulkan, dev tools, SVG and
font import).

| Game | Needs | Player | Size (bytes) | vs full | State hash at tick 240 |
|---|---|---|---|---|---|
| (any) | everything | full Release | 4,162,784 | | |
| `examples/tictak` | nothing optional (compatible tier) | `linux_core` | 2,480,896 | -40.4% | `0b1d5dd886e8c072` = the full engine's dev run |
| `examples/bench` | particles, collision | `linux_collision+particles` | 2,613,968 | -37.2% | `0d267da3c3be0700` = the full engine's dev run |
| `examples/shmup` | audio, ui, particles, collision | `linux_audio+collision+particles+ui` | 3,343,544 | -19.7% | `a3e57e79964421d3` = the full engine's dev run (source and `--aot`) |

Every stripped player also drops FreeType (and with it zlib, bzip2, libpng and
brotli) from its shared-library dependencies: it needs only SDL3 and the C/C++
runtime.

Web (emscripten 6.0.12, Release `-O2`): the payload that changes is
`index.wasm` (`index.js` is ~244 KB either way). The full web player is the
`web-pack` preset's runtime (every subsystem a web build has; never Vulkan).
Every web player carries both render backends, WebGPU and WebGL2 (milestone
10.3); WebGPU (emdawnwebgpu and the backend) adds about 0.87 MB to `index.wasm`
and 48 KB to `index.js`. The players are verified on SwiftShader, where the
page gets no WebGPU adapter and so runs WebGL2.

| Game | Needs | `index.wasm` (bytes) | vs full | State hash at tick 240 (headless Chrome) |
|---|---|---|---|---|
| (any) | everything | 4,308,581 | | |
| `examples/tictak` | nothing optional | 3,271,791 | -24.1% | `0b1d5dd886e8c072` (= the full engine) |
| `examples/bench` | particles, collision | 3,379,703 | -21.6% | `0d267da3c3be0700` (= the full engine) |
| `examples/shmup` | audio, ui, particles, collision | 3,794,264 | -11.9% | `a3e57e79964421d3` (= the full engine) |

The web saves less than Linux because a web build never had Vulkan, FreeType
or the SVG importer to drop; what goes is audio (miniaudio), UI, particles,
collision and their script preludes.

## Pack format

`cuervo/assets/pack.hpp`. Little-endian. Every offset is absolute and every
section is 16-byte aligned.

| Part | Content |
|---|---|
| header (64 B) | magic `CPAK`, u32 format version (1), u32 platform (0 any, 1 linux, 2 web, 3 windows), u32 entry count, u64 table / names / data offsets, u64 file size, u64 **content hash** (FNV-1a 64 of every byte after the header), u32 flags, u32 reserved |
| table | 32 B per entry, **sorted by path** (bytewise UTF-8, so it can be binary searched): u32 name offset, u32 name size, u64 data offset, u64 size, u32 compression (0 = stored), u32 reserved |
| names | the paths, concatenated |
| data | each entry's bytes, 16-byte aligned, zero padded |

- **Deterministic:** the same inputs always give a byte-identical pack. Entries
  are sorted, there are no timestamps, and padding is zero.
  - Packing the shmup twice gives the same bytes, and so do a Debug and a
    Release cuervo-tools; the `export_shmup` test checks this.
- **Integrity:** opening a pack checks the header ranges, the sort order, the
  path rules and the content hash, so a corrupted or truncated pack is refused
  with a message. `PackOpenOptions{.verify = false}` skips the hash for
  inspection tools.
- **Paths:** project-relative with `/`. Empty, `.` and `..` components, leading
  `/`, `\` and drive letters are not allowed. Lookups are case-sensitive on
  every OS, so a game behaves the same everywhere.
- **Reading:** memory-mapped on POSIX, read into memory on Windows and in the
  web's MEMFS. Entries are handed out as read-only `std::span` views that live as
  long as the `Pack`.
- **Compression:** none. No compression library is pinned in
  `cmake/Dependencies.cmake`. Most of the payload is already compact engine
  formats, and the web server compresses the download (gzip/brotli). The
  per-entry `compression` field is reserved, so a codec can be added later
  without a format break. Readers reject codes they don't know.
- Newer format versions are refused with "format version N is not supported".

## VFS rules

`cuervo/assets/vfs.hpp`. Every engine and game file goes through one
`assets::Vfs`:

- **Mounts** are searched in order and the first match wins.
  - Development (`Vfs::dev`) mounts the project directory.
  - Release (`Vfs::release`) mounts the pack.
- **Paths** are project-relative and normalized: `./a\b` becomes `a/b`.
  Absolute paths and `..` are errors.
- **Listing** (`list(dir, ext)`) merges all mounts and sorts in path-component
  order, which is what sorting `std::filesystem` paths gives. Dev and release
  therefore register scripts and sounds in the same order, and the simulation
  matches bit for bit.
  - Hidden files are never listed, because a pack never contains them.
  - Engine entries under `.cuervo/` can be opened but are never listed.
- **Missing files** fail with `'<path>' not found (searched ...)`, naming every
  mount that was searched.
- **Saves are not game data.**
  - `Scene.save` writes to the VFS's **user directory**: the project directory
    in development (as before), or the per-user directory in release
    (`SDL_GetPrefPath("Cuervo", <game>)`, e.g. `~/.local/share/Cuervo/Shmup/`).
    `CUERVO_USER_DIR` overrides it for tests and portable installs.
  - In release, `Scene.load` reads a saved file from the user directory first,
    so a save shadows the packed scene with the same path, as it did in
    development.
- **Hot reload is development-only.** A pack never changes under a running
  game: no file watcher, and F5/F6 are off.

How each kind of file is read:

| File kind | Read through |
|---|---|
| project.toml | `load_project(const Vfs&)` |
| scripts | `read_scripts(const Vfs&)` (`Vfs::list`) |
| scenes (`Scene.load`, `main_scene`) | `ScriptHost::load_scene` → `Vfs::open` + `scene::read_binary` |
| `Scene.save` | writes to `Vfs::user_dir() / path` |
| sounds (`assets/**`, `Audio.load`) | `AudioSystem::load_directory(const Vfs&, dir)`, `AudioOptions::vfs` |
| game font | in a pack, `.cuervo/default.efont` through `load_font_data` |

The path-taking functions are wrappers that build a development VFS, so the
editor and tools work on plain project directories. The following stay on the real file
system on purpose, because they are developer tools or explicit user paths, not
game data:

- editor project editing
- `cuervo-tools` (tier check, scene dump/build/upgrade)
- `--record` replays and `--screenshot` output
- `--font` / `--svg` / `--export` on the command line
- the hot-reload file watcher

## Platforms

### Linux

`dist/` holds:

- `<game>`, a copy of the player (no shell launcher needed);
- `game.pak`.

With no mode flags, the player looks for `game.pak` next to its executable
(`SDL_GetBasePath`). `--pack <file>` overrides that, and `--game <dir>` still
runs a project. Every runtime flag works:

```sh
SDL_VIDEO_DRIVER=offscreen ./dist/shmup --backend gl --no-audio --screenshot s.ppm   # state hash a3e57e79964421d3
```

The full player links the system SDL3 and FreeType dynamically; a stripped
player (no font import) only SDL3. The target machine needs them
(`libSDL3.so.0`, `libfreetype.so.6`), or the binary has to be built against
static copies. Shared libraries are not bundled.

### Web

The web build can take a pack instead of a raw project directory. The cache
variable `CUERVO_WEB_PACK` (cmake/Web.cmake) selects how:

| `CUERVO_WEB_PACK` | Build |
|---|---|
| empty (default) | the development build: `CUERVO_WEB_GAME` and the font preloaded as files |
| `fetch` (preset `web-pack`) | a **game-independent** runtime: `web/pack_loader.js` fetches `game.pak` next to the page into MEMFS before `main()`, and the runtime runs `--pack /game.pak`. `export --platform web` copies this build and writes `game.pak` |
| a `.pak` path | that pack preloaded as `/game.pak`: one self-contained build (`index.data` is the pack) |

`export --platform web` checks that the runtime directory is a pack-loading
build. For the opt-in WASM-threads variant, build the `web-pack-threads` preset and pass
`--runtime build/web-pack-threads/runtime` (docs/WEB.md "Threads"). Verify an export with:

```sh
node tools/web/smoke_test.mjs --dir dist-web --args "--screenshot /tmp/w.ppm" --expect "state hash a3e57e79964421d3"
```

### Windows

Milestone 10.5a (docs/WINDOWS.md). `export --platform windows` ships `cuervo-player.exe`
as `<game>.exe` next to `game.pak` (see "Players" above). It is the GUI-subsystem build of
the runtime, so the game opens no console window, and its log still reaches a redirected
stdout. `export_shmup` and `export_tutorial` pass with the MSVC and MinGW-w64 players. Both
games, exported with source and AOT scripts, run from another directory and reach their
dev state hashes. The release workflow repeats the shmup check from the unpacked
Windows archive.

- AOT scripts (`--aot`) need the export host to be Windows. The C compiler is LLVM
  `clang` with every toolchain.
- Stripped players build on a Windows host (below). With MSVC, run `cuervo-tools` from a VS
  developer prompt: `auto` otherwise ships the full player and says why.
- `pack --platform windows` works on any host: a pack is portable data with only a
  platform tag.
- The release paths are portable: `SDL_GetBasePath` / `SDL_GetPrefPath`, and
  `std::ifstream` instead of mmap.

## Tests

- `tests/cuervo/pack_tests.cpp`: the format.
  - round trip through bytes and through a file
  - byte determinism (insertion order and two writes)
  - missing entries
  - corruption: content hash, truncated, bad magic, newer version
  - path rules
  - VFS resolution order, merged listing, hidden files, normalization,
    listing order equal in dev and release
  - the manifest
- `tests/cuervo/export_tests.cpp`: packing projects.
  - the shmup packs byte-identically twice
  - hidden files are left out, `--strip`, TTF/SVG import
  - the tier gate
  - a `GameSession` on the pack reaches the dev hash at tick 240
  - release saves go to the user directory and shadow packed scenes
  - non-game packs are refused
- `export_tutorial` and `run_tutorial` (ctest): the tutorial game packs, exports (source and
  AOT), ships alone and reaches its state hash.
- `export_shmup` (ctest, `tests/export_shmup.cmake`): end to end.
  1. Export a copy of the shmup twice and require identical `game.pak` files.
  2. Delete the project copy and move only `dist/`.
  3. Run the launcher from an unrelated working directory and require
     `state hash a3e57e79964421d3` with no error or project path in the log.
  It ships the full player (`--engine full`) so it stays fast.
- `tests/cuervo/player_tests.cpp`: stripped players without a build.
  - `--engine` parsing, feature set → CMake definitions, what can be stripped,
    option dependencies
  - cache keys: stable, order-independent, changed by every input that changes
    the binary and by nothing else
  - the configure command (dependency checkouts sorted), toolchain checks,
    `run_process` (output, exit codes, timeouts)
  - which player ships: `auto` without a toolchain falls back with a note,
    `stripped` fails instead, `--runtime` wins, a missing toolchain is refused
- `export_stripped_tictak` (ctest, `tests/export_stripped.cmake`, only with
  `-DCUERVO_SLOW_TESTS=ON` because it builds an engine; the player cache stays
  in the build tree, so reruns take seconds):
  1. export a copy of tictak with `--engine stripped` (which verifies itself)
  2. ship only `dist/`, run it from elsewhere: it must run from the pack
  3. its state hash must equal the stripped player's dev run and the full
     player's, its tick-240 frame the full player's, and it must be smaller
     than the full player

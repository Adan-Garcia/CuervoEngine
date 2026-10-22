#pragma once

#include <filesystem>
#include <optional>
#include <string>

// What the target platform can do, decided at compile time, plus the few
// portable helpers that hide the differences (DESIGN.md 2 "Platforms": Linux,
// Windows, Web). Code that needs threads, a writable file system or file
// watching asks here instead of testing `_WIN32` / `__EMSCRIPTEN__` itself.
namespace cuervo::platform {

enum class Os { Linux, Windows, Web, Other };

#if defined(__EMSCRIPTEN__)
inline constexpr Os os = Os::Web;
#elif defined(_WIN32)
inline constexpr Os os = Os::Windows;
#elif defined(__linux__)
inline constexpr Os os = Os::Linux;
#else
inline constexpr Os os = Os::Other;
#endif

// Worker threads. The web build is single-threaded (no SharedArrayBuffer /
// COOP+COEP requirement): the job system runs inline (DESIGN.md 4.14).
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
inline constexpr bool has_threads = false;
#else
inline constexpr bool has_threads = true;
#endif

// Watching source files for hot reload. On the web the project is a
// read-only preloaded bundle, so there is nothing to watch.
inline constexpr bool has_file_watching = os != Os::Web;

// The frame loop must return to the browser after each frame
// (emscripten_set_main_loop); see main_loop.hpp.
inline constexpr bool callback_main_loop = os == Os::Web;

std::string_view os_name();

// Environment variable, or nullopt when unset. (std::getenv is deprecated
// on MSVC and environment variables don't exist in browsers; Emscripten
// emulates an empty environment, which this reports as unset.)
std::optional<std::string> env(const char* name);
// True when the variable is set to `value`.
bool env_is(const char* name, std::string_view value);

// Threads worth starting for parallel work: 1 without thread support.
unsigned hardware_threads();

// A path as UTF-8 with forward slashes, for logs and messages. (On Windows
// `path::string()` converts to the ANSI code page and can lose characters.)
std::string utf8(const std::filesystem::path& path);

// A UTF-8 string (e.g. from argv, project.toml or a script) as a path.
std::filesystem::path path_from_utf8(std::string_view text);

// The directory holding the running executable (SDL_GetBasePath), or empty
// if unknown. A release build finds its `game.pak` here (docs/EXPORT.md).
std::filesystem::path executable_dir();

// Finds an engine asset (e.g. "fonts/LiberationSans-Regular.ttf", "brand", "demo/robot.svg").
// Searches:
// 1. CUERVO_ASSETS_DIR environment variable
// 2. Compiled fallback path (if non-empty and exists on disk)
// 3. Upwards from executable_dir: <dir>/assets/<subpath>
// 4. <executable_dir>/../share/cuervo/assets/<subpath>
// 5. Upwards from current working directory: <dir>/assets/<subpath>
// Returns the first path that exists on disk, or the fallback path if none exist.
std::filesystem::path find_asset(std::string_view subpath, std::string_view fallback = {});

// The per-user writable directory for a game's saves (DESIGN.md 7.2 "Save
// data": SDL_GetPrefPath("Cuervo", <game>), e.g. ~/.local/share/Cuervo/<game>/
// on Linux, %APPDATA%/Cuervo/<game>/ on Windows, MEMFS on the web).
// CUERVO_USER_DIR overrides it (tests, portable installs). Created if
// missing; empty when no such directory can be had.
std::filesystem::path user_data_dir(std::string_view game);

} // namespace cuervo::platform

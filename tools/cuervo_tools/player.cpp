#include "tools/cuervo_tools/player.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>

#include "cuervo/platform/platform.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cuervo::tools {

namespace fs = std::filesystem;

namespace {

// FNV-1a 64, the same hash the pack format uses for its content
// (cuervo/assets/pack.hpp): no hashing library is pinned for the tools.
std::uint64_t fnv1a(std::string_view text, std::uint64_t h = 0xcbf29ce484222325ull) {
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

const char* on_off(bool v) { return v ? "ON" : "OFF"; }

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The last lines of a build log, for the error message.
std::string tail_lines(const std::string& text, std::size_t lines) {
    std::size_t pos = text.size();
    if (pos > 0 && text[pos - 1] == '\n') --pos;
    for (std::size_t n = 0; n < lines; ++n) {
        if (pos == 0) return text;
        const std::size_t nl = text.rfind('\n', pos - 1);
        if (nl == std::string::npos) return text;
        pos = nl;
    }
    return text.substr(pos + 1);
}

#if !defined(_WIN32)
// One build per cache entry at a time: two ninja runs in one build directory
// corrupt each other, and parallel exports of projects with the
// same features share an entry.
class FileLock {
public:
    explicit FileLock(const fs::path& file) {
        fd_ = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ >= 0) ::flock(fd_, LOCK_EX);
    }
    ~FileLock() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    int fd_ = -1;
};
#endif

} // namespace

std::optional<EngineChoice> parse_engine_choice(std::string_view text) {
    if (text == "auto") return EngineChoice::Auto;
    if (text == "full") return EngineChoice::Full;
    if (text == "stripped") return EngineChoice::Stripped;
    return std::nullopt;
}

std::string_view engine_choice_name(EngineChoice choice) {
    switch (choice) {
    case EngineChoice::Auto: return "auto";
    case EngineChoice::Full: return "full";
    case EngineChoice::Stripped: return "stripped";
    }
    return "auto";
}

std::string PlayerToolchain::missing(assets::PackPlatform platform) const {
    std::error_code ec;
    if (platform == assets::PackPlatform::Web) {
        if (emsdk.empty() || !fs::is_regular_file(emsdk / "upstream" / "emscripten" / "emcc", ec)) {
            return std::format("no emsdk at '{}' (--emsdk <dir>, or set EMSDK; docs/WEB.md)", platform::utf8(emsdk));
        }
        if (host_build.empty() || (!fs::is_regular_file(host_build / "shader_tool", ec) &&
                                   !fs::is_regular_file(host_build / "shader_tool.exe", ec))) {
            return std::format("no host-tools build at '{}' (cmake --preset host-tools && cmake --build --preset "
                               "host-tools, or --host-build <dir>)",
                               platform::utf8(host_build));
        }
    } else if (platform != assets::PackPlatform::Linux) {
        return std::format("building {} players isn't supported yet", assets::platform_name(platform));
    }
    if (source_dir.empty() || !fs::is_regular_file(source_dir / "CMakeLists.txt", ec)) {
        return std::format("the engine sources '{}' are not available", platform::utf8(source_dir));
    }
    if (cmake.empty() || !fs::is_regular_file(cmake, ec)) {
        return std::format("cmake '{}' not found (--cmake <path>)", platform::utf8(cmake));
    }
    if (!make_program.empty() && !fs::is_regular_file(make_program, ec)) {
        return std::format("the build tool '{}' not found", platform::utf8(make_program));
    }
    for (const fs::path* c : {&c_compiler, &cxx_compiler}) {
        if (!c->empty() && !fs::is_regular_file(*c, ec)) {
            return std::format("the compiler '{}' not found", platform::utf8(*c));
        }
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> web_build_environment(const fs::path& emsdk) {
    const char sep = platform::os == platform::Os::Windows ? ';' : ':';
    std::string path = platform::utf8(emsdk) + sep + platform::utf8(emsdk / "upstream" / "emscripten");
    // emsdk's own node (node/<version>/bin), so emcc doesn't depend on the host's.
    std::error_code ec;
    if (fs::is_directory(emsdk / "node", ec)) {
        std::vector<fs::path> nodes;
        for (const auto& e : fs::directory_iterator(emsdk / "node", ec)) nodes.push_back(e.path() / "bin");
        std::sort(nodes.begin(), nodes.end());
        if (!nodes.empty()) path += sep + platform::utf8(nodes.back());
    }
    if (auto p = platform::env("PATH"); p && !p->empty()) path += sep + *p;
    return {{"EMSDK", platform::utf8(emsdk)}, {"EM_CONFIG", platform::utf8(emsdk / ".emscripten")}, {"PATH", path}};
}

std::vector<std::string> player_cmake_definitions(const script::FeatureSet& f, assets::PackPlatform platform) {
    const bool web = platform == assets::PackPlatform::Web;
    std::vector<std::string> defs = {
        "CMAKE_BUILD_TYPE=Release",
        "CUERVO_BUILD_EDITOR=OFF",
        "CUERVO_BUILD_TESTS=OFF",
        // Shipping players carry no profiler zones (DESIGN.md 7.2 "Profiling",
        // 5.14 "Clarified (Phase 8 -- Profiler)").
        "CUERVO_PROFILING=OFF",
        std::format("CUERVO_FEATURE_AUDIO={}", on_off(f.audio)),
        std::format("CUERVO_FEATURE_COLLISION={}", on_off(f.collision)),
        std::format("CUERVO_FEATURE_DEV={}", on_off(f.dev)),
        std::format("CUERVO_FEATURE_PARTICLES={}", on_off(f.particles)),
        std::format("CUERVO_FEATURE_UI={}", on_off(f.ui)),
        std::format("CUERVO_FONT_IMPORT={}", on_off(f.font_import)),
        std::format("CUERVO_SVG_IMPORT={}", on_off(f.svg_import)),
        std::format("CUERVO_VULKAN={}", on_off(f.vulkan && !web)),
    };
    if (web) defs.push_back("CUERVO_WEB_PACK=fetch"); // loads the game.pak next to the page
    std::sort(defs.begin(), defs.end());
    return defs;
}

bool can_strip(const script::FeatureSet& f) {
    return !(f.audio && f.ui && f.particles && f.collision && f.vulkan && f.dev && f.svg_import && f.font_import);
}

std::string check_feature_dependencies(const script::FeatureSet& f) {
    if (f.particles && !f.collision) return "the particles feature needs collision (CUERVO_FEATURE_PARTICLES)";
    if (f.dev && !f.particles) return "the dev feature needs particles (CUERVO_FEATURE_DEV)";
    return {};
}

std::string player_cache_key(const script::FeatureSet& features, const PlayerToolchain& t,
                             assets::PackPlatform platform) {
    // Only what changes the produced binary goes in: not the cmake path or
    // the dependency checkouts (both pinned by the sources).
    std::string identity = std::format("cuervo-player/1\nplatform={}\nengine={}\nsource={}\ngenerator={}\ncc={}\ncxx={}\n",
                                       assets::platform_name(platform), t.engine_version,
                                       platform::utf8(t.source_dir.lexically_normal()), t.generator,
                                       platform::utf8(t.c_compiler), platform::utf8(t.cxx_compiler));
    for (const std::string& d : player_cmake_definitions(features, platform)) identity += d + "\n";
    if (platform == assets::PackPlatform::Web) {
        // The emscripten release decides the .wasm.
        identity += "emscripten=" + read_text(t.emsdk / "upstream" / "emscripten" / "emscripten-version.txt") + "\n";
    }

    std::string name;
    for (const std::string& f : features.active_list()) {
        if (!name.empty()) name += '+';
        name += f;
    }
    if (name.empty()) name = "core";
    return std::format("{}_{}_{:016x}", assets::platform_name(platform), name, fnv1a(identity));
}

std::vector<std::string> player_configure_command(const script::FeatureSet& features, const PlayerToolchain& t,
                                                  const fs::path& build_dir, assets::PackPlatform platform) {
    const bool web = platform == assets::PackPlatform::Web;
    std::vector<std::string> argv = {platform::utf8(t.cmake), "-S", platform::utf8(t.source_dir), "-B",
                                     platform::utf8(build_dir)};
    if (!t.generator.empty()) {
        argv.push_back("-G");
        argv.push_back(t.generator);
    }
    if (!t.make_program.empty()) argv.push_back("-DCMAKE_MAKE_PROGRAM=" + platform::utf8(t.make_program));
    if (web) {
        // Emscripten's compilers, from its toolchain file (the `web` preset's setup).
        argv.push_back("-DCMAKE_TOOLCHAIN_FILE=" +
                       platform::utf8(t.emsdk / "upstream" / "emscripten" / "cmake" / "Modules" / "Platform" /
                                      "Emscripten.cmake"));
        argv.push_back("-DCUERVO_HOST_BUILD=" + platform::utf8(t.host_build));
    } else {
        if (!t.c_compiler.empty()) argv.push_back("-DCMAKE_C_COMPILER=" + platform::utf8(t.c_compiler));
        if (!t.cxx_compiler.empty()) argv.push_back("-DCMAKE_CXX_COMPILER=" + platform::utf8(t.cxx_compiler));
    }
    for (const std::string& d : player_cmake_definitions(features, platform)) argv.push_back("-D" + d);
    // Reuse the pinned dependency checkouts of the tools' own build tree
    // (sorted, so the command line is the same every time).
    std::error_code ec;
    std::vector<std::string> deps;
    if (!t.deps_dir.empty() && fs::is_directory(t.deps_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(t.deps_dir, ec)) {
            const std::string name = platform::utf8(entry.path().filename());
            if (name.size() > 4 && name.ends_with("-src") && entry.is_directory(ec)) {
                deps.push_back(std::format("-DFETCHCONTENT_SOURCE_DIR_{}={}", upper(name.substr(0, name.size() - 4)),
                                           platform::utf8(entry.path())));
            }
        }
    }
    std::sort(deps.begin(), deps.end());
    argv.insert(argv.end(), deps.begin(), deps.end());
    return argv;
}

fs::path default_player_cache_dir() {
    if (auto env = platform::env("CUERVO_PLAYER_CACHE"); env && !env->empty()) return platform::path_from_utf8(*env);
    if (auto xdg = platform::env("XDG_CACHE_HOME"); xdg && !xdg->empty()) {
        return platform::path_from_utf8(*xdg) / "cuervo" / "players";
    }
    if (auto home = platform::env("HOME"); home && !home->empty()) {
        return platform::path_from_utf8(*home) / ".cache" / "cuervo" / "players";
    }
    return fs::temp_directory_path() / "cuervo-players";
}

std::optional<ProcessResult> run_process(const std::vector<std::string>& argv, const fs::path& cwd,
                                         const std::vector<std::pair<std::string, std::string>>& env,
                                         const fs::path& log, std::chrono::seconds timeout, std::string& error) {
#if defined(_WIN32)
    (void)argv;
    (void)cwd;
    (void)env;
    (void)log;
    (void)timeout;
    error = "running build tools from cuervo-tools is not supported on Windows yet";
    return std::nullopt;
#else
    if (argv.empty()) {
        error = "run_process: no program";
        return std::nullopt;
    }
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    const std::string log_path = platform::utf8(log);
    const std::string cwd_path = platform::utf8(cwd);

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed";
        return std::nullopt;
    }
    if (pid == 0) {
        // Child: only async-signal-safe calls plus setenv before exec.
        const int out = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int in = ::open("/dev/null", O_RDONLY);
        if (out < 0 || in < 0) ::_exit(126);
        ::dup2(in, 0);
        ::dup2(out, 1);
        ::dup2(out, 2);
        if (!cwd_path.empty() && ::chdir(cwd_path.c_str()) != 0) ::_exit(126);
        for (const auto& [k, v] : env) ::setenv(k.c_str(), v.c_str(), 1);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }

    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            error = "waitpid failed";
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            result.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else result.exit_code = -1;
    result.output = read_text(log);
    if (result.exit_code == 127 && !result.timed_out && result.output.empty()) {
        error = std::format("can't run '{}'", argv[0]);
        return std::nullopt;
    }
    return result;
#endif
}

std::optional<PlayerBuild> build_player(const script::FeatureSet& features, const PlayerToolchain& toolchain,
                                        const fs::path& cache_root, assets::PackPlatform platform,
                                        const Progress& progress, std::string& error) {
    if (std::string why = toolchain.missing(platform); !why.empty()) {
        error = "can't build a stripped player: " + why;
        return std::nullopt;
    }
    const bool web = platform == assets::PackPlatform::Web;
    if ((platform != assets::PackPlatform::Linux && !web) || assets::host_platform() != assets::PackPlatform::Linux) {
        // Windows needs a Windows host (docs/EXPORT.md "Stripped players").
        error = std::format("building a stripped player for {} is not supported yet (Linux and web, on a Linux "
                            "host); use --engine full",
                            assets::platform_name(platform));
        return std::nullopt;
    }
    if (std::string why = check_feature_dependencies(features); !why.empty()) {
        error = "can't build a stripped player: " + why;
        return std::nullopt;
    }

    PlayerBuild build;
    build.key = player_cache_key(features, toolchain, platform);
    build.build_dir = cache_root / build.key;
    build.log = build.build_dir / "player-build.log";
    std::error_code ec;
    fs::create_directories(build.build_dir, ec);
    if (ec) {
        error = std::format("can't create '{}': {}", platform::utf8(build.build_dir), ec.message());
        return std::nullopt;
    }
#if !defined(_WIN32)
    FileLock lock(cache_root / (build.key + ".lock"));
#endif

    // Configure once; afterwards `cmake --build` re-runs the configure step by
    // itself whenever a CMakeLists changed, so the cache entry tracks the sources.
    const auto timeout = std::chrono::seconds(3600);
    const std::vector<std::pair<std::string, std::string>> env =
        web ? web_build_environment(toolchain.emsdk) : std::vector<std::pair<std::string, std::string>>{};
    if (!fs::is_regular_file(build.build_dir / "CMakeCache.txt", ec)) {
        if (progress) progress(std::format("configuring a stripped player in {}", platform::utf8(build.build_dir)));
        auto configured = run_process(player_configure_command(features, toolchain, build.build_dir, platform), {},
                                      env, build.log, timeout, error);
        if (!configured) return std::nullopt;
        if (configured->exit_code != 0) {
            // A half-configured tree would be "reused" next time.
            fs::remove(build.build_dir / "CMakeCache.txt", ec);
            error = std::format("configuring the stripped player failed (log: {}):\n{}", platform::utf8(build.log),
                                tail_lines(configured->output, 30));
            return std::nullopt;
        }
        build.configured = true;
    }
    if (progress) progress(std::format("building the stripped player ({})", build.key));
    const fs::path build_log = build.build_dir / "player-build-step.log";
    auto built = run_process({platform::utf8(toolchain.cmake), "--build", platform::utf8(build.build_dir), "--target",
                              "runtime"},
                             {}, env, build_log, timeout, error);
    if (!built) return std::nullopt;
    {
        // One log with both steps when this run configured.
        std::ofstream log(build.log, build.configured ? std::ios::app : std::ios::trunc);
        log << built->output;
    }
    fs::remove(build_log, ec);
    if (built->exit_code != 0) {
        error = std::format("building the stripped player failed (log: {}):\n{}", platform::utf8(build.log),
                            tail_lines(built->output, 30));
        return std::nullopt;
    }
    // Web: the page directory (index.html, index.js, index.wasm) is the "player".
    build.executable = web ? build.build_dir / "runtime" : build.build_dir / "runtime" / "cuervo";
    if (!fs::exists(web ? build.executable / "index.wasm" : build.executable, ec)) {
        error = std::format("the stripped player build produced no '{}'", platform::utf8(build.executable));
        return std::nullopt;
    }
    return build;
}

} // namespace cuervo::tools

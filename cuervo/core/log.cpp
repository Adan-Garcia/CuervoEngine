#include "cuervo/core/log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace cuervo::log {

namespace {

Level g_min_level =
#ifdef NDEBUG
    Level::Info;
#else
    Level::Debug;
#endif

std::mutex g_mutex;
std::atomic<bool> g_stdout_free{false};
Sink g_sink = nullptr;
void* g_sink_user = nullptr;

const char* level_name(Level level) {
    switch (level) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info:  return "info ";
        case Level::Warn:  return "warn ";
        case Level::Error: return "error";
    }
    return "?????";
}

// Trim the path down to the part after the repository root folders we know.
std::string_view short_path(std::string_view path) {
    // Both separators: MSVC and MinGW spell __FILE__ with backslashes.
    for (std::string_view root : {"cuervo/", "runtime/", "pluma/", "tests/", "tools/", "editor/", "cuervo\\",
                                  "runtime\\", "pluma\\", "tests\\", "tools\\", "editor\\"}) {
        if (auto pos = path.rfind(root); pos != std::string_view::npos) {
            return path.substr(pos);
        }
    }
    return path;
}

} // namespace

void set_min_level(Level level) { g_min_level = level; }

void set_stdout_free(bool free) { g_stdout_free.store(free); }
Level min_level() { return g_min_level; }

void write(Level level, const char* file, int line, std::string_view message) {
    if (level < g_min_level) {
        return;
    }
    std::FILE* out = level >= Level::Warn || g_stdout_free.load() ? stderr : stdout;
    std::string_view path = short_path(file);
    std::lock_guard lock(g_mutex);
    std::fprintf(out, "[%s] %.*s:%d: %.*s\n", level_name(level),
                 static_cast<int>(path.size()), path.data(), line,
                 static_cast<int>(message.size()), message.data());
    std::fflush(out);
    if (g_sink != nullptr) g_sink(g_sink_user, level, path, line, message);
}

void set_sink(Sink sink, void* user) {
    std::lock_guard lock(g_mutex);
    g_sink = sink;
    g_sink_user = user;
}

} // namespace cuervo::log

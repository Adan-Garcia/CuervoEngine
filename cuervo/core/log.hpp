#pragma once

#include <format>
#include <string_view>

namespace cuervo::log {

enum class Level { Trace, Debug, Info, Warn, Error };

void set_min_level(Level level);
Level min_level();

// Info and below go to stdout unless this is on; then everything goes to
// stderr (the debug adapter speaks DAP on stdout: runtime/dap_cli.hpp).
void set_stdout_free(bool free);

// Writes one formatted line: "[level] file:line: message". Use the macros below.
void write(Level level, const char* file, int line, std::string_view message);

// An extra destination for every line that passes the level filter (the
// editor's console panel). Called with the log lock held, from whichever
// thread logged; it must not log itself. Null removes it.
using Sink = void (*)(void* user, Level level, std::string_view file, int line, std::string_view message);
void set_sink(Sink sink, void* user);

template<typename... Args>
void writef(Level level, const char* file, int line, std::format_string<Args...> fmt, Args&&... args) {
    if (level < min_level()) {
        return;
    }
    write(level, file, line, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace cuervo::log

#define LOG_TRACE(...) ::cuervo::log::writef(::cuervo::log::Level::Trace, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...) ::cuervo::log::writef(::cuervo::log::Level::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::cuervo::log::writef(::cuervo::log::Level::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::cuervo::log::writef(::cuervo::log::Level::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::cuervo::log::writef(::cuervo::log::Level::Error, __FILE__, __LINE__, __VA_ARGS__)

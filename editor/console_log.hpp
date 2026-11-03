#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "cuervo/core/log.hpp"

// The console panel's backing store: every LOG_* line (engine, scripts'
// print through LOG_INFO "[script]", the editor itself) while installed.
// Lines can arrive from job threads, hence the mutex; the panel copies them
// out once per frame.
namespace cuervo::editor {

struct ConsoleLine {
    log::Level level = log::Level::Info;
    std::string source; // "file:line"
    std::string text;
};

// Console filter chips: bit 1 = info and below, 2 = warn, 4 = error.
constexpr int console_filter_bit(log::Level level) {
    return level == log::Level::Error ? 4 : level == log::Level::Warn ? 2 : 1;
}
constexpr bool console_filter_shows(int mask, log::Level level) { return (mask & console_filter_bit(level)) != 0; }

class ConsoleLog {
public:
    explicit ConsoleLog(std::size_t capacity = 2000) : capacity_(capacity) {}
    ~ConsoleLog() { uninstall(); }
    ConsoleLog(const ConsoleLog&) = delete;
    ConsoleLog& operator=(const ConsoleLog&) = delete;

    // Becomes the log sink (cuervo/core/log.hpp); one at a time.
    void install();
    void uninstall();

    void add(log::Level level, std::string source, std::string text);
    void clear();
    // Copies the lines (oldest first) if anything changed since `seen`.
    bool snapshot(std::uint64_t& seen, std::vector<ConsoleLine>& out) const;
    std::size_t count(log::Level level) const;

private:
    mutable std::mutex mutex_;
    std::deque<ConsoleLine> lines_;
    std::size_t capacity_;
    std::uint64_t version_ = 0;
    bool installed_ = false;
};

} // namespace cuervo::editor

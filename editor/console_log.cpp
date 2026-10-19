#include "editor/console_log.hpp"

#include <algorithm>
#include <format>

namespace cuervo::editor {

namespace {

void sink(void* user, log::Level level, std::string_view file, int line, std::string_view message) {
    static_cast<ConsoleLog*>(user)->add(level, std::format("{}:{}", file, line), std::string(message));
}

} // namespace

void ConsoleLog::install() {
    log::set_sink(&sink, this);
    installed_ = true;
}

void ConsoleLog::uninstall() {
    if (!installed_) return;
    log::set_sink(nullptr, nullptr);
    installed_ = false;
}

void ConsoleLog::add(log::Level level, std::string source, std::string text) {
    std::lock_guard lock(mutex_);
    lines_.push_back({level, std::move(source), std::move(text)});
    while (lines_.size() > capacity_) lines_.pop_front();
    ++version_;
}

void ConsoleLog::clear() {
    std::lock_guard lock(mutex_);
    lines_.clear();
    ++version_;
}

bool ConsoleLog::snapshot(std::uint64_t& seen, std::vector<ConsoleLine>& out) const {
    std::lock_guard lock(mutex_);
    if (seen == version_ && !out.empty()) return false;
    out.assign(lines_.begin(), lines_.end());
    seen = version_;
    return true;
}

std::size_t ConsoleLog::count(log::Level level) const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(lines_.begin(), lines_.end(), [&](const ConsoleLine& l) { return l.level == level; }));
}

} // namespace cuervo::editor

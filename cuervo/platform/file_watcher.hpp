#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Development-time file watching for hot reload (DESIGN.md 4.11, 5.13).
//
// Portable polling by modification time: no inotify (Linux-only) or
// ReadDirectoryChangesW (Windows-only), the same code on both, and polling a
// few dozen script files twice a second is cheap. Editors that save through
// a temporary file + rename still change the mtime. On the web there is
// nothing to watch (platform::has_file_watching): `changed()` is always false.
//
// Wall clock and file system never reach the simulation; the caller decides
// when to poll.
namespace cuervo::platform {

class FileWatcher {
public:
    // Watches `files` plus the set of files with `extension` under `root`
    // (recursive), so added and deleted files count as changes too.
    void reset(std::vector<std::filesystem::path> files, std::filesystem::path root = {},
               std::string extension = {});

    // True when a watched file's modification time changed or the number of
    // matching files under the root changed since the last reset().
    bool changed() const;

    const std::vector<std::filesystem::path>& files() const { return files_; }

private:
    std::size_t count_matching() const;

    std::vector<std::filesystem::path> files_;
    std::vector<std::filesystem::file_time_type> stamps_;
    std::filesystem::path root_;
    std::string extension_;
    std::size_t matching_ = 0;
};

} // namespace cuervo::platform

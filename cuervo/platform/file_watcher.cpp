#include "cuervo/platform/file_watcher.hpp"

#include "cuervo/platform/platform.hpp"

namespace cuervo::platform {

namespace fs = std::filesystem;

namespace {

fs::file_time_type stamp(const fs::path& p) {
    std::error_code ec; // a missing file stamps as the epoch: deleting it is a change
    const fs::file_time_type t = fs::last_write_time(p, ec);
    return ec ? fs::file_time_type{} : t;
}

} // namespace

void FileWatcher::reset(std::vector<fs::path> files, fs::path root, std::string extension) {
    files_ = std::move(files);
    root_ = std::move(root);
    extension_ = std::move(extension);
    stamps_.clear();
    if constexpr (!has_file_watching) return;
    for (const fs::path& f : files_) stamps_.push_back(stamp(f));
    matching_ = count_matching();
}

bool FileWatcher::changed() const {
    if constexpr (!has_file_watching) {
        return false;
    } else {
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (stamp(files_[i]) != stamps_[i]) return true;
        }
        return count_matching() != matching_;
    }
}

std::size_t FileWatcher::count_matching() const {
    if (root_.empty()) return 0;
    std::size_t count = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && (extension_.empty() || it->path().extension() == extension_)) ++count;
    }
    return count;
}

} // namespace cuervo::platform

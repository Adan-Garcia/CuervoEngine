#include "editor/asset_library.hpp"

#include <algorithm>
#include <format>

#include "cuervo/platform/platform.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

std::string_view asset_kind_name(AssetKind kind) {
    switch (kind) {
        case AssetKind::Folder: return "folder";
        case AssetKind::Scene: return "scene";
        case AssetKind::Script: return "script";
        case AssetKind::Font: return "font";
        case AssetKind::Sound: return "sound";
        case AssetKind::Vector: return "vector";
        case AssetKind::Image: return "image";
        case AssetKind::Replay: return "replay";
        case AssetKind::Project: return "project";
        case AssetKind::Other: return "file";
    }
    return "file";
}

AssetKind asset_kind_of(const fs::path& path) {
    if (path.filename() == "project.toml") return AssetKind::Project;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".scene") return AssetKind::Scene;
    if (ext == ".plm") return AssetKind::Script;
    if (ext == ".ttf" || ext == ".otf" || ext == ".efont") return AssetKind::Font;
    if (ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3") return AssetKind::Sound;
    if (ext == ".svg" || ext == ".evec") return AssetKind::Vector;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") return AssetKind::Image;
    if (ext == ".replay") return AssetKind::Replay;
    return AssetKind::Other;
}

void AssetLibrary::open(const fs::path& project_dir) {
    dir_ = project_dir;
    stamps_.clear();
    poll();
}

fs::path AssetLibrary::absolute(std::string_view path) const {
    return path.empty() ? dir_ : dir_ / platform::path_from_utf8(path);
}

void AssetLibrary::scan(AssetEntry& folder, std::vector<Stamp>& stamps) const {
    std::error_code ec;
    std::vector<AssetEntry> dirs;
    std::vector<AssetEntry> files;
    for (fs::directory_iterator it(absolute(folder.path), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = platform::utf8(it->path().filename());
        if (name.empty() || name.front() == '.') continue; // .git, .import, editor temp files
        AssetEntry e;
        e.name = name;
        e.path = folder.path.empty() ? name : folder.path + "/" + name;
        std::error_code ec2;
        if (it->is_directory(ec2)) {
            e.kind = AssetKind::Folder;
            dirs.push_back(std::move(e));
        } else if (it->is_regular_file(ec2)) {
            e.kind = asset_kind_of(it->path());
            stamps.push_back({e.path, it->file_size(ec2), it->last_write_time(ec2)});
            files.push_back(std::move(e));
        }
    }
    auto by_name = [](const AssetEntry& a, const AssetEntry& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    for (AssetEntry& d : dirs) {
        stamps.push_back({d.path + "/", 0, {}});
        scan(d, stamps);
    }
    folder.children = std::move(dirs);
    folder.children.insert(folder.children.end(), std::make_move_iterator(files.begin()),
                           std::make_move_iterator(files.end()));
}

bool AssetLibrary::poll(std::vector<std::string>* changed) {
    AssetEntry root;
    root.kind = AssetKind::Folder;
    root.name = platform::utf8(dir_.filename());
    std::vector<Stamp> stamps;
    scan(root, stamps);
    std::sort(stamps.begin(), stamps.end(), [](const Stamp& a, const Stamp& b) { return a.path < b.path; });
    if (stamps == stamps_ && revision_ > 0) return false;
    if (changed != nullptr) {
        // Paths added, removed or modified (both lists are sorted by path).
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < stamps.size() || j < stamps_.size()) {
            if (j == stamps_.size() || (i < stamps.size() && stamps[i].path < stamps_[j].path)) {
                changed->push_back(stamps[i++].path);
            } else if (i == stamps.size() || stamps_[j].path < stamps[i].path) {
                changed->push_back(stamps_[j++].path);
            } else {
                if (!(stamps[i] == stamps_[j])) changed->push_back(stamps[i].path);
                ++i;
                ++j;
            }
        }
    }
    stamps_ = std::move(stamps);
    root_ = std::move(root);
    ++revision_;
    return true;
}

const AssetEntry* AssetLibrary::find(std::string_view path) const {
    const AssetEntry* e = &root_;
    if (path.empty()) return e;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        std::size_t slash = path.find('/', pos);
        if (slash == std::string_view::npos) slash = path.size();
        const std::string_view part = path.substr(pos, slash - pos);
        auto it = std::find_if(e->children.begin(), e->children.end(), [&](const AssetEntry& c) { return c.name == part; });
        if (it == e->children.end()) return nullptr;
        e = &*it;
        pos = slash + 1;
    }
    return e;
}

namespace {

void collect(const AssetEntry& e, AssetKind kind, std::vector<std::string>& out) {
    if (e.kind == kind) out.push_back(e.path);
    for (const AssetEntry& c : e.children) collect(c, kind, out);
}

} // namespace

std::vector<std::string> AssetLibrary::files_of(AssetKind kind) const {
    std::vector<std::string> out;
    collect(root_, kind, out);
    std::sort(out.begin(), out.end());
    return out;
}

bool AssetLibrary::valid_name(std::string_view name, std::string& error) const {
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:*?\"<>|") != std::string_view::npos) {
        error = std::format("'{}' is not a valid file name", name);
        return false;
    }
    if (name.front() == '.') {
        error = "names starting with '.' are hidden; pick another name";
        return false;
    }
    return true;
}

bool AssetLibrary::create_folder(std::string_view parent, std::string_view name, std::string& error) {
    const AssetEntry* p = find(parent);
    if (p == nullptr || p->kind != AssetKind::Folder) {
        error = std::format("no folder '{}'", parent);
        return false;
    }
    if (!valid_name(name, error)) return false;
    const fs::path target = absolute(parent) / platform::path_from_utf8(name);
    std::error_code ec;
    if (fs::exists(target, ec)) {
        error = std::format("'{}' already exists", name);
        return false;
    }
    if (!fs::create_directory(target, ec)) {
        error = std::format("cannot create '{}': {}", name, ec.message());
        return false;
    }
    poll();
    return true;
}

bool AssetLibrary::rename(std::string_view path, std::string_view name, std::string& error) {
    const AssetEntry* e = find(path);
    if (e == nullptr || path.empty()) {
        error = std::format("no file or folder '{}'", path);
        return false;
    }
    if (e->kind == AssetKind::Project) {
        error = "project.toml can't be renamed (the project is found by it)";
        return false;
    }
    if (!valid_name(name, error)) return false;
    const fs::path from = absolute(path);
    const fs::path to = from.parent_path() / platform::path_from_utf8(name);
    std::error_code ec;
    if (fs::exists(to, ec)) {
        error = std::format("'{}' already exists", name);
        return false;
    }
    fs::rename(from, to, ec);
    if (ec) {
        error = std::format("cannot rename '{}': {}", path, ec.message());
        return false;
    }
    poll();
    return true;
}

bool AssetLibrary::remove(std::string_view path, std::string& error) {
    const AssetEntry* e = find(path);
    if (e == nullptr || path.empty()) {
        error = path.empty() ? std::string("the project folder can't be deleted from the editor")
                             : std::format("no file or folder '{}'", path);
        return false;
    }
    if (e->kind == AssetKind::Project) {
        error = "project.toml can't be deleted from the editor";
        return false;
    }
    std::error_code ec;
    fs::remove_all(absolute(path), ec);
    if (ec) {
        error = std::format("cannot delete '{}': {}", path, ec.message());
        return false;
    }
    poll();
    return true;
}

} // namespace cuervo::editor

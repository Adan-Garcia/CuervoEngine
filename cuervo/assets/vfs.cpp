#include "cuervo/assets/vfs.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>

#include "cuervo/platform/platform.hpp"

namespace cuervo::assets {

namespace fs = std::filesystem;

namespace {

bool hidden(std::string_view path) {
    for (std::size_t pos = 0; pos < path.size();) {
        if (path[pos] == '.') return true;
        const std::size_t slash = path.find('/', pos);
        if (slash == std::string_view::npos) break;
        pos = slash + 1;
    }
    return false;
}

bool read_real(const fs::path& file, std::vector<std::byte>& out) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
}

bool under(std::string_view path, std::string_view dir) {
    if (dir.empty()) return true;
    return path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/';
}

} // namespace

bool path_component_less(std::string_view a, std::string_view b) {
    // Compare component by component, like std::filesystem::path::compare.
    std::size_t i = 0;
    std::size_t j = 0;
    while (i <= a.size() && j <= b.size()) {
        if (i == a.size() && j == b.size()) return false;
        if (i == a.size()) return true;
        if (j == b.size()) return false;
        std::size_t ie = a.find('/', i);
        std::size_t je = b.find('/', j);
        if (ie == std::string_view::npos) ie = a.size();
        if (je == std::string_view::npos) je = b.size();
        const std::string_view pa = a.substr(i, ie - i);
        const std::string_view pb = b.substr(j, je - j);
        if (pa != pb) return pa < pb;
        i = ie == a.size() ? a.size() : ie + 1;
        j = je == b.size() ? b.size() : je + 1;
    }
    return false;
}

std::shared_ptr<Vfs> Vfs::dev(const fs::path& project_dir) {
    auto vfs = std::make_shared<Vfs>();
    vfs->mount_dir(project_dir);
    vfs->set_user_dir(project_dir);
    return vfs;
}

std::shared_ptr<Vfs> Vfs::release(std::shared_ptr<const Pack> pack, fs::path user_dir) {
    auto vfs = std::make_shared<Vfs>();
    vfs->mount_pack(std::move(pack));
    vfs->set_user_dir(std::move(user_dir));
    return vfs;
}

void Vfs::mount_dir(fs::path root) { mounts_.push_back({std::move(root), nullptr}); }

void Vfs::mount_pack(std::shared_ptr<const Pack> pack) { mounts_.push_back({{}, std::move(pack)}); }

bool Vfs::normalize(std::string_view path, std::string& out, std::string& error) {
    std::string p(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    out.clear();
    for (std::size_t pos = 0; pos <= p.size();) {
        std::size_t end = p.find('/', pos);
        if (end == std::string::npos) end = p.size();
        const std::string_view part = std::string_view(p).substr(pos, end - pos);
        pos = end + 1;
        if (part.empty()) {
            if (end == 0) {
                error = std::format("'{}': game paths are relative to the project", path);
                return false;
            }
            continue; // "a//b", trailing '/'
        }
        if (part == ".") continue;
        if (part == "..") {
            error = std::format("'{}': game paths can't leave the project", path);
            return false;
        }
        if (!out.empty()) out += '/';
        out += part;
    }
    if (out.empty() || (out.size() >= 2 && out[1] == ':')) {
        error = std::format("'{}': not a project-relative path", path);
        return false;
    }
    return true;
}

void Vfs::add_derived(std::string source_extension, std::string output_extension, Convert convert) {
    derived_.push_back({std::move(source_extension), std::move(output_extension), std::move(convert)});
}

const Vfs::Derived* Vfs::derived_for(const std::string& path, std::string& source) const {
    for (const Derived& d : derived_) {
        if (path.size() <= d.output_extension.size() || !path.ends_with(d.output_extension)) continue;
        source = path.substr(0, path.size() - d.output_extension.size()) + d.source_extension;
        if (exists_mounted(source)) return &d;
    }
    return nullptr;
}

// An unreadable file is reported as not found; open() then explains it.
std::optional<VfsFile> Vfs::open_mounted(const std::string& p) const {
    for (const Mount& m : mounts_) {
        if (m.pack) {
            if (auto data = m.pack->find(p)) {
                VfsFile f;
                f.view_ = *data;
                f.pack_ = m.pack;
                return f;
            }
            continue;
        }
        const fs::path file = m.dir / platform::path_from_utf8(p);
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) continue;
        VfsFile f;
        if (!read_real(file, f.owned_)) return std::nullopt;
        f.view_ = f.owned_;
        return f;
    }
    return std::nullopt;
}

bool Vfs::exists_mounted(const std::string& p) const {
    for (const Mount& m : mounts_) {
        std::error_code ec;
        if (m.pack ? m.pack->contains(p) : fs::is_regular_file(m.dir / platform::path_from_utf8(p), ec)) return true;
    }
    return false;
}

std::optional<VfsFile> Vfs::open(std::string_view path, std::string& error) const {
    std::string p;
    if (!normalize(path, p, error)) return std::nullopt;
    if (auto f = open_mounted(p)) return f;
    if (exists_mounted(p)) {
        error = std::format("can't read '{}'", describe(p));
        return std::nullopt;
    }
    std::string source;
    if (const Derived* d = derived_for(p, source)) {
        auto in = open_mounted(source);
        if (!in) {
            error = std::format("can't read '{}'", describe(source));
            return std::nullopt;
        }
        VfsFile f;
        std::string why;
        if (!d->convert(source, in->bytes(), f.owned_, why)) {
            error = std::move(why); // the converter names the file
            return std::nullopt;
        }
        f.view_ = f.owned_;
        return f;
    }
    std::string searched;
    for (const Mount& m : mounts_) {
        if (!searched.empty()) searched += ", ";
        searched += m.pack ? std::format("pack '{}'", m.pack->source()) : std::format("'{}'", platform::utf8(m.dir));
    }
    error = std::format("'{}' not found (searched {})", p, searched.empty() ? std::string("nothing") : searched);
    return std::nullopt;
}

bool Vfs::read_text(std::string_view path, std::string& out, std::string& error) const {
    auto f = open(path, error);
    if (!f) return false;
    out.assign(f->text());
    return true;
}

bool Vfs::exists(std::string_view path) const {
    std::string p;
    std::string error;
    if (!normalize(path, p, error)) return false;
    std::string source;
    return exists_mounted(p) || derived_for(p, source) != nullptr;
}

std::vector<std::string> Vfs::list(std::string_view dir_in, std::string_view extension) const {
    std::string dir;
    std::string error;
    if (!dir_in.empty() && dir_in != "." && !normalize(dir_in, dir, error)) return {};
    std::vector<std::string> out;
    auto consider = [&](std::string path) {
        if (!under(path, dir) || hidden(path)) return;
        for (const Derived& d : derived_) {
            if (path.size() > d.source_extension.size() && path.ends_with(d.source_extension)) {
                std::string made = path.substr(0, path.size() - d.source_extension.size()) + d.output_extension;
                if (extension.empty() || made.ends_with(extension)) out.push_back(std::move(made));
            }
        }
        if (!extension.empty() && !path.ends_with(extension)) return;
        out.push_back(std::move(path));
    };
    for (const Mount& m : mounts_) {
        if (m.pack) {
            for (const PackEntry& e : m.pack->entries()) consider(std::string(e.path));
            continue;
        }
        const fs::path root = dir.empty() ? m.dir : m.dir / platform::path_from_utf8(dir);
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            consider(platform::utf8(it->path().lexically_relative(m.dir)));
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return path_component_less(a, b); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<fs::path> Vfs::real_path(std::string_view path) const {
    std::string p;
    std::string error;
    if (!normalize(path, p, error)) return std::nullopt;
    for (const Mount& m : mounts_) {
        if (m.pack) {
            if (m.pack->contains(p)) return std::nullopt;
            continue;
        }
        const fs::path file = m.dir / platform::path_from_utf8(p);
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) return file;
    }
    return std::nullopt;
}

std::string Vfs::describe(std::string_view path) const {
    std::string p;
    std::string error;
    if (!normalize(path, p, error)) return std::string(path);
    for (const Mount& m : mounts_) {
        if (m.pack) {
            if (m.pack->contains(p)) return std::format("{}:{}", m.pack->source().empty() ? "pack" : m.pack->source(), p);
            continue;
        }
        const fs::path file = m.dir / platform::path_from_utf8(p);
        std::error_code ec;
        if (fs::exists(file, ec)) return platform::utf8(file);
    }
    // Not found: name it where the first mount would have it.
    if (!mounts_.empty() && !mounts_.front().pack) return platform::utf8(mounts_.front().dir / platform::path_from_utf8(p));
    return p;
}

bool Vfs::packed() const {
    return std::any_of(mounts_.begin(), mounts_.end(), [](const Mount& m) { return m.pack != nullptr; });
}

std::shared_ptr<const Pack> Vfs::pack() const {
    for (const Mount& m : mounts_) {
        if (m.pack) return m.pack;
    }
    return nullptr;
}

fs::path Vfs::root_dir() const {
    for (const Mount& m : mounts_) {
        if (!m.pack) return m.dir;
    }
    return {};
}

} // namespace cuervo::assets

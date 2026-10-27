#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/assets/pack.hpp"

// The virtual file system every engine and game file is read through
// (docs/EXPORT.md "VFS rules"): project.toml, scripts, scenes, sounds, fonts
// and vectors. It resolves project-relative paths ("scenes/title.scene")
// against an ordered list of mounts, first match wins:
//   - development: the project directory (`Vfs::dev`);
//   - release:     the game's pack (`Vfs::release`).
// Saves and other user data are *not* game data: they live on the real file
// system in the per-user directory (`user_dir`, platform::user_data_dir) in
// release, and in the project directory in development (where Scene.save has
// always written). Hot reload and file watching are development-only:
// `real_path` is how dev tools find the file behind a path, and a pack has none.
namespace cuervo::assets {

// A file's bytes: a read-only view into a pack (no copy) or a buffer read
// from disk. Keeps the pack alive while the view exists.
class VfsFile {
public:
    std::span<const std::byte> bytes() const { return view_; }
    std::string_view text() const { return {reinterpret_cast<const char*>(view_.data()), view_.size()}; }
    std::size_t size() const { return view_.size(); }

private:
    friend class Vfs;
    std::span<const std::byte> view_;
    std::vector<std::byte> owned_;
    std::shared_ptr<const Pack> pack_;
};

class Vfs {
public:
    // A project directory, with Scene.save writing into it (development).
    static std::shared_ptr<Vfs> dev(const std::filesystem::path& project_dir);
    // A pack, with saves going to `user_dir` (release). An empty user_dir
    // disables saving.
    static std::shared_ptr<Vfs> release(std::shared_ptr<const Pack> pack, std::filesystem::path user_dir);

    // Mounts are searched in the order they were added.
    void mount_dir(std::filesystem::path root);
    void mount_pack(std::shared_ptr<const Pack> pack);

    // Files derived by a converter (plugin importers, DESIGN.md 4.18): a path
    // ending with `output_extension` that no mount has is read by converting
    // the same path with `source_extension` ("levels/a.scene" from
    // "levels/a.csv"). Development only: export stores the converted file in
    // the pack instead, so a release game never runs a converter. A failed
    // conversion's message is open()'s error.
    using Convert = std::function<bool(std::string_view source_path, std::span<const std::byte> in,
                                       std::vector<std::byte>& out, std::string& error)>;
    void add_derived(std::string source_extension, std::string output_extension, Convert convert);
    void set_user_dir(std::filesystem::path dir) { user_dir_ = std::move(dir); }

    // Normalizes a game path ("./a\\b.plm" -> "a/b.plm"); false + `error` for
    // absolute paths, ".." or empty components.
    static bool normalize(std::string_view path, std::string& out, std::string& error);

    // The file at `path`, or nullopt + `error` ("not found ... (searched ...)").
    std::optional<VfsFile> open(std::string_view path, std::string& error) const;
    bool read_text(std::string_view path, std::string& out, std::string& error) const;
    bool exists(std::string_view path) const;
    // Every file under `dir` ("" = everything), recursive, whose name ends
    // with `extension` (if given), from every mount (deduplicated), sorted in
    // path-component order (what sorting std::filesystem paths gives, so dev
    // and release see the same order). Hidden files and directories (a
    // component starting with '.') are never listed: packs don't contain them.
    std::vector<std::string> list(std::string_view dir, std::string_view extension = {}) const;

    // The real file behind `path` when it comes from a directory mount
    // (file watching, editor tools); nullopt for pack entries.
    std::optional<std::filesystem::path> real_path(std::string_view path) const;
    // Where `path` comes from, for messages: a real path or "<pack>:<path>".
    std::string describe(std::string_view path) const;

    // Where Scene.save and future save data go (real file system); empty
    // when saving is disabled.
    const std::filesystem::path& user_dir() const { return user_dir_; }
    // True when any mount is a pack (a release game: no hot reload).
    bool packed() const;
    // The first mounted pack (manifest, platform checks), or null.
    std::shared_ptr<const Pack> pack() const;
    // The first mounted directory (the project in development), or empty.
    std::filesystem::path root_dir() const;

private:
    struct Mount {
        std::filesystem::path dir;
        std::shared_ptr<const Pack> pack;
    };
    struct Derived {
        std::string source_extension;
        std::string output_extension;
        Convert convert;
    };
    std::optional<VfsFile> open_mounted(const std::string& path) const;
    bool exists_mounted(const std::string& path) const;
    // The derivation that makes `path` and its source path, if any.
    const Derived* derived_for(const std::string& path, std::string& source) const;
    std::vector<Mount> mounts_;
    std::vector<Derived> derived_;
    std::filesystem::path user_dir_;
};

// Ordering of game paths by component ("a/b" < "a-b"), the order sorting
// std::filesystem paths gives.
bool path_component_less(std::string_view a, std::string_view b);

} // namespace cuervo::assets

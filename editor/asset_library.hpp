#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The asset browser's model (DESIGN.md 4.13): the project directory as a
// tree of folders and files, classified by kind, with the file operations
// the browser offers (create folder, rename, delete). Paths are
// project-relative with '/' separators; no operation may leave the project.
// Hidden entries (".git", ".import"...) are not listed.
//
// External changes are found by polling (like platform::FileWatcher: no
// inotify, the same code on every desktop OS): poll() compares every
// entry's path, size and modification time with the last scan.
namespace cuervo::editor {

enum class AssetKind { Folder, Scene, Script, Font, Sound, Vector, Image, Replay, Project, Other };

std::string_view asset_kind_name(AssetKind kind);
AssetKind asset_kind_of(const std::filesystem::path& path);

struct AssetEntry {
    std::string path; // project-relative ('' = the project folder itself)
    std::string name; // last path component
    AssetKind kind = AssetKind::Other;
    std::vector<AssetEntry> children; // folders: folders first, then files, by name
};

class AssetLibrary {
public:
    // Scans `project_dir`.
    void open(const std::filesystem::path& project_dir);
    const std::filesystem::path& dir() const { return dir_; }
    const AssetEntry& root() const { return root_; }
    // Finds an entry by project-relative path (nullptr if absent).
    const AssetEntry* find(std::string_view path) const;
    // Every file of a kind, sorted (e.g. the scenes for a picker).
    std::vector<std::string> files_of(AssetKind kind) const;

    // Rescans; true if anything was added, removed or modified since the
    // last scan. `changed` receives the paths that differ.
    bool poll(std::vector<std::string>* changed = nullptr);
    // Bumps on every change seen.
    std::uint64_t revision() const { return revision_; }

    // ---- file operations (rescan on success; false + error otherwise)
    bool create_folder(std::string_view parent, std::string_view name, std::string& error);
    // Renames in place (same folder); `name` is the new last component.
    bool rename(std::string_view path, std::string_view name, std::string& error);
    // Deletes a file or a folder with everything in it. project.toml and the
    // project folder itself are refused.
    bool remove(std::string_view path, std::string& error);

    std::filesystem::path absolute(std::string_view path) const;

private:
    struct Stamp {
        std::string path;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type time;
        friend bool operator==(const Stamp&, const Stamp&) = default;
    };
    void scan(AssetEntry& folder, std::vector<Stamp>& stamps) const;
    bool valid_name(std::string_view name, std::string& error) const;

    std::filesystem::path dir_;
    AssetEntry root_;
    std::vector<Stamp> stamps_;
    std::uint64_t revision_ = 0;
};

} // namespace cuervo::editor

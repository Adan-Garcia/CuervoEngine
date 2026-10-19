#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The welcome screen's data (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): the
// recent projects of this user and the templates a new project starts from.
namespace cuervo::editor {

struct RecentProject {
    std::string path; // the project folder (absolute)
    std::string name; // project.toml's name, else the folder name
    bool exists = true;
};

class RecentProjects {
public:
    static constexpr std::size_t max_entries = 10;

    // Puts `dir` first (moving it up if it was there); keeps the newest max_entries.
    void add(const std::filesystem::path& dir);
    void remove(std::string_view path);
    // Newest first; `exists` says whether project.toml is still there.
    std::vector<RecentProject> list() const;
    const std::vector<std::string>& paths() const { return paths_; }

    std::string serialize() const;
    void parse(std::string_view text);
    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file, std::string& error) const;
    // <user data dir>/editor-recent-projects.txt
    static std::filesystem::path user_file();

private:
    std::vector<std::string> paths_;
};

struct ProjectTemplate {
    std::string id;
    std::string name;
    std::string description;
};

// "empty" (a blank scene plus one drawn node class) and "walker" (a node you
// move with the input map's `move` axis).
const std::vector<ProjectTemplate>& project_templates();

// Writes a new project named `name` from template `template_id` into `dir`
// (created; it must be missing or empty): project.toml, scripts/, scenes/main.scene.
// The messages follow the kit's formula (what happened, whether anything was lost, what to do).
bool create_project(std::string_view template_id, const std::filesystem::path& dir, std::string_view name,
                    std::string& error);

// The project name from project.toml's `name = "..."` line, "" if unreadable.
std::string read_project_name(const std::filesystem::path& dir);

} // namespace cuervo::editor

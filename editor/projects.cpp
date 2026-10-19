#include "editor/projects.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

#include "cuervo/platform/platform.hpp"
#include "cuervo/scene/scene_file.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    return std::string(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
}

const char* block_script = R"(// A rounded block, drawn from its fields. Every @export field shows in the
// inspector and is saved in the scene. Scripts of a project live in scripts/;
// saving one recompiles it in the open editor.
class Block extends Node2D {
    @export var size = vec2(160.0, 24.0)
    @export var color = Color.hex("2a3150")
    @export var corner = 6.0

    fn draw(g: Canvas) {
        g.rect(position - size * 0.5, size, fill = color, stroke = color.lerp(Color.hex("ffffff"), 0.25), width = 1.5,
               radius = corner, rotation = rotation)
    }
}
)";

const char* walker_script = R"(// A dot you move with the input map's `move` axis (arrow keys, WASD, the left
// stick). Press Play (F5) to try it; change `speed` in the inspector.
class Walker extends Node2D {
    @export var speed = 300.0
    @export var color = Color.hex("7fe6ff")
    @export var radius = 18.0

    fn update(dt: float) {
        position += Input.axis(#move) * speed * dt
        position = position.clamp(vec2(radius, radius), Screen.size - vec2(radius, radius))
    }

    fn draw(g: Canvas) {
        g.circle(position, radius, fill = color)
    }
}
)";

std::string project_toml(std::string_view name) {
    std::string safe;
    for (char c : name) safe += (c == '"' || c == '\\' || c == '\n') ? ' ' : c;
    return std::format(R"(# A Cuervo project. Edit it in the editor's panels or by hand (docs/GUIDE.md).
name = "{}"
main_scene = "scenes/main.scene"   # the root is this scene; or: main = "ClassName"
virtual_size = [1280, 720]
tick_rate = 60
seed = 1
clear_color = "0b1020"
render_tier = "compatible"          # runs on Vulkan, GL and the web

[input.actions]
pause = ["Key.Escape", "Key.P", "Pad.Start"]

[input.axes]
move = ["Keys.Arrows", "Keys.WASD", "Pad.LeftStick", "Pad.Dpad"]
)",
                       safe);
}

bool write_text(const fs::path& file, std::string_view text, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    out.flush();
    if (!out) {
        error = std::format("Could not write '{}'. The project folder was only partly created; delete it and try "
                            "again, or pick another folder. Details: {}",
                            platform::utf8(file), ec ? ec.message() : std::string("write failed"));
        return false;
    }
    return true;
}

} // namespace

// ------------------------------------------------------------ recent projects

void RecentProjects::add(const fs::path& dir) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(dir, ec);
    if (ec) abs = fs::absolute(dir);
    const std::string p = platform::utf8(abs);
    std::erase(paths_, p);
    paths_.insert(paths_.begin(), p);
    if (paths_.size() > max_entries) paths_.resize(max_entries);
}

void RecentProjects::remove(std::string_view path) { std::erase(paths_, std::string(path)); }

std::vector<RecentProject> RecentProjects::list() const {
    std::vector<RecentProject> out;
    for (const std::string& p : paths_) {
        RecentProject r;
        r.path = p;
        std::error_code ec;
        r.exists = fs::is_regular_file(platform::path_from_utf8(p) / "project.toml", ec);
        r.name = r.exists ? read_project_name(platform::path_from_utf8(p)) : std::string();
        if (r.name.empty()) r.name = platform::utf8(platform::path_from_utf8(p).filename());
        out.push_back(std::move(r));
    }
    return out;
}

std::string RecentProjects::serialize() const {
    std::string out;
    for (const std::string& p : paths_) out += p + '\n';
    return out;
}

void RecentProjects::parse(std::string_view text) {
    paths_.clear();
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty() && line[0] != '#' && std::find(paths_.begin(), paths_.end(), line) == paths_.end() &&
            paths_.size() < max_entries) {
            paths_.push_back(line);
        }
    }
}

fs::path RecentProjects::user_file() {
    const fs::path dir = platform::user_data_dir("Cuervo Engine");
    return dir.empty() ? fs::path{} : dir / "editor-recent-projects.txt";
}

bool RecentProjects::load(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    parse(ss.str());
    return true;
}

bool RecentProjects::save(const fs::path& file, std::string& error) const {
    if (file.empty()) {
        error = "There is no per-user folder to keep the recent projects in.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << serialize();
    out.flush();
    if (!out) {
        error = std::format("Could not write '{}'.", platform::utf8(file));
        return false;
    }
    return true;
}

// ------------------------------------------------------------ templates

const std::vector<ProjectTemplate>& project_templates() {
    static const std::vector<ProjectTemplate> list = {
        {"empty", "Empty 2D", "A blank scene and one drawn node class (Block) to place. Start here."},
        {"walker", "Walker", "A dot you move with the arrow keys or WASD: input, scripts and Play in one minute."},
    };
    return list;
}

std::string read_project_name(const fs::path& dir) {
    std::ifstream in(dir / "project.toml");
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (!t.starts_with("name")) continue;
        const std::size_t q1 = t.find('"');
        const std::size_t q2 = t.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) return t.substr(q1 + 1, q2 - q1 - 1);
    }
    return {};
}

bool create_project(std::string_view template_id, const fs::path& dir, std::string_view name, std::string& error) {
    const auto& list = project_templates();
    if (std::none_of(list.begin(), list.end(), [&](const ProjectTemplate& t) { return t.id == template_id; })) {
        error = std::format("There is no template '{}'. Nothing was created. Pick one from the list.", template_id);
        return false;
    }
    const std::string project_name = trim(name);
    if (project_name.empty()) {
        error = "A project needs a name. Nothing was created.";
        return false;
    }
    std::error_code ec;
    if (fs::exists(dir, ec) && (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec))) {
        error = std::format("'{}' already has files in it, so nothing was created and nothing there was changed. "
                            "Pick a new or empty folder.",
                            platform::utf8(dir));
        return false;
    }
    const bool walker = template_id == "walker";
    if (!write_text(dir / "project.toml", project_toml(project_name), error)) return false;
    if (!write_text(dir / "scripts" / (walker ? "walker.plm" : "block.plm"), walker ? walker_script : block_script,
                    error)) {
        return false;
    }
    scene::SceneDoc doc;
    doc.nodes.push_back({"Node2D", "main", -1, {}});
    if (walker) {
        doc.nodes.push_back({"Walker", "walker", 0,
                             {{"position", "vec2", scene::Value::vector({640.0f, 360.0f})}}});
    } else {
        doc.nodes.push_back({"Block", "block", 0, {{"position", "vec2", scene::Value::vector({640.0f, 360.0f})}}});
    }
    std::string scene_error;
    fs::create_directories(dir / "scenes", ec);
    if (!scene::save_file(dir / "scenes" / "main.scene", doc, scene_error)) {
        error = std::format("Could not write the starting scene, so the project is incomplete. Delete '{}' and try "
                            "again. Details: {}",
                            platform::utf8(dir), scene_error);
        return false;
    }
    return true;
}

} // namespace cuervo::editor

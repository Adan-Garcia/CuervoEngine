#include "editor/editor_controller.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "editor/file_import.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

const char* tool_name(Tool tool) {
    switch (tool) {
        case Tool::Select: return "Select";
        case Tool::Move: return "Move";
        case Tool::Rotate: return "Rotate";
        case Tool::Scale: return "Scale";
        case Tool::Paint: return "Paint";
    }
    return "Move";
}

void EditorController::notify(UserMessage message) { messages_.push(std::move(message), clock_); }

bool EditorController::instance_scene_asset(const std::string& path, std::optional<vec2> at) {
    const NodeId parent = doc_.selection() != 0 ? doc_.selection() : doc_.model().root();
    const std::string self = doc_.has_path() ? env_.relative(doc_.path()) : std::string();
    if (at) at = vec2{std::round(at->x()), std::round(at->y())};
    const bool ok = doc_.instance_scene(parent, path, at, self) != 0;
    report(ok);
    return ok;
}

bool EditorController::drop_asset_on_scene(const std::string& path, std::optional<vec2> at) {
    if (!path.ends_with(".scene")) {
        report_failure(std::format("Only scenes (.scene) can be dropped into the view; '{}' is not one", path));
        return false;
    }
    return instance_scene_asset(path, at);
}

void EditorController::request_open_scene(const std::string& path) {
    if (doc_.dirty()) {
        discard_path_ = path;
        discard_leave_ = false;
        prompt_ = {Prompt::DiscardOpen, 0, path};
        return;
    }
    std::string error;
    const fs::path p = env_.resolve(path);
    if (doc_.open(p, error)) {
        LOG_INFO("Opened '{}'", env_.relative(p));
        settings_.set_last_scene(env_.project_dir(), env_.relative(p));
        if (hooks.persist) hooks.persist();
        if (hooks.scene_opened) hooks.scene_opened();
    } else {
        LOG_ERROR("{}", error);
    }
}

void EditorController::save_current_scene() {
    if (!doc_.has_path()) {
        prompt_ = {Prompt::SaveAs, 0, {}};
        return;
    }
    std::string error;
    const std::string rel = env_.relative(doc_.path());
    if (doc_.save(error)) {
        LOG_INFO("Saved '{}'", rel);
        assets_.poll();
        notify_ok(std::format("Saved {}.", rel));
        settings_.set_last_scene(env_.project_dir(), rel);
        if (hooks.persist) hooks.persist();
    } else {
        LOG_ERROR("{}", error);
        notify(message_save_failed(rel, error));
    }
}

void EditorController::request_switch_project(std::string path) {
    if (doc_.dirty()) {
        discard_path_ = std::move(path);
        discard_leave_ = true;
        prompt_ = {Prompt::DiscardLeave, 0, discard_path_};
        return;
    }
    switch_target_ = std::move(path);
    switch_requested_ = true;
}

void EditorController::confirm_discard() {
    if (discard_leave_) {
        switch_target_ = discard_path_;
        switch_requested_ = true;
    } else {
        std::string error;
        const fs::path p = env_.resolve(discard_path_);
        if (doc_.open(p, error)) {
            LOG_INFO("Opened '{}'", env_.relative(p));
            if (hooks.scene_opened) hooks.scene_opened();
        } else {
            LOG_ERROR("{}", error);
            notify(message_open_failed(env_.relative(p), error, true));
        }
    }
    cancel_discard();
}

void EditorController::cancel_discard() {
    discard_leave_ = false;
    discard_path_.clear();
}

void EditorController::request_add_child(NodeId parent) { prompt_ = {Prompt::AddChild, parent, {}}; }

void EditorController::request_attach_script(NodeId node) { prompt_ = {Prompt::AttachScript, node, {}}; }

std::pair<std::string, std::string> EditorController::suggest_script(const std::string& node_name) {
    std::string cls = node_name.empty() ? "CustomNode" : node_name;
    cls[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(cls[0])));
    std::string file = node_name;
    std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) { return std::tolower(c); });
    if (file.empty()) file = "custom_node";
    return {cls, "scripts/" + file + ".plm"};
}

void EditorController::reload_project_scripts() {
    ProjectEnv fresh;
    std::string error;
    if (!fresh.open(env_.project_dir(), font_, error)) {
        LOG_ERROR("Scripts: {}", error);
        return;
    }
    if (!env_.open(env_.project_dir(), font_, error)) {
        LOG_ERROR("Scripts: {}", error);
        return;
    }
    doc_.set_registry(&env_.classes());
    class_scripts_cache_.clear();
    LOG_INFO("Scripts recompiled: {} node classes", env_.classes().classes().size());
    if (hooks.scripts_reloaded) hooks.scripts_reloaded();
}

void EditorController::notify_ok(std::string text) {
    UserMessage m;
    m.severity = Severity::Success;
    m.what = std::move(text);
    notify(std::move(m));
}
void EditorController::report_failure(const std::string& raw) {
    LOG_WARN("{}", raw);
    notify(message_edit_rejected(raw));
}
bool EditorController::run_action(std::string_view id) {
    const Action* a = actions_.find(id);
    if (a == nullptr || !actions_.enabled(*a)) return false;
    doc_.seal();
    const bool ok = actions_.run(id);
    doc_.seal();
    if (!ok && !doc_.error().empty()) report_failure(doc_.error());
    return ok;
}
void EditorController::report(bool ok) {
    if (!ok && !doc_.error().empty()) report_failure(doc_.error());
}
std::pair<std::string, int> EditorController::find_class_script(const std::string& class_name) const {
    if (class_name.empty() || !env_.has_project()) return {"", 0};

    if (const auto it = class_scripts_cache_.find(class_name); it != class_scripts_cache_.end()) {
        return it->second;
    }

    std::vector<fs::path> script_files;
    const fs::path scripts_dir = env_.project_dir() / "scripts";
    std::error_code ec;

    if (fs::exists(scripts_dir, ec) && fs::is_directory(scripts_dir, ec)) {
        for (const auto& entry : fs::recursive_directory_iterator(scripts_dir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".plm") {
                script_files.push_back(entry.path());
            }
        }
    }

    for (const auto& entry : fs::directory_iterator(env_.project_dir(), ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".plm") {
            script_files.push_back(entry.path());
        }
    }

    for (const fs::path& file : script_files) {
        std::ifstream in(file);
        if (!in) continue;
        std::string line;
        int line_num = 0;
        while (std::getline(in, line)) {
            ++line_num;
            const std::size_t cpos = line.find("class ");
            if (cpos != std::string::npos) {
                if (cpos > 0 && !std::isspace(static_cast<unsigned char>(line[cpos - 1]))) continue;
                std::size_t name_start = cpos + 6;
                while (name_start < line.size() && std::isspace(static_cast<unsigned char>(line[name_start]))) {
                    ++name_start;
                }
                if (line.substr(name_start, class_name.size()) == class_name) {
                    const std::size_t after = name_start + class_name.size();
                    if (after >= line.size() || std::isspace(static_cast<unsigned char>(line[after])) ||
                        line[after] == ':' || line[after] == '(' || line[after] == '{') {
                        auto res = std::make_pair(env_.relative(file), line_num);
                        class_scripts_cache_[class_name] = res;
                        return res;
                    }
                }
            }
        }
    }
    class_scripts_cache_[class_name] = {"", 0};
    return {"", 0};
}
bool EditorController::is_script_class(const std::string& class_name) const {
    return !find_class_script(class_name).first.empty();
}
bool EditorController::has_script_bound(NodeId id) const {
    const ModelNode* n = doc_.model().get(id);
    return n != nullptr && is_script_class(n->type);
}
std::pair<std::string, int> EditorController::node_script(NodeId id) const {
    const ModelNode* n = doc_.model().get(id);
    return n != nullptr ? find_class_script(n->type) : std::pair<std::string, int>{"", 0};
}
std::vector<std::pair<std::string, std::string>> EditorController::project_script_classes() const {
    std::vector<std::pair<std::string, std::string>> result;
    for (const ClassSpec& c : env_.classes().classes()) {
        const auto [path, line] = find_class_script(c.name);
        if (!path.empty()) {
            result.emplace_back(c.name, path);
        }
    }
    return result;
}
std::vector<std::string> EditorController::find_script_classes_in_file(const fs::path& file) const {
    std::vector<std::string> out;
    std::ifstream in(file);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t cpos = line.find("class ");
        if (cpos != std::string::npos) {
            if (cpos > 0 && !std::isspace(static_cast<unsigned char>(line[cpos - 1]))) continue;
            std::size_t name_start = cpos + 6;
            while (name_start < line.size() && std::isspace(static_cast<unsigned char>(line[name_start]))) {
                ++name_start;
            }
            std::size_t name_end = name_start;
            while (name_end < line.size() && (std::isalnum(static_cast<unsigned char>(line[name_end])) || line[name_end] == '_')) {
                ++name_end;
            }
            if (name_end > name_start) {
                out.push_back(line.substr(name_start, name_end - name_start));
            }
        }
    }
    return out;
}
bool EditorController::attach_script_file(NodeId id, const fs::path& file, std::string& error) {
    const ModelNode* n = doc_.model().get(id);
    if (n == nullptr) {
        error = "Node not found";
        return false;
    }
    const std::vector<std::string> classes = find_script_classes_in_file(file);
    if (classes.empty()) {
        error = std::format("No class found in '{}'", platform::utf8(file));
        return false;
    }
    std::string chosen_class = classes.front();
    if (env_.classes().find(chosen_class) == nullptr) {
        reload_project_scripts();
    }
    if (env_.classes().find(chosen_class) == nullptr) {
        error = std::format("Class '{}' not found after compilation", chosen_class);
        return false;
    }
    if (!doc_.change_type(id, chosen_class)) {
        error = doc_.error();
        return false;
    }
    class_scripts_cache_.clear();
    LOG_INFO("Bound script '{}' (class '{}') to '{}'", platform::utf8(file), chosen_class, n->name);
    notify_ok(std::format("Bound class '{}' to '{}'.", chosen_class, n->name));
    return true;
}
bool EditorController::create_and_attach_script(NodeId id, const std::string& class_name, const fs::path& rel_path,
                                        std::string& error) {
    const ModelNode* n = doc_.model().get(id);
    if (n == nullptr) {
        error = "Node not found";
        return false;
    }
    if (class_name.empty()) {
        error = "Class name cannot be empty";
        return false;
    }
    const ClassRegistry& reg = env_.classes();
    std::string base_class = n->type;
    const ClassSpec* current_cls = reg.find(n->type);
    if (current_cls != nullptr && !find_class_script(n->type).first.empty() && !current_cls->base.empty()) {
        base_class = current_cls->base;
    }

    const fs::path full_path = env_.resolve(rel_path.string());
    std::error_code ec;
    if (fs::exists(full_path, ec)) {
        error = std::format("File '{}' already exists", rel_path.string());
        return false;
    }

    fs::create_directories(full_path.parent_path(), ec);
    std::ofstream out(full_path);
    if (!out) {
        error = std::format("Could not create file '{}': {}", rel_path.string(), std::error_code(errno, std::generic_category()).message());
        return false;
    }

    out << "class " << class_name << " extends " << base_class << " {\n";
    out << "    fn ready() {\n";
    out << "    }\n\n";
    out << "    fn update(dt: float) {\n";
    out << "    }\n";
    out << "}\n";
    out.close();

    reload_project_scripts();
    class_scripts_cache_.clear();

    if (!doc_.change_type(id, class_name)) {
        error = doc_.error();
        return false;
    }

    LOG_INFO("Created script '{}' and bound class '{}' to '{}'", rel_path.string(), class_name, n->name);
    notify_ok(std::format("Bound '{}' ({}) to '{}'.", rel_path.string(), class_name, n->name));
    open_in_external_editor(full_path, 1);
    return true;
}
bool EditorController::open_in_external_editor(const fs::path& path, int line) {
    if (path.empty()) return false;
    std::error_code ec;
    const fs::path abs_path = fs::absolute(path, ec);
    const std::string path_str = platform::utf8(abs_path);

    std::string editor;
    if (const auto e = cuervo::platform::env("CUERVO_EDITOR"); e && !e->empty()) editor = *e;
    else if (const auto v = cuervo::platform::env("VISUAL"); v && !v->empty()) editor = *v;
    else if (const auto ed = cuervo::platform::env("EDITOR"); ed && !ed->empty()) editor = *ed;

    std::string command;
    if (!editor.empty()) {
        std::string ed_lower = editor;
        for (char& c : ed_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (ed_lower.find("code") != std::string::npos || ed_lower.find("cursor") != std::string::npos ||
            ed_lower.find("subl") != std::string::npos) {
            if (line > 0) {
                command = std::format("{} -g \"{}:{}\"", editor, path_str, line);
            } else {
                command = std::format("{} \"{}\"", editor, path_str);
            }
        } else if (ed_lower.find("vim") != std::string::npos || ed_lower.find("nvim") != std::string::npos ||
                   ed_lower.find("nano") != std::string::npos || ed_lower.find("emacs") != std::string::npos ||
                   ed_lower.find("gedit") != std::string::npos || ed_lower.find("kate") != std::string::npos) {
            if (line > 0) {
                command = std::format("{} +{} \"{}\"", editor, line, path_str);
            } else {
                command = std::format("{} \"{}\"", editor, path_str);
            }
        } else {
            if (line > 0) {
                command = std::format("{} \"{}:{}\"", editor, path_str, line);
            } else {
                command = std::format("{} \"{}\"", editor, path_str);
            }
        }
    } else {
#if defined(_WIN32)
        command = std::format("start \"\" \"{}\"", path_str);
#elif defined(__APPLE__)
        command = std::format("open \"{}\"", path_str);
#else
        if (std::system("which code >/dev/null 2>&1") == 0) {
            if (line > 0) {
                command = std::format("code -g \"{}:{}\"", path_str, line);
            } else {
                command = std::format("code \"{}\"", path_str);
            }
        } else if (std::system("which cursor >/dev/null 2>&1") == 0) {
            if (line > 0) {
                command = std::format("cursor -g \"{}:{}\"", path_str, line);
            } else {
                command = std::format("cursor \"{}\"", path_str);
            }
        } else {
            command = std::format("xdg-open \"{}\"", path_str);
        }
#endif
    }

#if defined(_WIN32)
    const std::string bg_cmd = command;
    LOG_INFO("Opening external editor: {}", command);
    const int res = std::system(bg_cmd.c_str());
    return res == 0;
#else
    const std::string bg_cmd = std::format("({} ) >/dev/null 2>&1 &", command);
    LOG_INFO("Opening external editor: {}", command);
    const int res = std::system(bg_cmd.c_str());
    return res == 0;
#endif
}

bool EditorController::detach_script(NodeId id) {
    const ModelNode* n = doc_.model().get(id);
    if (n == nullptr) return false;
    const ClassRegistry& reg = env_.classes();
    const ClassSpec* cls = reg.find(n->type);
    std::string base = "Node2D";
    if (cls != nullptr && !cls->base.empty()) {
        base = cls->base;
    } else {
        if (reg.is_a(n->type, "Control")) base = "Control";
        else if (reg.is_a(n->type, "Node2D")) base = "Node2D";
        else base = "Node";
    }
    const bool ok = doc_.change_type(id, base);
    if (ok) {
        LOG_INFO("Detached script from '{}' (type reverted to '{}')", n->name, base);
        notify_ok(std::format("Detached script from '{}'.", n->name));
    } else {
        report_failure(doc_.error());
    }
    return ok;
}

script::SessionOptions EditorController::play_options(bool current_scene) const {
    script::SessionOptions o;
    o.dir = env_.project_dir();
    o.font_path = play_setup_.font_path;
    o.jobs = play_setup_.jobs;
    o.audio_device = play_setup_.audio;
    if (current_scene) {
        o.scene = std::make_shared<const scene::SceneDoc>(doc_.model().to_doc());
        o.scene_label = doc_.has_path() ? env_.relative(doc_.path()) : std::string("(new scene)");
    }
    return o;
}

void EditorController::toggle_ready_preview() {
    ready_on_ = !ready_on_;
    ready_revision_ = ~std::uint64_t{0};
    if (!ready_on_) ready_.clear();
}

void EditorController::update_ready_preview() {
    if (!ready_on_ || play_.active()) return;
    if (ready_revision_ != doc_.revision()) {
        ready_revision_ = doc_.revision();
        ready_changed_at_ = clock_;
    } else if (ready_changed_at_ >= 0.0 && clock_ - ready_changed_at_ >= 0.3) {
        ready_changed_at_ = -1.0;
        std::string why;
        if (!ready_.refresh(play_options(true), why)) LOG_WARN("ready() preview: {}", why);
        else if (ready_.panicked()) LOG_WARN("ready() preview: {}", ready_.error());
    }
}

bool EditorController::start_play(bool current_scene, bool record) {
    std::string error;
    if (!play_.start(play_options(current_scene), error)) {
        LOG_ERROR("Play: {}", error.empty() ? std::string("the game could not start") : error);
        return false;
    }
    if (record) play_.start_recording();
    return true;
}

EditorController::ImportSummary EditorController::import_files(const std::vector<std::filesystem::path>& files) {
    ImportSummary sum;
    for (const ImportResult& r : ::cuervo::editor::import_files(env_.project_dir(), files)) {
        if (r.ok) {
            ++sum.imported;
            LOG_INFO("Imported '{}' as '{}'", r.source, r.dest);
            if (sum.first_dest.empty()) sum.first_dest = r.dest;
            if (sum.first_image.empty() && (r.kind == AssetKind::Image || r.kind == AssetKind::Vector)) sum.first_image = r.dest;
        } else {
            LOG_WARN("Not imported: '{}': {}", r.source, r.error);
            report_failure(r.error);
            if (sum.error.empty()) sum.error = r.error;
        }
    }
    if (sum.imported == 0) return sum;
    assets_.poll();
    notify_ok(sum.imported == 1 ? "Imported 1 file" : std::format("Imported {} files", sum.imported));
    return sum;
}

bool EditorController::start_replay(const std::string& path) {
    std::string error;
    auto replay = load_replay(env_.resolve(path), error);
    if (!replay || !play_.start_replay(play_options(false), std::move(*replay), error)) {
        LOG_ERROR("Replay: {}", error);
        return false;
    }
    return true;
}

} // namespace cuervo::editor

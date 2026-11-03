// The welcome screen: what the editor shows when no project is open (same features as the ImGui
// launcher in editor/launcher.cpp: recent projects with Remove for missing ones, open a folder or
// type a path, templates, version and license), plus the example projects. It is an overlay
// document region under the menu bar; New project and Open project reuse the shell's dialogs.
#include <SDL3/SDL.h>

#include <filesystem>
#include <format>
#include <mutex>

#include "cuervo/platform/platform.hpp"
#include "editor/projects.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;
using dom::find_attr;

void Shell::refresh_welcome() {
    const bool want = !ctl_.env().has_project() || force_welcome_;
    if (want != welcome_shown_) {
        welcome_shown_ = want;
        welcome_dirty_ = true;
        if (Rml::Element* e = by_id("welcome")) set_class_if(e, "open", want);
    }
    if (!want) return;
    if (welcome_dirty_ && dialog_ == Dialog::None) build_welcome();
}

void Shell::build_welcome() {
    Rml::Element* e = by_id("welcome");
    if (e == nullptr) return;
    welcome_dirty_ = false;
    std::string recent;
    int n = 0;
    for (const RecentProject& r : recents_.list()) {
        recent += std::format("<div class=\"wrow{}\" data-welcome=\"{}\">{}<div class=\"wtext\"><div class=\"wname\">{}</div><div class=\"wsub\">{}</div></div>{}</div>",
                              r.exists ? "" : " off", r.exists ? "project:" + w::esc(r.path) : "none", w::icon("folder", 18, r.exists ? "accent" : "muted"), w::esc(r.name),
                              w::esc(r.path) + (r.exists ? "" : "  (not found)"),
                              r.exists ? std::string() : std::format("<button class=\"btn sm\" data-welcome=\"remove:{}\"><span>Remove</span></button>", w::esc(r.path)));
        ++n;
    }
    if (n == 0) recent = "<div class=\"hint\">No projects yet. Open a folder or create one.</div>";
    std::string tpl;
    for (const ProjectTemplate& t : project_templates()) {
        tpl += std::format("<div class=\"wrow\" data-welcome=\"tpl:{}\"><div class=\"wtext\"><div class=\"wname\">{}</div><div class=\"wsub\">{}</div></div></div>", w::esc(t.id), w::esc(t.name), w::esc(t.description));
    }
    std::string ex;
    for (const auto& [name, path] : list_example_projects(platform::find_asset("examples", CUERVO_EXAMPLES_DIR))) {
        ex += std::format("<div class=\"wrow\" data-welcome=\"project:{}\">{}<div class=\"wtext\"><div class=\"wname\">{}</div></div></div>", w::esc(path), w::icon("folder", 18, "muted"), w::esc(name));
    }
    if (ex.empty()) ex = "<div class=\"hint\">No example projects found next to the editor.</div>";
    e->SetInnerRML(std::format(
        "<div id=\"wl-col\"><div class=\"wl-head\"><div class=\"wl-title\">Cuervo Engine</div><div class=\"wl-sub\">Open a project to start editing, or begin a new one.</div></div>"
        "<div class=\"wl-grid\">"
        "<div class=\"card wl-main\"><div class=\"section\"><span>Recent projects</span></div><div class=\"wl-list scroll\">{}</div>"
        "<div class=\"wl-open\"><button class=\"btn\" data-welcome=\"folder\">{}<span>Open folder...</span></button><span class=\"dm\">or type a path</span>"
        "<input id=\"wl-path\" type=\"text\" value=\"{}\" placeholder=\"/path/to/project\"/><button class=\"btn primary\" data-welcome=\"open\"><span>Open</span></button></div>"
        "<div id=\"wl-err\" class=\"derr\">{}</div></div>"
        "<div class=\"card wl-side\"><div class=\"section\"><span>New project</span></div><div class=\"wl-list wl-tpl\">{}</div>"
        "<div class=\"wl-open\"><button class=\"btn primary\" data-welcome=\"new\"><span>New project...</span></button></div>"
        "<div class=\"section\"><span>Examples</span></div><div class=\"wl-list scroll\">{}</div></div>"
        "</div><div class=\"wl-foot\"><span>Cuervo Engine {}  |  MIT License</span><button class=\"btn sm\" data-welcome=\"license\"><span>License</span></button></div></div>",
        recent, w::icon("open", 14), w::esc(welcome_path_), w::esc(welcome_error_), tpl, ex, CUERVO_EDITOR_VERSION));
}

void Shell::welcome_open(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(platform::path_from_utf8(path) / "project.toml", ec)) {
        welcome_error_ = path.empty() ? "Type or pick a project folder." : std::format("'{}' has no project.toml.", path);
        if (Rml::Element* x = by_id("wl-err")) x->SetInnerRML(w::esc(welcome_error_));
        return;
    }
    welcome_error_.clear();
    ctl_.request_switch_project(path);
}

bool Shell::welcome_click(Rml::Element* t) {
    Rml::Element* c = find_attr(t, "data-welcome");
    if (c == nullptr) return false;
    const std::string v = c->GetAttribute<Rml::String>("data-welcome", "");
    const auto after = [&](const char* prefix) { return v.substr(std::string_view(prefix).size()); };
    if (v.rfind("project:", 0) == 0) welcome_open(after("project:"));
    else if (v.rfind("tpl:", 0) == 0) { np_template_ = after("tpl:"); open_new_project(); }
    else if (v.rfind("remove:", 0) == 0) {
        recents_.remove(after("remove:"));
        if (persist_) { std::string error; recents_.save(RecentProjects::user_file(), error); }
        welcome_dirty_ = true;
    } else if (v == "new") open_new_project();
    else if (v == "open") welcome_open(welcome_path_);
    else if (v == "license") open_dialog(Dialog::License);
    else if (v == "folder") {
        native_pick(PickKind::ProjectFolder);
    }
    return true;
}

} // namespace cuervo::editor

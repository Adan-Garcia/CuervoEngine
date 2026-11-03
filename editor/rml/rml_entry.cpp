// Dialogs for starting and learning about the editor: New project (template, name, folder) and the
// license view (License / Third-party notices, read-only). Same engine calls as the ImGui editor
// (projects.hpp create_project, cuervo/platform/license.hpp).
#include <cstdlib>
#include <filesystem>
#include <format>

#include "cuervo/platform/license.hpp"
#include "cuervo/platform/platform.hpp"
#include "editor/projects.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;
using dom::find_attr;

namespace {

std::string default_parent() {
    std::optional<std::string> home = platform::env("HOME");
    if (!home) home = platform::env("USERPROFILE");
    return platform::utf8(platform::path_from_utf8(home ? *home : std::string(".")) / "cuervo-projects");
}

std::string slug(const std::string& name) {
    std::string s;
    for (char c : name) s += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
    return s.empty() ? std::string("my_game") : s;
}

} // namespace

std::string Shell::entry_dialog_html(Dialog d) {
    const auto buttons = [](const char* ok) {
        return std::format("<div class=\"dbtns\"><button class=\"btn\" data-dlg=\"cancel\"><span>Cancel</span></button><button class=\"btn primary\" data-dlg=\"ok\"><span>{}</span></button></div>", ok);
    };
    if (d == Dialog::License) {
        const std::string_view text = np_notices_ ? license::notices_text() : license::license_text();
        return std::format("<div class=\"dt\">Cuervo Engine license</div><div class=\"seg\" data-seg=\"lic\"><div class=\"segi{}\" data-lic=\"0\">License</div><div class=\"segi{}\" data-lic=\"1\">Third-party notices</div></div>"
                           "<div id=\"lic-text\" class=\"lictext scroll\">{}</div><div class=\"dbtns\"><button class=\"btn primary\" data-dlg=\"cancel\"><span>Close</span></button></div>",
                           np_notices_ ? "" : " active", np_notices_ ? " active" : "", w::esc(std::string(text)));
    }
    // New project.
    if (np_dir_.empty()) np_dir_ = platform::utf8(platform::path_from_utf8(default_parent()) / slug(np_name_));
    std::string chips;
    for (const ProjectTemplate& t : project_templates()) chips += std::format("<div class=\"btn sm{}\" data-np=\"{}\" title=\"{}\"><span>{}</span></div>", t.id == np_template_ ? " on" : "", w::esc(t.id), w::esc(t.description), w::esc(t.name));
    std::string desc;
    for (const ProjectTemplate& t : project_templates()) if (t.id == np_template_) desc = t.description;
    return std::format("<div class=\"dt\">New project</div><div class=\"dm\">{}</div><div class=\"opts\">{}</div>"
                       "<div class=\"dm\">Name</div><input id=\"np-name\" type=\"text\" value=\"{}\"/><div class=\"dm\">Folder (created; must be missing or empty)</div><input id=\"np-dir\" type=\"text\" value=\"{}\"/>"
                       "<div id=\"dlg-err\" class=\"derr\">{}</div>{}",
                       w::esc(desc), chips, w::esc(np_name_), w::esc(np_dir_), w::esc(np_error_), buttons("Create and open"));
}

void Shell::open_new_project() {
    np_name_ = "My Game";
    np_dir_.clear();
    np_error_.clear();
    if (np_template_.empty() && !project_templates().empty()) np_template_ = project_templates().front().id;
    open_dialog(Dialog::NewProject);
}

bool Shell::new_project_confirm() {
    if (np_name_.empty()) { np_error_ = "Name the project."; }
    else {
        std::string error;
        if (create_project(np_template_, platform::path_from_utf8(np_dir_), np_name_, error)) {
            close_dialog();
            ctl_.request_switch_project(np_dir_);
            return true;
        }
        np_error_ = error;
    }
    if (Rml::Element* x = by_id("dlg-err")) x->SetInnerRML(w::esc(np_error_));
    return false;
}

bool Shell::entry_click(Rml::Element* t) {
    if (welcome_shown_ && dialog_ == Dialog::None && welcome_click(t)) return true;
    if (dialog_ == Dialog::NewProject) {
        if (Rml::Element* c = find_attr(t, "data-np")) {
            np_template_ = c->GetAttribute<Rml::String>("data-np", "");
            open_dialog(Dialog::NewProject);
            return true;
        }
    } else if (dialog_ == Dialog::License) {
        if (Rml::Element* c = find_attr(t, "data-lic")) {
            np_notices_ = c->GetAttribute<int>("data-lic", 0) == 1;
            open_dialog(Dialog::License);
            return true;
        }
    }
    return false;
}

bool Shell::entry_change(Rml::Element* t, const std::string& value, bool enter) {
    const std::string id = t != nullptr ? t->GetId() : std::string();
    if (id == "wl-path") {
        welcome_path_ = value;
        if (enter) welcome_open(value);
        return true;
    }
    if (id == "np-name") {
        const bool follow = np_dir_ == platform::utf8(platform::path_from_utf8(default_parent()) / slug(np_name_));
        np_name_ = value;
        if (follow) { // the folder follows the name until it is edited by hand
            np_dir_ = platform::utf8(platform::path_from_utf8(default_parent()) / slug(np_name_));
            if (Rml::Element* f = by_id("np-dir")) f->SetAttribute("value", np_dir_);
        }
        if (enter) new_project_confirm();
        return true;
    }
    if (id == "np-dir") {
        np_dir_ = value;
        if (enter) new_project_confirm();
        return true;
    }
    return false;
}

} // namespace cuervo::editor

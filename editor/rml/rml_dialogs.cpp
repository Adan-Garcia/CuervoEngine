// The RmlUi shell's pickers and dialogs that are more than a message: the Add Child picker anchored to
// its "+" button, the Attach script dialog and the Open project dialog (DESIGN.md 4.13).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "editor/actions.hpp"
#include "editor/icons.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml/script/script_panel.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;
using dom::icon_img;

namespace {

// "name" with the matched characters wrapped in <span class="m">.
std::string highlighted(const std::string& text, const std::vector<std::size_t>& positions) {
    std::string out;
    std::size_t next = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool hit = next < positions.size() && positions[next] == i;
        if (hit) ++next;
        const std::string ch = w::esc(std::string_view(text).substr(i, 1));
        out += hit ? "<span class=\"m\">" + ch + "</span>" : ch;
    }
    return out;
}

} // namespace

// ------------------------------------------------------------------ Add Child picker

void Shell::open_picker(NodeId parent) {
    Rml::Element* p = by_id("popup");
    if (p == nullptr) return;
    close_popup();
    picker_ = true;
    popup_open_ = true;
    picker_parent_ = parent != 0 ? parent : ctl_.doc().model().root();
    picker_query_.clear();
    picker_sel_ = 0;
    const ModelNode* parent_node = ctl_.doc().model().get(picker_parent_);
    p->SetInnerRML(std::format(
        "<div class=\"pk-head\"><span>Add child to</span><span class=\"pk-parent\">{}</span></div>"
        "<div class=\"pad\"><input id=\"pick-field\" type=\"text\" placeholder=\"Search classes\"/></div>"
        "<div id=\"pick-list\" class=\"scroll\"></div>"
        "<div class=\"pk-foot\"><span class=\"kbd\">Up Down</span> move <span class=\"kbd\">Enter</span> add <span class=\"kbd\">Ctrl+D</span> star <span class=\"kbd\">Esc</span> close</div>",
        w::esc(parent_node != nullptr ? parent_node->name : "scene")));
    // Anchored under the "+" button when the Scene pane shows it; else near the top, like the palette.
    const float ratio = ctx_->GetDensityIndependentPixelRatio();
    const float width = 340.0f * ratio;
    const float win_w = ctx_->GetDimensions().x;
    float left = (win_w - width) / 2.0f, top = 60.0f * ratio;
    Rml::Element* btn = page_->QuerySelector("[data-act=add-child]");
    if (btn != nullptr && btn->IsVisible(true) && layout_.rail == "scene" && !layout_.left_collapsed) {
        left = std::min(btn->GetAbsoluteLeft(), win_w - width - 8.0f * ratio);
        top = btn->GetAbsoluteTop() + btn->GetOffsetHeight() + 2.0f * ratio;
    }
    p->SetProperty("left", std::format("{}px", static_cast<int>(std::max(left, 4.0f * ratio))));
    p->SetProperty("top", std::format("{}px", static_cast<int>(top)));
    p->SetProperty("width", "340dp");
    p->SetProperty("display", "block");
    build_picker();
    if (Rml::Element* f = by_id("pick-field")) f->Focus();
}

void Shell::close_picker() {
    picker_ = false;
    picker_names_.clear();
}

void Shell::build_picker() {
    Rml::Element* l = by_id("pick-list");
    if (l == nullptr) return;
    const CatalogUser user = CatalogUser::parse(layout_.add_recents, layout_.add_favorites);
    const auto groups = build_catalog(ctl_.env().classes(), picker_query_, user, [this](const std::string& n) { return ctl_.is_script_class(n); });
    const auto flat = catalog_flat(groups);
    picker_names_.clear();
    for (const CatalogEntry* e : flat) picker_names_.push_back(e->name);
    picker_sel_ = flat.empty() ? 0 : std::clamp(picker_sel_, 0, static_cast<int>(flat.size()) - 1);
    std::string out;
    int index = 0;
    for (const CatalogGroup& g : groups) {
        out += std::format("<div class=\"pkg\">{}</div>", w::esc(g.title));
        for (const CatalogEntry& e : g.items) {
            const std::string_view icon = icon_for_class(&ctl_.env().classes(), e.name);
            out += std::format("<div class=\"pk{}\" data-pick=\"{}\">{}<span class=\"nm\">{}</span><span class=\"ds\">{}</span><span class=\"star{}\" data-star=\"{}\" title=\"{} favorite (Ctrl+D)\">{}</span></div>",
                               index == picker_sel_ ? " sel" : "", w::esc(e.name), icon_img(icon.empty() ? "node" : icon, 14),
                               highlighted(e.name, e.positions), w::esc(e.description), e.favorite ? " on" : "", w::esc(e.name),
                               e.favorite ? "Remove" : "Add", icon_img("star", 12));
            ++index;
        }
    }
    if (flat.empty()) {
        out = picker_query_.empty()
                  ? "<div class=\"hint\">No node classes are loaded. Fix the script errors shown in the Console.</div>"
                  : std::format("<div class=\"hint\">No class matches '{}'.</div><div class=\"pad\"><button class=\"btn\" data-act=\"pick-clear\"><span>Clear search</span></button></div>", w::esc(picker_query_));
    }
    l->SetInnerRML(out);
    if (Rml::Element* sel = l->QuerySelector(".pk.sel")) sel->ScrollIntoView(false);
}

bool Shell::picker_key(Rml::Input::KeyIdentifier key, bool ctrl) {
    const int count = static_cast<int>(picker_names_.size());
    switch (key) {
    case Rml::Input::KI_DOWN: if (count > 0) { picker_sel_ = (picker_sel_ + 1) % count; build_picker(); } return true;
    case Rml::Input::KI_UP: if (count > 0) { picker_sel_ = (picker_sel_ + count - 1) % count; build_picker(); } return true;
    case Rml::Input::KI_HOME: picker_sel_ = 0; build_picker(); return true;
    case Rml::Input::KI_END: picker_sel_ = std::max(count - 1, 0); build_picker(); return true;
    case Rml::Input::KI_RETURN:
    case Rml::Input::KI_NUMPADENTER:
        if (count > 0) pick_class(picker_names_[static_cast<std::size_t>(picker_sel_)]);
        return true;
    case Rml::Input::KI_D:
        if (ctrl && count > 0) {
            CatalogUser user = CatalogUser::parse(layout_.add_recents, layout_.add_favorites);
            user.toggle_favorite(picker_names_[static_cast<std::size_t>(picker_sel_)]);
            layout_.add_favorites = user.favorites_text();
            save_layout();
            build_picker();
            return true;
        }
        return false;
    default: return false;
    }
}

void Shell::pick_class(const std::string& name) {
    EditorDocument& doc = ctl_.doc();
    const ModelNode* parent = doc.model().get(picker_parent_);
    if (parent == nullptr) { close_popup(); return; }
    const ClassRegistry& reg = ctl_.env().classes();
    std::vector<scene::Field> fields;
    // A positioned node appears in the middle of the scene view.
    if (reg.is_a(name, "Node2D") && !reg.is_a(name, "Control") && host_.view_center) {
        const vec2 c = host_.view_center();
        fields.push_back({"position", "vec2", scene::Value::vector({std::round(c.x()), std::round(c.y())})});
    }
    const NodeId parent_id = parent->id;
    CatalogUser user = CatalogUser::parse(layout_.add_recents, layout_.add_favorites);
    user.note_used(name);
    layout_.add_recents = user.recents_text();
    close_popup();
    ctl_.report(doc.add_child(parent_id, name, {}, std::move(fields)) != 0);
    doc.seal();
    save_layout();
}

// ------------------------------------------------------------------ Attach script

void Shell::build_attach() {
    Rml::Element* dlg = by_id("dialog");
    const ModelNode* n = ctl_.doc().model().get(attach_node_);
    if (dlg == nullptr || n == nullptr) return;
    std::string body;
    const auto seg = [&](int tab, const char* label) {
        return std::format("<div class=\"segi{}\" data-attach-tab=\"{}\">{}</div>", attach_tab_ == tab ? " active" : "", tab, label);
    };
    std::string out = std::format("<div class=\"dt\">Attach script to {}</div><div class=\"seg\">{}{}</div>", w::esc(n->name), seg(0, "Existing script"), seg(1, "New script"));
    if (attach_tab_ == 0) {
        out += "<div class=\"pad0\"><input id=\"at-filter\" type=\"text\" placeholder=\"Filter scripts and classes\"/></div><div id=\"at-list\" class=\"scroll\"></div>";
    } else {
        out += std::format("<div class=\"dm\">Class name</div><input id=\"at-class\" type=\"text\" value=\"{}\"/><div class=\"dm\">File, relative to the project</div><input id=\"at-path\" type=\"text\" value=\"{}\"/><div class=\"dm\">The new class extends {}.</div>",
                           w::esc(new_class_), w::esc(new_path_), w::esc(n->type));
    }
    out += "<div id=\"at-error\" class=\"derr\"></div>";
    out += std::format("<div class=\"dbtns\"><button class=\"btn\" data-dlg=\"cancel\"><span>Cancel</span></button><button class=\"btn primary\" data-dlg=\"ok\"><span>{}</span></button></div>", attach_tab_ == 0 ? "Attach" : "Create and attach");
    dlg->SetInnerRML(out);
    if (attach_tab_ == 0) {
        build_attach_list();
        if (Rml::Element* f = by_id("at-filter")) f->Focus();
    } else if (Rml::Element* f = by_id("at-class")) {
        f->Focus();
    }
    if (Rml::Element* e = by_id("at-error")) e->SetInnerRML(w::esc(attach_error_));
}

void Shell::build_attach_list() {
    Rml::Element* l = by_id("at-list");
    if (l == nullptr) return;
    attach_shown_.clear();
    std::vector<std::pair<int, std::pair<std::string, std::string>>> scored;
    for (const auto& item : ctl_.project_script_classes()) {
        FuzzyMatch m;
        if (attach_filter_.empty()) scored.push_back({0, item});
        else if (fuzzy_match(attach_filter_, item.first + " " + item.second, m)) scored.push_back({m.score, item});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out;
    for (const auto& [score, item] : scored) {
        attach_shown_.push_back(item);
        out += std::format("<div class=\"pr{}\" data-attach=\"{}\">{}<span>{}</span><span class=\"src\">{}</span></div>", item.first == attach_sel_ ? " sel" : "", w::esc(item.first),
                           icon_img("code", 14), w::esc(item.first), w::esc(item.second));
    }
    if (out.empty()) {
        out = ctl_.project_script_classes().empty()
                  ? "<div class=\"hint\">This project has no script classes yet. Use New script to create one.</div>"
                  : "<div class=\"hint\">No script matches the filter.</div>";
    } else if (attach_sel_.empty() || std::none_of(attach_shown_.begin(), attach_shown_.end(), [&](const auto& s) { return s.first == attach_sel_; })) {
        attach_sel_ = attach_shown_.front().first;
        const std::string first = std::format("data-attach=\"{}\"", w::esc(attach_sel_));
        out.replace(out.find("class=\"pr\" " + first), 10, "class=\"pr sel\"");
    }
    l->SetInnerRML(out);
}

void Shell::attach_confirm() {
    const ModelNode* n = ctl_.doc().model().get(attach_node_);
    if (n == nullptr) { close_dialog(); return; }
    const std::string name = n->name;
    if (attach_tab_ == 0) {
        if (attach_sel_.empty()) { attach_error_ = "Select a script class first."; build_attach(); return; }
        if (ctl_.doc().change_type(attach_node_, attach_sel_)) {
            ctl_.doc().seal();
            ctl_.notify_ok(std::format("Bound '{}' to '{}'.", attach_sel_, name));
            close_dialog();
        } else {
            attach_error_ = ctl_.doc().error();
            if (Rml::Element* e = by_id("at-error")) e->SetInnerRML(w::esc(attach_error_));
        }
    } else {
        std::string err;
        if (ctl_.create_and_attach_script(attach_node_, new_class_, new_path_, err)) {
            ctl_.doc().seal();
            close_dialog();
        } else {
            attach_error_ = err;
            if (Rml::Element* e = by_id("at-error")) e->SetInnerRML(w::esc(attach_error_));
        }
    }
}

// ------------------------------------------------------------------ Open project

void Shell::note_project(const fs::path& dir) {
    force_welcome_ = false;
    if (script_) script_->project_changed();
    recents_.add(dir);
    if (!persist_) return;
    std::string error;
    recents_.save(RecentProjects::user_file(), error);
}

void Shell::build_open_project() {
    Rml::Element* dlg = by_id("dialog");
    if (dlg == nullptr) return;
    std::string out = "<div class=\"dt\">Open project</div><div class=\"dm\">Recent projects</div>";
    int n = 0;
    for (const RecentProject& r : recents_.list()) {
        const bool current = ctl_.env().has_project() && r.exists && fs::equivalent(r.path, ctl_.env().project_dir());
        out += std::format("<div class=\"pr{}\" data-project=\"{}\">{}<span>{}</span><span class=\"src\">{}</span></div>", r.exists && !current ? "" : " off", w::esc(r.path),
                           icon_img("folder", 14), w::esc(r.name), current ? "open now" : r.exists ? w::esc(r.path) : "missing");
        ++n;
    }
    if (n == 0) out += "<div class=\"hint\">No recent projects. Type a project folder below.</div>";
    out += std::format("<div class=\"dm\">Project folder</div><input id=\"proj-field\" type=\"text\" value=\"{}\" placeholder=\"/path/to/project\"/><div id=\"proj-error\" class=\"derr\">{}</div>", w::esc(project_text_), w::esc(project_error_));
    out += "<div class=\"dbtns\"><button class=\"btn\" data-dlg=\"browse\" title=\"Choose a folder with the system file dialog\"><span>Browse...</span></button><div class=\"grow\"></div><button class=\"btn\" data-dlg=\"cancel\"><span>Cancel</span></button><button class=\"btn primary\" data-dlg=\"ok\"><span>Open</span></button></div>";
    dlg->SetInnerRML(out);
    if (Rml::Element* f = by_id("proj-field")) f->Focus();
}

void Shell::open_project_confirm(const std::string& path) {
    const fs::path dir = platform::path_from_utf8(path);
    std::error_code ec;
    if (path.empty() || !fs::exists(dir / "project.toml", ec)) {
        project_error_ = path.empty() ? "Type or pick a project folder." : std::format("'{}' has no project.toml.", path);
        if (Rml::Element* e = by_id("proj-error")) e->SetInnerRML(w::esc(project_error_));
        return;
    }
    project_error_.clear();
    close_dialog();
    ctl_.request_switch_project(path);
}

} // namespace cuervo::editor

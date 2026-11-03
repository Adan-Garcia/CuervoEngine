#include "editor/rml/rml_shell.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml_widgets.hpp"
#include "editor/tree_view.hpp"
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "cuervo/core/log.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "editor/icons.hpp"

namespace cuervo::editor {

namespace {

using dom::find_attr;
using dom::inside;
using dom::tid_is;
using dom::typing;
using dom::icon_img;

struct MenuSpec {
    const char* name;
    std::vector<const char*> ids; // "-" is a separator
};

const std::vector<MenuSpec>& menus() {
    static const std::vector<MenuSpec> m = {
        {"file", {"file.new", "file.open", "file.save", "file.save_as", "-", "file.new_project", "project.switch", "project.close", "-", "file.open_code_editor", "file.open_scripts_editor", "-", "file.quit"}},
        {"edit", {"edit.undo", "edit.redo", "-", "edit.duplicate", "edit.delete", "edit.group", "-", "edit.add_child", "-", "select.all", "select.parent", "select.children", "select.siblings", "select.none"}},
        {"play", {"play.play", "play.scene", "play.record", "play.pause", "play.step", "play.stop", "-", "play.reload"}},
        {"view", {"view.frame", "view.zoom_in", "view.zoom_out", "view.zoom_reset", "view.zoom100", "-", "view.snap", "view.collision_shapes", "view.lighting", "view.ready_preview", "-", "view.pane_scene", "view.pane_assets", "view.pane_search", "view.pane_settings", "view.left", "view.right", "-", "view.drawer_console", "view.drawer_history", "view.drawer_profiler", "view.drawer_replay", "view.drawer_tiers", "view.drawer_input", "view.drawer_debugger", "view.drawer_sprite", "view.drawer", "-", "view.theme_light", "view.theme_dark", "-", "view.layout_save", "view.layout_reset"}},
        {"help", {"help.palette", "help.license"}},
    };
    return m;
}

int tool_index(Tool t) { return static_cast<int>(t); }

} // namespace

std::string rml_escape(std::string_view v) { return w::esc(v); }

std::string shortcut_string(const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN) return {};
    const SDL_Keycode k = ev.key.key;
    if (k == SDLK_LCTRL || k == SDLK_RCTRL || k == SDLK_LSHIFT || k == SDLK_RSHIFT || k == SDLK_LALT || k == SDLK_RALT) return {};
    std::string name = k == SDLK_DELETE ? "Del" : SDL_GetKeyName(k);
    if (name.empty()) return {};
    std::string out;
    if ((ev.key.mod & SDL_KMOD_CTRL) != 0) out += "Ctrl+";
    if ((ev.key.mod & SDL_KMOD_SHIFT) != 0) out += "Shift+";
    if ((ev.key.mod & SDL_KMOD_ALT) != 0) out += "Alt+";
    return out + name;
}

Shell::Shell(EditorController& ctl, ShellHost host, bool persist) : ctl_(ctl), host_(std::move(host)), persist_(persist) {
    layout_ = ctl_.settings().rml_layout();
    if (persist_) recents_.load(RecentProjects::user_file());
    register_actions();
    ctl_.hooks.scripts_reloaded = [this] { register_actions(); };
}

void Shell::set_layout(const RmlLayout& l) {
    layout_ = l;
    if (page_ != nullptr) apply_layout();
}

void Shell::reset_panel_layout() {
    layout_.set_panels(RmlPanels{});
    layout_.rail = "settings"; // keep the pane the button lives in on screen
    apply_layout();
    save_layout();
    ctl_.notify_ok("Panel layout reset.");
}

void Shell::save_named_layout(const std::string& name) {
    const std::string clean = EditorSettings::clean_layout_name(name);
    if (!ctl_.settings().save_layout_as(clean, layout_.panels())) {
        ctl_.notify_ok("Name the layout first.");
        return;
    }
    layout_name_.clear();
    save_layout();
    register_actions(); // the palette lists "Load Layout: <name>"
    build_settings();
    ctl_.notify_ok(std::format("Saved layout {}.", clean));
}

bool Shell::load_named_layout(const std::string& name) {
    const auto& all = ctl_.settings().layouts();
    const auto it = all.find(name);
    if (it == all.end()) return false;
    layout_.set_panels(it->second);
    apply_layout();
    save_layout();
    ctl_.notify_ok(std::format("Loaded layout {}.", name));
    return true;
}

void Shell::apply_start(const StartOptions& o) {
    if (!o.layout.empty() && !load_named_layout(o.layout)) LOG_WARN("--layout: no saved layout '{}'", o.layout);
    for (const std::string& raw : o.tabs) {
        std::string t;
        for (char c : raw) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // ImGui panel titles and the Rml names for the same panel.
        static const std::pair<const char*, const char*> map[] = {
            {"scene", "view.pane_scene"}, {"scene tree", "view.pane_scene"}, {"assets", "view.pane_assets"}, {"search", "view.pane_search"},
            {"settings", "view.pane_settings"}, {"console", "view.drawer_console"}, {"history", "view.drawer_history"},
            {"profiler", "view.drawer_profiler"}, {"replay", "view.drawer_replay"}, {"tiers", "view.drawer_tiers"}, {"tier report", "view.drawer_tiers"},
            {"input map", "view.drawer_input"}, {"input", "view.drawer_input"}, {"debugger", "view.drawer_debugger"}, {"sprites", "view.drawer_sprite"}, {"sprite", "view.drawer_sprite"}};
        bool found = false;
        for (const auto& [name, id] : map) {
            if (t == name) { found = ctl_.actions().run(id); break; }
        }
        if (!found) LOG_WARN("--tabs: no panel '{}'", raw);
    }
    force_welcome_ = o.welcome;
    start_palette_ = o.palette;
}

void Shell::save_layout() {
    ctl_.settings().rml_layout() = layout_;
    layout_dirty_ = false;
    if (!persist_) return;
    std::string error;
    if (!ctl_.settings().save(EditorSettings::user_file(), error)) LOG_WARN("editor settings: {}", error);
}

void Shell::register_actions() {
    ActionRegistry& reg = ctl_.actions();
    reg.reset_actions();
    add_document_actions(reg, ctl_.doc());
    const auto add = [&](std::string id, std::string title, std::string cat, std::string key, std::function<bool()> enabled, std::function<bool()> run) {
        reg.add({std::move(id), std::move(title), std::move(cat), std::move(key), std::move(enabled), std::move(run)});
    };
    const auto idle = [this] { return !ctl_.play().active(); };
    add("file.new", "New Scene", "File", "Ctrl+N", idle, [this] {
        if (ctl_.doc().dirty()) open_dialog(Dialog::DiscardNew);
        else {
            ctl_.doc().new_scene();
            if (host_.frame_scene) host_.frame_scene();
        }
        return true;
    });
    add("file.open", "Open Scene...", "File", "Ctrl+O", idle, [this] { open_dialog(Dialog::OpenScene); return true; });
    add("file.save", "Save Scene", "File", "Ctrl+S", idle, [this] { ctl_.save_current_scene(); return true; });
    add("file.save_as", "Save Scene As...", "File", "Ctrl+Shift+S", idle, [this] { open_dialog(Dialog::SaveAs); return true; });
    add("file.quit", "Quit", "File", "Ctrl+Q", {}, [this] {
        if (ctl_.doc().dirty()) open_dialog(Dialog::DiscardQuit);
        else if (host_.quit) host_.quit();
        return true;
    });
    add("file.open_code_editor", "Open Project in Code Editor", "File", "", {}, [this] { return ctl_.open_in_external_editor(ctl_.env().project_dir()); });
    add("file.open_scripts_editor", "Open Scripts Folder in Code Editor", "File", "", {},
        [this] { return ctl_.open_in_external_editor(ctl_.env().project_dir() / "scripts"); });
    add("edit.add_child", "Add Child Node...", "Edit", "Ctrl+A", idle, [this] {
        ctl_.request_add_child(ctl_.doc().selection() != 0 ? ctl_.doc().selection() : ctl_.doc().model().root());
        return true;
    });
    add("project.switch", "Open Project...", "Project", "", idle, [this] {
        project_text_.clear();
        project_error_.clear();
        open_dialog(Dialog::OpenProject);
        return true;
    });
    add("project.close", "Close Project", "Project", "", [this] { return ctl_.env().has_project(); }, [this] { ctl_.request_switch_project(""); return true; });
    for (const RecentProject& r : recents_.list()) {
        if (!r.exists || (ctl_.env().has_project() && std::filesystem::equivalent(r.path, ctl_.env().project_dir()))) continue;
        const std::string path = r.path;
        add("project.recent." + path, "Open Recent: " + r.name, "Project", "", idle, [this, path] { ctl_.request_switch_project(path); return true; });
    }
    // ---- play
    add("play.play", "Play", "Play", "F5", [this] { return !ctl_.play().active() || ctl_.play().state() == PlayState::Paused; }, [this] {
        if (ctl_.play().active()) ctl_.play().resume();
        else return ctl_.start_play(false);
        return true;
    });
    add("play.scene", "Play Current Scene", "Play", "F6", idle, [this] { return ctl_.start_play(true); });
    add("play.record", "Play and Record", "Play", "", idle, [this] { return ctl_.start_play(false, true); });
    add("play.pause", "Pause", "Play", "F7", [this] { return ctl_.play().state() == PlayState::Playing; }, [this] { ctl_.play().pause(); return true; });
    add("play.step", "Step One Tick", "Play", "F10", [this] { return ctl_.play().active(); }, [this] {
        ctl_.play().pause();
        ctl_.play().step(idle_, {0.0f, 0.0f}, 1);
        return true;
    });
    add("play.stop", "Stop", "Play", "Shift+F5", [this] { return ctl_.play().active(); }, [this] { ctl_.stop_play(); return true; });
    add("play.reload", "Reload Scripts", "Play", "", [this] { return ctl_.play().active(); }, [this] { ctl_.play().reload(true); return true; });
    // ---- view
    add("view.frame", "Frame Scene", "View", "F", {}, [this] { if (host_.frame_scene) host_.frame_scene(); return true; });
    // As in the ImGui editor, Ctrl+= / Ctrl+- / Ctrl+0 change the interface scale; the scene view zooms with
    // the wheel, "Zoom to 100%" and Frame Scene.
    const auto rescale = [this](float to) {
        layout_.ui_scale = std::clamp(std::round(to * 100.0f) / 100.0f, 0.5f, 3.0f);
        if (host_.set_scale) host_.set_scale(layout_.ui_scale);
        save_layout();
        if (Rml::Element* e = by_id("settings"); e != nullptr && layout_.rail == "settings") build_settings();
        return true;
    };
    const auto cur_scale = [this] { return layout_.ui_scale > 0.0f ? layout_.ui_scale : 1.0f; };
    add("view.zoom100", "Zoom to 100%", "View", "", {}, [this] { if (host_.zoom_by) host_.zoom_by(0.0f); return true; });
    add("view.zoom_in", "Zoom In", "View", "Ctrl+=", {}, [=] { return rescale(cur_scale() + 0.10f); });
    add("view.zoom_out", "Zoom Out", "View", "Ctrl+-", {}, [=] { return rescale(cur_scale() - 0.10f); });
    add("view.zoom_reset", "Reset Zoom", "View", "Ctrl+0", {}, [=] { return rescale(1.0f); });
    add("view.ready_preview", "Toggle Scene Preview (run ready())", "View", "", idle, [this] { ctl_.toggle_ready_preview(); return true; });
    add("view.snap", "Toggle Snapping", "View", "", {}, [this] { ctl_.set_snap(!ctl_.snap()); return true; });
    add("view.collision_shapes", "Toggle Collision Shapes", "View", "F3", {}, [this] { ctl_.set_collision_shapes(!ctl_.collision_shapes()); return true; });
    add("view.lighting", "Toggle Lighting Preview", "View", "", {}, [this] { ctl_.set_preview_lighting(!ctl_.preview_lighting()); return true; });
    for (const char* r : {"scene", "assets", "search", "settings"}) {
        std::string title = r;
        title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
        add(std::string("view.pane_") + r, title + " Pane", "View", "", {}, [this, r] { show_rail(r, false); return true; });
    }
    add("view.left", "Left Sidebar", "View", "Ctrl+B", {}, [this] { layout_.left_collapsed = !layout_.left_collapsed; apply_layout(); save_layout(); return true; });
    add("view.right", "Right Column", "View", "Ctrl+Alt+B", {}, [this] { layout_.right_collapsed = !layout_.right_collapsed; apply_layout(); save_layout(); return true; });
    add("view.drawer", "Bottom Drawer", "View", "Ctrl+J", {}, [this] { layout_.drawer_open = !layout_.drawer_open; console_dirty_ = true; history_rev_ = ~0ull; apply_layout(); save_layout(); return true; });
    for (const char* d : {"console", "history", "profiler", "replay", "tiers", "input", "debugger", "sprite"}) {
        std::string title = d;
        title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
        add(std::string("view.drawer_") + d, title + " Tab", "View", "", {}, [this, d] { layout_.drawer_open = true; layout_.drawer = d; console_dirty_ = true; history_rev_ = ~0ull; apply_layout(); save_layout(); return true; });
    }
    add("view.layout_save", "Save Layout...", "View", "", {}, [this] {
        show_rail("settings", false);
        if (Rml::Element* f = by_id("layout-name")) f->Focus();
        return true;
    });
    add("view.layout_reset", "Reset Panel Layout", "View", "", {}, [this] { reset_panel_layout(); return true; });
    for (const auto& [name, panels] : ctl_.settings().layouts()) {
        add("view.layout_load." + name, "Load Layout: " + name, "View", "", {}, [this, name] { return load_named_layout(name); });
    }
    add("view.theme_light", "Light Theme", "View", "", {}, [this] { if (host_.set_theme) host_.set_theme(brand::Theme::Light); return true; });
    add("view.theme_dark", "Dark Theme", "View", "", {}, [this] { if (host_.set_theme) host_.set_theme(brand::Theme::Dark); return true; });
    register_script_actions(); // Script mode commands (script_glue.cpp)
    add("file.new_project", "New Project...", "File", "", {}, [this] { open_new_project(); return true; });
    add("help.license", "License", "Help", "", {}, [this] { open_dialog(Dialog::License); return true; });
    add("help.palette", "Command Palette", "Help", "Ctrl+Shift+P", {}, [this] { open_palette({}); return true; });
}

// ------------------------------------------------------------------ layout

void Shell::set_class_if(Rml::Element* e, const char* cls, bool on) {
    if (e != nullptr && e->IsClassSet(cls) != on) e->SetClass(cls, on);
}

void Shell::apply_layout() {
    const auto dp = [](int v) { return std::format("{}dp", v); };
    if (Rml::Element* e = by_id("left")) {
        e->SetProperty("width", dp(layout_.left_width));
        e->SetProperty("display", layout_.left_collapsed ? "none" : "flex");
    }
    if (Rml::Element* e = by_id("sp-left")) e->SetProperty("display", layout_.left_collapsed ? "none" : "block");
    if (Rml::Element* e = by_id("right")) {
        e->SetProperty("width", dp(layout_.right_width));
        e->SetProperty("display", layout_.right_collapsed ? "none" : "flex");
    }
    if (Rml::Element* e = by_id("sp-right")) e->SetProperty("display", layout_.right_collapsed ? "none" : "block");
    if (Rml::Element* e = by_id("right-open")) e->SetProperty("display", layout_.right_collapsed ? "block" : "none");
    if (Rml::Element* e = by_id("drawer")) {
        e->SetProperty("height", dp(layout_.drawer_height));
        e->SetProperty("display", layout_.drawer_open ? "flex" : "none");
    }
    if (Rml::Element* e = by_id("sp-drawer")) e->SetProperty("display", layout_.drawer_open ? "block" : "none");
    // Script mode swaps the Scene pane for the script list and outline.
    for (const char* pane : {"scene", "assets", "search", "settings", "script"}) {
        const bool script_mode = layout_.center == "script";
        const bool on = std::string(pane) == "script" ? (script_mode && layout_.rail == "scene") : (std::string(pane) == "scene" ? (!script_mode && layout_.rail == "scene") : layout_.rail == pane);
        set_class_if(by_id(std::format("pane-{}", pane).c_str()), "active", on);
    }
    if (Rml::Element* rail = by_id("rail")) {
        for (int i = 0; i < rail->GetNumChildren(); ++i) {
            Rml::Element* b = rail->GetChild(i);
            if (b->HasAttribute("data-rail")) set_class_if(b, "active", !layout_.left_collapsed && b->GetAttribute<Rml::String>("data-rail", "") == layout_.rail);
        }
    }
    if (Rml::Element* seg = page_->QuerySelector(".seg")) {
        for (int i = 0; i < seg->GetNumChildren(); ++i) {
            Rml::Element* b = seg->GetChild(i);
            set_class_if(b, "active", b->GetAttribute<Rml::String>("data-center", "") == layout_.center);
        }
    }
    const bool scene = layout_.center == "scene";
    if (Rml::Element* e = by_id("canvas")) e->SetProperty("display", scene ? "block" : "none");
    if (Rml::Element* e = by_id("script-pane")) e->SetProperty("display", scene ? "none" : "flex");
    if (Rml::Element* e = by_id("strip")) {
        for (int i = 0; i < e->GetNumChildren(); ++i) {
            Rml::Element* b = e->GetChild(i);
            if (b->HasAttribute("data-drawer")) set_class_if(b, "active", layout_.drawer_open && b->GetAttribute<Rml::String>("data-drawer", "") == layout_.drawer);
        }
    }
    if (layout_.drawer_open) build_drawer();
}

void Shell::show_rail(const std::string& r, bool toggle) {
    if (toggle && layout_.rail == r) layout_.left_collapsed = !layout_.left_collapsed;
    else { layout_.rail = r; layout_.left_collapsed = false; }
    apply_layout();
    save_layout();
    if (r == "search" && !layout_.left_collapsed) if (Rml::Element* f = by_id("search-field")) f->Focus();
}

bool Shell::menu_checked(std::string_view id) const {
    if (id == "view.snap") return ctl_.snap();
    if (id == "view.collision_shapes") return ctl_.collision_shapes();
    if (id == "view.lighting") return ctl_.preview_lighting();
    if (id == "view.ready_preview") return ctl_.ready_preview_on();
    if (id == "view.left") return !layout_.left_collapsed;
    if (id == "view.right") return !layout_.right_collapsed;
    if (id == "view.drawer") return layout_.drawer_open;
    if (id.starts_with("view.pane_")) return !layout_.left_collapsed && layout_.rail == id.substr(10);
    if (id.starts_with("view.drawer_")) return layout_.drawer_open && layout_.drawer == id.substr(12);
    if (id == "view.theme_light") return layout_.theme == "light";
    if (id == "view.theme_dark") return layout_.theme != "light";
    return false;
}

void Shell::set_drawer(const std::string& which) {
    if (layout_.drawer_open && layout_.drawer == which) layout_.drawer_open = false;
    else {
        layout_.drawer_open = true;
        layout_.drawer = which;
        console_dirty_ = true;
        history_rev_ = ~0ull;
    }
    apply_layout();
    save_layout();
}

void Shell::begin_drag(Rml::Element* split, Rml::Vector2f at) {
    drag_ = split;
    drag_start_ = at;
    split->SetClass("drag", true);
    const std::string id = split->GetId();
    drag_size_ = id == "sp-left" ? layout_.left_width : id == "sp-right" ? layout_.right_width : layout_.drawer_height;
}

void Shell::drag_to(Rml::Vector2f at) {
    if (drag_ == nullptr) return;
    const float ratio = ctx_->GetDensityIndependentPixelRatio();
    const int dx = static_cast<int>(std::lround((at.x - drag_start_.x) / ratio));
    const int dy = static_cast<int>(std::lround((at.y - drag_start_.y) / ratio));
    const std::string id = drag_->GetId();
    const int screen_w = static_cast<int>(ctx_->GetDimensions().x / ratio);
    if (id == "sp-left") layout_.left_width = std::clamp(drag_size_ + dx, 160, std::max(160, screen_w - layout_.right_width - 260));
    else if (id == "sp-right") layout_.right_width = std::clamp(drag_size_ - dx, 200, std::max(200, screen_w - layout_.left_width - 260));
    else layout_.drawer_height = std::clamp(drag_size_ - dy, 80, std::max(80, static_cast<int>(ctx_->GetDimensions().y / ratio) - 200));
    layout_dirty_ = true;
    apply_layout();
}

void Shell::end_drag() {
    if (drag_ == nullptr) return;
    drag_->SetClass("drag", false);
    drag_ = nullptr;
    if (layout_dirty_) save_layout();
}

// ------------------------------------------------------------------ bind and refresh

void Shell::bind(Rml::Context* ctx, Rml::ElementDocument* page) {
    ctx_ = ctx;
    page_ = page;
    for (const char* ev : {"click", "mousedown", "mousemove", "mouseup", "mouseover", "change", "keydown", "mousescroll"}) page->AddEventListener(ev, this);
    init_script_panel();
    build_settings();
    build_assets();
    apply_layout();
    revision_ = ~0ull;
    refresh();
}

void Shell::hide_tip() {
    tip_target_ = nullptr;
    if (tip_shown_) if (Rml::Element* e = by_id("tooltip")) e->SetProperty("display", "none");
    tip_shown_ = false;
}

void Shell::tip_over(Rml::Element* t) {
    Rml::Element* e = find_attr(t, "title");
    if (e == tip_target_) return;
    hide_tip();
    tip_target_ = e;
    tip_since_ = SDL_GetTicks();
}

void Shell::update_tip() {
    if (tip_target_ == nullptr || tip_shown_ || SDL_GetTicks() - tip_since_ < 400) return;
    Rml::Element* tip = by_id("tooltip");
    if (tip == nullptr) return;
    tip->SetInnerRML(w::tip_body(w::split_tip(tip_target_->GetAttribute<Rml::String>("title", ""))));
    const Rml::Vector2f dims{static_cast<float>(ctx_->GetDimensions().x), static_cast<float>(ctx_->GetDimensions().y)};
    const float x = std::min(tip_target_->GetAbsoluteLeft(), dims.x - tip->GetOffsetWidth() - 8.0f);
    float y = tip_target_->GetAbsoluteTop() + tip_target_->GetOffsetHeight() + 4.0f;
    if (y + tip->GetOffsetHeight() > dims.y) y = tip_target_->GetAbsoluteTop() - tip->GetOffsetHeight() - 4.0f;
    tip->SetProperty("left", std::format("{}px", static_cast<int>(std::max(4.0f, x))));
    tip->SetProperty("top", std::format("{}px", static_cast<int>(std::max(0.0f, y))));
    tip->SetProperty("display", "flex");
    tip_shown_ = true;
}

void Shell::refresh() {
    poll_native_pick();
    if (start_palette_) open_palette(*std::exchange(start_palette_, std::nullopt));
    update_tip();
    ctl_.messages().update(ctl_.clock());
    const MessageCenter& mc = ctl_.messages();
    toast_ = mc.empty() ? std::string() : mc.items().back().message.what;

    // Project and scene name; the path changes on Save As and when another project opens.
    if (Rml::Element* t = by_id("doc-title")) {
        const std::string title = std::format("{}<span class=\"bar\">|</span>{}", rml_escape(ctl_.env().has_project() ? ctl_.env().project().name : "Cuervo"),
                                              rml_escape(!ctl_.env().has_project() ? std::string("welcome") : ctl_.doc().has_path() ? ctl_.env().relative(ctl_.doc().path()) : std::string("untitled")));
        if (title != title_) { title_ = title; t->SetInnerRML(title); }
    }
    // Controller prompts become dialogs.
    if (ctl_.prompt().kind != Prompt::None && dialog_ == Dialog::None) {
        const PendingPrompt p = ctl_.take_prompt();
        switch (p.kind) {
        case Prompt::SaveAs: open_dialog(Dialog::SaveAs); break;
        case Prompt::DiscardOpen: open_dialog(Dialog::DiscardOpen); break;
        case Prompt::DiscardLeave: open_dialog(Dialog::DiscardLeave); break;
        case Prompt::AddChild: open_picker(p.node); break;
        case Prompt::AttachScript: {
            attach_node_ = p.node;
            attach_tab_ = 0;
            attach_filter_.clear();
            attach_sel_.clear();
            attach_error_.clear();
            if (const ModelNode* n = ctl_.doc().model().get(p.node)) {
                const auto [cls, path] = EditorController::suggest_script(n->name);
                new_class_ = cls;
                new_path_ = path;
                open_dialog(Dialog::AttachScript);
            }
            break;
        }
        case Prompt::None: break;
        }
    }
    EditorDocument& doc = ctl_.doc();
    const std::uint64_t rev = doc.revision();
    if (rev != revision_ || doc.selection() != selected_) {
        const bool sel_changed = doc.selection() != selected_;
        revision_ = rev;
        selected_ = doc.selection();
        if (sel_changed && selected_ != 0) tree_.reveal(doc.model(), selected_);
        if (rename_ != 0 && sel_changed) rename_ = 0;
        build_tree();
        if (sel_changed && selected_ != 0) tree_scroll_to(selected_);
        build_inspector();
        if (auto* c = by_id("count")) c->SetInnerRML(std::to_string(doc.model().size()));
        const ModelNode* n = doc.model().get(selected_);
        const std::string tab = n != nullptr ? ve::drawer_tab_for(n->type, !ctl_.find_class_script(n->type).first.empty()) : std::string();
        if (tab != node_tab_) {
            node_tab_ = tab;
            if (Rml::Element* e = by_id("node-title")) e->SetInnerRML(rml_escape(tab));
            set_class_if(by_id("tab-node"), "hidden", tab.empty());
        }
        if (node_tab_.empty() && layout_.drawer == "node") { // also right after loading a saved layout
            layout_.drawer = "console";
            apply_layout();
        }
        if (layout_.drawer_open) build_drawer();
    }
    if (ctl_.assets().revision() != assets_rev_) build_assets();
    if (ctl_.console().snapshot(seen_, lines_)) {
        console_dirty_ = true;
        if (layout_.drawer_open && layout_.drawer == "console") build_console();
    }
    if (layout_.drawer_open && layout_.drawer == "history" && history_rev_ != doc.stack().revision() + doc.stack().index()) build_history();
    if (layout_.drawer_open && layout_.drawer == "profiler") build_profiler();
    if (layout_.drawer_open && layout_.drawer == "replay") build_replay();
    poll_capture();
    refresh_script();
    if (layout_.drawer_open && (layout_.drawer == "node" || layout_.drawer == "debugger") && !inspector_busy_) {
        // The Script tab shows the running game: refresh a few times a second, not every frame.
        const std::uint32_t now = SDL_GetTicks();
        const int play_key = static_cast<int>(ctl_.play().state()) * 2 + (ctl_.play().panic().has_value() ? 1 : 0);
        if ((ctl_.play().active() && now - node_drawer_ms_ > 250) || play_key != node_drawer_play_) {
            node_drawer_play_ = play_key; // pause and panic switch the tab at once
            node_drawer_ms_ = now;
            build_node_drawer();
        }
    }
    if (sprite_fit_) sprite_apply();
    if (layout_.drawer_open && layout_.drawer == "node" && node_tab_ == "Particles") step_particles();
    if (layout_.drawer_open && layout_.drawer == "node" && node_tab_ == "Animation") step_timeline();
    if (Rml::Element* tree = by_id("tree"); tree != nullptr && tree->GetScrollTop() != tree_scroll_) {
        tree_scroll_ = tree->GetScrollTop();
        build_tree_window();
    }
    refresh_toolbar();
    refresh_strip();
    refresh_welcome();
    refresh_status();
    if (Rml::Element* body = page_) {
        const bool play = ctl_.play().state() == PlayState::Playing;
        const bool pause = ctl_.play().state() == PlayState::Paused;
        set_class_if(body, "playing", play);
        set_class_if(body, "paused", pause);
    }
}

void Shell::refresh_toolbar() {
    const auto walk = [&](Rml::Element* root) {
        std::vector<Rml::Element*> stack{root};
        while (!stack.empty()) {
            Rml::Element* e = stack.back();
            stack.pop_back();
            for (int i = 0; i < e->GetNumChildren(); ++i) stack.push_back(e->GetChild(i));
            if (e->HasAttribute("data-action") && e->IsClassSet("ibtn")) {
                const Action* a = ctl_.actions().find(e->GetAttribute<Rml::String>("data-action", ""));
                const bool on_state = a != nullptr && ((a->id == "view.snap" && ctl_.snap()) || (a->id == "play.record" && ctl_.play().recording()));
                set_class_if(e, "off", a == nullptr || !ctl_.actions().enabled(*a));
                set_class_if(e, "on", on_state);
            } else if (e->HasAttribute("data-tool")) {
                static const char* names[] = {"select", "move", "rotate", "scale", "paint"};
                set_class_if(e, "on", e->GetAttribute<Rml::String>("data-tool", "") == names[tool_index(ctl_.tool())]);
            }
        }
    };
    walk(by_id("toolbar"));
}

void Shell::refresh_strip() {
    const auto badge = [&](const char* id, const std::string& text, const char* cls) {
        Rml::Element* e = by_id(id);
        if (e == nullptr) return;
        if (e->GetInnerRML() != text) e->SetInnerRML(text);
        set_class_if(e, "err", std::string_view(cls) == "err");
        set_class_if(e, "warn", std::string_view(cls) == "warn");
    };
    const std::size_t errs = ctl_.console().count(log::Level::Error);
    const std::size_t warns = ctl_.console().count(log::Level::Warn);
    badge("n-console", errs > 0 ? std::format("{} err", errs) : warns > 0 ? std::format("{} warn", warns) : std::string(), errs > 0 ? "err" : warns > 0 ? "warn" : "");
    badge("n-history", std::to_string(ctl_.doc().stack().size()), "");
    badge("n-profiler", ctl_.play().active() ? std::format("{:.1f} ms", ctl_.play().average_tick_ms()) : std::string(), "");
    if (Rml::Element* e = by_id("lastlog")) {
        const std::string txt = lines_.empty() ? std::string() : rml_escape(lines_.back().text.substr(0, 120));
        if (e->GetInnerRML() != txt) e->SetInnerRML(txt);
    }
}

void Shell::refresh_status() {
    const PlayController& pc = ctl_.play();
    std::string mode = "Editing";
    if (pc.active()) {
        mode = std::format("{}{}  tick {}", pc.state() == PlayState::Paused ? "Paused" : "Playing", pc.panic() ? " (panic)" : "", pc.tick());
    }
    const auto set = [&](const char* id, const std::string& text) {
        Rml::Element* e = by_id(id);
        if (e != nullptr && e->GetInnerRML() != text) e->SetInnerRML(text);
    };
    set("st-mode", rml_escape(mode));
    set("st-scene", rml_escape(ctl_.env().relative(ctl_.doc().path())) + (ctl_.doc().dirty() ? " *" : ""));
    const ModelNode* n = ctl_.doc().model().get(selected_);
    set("st-sel", !toast_.empty() ? rml_escape(toast_) : n != nullptr ? "Selected: " + rml_escape(n->name) : std::string());
    const std::string readout = host_.view_readout ? host_.view_readout() : std::string();
    set("hud", std::format("{}%  {}{}", host_.zoom_percent ? static_cast<int>(std::lround(host_.zoom_percent())) : 100, rml_escape(tool_name(ctl_.tool())), readout.empty() ? std::string() : "  " + rml_escape(readout)));
}

// ------------------------------------------------------------------ panes

constexpr int kRowDp = 22;

void Shell::persist_tree() {
    const std::string scene = ctl_.env().relative(ctl_.doc().path());
    if (scene.empty()) return;
    layout_.tree_scene = scene;
    layout_.tree_collapsed = tree_.save(ctl_.doc().model());
    save_layout();
}

void Shell::build_tree() {
    const SceneModel& model = ctl_.doc().model();
    // A different scene file: start from what was saved for it (everything open when nothing was).
    if (const std::string scene = ctl_.env().relative(ctl_.doc().path()); scene != tree_scene_) {
        tree_scene_ = scene;
        tree_ = TreeState();
        if (!scene.empty() && layout_.tree_scene == scene) tree_.load(model, layout_.tree_collapsed);
    }
    tree_.prune(model);
    rows_ = tree_.flatten(model);
    if (rename_ != 0 && !model.contains(rename_)) rename_ = 0;
    if (Rml::Element* in = by_id("tree-in")) in->SetProperty("height", std::format("{}dp", rows_.size() * kRowDp + 8));
    built_ = {~std::size_t{0}, 0};
    build_tree_window();
}

// Only the rows near the viewport exist as elements (thousands of nodes stay cheap, DESIGN.md 4.13).
void Shell::build_tree_window() {
    Rml::Element* tree = by_id("tree");
    Rml::Element* in = by_id("tree-in");
    if (tree == nullptr || in == nullptr) return;
    const float ratio = ctx_->GetDensityIndependentPixelRatio();
    const float px = ratio * kRowDp;
    const float view = tree->GetClientHeight() > 0 ? tree->GetClientHeight() : 600.0f * ratio;
    const RowWindow tight = row_window(rows_.size(), tree->GetScrollTop(), view, px, 0);
    const bool have = built_.first != ~std::size_t{0};
    if (have && (tight.first >= built_.first || built_.first == 0) && (tight.last <= built_.last || built_.last >= rows_.size())) return;
    const RowWindow win = row_window(rows_.size(), tree->GetScrollTop(), view, px, 12);
    built_ = win;
    const ClassRegistry& classes = ctl_.env().classes();
    const SceneModel& model = ctl_.doc().model();
    std::string out = std::format("<div style=\"height: {}dp\"></div>", win.first * kRowDp + 4);
    for (std::size_t i = win.first; i < win.last; ++i) {
        const TreeRow& r = rows_[i];
        const ModelNode* n = model.get(r.id);
        if (n == nullptr) continue;
        const std::string_view icon = icon_for_class(&classes, n->type);
        const std::string lead = r.has_children ? std::format("<span class=\"chev\" data-chev=\"{}\">{}</span>", r.id, icon_img(r.expanded ? "chevron_down" : "chevron_right", 12, "chev")) : std::string();
        const bool renaming = rename_ == r.id;
        std::string row = w::row(std::format("data-id=\"{}\"", r.id), renaming ? "@@" : n->name, n->type,
                                 {.selected = ctl_.doc().is_selected(r.id), .indent = r.depth * 14}, icon.empty() ? "node" : icon, lead);
        if (renaming) {
            const std::size_t at = row.find("@@");
            if (at != std::string::npos) row.replace(at, 2, std::format("<input id=\"rename-field\" type=\"text\" value=\"{}\"/>", rml_escape(n->name)));
        }
        out += row;
    }
    in->SetInnerRML(out);
    drop_row_ = nullptr;
    if (rename_ != 0) {
        if (Rml::Element* f = by_id("rename-field")) {
            f->Focus();
            if (auto* inp = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(f)) inp->Select();
        }
    }
}

void Shell::tree_scroll_to(NodeId id) {
    Rml::Element* tree = by_id("tree");
    if (tree == nullptr) return;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].id != id) continue;
        const float px = ctx_->GetDensityIndependentPixelRatio() * kRowDp;
        const float top = static_cast<float>(i) * px + 4.0f, view = tree->GetClientHeight();
        if (top < tree->GetScrollTop()) tree->SetScrollTop(top - px);
        else if (top + px > tree->GetScrollTop() + view) tree->SetScrollTop(top + 2.0f * px - view);
        return;
    }
}

void Shell::begin_rename(NodeId id) {
    if (!ctl_.doc().model().contains(id)) return;
    rename_ = id;
    built_ = {~std::size_t{0}, 0};
    build_tree_window();
}

void Shell::end_rename(bool commit) {
    const NodeId id = rename_;
    if (id == 0) return;
    rename_ = 0;
    if (commit) {
        if (Rml::Element* f = by_id("rename-field")) {
            const std::string name = f->GetAttribute<Rml::String>("value", "");
            const ModelNode* n = ctl_.doc().model().get(id);
            if (n != nullptr && !name.empty() && name != n->name) {
                ctl_.report(ctl_.doc().rename(id, name));
                ctl_.doc().seal();
            }
        }
    }
    built_ = {~std::size_t{0}, 0};
    revision_ = ~0ull;
}

void Shell::tree_select(NodeId id, bool toggle, bool range) {
    EditorDocument& doc = ctl_.doc();
    if (range && anchor_ != 0) {
        const std::vector<NodeId> ids = range_between(rows_, anchor_, id);
        if (!ids.empty()) {
            doc.select_set(ids);
            return;
        }
    }
    if (toggle) doc.select_toggle(id);
    else doc.select(id);
    anchor_ = id;
}

void Shell::open_tree_menu(NodeId id, Rml::Vector2f at) {
    Rml::Element* p = by_id("popup");
    if (p == nullptr) return;
    EditorDocument& doc = ctl_.doc();
    if (!doc.is_selected(id)) tree_select(id, false, false);
    const bool root = id == doc.model().root();
    const bool script = ctl_.has_script_bound(id);
    std::vector<w::MenuItem> items = {{"add", "Add child node...", "Ctrl+A"}, {"rename", "Rename", "F2"}, {"dup", "Duplicate", "Ctrl+D", root}, {"group", "Group", "Ctrl+G", root},
                                      {"up", "Move up", "", root}, {"down", "Move down", "", root}, {"-", "", ""}};
    if (script) {
        items.push_back({"script-open", "Open script", ""});
        items.push_back({"script-change", "Change script...", ""});
        items.push_back({"script-detach", "Detach script", ""});
    } else {
        items.push_back({"script-attach", "Attach script...", ""});
    }
    items.push_back({"-", "", ""});
    items.push_back({"delete", "Delete", "Del", root});
    ctx_node_ = id;
    p->SetInnerRML(w::popup_menu("data-ctx", items));
    p->SetProperty("width", "220dp");
    p->SetProperty("left", std::format("{}px", static_cast<int>(at.x)));
    p->SetProperty("top", std::format("{}px", static_cast<int>(at.y)));
    p->SetProperty("display", "block");
    popup_open_ = true;
    palette_ = false;
}

void Shell::run_tree_menu(const std::string& key) {
    EditorDocument& doc = ctl_.doc();
    const NodeId id = ctx_node_;
    if (key == "add") ctl_.request_add_child(id);
    else if (key == "rename") begin_rename(id);
    else if (key == "dup") ctl_.report(doc.duplicate_selection());
    else if (key == "group") ctl_.report(doc.group_selection() != 0);
    else if (key == "up") ctl_.report(move_sibling(doc, id, -1));
    else if (key == "down") ctl_.report(move_sibling(doc, id, 1));
    else if (key == "delete") ctl_.report(doc.remove_selection());
    else if (key == "script-attach" || key == "script-change") ctl_.request_attach_script(id);
    else if (key == "script-detach") ctl_.report(ctl_.detach_script(id));
    else if (key == "script-open") {
        const auto [path, line] = ctl_.node_script(id);
        if (!path.empty()) open_script(ctl_.env().resolve(path), line);
    }
}

void Shell::tree_drag_update(Rml::Element* over, Rml::Vector2f at) {
    Rml::Element* ghost = by_id("ghost");
    Rml::Element* line = by_id("dropline");
    if (ghost != nullptr) {
        ghost->SetProperty("left", std::format("{}px", static_cast<int>(at.x + 12)));
        ghost->SetProperty("top", std::format("{}px", static_cast<int>(at.y + 8)));
    }
    if (drop_row_ != nullptr) {
        drop_row_->SetClass("dropinto", false);
        drop_row_ = nullptr;
    }
    drop_ = {};
    Rml::Element* tree = by_id("tree");
    if (tree != nullptr) { // edge auto-scroll
        const float edge = 20.0f * ctx_->GetDensityIndependentPixelRatio();
        const float top = tree->GetAbsoluteTop(), bottom = top + tree->GetClientHeight();
        if (at.y < top + edge && at.y > top - edge) tree->SetScrollTop(tree->GetScrollTop() - 8.0f);
        else if (at.y > bottom - edge && at.y < bottom + edge) tree->SetScrollTop(tree->GetScrollTop() + 8.0f);
    }
    Rml::Element* row = find_attr(over, "data-id");
    bool shown = false;
    if (row != nullptr && inside(over, "tree")) {
        const NodeId over_id = static_cast<NodeId>(std::atoi(row->GetAttribute<Rml::String>("data-id", "0").c_str()));
        const float frac = (at.y - row->GetAbsoluteTop()) / std::max(1.0f, row->GetOffsetHeight());
        drop_ = drop_target(ctl_.doc().model(), press_node_, over_id, frac);
        if (drop_.zone == DropZone::Into) {
            row->SetClass("dropinto", true);
            drop_row_ = row;
        } else if (line != nullptr && drop_.valid()) {
            const float y = drop_.zone == DropZone::Before ? row->GetAbsoluteTop() : row->GetAbsoluteTop() + row->GetOffsetHeight();
            line->SetProperty("left", std::format("{}px", static_cast<int>(row->GetAbsoluteLeft())));
            line->SetProperty("top", std::format("{}px", static_cast<int>(y) - 1));
            line->SetProperty("width", std::format("{}px", static_cast<int>(row->GetOffsetWidth())));
            line->SetProperty("display", "block");
            shown = true;
        }
    }
    if (!shown && line != nullptr) line->SetProperty("display", "none");
}

void Shell::tree_drag_end(bool drop) {
    if (drop_row_ != nullptr) {
        drop_row_->SetClass("dropinto", false);
        drop_row_ = nullptr;
    }
    for (const char* id : {"ghost", "dropline"}) if (Rml::Element* e = by_id(id)) e->SetProperty("display", "none");
    const NodeId dragged = press_node_;
    press_node_ = 0;
    tree_dragging_ = false;
    if (drop && drop_.valid()) {
        if (apply_drop(ctl_.doc(), dragged, drop_)) tree_.reveal(ctl_.doc().model(), dragged);
        else ctl_.report(false);
    }
    drop_ = {};
}

void Shell::tree_key(Rml::Input::KeyIdentifier key) {
    EditorDocument& doc = ctl_.doc();
    std::size_t at = rows_.size();
    for (std::size_t i = 0; i < rows_.size(); ++i) if (rows_[i].id == doc.selection()) at = i;
    using K = Rml::Input::KeyIdentifier;
    NodeId pick = 0;
    if (key == K::KI_DOWN) pick = at + 1 < rows_.size() ? rows_[at + 1].id : 0;
    else if (key == K::KI_UP) pick = at != rows_.size() && at > 0 ? rows_[at - 1].id : 0;
    else if (key == K::KI_HOME && !rows_.empty()) pick = rows_.front().id;
    else if (key == K::KI_END && !rows_.empty()) pick = rows_.back().id;
    else if (key == K::KI_RIGHT && at < rows_.size()) {
        if (rows_[at].has_children && !rows_[at].expanded) { tree_.set_expanded(rows_[at].id, true); persist_tree(); revision_ = ~0ull; }
        else if (rows_[at].has_children && at + 1 < rows_.size()) pick = rows_[at + 1].id;
    } else if (key == K::KI_LEFT && at < rows_.size()) {
        const ModelNode* n = doc.model().get(rows_[at].id);
        if (rows_[at].has_children && rows_[at].expanded) { tree_.set_expanded(rows_[at].id, false); persist_tree(); revision_ = ~0ull; }
        else if (n != nullptr && n->parent != 0) pick = n->parent;
    } else if (key == K::KI_F2 && doc.selection() != 0) {
        begin_rename(doc.selection());
    }
    if (pick != 0) {
        tree_select(pick, false, false);
        tree_scroll_to(pick);
    }
}

void Shell::build_settings() {
    Rml::Element* e = by_id("settings");
    if (e == nullptr) return;
    const float cur = layout_.ui_scale > 0.0f ? layout_.ui_scale : 1.0f;
    const auto seg = [](const char* attr, const std::string& val, const std::string& label, bool on) {
        return std::format("<div class=\"segi{}\" {}=\"{}\">{}</div>", on ? " active" : "", attr, val, label);
    };
    std::string out = "<div class=\"section\"><span>Theme</span></div><div class=\"opts\">";
    out += seg("data-theme", "system", "System", layout_.theme.empty());
    out += seg("data-theme", "dark", "Dark", layout_.theme == "dark");
    out += seg("data-theme", "light", "Light", layout_.theme == "light") + "</div>";
    out += "<div class=\"section\"><span>Scale</span><span class=\"count\">interface and text</span></div><div class=\"opts\">";
    for (const float sc : {0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) out += seg("data-scale", std::format("{}", sc), std::format("{:.2f}", sc), std::abs(cur - sc) < 0.01f);
    out += "</div>";
    out += std::format("<div class=\"opts\"><button class=\"btn sm\" data-act=\"scale-down\" title=\"Smaller (-0.05)\"><span>-</span></button><span class=\"val\">{:.2f}</span>"
                       "<button class=\"btn sm\" data-act=\"scale-up\" title=\"Larger (+0.05)\"><span>+</span></button></div>", cur);
    out += "<div class=\"section\"><span>Layout</span></div><div class=\"opts\">"
           "<button class=\"btn sm\" data-act=\"reset-layout\" title=\"Restore panel sizes, collapsed panels and the drawer\"><span>Reset panel layout</span></button>"
           "<button class=\"btn sm\" data-act=\"reset-sections\" title=\"Open every inspector section that you folded\"><span>Reset sections</span></button></div>";
    for (const auto& [name, panels] : ctl_.settings().layouts()) {
        out += std::format("<div class=\"pr\"><span class=\"lbl\">{}</span><button class=\"btn sm\" data-act=\"layout-load\" data-name=\"{}\" title=\"Apply this layout\"><span>Load</span></button>"
                           "<button class=\"btn sm\" data-act=\"layout-del\" data-name=\"{}\" title=\"Delete this layout\"><span>Delete</span></button></div>", rml_escape(name), rml_escape(name), rml_escape(name));
    }
    out += std::format("<div class=\"opts\"><input id=\"layout-name\" type=\"text\" value=\"{}\" placeholder=\"Layout name\"/><button class=\"btn sm\" data-act=\"layout-save\" title=\"Save the current panel sizes and tabs under this name\"><span>Save layout</span></button></div>", rml_escape(layout_name_));
    out += "<div class=\"section\"><span>Shortcuts</span></div>";
    for (const char* id : {"play.play", "play.stop", "view.frame", "view.left", "view.right", "view.drawer", "help.palette"}) {
        if (const Action* a = ctl_.actions().find(id)) out += std::format("<div class=\"pr\"><span>{}</span><span class=\"src\">{}</span></div>", rml_escape(a->title), rml_escape(a->shortcut));
    }
    e->SetInnerRML(out);
}

void Shell::build_search(const std::string& query) {
    search_query_ = query;
    Rml::Element* e = by_id("search-results");
    if (e == nullptr) return;
    std::string out;
    if (query.empty()) {
        e->SetInnerRML("<div class=\"hint\">Type to find nodes in this scene and commands.</div>");
        return;
    }
    const auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return s; };
    const std::string q = lower(query);
    std::string nodes;
    int shown = 0;
    const auto walk = [&](auto&& self, NodeId id) -> void {
        const ModelNode* n = ctl_.doc().model().get(id);
        if (n == nullptr || shown >= 40) return;
        if (lower(n->name).find(q) != std::string::npos || lower(n->type).find(q) != std::string::npos) {
            const std::string_view icon = icon_for_class(&ctl_.env().classes(), n->type);
            nodes += std::format("<div class=\"pr\" data-id=\"{}\">{}<span>{}</span><span class=\"src\">{}</span></div>", id, icon_img(icon.empty() ? "node" : icon, 14), rml_escape(n->name), rml_escape(n->type));
            ++shown;
        }
        for (const NodeId c : n->children) self(self, c);
    };
    walk(walk, ctl_.doc().model().root());
    if (!nodes.empty()) out += "<div class=\"section\"><span>Nodes</span></div>" + nodes;
    std::string cmds;
    int nc = 0;
    for (const ActionRegistry::Hit& h : ctl_.actions().search(query)) {
        if (nc++ >= 12) break;
        cmds += std::format("<div class=\"pr{}\" data-action=\"{}\"><span class=\"lbl\">{}</span><span class=\"src\">{}</span></div>", h.enabled ? "" : " off", rml_escape(h.action->id), rml_escape(ActionRegistry::label(*h.action)), rml_escape(h.action->shortcut));
    }
    std::string files;
    int nf = 0;
    const auto walk_files = [&](auto&& self, const AssetEntry& dir) -> void {
        for (const AssetEntry& c : dir.children) {
            if (nf >= 12) return;
            if (c.kind == AssetKind::Folder) { self(self, c); continue; }
            if (lower(c.path).find(q) == std::string::npos) continue;
            files += std::format("<div class=\"pr\" data-sasset=\"{}\"><span class=\"lbl\">{}</span><span class=\"src\">{}</span></div>", rml_escape(c.path), rml_escape(c.name), std::string(asset_kind_name(c.kind)));
            ++nf;
        }
    };
    walk_files(walk_files, ctl_.assets().root());
    if (!files.empty()) out += "<div class=\"section\"><span>Assets</span></div>" + files;
    if (!cmds.empty()) out += "<div class=\"section\"><span>Commands</span></div>" + cmds;
    if (out.empty()) out = "<div class=\"hint\">Nothing matches.</div>";
    e->SetInnerRML(out);
}

void Shell::build_drawer() {
    if (layout_.drawer == "console") build_console();
    else if (layout_.drawer == "history") build_history();
    else if (layout_.drawer == "profiler") { prof_built_ = false; build_profiler(); }
    else if (layout_.drawer == "replay") { rp_dirty_ = true; build_replay(); }
    else if (layout_.drawer == "tiers") build_tiers();
    else if (layout_.drawer == "input") build_inputmap();
    else if (layout_.drawer == "diag") script_diagnostics();
    else if (layout_.drawer == "refs") script_diagnostics(true);
    else build_node_drawer();
}

void Shell::set_drawer_head(const std::string& html) {
    Rml::Element* h = by_id("drawer-head");
    if (h != nullptr && h->GetInnerRML() != html) h->SetInnerRML(html);
}

void Shell::build_console() {
    Rml::Element* e = by_id("drawer-body");
    if (e == nullptr) return;
    console_dirty_ = false;
    const ConsoleLog& log = ctl_.console();
    const std::size_t n_err = log.count(log::Level::Error);
    const std::size_t n_warn = log.count(log::Level::Warn);
    const std::size_t n_info = lines_.size() > n_err + n_warn ? lines_.size() - n_err - n_warn : 0;
    const auto chip = [&](int bit, const char* label, std::size_t n, const char* cls) {
        return std::format("<div class=\"chip {}{}\" data-cf=\"{}\" title=\"Show or hide {} lines\"><span>{}</span><span class=\"n\">{}</span></div>", cls, (console_mask_ & bit) != 0 ? " on" : "", bit, label, label, n);
    };
    std::string head = "<span class=\"dh-title\">Console</span>" + chip(1, "Info", n_info, "info") + chip(2, "Warn", n_warn, "warn") + chip(4, "Error", n_err, "err");
    head += "<div class=\"grow\"></div>";
    head += std::format("<button class=\"btn sm{}\" data-act=\"con-copy\" title=\"{}\"><span>{}</span></button>", lines_.empty() ? " off" : "",
                        console_sel_ >= 0 ? "Copy the selected line" : "Copy all shown lines", console_sel_ >= 0 ? "Copy line" : "Copy all");
    head += std::format("<button class=\"btn sm{}\" data-act=\"con-clear\" title=\"Remove all log lines\"><span>Clear</span></button>", lines_.empty() ? " off" : "");
    set_drawer_head(head);

    std::string out;
    std::size_t shown = 0;
    const std::size_t first = lines_.size() > 600 ? lines_.size() - 600 : 0;
    for (std::size_t i = first; i < lines_.size(); ++i) {
        const int bit = console_filter_bit(lines_[i].level);
        if (!console_filter_shows(console_mask_, lines_[i].level)) continue;
        ++shown;
        const char* cls = bit == 4 ? "err" : bit == 2 ? "warn" : "info";
        const char* tag = bit == 4 ? "error " : bit == 2 ? "warn  " : "";
        out += std::format("<div class=\"line {}{}\" data-ci=\"{}\"><span class=\"src\">{}</span>{}{}</div>", cls, static_cast<int>(i) == console_sel_ ? " sel" : "", i, rml_escape(lines_[i].source), tag, rml_escape(lines_[i].text));
    }
    if (lines_.empty()) out = "<div class=\"empty\">No log lines. Output from the editor and the running game appears here.</div>";
    else if (shown == 0) out = "<div class=\"empty\">Every severity is hidden. Turn a filter back on above.</div>";
    const bool follow = e->GetScrollTop() + e->GetClientHeight() >= e->GetScrollHeight() - 24.0f;
    e->SetInnerRML(out);
    e->SetClass("mono", true);
    if (follow) e->SetScrollTop(e->GetScrollHeight());
}

void Shell::console_copy() {
    std::string text;
    if (console_sel_ >= 0 && static_cast<std::size_t>(console_sel_) < lines_.size()) {
        const ConsoleLine& l = lines_[static_cast<std::size_t>(console_sel_)];
        text = l.source + " " + l.text;
    } else {
        for (const ConsoleLine& l : lines_) {
            if (console_filter_shows(console_mask_, l.level)) text += l.source + " " + l.text + "\n";
        }
    }
    if (text.empty()) return;
    SDL_SetClipboardText(text.c_str());
    UserMessage m;
    m.severity = Severity::Success;
    m.what = console_sel_ >= 0 ? "Copied the log line." : "Copied the shown log lines.";
    ctl_.messages().push(std::move(m), ctl_.clock());
}

void Shell::build_history() {
    Rml::Element* e = by_id("drawer-body");
    if (e == nullptr) return;
    const CommandStack& st = ctl_.doc().stack();
    history_rev_ = st.revision() + st.index();
    set_drawer_head(std::format("<span class=\"dh-title\">History</span><span class=\"dh-note\">Step {} of {}. Click a step to return to it.</span>", st.index(), st.size()));
    std::string out = std::format("<div class=\"pr{}\" data-hist=\"0\">{}<span>Scene opened</span></div>", st.index() == 0 ? " sel" : "", icon_img("project", 14));
    for (std::size_t i = 0; i < st.size(); ++i) {
        const bool applied = i < st.index();
        out += std::format("<div class=\"pr{}{}\" data-hist=\"{}\">{}<span>{}</span></div>", i + 1 == st.index() ? " sel" : "", applied ? "" : " off", i + 1, icon_img("history", 14), rml_escape(st.label_at(i)));
    }
    e->SetInnerRML(out);
    e->SetClass("mono", false);
}

// ------------------------------------------------------------------ popups and dialogs

void Shell::close_popup() {
    close_picker();
    cp_open_ = false;
    cp_drag_ = 0;
    popup_open_ = false;
    palette_ = false;
    if (Rml::Element* p = by_id("popup")) p->SetProperty("display", "none");
}

void Shell::open_menu(const std::string& name, Rml::Element* anchor) {
    Rml::Element* p = by_id("popup");
    if (p == nullptr) return;
    for (const MenuSpec& m : menus()) {
        if (name != m.name) continue;
        std::string out;
        for (const char* id : m.ids) {
            if (std::string_view(id) == "-") { out += "<div class=\"msep\"></div>"; continue; }
            const Action* a = ctl_.actions().find(id);
            if (a == nullptr) continue;
            const bool is_view = std::string_view(id).starts_with("view.");
            const std::string check = is_view ? (menu_checked(id) ? icon_img("check", 12) : std::string("<span class=\"chk\"></span>")) : std::string();
            out += std::format("<div class=\"mi{}\" data-action=\"{}\">{}<span class=\"lbl\">{}</span><span class=\"kbd\">{}</span></div>", ctl_.actions().enabled(*a) ? "" : " off", id, check, rml_escape(a->title), rml_escape(a->shortcut));
        }
        p->SetInnerRML(out);
        p->SetProperty("left", std::format("{}px", static_cast<int>(anchor->GetAbsoluteLeft())));
        p->SetProperty("top", std::format("{}px", static_cast<int>(anchor->GetAbsoluteTop() + anchor->GetOffsetHeight())));
        p->SetProperty("display", "block");
        popup_open_ = true;
        palette_ = false;
    }
}

void Shell::open_palette(const std::string& query) {
    Rml::Element* p = by_id("popup");
    if (p == nullptr) return;
    palette_ = true;
    popup_open_ = true;
    const float ratio = ctx_->GetDensityIndependentPixelRatio();
    const int w = static_cast<int>(ctx_->GetDimensions().x);
    const int pw = static_cast<int>(480 * ratio);
    p->SetInnerRML(std::format("<div class=\"pad\"><input id=\"palette-field\" type=\"text\" value=\"{}\" placeholder=\"Search commands\"/></div><div id=\"palette-list\"></div>", rml_escape(query)));
    p->SetProperty("left", std::format("{}px", (w - pw) / 2));
    p->SetProperty("top", std::format("{}px", static_cast<int>(60 * ratio)));
    p->SetProperty("width", "480dp");
    p->SetProperty("display", "block");
    build_palette(query);
    if (Rml::Element* f = by_id("palette-field")) f->Focus();
}

void Shell::build_palette(const std::string& query) {
    Rml::Element* l = by_id("palette-list");
    if (l == nullptr) return;
    palette_ids_.clear();
    std::string out;
    int n = 0;
    for (const ActionRegistry::Hit& h : ctl_.actions().search(query)) {
        if (n >= 12) break;
        palette_ids_.push_back(h.action->id);
        out += std::format("<div class=\"pr{}{}\" data-action=\"{}\"><span>{}</span><span class=\"src\">{}</span></div>", n == 0 ? " sel" : "", h.enabled ? "" : " off", rml_escape(h.action->id), rml_escape(ActionRegistry::label(*h.action)), rml_escape(h.action->shortcut));
        ++n;
    }
    if (out.empty()) out = "<div class=\"hint\">No command matches.</div>";
    l->SetInnerRML(out);
}

void Shell::open_dialog(Dialog d) {
    Rml::Element* modal = by_id("modal");
    Rml::Element* dlg = by_id("dialog");
    if (modal == nullptr || dlg == nullptr) return;
    close_popup();
    dialog_ = d;
    std::string out;
    const auto buttons = [d](const char* ok, const char* cancel = "Cancel") {
        const char* browse = d == Dialog::SaveAs || d == Dialog::AssetImport ? "<button class=\"btn\" data-dlg=\"browse\" title=\"Choose with the system file dialog\"><span>Browse...</span></button><div class=\"grow\"></div>" : "";
        return std::format("<div class=\"dbtns\">{}<button class=\"btn\" data-dlg=\"cancel\"><span>{}</span></button><button class=\"btn primary\" data-dlg=\"ok\"><span>{}</span></button></div>", browse, cancel, ok);
    };
    switch (d) {
    case Dialog::SaveAs:
        dialog_text_ = ctl_.doc().has_path() ? ctl_.env().relative(ctl_.doc().path()) : std::string("scenes/new.scene");
        out = std::format("<div class=\"dt\">Save scene as</div><div class=\"dm\">Project-relative path.</div><input id=\"dlg-field\" type=\"text\" value=\"{}\"/>{}", rml_escape(dialog_text_), buttons("Save"));
        break;
    case Dialog::DiscardNew:
    case Dialog::DiscardQuit:
    case Dialog::DiscardOpen:
    case Dialog::DiscardLeave:
        out = std::format("<div class=\"dt\">Discard unsaved changes?</div><div class=\"dm\">Changes to {} since the last save are lost.</div>{}", rml_escape(ctl_.env().relative(ctl_.doc().path())),
                          buttons(d == Dialog::DiscardQuit ? "Quit and discard" : "Discard changes", "Keep editing"));
        break;
    case Dialog::OpenScene: {
        out = "<div class=\"dt\">Open scene</div>";
        const auto scenes = ctl_.assets().files_of(AssetKind::Scene);
        for (const std::string& s : scenes) out += std::format("<div class=\"pr\" data-open=\"{}\">{}<span>{}</span></div>", rml_escape(s), icon_img("layout", 14), rml_escape(s));
        if (scenes.empty()) out += "<div class=\"hint\">This project has no scenes.</div>";
        out += "<div class=\"dbtns\"><button class=\"btn\" data-dlg=\"browse\" title=\"Choose a scene with the system file dialog\"><span>Browse...</span></button><div class=\"grow\"></div><button class=\"btn\" data-dlg=\"cancel\"><span>Close</span></button></div>";
        break;
    }
    case Dialog::AssetImport:
    case Dialog::AssetFolder:
    case Dialog::AssetRename:
    case Dialog::AssetDelete: out = asset_dialog_html(d); break;
    case Dialog::NewProject:
    case Dialog::License: out = entry_dialog_html(d); break;
    case Dialog::AttachScript: build_attach(); break;
    case Dialog::OpenProject: build_open_project(); break;
    case Dialog::None: break;
    }
    if (!out.empty()) dlg->SetInnerRML(out);
    dlg->SetProperty("width", d == Dialog::License ? "760dp" : "420dp");
    modal->SetClass("open", true);
    if (Rml::Element* f = by_id("dlg-field")) f->Focus();
}

void Shell::close_dialog() {
    if (dialog_ == Dialog::DiscardOpen || dialog_ == Dialog::DiscardLeave) ctl_.cancel_discard();
    dialog_ = Dialog::None;
    if (Rml::Element* m = by_id("modal")) m->SetClass("open", false);
}

void Shell::dialog_confirm() {
    const Dialog d = dialog_;
    switch (d) {
    case Dialog::SaveAs: {
        std::string error;
        const std::filesystem::path path = ctl_.env().resolve(dialog_text_);
        if (dialog_text_.empty()) return;
        if (ctl_.doc().save_as(path, error)) {
            LOG_INFO("Saved '{}'", ctl_.env().relative(path));
            ctl_.assets().poll();
            ctl_.notify_ok(std::format("Saved {}.", ctl_.env().relative(path)));
        } else {
            ctl_.report_failure(error);
        }
        break;
    }
    case Dialog::DiscardNew:
        ctl_.doc().new_scene();
        if (host_.frame_scene) host_.frame_scene();
        break;
    case Dialog::DiscardQuit:
        if (host_.quit) host_.quit();
        break;
    case Dialog::DiscardOpen:
    case Dialog::DiscardLeave:
        dialog_ = Dialog::None;
        ctl_.confirm_discard();
        break;
    case Dialog::AssetImport:
    case Dialog::AssetFolder:
    case Dialog::AssetRename:
    case Dialog::AssetDelete:
        if (!asset_dialog_confirm(d)) return; // stays open on an error
        break;
    case Dialog::AttachScript: attach_confirm(); return; // stays open on an error
    case Dialog::OpenProject: open_project_confirm(project_text_); return;
    case Dialog::NewProject: new_project_confirm(); return; // stays open on an error
    default: break;
    }
    close_dialog();
}

// ------------------------------------------------------------------ events


void Shell::click(Rml::Element* t) {
    if (inspector_click(t)) return;
    if (entry_click(t)) return;
    if (profiler_click(t) || replay_click(t) || tier_click(t) || inputmap_click(t)) return;
    if (Rml::Element* e = find_attr(t, "data-clear")) {
        if (e->GetAttribute<Rml::String>("data-clear", "") == "insp-filter") {
            if (Rml::Element* f = by_id("insp-filter")) f->SetAttribute("value", "");
            filter_.clear();
            e->SetClass("hidden", true);
            revision_ = ~0ull;
        }
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-star")) {
        CatalogUser user = CatalogUser::parse(layout_.add_recents, layout_.add_favorites);
        user.toggle_favorite(e->GetAttribute<Rml::String>("data-star", ""));
        layout_.add_favorites = user.favorites_text();
        save_layout();
        build_picker();
        if (Rml::Element* f = by_id("pick-field")) f->Focus();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-pick")) { pick_class(e->GetAttribute<Rml::String>("data-pick", "")); return; }
    if (Rml::Element* e = find_attr(t, "data-attach-tab")) {
        attach_tab_ = e->GetAttribute<int>("data-attach-tab", 0);
        attach_error_.clear();
        build_attach();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-attach")) {
        attach_sel_ = e->GetAttribute<Rml::String>("data-attach", "");
        attach_error_.clear();
        build_attach_list();
        if (Rml::Element* er = by_id("at-error")) er->SetInnerRML("");
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-project")) {
        if (!e->IsClassSet("off")) open_project_confirm(e->GetAttribute<Rml::String>("data-project", ""));
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-snode")) {
        const NodeId id = static_cast<NodeId>(e->GetAttribute<int>("data-snode", 0));
        tree_.reveal(ctl_.doc().model(), id);
        tree_select(id, false, false);
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-sasset")) {
        const std::string path = e->GetAttribute<Rml::String>("data-sasset", "");
        assets_sel_ = path;
        for (std::size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) assets_open_[path.substr(0, slash)] = true;
        show_rail("assets", false);
        assets_filter_.clear();
        if (Rml::Element* f = by_id("asset-filter")) f->SetAttribute("value", "");
        build_assets();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-actx")) {
        const std::string key = e->GetAttribute<Rml::String>("data-actx", "");
        close_popup();
        run_asset_menu(key);
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-ctx")) {
        const std::string key = e->GetAttribute<Rml::String>("data-ctx", "");
        if (e->IsClassSet("off")) return;
        close_popup();
        run_tree_menu(key);
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-action")) {
        const std::string id = e->GetAttribute<Rml::String>("data-action", "");
        const Action* a = ctl_.actions().find(id);
        if (a != nullptr && !ctl_.actions().enabled(*a)) return;
        close_popup();
        ctl_.run_action(id);
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-tool")) {
        const std::string tool = e->GetAttribute<Rml::String>("data-tool", "");
        static const std::pair<const char*, Tool> tools[] = {{"select", Tool::Select}, {"move", Tool::Move}, {"rotate", Tool::Rotate}, {"scale", Tool::Scale}, {"paint", Tool::Paint}};
        for (const auto& [n, k] : tools) if (tool == n) ctl_.set_tool(k);
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-rail")) {
        show_rail(e->GetAttribute<Rml::String>("data-rail", ""), true);
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-center")) {
        layout_.center = e->GetAttribute<Rml::String>("data-center", "scene");
        apply_layout();
        save_layout();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-drawer")) { set_drawer(e->GetAttribute<Rml::String>("data-drawer", "console")); return; }
    if (drawer_click(t)) return;
    if (Rml::Element* e = find_attr(t, "data-cf")) {
        console_mask_ ^= e->GetAttribute<int>("data-cf", 0);
        build_console();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-ci")) {
        const int i = e->GetAttribute<int>("data-ci", -1);
        console_sel_ = console_sel_ == i ? -1 : i;
        build_console();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-hist")) {
        ctl_.report(ctl_.doc().jump_to_history(static_cast<std::size_t>(e->GetAttribute<int>("data-hist", 0))));
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-open")) {
        const std::string path = e->GetAttribute<Rml::String>("data-open", "");
        close_dialog();
        ctl_.request_open_scene(path);
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-theme")) {
        const std::string th = e->GetAttribute<Rml::String>("data-theme", "dark");
        layout_.theme = th == "system" ? std::string() : th;
        if (host_.set_theme) host_.set_theme(th == "light" ? brand::Theme::Light : th == "dark" ? brand::Theme::Dark : (host_.system_theme ? host_.system_theme() : brand::Theme::Dark));
        build_settings();
        save_layout();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-scale")) {
        layout_.ui_scale = e->GetAttribute<float>("data-scale", 1.0f);
        if (host_.set_scale) host_.set_scale(layout_.ui_scale);
        build_settings();
        save_layout();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-dlg")) {
        const std::string which = e->GetAttribute<Rml::String>("data-dlg", "");
        if (which == "browse") {
            native_pick(dialog_ == Dialog::SaveAs ? PickKind::SaveScene : dialog_ == Dialog::AssetImport ? PickKind::ImportFile : dialog_ == Dialog::OpenProject ? PickKind::ProjectFolder : PickKind::OpenScene);
            return;
        }
        if (which == "ok") dialog_confirm();
        else close_dialog();
        return;
    }
    if (Rml::Element* e = find_attr(t, "data-act")) {
        const std::string act = e->GetAttribute<Rml::String>("data-act", "");
        if (act == "palette") open_palette({});
        else if (act == "add-child") ctl_.run_action("edit.add_child");
        else if (act == "pick-clear") {
            picker_query_.clear();
            if (Rml::Element* f = by_id("pick-field")) { f->SetAttribute("value", ""); f->Focus(); }
            build_picker();
        }
        else if (act == "asset-import") open_dialog(Dialog::AssetImport);
        else if (act == "asset-folder") { asset_op_path_ = asset_selected_folder(); open_dialog(Dialog::AssetFolder); }
        else if (act == "asset-refresh") { ctl_.assets().poll(); build_assets(); }
        else if (act == "scale-up" || act == "scale-down") {
            const float cur = layout_.ui_scale > 0.0f ? layout_.ui_scale : 1.0f;
            layout_.ui_scale = std::clamp(std::round((cur + (act == "scale-up" ? 0.05f : -0.05f)) * 100.0f) / 100.0f, 0.5f, 3.0f);
            if (host_.set_scale) host_.set_scale(layout_.ui_scale);
            build_settings();
            save_layout();
        }
        else if (act == "reset-layout") reset_panel_layout();
        else if (act == "layout-save") save_named_layout(layout_name_);
        else if (act == "layout-load") load_named_layout(e->GetAttribute<Rml::String>("data-name", ""));
        else if (act == "layout-del") {
            ctl_.settings().delete_layout(e->GetAttribute<Rml::String>("data-name", ""));
            save_layout();
            register_actions();
            build_settings();
        }
        else if (act == "reset-sections") { layout_.closed_sections.clear(); revision_ = ~0ull; save_layout(); ctl_.notify_ok("Inspector sections reset."); }
        else if (act == "con-copy") console_copy();
        else if (act == "con-clear") { ctl_.console().clear(); console_sel_ = -1; }
        else if (act == "open-right") { layout_.right_collapsed = false; apply_layout(); save_layout(); }
        else if (act == "collapse-right") { layout_.right_collapsed = true; apply_layout(); save_layout(); }
        else if (act == "open-script") open_script(ctl_.env().resolve(e->GetAttribute<Rml::String>("data-file", "")), e->GetAttribute<int>("data-line", 0));
    }
}

void Shell::ProcessEvent(Rml::Event& e) {
    const Rml::EventId id = e.GetId();
    Rml::Element* t = e.GetTargetElement();
    const Rml::Vector2f at{e.GetParameter("mouse_x", 0.0f), e.GetParameter("mouse_y", 0.0f)};
    if (id == Rml::EventId::Mousedown) {
        hide_tip();
        if (assets_mousedown(t, at, static_cast<int>(e.GetParameter("button", 0)))) return;
        if (sprite_mousedown(t, at, static_cast<int>(e.GetParameter("button", 0)))) return;
        if (timeline_mousedown(t, at, static_cast<int>(e.GetParameter("button", 0)))) return;
        if (replay_mousedown(t, at)) return;
        if (inspector_mousedown(t, at)) return;
        if (popup_open_ && !inside(t, "popup") && find_attr(t, "data-menu") == nullptr) close_popup();
        if (t != nullptr && t->IsClassSet("split")) { begin_drag(t, at); return; }
        if (dialog_ == Dialog::None) {
            tree_focus_ = inside(t, "tree");
            if (rename_ != 0 && !inside(t, "rename-field")) end_rename(true);
            if (Rml::Element* chev = find_attr(t, "data-chev")) {
                if (e.GetParameter("button", 0) == 0) {
                    tree_.toggle(static_cast<NodeId>(chev->GetAttribute<int>("data-chev", 0)));
                    persist_tree();
                    revision_ = ~0ull;
                    return;
                }
            }
            if (Rml::Element* row = find_attr(t, "data-id"); row != nullptr && inside(t, "tree") && rename_ == 0) {
                const NodeId nid = static_cast<NodeId>(std::atoi(row->GetAttribute<Rml::String>("data-id", "0").c_str()));
                const Uint32 mod = SDL_GetModState();
                if (e.GetParameter("button", 0) == 1) {
                    open_tree_menu(nid, at);
                    return;
                }
                // Rows are rebuilt on selection change, so RmlUi's own dblclick never sees the same element twice.
                const std::uint32_t now = SDL_GetTicks();
                const bool twice = nid == last_click_node_ && now - last_click_ms_ < 400 && (mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) == 0;
                last_click_node_ = nid;
                last_click_ms_ = now;
                if (twice) {
                    last_click_node_ = 0;
                    begin_rename(nid);
                    return;
                }
                tree_select(nid, (mod & SDL_KMOD_CTRL) != 0, (mod & SDL_KMOD_SHIFT) != 0);
                if (nid != ctl_.doc().model().root()) {
                    press_node_ = nid;
                    press_at_ = at;
                }
            }
        }
        if (Rml::Element* m = find_attr(t, "data-menu")) {
            if (popup_open_ && !palette_) close_popup();
            else open_menu(m->GetAttribute<Rml::String>("data-menu", ""), m);
        }
    } else if (id == Rml::EventId::Mouseover) {
        tip_over(t);
        if (popup_open_ && !palette_) if (Rml::Element* m = find_attr(t, "data-menu")) open_menu(m->GetAttribute<Rml::String>("data-menu", ""), m);
    } else if (id == Rml::EventId::Mousemove) {
        drag_to(at);
        assets_mousemove(t, at);
        sprite_mousemove(at);
        timeline_mousemove(at);
        replay_mousemove(at);
        inspector_mousemove(at);
        if (press_node_ != 0) {
            const float dx = at.x - press_at_.x, dy = at.y - press_at_.y;
            if (!tree_dragging_ && dx * dx + dy * dy > 25.0f * ctx_->GetDensityIndependentPixelRatio()) {
                tree_dragging_ = true;
                hide_tip();
                const ModelNode* n = ctl_.doc().model().get(press_node_);
                if (Rml::Element* g = by_id("ghost")) {
                    g->SetInnerRML(std::format("{}<span>{}</span>", icon_img(icon_for_class(&ctl_.env().classes(), n != nullptr ? n->type : ""), 14), rml_escape(n != nullptr ? n->name : "")));
                    g->SetProperty("display", "flex");
                }
            }
            if (tree_dragging_) tree_drag_update(t, at);
        }
    } else if (id == Rml::EventId::Mouseup) {
        end_drag();
        assets_mouseup(t, at);
        sprite_mouseup();
        timeline_mouseup();
        replay_mouseup();
        inspector_mouseup();
        if (tree_dragging_) tree_drag_end(true);
        press_node_ = 0;
    } else if (id == Rml::EventId::Mousescroll) {
        sprite_scroll(t, at, e.GetParameter("wheel_delta_y", 0.0f));
    } else if (id == Rml::EventId::Click) {
        click(t);
    } else if (id == Rml::EventId::Change) {
        const std::string tid = t != nullptr ? t->GetId() : std::string();
        const std::string value = e.GetParameter<Rml::String>("value", "");
        const bool enter = e.GetParameter("linebreak", false);
        if (sprite_change(t, value, enter) || timeline_change(t, value, enter) || inputmap_change(t, value, enter) || entry_change(t, value, enter)) {
        } else if (inspector_change(t, value, enter)) {
        } else if (tid == "rename-field") {
            if (enter) end_rename(true);
        } else if (tid == "name-field" && enter) {
            ctl_.report(ctl_.doc().rename(selected_, value));
            revision_ = ~0ull;
        } else if (tid == "asset-filter") {
            assets_filter(value);
        } else if (tid == "search-field") {
            build_search(value);
            if (enter && !value.empty()) {
                for (const ActionRegistry::Hit& h : ctl_.actions().search(value)) { if (h.enabled) { ctl_.run_action(h.action->id); break; } }
            }
        } else if (tid == "palette-field") {
            if (enter) {
                const std::string first = palette_ids_.empty() ? std::string() : palette_ids_.front();
                close_popup();
                if (!first.empty()) ctl_.run_action(first);
            } else {
                build_palette(value);
            }
        } else if (tid == "pick-field") {
            if (enter) { picker_key(Rml::Input::KI_RETURN, false); return; }
            if (value != picker_query_) { picker_query_ = value; picker_sel_ = 0; build_picker(); }
        } else if (tid == "at-filter") {
            attach_filter_ = value;
            attach_error_.clear();
            build_attach_list();
            if (enter) attach_confirm();
        } else if (tid == "layout-name") {
            layout_name_ = value;
            if (enter) save_named_layout(layout_name_);
        } else if (tid == "at-class") {
            new_class_ = value;
        } else if (tid == "at-path") {
            new_path_ = value;
            if (enter) attach_confirm();
        } else if (tid == "proj-field") {
            project_text_ = value;
            if (enter) open_project_confirm(project_text_);
        } else if (tid == "dlg-field") {
            dialog_text_ = value;
            if (enter) dialog_confirm();
        }
    } else if (id == Rml::EventId::Keydown) {
        const auto key = static_cast<Rml::Input::KeyIdentifier>(e.GetParameter("key_identifier", 0));
        if (picker_ && picker_key(key, e.GetParameter("ctrl_key", false))) {
            e.StopPropagation();
        } else if (dialog_ == Dialog::AttachScript && attach_tab_ == 0 && (key == Rml::Input::KI_DOWN || key == Rml::Input::KI_UP) && !attach_shown_.empty()) {
            std::size_t cur = 0;
            for (std::size_t i = 0; i < attach_shown_.size(); ++i) if (attach_shown_[i].first == attach_sel_) cur = i;
            const std::size_t n = attach_shown_.size();
            attach_sel_ = attach_shown_[key == Rml::Input::KI_DOWN ? (cur + 1) % n : (cur + n - 1) % n].first;
            build_attach_list();
        } else if (key == Rml::Input::KI_ESCAPE && !edit_key_.empty()) {
            cancel_edits();
        } else if (key == Rml::Input::KI_ESCAPE && tree_dragging_) {
            tree_drag_end(false);
        } else if (key == Rml::Input::KI_ESCAPE && rename_ != 0) {
            end_rename(false);
        } else if (tid_is(t, "rename-field") && key == Rml::Input::KI_RETURN) {
            end_rename(true);
        } else if (tree_focus_ && rename_ == 0 && dialog_ == Dialog::None && !popup_open_ && !typing(ctx_)) {
            tree_key(key);
        } else if (key == Rml::Input::KI_ESCAPE) {
            if (dialog_ != Dialog::None) close_dialog();
            else if (popup_open_) close_popup();
        }
    }
}

bool Shell::handle_shortcut(const SDL_Event& ev) {
    const std::string s = shortcut_string(ev);
    if (!im_capture_.empty()) return true; // "Bind" is waiting for a key
    if (s.empty() || dialog_ != Dialog::None) return false;
    const bool fkey = s.find('F') != std::string::npos && std::isdigit(static_cast<unsigned char>(s.back())) != 0;
    if (s.find("Ctrl+") == std::string::npos && !fkey && s != "Del") return false;
    for (const Action& a : ctl_.actions().all()) {
        if (a.shortcut == s) {
            if (ctl_.actions().enabled(a)) ctl_.run_action(a.id);
            return true;
        }
    }
    return false;
}

bool Shell::handle_escape() {
    if (dialog_ != Dialog::None) { close_dialog(); return true; }
    if (popup_open_) { close_popup(); return true; }
    return false;
}

bool Shell::handle_tool_key(const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || (ev.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) != 0) return false;
    switch (ev.key.key) {
    case SDLK_Q: ctl_.set_tool(Tool::Select); return true;
    case SDLK_W: ctl_.set_tool(Tool::Move); return true;
    case SDLK_E: ctl_.set_tool(Tool::Rotate); return true;
    case SDLK_R: ctl_.set_tool(Tool::Scale); return true;
    case SDLK_B: ctl_.set_tool(Tool::Paint); return true;
    default: return false;
    }
}

} // namespace cuervo::editor

// The editor window's chrome (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): menu bar,
// toolbar, command palette, message banners, the dock layout with draggable
// splitters and saved layouts, shortcuts, and the action registry the menus,
// toolbar and palette all run. Split from app.cpp (the frame loop and rendering).

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "editor/app.hpp"
#include "editor/icons.hpp"
#include "editor/theme.hpp"
#include "editor/ui.hpp"

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

// ------------------------------------------------------------ messages

void EditorApp::notify(UserMessage message) { messages_.push(std::move(message), clock_); }

void EditorApp::notify_ok(std::string text) {
    UserMessage m;
    m.severity = Severity::Success;
    m.what = std::move(text);
    notify(std::move(m));
}

void EditorApp::report_failure(const std::string& raw) {
    LOG_WARN("{}", raw);
    notify(message_edit_rejected(raw));
}

bool EditorApp::run_action(std::string_view id) {
    const Action* a = actions_.find(id);
    if (a == nullptr || !actions_.enabled(*a)) return false;
    doc_.seal();
    const bool ok = actions_.run(id);
    doc_.seal();
    if (!ok && !doc_.error().empty()) report_failure(doc_.error());
    return ok;
}

void EditorApp::open_palette(std::string query) {
    palette_query_ = std::move(query);
    palette_request_ = true;
}

void EditorApp::request_switch_project(std::string path) {
    if (doc_.dirty()) {
        pending_open_ = std::move(path);
        discard_switch_ = true;
        dialog_ = Dialog::DiscardChanges;
        dialog_opened_ = false;
        return;
    }
    switch_target_ = std::move(path);
    switch_requested_ = true;
    running_ = false;
}

void EditorApp::save_current_scene() {
    if (!doc_.has_path()) {
        dialog_ = Dialog::SaveAs;
        dialog_opened_ = false;
        return;
    }
    std::string error;
    const std::string rel = env_.relative(doc_.path());
    if (doc_.save(error)) {
        LOG_INFO("Saved '{}'", rel);
        assets_.poll();
        notify_ok(std::format("Saved {}.", rel));
        settings_.set_last_scene(env_.project_dir(), rel);
        save_user_state();
    } else {
        LOG_ERROR("{}", error);
        notify(message_save_failed(rel, error));
    }
}

// ------------------------------------------------------------ layouts and user state

Layout EditorApp::current_layout() const {
    Layout l = layout_;
    l.hidden.clear();
    if (imgui_ready_ && dock_built_) l.dock = ImGui::SaveIniSettingsToMemory();
    for (const auto& p : panels_.all()) {
        if (!p->visible) l.hidden.emplace_back(p->title());
    }
    return l;
}

void EditorApp::apply_layout(const std::string& name) {
    if (!layouts_.contains(name)) {
        UserMessage m;
        m.severity = Severity::Warning;
        m.what = std::format("There is no layout named '{}'.", name);
        m.safety = "Your panels are unchanged.";
        m.action = "Pick one from View > Layout.";
        notify(std::move(m));
        return;
    }
    layout_ = layouts_.layout(name);
    layouts_.set_active(name);
    for (const auto& p : panels_.all()) p->visible = !layout_.is_hidden(p->title());
    // Saved layouts carry ImGui's dock text; the built-in ones are built from the fractions.
    if (layout_.dock.empty()) rebuild_dock_ = true;
    else pending_dock_ = layout_.dock;
}

void EditorApp::save_layout_as(const std::string& name) {
    std::string error;
    if (!layouts_.save_as(name, current_layout(), error)) {
        UserMessage m;
        m.severity = Severity::Warning;
        m.what = "The layout was not saved.";
        m.safety = "Your panels are unchanged.";
        m.action = error;
        notify(std::move(m));
        return;
    }
    notify_ok(std::format("Layout '{}' saved.", name));
    rebuild_actions();
    save_user_state();
}

void EditorApp::restore_user_state() {
    settings_.load(EditorSettings::user_file());
    const float init_ui_scale = options_.ui_scale.value_or(settings_.ui_scale());
    const float init_font_scale = options_.font_scale.value_or(settings_.font_scale());
    settings_.set_ui_scale(init_ui_scale);
    settings_.set_font_scale(init_font_scale);
    theme::set_ui_scale(init_ui_scale);
    theme::set_font_scale(init_font_scale);

    std::string error;
    if (!layouts_.load(LayoutStore::user_file(), error)) {
        UserMessage m;
        m.severity = Severity::Warning;
        m.what = "Your saved layouts could not be read, so the default layout is used.";
        m.safety = "The file was not changed.";
        m.action = "Delete editor-layouts.txt in your Cuervo user folder to start fresh.";
        m.details = error;
        notify(std::move(m));
    }
    recents_.load(RecentProjects::user_file());
    layout_ = layouts_.current();
    for (const auto& p : panels_.all()) p->visible = !layout_.is_hidden(p->title());
    if (!layout_.dock.empty()) pending_dock_ = layout_.dock;
    if (!options_.layout.empty()) apply_layout(options_.layout);
}

void EditorApp::save_user_state() {
    // Screenshot and test runs don't touch the user's saved state.
    if (!options_.screenshot.empty() || !options_.script_commands.empty()) return;
    std::string error;
    layouts_.set_current(current_layout());
    if (!layouts_.save(LayoutStore::user_file(), error)) LOG_WARN("{}", error);
    if (env_.has_project()) recents_.add(env_.project_dir());
    if (!recents_.save(RecentProjects::user_file(), error)) LOG_WARN("{}", error);
    if (!settings_.save(EditorSettings::user_file(), error)) LOG_WARN("{}", error);
}

void EditorApp::zoom_in() {
    set_ui_scale(std::min(theme::ui_scale() + 0.10f, 3.0f));
}

void EditorApp::zoom_out() {
    set_ui_scale(std::max(theme::ui_scale() - 0.10f, 0.50f));
}

void EditorApp::reset_zoom() {
    set_ui_scale(1.0f);
    set_font_scale(1.0f);
}

void EditorApp::set_ui_scale(float scale) {
    theme::set_ui_scale(scale);
    settings_.set_ui_scale(scale);
    save_user_state();
}

void EditorApp::set_font_scale(float scale) {
    theme::set_font_scale(scale);
    settings_.set_font_scale(scale);
    save_user_state();
}

float EditorApp::ui_scale() const {
    return theme::ui_scale();
}

float EditorApp::font_scale() const {
    return theme::font_scale();
}

// ------------------------------------------------------------ actions

void EditorApp::rebuild_actions() {
    actions_.reset_actions();
    add_document_actions(actions_, doc_, [this](std::string_view cls) {
        std::vector<scene::Field> fields;
        const ClassRegistry& reg = env_.classes();
        // A positioned node appears in the middle of the scene view.
        if (reg.is_a(cls, "Node2D") && !reg.is_a(cls, "Control")) {
            fields.push_back({"position", "vec2",
                              scene::Value::vector({std::round(camera_.center.x()), std::round(camera_.center.y())})});
        }
        return fields;
    });
    add_app_actions();
}

void EditorApp::add_app_actions() {
    auto add = [&](std::string id, std::string title, std::string category, std::string shortcut,
                   std::function<bool()> enabled, std::function<bool()> run) {
        actions_.add({std::move(id), std::move(title), std::move(category), std::move(shortcut), std::move(enabled),
                      std::move(run)});
    };
    auto dialog = [this](Dialog d) {
        dialog_ = d;
        dialog_opened_ = false;
        return true;
    };
    const bool have_selection = true;
    (void)have_selection;
    auto idle = [this] { return !play_.active(); };
    // ---- file and project
    add("file.new", "New Scene...", "File", "Ctrl+N", {}, [=] { return dialog(Dialog::NewScene); });
    add("file.open", "Open Scene...", "File", "Ctrl+O", {}, [=] { return dialog(Dialog::Open); });
    add("file.save", "Save Scene", "File", "Ctrl+S", {}, [this] {
        save_current_scene();
        return true;
    });
    add("file.save_as", "Save Scene As...", "File", "Ctrl+Shift+S", {}, [=] { return dialog(Dialog::SaveAs); });
    add("project.switch", "Switch Project...", "Project", "", {}, [this] {
        request_switch_project();
        return true;
    });
    for (const RecentProject& r : recents_.list()) {
        if (!r.exists || (env_.has_project() && fs::equivalent(platform::path_from_utf8(r.path), env_.project_dir()))) continue;
        const std::string path = r.path;
        add("project.recent." + path, "Open Recent: " + r.name, "Project", "", {}, [this, path] {
            request_switch_project(path);
            return true;
        });
    }
    add("file.quit", "Quit", "File", "", {}, [this] {
        running_ = false;
        return true;
    });
    add("file.open_code_editor", "Open Project in Code Editor", "File", "", {}, [this] {
        open_in_external_editor(env_.project_dir());
        return true;
    });
    add("file.open_scripts_editor", "Open Scripts Folder in Code Editor", "File", "", {}, [this] {
        open_in_external_editor(env_.project_dir() / "scripts");
        return true;
    });
    // ---- play
    add("play.play", "Play", "Play", "F5", [this] { return !play_.active() || play_.state() == PlayState::Paused; },
        [this] {
            if (play_.active()) {
                play_.resume();
            } else {
                start_play(false);
            }
            return true;
        });
    add("play.scene", "Play Current Scene", "Play", "F6", idle, [this] {
        start_play(true);
        return true;
    });
    add("play.record", "Play and Record", "Play", "", idle, [this] {
        start_play(false, true);
        return true;
    });
    add("play.pause", "Pause", "Play", "F7", [this] { return play_.state() == PlayState::Playing; }, [this] {
        play_.pause();
        return true;
    });
    add("play.step", "Step One Tick", "Play", "F10", [this] { return play_.active(); }, [this] {
        play_.pause();
        play_.step(devices_, last_pointer_, 1);
        return true;
    });
    add("play.stop", "Stop", "Play", "Shift+F5", [this] { return play_.active(); }, [this] {
        stop_play();
        return true;
    });
    add("play.reload", "Reload Scripts", "Play", "", [this] { return play_.active(); }, [this] {
        play_.reload(true);
        return true;
    });
    // ---- view and tools
    add("view.frame", "Frame Scene", "View", "F", {}, [this] {
        frame_scene();
        return true;
    });
    add("view.zoom100", "Zoom to 100%", "View", "", {}, [this] {
        camera_.zoom = 1.0f;
        return true;
    });
    add("view.zoom_in", "Zoom In", "View", "Ctrl+=", {}, [this] {
        zoom_in();
        return true;
    });
    add("view.zoom_out", "Zoom Out", "View", "Ctrl+-", {}, [this] {
        zoom_out();
        return true;
    });
    add("view.zoom_reset", "Reset Zoom", "View", "Ctrl+0", {}, [this] {
        reset_zoom();
        return true;
    });
    add("view.theme_light", "Light Theme", "View", "", {}, [] {
        theme::apply(brand::Theme::Light);
        return true;
    });
    add("view.theme_dark", "Dark Theme", "View", "", {}, [] {
        theme::apply(brand::Theme::Dark);
        return true;
    });
    add("view.snap", "Toggle Snapping", "View", "", {}, [this] {
        snap_ = !snap_;
        return true;
    });
    add("view.collision_shapes", "Toggle Collision Shapes", "View", "F3", {}, [this] {
        collision_shapes_ = !collision_shapes_;
#if CUERVO_FEATURE_PHYSICS
        env_.host().physics_world().set_editor_debug_shapes(collision_shapes_);
        if (play_.active() && play_.session()) {
            play_.session()->host().physics_world().set_debug_shapes(collision_shapes_);
        }
#endif
        return true;
    });
    add("view.lighting", "Toggle Lighting Preview", "View", "", {}, [this] {
        preview_lighting_ = !preview_lighting_;
        return true;
    });
    const std::pair<Tool, const char*> tools[] = {
        {Tool::Select, "Q"}, {Tool::Move, "W"}, {Tool::Rotate, "E"}, {Tool::Scale, "R"}, {Tool::Paint, "B"}};
    for (const auto& [tool, key] : tools) {
        const Tool t = tool;
        add(std::string("tool.") + tool_name(t), std::string(tool_name(t)) + " Tool", "Tool", key, {}, [this, t] {
            tool_ = t;
            return true;
        });
    }
    for (const auto& p : panels_.all()) {
        Panel* panel = p.get();
        add(std::string("panel.") + panel->title(), std::string("Toggle Panel: ") + panel->title(), "View", "", {}, [panel] {
            panel->visible = !panel->visible;
            panel->select = panel->visible;
            return true;
        });
    }
    for (const std::string& name : layouts_.names()) {
        add("layout.apply." + name, "Layout: " + name, "View", "", {}, [this, name] {
            apply_layout(name);
            return true;
        });
    }
    add("layout.save", "Save Layout As...", "View", "", {}, [=] { return dialog(Dialog::SaveLayout); });
    // ---- scene
    add("edit.add_child", "Add Child Node...", "Edit", "Ctrl+A", {}, [this] {
        request_add_child(doc_.selection() != 0 ? doc_.selection() : doc_.model().root());
        return true;
    });
    add("help.license", "License", "Help", "", {}, [this] {
        show_license();
        return true;
    });
}

// ------------------------------------------------------------ shortcuts

void EditorApp::handle_shortcuts() {
    const ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (capturing_binding) return; // the key is for the input map
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, global)) open_palette();
    // Play controls work everywhere, also while the game has the keyboard.
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F5, global)) {
        stop_play();
    } else if (ImGui::Shortcut(ImGuiKey_F5, global)) {
        run_action("play.play");
    }
    if (ImGui::Shortcut(ImGuiKey_F6, global)) run_action("play.scene");
    if (ImGui::Shortcut(ImGuiKey_F7, global)) {
        if (play_.state() == PlayState::Playing) {
            play_.pause();
        } else {
            play_.resume();
        }
    }
    if (ImGui::Shortcut(ImGuiKey_F10, global)) run_action("play.step");
    // The game's keys are not editor shortcuts.
    if (game_input_) return;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, global)) run_action("edit.undo");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, global)) {
        run_action("edit.redo");
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global)) save_current_scene();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global)) run_action("file.save_as");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) run_action("file.open");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global)) run_action("file.new");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, global)) run_action("edit.duplicate");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G, global)) run_action("edit.group");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, global)) run_action("edit.add_child");
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, global)) {
        zoom_in();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, global)) {
        zoom_out();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, global) ||
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Keypad0, global)) {
        reset_zoom();
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    // Delete, tool keys and framing only when no text field has the keyboard.
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) run_action("edit.delete");
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) tool_ = Tool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) tool_ = Tool::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) tool_ = Tool::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tool_ = Tool::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) tool_ = Tool::Paint;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) frame_scene();
    }
}

// ------------------------------------------------------------ menu bar

void EditorApp::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    auto item = [&](const char* label, const char* id) {
        const Action* a = actions_.find(id);
        if (a == nullptr) return;
        if (ImGui::MenuItem(label, a->shortcut.empty() ? nullptr : a->shortcut.c_str(), false, actions_.enabled(*a))) {
            run_action(id);
        }
    };
    if (ImGui::BeginMenu("File")) {
        item("New Scene...", "file.new");
        item("Open Scene...", "file.open");
        item("Save", "file.save");
        item("Save As...", "file.save_as");
        ImGui::Separator();
        item("Switch Project...", "project.switch");
        if (ImGui::BeginMenu("Open Recent", !recents_.paths().empty())) {
            for (const RecentProject& r : recents_.list()) {
                if (ImGui::MenuItem(r.name.c_str(), nullptr, false, r.exists)) request_switch_project(r.path);
                ImGui::SetItemTooltip("%s", r.path.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item("Open Project in Code Editor", "file.open_code_editor");
        item("Open Scripts Folder in Code Editor", "file.open_scripts_editor");
        ImGui::Separator();
        item("Quit", "file.quit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        const std::string undo = doc_.stack().can_undo() ? "Undo " + doc_.stack().undo_label() : std::string("Undo");
        const std::string redo = doc_.stack().can_redo() ? "Redo " + doc_.stack().redo_label() : std::string("Redo");
        if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, doc_.stack().can_undo())) run_action("edit.undo");
        if (ImGui::MenuItem(redo.c_str(), "Ctrl+Shift+Z", false, doc_.stack().can_redo())) run_action("edit.redo");
        ImGui::Separator();
        item("Add Child Node...", "edit.add_child");
        item("Duplicate", "edit.duplicate");
        item("Delete", "edit.delete");
        item("Group", "edit.group");
        ImGui::Separator();
        if (ImGui::BeginMenu("Select")) {
            item("Select All Nodes", "select.all");
            item("Select Parent", "select.parent");
            item("Select Children", "select.children");
            item("Select Siblings", "select.siblings");
            item("Clear Selection", "select.none");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Command Palette...", "Ctrl+Shift+P")) open_palette();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Play")) {
        item("Play", "play.play");
        item("Play Current Scene", "play.scene");
        item("Play and Record", "play.record");
        ImGui::Separator();
        item("Pause", "play.pause");
        item("Step One Tick", "play.step");
        item("Stop", "play.stop");
        ImGui::Separator();
        item("Reload Scripts", "play.reload");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        item("Frame Scene", "view.frame");
        item("Zoom to 100%", "view.zoom100");
        if (ImGui::MenuItem("Snapping", nullptr, snap_)) snap_ = !snap_;
        if (ImGui::MenuItem("Collision Shapes", "F3", collision_shapes_)) {
            collision_shapes_ = !collision_shapes_;
#if CUERVO_FEATURE_PHYSICS
            env_.host().physics_world().set_editor_debug_shapes(collision_shapes_);
            if (play_.active() && play_.session()) {
                play_.session()->host().physics_world().set_debug_shapes(collision_shapes_);
            }
#endif
        }
        if (ImGui::MenuItem("Lighting Preview", nullptr, preview_lighting_)) preview_lighting_ = !preview_lighting_;
        ImGui::Separator();
        // The brand's light (Bone) and dark (Raven) themes (DESIGN.md 4.16).
        if (ImGui::BeginMenu("Theme")) {
            if (ImGui::MenuItem("Light", nullptr, theme::current() == brand::Theme::Light)) theme::apply(brand::Theme::Light);
            if (ImGui::MenuItem("Dark", nullptr, theme::current() == brand::Theme::Dark)) theme::apply(brand::Theme::Dark);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("UI Scale")) {
            const float scales[] = {0.75f, 0.90f, 1.0f, 1.10f, 1.25f, 1.50f, 1.75f, 2.0f};
            for (float s : scales) {
                const std::string label = std::format("{}%", static_cast<int>(std::round(s * 100.0f)));
                const bool selected = std::abs(theme::ui_scale() - s) < 0.02f;
                if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
                    set_ui_scale(s);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Font Scale")) {
            const float fscales[] = {0.80f, 0.90f, 1.0f, 1.10f, 1.20f, 1.30f, 1.40f, 1.50f};
            for (float s : fscales) {
                const std::string label = std::format("{}%", static_cast<int>(std::round(s * 100.0f)));
                const bool selected = std::abs(theme::font_scale() - s) < 0.02f;
                if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
                    set_font_scale(s);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item("Zoom In", "view.zoom_in");
        item("Zoom Out", "view.zoom_out");
        item("Reset Zoom", "view.zoom_reset");
        if (ImGui::BeginMenu("Layout")) {
            for (const std::string& name : layouts_.names()) {
                if (ImGui::MenuItem(name.c_str(), nullptr, layouts_.active() == name)) apply_layout(name);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Layout As...")) {
                dialog_ = Dialog::SaveLayout;
                dialog_opened_ = false;
            }
            if (ImGui::BeginMenu("Delete Layout")) {
                for (const std::string& name : layouts_.names()) {
                    if (LayoutStore::is_builtin(name)) continue;
                    if (ImGui::MenuItem(name.c_str())) {
                        std::string error;
                        if (layouts_.remove(name, error)) rebuild_actions();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        for (const auto& p : panels_.all()) ImGui::MenuItem(p->title(), nullptr, &p->visible);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        item("License Agreement", "help.license");
        ImGui::EndMenu();
    }
    ImGui::TextDisabled("  %s  |  %s", env_.project().name.c_str(),
                        doc_.has_path() ? env_.relative(doc_.path()).c_str() : "(new scene)");
    if (doc_.dirty()) ImGui::TextColored(theme::im(theme::colors().warning), "(modified)");
    ImGui::EndMainMenuBar();
}

// ------------------------------------------------------------ toolbar

void EditorApp::draw_toolbar() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const brand::ThemeColors& t = theme::colors();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize({vp->WorkSize.x, theme::toolbar_height});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::space_2, 4.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.0f, 0.0f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::im(t.bg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##toolbar", nullptr, flags)) {
        auto same = [] { ImGui::SameLine(0.0f, 2.0f); };
        auto action_button = [&](const char* icon, const char* id, const char* tip, bool selected = false) {
            const Action* a = actions_.find(id);
            const bool enabled = a != nullptr && actions_.enabled(*a);
            const std::string text = a != nullptr && !a->shortcut.empty() ? std::format("{} ({})", tip, a->shortcut) : tip;
            if (ui::icon_button(id, icon, text.c_str(), selected, enabled)) run_action(id);
            same();
        };
        action_button("new", "file.new", "New scene");
        action_button("open", "file.open", "Open scene");
        action_button("save", "file.save", "Save scene");
        ui::toolbar_separator();
        same();
        action_button("undo", "edit.undo", "Undo");
        action_button("redo", "edit.redo", "Redo");
        ui::toolbar_separator();
        same();
        action_button("select", "tool.Select", "Select tool", tool_ == Tool::Select);
        action_button("move", "tool.Move", "Move tool", tool_ == Tool::Move);
        action_button("rotate", "tool.Rotate", "Rotate tool", tool_ == Tool::Rotate);
        action_button("scale", "tool.Scale", "Scale tool", tool_ == Tool::Scale);
        action_button("paint", "tool.Paint", "Paint tool", tool_ == Tool::Paint);
        if (ui::icon_button("snap", "grid", "Snap to the grid, 15 degrees and steps of 0.1 (Ctrl while dragging inverts)",
                            snap_)) {
            snap_ = !snap_;
        }
        same();
        action_button("frame", "view.frame", "Frame the game screen");
        ui::toolbar_separator();
        same();
        const bool active = play_.active();
        const bool paused = play_.state() == PlayState::Paused;
        if (ui::icon_button("play", "play", paused ? "Resume (F5)" : "Play the project (F5)", false,
                            !active || (paused && !play_.panic()))) {
            run_action("play.play");
        }
        same();
        action_button("play_scene_icon_skip", "play.scene", "Play the open scene as edited");
        action_button("pause", "play.pause", "Pause");
        action_button("step", "play.step", "Step one tick");
        action_button("stop", "play.stop", "Stop");
        bool rec = play_.recording();
        if (ui::icon_button("rec", "record", "Record the inputs (a replay)", rec, active && play_.mode() == PlayMode::Live)) {
            if (rec) {
                play_.stop_recording();
            } else {
                play_.start_recording();
            }
        }
        same();
        ImGui::SameLine(0.0f, theme::space_3);
        ImGui::AlignTextToFramePadding();
        if (active) {
            const rhi::Color c = play_.panic() ? t.danger : play_.state() == PlayState::Playing ? t.success : t.warning;
            ImGui::TextColored(theme::im(c), "%s%s  tick %llu  %.3f ms/tick%s", play_.mode() == PlayMode::Replay ? "replay " : "",
                               play_.panic() ? "PANIC" : std::string(play_.state_name()).c_str(),
                               static_cast<unsigned long long>(play_.tick()), play_.average_tick_ms(),
                               play_.recording() ? "  REC" : "");
        } else {
            ImGui::TextDisabled("editing");
        }
        // The palette entry, right-aligned.
        const char* label = "Search commands   Ctrl+Shift+P";
        const float w = ImGui::CalcTextSize(label).x + theme::icon_size + 28.0f;
        ImGui::SameLine(vp->WorkSize.x - w - theme::space_2);
        ImGui::SetCursorPosY(3.0f);
        if (ui::icon_label_button(label, "search")) open_palette();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    // A hairline under the toolbar.
    ImGui::GetBackgroundDrawList()->AddLine({vp->WorkPos.x, vp->WorkPos.y + theme::toolbar_height - 0.5f},
                                            {vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + theme::toolbar_height - 0.5f},
                                            theme::im32(t.border));
}

// ------------------------------------------------------------ docks

namespace {

// The dock id of the editor's dock space (stable across frames and runs).
ImGuiID editor_dock_id() { return ImHashStr("CuervoEditorDock"); }

} // namespace

void EditorApp::build_default_dock(ImGuiID root, ImVec2 size) {
    // Godot-like layout (DESIGN.md 7.2): tree and assets left, view center,
    // inspector and debugger right, console and the 6B panels along the bottom.
    // The sizes are the layout's fractions (4.16 "Clarified (Milestone 8.3)").
    layout_.clamp();
    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(root, size);
    ImGuiID rest = root;
    const ImGuiID left = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, layout_.left, nullptr, &rest);
    // The right dock's fraction is of the whole window, the split's is of what is left.
    const ImGuiID right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, layout_.right / (1.0f - layout_.left),
                                                      nullptr, &rest);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Down, layout_.bottom, nullptr, &rest);
    const ImGuiID center = rest;
    for (const auto& p : panels_.all()) {
        const ImGuiID target = p->dock() == Dock::Left     ? left
                               : p->dock() == Dock::Right  ? right
                               : p->dock() == Dock::Bottom ? bottom
                                                           : center;
        ImGui::DockBuilderDockWindow(p->title(), target);
    }
    ImGui::DockBuilderFinish(root);
}

void EditorApp::draw_docks() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 origin{vp->WorkPos.x, vp->WorkPos.y + theme::toolbar_height};
    const ImVec2 size{vp->WorkSize.x, std::max(120.0f, vp->WorkSize.y - theme::toolbar_height)};

    // A new layout applied last frame (View > Layout, --layout): ImGui's ini text, or
    // the default arrangement built from the fractions. Done here, between frames' windows.
    if (!pending_dock_.empty()) {
        ImGui::LoadIniSettingsFromMemory(pending_dock_.c_str(), pending_dock_.size());
        pending_dock_.clear();
        dock_built_ = true;
    }
    const ImGuiID root = editor_dock_id();
    if (rebuild_dock_ || !dock_built_) {
        // First frame: ImGui settings (loaded from a saved layout) may already have the node.
        if (rebuild_dock_ || ImGui::DockBuilderGetNode(root) == nullptr) build_default_dock(root, size);
        rebuild_dock_ = false;
        dock_built_ = true;
    }

    // The host window: the dock space fills the area under the toolbar.
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(size);
    const ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNavFocus |
                                        ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##cuervo_dock_host", nullptr, host_flags);
    ImGui::PopStyleVar(2);
    ImGui::DockSpace(root, {0.0f, 0.0f}, ImGuiDockNodeFlags_None);
    ImGui::End();

    // Every panel is a dockable window: drag its tab to move it, drop it on another
    // dock's edge to split, or pull it out to float. The close button hides it (View menu).
    for (const auto& p : panels_.all()) {
        if (!p->visible) continue;
        // Focus once the window exists (a new panel docks on its first frame).
        if (p->select && ImGui::FindWindowByName(p->title()) != nullptr) {
            p->select = false;
            ImGui::SetNextWindowFocus();
        }
        // A panel newer than the saved layout (ImGui has no settings for it)
        // joins the dock of a panel with the same default dock, as a tab,
        // instead of floating over the editor.
        if (ImGui::FindWindowByName(p->title()) == nullptr &&
            ImGui::FindWindowSettingsByID(ImHashStr(p->title())) == nullptr) {
            for (const auto& q : panels_.all()) {
                if (q.get() == p.get() || q->dock() != p->dock()) continue;
                if (const ImGuiWindow* w = ImGui::FindWindowByName(q->title()); w != nullptr && w->DockId != 0) {
                    ImGui::SetNextWindowDockID(w->DockId);
                    break;
                }
            }
        }
        ImGui::SetNextWindowSizeConstraints({120.0f, 60.0f}, {FLT_MAX, FLT_MAX});
        // Each tab has its own close button; the dock node's extra one would only repeat it.
        ImGuiWindowClass window_class;
        window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoCloseButton;
        ImGui::SetNextWindowClass(&window_class);
        ImGuiWindowFlags flags = ImGuiWindowFlags_None;
        if (!p->padded()) flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (!p->padded()) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        if (ImGui::Begin(p->title(), &p->visible, flags)) p->draw(*this);
        ImGui::End();
        if (!p->padded()) ImGui::PopStyleVar();
    }
}

// ------------------------------------------------------------ command palette

void EditorApp::draw_palette() {
    if (palette_request_) {
        palette_request_ = false;
        palette_open_ = true;
        palette_index_ = 0;
        ImGui::OpenPopup("##palette");
    }
    if (!palette_open_) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const brand::ThemeColors& t = theme::colors();
    const float width = std::min(620.0f, vp->WorkSize.x - 40.0f);
    ImGui::SetNextWindowPos({vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f, vp->WorkPos.y + 56.0f});
    ImGui::SetNextWindowSize({width, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::radius_lg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::space_2, theme::space_2});
    ImGui::PushStyleColor(ImGuiCol_Border, theme::im(t.border_strong));
    if (!ImGui::BeginPopup("##palette", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) {
        palette_open_ = false;
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return;
    }
    const bool appearing = ImGui::IsWindowAppearing();
    if (appearing) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const std::string before = palette_query_;
    ImGui::InputTextWithHint("##query", "Type a command, e.g. \"dup sel\" or \"add node label\"", &palette_query_);
    if (palette_query_ != before) palette_index_ = 0;
    const std::vector<ActionRegistry::Hit> hits = actions_.search(palette_query_);
    const int count = static_cast<int>(hits.size());
    bool scroll_to_selected = appearing;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && count > 0) {
        palette_index_ = (palette_index_ + 1) % count;
        scroll_to_selected = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && count > 0) {
        palette_index_ = (palette_index_ + count - 1) % count;
        scroll_to_selected = true;
    }
    palette_index_ = std::clamp(palette_index_, 0, std::max(0, count - 1));
    std::string run_id;
    const float row_h = 26.0f;
    const float list_h = std::min(12.0f, static_cast<float>(std::max(count, 1))) * row_h;
    if (ImGui::BeginChild("##hits", {0.0f, list_h}, ImGuiChildFlags_None)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < count; ++i) {
            const ActionRegistry::Hit& hit = hits[static_cast<std::size_t>(i)];
            const Action& a = *hit.action;
            ImGui::PushID(i);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("##row", {w, row_h});
            if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0.0f) palette_index_ = i;
            if (ImGui::IsItemClicked() && hit.enabled) run_id = a.id;
            const bool selected = i == palette_index_;
            if (selected) {
                dl->AddRectFilled(p, {p.x + w, p.y + row_h}, theme::im32(t.accent_subtle), theme::radius_md);
                dl->AddRectFilled(p, {p.x + 2.0f, p.y + row_h}, theme::im32(t.accent));
                if (scroll_to_selected) ImGui::SetScrollHereY();
            }
            const float ty = p.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
            const rhi::Color ink = hit.enabled ? t.text : t.text_muted;
            const std::string label = ActionRegistry::label(a);
            const std::size_t cat = a.category.empty() ? 0 : a.category.size() + 2;
            // Category dimmed, title in full; matched characters in the accent.
            std::vector<std::size_t> cat_pos;
            std::vector<std::size_t> title_pos;
            for (std::size_t pos : hit.positions) {
                if (pos < cat) {
                    cat_pos.push_back(pos);
                } else {
                    title_pos.push_back(pos - cat);
                }
            }
            const ImU32 hl = theme::im32(t.accent);
            float x = p.x + 12.0f;
            if (cat > 0) {
                const std::string c = label.substr(0, cat);
                ui::highlighted_text(dl, {x, ty}, c, cat_pos, theme::im32(t.text_muted), hl);
                x += ImGui::CalcTextSize(c.c_str()).x;
            }
            ui::highlighted_text(dl, {x, ty}, std::string_view(label).substr(cat), title_pos, theme::im32(ink), hl);
            if (!a.shortcut.empty()) {
                const float sw = ImGui::CalcTextSize(a.shortcut.c_str()).x;
                dl->AddText({p.x + w - sw - 10.0f, ty}, theme::im32(t.text_muted), a.shortcut.c_str());
            }
            ImGui::PopID();
        }
        if (count == 0) {
            ImGui::Dummy({0.0f, 6.0f});
            ImGui::TextDisabled("  No command matches '%s'. Try fewer letters.", palette_query_.c_str());
        }
    }
    ImGui::EndChild();
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && count > 0 &&
        hits[static_cast<std::size_t>(palette_index_)].enabled) {
        run_id = hits[static_cast<std::size_t>(palette_index_)].action->id;
    }
    if (!run_id.empty()) {
        ImGui::CloseCurrentPopup();
        palette_open_ = false;
        ImGui::EndPopup();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        run_action(run_id);
        return;
    }
    ImGui::EndPopup();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// ------------------------------------------------------------ messages

void EditorApp::draw_messages() {
    messages_.update(clock_);
    if (messages_.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(640.0f, vp->WorkSize.x - 40.0f);
    ImGui::SetNextWindowPos({vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f, vp->WorkPos.y + theme::toolbar_height + 8.0f});
    ImGui::SetNextWindowSize({width, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0f, 6.0f});
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##messages", nullptr, flags)) {
        for (MessageCenter::Item& item : messages_.items()) {
            if (ui::message_row(item, width)) item.dismissed = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace cuervo::editor

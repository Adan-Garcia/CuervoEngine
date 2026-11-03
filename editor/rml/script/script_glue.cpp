// Shell side of the script editor (hook file): constructs the ScriptPanel, forwards the frame, the keys
// and "open this script" requests. Everything else lives in editor/rml/script/script_panel.cpp.
#include <SDL3/SDL.h>

#include "editor/rml/rml_shell.hpp"
#include "editor/rml/script/script_panel.hpp"

namespace cuervo::editor {

void Shell::init_script_panel() {
    ScriptPanel::Host host;
    host.script_mode = [this] { return layout_.center == "script"; };
    host.persist = [this] { save_layout(); };
    host.show_script_mode = [this] {
        layout_.center = "script";
        apply_layout();
        save_layout();
    };
    host.show_diagnostics = [this] {
        layout_.drawer_open = true;
        layout_.drawer = "diag";
        apply_layout();
        build_drawer();
        save_layout();
    };
    host.show_drawer = [this](const std::string& which) {
        layout_.drawer_open = true;
        layout_.drawer = which;
        apply_layout();
        build_drawer();
        save_layout();
    };
    script_ = std::make_shared<ScriptPanel>(ctl_, std::move(host));
    script_->bind(page_);
}

void Shell::refresh_script() {
    if (!script_) return;
    // Script mode opens with Diagnostics in the drawer unless the user already has one of the other tabs there.
    if (layout_.center == "script" && !script_entered_) {
        script_entered_ = true;
        if (!layout_.drawer_open || layout_.drawer == "node") {
            layout_.drawer_open = true;
            layout_.drawer = "diag";
            apply_layout();
            build_drawer();
        }
    }
    script_->refresh(layout_.drawer_open ? layout_.drawer : std::string(), SDL_GetTicks());
}

void Shell::script_diagnostics(bool refs) {
    if (!script_) return;
    if (refs) script_->rebuild_references();
    else script_->build_diagnostics();
}

bool Shell::script_debug_drawer() {
    if (!script_ || !script_->debug_view()) return false;
    script_->rebuild_debugger();
    return true;
}

std::string Shell::script_debug_chips() const { return script_ ? script_->debug_chips() : std::string(); }

void Shell::open_script(const std::filesystem::path& file, int line) {
    if (script_) script_->open_file(file, line);
    else ctl_.open_in_external_editor(file, line);
}

bool Shell::script_key(const SDL_Event& ev) { return script_ && script_->handle_key(ev); }

void Shell::register_script_actions() {
    ActionRegistry& reg = ctl_.actions();
    const auto add = [&](std::string id, std::string title, std::string key, std::function<bool()> run) {
        reg.add({std::move(id), std::move(title), "Script", std::move(key), [this] { return script_ && script_->script_active(); }, std::move(run)});
    };
    add("script.format", "Format Document", "Shift+Alt+F", [this] { return script_ && script_->format_document(); });
    add("script.format_on_save", "Toggle Format on Save", "", [this] {
        if (!script_) return false;
        script_->set_format_on_save(!script_->workspace().format_on_save);
        ctl_.notify_ok(script_->workspace().format_on_save ? "Format on save is on" : "Format on save is off");
        return true;
    });
    add("script.goto_definition", "Go to Definition", "F12", [this] { return script_ && script_->goto_definition(); });
    add("script.references", "Find References", "Shift+F12", [this] { return script_ && script_->find_references(); });
    add("script.rename", "Rename Symbol", "F2", [this] { return script_ && script_->open_rename(); });
    add("script.find", "Find in Script", "Ctrl+F", [this] { if (script_) script_->open_find(false); return true; });
    add("script.replace", "Replace in Script", "Ctrl+H", [this] { if (script_) script_->open_find(true); return true; });
}

bool Shell::script_wants_frames() const { return script_ && script_->pending(); }

} // namespace cuervo::editor

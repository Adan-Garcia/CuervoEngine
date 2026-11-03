#pragma once
// The Script workspace UI (AUDIT.md section 15): script list and outline in the left sidebar, file tabs,
// breadcrumb/status and the <code-view> in the center card, and the Diagnostics list in the bottom
// drawer. All document logic lives in ScriptWorkspace (editor_core); this only presents it.

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <string>

#include "editor/editor_controller.hpp"
#include "editor/rml/script/code_view.hpp"
#include "editor/script_assist.hpp"
#include "editor/script_find.hpp"
#include "editor/script_workspace.hpp"

namespace cuervo::editor {

class ScriptPanel final : public Rml::EventListener {
public:
    // What the shell provides.
    struct Host {
        std::function<void()> show_script_mode;   // switch the center card to Script
        std::function<void()> show_diagnostics;   // open the drawer on Diagnostics
        std::function<void(const std::string&)> show_drawer; // open the bottom drawer on this tab ("refs", ...)
        std::function<void()> persist;            // write the user settings (format on save, breakpoints)
        std::function<bool()> script_mode;        // the center card shows Script
    };
    ScriptPanel(EditorController& ctl, Host host);

    void bind(Rml::ElementDocument* page);
    // Once per frame. `diag_drawer` = the drawer currently shows Diagnostics.
    void refresh(const std::string& drawer, std::uint64_t now_ms);
    void ProcessEvent(Rml::Event& e) override;

    // Opens a file (absolute or project relative) at a 1-based line (0 = keep).
    bool open_file(const std::filesystem::path& file, int line = 0, int col = 0);
    // Keys that work anywhere in Script mode (Ctrl+S, Ctrl+W, Ctrl+Tab ...); true when handled.
    bool handle_key(const SDL_Event& ev);
    // True while an edit waits for its diagnostics (the app keeps drawing until they arrive).
    bool pending() const;
    // The drawer's Diagnostics content.
    void build_diagnostics();
    // A project was opened or closed: tabs reset (after the shell has asked about unsaved scripts, see dirty_count).
    void project_changed();
    int dirty_count() const { return ws_.dirty_count(); }
    ScriptWorkspace& workspace() { return ws_; }
    CodeView* view() { return view_; }
    // Test and screenshot hooks.
    bool save_active();
    void close_tab(int id, bool force);
    // Find and replace (Ctrl+F, Ctrl+H, F3, Shift+F3, Esc). Public for tests.
    void open_find(bool with_replace);
    void close_find();
    void find_step(bool back);
    void find_replace_current();
    int find_replace_everything();
    void set_find(const std::string& query, const FindOptions& opt);
    // Autocomplete / hover / signature help. Public for tests.
    CompletionState& completion() { return comp_; }
    void trigger_completion(int min_prefix);
    bool accept_completion();
    bool goto_definition();
    // Palette actions.
    bool format_document();
    // ---- debugger drawer
    // The script debugger view is shown while a debug session exists (breakpoints at Play); "Nodes" keeps
    // the live node view of the old Debugger tab.
    bool debug_view() const { return ctl_.play().debug() != nullptr && !dbg_nodes_; }
    bool debug_session() const { return ctl_.play().debug() != nullptr; }
    std::string debug_chips() const;
    void rebuild_debugger() { build_debugger(); } // compares with what is shown, so it is cheap and keeps field focus
    bool debug_paused() const { const DebugSession* d = ctl_.play().debug(); return d != nullptr && d->stopped(); }
    void toggle_breakpoint_at(int line0);
    void set_format_on_save(bool on);
    bool find_references();
    void build_references();
    void rebuild_references() { refs_html_.clear(); refs_head_html_.clear(); build_references(); }
    bool open_rename();
    void close_rename();
    bool apply_rename();
    bool rename_open() const { return rename_open_; }
    const std::vector<RefHit>& refs() const { return refs_; }
    bool script_active() const { return ws_.doc(ws_.active()) != nullptr; }
    bool find_open() const { return find_open_; }
    const FindSession& find_session() const { return fs_; }

private:
    Rml::Element* by_id(const char* id) const { return page_ != nullptr ? page_->GetElementById(id) : nullptr; }
    void attach_view();
    void rebuild_tabs();
    void rebuild_side();
    void rebuild_status();
    void set_html(Rml::Element* e, const std::string& html, std::string& cache);
    void save(int id);
    void save_all();
    void create_script(const std::string& typed);
    void jump(const std::string& rel, int line, int col);
    std::uint64_t tabs_signature() const;

    EditorController& ctl_;
    Host host_;
    ScriptWorkspace ws_;
    Rml::ElementDocument* page_ = nullptr;
    CodeView* view_ = nullptr;
    std::filesystem::path project_;
    int shown_doc_ = -2;
    std::string tabs_html_, side_html_, status_html_, diag_html_, diag_head_html_, badge_;
    std::string filter_;
    int closing_ = 0;          // tab with the "save changes?" bar
    bool adding_ = false;      // the new-script name row
    int sev_filter_ = 0;       // 0 all, 1 errors, 2 warnings
    std::uint64_t diag_sig_ = 0;
    int diag_active_ = -2; // the script the Diagnostics header was built for
    std::uint64_t side_sig_ = 0;
    std::uint64_t status_sig_ = 0;
    bool ctrl_tab_ = false;
    // Find state.
    bool find_open_ = false;
    bool fr_open_ = false;
    FindSession fs_;
    int fdoc_ = -1;
    void find_sync(bool reveal);
    void find_ui();
    // Assist state.
    CompletionState comp_;
    int ac_first_ = 0;
    std::string ac_html_, hov_html_, sig_html_;
    std::uint64_t now_ms_ = 0;
    std::uint64_t comp_due_ms_ = 0, sig_due_ms_ = 0, hover_since_ms_ = 0;
    bool comp_pending_ = false, sig_pending_ = false, sig_open_ = false;
    TextPos hover_pos_{-1, 0};
    TextRange hover_word_;
    Rml::Vector2f hover_at_{};
    bool hover_shown_ = false, hover_tried_ = false;
    std::vector<RefHit> refs_;
    std::string refs_name_, refs_html_, refs_head_html_;
    bool rename_open_ = false;
    int rename_doc_ = -1;
    TextPos rename_at_;
    std::string rename_old_;
    std::optional<RenamePlan> rename_plan_;
    void rename_preview(const std::string& name);
    void build_debugger();
    void debug_tick();
    void push_breakpoints(bool all);
    void save_breakpoints();
    bool dbg_nodes_ = false;
    std::string dbg_head_html_, dbg_html_;
    std::uint64_t bp_seen_ = 0, dbg_seen_stop_ = 0, dbg_sig_ = 0;
    int dbg_seen_frame_ = -1;
    std::set<std::string> bp_sent_;
    int dbg_reload_seen_ = -1;
    void assist_tick();
    void render_completion();
    void render_signature();
    void show_hover();
    void hide_hover();
    void place(Rml::Element* e, Rml::Vector2f anchor_top, Rml::Vector2f anchor_bottom, float est_h, bool above = false);
    bool assist_key(Rml::Input::KeyIdentifier k, bool ctrl, bool shift, bool alt);
    void assist_changed();
};

} // namespace cuervo::editor

#include "editor/rml/script/script_panel.hpp"

#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <algorithm>
#include <format>
#include <set>
#include <functional>

#include "editor/rml/rml_dom.hpp"
#include "editor/rml_widgets.hpp"

namespace fs = std::filesystem;

namespace cuervo::editor {

namespace {

using dom::find_attr;

std::string kind_letter(int kind) {
    switch (kind) {
    case 5: return "C";
    case 23: return "S";
    case 10: return "E";
    case 11: return "T";
    case 6: case 12: case 9: return "m";
    case 24: return "s";
    case 22: return "e";
    default: return "f";
    }
}

std::string value_of(Rml::Event& e) { return e.GetParameter("value", Rml::String()); }

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ull; }

std::string pascal(const std::string& stem) {
    std::string out;
    bool up = true;
    for (char c : stem) {
        if (c == '_' || c == '-' || c == ' ') { up = true; continue; }
        out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        up = false;
    }
    return out.empty() ? "NewScript" : out;
}

} // namespace

ScriptPanel::ScriptPanel(EditorController& ctl, Host host) : ctl_(ctl), host_(std::move(host)) {}

void ScriptPanel::bind(Rml::ElementDocument* page) {
    page_ = page;
    view_ = dynamic_cast<CodeView*>(by_id("code"));
    for (const char* id : {"pane-script", "script-pane", "drawer"}) {
        if (Rml::Element* e = by_id(id)) {
            e->AddEventListener(Rml::EventId::Click, this);
            e->AddEventListener(Rml::EventId::Change, this);
            e->AddEventListener(Rml::EventId::Keydown, this, true); // capture: text inputs stop the key before it bubbles
        }
    }
    if (view_ != nullptr) {
        view_->hooks.changed = [this] { status_sig_ = 0; assist_changed(); };
        view_->hooks.caret_moved = [this] { status_sig_ = 0; assist_changed(); };
        view_->hooks.key = [this](Rml::Input::KeyIdentifier k, bool ctrl, bool shift, bool alt) { return assist_key(k, ctrl, shift, alt); };
        view_->hooks.gutter_click = [this](int line) { toggle_breakpoint_at(line); };
        view_->hooks.ctrl_click = [this](TextPos) { goto_definition(); };
        view_->hooks.typed = [this](const std::string& t) {
            hide_hover();
            const unsigned char c = t.empty() ? 0 : static_cast<unsigned char>(t[0]);
            const bool ident = t.size() == 1 && (std::isalnum(c) != 0 || c == '_' || c == '.');
            if (t == "(" || t == ",") { sig_pending_ = true; sig_due_ms_ = SDL_GetTicks() + 60; }
            else if (t == ")") { sig_open_ = false; sig_pending_ = false; }
            if (ident) { comp_pending_ = true; comp_due_ms_ = SDL_GetTicks() + 110; }
            else if (comp_.active()) comp_.close();
        };
        view_->hooks.hover = [this](TextPos p, Rml::Vector2f at) {
            if (p.line < 0) { hide_hover(); hover_pos_ = {-1, 0}; return; }
            ScriptDoc* d = ws_.doc(ws_.active());
            if (d == nullptr) return;
            const TextRange w = d->buf.word_at(p);
            if (hover_shown_ && w == hover_word_ && !w.empty()) return;
            if (hover_pos_.line >= 0 && w == hover_word_ && !w.empty() && !hover_shown_) { hover_at_ = at; return; } // still dwelling on the same word
            hide_hover();
            hover_pos_ = p; hover_word_ = w; hover_at_ = at; hover_since_ms_ = SDL_GetTicks();
        };
    }
    project_changed();
}

bool ScriptPanel::pending() const {
    for (const auto& d : ws_.tabs()) if (d->checked_rev != d->buf.revision()) return true;
    // Debounced assists keep the frame loop running until they fire.
    return comp_pending_ || sig_pending_ || (hover_pos_.line >= 0 && !hover_shown_ && !hover_tried_);
}

void ScriptPanel::project_changed() {
    project_ = ctl_.env().has_project() ? ctl_.env().project_dir() : fs::path();
    ws_.reset(project_);
    ws_.format_on_save = ctl_.settings().format_on_save();
    {
        ScriptWorkspace::Breakpoints b;
        if (!project_.empty()) for (const auto& [file, line] : ctl_.settings().breakpoints(project_)) b[file].insert(line);
        ws_.set_breakpoints(std::move(b));
        bp_seen_ = 0;
        bp_sent_.clear();
    }
    shown_doc_ = -2;
    closing_ = 0;
    adding_ = false;
    tabs_html_.clear();
    side_html_.clear();
    status_html_.clear();
    diag_html_.clear();
    side_sig_ = 0;
}

bool ScriptPanel::open_file(const fs::path& file, int line, int col) {
    std::string err;
    const int id = ws_.open(file, err);
    if (id < 0) { ctl_.notify_ok(err); return false; }
    if (host_.show_script_mode) host_.show_script_mode();
    shown_doc_ = -2; // attach on next refresh
    refresh(std::string(), 0);
    if (line > 0 && view_ != nullptr) {
        ScriptDoc* d = ws_.doc(id);
        const TextPos p = d->buf.clamp({line - 1, std::max(0, col - 1)});
        view_->goto_pos(p, true);
    }
    if (view_ != nullptr) view_->Focus();
    return true;
}

bool ScriptPanel::format_document() {
    const int id = ws_.active();
    if (id < 0) return false;
    const bool changed = ws_.format(id);
    if (view_ != nullptr && changed) view_->edit_done();
    ctl_.notify_ok(changed ? "Formatted" : "Nothing to format, or the script has errors");
    return changed;
}

// F12 and Ctrl+click: opens the declaring file (another tab when needed) and puts the caret on the name.
void ScriptPanel::toggle_breakpoint_at(int line0) {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr || line0 < 0 || line0 >= d->buf.line_count()) return;
    const bool on = ws_.toggle_breakpoint(d->rel, line0 + 1);
    save_breakpoints();
    if (on && ctl_.play().active() && ctl_.play().debug() == nullptr && ctl_.play().mode() == PlayMode::Live)
        ctl_.notify_ok("Breakpoints apply from the next Play: the running game has no debugger attached.");
}

void ScriptPanel::save_breakpoints() {
    if (project_.empty()) return;
    EditorSettings::BreakpointList list;
    for (const auto& [file, lines] : ws_.breakpoints()) for (const int l : lines) list.emplace_back(file, l);
    ctl_.settings().set_breakpoints(project_, list);
    if (host_.persist) host_.persist();
}

// Keeps the controller's start-up list and a running debugger in step with the gutter.
void ScriptPanel::push_breakpoints(bool all) {
    ctl_.play().set_debug_breakpoints(ws_.breakpoints());
    DebugSession* ds = ctl_.play().debug();
    if (ds == nullptr) return;
    std::set<std::string> now;
    for (const auto& [file, lines] : ws_.breakpoints()) {
        if (!lines.empty()) now.insert(file);
        if (all || !lines.empty() || bp_sent_.count(file) > 0) ds->set_breakpoints(file, lines);
    }
    bp_sent_ = std::move(now);
}

void ScriptPanel::debug_tick() {
    if (ws_.breakpoint_revision() != bp_seen_) { bp_seen_ = ws_.breakpoint_revision(); push_breakpoints(false); }
    DebugSession* ds = ctl_.play().debug();
    if (ds == nullptr) { dbg_reload_seen_ = -1; if (view_ != nullptr) view_->set_paused(-1, -1); return; }
    // A hot reload builds new code: resend every file so the debugger re-verifies against it.
    if (const script::GameSession* gs = ctl_.play().session()) {
        const int reloads = static_cast<int>(gs->host().reloads());
        if (dbg_reload_seen_ >= 0 && reloads != dbg_reload_seen_) push_breakpoints(true);
        dbg_reload_seen_ = reloads;
    }
    if (!ds->stopped() || ds->frames().empty()) { if (view_ != nullptr && !ds->stopped()) view_->set_paused(-1, -1); return; }
    const DebugFrame& f = ds->frames()[static_cast<std::size_t>(ds->frame())];
    const bool fresh = dbg_seen_stop_ != ds->stop_serial();
    if (fresh || dbg_seen_frame_ != ds->frame()) {
        dbg_seen_stop_ = ds->stop_serial();
        dbg_seen_frame_ = ds->frame();
        if (fresh) {
            // The drawer takes over: Debugger tab, script view.
            dbg_nodes_ = false;
            if (host_.show_drawer) host_.show_drawer("debugger");
        }
        if (!f.rel.empty()) {
            const fs::path file = project_ / f.rel;
            if (ws_.find(file) < 0 || ws_.find(file) != ws_.active() || fresh) open_file(file, 0, 0);
            const int id = ws_.find(file);
            if (id >= 0 && view_ != nullptr) {
                view_->set_paused(id, f.line - 1);
                if (ScriptDoc* d = ws_.doc(id)) view_->goto_pos(d->buf.clamp({f.line - 1, 0}), true);
            }
        } else if (view_ != nullptr) view_->set_paused(-1, -1);
    }
}

void ScriptPanel::set_format_on_save(bool on) {
    ws_.format_on_save = on;
    ctl_.settings().set_format_on_save(on);
    if (host_.persist) host_.persist();
}

bool ScriptPanel::goto_definition() {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return false;
    ws_.sync(*d);
    const auto loc = ws_.server().definition(ws_.uri(*d), ws_.to_lsp(*d, d->buf.primary().head));
    if (!loc) { ctl_.notify_ok("No definition found"); return false; }
    const fs::path target = pluma::lsp::uri_to_path(loc->uri);
    std::error_code ec;
    if (!fs::exists(target, ec)) { ctl_.notify_ok("The definition is built in"); return false; }
    if (!open_file(target)) return false;
    ScriptDoc* t = ws_.doc(ws_.active());
    if (t != nullptr && view_ != nullptr) view_->goto_pos(ws_.from_lsp(*t, loc->range.start), true);
    return true;
}

void ScriptPanel::attach_view() {
    if (view_ == nullptr) return;
    const int id = ws_.active();
    if (id == shown_doc_) return;
    shown_doc_ = id;
    view_->set_doc(&ws_, id);
    view_->SetProperty("display", id < 0 ? "none" : "block");
    if (Rml::Element* e = by_id("sempty")) e->SetProperty("display", id < 0 ? "flex" : "none");
}

std::uint64_t ScriptPanel::tabs_signature() const {
    std::uint64_t h = 1469598103934665603ull;
    h = mix(h, static_cast<std::uint64_t>(ws_.active() + 2));
    h = mix(h, static_cast<std::uint64_t>(closing_));
    for (const auto& d : ws_.tabs()) {
        h = mix(h, static_cast<std::uint64_t>(d->id));
        h = mix(h, d->buf.dirty() ? 7 : 3);
        h = mix(h, static_cast<std::uint64_t>(ws_.error_count(*d) * 131 + ws_.warning_count(*d)));
        h = mix(h, d->missing ? 11 : 5);
    }
    return h;
}

void ScriptPanel::set_html(Rml::Element* e, const std::string& html, std::string& cache) {
    if (e == nullptr || cache == html) return;
    cache = html;
    e->SetInnerRML(html);
}

void ScriptPanel::rebuild_tabs() {
    std::string out;
    for (const auto& d : ws_.tabs()) {
        const bool active = d->id == ws_.active(), dirty = d->buf.dirty();
        const int errs = ws_.error_count(*d);
        out += std::format("<div class=\"ftab{}\" data-sc=\"tab\" data-id=\"{}\" title=\"{}{}\">", active ? " active" : "", d->id, w::esc(d->rel), dirty ? " (unsaved changes)" : "");
        if (dirty) out += "<span class=\"dirty\"></span>";
        out += std::format("<span class=\"fname\">{}</span>", w::esc(d->name()));
        if (errs > 0) out += std::format("<span class=\"ferr\" title=\"{} errors\">{}</span>", errs, errs);
        out += std::format("<span class=\"fx\" data-sc=\"close\" data-id=\"{}\" title=\"Close (Ctrl+W)\">{}</span></div>", d->id, w::icon("close", 12));
    }
    out += "<div class=\"ftab-add\" data-sc=\"new\" title=\"New script\">" + w::icon("plus", 14) + "</div><div class=\"grow\"></div>";
    if (closing_ != 0) {
        if (const ScriptDoc* d = ws_.doc(closing_)) {
            out += std::format("<div class=\"closebar\"><span>Save changes to {}?</span><button class=\"btn sm primary\" data-sc=\"close-save\"><span>Save</span></button><button class=\"btn sm\" data-sc=\"close-discard\"><span>Discard</span></button><button class=\"btn sm\" data-sc=\"close-cancel\"><span>Cancel</span></button></div>", w::esc(d->name()));
        }
    }
    out += std::format("<button class=\"btn sm{}\" data-sc=\"save-all\" title=\"Save all (Ctrl+Shift+S)\"><span>Save all</span></button>", ws_.dirty_count() > 0 ? "" : " off");
    set_html(by_id("stabs"), out, tabs_html_);
}

void ScriptPanel::rebuild_side() {
    // Scripts list.
    std::string out;
    std::string needle = filter_;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<std::string> files = ws_.project_scripts();
    if (!project_.empty()) {
        for (const std::string& rel : files) {
            std::string low = rel;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!needle.empty() && low.find(needle) == std::string::npos) continue;
            const int id = ws_.find(project_ / rel);
            const ScriptDoc* d = id >= 0 ? ws_.doc(id) : nullptr;
            const bool active = d != nullptr && d->id == ws_.active();
            std::string sec;
            if (d != nullptr) {
                const int e = ws_.error_count(*d), wn = ws_.warning_count(*d);
                if (e + wn > 0) sec = std::format("<span class=\"prob\">{} problem{}</span>", e + wn, e + wn == 1 ? "" : "s");
                if (d->buf.dirty()) sec += "<span class=\"dirty\" title=\"Unsaved changes\"></span>";
            }
            const std::size_t slash = rel.rfind('/');
            const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
            const std::string dir = slash == std::string::npos ? std::string() : rel.substr(0, slash + 1);
            out += std::format("<div class=\"row{}\" data-sc=\"open\" data-file=\"{}\" title=\"{}\">{}<span class=\"name\">{}</span><span class=\"dir\">{}</span><div class=\"grow\"></div>{}</div>", active ? " selected" : "", w::esc(rel), w::esc(rel), w::icon("code", 14), w::esc(name), w::esc(dir), sec);
        }
        if (out.empty()) out = files.empty() ? "<div class=\"hint\">No scripts in this project. Use + to create one.</div>" : "<div class=\"hint\">Nothing matches.</div>";
    } else out = "<div class=\"hint\">Open a project to edit its scripts.</div>";
    if (adding_) out = "<div class=\"newrow\"><input id=\"script-new\" type=\"text\" placeholder=\"scripts/name.plm\"/></div>" + out;
    set_html(by_id("script-list"), out, side_html_);

    // Outline of the active document, with the item under the caret marked.
    std::string ol;
    const ScriptDoc* d = ws_.doc(ws_.active());
    if (Rml::Element* t = by_id("outline-file")) t->SetInnerRML(d != nullptr ? w::esc(d->name()) : std::string());
    if (d != nullptr) {
        const TextPos caret = d->buf.primary().head;
        int deepest = -1;
        for (std::size_t i = 0; i < d->outline.size(); ++i) {
            const OutlineItem& o = d->outline[i];
            if (o.range.start <= caret && caret <= o.range.end) deepest = static_cast<int>(i);
        }
        for (std::size_t i = 0; i < d->outline.size(); ++i) {
            const OutlineItem& o = d->outline[i];
            ol += std::format("<div class=\"row orow{}\" data-sc=\"outline\" data-line=\"{}\" data-col=\"{}\" style=\"padding-left: {}dp\"><span class=\"kind k{}\">{}</span><span class=\"name\">{}</span><div class=\"grow\"></div><span class=\"type\">{}</span></div>",
                              static_cast<int>(i) == deepest ? " selected" : "", o.pos.line + 1, o.pos.col + 1, o.depth * 12, kind_letter(o.kind), kind_letter(o.kind), w::esc(o.name), w::esc(o.detail));
        }
        if (ol.empty()) ol = "<div class=\"hint\">No symbols.</div>";
    } else ol = "<div class=\"hint\">The outline of the open script shows here.</div>";
    if (Rml::Element* e = by_id("script-outline")) {
        static thread_local std::string cache; // one panel per window
        set_html(e, ol, cache);
    }
}

void ScriptPanel::rebuild_status() {
    const ScriptDoc* d = ws_.doc(ws_.active());
    std::string out;
    if (d != nullptr) {
        const TextPos c = d->buf.primary().head;
        std::string crumb = std::format("{}", w::esc(d->rel));
        // Breadcrumb: the outline items that contain the caret, outermost first.
        for (const OutlineItem& o : d->outline) if (o.range.start <= c && c <= o.range.end) crumb += std::format("<span class=\"sep\">&gt;</span>{}", w::esc(o.name));
        const int dcol = display_col(d->buf.line(c.line), c.col, d->buf.indent().width) + 1;
        out = std::format("<span class=\"crumbs\">{}</span><div class=\"grow\"></div><span class=\"pos\">Ln {}, Col {}<span class=\"sep\">|</span>UTF-8<span class=\"sep\">|</span>{}<span class=\"sep\">|</span>{}</span>",
                          crumb, c.line + 1, dcol, d->buf.line_ending() == LineEnding::CrLf ? "CRLF" : "LF",
                          d->buf.indent().tabs ? std::string("Tabs") : std::format("Spaces: {}", d->buf.indent().width));
    }
    set_html(by_id("scrumb"), out, status_html_);
}

void ScriptPanel::refresh(const std::string& drawer, std::uint64_t now_ms) {
    const bool diag_drawer = drawer == "diag";
    now_ms_ = now_ms;
    if (page_ == nullptr) return;
    const bool changed = ws_.tick(now_ms);
    attach_view();
    const bool visible = host_.script_mode && host_.script_mode();
    // The strip badge counts errors and warnings of every open script, in any mode.
    int errs = 0, warns = 0;
    for (const auto& d : ws_.tabs()) { errs += ws_.error_count(*d); warns += ws_.warning_count(*d); }
    const std::string badge = errs + warns > 0 ? std::to_string(errs + warns) : std::string();
    if (badge != badge_) {
        badge_ = badge;
        if (Rml::Element* b = by_id("n-diag")) b->SetInnerRML(badge);
    }
    if (visible) {
        const std::uint64_t sig = tabs_signature();
        if (sig != diag_sig_ || tabs_html_.empty()) { diag_sig_ = sig; rebuild_tabs(); side_sig_ = 0; }
        std::uint64_t ss = mix(sig, static_cast<std::uint64_t>(adding_ ? 1 : 0));
        if (const ScriptDoc* d = ws_.doc(ws_.active())) { ss = mix(ss, d->checked_rev); ss = mix(ss, static_cast<std::uint64_t>(d->buf.primary().head.line)); }
        if (ss != side_sig_ || changed) { side_sig_ = ss; rebuild_side(); }
        const ScriptDoc* d = ws_.doc(ws_.active());
        std::uint64_t st = 0;
        if (d != nullptr) st = mix(mix(mix(static_cast<std::uint64_t>(d->id + 1), d->buf.revision()), static_cast<std::uint64_t>(d->buf.primary().head.line) * 4099 + static_cast<std::uint64_t>(d->buf.primary().head.col)), d->checked_rev);
        if (st != status_sig_ || changed) { status_sig_ = st; rebuild_status(); }
    }
    if (find_open_) {
        const ScriptDoc* fd = ws_.doc(ws_.active());
        if (fd == nullptr) close_find();
        else if (fdoc_ != ws_.active() || fs_.revision != fd->buf.revision()) {
            ScriptDoc* md = ws_.doc(ws_.active());
            fs_.refresh(md->buf, false);
            find_sync(false);
        }
    }
    debug_tick();
    if (visible) assist_tick();
    // The header names the active script and the list puts it first: a tab switch (Ctrl+Tab has no mouse event) must rebuild too.
    if (diag_drawer && (changed || diag_html_.empty() || diag_active_ != ws_.active())) build_diagnostics();
    if (drawer == "refs" && refs_html_.empty()) build_references();
    if (drawer != "debugger") { dbg_html_.clear(); dbg_head_html_.clear(); } // another tab replaced the body
    if (drawer == "debugger" && debug_view()) {
        const DebugSession* ds = ctl_.play().debug();
        std::uint64_t h = mix(ds->stop_serial(), static_cast<std::uint64_t>(ds->frame() + 7));
        h = mix(h, ds->stopped() ? 3 : 1);
        h = mix(h, ds->frames().size() * 131 + ds->locals().size());
        for (const WatchItem& w : ds->watches()) h = mix(h, std::hash<std::string>{}(w.expr + w.result) + (w.pending ? 1 : 0));
        for (const DebugVar& v : ds->locals()) h = mix(h, std::hash<std::string>{}(v.name + v.value) + (v.open ? 1 : 0));
        h = mix(h, ws_.breakpoint_revision());
        if (h != dbg_sig_ || dbg_html_.empty()) { dbg_sig_ = h; build_debugger(); }
    }
}

// ---- autocomplete, hover, signature help

void ScriptPanel::assist_changed() {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return;
    if (comp_.active()) {
        if (!comp_.still_valid(d->buf)) comp_.close();
        else { comp_pending_ = true; comp_due_ms_ = SDL_GetTicks() + 60; }
    }
    if (sig_open_) { sig_pending_ = true; sig_due_ms_ = SDL_GetTicks() + 60; }
    hide_hover();
}

void ScriptPanel::trigger_completion(int min_prefix) {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return;
    comp_pending_ = false;
    hide_hover();
    comp_.query(ws_, *d, min_prefix);
    ac_first_ = 0;
    render_completion();
}

bool ScriptPanel::accept_completion() {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr || !comp_.accept(d->buf)) return false;
    comp_pending_ = false;
    if (view_ != nullptr) { view_->edit_done(); view_->Focus(); }
    render_completion();
    return true;
}

bool ScriptPanel::assist_key(Rml::Input::KeyIdentifier k, bool ctrl, bool shift, bool alt) {
    using K = Rml::Input::KeyIdentifier;
    hide_hover();
    if (ctrl && !shift && !alt && k == K::KI_SPACE) { trigger_completion(0); return true; }
    if (!ctrl && !shift && !alt && k == K::KI_F12) { goto_definition(); return true; }
    if (!ctrl && shift && !alt && k == K::KI_F12) { find_references(); return true; }
    if (!ctrl && !shift && !alt && k == K::KI_F2) { open_rename(); return true; }
    if (k == K::KI_ESCAPE && (comp_.active() || sig_open_) && !find_open_) {
        if (comp_.active()) comp_.close(); else { sig_open_ = false; sig_pending_ = false; }
        render_completion();
        render_signature();
        return true;
    }
    if (!comp_.active() || ctrl || alt) return false;
    switch (k) {
    case K::KI_UP: comp_.move(-1); render_completion(); return true;
    case K::KI_DOWN: comp_.move(1); render_completion(); return true;
    case K::KI_PRIOR: comp_.move(-8); render_completion(); return true;
    case K::KI_NEXT: comp_.move(8); render_completion(); return true;
    case K::KI_TAB: case K::KI_RETURN: case K::KI_NUMPADENTER: return !shift && accept_completion();
    default: return false;
    }
}

void ScriptPanel::place(Rml::Element* e, Rml::Vector2f top, Rml::Vector2f bottom, float est_h, bool above) {
    Rml::Element* sv = by_id("sview");
    if (e == nullptr || sv == nullptr) return;
    const Rml::Vector2f o = sv->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f sz = sv->GetBox().GetSize(Rml::BoxArea::Border);
    const float w = e->GetBox().GetSize(Rml::BoxArea::Border).x > 0 ? e->GetBox().GetSize(Rml::BoxArea::Border).x : 320.0f;
    const float x = std::clamp(top.x - o.x, 0.0f, std::max(0.0f, sz.x - w - 4.0f));
    const bool below = !above && bottom.y - o.y + est_h < sz.y;
    e->SetProperty("left", std::to_string(static_cast<int>(x)) + "px");
    if (below) { e->SetProperty("top", std::to_string(static_cast<int>(bottom.y - o.y + 2)) + "px"); e->SetProperty("bottom", "auto"); }
    else { e->SetProperty("bottom", std::to_string(static_cast<int>(sz.y - (top.y - o.y) + 2)) + "px"); e->SetProperty("top", "auto"); }
}

void ScriptPanel::render_completion() {
    Rml::Element* pop = by_id("acpop");
    if (pop == nullptr) return;
    if (!comp_.active() || view_ == nullptr) {
        if (pop->IsClassSet("open")) pop->SetClass("open", false);
        ac_html_.clear();
        return;
    }
    constexpr int kRows = 8;
    const int n = static_cast<int>(comp_.rows.size());
    if (comp_.sel < ac_first_) ac_first_ = comp_.sel;
    if (comp_.sel >= ac_first_ + kRows) ac_first_ = comp_.sel - kRows + 1;
    ac_first_ = std::clamp(ac_first_, 0, std::max(0, n - kRows));
    std::string h;
    for (int i = ac_first_; i < std::min(n, ac_first_ + kRows); ++i) {
        const CompletionRow& r = comp_.rows[static_cast<std::size_t>(i)];
        h += std::format("<div class=\"ac-it{}\" data-sc=\"acpick\" data-i=\"{}\"><span class=\"k\">{}</span><span class=\"lbl\">{}</span><span class=\"sig\">{}</span></div>", i == comp_.sel ? " sel" : "", i, r.letter, escape_rml(r.label), escape_rml(r.detail));
    }
    const CompletionRow& cur = comp_.rows[static_cast<std::size_t>(comp_.sel)];
    std::string doc = cur.doc.substr(0, cur.doc.find('\n'));
    if (!doc.empty() || n > kRows) h += std::format("<div class=\"ac-ft\"><span class=\"nm\">{}</span> {}{}</div>", escape_rml(cur.label), escape_rml(doc), n > kRows ? std::format(" ({}/{})", comp_.sel + 1, n) : std::string());
    if (h != ac_html_) { ac_html_ = h; pop->SetInnerRML(h); }
    pop->SetClass("open", true);
    const Rml::Rectanglef c = view_->caret_rect();
    place(pop, {c.Left(), c.Top()}, {c.Left(), c.Bottom()}, static_cast<float>(std::min(n, kRows) * 22 + 40));
}

void ScriptPanel::render_signature() {
    Rml::Element* card = by_id("sigcard");
    if (card == nullptr) return;
    ScriptDoc* d = ws_.doc(ws_.active());
    SignatureView v;
    if (sig_open_ && d != nullptr && view_ != nullptr) {
        ws_.sync(*d);
        const TextPos caret = d->buf.primary().head;
        if (auto h = ws_.server().signature_help(ws_.uri(*d), ws_.to_lsp(*d, caret))) v = signature_view(*h);
        if (!v.ok) sig_open_ = false;
    }
    if (!v.ok || comp_.active()) { card->SetClass("open", false); sig_html_.clear(); return; }
    std::string h = escape_rml(v.before) + "<span class=\"cur\">" + escape_rml(v.active) + "</span>" + escape_rml(v.after);
    if (v.count > 1) h += std::format(" <span class=\"sd\">{}/{}</span>", v.index + 1, v.count);
    if (!v.doc.empty()) h += "<div class=\"sd\">" + escape_rml(v.doc.substr(0, v.doc.find('\n'))) + "</div>";
    if (h != sig_html_) { sig_html_ = h; card->SetInnerRML(h); }
    card->SetClass("open", true);
    const Rml::Rectanglef c = view_->caret_rect();
    // Above the caret line so it never covers the text being typed.
    place(card, {c.Left(), c.Top()}, {c.Left(), c.Bottom()}, 40.0f, true);
}

void ScriptPanel::hide_hover() {
    hover_shown_ = false;
    hover_tried_ = false;
    hover_pos_ = {-1, 0};
    hover_word_ = {};
    if (Rml::Element* c = by_id("hovercard")) if (c->IsClassSet("open")) c->SetClass("open", false);
    hov_html_.clear();
}

void ScriptPanel::show_hover() {
    ScriptDoc* d = ws_.doc(ws_.active());
    Rml::Element* card = by_id("hovercard");
    if (d == nullptr || card == nullptr || hover_pos_.line < 0 || hover_pos_.line >= d->buf.line_count()) return;
    ws_.sync(*d);
    std::string h;
    for (const Problem& p : d->problems) {
        if (!(p.range.start <= hover_pos_ && hover_pos_ < p.range.end)) continue;
        h += std::format("<div class=\"dg\"><span class=\"{}\">{}</span> {}</div>", p.severity == ProblemSeverity::Error ? "er" : "wr", p.severity == ProblemSeverity::Error ? "Error" : p.severity == ProblemSeverity::Warning ? "Warning" : "Note", escape_rml(p.message));
    }
    if (auto info = ws_.server().hover(ws_.uri(*d), ws_.to_lsp(*d, hover_pos_))) h = markdown_to_rml(info->contents) + h;
    if (h.empty()) return;
    hover_shown_ = true;
    if (h != hov_html_) { hov_html_ = h; card->SetInnerRML(h); }
    card->SetClass("open", true);
    place(card, hover_at_, {hover_at_.x, hover_at_.y + 14.0f}, 90.0f);
}

void ScriptPanel::assist_tick() {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) {
        if (comp_.active()) { comp_.close(); render_completion(); }
        sig_open_ = false; render_signature(); hide_hover();
        return;
    }
    if (comp_pending_ && now_ms_ >= comp_due_ms_) {
        comp_pending_ = false;
        comp_.query(ws_, *d, 2);
        render_completion();
        render_signature();
    } else if (comp_.active()) render_completion(); // follows the caret while scrolling
    if (sig_pending_ && now_ms_ >= sig_due_ms_) {
        sig_pending_ = false;
        sig_open_ = true;
        render_signature();
    } else if (sig_open_) render_signature();
    if (hover_pos_.line >= 0 && !hover_shown_ && !hover_tried_ && !comp_.active() && now_ms_ >= hover_since_ms_ + 350) { hover_tried_ = true; show_hover(); }
}

void ScriptPanel::build_diagnostics() {
    Rml::Element* body = by_id("drawer-body");
    Rml::Element* head = by_id("drawer-head");
    if (body == nullptr || head == nullptr) return;
    int errs = 0, warns = 0, lint = 0;
    for (const auto& d : ws_.tabs()) {
        errs += ws_.error_count(*d); warns += ws_.warning_count(*d);
        for (const Problem& p : d->problems) if (!p.code.empty()) ++lint;
    }
    diag_active_ = ws_.active();
    const ScriptDoc* act = ws_.doc(ws_.active());
    std::string h = std::format("<span class=\"dh-note\">{}</span><span class=\"dh-title\">Diagnostics</span><div class=\"chips\"><div class=\"chip{}\" data-sc=\"sev\" data-v=\"0\"><span>All {}</span></div><div class=\"chip{}\" data-sc=\"sev\" data-v=\"1\"><span>Errors {}</span></div><div class=\"chip{}\" data-sc=\"sev\" data-v=\"2\"><span>Warnings {}</span></div><div class=\"chip{}\" data-sc=\"sev\" data-v=\"3\"><span>Lint {}</span></div></div>",
                                act != nullptr ? w::esc(act->name()) : std::string(), sev_filter_ == 0 ? " on" : "", errs + warns, sev_filter_ == 1 ? " on" : "", errs, sev_filter_ == 2 ? " on" : "", warns, sev_filter_ == 3 ? " on" : "", lint);
    std::string out;
    int shown = 0;
    // The active script first, then the other open ones.
    std::vector<const ScriptDoc*> order;
    if (act != nullptr) order.push_back(act);
    for (const auto& d : ws_.tabs()) if (d.get() != act) order.push_back(d.get());
    for (const ScriptDoc* d : order) {
        for (const Problem& p : d->problems) {
            if (sev_filter_ == 1 && p.severity != ProblemSeverity::Error) continue;
            if (sev_filter_ == 2 && p.severity != ProblemSeverity::Warning) continue;
            if (sev_filter_ == 3 && p.code.empty()) continue; // lint findings (unused-*, unreachable-code, style)
            const char* letter = p.severity == ProblemSeverity::Error ? "E" : p.severity == ProblemSeverity::Warning ? "W" : "N";
            const char* cls = p.severity == ProblemSeverity::Error ? "s-err" : p.severity == ProblemSeverity::Warning ? "s-warn" : "s-note";
            out += std::format("<div class=\"drow\" data-sc=\"diag\" data-file=\"{}\" data-line=\"{}\" data-col=\"{}\"><span class=\"sev {}\">{}</span><span class=\"dmsg\">{}</span><div class=\"grow\"></div><span class=\"dcode\">{}</span><span class=\"dloc\">{}:{}:{}</span></div>",
                              w::esc(d->rel), p.range.start.line + 1, p.range.start.col + 1, cls, letter, w::esc(p.message), w::esc(p.code), w::esc(d->name()), p.range.start.line + 1, d->buf.to_utf16(p.range.start) + 1);
            ++shown;
        }
    }
    if (shown == 0) out = ws_.tabs().empty() ? "<div class=\"hint\">Open a script to see its diagnostics. Errors and warnings from the compiler show here.</div>" : "<div class=\"hint\">No problems in the open scripts.</div>";
    set_html(head, h, diag_head_html_);
    set_html(body, out, diag_html_);
}

// ---- find references and rename

bool ScriptPanel::find_references() {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return false;
    const TextPos at = d->buf.primary().head;
    const TextRange w = d->buf.word_at(at);
    refs_name_ = w.empty() ? std::string() : d->buf.text_in(w);
    refs_ = ws_.references(d->id, at);
    refs_html_.clear();
    refs_head_html_.clear();
    if (refs_.empty()) { ctl_.notify_ok("No references found"); return false; }
    if (host_.show_drawer) host_.show_drawer("refs");
    build_references();
    return true;
}

void ScriptPanel::build_references() {
    Rml::Element* body = by_id("drawer-body");
    Rml::Element* head = by_id("drawer-head");
    if (body == nullptr || head == nullptr) return;
    std::set<std::string> files;
    for (const RefHit& r : refs_) files.insert(r.rel);
    const std::string h = std::format("<span class=\"dh-title\">References</span><span class=\"dh-note\">{} in {} file{}{}</span>",
                                      refs_.size(), files.size(), files.size() == 1 ? "" : "s", refs_name_.empty() ? std::string() : " for " + w::esc(refs_name_));
    std::string out;
    if (refs_.empty()) out = "<div class=\"hint\">Put the caret on a name and press Shift+F12 to list where it is used.</div>";
    for (const RefHit& r : refs_) {
        out += std::format("<div class=\"drow\" data-sc=\"diag\" data-file=\"{}\" data-line=\"{}\" data-col=\"{}\"><span class=\"dmsg mono\">{}</span><div class=\"grow\"></div><span class=\"dloc\">{}:{}</span></div>",
                          w::esc(r.rel), r.range.start.line + 1, r.range.start.col + 1, w::esc(r.preview), w::esc(r.rel), r.range.start.line + 1);
    }
    set_html(head, h, refs_head_html_);
    set_html(body, out, refs_html_);
}

bool ScriptPanel::open_rename() {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return false;
    const TextPos at = d->buf.primary().head;
    const auto r = ws_.prepare_rename(d->id, at);
    if (!r) { ctl_.notify_ok("Nothing to rename here"); return false; }
    if (host_.show_script_mode) host_.show_script_mode();
    rename_open_ = true;
    rename_doc_ = d->id;
    rename_at_ = at;
    rename_old_ = d->buf.text_in(*r);
    if (Rml::Element* b = by_id("renamebar")) b->SetClass("open", true);
    if (auto* q = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(by_id("rename-q"))) { q->SetValue(rename_old_); q->Focus(); q->Select(); }
    rename_preview(rename_old_);
    return true;
}

void ScriptPanel::close_rename() {
    rename_open_ = false;
    rename_plan_.reset();
    if (Rml::Element* b = by_id("renamebar")) b->SetClass("open", false);
    if (view_ != nullptr) view_->Focus();
}

void ScriptPanel::rename_preview(const std::string& name) {
    rename_plan_.reset();
    std::string msg;
    if (name.empty() || name == rename_old_) msg = "Type the new name";
    else if (auto plan = ws_.plan_rename(rename_doc_, rename_at_, name)) {
        rename_plan_ = std::move(plan);
        msg = std::format("{} change{} in {} file{}", rename_plan_->edits, rename_plan_->edits == 1 ? "" : "s", rename_plan_->files, rename_plan_->files == 1 ? "" : "s");
        if (rename_plan_->unsaved > 0) msg += std::format(", {} with unsaved edits", rename_plan_->unsaved);
    } else msg = "Not a valid name here";
    if (Rml::Element* c = by_id("rename-cnt")) c->SetInnerRML(w::esc(msg));
    if (Rml::Element* b = by_id("rename-go")) b->SetClass("off", !rename_plan_.has_value());
}

bool ScriptPanel::apply_rename() {
    if (!rename_open_ || !rename_plan_) return false;
    std::string err;
    const RenamePlan plan = std::move(*rename_plan_);
    const std::vector<int> docs = ws_.apply_edit(plan.edit, err);
    close_rename();
    if (docs.empty()) { ctl_.notify_ok(err.empty() ? "Rename changed nothing" : err); return false; }
    shown_doc_ = -2;
    tabs_html_.clear();
    refs_.clear();
    refs_html_.clear();
    ctl_.notify_ok(std::format("Renamed {} place{} in {} file{}. Save to keep it; Ctrl+Z undoes it per file.", plan.edits, plan.edits == 1 ? "" : "s", plan.files, plan.files == 1 ? "" : "s"));
    return true;
}

void ScriptPanel::save(int id) {
    ScriptDoc* d = ws_.doc(id);
    if (d == nullptr) return;
    std::string err;
    if (!ws_.save(id, err)) { ctl_.notify_ok(err); return; }
    ws_.check_now(id);
    const int errors = ws_.error_count(*d);
    if (errors > 0) {
        // The previous build keeps running; the errors are in Diagnostics.
        ctl_.notify_ok(std::format("Saved {} with {} error{}: the last good build stays active.", d->name(), errors, errors == 1 ? "" : "s"));
        if (host_.show_diagnostics) host_.show_diagnostics();
    } else {
        // Playing sessions hot-reload by themselves; otherwise rebuild the class registry and previews.
        if (!ctl_.play().active()) ctl_.reload_project_scripts();
        ctl_.notify_ok(std::format("Saved {}, hot reload ok.", d->name()));
    }
    diag_html_.clear();
    status_sig_ = 0;
}

bool ScriptPanel::save_active() {
    if (ws_.active() < 0) return false;
    save(ws_.active());
    return true;
}

void ScriptPanel::save_all() {
    std::vector<int> ids;
    for (const auto& d : ws_.tabs()) if (d->buf.dirty()) ids.push_back(d->id);
    for (int id : ids) save(id);
    if (ids.empty()) ctl_.notify_ok("No unsaved scripts.");
}

void ScriptPanel::close_tab(int id, bool force) {
    ScriptDoc* d = ws_.doc(id);
    if (d == nullptr) return;
    if (d->buf.dirty() && !force) { closing_ = id; ws_.activate(id); return; }
    ws_.close(id);
    closing_ = 0;
    shown_doc_ = -2;
    side_sig_ = 0;
}

void ScriptPanel::create_script(const std::string& typed) {
    std::string rel = typed;
    while (!rel.empty() && rel.front() == ' ') rel.erase(rel.begin());
    if (rel.empty()) { adding_ = false; return; }
    if (rel.find('/') == std::string::npos) rel = "scripts/" + rel;
    if (!rel.ends_with(".plm")) rel += ".plm";
    std::string base = "Node2D";
    if (const ModelNode* n = ctl_.doc().model().get(ctl_.doc().selection())) if (ctl_.env().classes().find(n->type) != nullptr) base = n->type;
    std::string err;
    const int id = ws_.new_script(rel, pascal(fs::path(rel).stem().string()), base, err);
    if (id < 0) { ctl_.notify_ok(err); return; }
    adding_ = false;
    ctl_.assets().open(ctl_.env().project_dir());
    shown_doc_ = -2;
    side_sig_ = 0;
}

void ScriptPanel::jump(const std::string& rel, int line, int col) { open_file(project_ / rel, line, col); }

bool ScriptPanel::handle_key(const SDL_Event& ev) {
    if (!(host_.script_mode && host_.script_mode())) return false;
    if (ev.type == SDL_EVENT_KEY_UP) {
        if (ctrl_tab_ && (ev.key.key == SDLK_LCTRL || ev.key.key == SDLK_RCTRL)) { ctrl_tab_ = false; ws_.activate(ws_.active()); }
        return false;
    }
    const bool ctrl = (ev.key.mod & SDL_KMOD_CTRL) != 0, shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0, alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
    if (!ctrl && !alt && ev.key.key == SDLK_F3) { if (find_open_ || !fs_.query.empty()) { if (!find_open_) open_find(false); find_step(shift); } return true; }
    if (!ctrl && !alt && !shift && ev.key.key == SDLK_ESCAPE && rename_open_) { close_rename(); return true; }
    if (!ctrl && !alt && !shift && ev.key.key == SDLK_ESCAPE && find_open_) { close_find(); return true; }
    if (!ctrl || alt) return false;
    switch (ev.key.key) {
    case SDLK_S: if (shift) save_all(); else save_active(); return true;
    case SDLK_W: if (ws_.active() >= 0) close_tab(ws_.active(), false); return true;
    case SDLK_F: if (ws_.active() >= 0) open_find(false); return true;
    case SDLK_H: if (ws_.active() >= 0) open_find(true); return true;
    case SDLK_TAB:
        if (ws_.tabs().size() > 1) { ctrl_tab_ = true; ws_.cycle(shift); shown_doc_ = -2; if (view_ != nullptr) view_->Focus(); }
        return true;
    default: return false;
    }
}

// ---- find and replace

void ScriptPanel::find_ui() {
    if (Rml::Element* b = by_id("findbar")) {
        b->SetClass("open", find_open_);
        b->SetClass("rep", find_open_ && fr_open_);
        Rml::ElementList items;
        b->GetElementsByTagName(items, "span");
        for (Rml::Element* it : items) {
            const std::string v = it->GetAttribute<Rml::String>("data-v", "");
            if (v == "case") it->SetClass("on", fs_.opt.match_case);
            else if (v == "word") it->SetClass("on", fs_.opt.whole_word);
            else if (v == "regex") it->SetClass("on", fs_.opt.regex);
        }
    }
    if (Rml::Element* c = by_id("find-cnt")) {
        std::string txt;
        bool bad = false;
        const FindResult& r = fs_.result;
        if (!r.error.empty()) { txt = "Invalid"; bad = true; }
        else if (!fs_.message.empty()) txt = fs_.message;
        else if (fs_.query.empty()) txt = "";
        else if (r.ranges.empty()) { txt = r.limited ? "Too large" : "No results"; bad = true; }
        else {
            const std::string n = std::to_string(r.ranges.size()) + (r.truncated ? "+" : "");
            txt = fs_.current >= 0 ? std::to_string(fs_.current + 1) + " of " + n : n;
        }
        c->SetInnerRML(txt);
        c->SetClass("bad", bad);
        c->SetAttribute("title", r.error);
    }
}

// Pushes the session to the view and the bar.
void ScriptPanel::find_sync(bool reveal) {
    ScriptDoc* d = ws_.doc(ws_.active());
    fdoc_ = ws_.active();
    if (view_ != nullptr) {
        view_->set_matches(find_open_ ? fs_.result.ranges : std::vector<TextRange>{}, fs_.current);
        if (reveal && d != nullptr && fs_.current >= 0) view_->reveal(fs_.result.ranges[static_cast<std::size_t>(fs_.current)].start, true);
    }
    find_ui();
}

void ScriptPanel::set_find(const std::string& query, const FindOptions& opt) {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return;
    fs_.query = query;
    fs_.opt = opt;
    fs_.message.clear();
    fs_.refresh(d->buf, true);
    find_sync(true);
}

void ScriptPanel::open_find(bool with_replace) {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return;
    if (host_.show_script_mode) host_.show_script_mode();
    find_open_ = true;
    fr_open_ = with_replace || fr_open_;
    if (d->buf.has_selection()) {
        const Selection& s = d->buf.primary();
        if (s.start().line == s.end().line) fs_.query = d->buf.selected_text();
    } else if (fs_.query.empty()) {
        const TextRange w = d->buf.word_at(d->buf.primary().head);
        if (!w.empty()) fs_.query = d->buf.text_in(w);
    }
    if (auto* q = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(by_id("find-q"))) { q->SetValue(fs_.query); q->Focus(); q->Select(); }
    if (auto* r = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(by_id("find-r"))) r->SetValue(fs_.replacement);
    fs_.message.clear();
    fs_.refresh(d->buf, false);
    find_sync(false);
}

void ScriptPanel::close_find() {
    find_open_ = false;
    fr_open_ = false;
    if (view_ != nullptr) view_->Focus();
    find_sync(false);
}

void ScriptPanel::find_step(bool back) {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return;
    fs_.step(d->buf, back);
    find_sync(true);
}

void ScriptPanel::find_replace_current() {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return;
    fs_.replace_current(d->buf);
    if (view_ != nullptr) view_->edit_done();
    find_sync(true);
}

int ScriptPanel::find_replace_everything() {
    ScriptDoc* d = ws_.doc(ws_.active());
    if (d == nullptr) return 0;
    const int n = fs_.replace_everything(d->buf);
    if (view_ != nullptr) view_->edit_done();
    find_sync(false);
    return n;
}

void ScriptPanel::ProcessEvent(Rml::Event& e) {
    Rml::Element* t = e.GetTargetElement();
    if (e.GetId() == Rml::EventId::Keydown && t != nullptr) {
        const auto key = static_cast<Rml::Input::KeyIdentifier>(e.GetParameter<int>("key_identifier", 0));
        const bool shift = e.GetParameter("shift_key", false);
        if (t->GetId() == "find-q" && key == Rml::Input::KI_RETURN) { find_step(shift); e.StopPropagation(); }
        else if (t->GetId() == "find-r" && key == Rml::Input::KI_RETURN) { find_replace_current(); e.StopPropagation(); }
        else if (t->GetId() == "rename-q" && key == Rml::Input::KI_RETURN) { apply_rename(); e.StopPropagation(); }
        return;
    }
    if (e.GetId() == Rml::EventId::Change && t != nullptr) {
        if (t->GetId() == "find-q") { set_find(value_of(e), fs_.opt); return; }
        if (t->GetId() == "find-r") { fs_.replacement = value_of(e); return; }
        if (t->GetId() == "watch-add") {
            if (e.GetParameter("linebreak", false)) {
                if (DebugSession* d = ctl_.play().debug()) d->add_watch(value_of(e));
                if (auto* in = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(t)) in->SetValue("");
                dbg_html_.clear();
            }
            return;
        }
        if (t->GetId() == "rename-q") { rename_preview(value_of(e)); return; }
        const Rml::String value = e.GetParameter("value", Rml::String());
        if (t->GetId() == "script-filter") { filter_ = value; side_sig_ = 0; }
        else if (t->GetId() == "script-new" && e.GetParameter("linebreak", false)) create_script(value);
        return;
    }
    if (e.GetId() != Rml::EventId::Click || t == nullptr) return;
    Rml::Element* a = find_attr(t, "data-sc");
    if (a == nullptr) return;
    const std::string act = a->GetAttribute<Rml::String>("data-sc", "");
    const int id = a->GetAttribute<int>("data-id", 0);
    if (act == "tab") { ws_.activate(id); shown_doc_ = -2; if (view_ != nullptr) view_->Focus(); }
    else if (act == "close") close_tab(id, false);
    else if (act == "close-save") { const int c = closing_; save(c); close_tab(c, true); }
    else if (act == "close-discard") close_tab(closing_, true);
    else if (act == "close-cancel") closing_ = 0;
    else if (act == "save-all") save_all();
    else if (act == "fopt") {
        const std::string v = a->GetAttribute<Rml::String>("data-v", "");
        FindOptions o = fs_.opt;
        bool& f = v == "case" ? o.match_case : v == "word" ? o.whole_word : o.regex;
        f = !f;
        set_find(fs_.query, o);
    }
    else if (act == "fnext") find_step(false);
    else if (act == "fprev") find_step(true);
    else if (act == "fclose") close_find();
    else if (act == "frep") find_replace_current();
    else if (act == "frepall") find_replace_everything();
    else if (act == "acpick") {
        comp_.sel = a->GetAttribute<int>("data-i", 0);
        accept_completion();
    }
    else if (act == "new") { adding_ = true; side_sig_ = 0; if (host_.show_script_mode) host_.show_script_mode(); }
    else if (act == "open") open_file(project_ / a->GetAttribute<Rml::String>("data-file", ""));
    else if (act == "outline") { if (view_ != nullptr && ws_.doc(ws_.active()) != nullptr) { view_->goto_pos(ws_.doc(ws_.active())->buf.clamp({a->GetAttribute<int>("data-line", 1) - 1, a->GetAttribute<int>("data-col", 1) - 1}), true); view_->Focus(); } }
    else if (act == "diag") jump(a->GetAttribute<Rml::String>("data-file", ""), a->GetAttribute<int>("data-line", 1), a->GetAttribute<int>("data-col", 1));
    else if (act == "dbgtab") { dbg_nodes_ = a->GetAttribute<int>("data-v", 0) == 1; dbg_html_.clear(); dbg_head_html_.clear(); if (host_.show_drawer) host_.show_drawer("debugger"); }
    else if (act == "dbg-continue") { if (DebugSession* d = ctl_.play().debug()) d->resume(); }
    else if (act == "dbg-over") { if (DebugSession* d = ctl_.play().debug()) d->step_over(); }
    else if (act == "dbg-in") { if (DebugSession* d = ctl_.play().debug()) d->step_in(); }
    else if (act == "dbg-out") { if (DebugSession* d = ctl_.play().debug()) d->step_out(); }
    else if (act == "dbg-stop") ctl_.stop_play();
    else if (act == "frame") { if (DebugSession* d = ctl_.play().debug()) d->select_frame(a->GetAttribute<int>("data-i", 0)); }
    else if (act == "var") { if (DebugSession* d = ctl_.play().debug()) d->toggle_var(a->GetAttribute<int>("data-i", 0)); }
    else if (act == "watch-del") { if (DebugSession* d = ctl_.play().debug()) d->remove_watch(a->GetAttribute<int>("data-i", 0)); }
    else if (act == "bp-del") {
        ws_.toggle_breakpoint(a->GetAttribute<Rml::String>("data-file", ""), a->GetAttribute<int>("data-line", 0));
        save_breakpoints();
    }
    else if (act == "rename-apply") apply_rename();
    else if (act == "rename-close") close_rename();
    else if (act == "sev") { sev_filter_ = a->GetAttribute<int>("data-v", 0); diag_html_.clear(); }
    tabs_html_.clear();
    diag_sig_ = 0;
}

} // namespace cuervo::editor

namespace cuervo::editor {

std::string ScriptPanel::debug_chips() const {
    if (!debug_session()) return {};
    return std::format("<div class=\"chips\"><div class=\"chip{}\" data-sc=\"dbgtab\" data-v=\"0\"><span>Script</span></div><div class=\"chip{}\" data-sc=\"dbgtab\" data-v=\"1\"><span>Nodes</span></div></div>", dbg_nodes_ ? "" : " on", dbg_nodes_ ? " on" : "");
}

void ScriptPanel::build_debugger() {
    Rml::Element* body = by_id("drawer-body");
    Rml::Element* head = by_id("drawer-head");
    const DebugSession* ds = ctl_.play().debug();
    if (body == nullptr || head == nullptr || ds == nullptr) return;
    // Head: state tag, location, then the run controls (disabled look while the game runs).
    const bool paused = ds->stopped();
    std::string where;
    if (paused && !ds->frames().empty()) {
        const DebugFrame& f = ds->frames()[static_cast<std::size_t>(ds->frame())];
        where = std::format("{}:{} in {}", f.rel.empty() ? std::string("built-in") : f.rel, f.line, f.name);
    }
    const char* off = paused ? "" : " off";
    std::string h = std::format("<span class=\"dh-tag{}\">{}</span><span class=\"dh-title\">Debugger</span><span class=\"dh-note\">{}</span>{}<div class=\"grow\"></div>",
                                paused ? " paused" : "", paused ? "paused" : "running", w::esc(paused ? where : std::string("breakpoints are active")), debug_chips());
    h += std::format("<div class=\"btn sm{0}\" data-sc=\"dbg-continue\" title=\"Continue (F5)\">{1}<span>Continue</span></div><div class=\"btn sm{0}\" data-sc=\"dbg-over\" title=\"Step over (F10)\">{2}<span>Over</span></div><div class=\"btn sm{0}\" data-sc=\"dbg-in\" title=\"Step into (F11)\"><span>In</span></div><div class=\"btn sm{0}\" data-sc=\"dbg-out\" title=\"Step out (Shift+F11)\"><span>Out</span></div><div class=\"btn sm\" data-sc=\"dbg-stop\" title=\"Stop the game (Shift+F5)\">{3}<span>Stop</span></div>",
                     off, w::icon("play", 12), w::icon("step", 12), w::icon("stop", 12));
    std::string out = "<div class=\"dcols\">";
    // Call stack
    out += "<div class=\"dcol\"><div class=\"dch\">Call stack</div>";
    if (!paused) out += "<div class=\"hint\">Runs until a breakpoint is hit.</div>";
    for (std::size_t i = 0; i < ds->frames().size(); ++i) {
        const DebugFrame& f = ds->frames()[i];
        out += std::format("<div class=\"drow{}\" data-sc=\"frame\" data-i=\"{}\"><span class=\"dmsg mono\">{}</span><div class=\"grow\"></div><span class=\"dloc\">{}</span></div>",
                           static_cast<int>(i) == ds->frame() ? " selected" : "", i, w::esc(f.name), f.rel.empty() ? std::string("built-in") : w::esc(f.rel) + ":" + std::to_string(f.line));
    }
    out += "</div><div class=\"dcol\"><div class=\"dch\">Locals</div>";
    for (std::size_t i = 0; i < ds->locals().size(); ++i) {
        const DebugVar& v = ds->locals()[i];
        out += std::format("<div class=\"drow\" data-sc=\"var\" data-i=\"{}\" style=\"padding-left: {}dp\"><span class=\"twist\">{}</span><span class=\"dmsg mono\">{}</span><div class=\"grow\"></div><span class=\"dloc\">{}</span></div>",
                           i, 8 + v.depth * 12, v.ref > 0 ? (v.open ? "v" : ">") : " ", w::esc(v.name), w::esc(v.value));
    }
    out += "</div><div class=\"dcol\"><div class=\"dch\">Watch</div>";
    for (std::size_t i = 0; i < ds->watches().size(); ++i) {
        const WatchItem& wi = ds->watches()[i];
        out += std::format("<div class=\"drow\"><span class=\"dmsg mono\">{}</span><div class=\"grow\"></div><span class=\"dloc{}\">{}</span><span class=\"fx\" data-sc=\"watch-del\" data-i=\"{}\" title=\"Remove\">{}</span></div>",
                           w::esc(wi.expr), wi.ok ? "" : " bad", wi.pending ? "..." : w::esc(paused ? wi.result : std::string("-")), i, w::icon("close", 10));
    }
    out += "<div class=\"drow\"><input id=\"watch-add\" type=\"text\" placeholder=\"Add expression\"/></div></div><div class=\"dcol\"><div class=\"dch\">Breakpoints</div>";
    int nbp = 0;
    for (const auto& [file, lines] : ws_.breakpoints()) {
        for (const int l : lines) {
            const bool here = paused && !ds->frames().empty() && ds->frames()[static_cast<std::size_t>(ds->frame())].rel == file && ds->frames()[static_cast<std::size_t>(ds->frame())].line == l;
            out += std::format("<div class=\"drow\" data-sc=\"diag\" data-file=\"{0}\" data-line=\"{1}\" data-col=\"1\"><span class=\"bpdot\">{3}</span><span class=\"dmsg mono\">{5}:{1}</span><div class=\"grow\"></div><span class=\"dloc\">{2}</span><span class=\"fx\" data-sc=\"bp-del\" data-file=\"{0}\" data-line=\"{1}\" title=\"Remove breakpoint\">{4}</span></div>",
                               w::esc(file), l, here ? "hit" : "on", here ? ">" : "o", w::icon("close", 10), w::esc(fs::path(file).filename().string()));
            ++nbp;
        }
    }
    if (nbp == 0) out += "<div class=\"hint\">Click the gutter next to a line to set one.</div>";
    out += "</div></div>";
    set_html(head, h, dbg_head_html_);
    // The watch field keeps its focus: only replace the body when something changed.
    if (dbg_html_ != out) {
        const bool typing = by_id("watch-add") != nullptr && by_id("watch-add") == page_->GetContext()->GetFocusElement();
        if (!typing || ds->stopped()) set_html(body, out, dbg_html_);
    }
    body->SetClass("mono", false);
}

} // namespace cuervo::editor

// Drawer tabs for Replay, Tier report and Input map (the ImGui panels replay_panel / tier_panel /
// input_map_panel, same engine calls). Rows are rebuilt only when what they show changes, and never
// while a field in the drawer has the keyboard.
#include <SDL3/SDL_clipboard.h>

#include <algorithm>
#include <format>

#include "cuervo/core/log.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/tier_check.hpp"
#include "cuervo/platform/platform.hpp"
#include "editor/icons.hpp"
#include "editor/input_map_doc.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"

namespace cuervo::editor {
using dom::find_attr;
using dom::typing;

namespace {

std::string btn(const char* label, const char* attr, const std::string& value, const char* tip, bool on = false, const char* icon = nullptr, bool off = false, const std::string& extra = {}) {
    return std::format("<div class=\"btn sm{}{}\" {}=\"{}\" {} title=\"{}\">{}<span>{}</span></div>", on ? " on" : "", off ? " off" : "", attr, value, extra, w::esc(tip), icon != nullptr ? w::icon(icon, 12) : std::string(), w::esc(label));
}

std::string status(const char* icon, const std::string& text, const char* cls) {
    return std::format("<div class=\"stat {}\">{}<span>{}</span></div>", cls, w::icon(icon, 14), w::esc(text));
}

std::string input(const char* id, const std::string& value, int width, const char* placeholder = "", const std::string& extra = {}) {
    return std::format("<span class=\"tf tlin\" style=\"width: {}dp\"><input type=\"text\" id=\"{}\" value=\"{}\" placeholder=\"{}\" {}/></span>", width, id, w::esc(value), w::esc(placeholder), extra);
}

std::string hx(std::uint64_t v) { return std::format("{:016x}", v); }

} // namespace

bool Shell::drawer_input_busy() const { return typing(ctx_); }

// ------------------------------------------------------------------ replay

void Shell::build_replay() {
    Rml::Element* e = by_id("drawer-body");
    if (e == nullptr) return;
    PlayController& play = ctl_.play();
    const bool replaying = play.mode() == PlayMode::Replay && play.active();
    const std::uint64_t length = replaying ? play.replay_length() : 0;
    const std::uint64_t tick = replaying ? (rp_scrub_ ? rp_pos_ : play.tick()) : 0;
    // What the panel shows, as one key: skip the rebuild when it is the same.
    const std::string key = std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}", replaying, play.recording(), play.recording() ? play.recorded().ticks() : play.recorded().ticks(), tick, replaying ? static_cast<int>(play.state()) : 0, play.speed, play.verified(), play.first_mismatch().value_or(~0ull), ctl_.assets().revision());
    if (key == rp_key_ && !rp_dirty_) return;
    if (drawer_input_busy()) return;
    rp_key_ = key;
    rp_dirty_ = false;

    set_drawer_head(std::format("<span class=\"dh-title\">Replay</span><span class=\"dh-note\">{}</span>", replaying ? "playing back" : play.recording() ? "recording" : "record and play back inputs"));
    std::string out = "<div class=\"tl\">";
    if (!replaying) {
        out += "<div class=\"tlbar\">";
        if (play.recording()) {
            out += std::format("<span class=\"rec\">{}<span>REC {} ticks</span></span>{}", w::icon("record", 12), play.recorded().ticks(), btn("Stop recording", "data-rp", "stoprec", "Stop recording"));
        } else {
            out += btn("Play and record", "data-rp", "rec", "Play the project and record its inputs", false, "play", play.active());
            out += btn("Play scene and record", "data-rp", "rec-scene", "Play the open scene and record its inputs", false, "play", play.active());
            const std::uint64_t n = play.recorded().ticks();
            out += std::format("<span class=\"tlf\">{}</span>", n > 0 ? std::format("recorded: {} ticks", n) : std::string("nothing recorded"));
        }
        out += "</div><div class=\"tlbar\">";
        out += input("rp-path", rp_path_, 240, "replays/last.replay");
        out += btn("Save recording", "data-rp", "save", "Write the recording to this path", false, "save", play.recorded().frames.empty());
        out += btn("Load", "data-rp", "load", "Play this replay file back in the scene view", false, "open");
        out += "</div>";
        const std::vector<std::string> files = ctl_.assets().files_of(AssetKind::Replay);
        out += "<div class=\"tlbody scroll\">";
        if (files.empty()) out += "<div class=\"hint\">No replay files in the project yet. Record one, or drop a .replay file in the project folder.</div>";
        for (const std::string& f : files) out += std::format("<div class=\"pr{}\" data-rpfile=\"{}\">{}<span>{}</span></div>", f == rp_path_ ? " sel" : "", w::esc(f), w::icon("history", 14), w::esc(f));
        out += "</div>";
    } else {
        const Replay& r = play.replay();
        out += std::format("<div class=\"tlbar\"><span class=\"mono\">'{}', seed {}, {} ticks{}{}</span></div>", w::esc(r.project), r.seed, length, r.start_scene.empty() ? "" : ", from ", w::esc(r.start_scene));
        if (const auto m = play.first_mismatch()) {
            out += std::format("<div class=\"tlbar\">{}{}</div>", status("warning", std::format("State hash mismatch at tick {}: recorded {}, replayed {}", *m, hx(play.mismatch_expected()), hx(play.mismatch_actual())), "bad"), btn("Seek there", "data-rp", "seekmis", "Jump to the first mismatching tick"));
        } else if (play.verified() > 0) {
            out += std::format("<div class=\"tlbar\">{}</div>", status("check", std::format("{} / {} ticks verified: every state hash matches", play.verified(), std::min<std::uint64_t>(length, r.hashes.size())), "good"));
        }
        const bool playing = play.state() == PlayState::Playing;
        out += "<div class=\"tlbar\">";
        out += btn(playing ? "Pause" : "Play", "data-rp", "toggle", "Pause or resume", playing, playing ? "pause" : "play");
        out += btn("Step", "data-rp", "step", "Advance one tick", false, "step");
        out += btn("Verify all", "data-rp", "verify", "Simulate to the end and compare every hash");
        out += btn(std::format("{}x", play.speed).c_str(), "data-rp", "speed", "Playback speed (click to change)");
        out += btn("Stop", "data-rp", "stop", "Stop the replay", false, "stop");
        out += std::format("<span class=\"tlf mono\">tick {} / {}</span></div>", tick, length);
        const double len = static_cast<double>(std::max<std::uint64_t>(length, 1));
        out += std::format("<div class=\"tlbar\"><div id=\"rp-bar\"><div class=\"rp-ver\" style=\"width: {:.3f}%\"></div>{}<div class=\"rp-pos\" style=\"left: {:.3f}%\"></div></div></div>", static_cast<double>(play.verified()) * 100.0 / len,
                           play.first_mismatch() ? std::format("<div class=\"rp-mis\" style=\"left: {:.3f}%\"></div>", static_cast<double>(*play.first_mismatch()) * 100.0 / len) : std::string(), static_cast<double>(tick) * 100.0 / len);
        const std::uint64_t t = play.tick();
        if (t >= 1 && t <= r.hashes.size()) {
            const bool same = r.hashes[t - 1] == play.state_hash();
            out += std::format("<div class=\"tlbar\"><span class=\"mono\">tick {}  recorded {}  now {}</span>{}</div>", t, hx(r.hashes[t - 1]), hx(play.state_hash()), status(same ? "check" : "warning", same ? "match" : "MISMATCH", same ? "good" : "bad"));
        }
        if (t < r.frames.size()) out += std::format("<div class=\"hint mono\">next input: held {}  pointer ({}, {})</div>", hx(r.frames[t].held), r.frames[t].pointer[0], r.frames[t].pointer[1]);
    }
    out += "</div>";
    e->SetInnerRML(out);
    e->SetClass("mono", false);
}

void Shell::replay_seek(std::uint64_t tick) {
    std::string error;
    if (!ctl_.play().seek(tick, error)) LOG_ERROR("Replay: {}", error);
    rp_dirty_ = true;
}

bool Shell::replay_click(Rml::Element* t) {
    PlayController& play = ctl_.play();
    if (Rml::Element* f = find_attr(t, "data-rpfile")) {
        rp_path_ = f->GetAttribute<Rml::String>("data-rpfile", "");
        rp_dirty_ = true;
        build_replay();
        return true;
    }
    Rml::Element* b = find_attr(t, "data-rp");
    if (b == nullptr) return false;
    const std::string what = b->GetAttribute<Rml::String>("data-rp", "");
    if (b->IsClassSet("off")) return true;
    if (what == "rec") ctl_.start_play(false, true);
    else if (what == "rec-scene") ctl_.start_play(true, true);
    else if (what == "stoprec") play.stop_recording();
    else if (what == "save") {
        std::string error;
        if (save_replay(ctl_.env().resolve(rp_path_), play.recorded(), error)) { ctl_.notify_ok(std::format("Replay of {} ticks saved to '{}'", play.recorded().ticks(), rp_path_)); ctl_.assets().poll(); }
        else ctl_.report_failure(error);
    } else if (what == "load") ctl_.start_replay(rp_path_);
    else if (what == "toggle") { if (play.state() == PlayState::Playing) play.pause(); else play.resume(); }
    else if (what == "step") { play.pause(); DeviceState none; play.step(none, {0.0f, 0.0f}, 1); }
    else if (what == "verify") replay_seek(play.replay_length());
    else if (what == "seekmis") { if (auto m = play.first_mismatch()) replay_seek(*m); }
    else if (what == "speed") {
        const float speeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        std::size_t i = 0;
        for (std::size_t k = 0; k < 6; ++k) if (speeds[k] == play.speed) i = k;
        play.speed = speeds[(i + 1) % 6];
    } else if (what == "stop") ctl_.stop_play();
    else return false;
    rp_dirty_ = true;
    build_replay();
    return true;
}

bool Shell::replay_mousedown(Rml::Element* t, Rml::Vector2f at) {
    if (!dom::inside(t, "rp-bar") || ctl_.play().mode() != PlayMode::Replay) return false;
    rp_scrub_ = true;
    replay_scrub_to(at.x);
    return true;
}

void Shell::replay_scrub_to(float x) {
    Rml::Element* bar = by_id("rp-bar");
    if (bar == nullptr) return;
    const float left = bar->GetAbsoluteOffset(Rml::BoxArea::Content).x;
    const float width = std::max(bar->GetBox().GetSize(Rml::BoxArea::Content).x, 1.0f);
    const std::uint64_t len = ctl_.play().replay_length();
    rp_pos_ = static_cast<std::uint64_t>(std::clamp((x - left) / width, 0.0f, 1.0f) * static_cast<float>(len));
    Rml::ElementList l;
    bar->GetElementsByClassName(l, "rp-pos");
    for (Rml::Element* p : l) p->SetProperty("left", std::format("{:.3f}%", static_cast<double>(rp_pos_) * 100.0 / static_cast<double>(std::max<std::uint64_t>(len, 1))));
}

void Shell::replay_mousemove(Rml::Vector2f at) { if (rp_scrub_) replay_scrub_to(at.x); }

void Shell::replay_mouseup() {
    if (!rp_scrub_) return;
    rp_scrub_ = false;
    replay_seek(rp_pos_); // applied on release, like the ImGui slider
}

// ------------------------------------------------------------------ tier report

void Shell::run_tier() {
    std::optional<rhi::Tier> target;
    if (tier_target_ == 1) target = rhi::Tier::Compatible;
    if (tier_target_ == 2) target = rhi::Tier::Full;
    tier_error_.clear();
    tier_report_ = script::check_project_tier(ctl_.env().project_dir(), target, tier_error_);
    tier_text_ = tier_report_ ? script::format_tier_report(*tier_report_) : tier_error_;
    tier_ran_ = true;
    if (tier_report_) LOG_INFO("Tier report ({}): {} error(s), {} warning(s)", rhi::tier_name(tier_report_->target), tier_report_->count(script::Severity::Error), tier_report_->count(script::Severity::Warning));
}

void Shell::build_tiers() {
    Rml::Element* e = by_id("drawer-body");
    if (e == nullptr) return;
    if (!tier_ran_) run_tier();
    const char* targets[] = {"the project's tier", "compatible", "full"};
    set_drawer_head("<span class=\"dh-title\">Tier report</span><span class=\"dh-note\">what the project needs to run on each render tier</span>");
    std::string out = "<div class=\"tl\"><div class=\"tlbar\">";
    out += btn(std::format("target: {}", targets[tier_target_]).c_str(), "data-tier", "target", "Tier to check against (click to change)");
    out += btn("Run again", "data-tier", "run", "Check the project again", false, "reset");
    out += btn("Copy as text", "data-tier", "copy", "Copy the report", false, "code", !tier_report_);
    if (tier_report_) {
        const script::TierCheckReport& r = *tier_report_;
        const bool ok = r.ok();
        out += status(ok ? "check" : "warning", std::format("{}: {}  ({} errors, {} warnings, {} notes; declared {}; web download {:.1f} MB)", rhi::tier_name(r.target), ok ? "fits" : "blocked", r.count(script::Severity::Error), r.count(script::Severity::Warning), r.count(script::Severity::Note), rhi::tier_name(r.declared), static_cast<double>(r.download_bytes) / (1024.0 * 1024.0)), ok ? "good" : "bad");
    }
    out += "</div>";
    if (!tier_report_) {
        out += std::format("<div class=\"warn\">{}<span>{}</span></div></div>", w::icon("warning", 14), w::esc(tier_error_));
    } else if (tier_report_->issues.empty()) {
        out += "<div class=\"empty\">Nothing to report.</div></div>";
    } else {
        out += "<div class=\"tlbody scroll\"><div class=\"pfr hd\"><span class=\"sev\">severity</span><span class=\"wh\">where</span><span class=\"nm\">message</span></div>";
        for (const script::TierIssue& is : tier_report_->issues) {
            const std::string where = is.file.empty() ? std::string("project") : is.line == 0 ? is.file : is.column == 0 ? std::format("{}:{}", is.file, is.line) : std::format("{}:{}:{}", is.file, is.line, is.column);
            const char* cls = is.severity == script::Severity::Error ? "err" : is.severity == script::Severity::Warning ? "warn" : "note";
            out += std::format("<div class=\"tir {}\" data-loc=\"{}\" data-line=\"{}\" title=\"Open or show {}\"><span class=\"sev\">{}</span><span class=\"wh mono\">{}</span><span class=\"nm\">{}</span></div>", cls, w::esc(is.file.empty() ? std::string("project.toml") : is.file), is.line, w::esc(is.file.empty() ? std::string("project.toml") : is.file), std::string(script::severity_name(is.severity)), w::esc(where), w::esc(is.message));
        }
        out += "</div></div>";
    }
    e->SetInnerRML(out);
    e->SetClass("mono", false);
}

// Scenes open in the editor; text files go to the code editor; everything else is revealed in Assets.
void Shell::open_location(const std::string& file, int line) {
    if (file.ends_with(".scene")) { ctl_.request_open_scene(file); return; }
    if (file.ends_with(".plm")) { open_script(ctl_.env().resolve(file), line); return; }
    assets_sel_ = file;
    show_rail("assets", false);
    build_assets();
    SDL_SetClipboardText(platform::utf8(ctl_.env().resolve(file)).c_str());
    if (file.ends_with(".txt") || file.ends_with(".toml") || file.ends_with(".json")) ctl_.open_in_external_editor(ctl_.env().resolve(file), line);
    ctl_.notify_ok(line > 0 ? std::format("{}:{} (path copied)", file, line) : std::format("{} (path copied)", file));
}

bool Shell::tier_click(Rml::Element* t) {
    if (Rml::Element* l = find_attr(t, "data-loc")) {
        open_location(l->GetAttribute<Rml::String>("data-loc", ""), l->GetAttribute<int>("data-line", 0));
        return true;
    }
    Rml::Element* b = find_attr(t, "data-tier");
    if (b == nullptr) return false;
    const std::string what = b->GetAttribute<Rml::String>("data-tier", "");
    if (what == "target") { tier_target_ = (tier_target_ + 1) % 3; run_tier(); }
    else if (what == "run") run_tier();
    else if (what == "copy" && tier_report_) { SDL_SetClipboardText(tier_text_.c_str()); ctl_.notify_ok("Copied the tier report."); }
    build_tiers();
    return true;
}

// ------------------------------------------------------------------ input map

void Shell::build_inputmap() {
    Rml::Element* e = by_id("drawer-body");
    if (e == nullptr) return;
    InputMapDoc& map = ctl_.input_map();
    set_drawer_head(std::format("<span class=\"dh-title\">Input map</span><span class=\"dh-note\">{}</span>", map.loaded() ? (map.dirty() ? "unsaved changes to project.toml" : "saved") : "no project.toml"));
    if (!map.loaded()) { e->SetInnerRML("<div class=\"empty\">This project has no project.toml.</div>"); return; }
    std::string out = "<div class=\"tl\"><div class=\"tlbar\">";
    out += btn("Save", "data-im", "save", "Write [input.actions] and [input.axes] back into project.toml", map.dirty(), "save", !map.dirty());
    out += btn("Undo", "data-im", "undo", "Undo in this panel (Ctrl+Z stays the scene's)", false, "undo", !map.can_undo());
    out += btn("Redo", "data-im", "redo", "Redo in this panel", false, "redo", !map.can_redo());
    out += input("im-new", im_new_, 150, "new action name");
    out += btn("Add action", "data-im", "add-action", "Actions are numbered by their order: adding or removing one makes older replays refuse to play.", false, "plus");
    out += btn("Add axis", "data-im", "add-axis", "A 2D axis read with Input.axis(#name)", false, "plus");
    if (!im_error_.empty()) out += std::format("<span class=\"tlf err\">{}</span>", w::esc(im_error_));
    out += "</div>";
    if (!im_capture_.empty()) {
        out += std::format("<div class=\"tlbar\">{}<span>Binding '{}': press a key, mouse button or gamepad button, or push a stick or trigger. Esc cancels.</span></div>", w::icon("record", 14), w::esc(im_capture_));
    }
    out += "<div class=\"tlbody scroll\">";
    const auto bindings = [&](const script::InputEntry& a, const char* unbind, const char* attr) {
        std::string s;
        if (a.bindings.empty()) s += "<span class=\"tlf\">(unbound)</span>";
        for (const std::string& b : a.bindings) s += std::format("<span class=\"bind\"><span class=\"mono\">{}</span><span class=\"bx\" {}=\"{}\" data-a=\"{}\" data-b=\"{}\" title=\"Remove {}\">{}</span></span>", w::esc(b), "data-im", unbind, w::esc(a.name), w::esc(b), w::esc(b), w::icon("close", 10));
        (void)attr;
        return s;
    };
    const std::vector<script::InputEntry> actions = map.actions();
    out += "<div class=\"pfr hd\"><span class=\"an\">action</span><span class=\"nm\">bindings</span></div>";
    for (const script::InputEntry& a : actions) {
        const bool renaming = im_rename_ == a.name;
        out += "<div class=\"imr\">";
        out += renaming ? input("im-ren", im_rename_text_, 130, "", std::format("data-a=\"{}\"", w::esc(a.name))) : std::format("<span class=\"an\">{}</span>", w::esc(a.name));
        out += "<span class=\"nm\">" + bindings(a, "unbind", "") + "</span>";
        out += btn("Bind", "data-im", "bind", "Press the next key, button or stick to bind", false, "plus", false, std::format("data-a=\"{}\"", w::esc(a.name)));
        out += input(std::format("im-t:{}", a.name).c_str(), "", 120, "or type Key.Escape", std::format("data-a=\"{}\"", w::esc(a.name)));
        out += btn("Rename", "data-im", "rename", "Rename this action", false, nullptr, false, std::format("data-a=\"{}\"", w::esc(a.name)));
        out += btn("Remove", "data-im", "rm-action", "Remove this action", false, "trash", false, std::format("data-a=\"{}\"", w::esc(a.name)));
        out += "</div>";
    }
    if (actions.empty()) out += "<div class=\"hint\">No actions. Type a name above and press Add action.</div>";
    if (!map.axes().empty()) out += "<div class=\"section\"><span>2D axes</span><span class=\"count\">Input.axis(#name) returns a vec2</span></div>";
    const std::vector<script::InputEntry> axes = map.axes();
    for (const script::InputEntry& a : axes) {
        out += std::format("<div class=\"imr\"><span class=\"an\">{}</span><span class=\"nm\">{}</span>", w::esc(a.name), bindings(a, "rm-src", ""));
        out += btn("+ source", "data-im", "src-menu", "Add a source to this axis", false, nullptr, false, std::format("data-a=\"{}\"", w::esc(a.name)));
        out += btn("Remove", "data-im", "rm-axis", "Remove this axis", false, "trash", false, std::format("data-a=\"{}\"", w::esc(a.name)));
        out += "</div>";
    }
    out += "</div></div>";
    e->SetInnerRML(out);
    e->SetClass("mono", false);
}

bool Shell::inputmap_click(Rml::Element* t) {
    if (Rml::Element* m = find_attr(t, "data-imenu")) { // the "+ source" popup
        const std::string src = m->GetAttribute<Rml::String>("data-imenu", "");
        close_popup();
        im_error_.clear();
        if (!ctl_.input_map().add_axis_source(im_axis_menu_, src)) im_error_ = ctl_.input_map().error();
        build_inputmap();
        return true;
    }
    Rml::Element* b = find_attr(t, "data-im");
    if (b == nullptr) return false;
    if (b->IsClassSet("off")) return true;
    InputMapDoc& map = ctl_.input_map();
    const std::string what = b->GetAttribute<Rml::String>("data-im", "");
    const std::string a = b->GetAttribute<Rml::String>("data-a", "");
    const std::string bind = b->GetAttribute<Rml::String>("data-b", "");
    im_error_.clear();
    bool ok = true;
    if (what == "save") { std::string error; if (map.save(error)) ctl_.notify_ok("Input map saved to project.toml"); else ctl_.report_failure(error); }
    else if (what == "undo") map.undo();
    else if (what == "redo") map.redo();
    else if (what == "add-action") { ok = map.add_action(im_new_); if (ok) im_new_.clear(); }
    else if (what == "add-axis") ok = map.add_axis(im_new_.empty() ? "axis" : im_new_);
    else if (what == "unbind") ok = map.unbind(a, bind);
    else if (what == "rm-src") ok = map.remove_axis_source(a, bind);
    else if (what == "rm-action") ok = map.remove_action(a);
    else if (what == "rm-axis") ok = map.remove_axis(a);
    else if (what == "rename") { im_rename_ = a; im_rename_text_ = a; }
    else if (what == "bind") { im_capture_ = a; im_capture_frames_ = 0; capture_dev_.clear_transients(); }
    else if (what == "src-menu") {
        Rml::Element* p = by_id("popup");
        if (p == nullptr) return true;
        std::vector<w::MenuItem> items;
        for (std::string_view src : script::axis2_source_names()) items.push_back({std::string(src), std::string(src), ""});
        im_axis_menu_ = a;
        p->SetInnerRML(w::popup_menu("data-imenu", items));
        p->SetProperty("width", "200dp");
        const Rml::Vector2f at = b->GetAbsoluteOffset(Rml::BoxArea::Border);
        p->SetProperty("left", std::format("{}px", static_cast<int>(at.x)));
        p->SetProperty("top", std::format("{}px", std::max(static_cast<int>(at.y) - 260, 40)));
        p->SetProperty("display", "block");
        popup_open_ = true;
        palette_ = false;
        return true;
    } else return false;
    if (!ok) im_error_ = map.error();
    build_inputmap();
    return true;
}

bool Shell::inputmap_change(Rml::Element* t, const std::string& value, bool enter) {
    const std::string id = t != nullptr ? t->GetId() : std::string();
    if (id.rfind("im-", 0) != 0 && id != "rp-path") return false;
    InputMapDoc& map = ctl_.input_map();
    if (id == "rp-path") { rp_path_ = value; return true; }
    if (id == "im-new") {
        im_new_ = value;
        if (enter) { im_error_.clear(); if (map.add_action(im_new_)) im_new_.clear(); else im_error_ = map.error(); build_inputmap(); }
    } else if (id == "im-ren") {
        im_rename_text_ = value;
        if (enter) { const std::string from = im_rename_; im_rename_.clear(); im_error_.clear(); if (!map.rename_action(from, value)) im_error_ = map.error(); build_inputmap(); }
    } else if (id.rfind("im-t:", 0) == 0 && enter) {
        im_error_.clear();
        if (!map.bind(t->GetAttribute<Rml::String>("data-a", ""), value)) im_error_ = map.error();
        build_inputmap();
    }
    return true;
}

// "Press a key": raw device state fed by the event loop while capturing; the next input becomes the binding.
void Shell::poll_capture() {
    if (im_capture_.empty()) return;
    if (++im_capture_frames_ > 2) { // the click that started it is still in this frame's taps
        if (capture_dev_.key(Key::Escape)) {
            im_capture_.clear();
        } else if (auto b = captured_binding(capture_dev_)) {
            if (!ctl_.input_map().bind(im_capture_, *b)) im_error_ = ctl_.input_map().error();
            im_capture_.clear();
        }
        if (im_capture_.empty()) build_inputmap();
    }
    capture_dev_.clear_transients();
}

} // namespace cuervo::editor

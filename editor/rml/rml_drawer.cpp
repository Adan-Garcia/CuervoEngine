// The drawer's selection-following tab (E2): Sprite, Particles, Animation and Script nodes each get the
// tools of their own node here. Field rows are the inspector's (same keys, same edit path), so scrub,
// click-to-type, reset and undo behave the same in both places.
#include <format>
#include <map>
#include <string>

#include "editor/anim_edit.hpp"
#include "editor/icons.hpp"
#include "editor/live_tree.hpp"
#include "editor/particle_preview.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"
#include "editor/sprite_tools.hpp"
#include "pluma/heap.hpp"

namespace cuervo::editor {


namespace {

using scene::Value;


Rml::Element* find_attr(Rml::Element* e, const char* attr) {
    for (; e != nullptr; e = e->GetParentNode()) if (e->HasAttribute(attr)) return e;
    return nullptr;
}

std::string kv(const char* k, const std::string& v, bool mono = true) {
    return std::format("<div class=\"prop\"><label><span class=\"dot\"></span><span class=\"lt\">{}</span></label><div class=\"ctl\"><span{}>{}</span></div></div>", k, mono ? " class=\"mono\"" : "", w::esc(v));
}

} // namespace

// The Debugger tab: the running game's state whatever is selected (the Script tab shows the same body
// for a scripted node only). While not playing it says how to start.
void Shell::build_debugger_drawer() {
    Rml::Element* e = by_id("drawer-body");
    if (e == nullptr) return;
    if (script_debug_drawer()) return; // a game with breakpoints is running: call stack, locals, watches
    const PlayController& play = ctl_.play();
    set_drawer_head(std::format("<span class=\"dh-title\">Debugger</span><span class=\"dh-note\">{}</span>{}", play.active() ? w::esc(std::string(play.state_name())) : std::string("not playing"), script_debug_chips()));
    std::string out;
    build_debugger_body(out);
    e->SetInnerRML(out);
    e->SetClass("mono", false);
}

void Shell::build_node_drawer() {
    if (layout_.drawer == "debugger") { build_debugger_drawer(); return; }
    if (layout_.drawer == "sprite") { // stand-alone asset view (Assets double-click, View > Sprites Tab)
        if (sprite_pick_.empty()) {
            const ModelNode* sel = ctl_.doc().model().get(selected_);
            if (sel != nullptr && node_tab_ == "Sprite") { build_sprite_drawer(sel); return; }
        }
        build_sprite_drawer(nullptr);
        return;
    }
    Rml::Element* e = by_id("drawer-body");
    if (e == nullptr || inspector_busy_) return;
    const ModelNode* n = ctl_.doc().model().get(selected_);
    if (n == nullptr) {
        set_drawer_head("<span class=\"dh-title\">Node</span>");
        e->SetInnerRML("<div class=\"empty\">Select a node to see its tools here.</div>");
        return;
    }
    e->SetClass("mono", false);
    if (node_tab_ == "Particles") build_particle_drawer();
    else if (node_tab_ == "Animation") build_animation_drawer(*n);
    else if (node_tab_ == "Sprite") build_sprite_drawer(n);
    else if (node_tab_ == "Script") build_script_drawer(*n);
    else if (node_tab_ == "Tiles") build_tiles_drawer(*n);
    else {
        set_drawer_head(std::format("<span class=\"dh-title\">{}</span>", w::esc(node_tab_)));
        e->SetInnerRML("<div class=\"empty\">No tools for this node.</div>");
    }
}

void Shell::build_tiles_drawer(const ModelNode& n) {
    Rml::Element* e = by_id("drawer-body");
    const EditorDocument& doc = ctl_.doc();
    set_drawer_head(std::format("<span class=\"dh-title\">Tiles</span><span class=\"dh-note\">{}</span>", w::esc(n.name)));
    if (n.type != "TileMapLayer") {
        e->SetInnerRML(std::format("<div class=\"empty\">{} has no tile tools. Select a TileMapLayer.</div>", w::esc(n.type)));
        return;
    }
    // Cells: count per tile id and the covered cell range.
    std::map<int, int> per_tile;
    int total = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const Value cells = doc.field_value(selected_, "cells");
    if (cells.kind == scene::ValueKind::List) {
        for (const Value& c : cells.items) {
            int x = 0, y = 0, id = 0;
            for (const auto& f : c.fields) {
                if (f.value.kind != scene::ValueKind::Int) continue;
                if (f.name == "x") x = static_cast<int>(f.value.i);
                else if (f.name == "y") y = static_cast<int>(f.value.i);
                else if (f.name == "tile_id") id = static_cast<int>(f.value.i);
            }
            if (total == 0) { x0 = x1 = x; y0 = y1 = y; }
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
            ++per_tile[id];
            ++total;
        }
    }
    const bool painting = ctl_.tool() == Tool::Paint;
    int count = 6;
    if (!per_tile.empty()) count = std::max(count, std::min(per_tile.rbegin()->first + 1, 64));
    std::string out = std::format("<div class=\"section\"><span>Palette</span><span class=\"count\">{}</span></div><div class=\"opts\">", painting ? std::format("painting tile {}", ctl_.paint_tile_id()) : std::string("pick a tile to paint"));
    for (int t = 0; t < count; ++t) {
        out += std::format("<div class=\"btn sm{}\" data-tile=\"{}\" title=\"Paint tile {}\"><span>{}</span><span class=\"src\">{}</span></div>", painting && ctl_.paint_tile_id() == t ? " on" : "", t, t, t, per_tile.contains(t) ? std::format(" ({})", per_tile[t]) : std::string());
    }
    out += "</div><div class=\"opts\">";
    out += std::format("<div class=\"btn sm{}\" data-tilemode=\"paint\" title=\"Paint tool (B)\">{}<span>Paint</span></div>", painting ? " on" : "", w::icon("grid", 14));
    out += std::format("<div class=\"btn sm\" data-tilemode=\"clear\" title=\"Remove every cell (undoable)\">{}<span>Clear cells</span></div></div>", w::icon("trash", 14));
    out += "<div class=\"hint\">Left button paints, right button erases (one undo step per stroke); middle button pans.</div>";
    out += "<div class=\"section\"><span>Cells</span></div>";
    out += kv("placed", std::to_string(total));
    if (total > 0) out += kv("range", std::format("({}, {}) to ({}, {})", x0, y0, x1, y1));
    out += field_group("Layer", {"tile_size", "tileset", "enabled", "light_occluders"});
    e->SetInnerRML(out);
}

void Shell::build_debugger_body(std::string& out) {
    PlayController& play = ctl_.play();
    script::GameSession* session = play.session();
    if (!play.active() || session == nullptr) {
        out += "<div class=\"empty\">Not playing. Press F5 to play the project; this tab then shows the running game.</div>";
        return;
    }
    script::ScriptHost& host = session->host();
    out += "<div class=\"section\"><span>Game</span></div>";
    out += kv("state", std::format("{}{}", play.mode() == PlayMode::Replay ? "replay, " : "", play.state_name()));
    out += kv("tick", std::format("{}  ({:.2f} s at {} Hz)", play.tick(), static_cast<double>(play.tick()) / session->project().tick_rate, session->project().tick_rate));
    out += kv("sim ms / tick", std::format("{:.3f} (last {:.3f})", play.average_tick_ms(), play.last_tick_ms()));
    out += kv("nodes", std::to_string(live_count(host.root())));
    out += kv("state hash", std::format("{:016x}", host.state_hash()));
    const pluma::HeapStats heap = pluma::heap_stats();
    out += kv("script objects", std::format("{} live, {} suspects", heap.live, heap.suspects));
    if (const auto& panic = play.panic()) {
        out += std::format("<div class=\"section err\"><span>Panic</span></div><div class=\"warn\">{}<span>{}</span></div><div class=\"hint\">Fix the script and save: it hot-reloads and Play resumes. Or Stop.</div>", w::icon("warning", 14), w::esc(panic->message));
        for (std::size_t i = 0; i < panic->frames.size() && i < 12; ++i) {
            const pluma::PanicFrame& f = panic->frames[i];
            const bool own = !f.file.starts_with("cuervo/");
            out += std::format("<div class=\"pr{}\" {}><span>{}</span><span class=\"src\">{}:{}</span></div>", i == 0 ? " sel" : "", own ? std::format("data-file=\"{}\" data-line=\"{}\" data-act=\"open-script\"", w::esc(f.file), f.line) : std::string(), w::esc(f.function), w::esc(f.file), f.line);
            if (i == 0) for (const pluma::PanicLocal& l : f.locals) out += kv(w::esc(l.name).c_str(), l.type + " = " + l.value);
        }
    }
    build_live_tree(out, *session);
}

// The running game's node tree (live_tree.hpp reads instances without running script code). Open state
// and the selected node are kept by object address; rows are capped so a big scene stays cheap.
void Shell::build_live_tree(std::string& out, script::GameSession& session) {
    script::ScriptHost& host = session.host();
    out += "<div class=\"section\"><span>Scene tree (live)</span></div>";
    if (!host.root().is_obj()) { out += "<div class=\"hint\">(no root)</div>"; return; }
    live_root_ = reinterpret_cast<std::uintptr_t>(host.root().o);
    int rows = 0;
    std::vector<pluma::Value> selected;
    const auto walk = [&](auto&& self, const pluma::Value& v, int depth) -> void {
        if (depth > 64 || rows > 400) return;
        const auto id = reinterpret_cast<std::uintptr_t>(v.o);
        const std::vector<pluma::Value> children = live_children(v);
        const bool open = depth == 0 ? !live_closed_.contains(id) : live_open_.contains(id);
        const std::string name = live_name(v);
        if (id == live_sel_) selected.push_back(v);
        out += std::format("<div class=\"pr{}\" style=\"padding-left: {}dp\" data-live=\"{}\">{}<span>{}</span><span class=\"src\">{}</span></div>", id == live_sel_ ? " sel" : "", 8 + depth * 12, id,
                           children.empty() ? std::string("<span class=\"chs\"></span>") : std::format("<span class=\"chs\" data-livechev=\"{}\">{}</span>", id, w::icon(open ? "chevron_down" : "chevron_right", 12)),
                           w::esc(name.empty() ? live_class(v) : name), name.empty() ? std::string() : w::esc(live_class(v)));
        ++rows;
        if (open) for (const pluma::Value& c : children) self(self, c, depth + 1);
    };
    walk(walk, host.root(), 0);
    if (rows > 400) out += "<div class=\"hint\">Tree capped at 400 rows; collapse branches to see the rest.</div>";
    if (!selected.empty()) {
        out += std::format("<div class=\"section\"><span>Fields of {}</span></div>", w::esc(live_name(selected.front()).empty() ? live_class(selected.front()) : live_name(selected.front())));
        for (const LiveField& f : live_fields(host.session()->vm(), selected.front())) out += kv(w::esc(f.name).c_str(), f.value);
    }
}

void Shell::build_script_drawer(const ModelNode& n) {
    Rml::Element* e = by_id("drawer-body");
    const auto [file, line] = ctl_.find_class_script(n.type);
    const PlayController& play = ctl_.play();
    // E2: Diagnostics by default, the debugger takes over while the game is paused.
    const bool debugger = play.active() && (play.state() == PlayState::Paused || play.panic().has_value());
    std::string out;
    set_drawer_head(std::format("<span class=\"dh-title\">{}</span><span class=\"dh-note\">{}:{}</span><div class=\"grow\"></div><button class=\"btn sm\" data-act=\"open-script\" data-file=\"{}\" data-line=\"{}\" title=\"Open in your code editor\"><span>Open script</span></button>",
                           debugger ? "Debugger" : "Diagnostics", w::esc(file), line, w::esc(file), line));
    if (debugger) {
        build_debugger_body(out);
    } else {
        const ClassSpec* spec = ctl_.env().classes().find(n.type);
        int shown = 0;
        if (spec != nullptr && !spec->default_error.empty()) {
            out += std::format("<div class=\"warn\">{}<span>{}</span></div>", w::icon("warning", 14), w::esc(spec->default_error));
            ++shown;
        }
        for (const ConsoleLine& l : lines_) {
            if (l.level != log::Level::Warn && l.level != log::Level::Error) continue;
            if (l.text.find(file) == std::string::npos && l.source.find(file) == std::string::npos) continue;
            out += std::format("<div class=\"line {}\"><span class=\"src\">{}</span>{}</div>", l.level == log::Level::Error ? "err" : "warn", w::esc(l.source), w::esc(l.text));
            ++shown;
        }
        if (shown == 0) out += std::format("<div class=\"empty\">No problems reported for {}. Errors from compiling or running it show here; Play and pause to inspect values.</div>", w::esc(file));
        if (play.active()) out += kv("game", std::string(play.state_name()));
    }
    e->SetInnerRML(out);
    e->SetClass("mono", false);
}

bool Shell::drawer_click(Rml::Element* t) {
    if (Rml::Element* c = find_attr(t, "data-livechev")) {
        const auto id = static_cast<std::uintptr_t>(std::strtoull(c->GetAttribute<Rml::String>("data-livechev", "0").c_str(), nullptr, 10));
        // The root starts open (tracked in the closed set); every other node starts closed (open set).
        if (id == live_root_) { if (!live_closed_.erase(id)) live_closed_.insert(id); }
        else if (!live_open_.erase(id)) live_open_.insert(id);
        build_node_drawer();
        return true;
    }
    if (Rml::Element* r = find_attr(t, "data-live")) {
        live_sel_ = static_cast<std::uintptr_t>(std::strtoull(r->GetAttribute<Rml::String>("data-live", "0").c_str(), nullptr, 10));
        build_node_drawer();
        return true;
    }
    if (sprite_click(t) || particles_click(t) || timeline_click(t)) return true;
    if (Rml::Element* e = find_attr(t, "data-tile")) {
        ctl_.set_paint_tile_id(std::atoi(e->GetAttribute<Rml::String>("data-tile", "0").c_str()));
        ctl_.set_tool(Tool::Paint);
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-tilemode")) {
        if (e->GetAttribute<Rml::String>("data-tilemode", "") == "paint") {
            ctl_.set_tool(ctl_.tool() == Tool::Paint ? Tool::Move : Tool::Paint);
        } else {
            Value empty;
            empty.kind = scene::ValueKind::List;
            ctl_.report(ctl_.doc().set_field(selected_, "cells", empty));
            ctl_.doc().seal();
        }
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-frame")) {
        const std::string f = e->GetAttribute<Rml::String>("data-frame", "");
        if (layout_.drawer == "sprite" && !sprite_pick_.empty()) { // stand-alone view: preview only
            sprite_.set_frame(f);
            build_node_drawer();
            return true;
        }
        ctl_.report(ctl_.doc().set_field(selected_, "frame", Value::str(f)));
        ctl_.doc().seal();
        return true;
    }
    return false;
}

} // namespace cuervo::editor

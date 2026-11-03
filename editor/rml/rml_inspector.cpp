// The RmlUi Inspector (mockup E4): object header, filter, grouped sections, modified markers, scrub
// fields, dropdowns, toggles. Every edit is EditorDocument::set_field / reset_field, the commands the
// ImGui inspector issues, so undo and redo are the same. A drag merges into one step and is sealed on
// release (DESIGN.md 4.13).

#include <algorithm>
#include <cmath>
#include <format>

#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include "editor/icons.hpp"
#include "editor/inspector_model.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"

namespace cuervo::editor {

namespace {

using scene::Value;
using scene::ValueKind;

Rml::Element* find_attr(Rml::Element* e, const char* attr) {
    for (; e != nullptr; e = e->GetParentNode()) if (e->HasAttribute(attr)) return e;
    return nullptr;
}

bool inside_id(Rml::Element* e, const char* id) {
    for (; e != nullptr; e = e->GetParentNode()) if (e->GetId() == id) return true;
    return false;
}

// Field rows live in the inspector and in the drawer's node tabs; both edit the primary selection.
bool in_fields(Rml::Element* e) { return inside_id(e, "inspector") || inside_id(e, "drawer-body"); }

// key = "field" | "field:path" | "field:path@component" (vec components).
struct Key {
    std::string field, path;
    int comp = -1;
};

Key parse_key(std::string_view k) {
    Key out;
    const std::size_t at = k.rfind('@');
    if (at != std::string_view::npos) {
        out.comp = std::atoi(std::string(k.substr(at + 1)).c_str());
        k = k.substr(0, at);
    }
    const std::size_t colon = k.find(':');
    out.field = std::string(k.substr(0, colon));
    if (colon != std::string_view::npos) out.path = std::string(k.substr(colon + 1));
    return out;
}

std::string make_key(const std::string& field, const std::string& path, int comp = -1) {
    std::string k = field;
    if (!path.empty()) k += ":" + path;
    if (comp >= 0) k += "@" + std::to_string(comp);
    return k;
}

constexpr const char* axes[] = {"x", "y", "z", "w"};

// One control for a leaf value; empty for compound kinds (rows are built by rows()).
std::string control(const std::string& key, const Value& v, const ClassRegistry& classes, bool modified) {
    switch (v.kind) {
    case ValueKind::Bool: return w::toggle(key, v.b) + std::format("<span class=\"tog-val\">{}</span>", v.b ? "true" : "false");
    case ValueKind::Int:
    case ValueKind::Float: return w::scrub(key, ve::format_number(v), {}, modified);
    case ValueKind::Vec: {
        std::string out;
        for (std::size_t i = 0; i < v.vec.size(); ++i) {
            Value c = Value::number(v.vec[i]);
            out += w::scrub(make_key(parse_key(key).field, parse_key(key).path, static_cast<int>(i)), ve::format_number(c), i < 4 ? axes[i] : "", modified);
        }
        return out;
    }
    case ValueKind::Str:
    case ValueKind::Name: return w::text_field(key, v.text, v.kind == ValueKind::Name ? "name" : "", false, true);
    case ValueKind::Enum: {
        const bool known = classes.find_enum(v.type) != nullptr;
        return w::dropdown(key, v.text, !known);
    }
    case ValueKind::Struct:
        if (ve::is_color(v))
            return std::format("<span class=\"swatch\" data-cp=\"{}\" title=\"Pick a color\" style=\"background-color: {}\"></span>", w::esc(key), ve::color_css(v)) + w::text_field(key, ve::color_hex(v), "", false, true);
        return {};
    case ValueKind::Null: return w::text_field(key, "", "no default: type a value", false, true);
    case ValueKind::List: return {};
    }
    return {};
}

std::string rows(const std::string& field, const std::string& path, const std::string& label, const Value& v, const ClassRegistry& classes, int depth, bool modified, bool dim, const std::string& tip) {
    const std::string key = make_key(field, path);
    const std::string pad = std::format("<span style=\"width: {}dp; flex: none\"></span>", depth * 12);
    if (v.kind == ValueKind::Struct && !ve::is_color(v) && depth < 8) {
        std::string out = w::prop(key, label, std::format("<span class=\"tog-val\">{}</span>", w::esc(v.type)), {.modified = modified && depth == 0, .dim = dim, .reset = depth == 0, .tip = tip});
        for (const scene::Field& f : v.fields) out += rows(field, ve::join_path(path, f.name), "\xC2\xA0\xC2\xA0\xC2\xA0" + f.name, f.value, classes, depth + 1, false, dim, "");
        return out;
    }
    if (v.kind == ValueKind::List && depth < 8) {
        std::string out = w::prop(key, label, std::format("<span class=\"tog-val\">[{}]</span><div class=\"btn sm\" data-listadd=\"{}\" title=\"Add an item (copies the last one)\">{}<span>Item</span></div>", v.items.size(), w::esc(key), w::icon("plus", 12)),
                                  {.modified = modified && depth == 0, .dim = dim, .reset = depth == 0, .tip = tip});
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            const std::string sub = ve::join_path(path, std::to_string(i));
            const Value& item = v.items[i];
            const std::string del = std::format("<div class=\"rst\" data-listdel=\"{}\" title=\"Remove this item\">{}</div>", w::esc(make_key(field, sub)), w::icon("close", 12));
            if (item.kind == ValueKind::Struct && !ve::is_color(item)) {
                out += rows(field, sub, std::format("\xC2\xA0\xC2\xA0\xC2\xA0[{}]", i), item, classes, depth + 1, false, dim, "");
            } else {
                out += w::prop(make_key(field, sub), std::format("\xC2\xA0\xC2\xA0\xC2\xA0[{}]", i), control(make_key(field, sub), item, classes, false) + del, {.dim = dim, .reset = false});
            }
        }
        return out;
    }
    (void)pad;
    return w::prop(key, label, control(key, v, classes, modified), {.modified = modified && depth == 0, .dim = dim, .reset = depth == 0, .tip = tip});
}

} // namespace

void Shell::build_inspector() {
    Rml::Element* e = by_id("inspector");
    if (e == nullptr) return;
    if (inspector_busy_) return; // a drag or a typed edit owns the elements
    EditorDocument& doc = ctl_.doc();
    const ModelNode* n = doc.model().get(selected_);
    if (n == nullptr) {
        sections_.clear();
        e->SetInnerRML("<div class=\"empty\">Select a node in the scene tree or the view to edit its fields.</div>");
        return;
    }
    const ClassRegistry& classes = ctl_.env().classes();
    const ClassSpec* spec = classes.find(n->type);
    const std::string_view icon = icon_for_class(&classes, n->type);
    const auto [script_path, script_line] = ctl_.find_class_script(n->type);
    const bool has_script = !script_path.empty();
    ctx_node_ = selected_;

    std::string out = std::format("<div class=\"obj\">{}<div class=\"obj-text\"><div class=\"obj-name\">{}</div><div class=\"obj-type\">{} extends {}</div></div>{}</div>", w::icon(icon.empty() ? "node" : icon, 20),
                                  w::esc(n->name), w::esc(n->type), spec != nullptr && !spec->base.empty() ? w::esc(spec->base) : "Object",
                                  has_script ? std::format("<div class=\"ibtn sm tright\" data-ctx=\"script-open\" title=\"Open script ({}:{})\">{}</div>", w::esc(script_path), script_line, w::icon("code", 14)) : std::string());
    out += has_script ? std::format("<div class=\"srow\"><span class=\"muted-t\">Script</span><span class=\"slink\">{}</span><div class=\"btn sm\" data-ctx=\"script-change\"><span>Change</span></div><div class=\"btn sm\" data-ctx=\"script-detach\"><span>Detach</span></div></div>", w::esc(script_path))
                      : std::string("<div class=\"srow\"><span class=\"muted-t\">Script</span><span class=\"muted-t\">none</span><div class=\"btn sm\" data-ctx=\"script-attach\"><span>Attach script...</span></div></div>");
    out += std::format("<div class=\"srow\"><span class=\"muted-t\">Path</span><span class=\"mono\">{}</span></div>", w::esc(doc.model().path_of(selected_)));
    out += w::prop("@name", "name", w::text_field("@name", n->name, "", false, true), {.modified = false, .reset = false});

    if (n->type == "TileMapLayer") {
        std::vector<w::Seg> tiles;
        for (int t = 0; t < 6; ++t) tiles.push_back({std::to_string(t), std::format("Tile {}", t)});
        out += w::prop("@tile", "tile palette", w::segmented("tile", tiles, ctl_.tool() == Tool::Paint ? std::to_string(ctl_.paint_tile_id()) : std::string("-")), {.reset = false});
    }
    if (spec == nullptr) out += std::format("<div class=\"warn\">{}<span>'{}' is not a node class of this project; its stored fields are shown as saved.</span></div>", w::icon("warning", 14), w::esc(n->type));
    else if (!spec->default_error.empty()) out += std::format("<div class=\"warn\">{}<span>defaults unavailable: {}</span></div>", w::icon("warning", 14), w::esc(spec->default_error));

    std::vector<std::string> stored;
    for (const scene::Field& f : n->fields) stored.push_back(f.name);
    sections_ = ve::group_fields(classes, n->type, stored);
    const bool filtering = !filter_.empty();
    const std::vector<ve::Section> shown = ve::filter_sections(sections_, filter_);
    std::size_t hidden_total = 0;
    for (const ve::Section& sec : shown) {
        const bool open = filtering || ve::section_open(layout_.closed_sections, sec);
        std::string count;
        if (!open) count = sec.key == "Advanced" ? std::format("{} hidden", sec.fields.size()) : std::to_string(sec.fields.size());
        out += w::section(sec.key, sec.title, open, count, open ? "Fold this section" : "Unfold this section");
        if (!open) {
            hidden_total += sec.fields.size();
            continue;
        }
        for (const std::string& field : sec.fields) {
            bool is_stored = false;
            const Value v = doc.field_value(selected_, field, &is_stored);
            const std::string type = doc.field_type(selected_, field);
            out += rows(field, "", field, v, classes, 0, is_stored, !is_stored, std::format("{}: {}{}", field, type, is_stored ? "" : " (class default)"));
        }
    }
    if (shown.empty()) out += filtering ? "<div class=\"hint\">No field matches the filter.</div>" : (spec != nullptr && spec->fields.empty() ? "<div class=\"hint\">No exported fields.</div>" : "");
    (void)hidden_total;
    e->SetInnerRML(out);
}

// ---- editing

bool Shell::with_target(const std::string& key, const std::function<void(Value&)>& fn, bool merge) {
    EditorDocument& doc = ctl_.doc();
    const Key k = parse_key(key);
    if (selected_ == 0 || k.field.empty()) return false;
    Value root = doc.field_value(selected_, k.field);
    Value* t = ve::at_path(root, k.path);
    if (t == nullptr) return false;
    if (k.comp >= 0) {
        if (t->kind != ValueKind::Vec || static_cast<std::size_t>(k.comp) >= t->vec.size()) return false;
        Value tmp = Value::number(t->vec[static_cast<std::size_t>(k.comp)]);
        fn(tmp);
        t->vec[static_cast<std::size_t>(k.comp)] = static_cast<float>(tmp.f);
    } else {
        fn(*t);
    }
    const bool ok = doc.set_field(selected_, k.field, std::move(root), merge);
    if (!ok) ctl_.report(false);
    if (ok && !merge) doc.seal();
    return ok;
}

Value Shell::target_value(const std::string& key) const {
    const Key k = parse_key(key);
    Value root = ctl_.doc().field_value(selected_, k.field);
    Value* t = ve::at_path(root, k.path);
    if (t == nullptr) return {};
    if (k.comp >= 0 && t->kind == ValueKind::Vec && static_cast<std::size_t>(k.comp) < t->vec.size()) return Value::number(t->vec[static_cast<std::size_t>(k.comp)]);
    return *t;
}

void Shell::commit_edits() {
    if (!edit_key_.empty()) {
        const std::string key = edit_key_, text = edit_text_;
        edit_key_.clear();
        inspector_busy_ = false;
        apply_text(key, text);
    }
    inspector_busy_ = false;
    revision_ = ~0ull;
}

void Shell::cancel_edits() {
    edit_key_.clear();
    inspector_busy_ = false;
    revision_ = ~0ull;
}

void Shell::apply_text(const std::string& key, const std::string& text) {
    if (key == "@name") {
        const ModelNode* n = ctl_.doc().model().get(selected_);
        if (n != nullptr && !text.empty() && text != n->name) {
            ctl_.report(ctl_.doc().rename(selected_, text));
            ctl_.doc().seal();
        }
        return;
    }
    const Value cur = target_value(key);
    with_target(key, [&](Value& v) {
        switch (v.kind) {
        case ValueKind::Int:
        case ValueKind::Float:
            if (!ve::parse_number(text, v)) ctl_.report_failure(std::format("'{}' is not a number.", text));
            break;
        case ValueKind::Str:
        case ValueKind::Name: v.text = text; break;
        case ValueKind::Struct:
            if (ve::is_color(v) && !ve::parse_color_hex(text, v)) ctl_.report_failure("Colors are #RRGGBB or #RRGGBBAA.");
            break;
        case ValueKind::Null: {
            std::string error;
            if (auto parsed = scene::parse_value(text, error)) v = *parsed;
            else ctl_.report_failure(std::format("{}: {}", key, error));
            break;
        }
        default: break;
        }
    }, false);
    (void)cur;
}

// ---- color picker popup: SV square, hue strip, alpha strip, hex field. Drags merge into one undo step.
void Shell::open_color_picker(const std::string& key, Rml::Element* anchor) {
    Rml::Element* p = by_id("popup");
    const Value v = target_value(key);
    if (p == nullptr || !ve::is_color(v)) return;
    const ve::Hsva c = ve::hsva_of(v);
    cp_key_ = key;
    cp_start_ = v;
    cp_ = c;
    cp_drag_ = 0;
    close_popup();
    p->SetInnerRML(std::format(
        "<div class=\"cp\"><div id=\"cp-sv\" class=\"cp-sv\"><img src=\"grad:h\"/><img src=\"grad:v\"/><div id=\"cp-svm\" class=\"cp-m\"></div></div>"
        "<div id=\"cp-hue\" class=\"cp-bar\"><img src=\"grad:hue\"/><div id=\"cp-huem\" class=\"cp-bm\"></div></div>"
        "<div id=\"cp-alpha\" class=\"cp-bar\"><img src=\"grad:fade\"/><div id=\"cp-am\" class=\"cp-bm\"></div></div>"
        "<div class=\"cp-row\"><span id=\"cp-old\" class=\"swatch\" title=\"Original (click to restore)\" data-cp-old=\"1\" style=\"background-color: {}\"></span><span id=\"cp-new\" class=\"swatch\"></span>"
        "<input id=\"cp-hex\" type=\"text\" value=\"{}\"/></div></div>", ve::color_css(v), ve::color_hex(v)));
    const float ratio = ctx_->GetDensityIndependentPixelRatio();
    const float pw = 212.0f * ratio;
    const float left = std::min(anchor->GetAbsoluteLeft(), ctx_->GetDimensions().x - pw - 8.0f * ratio);
    p->SetProperty("width", "212dp");
    p->SetProperty("left", std::format("{}px", static_cast<int>(left)));
    p->SetProperty("top", std::format("{}px", static_cast<int>(anchor->GetAbsoluteTop() + anchor->GetOffsetHeight() + 2)));
    p->SetProperty("display", "block");
    popup_open_ = true;
    palette_ = false;
    cp_open_ = true;
    cp_refresh(true);
}

void Shell::cp_refresh(bool hex) {
    const auto set = [&](const char* id, const char* prop, const std::string& val) { if (Rml::Element* e = by_id(id)) e->SetProperty(prop, val); };
    ve::Hsva c = cp_;
    Value tmp = cp_start_;
    ve::set_hsva(tmp, c);
    set("cp-sv", "background-color", ve::hue_css(c.h));
    set("cp-svm", "left", std::format("{}%", c.s * 100.0f));
    set("cp-svm", "top", std::format("{}%", (1.0f - c.v) * 100.0f));
    set("cp-huem", "left", std::format("{}%", c.h * 100.0f));
    set("cp-am", "left", std::format("{}%", c.a * 100.0f));
    Value opaque = tmp;
    opaque.fields[3].value.f = 1.0;
    set("cp-alpha", "background-color", ve::color_css(opaque));
    set("cp-new", "background-color", ve::color_css(tmp));
    if (hex) if (Rml::Element* h = by_id("cp-hex")) h->SetAttribute("value", ve::color_hex(tmp));
}

void Shell::cp_apply(bool merge) {
    ve::Hsva c = cp_;
    with_target(cp_key_, [&](Value& v) { if (ve::is_color(v)) ve::set_hsva(v, c); }, merge);
}

void Shell::cp_drag_to(Rml::Vector2f at) {
    const char* id = cp_drag_ == 1 ? "cp-sv" : cp_drag_ == 2 ? "cp-hue" : "cp-alpha";
    Rml::Element* e = by_id(id);
    if (e == nullptr) return;
    const float fx = std::clamp((at.x - e->GetAbsoluteLeft()) / std::max(1.0f, e->GetOffsetWidth()), 0.0f, 1.0f);
    const float fy = std::clamp((at.y - e->GetAbsoluteTop()) / std::max(1.0f, e->GetOffsetHeight()), 0.0f, 1.0f);
    if (cp_drag_ == 1) { cp_.s = fx; cp_.v = 1.0f - fy; }
    else if (cp_drag_ == 2) cp_.h = std::min(fx, 0.9999f);
    else cp_.a = fx;
    cp_apply(true);
    cp_refresh(true);
}

bool Shell::cp_mousedown(Rml::Element* t, Rml::Vector2f at) {
    if (!cp_open_) return false;
    if (!inside_id(t, "popup")) { cp_open_ = false; return false; }
    if (find_attr(t, "data-cp-old") != nullptr) {
        cp_ = ve::hsva_of(cp_start_);
        cp_apply(false);
        cp_refresh(true);
        return true;
    }
    cp_drag_ = inside_id(t, "cp-sv") ? 1 : inside_id(t, "cp-hue") ? 2 : inside_id(t, "cp-alpha") ? 3 : 0;
    if (cp_drag_ == 0) return false;
    inspector_busy_ = true;
    cp_drag_to(at);
    return true;
}

bool Shell::cp_mouseup() {
    if (cp_drag_ == 0) return false;
    cp_drag_ = 0;
    ctl_.doc().seal();
    inspector_busy_ = false;
    revision_ = ~0ull;
    return true;
}

bool Shell::cp_change(Rml::Element* t, const std::string& value, bool enter) {
    if (!cp_open_ || t == nullptr || t->GetId() != "cp-hex") return false;
    Value tmp = cp_start_;
    if (ve::parse_color_hex(value, tmp)) {
        const ve::Hsva c = ve::hsva_of(tmp);
        // Keep hue/saturation the hex cannot express (gray, black).
        cp_.v = c.v;
        cp_.a = c.a;
        if (c.s > 0.0f) { cp_.s = c.s; cp_.h = c.h; }
        else cp_.s = 0.0f;
        cp_apply(false);
        cp_refresh(false);
    } else if (enter) {
        ctl_.report_failure("Colors are #RRGGBB or #RRGGBBAA.");
    }
    return true;
}

bool Shell::inspector_mousedown(Rml::Element* t, Rml::Vector2f at) {
    if (cp_mousedown(t, at)) return true;
    // Any press outside the field being typed in applies what was typed.
    if (!edit_key_.empty() && !inside_id(t, "edit-field") && find_attr(t, "data-field") == nullptr) commit_edits();
    if (!in_fields(t)) return false;
    if (find_attr(t, "data-field") != nullptr || t->GetTagName() == "input") return false;
    if (Rml::Element* s = find_attr(t, "data-scrub"); s != nullptr && !inside_id(t, "edit-field")) {
        scrub_ = {};
        scrub_.key = s->GetAttribute<Rml::String>("data-scrub", "");
        scrub_.start = target_value(scrub_.key);
        scrub_.press = scrub_.last = at;
        scrub_.el = s;
        scrub_.down = true;
        return true;
    }
    return false;
}

void Shell::inspector_mousemove(Rml::Vector2f at) {
    if (cp_drag_ != 0) { cp_drag_to(at); return; }
    if (!scrub_.down) return;
    const float ratio = ctx_->GetDensityIndependentPixelRatio();
    if (!scrub_.active && std::fabs(at.x - scrub_.press.x) < 3.0f * ratio) return;
    if (!scrub_.active) {
        scrub_.active = true;
        inspector_busy_ = true;
        hide_tip();
        scrub_.el->SetClass("drag", true);
    }
    const Uint32 mod = SDL_GetModState();
    const double speed = ve::scrub_speed(scrub_.start, (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_ALT) != 0);
    scrub_.acc += static_cast<double>(at.x - scrub_.last.x) / ratio * speed;
    scrub_.last = at;
    Value shown;
    with_target(scrub_.key, [&](Value& v) {
        ve::apply_scrub(v, scrub_.start, scrub_.acc);
        shown = v;
    }, true);
    if (Rml::Element* sv = scrub_.el->GetFirstChild(); sv != nullptr) {
        for (int i = 0; i < scrub_.el->GetNumChildren(); ++i) if (scrub_.el->GetChild(i)->IsClassSet("sv")) sv = scrub_.el->GetChild(i);
        sv->SetInnerRML(w::esc(ve::format_number(shown)));
    }
}

void Shell::inspector_mouseup() {
    if (cp_mouseup()) return;
    if (!scrub_.down) return;
    const bool dragged = scrub_.active;
    Rml::Element* el = scrub_.el;
    const std::string key = scrub_.key;
    const Value start = scrub_.start;
    scrub_ = {};
    if (dragged) {
        el->SetClass("drag", false);
        ctl_.doc().seal();
        inspector_busy_ = false;
        revision_ = ~0ull;
        return;
    }
    // A click: type the number.
    edit_key_ = key;
    edit_text_ = ve::format_number(start);
    inspector_busy_ = true;
    el->SetInnerRML(std::format("<input id=\"edit-field\" type=\"text\" value=\"{}\"/>", w::esc(edit_text_)));
    if (Rml::Element* f = by_id("edit-field")) {
        f->Focus();
        if (auto* inp = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(f)) inp->Select();
    }
}

bool Shell::inspector_click(Rml::Element* t) {
    if (!in_fields(t) && !inside_id(t, "popup")) return false;
    EditorDocument& doc = ctl_.doc();
    if (inside_id(t, "popup") && cp_open_) return true; // clicks inside the picker are handled by the drag path
    if (Rml::Element* e = find_attr(t, "data-cp")) {
        open_color_picker(e->GetAttribute<Rml::String>("data-cp", ""), e);
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-pick")) {
        const std::string v = e->GetAttribute<Rml::String>("data-pick", "");
        const std::string key = dd_key_;
        close_popup();
        with_target(key, [&](Value& val) { if (val.kind == ValueKind::Enum) val.text = v; }, false);
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-tog")) {
        if (e->IsClassSet("disabled")) return true;
        with_target(e->GetAttribute<Rml::String>("data-tog", ""), [](Value& v) { if (v.kind == ValueKind::Bool) v.b = !v.b; }, false);
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-dd")) {
        if (e->IsClassSet("disabled")) return true;
        dd_key_ = e->GetAttribute<Rml::String>("data-dd", "");
        const Value cur = target_value(dd_key_);
        const EnumSpec* spec = ctl_.env().classes().find_enum(cur.type);
        Rml::Element* p = by_id("popup");
        if (spec == nullptr || p == nullptr) return true;
        std::vector<w::MenuItem> items;
        for (const std::string& variant : spec->variants) items.push_back({variant, variant, "", false, variant == cur.text});
        p->SetInnerRML(w::popup_menu("data-pick", items));
        p->SetProperty("width", std::format("{}px", static_cast<int>(std::max(160.0f, e->GetOffsetWidth()))));
        p->SetProperty("left", std::format("{}px", static_cast<int>(e->GetAbsoluteLeft())));
        p->SetProperty("top", std::format("{}px", static_cast<int>(e->GetAbsoluteTop() + e->GetOffsetHeight() + 2)));
        p->SetProperty("display", "block");
        popup_open_ = true;
        palette_ = false;
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-reset")) {
        const std::string field = parse_key(e->GetAttribute<Rml::String>("data-reset", "")).field;
        ctl_.report(doc.reset_field(selected_, field));
        doc.seal();
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-sect")) {
        const std::string key = e->GetAttribute<Rml::String>("data-sect", "");
        for (const ve::Section& s : sections_) {
            if (s.key != key) continue;
            layout_.closed_sections = ve::toggle_section(layout_.closed_sections, s);
            save_layout();
            revision_ = ~0ull;
        }
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-listadd")) {
        with_target(e->GetAttribute<Rml::String>("data-listadd", ""), [](Value& v) { if (v.kind == ValueKind::List && !v.items.empty()) v.items.push_back(v.items.back()); }, false);
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-listdel")) {
        const std::string key = e->GetAttribute<Rml::String>("data-listdel", "");
        const Key k = parse_key(key);
        const std::size_t slash = k.path.rfind('/');
        const std::string parent = slash == std::string::npos ? std::string() : k.path.substr(0, slash);
        const std::size_t index = static_cast<std::size_t>(std::atoi((slash == std::string::npos ? k.path : k.path.substr(slash + 1)).c_str()));
        with_target(make_key(k.field, parent), [&](Value& v) { if (v.kind == ValueKind::List && index < v.items.size()) v.items.erase(v.items.begin() + static_cast<std::ptrdiff_t>(index)); }, false);
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-segi")) {
        const int tile = std::atoi(e->GetAttribute<Rml::String>("data-segi", "0").c_str());
        ctl_.set_paint_tile_id(tile);
        ctl_.set_tool(Tool::Paint);
        revision_ = ~0ull;
        return true;
    }
    if (Rml::Element* e = find_attr(t, "data-clear")) {
        (void)e;
        return false; // handled by the filter's own clear below
    }
    return false;
}

bool Shell::inspector_change(Rml::Element* t, const std::string& value, bool enter) {
    if (cp_change(t, value, enter)) return true;
    const std::string tid = t != nullptr ? t->GetId() : std::string();
    if (tid == "insp-filter") {
        filter_ = value;
        if (Rml::Element* c = by_id("insp-clear")) c->SetClass("hidden", value.empty());
        revision_ = ~0ull;
        return true;
    }
    if (tid == "edit-field") {
        edit_text_ = value;
        if (enter) commit_edits();
        return true;
    }
    Rml::Element* f = find_attr(t, "data-field");
    if (f != nullptr && in_fields(t)) {
        edit_key_ = f->GetAttribute<Rml::String>("data-field", "");
        edit_text_ = value;
        inspector_busy_ = true;
        if (enter) commit_edits();
        return true;
    }
    return false;
}

// Rows for named fields of the selected node (drawer tabs): the same controls, keys and edit paths as
// the inspector. Fields the node's class does not have are skipped.
std::string Shell::field_group(const std::string& title, const std::vector<std::string>& fields) {
    const EditorDocument& doc = ctl_.doc();
    const ClassRegistry& classes = ctl_.env().classes();
    std::string out = std::format("<div class=\"section\"><span>{}</span></div>", w::esc(title));
    for (const std::string& field : fields) {
        const std::string type = doc.field_type(selected_, field);
        if (type.empty()) continue;
        bool stored = false;
        const Value v = doc.field_value(selected_, field, &stored);
        out += rows(field, "", field, v, classes, 0, stored, !stored, std::format("{}: {}{}", field, type, stored ? "" : " (class default)"));
    }
    return out;
}

} // namespace cuervo::editor

// The drawer's Sprite tab (E2): image / atlas-frame preview with zoom and pan, the slice grid and
// 9-slice guides drawn over it, the atlas listing, and the grid slice tool. State lives in SpriteView.
#include <algorithm>
#include <cmath>
#include <format>

#include "editor/icons.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_rhi.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"

namespace cuervo::editor {

namespace {

using scene::Value;

Rml::Element* find_attr(Rml::Element* e, const char* attr) {
    for (; e != nullptr; e = e->GetParentNode()) if (e->HasAttribute(attr)) return e;
    return nullptr;
}

bool inside_id(Rml::Element* e, const char* id) {
    for (; e != nullptr; e = e->GetParentNode()) if (e->GetId() == id) return true;
    return false;
}

std::string pct(double v, double of) { return std::format("{:.4f}%", of > 0 ? v * 100.0 / of : 0.0); }

std::string num_row(const char* label, const char* id, int v, const char* tip) {
    return std::format("<div class=\"prop\" title=\"{}\"><label><span class=\"dot\"></span><span class=\"lt\">{}</span></label><div class=\"ctl\"><div class=\"tf\"><input type=\"text\" id=\"{}\" value=\"{}\"/></div></div></div>", tip, label, id, v);
}

std::string warn_row(const std::string& text) {
    return std::format("<div class=\"warn\">{}<span>{}</span></div>", w::icon("warning", 14), w::esc(text));
}

} // namespace

// `n` is the selected sprite-like node, or null for the stand-alone view of an asset picked in the Assets
// pane (double-click an image, atlas or SVG): the same preview and tools, without node fields.
void Shell::build_sprite_drawer(const ModelNode* n) {
    Rml::Element* e = by_id("drawer-body");
    const EditorDocument& doc = ctl_.doc();
    const auto dir = ctl_.env().project_dir();
    const NodeId target = n != nullptr ? selected_ : 0;
    const Value a = doc.field_value(target, "asset");
    const std::string asset = n == nullptr ? sprite_pick_ : a.kind == scene::ValueKind::Str ? a.text : std::string();
    if (n == nullptr && asset.empty()) {
        set_drawer_head("<span class=\"dh-title\">Sprites</span>");
        e->SetInnerRML("<div class=\"empty\">Double-click an image, atlas or SVG in Assets to preview it here, or select a Sprite2D to see its image.</div>");
        e->SetClass("mono", false);
        sprite_fit_ = false;
        return;
    }
    // The view follows the node's asset; a path chosen by the slice tool (the atlas just written) stays
    // until the selection or the node's asset changes.
    if (target != sprite_node_ || asset != sprite_asset_) {
        sprite_node_ = target;
        sprite_asset_ = asset;
        sprite_msg_.clear();
        sprite_.set_path(dir, asset, true);
    }
    const Value fv = doc.field_value(target, "frame");
    if (fv.kind == scene::ValueKind::Str && !fv.text.empty() && sprite_.atlas() != nullptr) sprite_.set_frame(fv.text);

    set_drawer_head(std::format("<span class=\"dh-title\">{}</span><span class=\"dh-note\">{}</span>", n == nullptr ? "Sprites" : "Sprite", asset.empty() ? "No image set" : w::esc(asset)));
    const bool nine = n != nullptr && n->type == "NinePatchRect";

    // Left: the preview.
    std::string out = "<div class=\"dsplit\"><div class=\"dleft\">";
    out += std::format("<div class=\"pvbar\"><div class=\"btn sm\" data-sv=\"out\" title=\"Zoom out\">{}</div><span id=\"sv-zoom\" class=\"mono\"></span><div class=\"btn sm\" data-sv=\"in\" title=\"Zoom in\">{}</div>"
                       "<div class=\"btn sm\" data-sv=\"fit\" title=\"Fit the image in the view\"><span>Fit</span></div><div class=\"btn sm\" data-sv=\"one\" title=\"One image pixel per screen pixel\"><span>1:1</span></div>{}</div>",
                       w::icon("minus", 12), w::icon("plus", 12),
                       sprite_.is_image() ? std::format("<div class=\"btn sm{}\" data-sv=\"grid\" title=\"Show the slice grid\">{}<span>Grid</span></div>", sprite_.show_grid ? " on" : "", w::icon("grid", 12)) : std::string());
    const auto shown = sprite_.shown(dir);
    if (shown) {
        std::string src;
        if (shown->pixels) {
            rml_set_memory_image(shown->image, *shown->pixels); // SVG and packed-atlas frames: pixels made in memory
            src = "mem:" + shown->image;
        } else {
            src = asset_thumb_src(shown->image, 4096);
            src.replace(0, 4, "pixel"); // crisp pixels when zoomed (nearest-neighbour texture)
        }
        std::string rect = shown->w < shown->full_w || shown->h < shown->full_h || shown->x != 0 || shown->y != 0 ? std::format(" rect=\"{} {} {} {}\"", shown->x, shown->y, shown->w, shown->h) : std::string();
        out += std::format("<div id=\"spv\" class=\"pvview\"><div id=\"spv-in\" class=\"pvin\"><img class=\"pvimg pvchk\" src=\"check:16\"/><img class=\"pvimg\" src=\"{}\"{}/>", w::esc(src), rect);
        if (sprite_.is_image() && sprite_.show_grid) {
            std::string err;
            const auto frames = sprite_.slice(err);
            if (frames.size() <= 1500) {
                for (const auto& f : frames) out += std::format("<div class=\"pvcell\" style=\"left:{};top:{};width:{};height:{}\"></div>", pct(f.x, shown->w), pct(f.y, shown->h), pct(f.w, shown->w), pct(f.h, shown->h));
            }
        }
        if (nine) {
            const auto m = [&](const char* f) { return static_cast<double>(doc.field_value(target, f).f); };
            out += std::format("<div class=\"pvline v\" style=\"left:{}\"></div><div class=\"pvline v\" style=\"left:{}\"></div><div class=\"pvline h\" style=\"top:{}\"></div><div class=\"pvline h\" style=\"top:{}\"></div>",
                               pct(m("margin_left"), shown->w), pct(shown->w - m("margin_right"), shown->w), pct(m("margin_top"), shown->h), pct(shown->h - m("margin_bottom"), shown->h));
        }
        out += "</div></div>";
        sprite_fit_ = true;
    } else {
        out += std::format("<div class=\"pvview\"><div class=\"empty\">{}</div></div>", w::esc(asset.empty() ? std::string("Set the asset to preview it.") : sprite_.status().empty() ? std::string("Nothing to preview.") : sprite_.status()));
        sprite_fit_ = false;
    }
    out += "</div><div class=\"dright scroll\">";

    // Right: node fields, frames, tools.
    if (n != nullptr) out += field_group(n->type, {"asset", "frame", "centered", "flip_h", "flip_v", "tint", "filter", "size", "fps", "playing", "animation", "speed", "margin_left", "margin_top", "margin_right", "margin_bottom"});
    if (!asset.empty()) {
        out += "<div class=\"section\"><span>Image</span></div>";
        if (sprite_.is_image() && sprite_.info().ok) {
            out += std::format("<div class=\"prop\"><label><span class=\"dot\"></span><span class=\"lt\">size</span></label><div class=\"ctl\"><span class=\"mono\">{} x {} px</span></div></div>", sprite_.info().width, sprite_.info().height);
        } else if (!sprite_.status().empty() && !shown) {
            out += warn_row(sprite_.status());
        }
        if (!sprite_.frames().empty()) {
            const std::string current = n == nullptr ? sprite_.frame() : fv.kind == scene::ValueKind::Str ? fv.text : std::string();
            out += std::format("<div class=\"section\"><span>Frames</span><span class=\"count\">{} - click to preview and use</span></div>", sprite_.frames().size());
            const std::size_t cap = 200;
            for (std::size_t i = 0; i < sprite_.frames().size() && i < cap; ++i) {
                const std::string& f = sprite_.frames()[i];
                out += std::format("<div class=\"pr{}\" data-frame=\"{}\"><span>{}</span></div>", f == current ? " sel" : "", w::esc(f), w::esc(f));
            }
            if (sprite_.frames().size() > cap) out += std::format("<div class=\"hint\">{} more frames not listed.</div>", sprite_.frames().size() - cap);
        }
        if (const AtlasEditor* at = sprite_.atlas()) {
            out += std::format("<div class=\"section\"><span>Atlas sources</span><span class=\"count\">{}</span></div>", at->dirty() ? "unsaved" : "saved");
            const assets::AtlasSpec& spec = at->spec();
            for (std::size_t i = 0; i < spec.sprites.size(); ++i) {
                out += std::format("<div class=\"pr\"><span>sprite {}</span><div class=\"btn sm\" data-sv=\"rm-sprite\" data-i=\"{}\" title=\"Remove from the atlas\"><span>Remove</span></div></div>", w::esc(spec.sprites[i].path), i);
            }
            for (std::size_t i = 0; i < spec.sheets.size(); ++i) {
                const assets::GridOptions& g = spec.sheets[i].grid;
                out += std::format("<div class=\"pr\"><span>sheet {} {}x{}</span><div class=\"btn sm\" data-sv=\"rm-sheet\" data-i=\"{}\" title=\"Remove from the atlas\"><span>Remove</span></div></div>", w::esc(spec.sheets[i].image), g.frame_w, g.frame_h, i);
            }
            if (at->dirty()) out += "<div class=\"opts\"><div class=\"btn sm primary\" data-sv=\"save\"><span>Save atlas</span></div></div>";
        } else if (sprite_.is_image() && sprite_.info().ok) {
            std::string err;
            const auto frames = sprite_.slice(err);
            out += "<div class=\"section\"><span>Slice sheet</span><span class=\"count\">" + (err.empty() ? std::format("{} frames, {}_0 ...", frames.size(), w::esc(sprite_.prefix)) : w::esc(err)) + "</span></div>";
            out += num_row("frame w", "sv-fw", sprite_.fw, "Frame width in pixels");
            out += num_row("frame h", "sv-fh", sprite_.fh, "Frame height in pixels");
            out += num_row("margin", "sv-margin", sprite_.margin, "Border around the whole grid");
            out += num_row("spacing", "sv-spacing", sprite_.spacing, "Gap between cells");
            out += std::format("<div class=\"prop\"><label><span class=\"dot\"></span><span class=\"lt\">prefix</span></label><div class=\"ctl\"><div class=\"tf\"><input type=\"text\" id=\"sv-prefix\" value=\"{}\"/></div></div></div>", w::esc(sprite_.prefix));
            out += std::format("<div class=\"prop\"><label><span class=\"dot\"></span><span class=\"lt\">atlas file</span></label><div class=\"ctl\"><div class=\"tf\"><input type=\"text\" id=\"sv-target\" value=\"{}\"/></div></div></div>", w::esc(sprite_.target));
            out += "<div class=\"opts\"><div class=\"btn sm primary\" data-sv=\"add-sheet\" title=\"Slice by this grid and add it to the atlas file\"><span>Add to atlas</span></div><div class=\"btn sm\" data-sv=\"add-whole\" title=\"Add the whole image as one sprite\"><span>Add whole image</span></div></div>";
        }
        if (!sprite_msg_.empty()) out += std::format("<div class=\"hint\">{}</div>", w::esc(sprite_msg_));
    }
    out += "</div></div>";
    e->SetInnerRML(out);
}

// Positions the image box from zoom and pan; fits once when a new image appears.
void Shell::sprite_apply() {
    Rml::Element* view = by_id("spv");
    Rml::Element* in = by_id("spv-in");
    if (view == nullptr || in == nullptr) { sprite_fit_ = false; return; }
    const auto shown = sprite_.shown(ctl_.env().project_dir());
    if (!shown) { sprite_fit_ = false; return; }
    const Rml::Vector2f sz = view->GetBox().GetSize(Rml::BoxArea::Content);
    if (sz.x < 8.0f || sz.y < 8.0f) return; // not laid out yet; try again next frame
    if (sprite_fit_) {
        sprite_.fit(sz.x, sz.y, static_cast<float>(shown->w), static_cast<float>(shown->h));
        sprite_fit_ = false;
    }
    const float z = sprite_.zoom;
    in->SetProperty("left", std::format("{:.2f}px", sprite_.pan_x));
    in->SetProperty("top", std::format("{:.2f}px", sprite_.pan_y));
    in->SetProperty("width", std::format("{:.2f}px", static_cast<float>(shown->w) * z));
    in->SetProperty("height", std::format("{:.2f}px", static_cast<float>(shown->h) * z));
    if (Rml::Element* zl = by_id("sv-zoom")) zl->SetInnerRML(std::format("{:.0f}%", z * 100.0f));
}

bool Shell::sprite_mousedown(Rml::Element* t, Rml::Vector2f at, int button) {
    if (!inside_id(t, "spv")) return false;
    if (button == 0 || button == 2) { sprite_pan_ = true; sprite_last_ = at; return true; }
    return false;
}

void Shell::sprite_mousemove(Rml::Vector2f at) {
    if (!sprite_pan_) return;
    sprite_.pan_x += at.x - sprite_last_.x;
    sprite_.pan_y += at.y - sprite_last_.y;
    sprite_last_ = at;
    sprite_apply();
}

void Shell::sprite_scroll(Rml::Element* t, Rml::Vector2f at, float dy) {
    Rml::Element* view = by_id("spv");
    if (view == nullptr || !inside_id(t, "spv") || dy == 0.0f) return;
    const Rml::Vector2f o = view->GetAbsoluteOffset(Rml::BoxArea::Content);
    sprite_.zoom_about(std::pow(1.2f, -std::clamp(dy, -3.0f, 3.0f)), at.x - o.x, at.y - o.y);
    sprite_apply();
}

bool Shell::sprite_click(Rml::Element* t) {
    Rml::Element* b = find_attr(t, "data-sv");
    if (b == nullptr || node_tab_ != "Sprite") return false;
    const std::string what = b->GetAttribute<Rml::String>("data-sv", "");
    const auto dir = ctl_.env().project_dir();
    Rml::Element* view = by_id("spv");
    const Rml::Vector2f sz = view != nullptr ? view->GetBox().GetSize(Rml::BoxArea::Content) : Rml::Vector2f{};
    if (what == "in" || what == "out") {
        sprite_.zoom_about(what == "in" ? 1.5f : 1.0f / 1.5f, sz.x * 0.5f, sz.y * 0.5f);
        sprite_apply();
    } else if (what == "fit") {
        sprite_fit_ = true;
        sprite_apply();
    } else if (what == "one") {
        sprite_.zoom_about(1.0f / sprite_.zoom, sz.x * 0.5f, sz.y * 0.5f);
        sprite_apply();
    } else if (what == "grid") {
        sprite_.show_grid = !sprite_.show_grid;
        build_node_drawer();
    } else if (what == "rm-sprite" || what == "rm-sheet") {
        const std::size_t i = static_cast<std::size_t>(b->GetAttribute<int>("data-i", 0));
        if (what == "rm-sprite") sprite_.remove_sprite(i); else sprite_.remove_sheet(i);
        build_node_drawer();
    } else if (what == "save") {
        std::string err;
        if (sprite_.save_atlas(err)) { ctl_.notify_ok("Saved " + sprite_.path()); ctl_.assets().poll(); sprite_.set_path(dir, sprite_.path(), true); }
        else ctl_.report_failure(err);
        build_node_drawer();
    } else if (what == "add-sheet" || what == "add-whole") {
        std::string msg;
        const bool ok = what == "add-sheet" ? sprite_.add_sheet(dir, msg) : sprite_.add_whole(dir, msg);
        if (ok) {
            ctl_.notify_ok(msg);
            ctl_.assets().poll();
            sprite_.set_path(dir, sprite_.target); // show the atlas that now has the frames
        } else {
            ctl_.report_failure(msg);
            sprite_msg_ = msg;
        }
        build_node_drawer();
    } else {
        return false;
    }
    return true;
}

bool Shell::sprite_change(Rml::Element* t, const std::string& value, bool enter) {
    const std::string id = t != nullptr ? t->GetId() : std::string();
    if (id.rfind("sv-", 0) != 0) return false;
    const auto num = [&](int lo, int hi) { return std::clamp(std::atoi(value.c_str()), lo, hi); };
    const int mw = std::max(sprite_.info().width, 1), mh = std::max(sprite_.info().height, 1);
    if (id == "sv-fw") sprite_.fw = num(1, mw);
    else if (id == "sv-fh") sprite_.fh = num(1, mh);
    else if (id == "sv-margin") sprite_.margin = num(0, 256);
    else if (id == "sv-spacing") sprite_.spacing = num(0, 256);
    else if (id == "sv-prefix") sprite_.prefix = value;
    else if (id == "sv-target") sprite_.target = value;
    else return false;
    // Grid values change the overlay: rebuild on Enter (typing keeps the focus in the field).
    if (enter) build_node_drawer();
    return true;
}

} // namespace cuervo::editor

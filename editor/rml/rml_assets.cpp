// The Assets pane of the RmlUi shell: the project's folder tree with a filter, thumbnails for images,
// a preview of the selected asset, a context menu, folder / rename / delete dialogs, and drag-out of an
// asset onto the scene view (instances a scene) or an inspector text field (stores the path). All file
// operations go through AssetLibrary; the scene drop goes through EditorController.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <string>

#include "cuervo/platform/platform.hpp"
#include "editor/icons.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"

namespace cuervo::editor {

using namespace dom;
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const char* kind_icon(AssetKind k) {
    switch (k) {
    case AssetKind::Folder: return "folder";
    case AssetKind::Scene: return "layout";
    case AssetKind::Script: return "script";
    case AssetKind::Vector: return "shape";
    case AssetKind::Image:
    case AssetKind::Atlas: return "sprite";
    case AssetKind::Font: return "text";
    case AssetKind::Sound: return "audio";
    case AssetKind::Replay: return "history";
    case AssetKind::Project: return "project";
    case AssetKind::Other: break;
    }
    return "path";
}

std::string folder_of(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

} // namespace

std::string Shell::asset_thumb_src(const std::string& path, int px) const {
    std::error_code ec;
    const fs::path abs = ctl_.assets().absolute(path);
    const auto t = fs::last_write_time(abs, ec);
    const long long stamp = ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
    return std::format("file:{}:{}?{}", px, platform::utf8(abs), stamp);
}

bool Shell::asset_open(int depth, const AssetEntry& e) const {
    const auto it = assets_open_.find(e.path);
    if (it != assets_open_.end()) return it->second;
    return depth == 0; // top-level folders start open
}

bool Shell::asset_matches(const AssetEntry& e, const std::string& q) const {
    if (q.empty()) return true;
    if (e.kind != AssetKind::Folder) return lower(e.path).find(q) != std::string::npos;
    return std::any_of(e.children.begin(), e.children.end(), [&](const AssetEntry& c) { return asset_matches(c, q); });
}

void Shell::build_assets() {
    assets_rev_ = ctl_.assets().revision();
    Rml::Element* list = by_id("assets");
    if (list == nullptr) return;
    const std::string q = lower(assets_filter_);
    std::string out;
    int rows = 0;
    const auto walk = [&](auto&& self, const AssetEntry& e, int depth) -> void {
        for (const AssetEntry& c : e.children) {
            if (rows > 3000) return;
            if (!asset_matches(c, q)) continue;
            const bool folder = c.kind == AssetKind::Folder;
            const bool open = folder && (!q.empty() || asset_open(depth, c));
            const std::string cls = std::string("ar") + (c.path == assets_sel_ ? " sel" : "");
            std::string lead;
            if (folder) lead = std::format("<span class=\"chev\" data-achev=\"{}\">{}</span>", w::esc(c.path), icon_img(open ? "chevron_down" : "chevron_right", 12, "chev"));
            else lead = "<span class=\"chev\"></span>";
            std::string glyph;
            if (c.kind == AssetKind::Image) glyph = std::format("<span class=\"thumb\"><img src=\"{}\"/></span>", w::esc(asset_thumb_src(c.path, 36)));
            else glyph = icon_img(kind_icon(c.kind), 14);
            out += std::format("<div class=\"{}\" style=\"padding-left: {}dp\" data-apath=\"{}\" data-kind=\"{}\">{}{}<span class=\"an\">{}</span></div>", cls, 6 + depth * 12, w::esc(c.path), static_cast<int>(c.kind), lead, glyph, w::esc(c.name));
            ++rows;
            if (open) self(self, c, depth + 1);
        }
    };
    walk(walk, ctl_.assets().root(), 0);
    if (out.empty()) {
        out = q.empty() ? "<div class=\"hint\">No assets yet. Files in the project folder appear here.</div>" : "<div class=\"hint\">No asset matches the filter.</div>";
    }
    list->SetInnerRML(out);
    build_asset_preview();
}

void Shell::build_asset_preview() {
    Rml::Element* p = by_id("asset-prev");
    if (p == nullptr) return;
    const AssetEntry* e = assets_sel_.empty() ? nullptr : ctl_.assets().find(assets_sel_);
    if (e == nullptr || e->kind == AssetKind::Folder) {
        p->SetProperty("display", "none");
        return;
    }
    std::error_code ec;
    const auto bytes = fs::file_size(ctl_.assets().absolute(e->path), ec);
    std::string size = ec ? std::string() : bytes < 1024 ? std::format("{} B", bytes) : std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
    std::string out;
    if (e->kind == AssetKind::Image) out += std::format("<div class=\"pv\"><img src=\"{}\"/></div>", w::esc(asset_thumb_src(e->path, 192)));
    out += std::format("<div class=\"pm\"><span class=\"pn\">{}</span><span class=\"src\">{} {}</span></div>", w::esc(e->path), std::string(asset_kind_name(e->kind)), size);
    p->SetInnerRML(out);
    p->SetProperty("display", "block");
}

std::string Shell::asset_selected_folder() const {
    const AssetEntry* e = assets_sel_.empty() ? nullptr : ctl_.assets().find(assets_sel_);
    if (e == nullptr) return {};
    return e->kind == AssetKind::Folder ? e->path : folder_of(e->path);
}

void Shell::assets_filter(const std::string& value) {
    assets_filter_ = value;
    build_assets();
}

void Shell::asset_activate(const std::string& path) {
    const AssetEntry* e = ctl_.assets().find(path);
    if (e == nullptr) return;
    if (e->kind == AssetKind::Folder) {
        assets_open_[path] = !asset_open(static_cast<int>(std::count(path.begin(), path.end(), '/')), *e);
        build_assets();
    } else if (e->kind == AssetKind::Scene) ctl_.request_open_scene(path);
    else if (e->kind == AssetKind::Script) open_script(ctl_.assets().absolute(path));
    else if (e->kind == AssetKind::Replay) play_replay(path);
    else if (e->kind == AssetKind::Image || e->kind == AssetKind::Atlas || e->kind == AssetKind::Vector) show_sprite_asset(path);
}

// Double-click on an image, atlas or SVG: the Sprites tab previews it (ImGui: opens the Sprites panel).
void Shell::show_sprite_asset(const std::string& path) {
    sprite_pick_ = path;
    sprite_asset_.clear(); // reload even when the same asset is picked again
    layout_.drawer_open = true;
    layout_.drawer = "sprite";
    apply_layout();
    save_layout();
}

// Starts the replay and shows the Replay tab (ImGui: opens the Replay panel).
void Shell::play_replay(const std::string& path) {
    rp_path_ = path;
    if (!ctl_.start_replay(path)) return;
    layout_.drawer_open = true;
    layout_.drawer = "replay";
    rp_dirty_ = true;
    apply_layout();
}

bool Shell::assets_mousedown(Rml::Element* t, Rml::Vector2f at, int button) {
    Rml::Element* row = find_attr(t, "data-apath");
    if (row == nullptr || !inside(t, "assets")) return false;
    const std::string path = row->GetAttribute<Rml::String>("data-apath", "");
    if (Rml::Element* c = find_attr(t, "data-achev"); c != nullptr && button == 0) {
        const AssetEntry* e = ctl_.assets().find(path);
        if (e != nullptr) assets_open_[path] = !asset_open(static_cast<int>(std::count(path.begin(), path.end(), '/')), *e);
        build_assets();
        return true;
    }
    assets_sel_ = path;
    if (button == 1) {
        build_assets();
        open_asset_menu(path, at);
        return true;
    }
    const std::uint32_t now = SDL_GetTicks();
    const bool twice = path == asset_last_click_ && now - asset_last_ms_ < 400;
    asset_last_click_ = path;
    asset_last_ms_ = now;
    build_assets();
    if (twice) {
        asset_last_click_.clear();
        asset_activate(path);
        return true;
    }
    asset_press_ = path;
    asset_press_at_ = at;
    return true;
}

void Shell::assets_mousemove(Rml::Element* t, Rml::Vector2f at) {
    if (asset_press_.empty()) return;
    const float ratio = ctx_->GetDensityIndependentPixelRatio();
    const float dx = at.x - asset_press_at_.x, dy = at.y - asset_press_at_.y;
    if (!asset_dragging_ && dx * dx + dy * dy > 25.0f * ratio * ratio) {
        const AssetEntry* e = ctl_.assets().find(asset_press_);
        if (e == nullptr || e->kind == AssetKind::Folder) { asset_press_.clear(); return; }
        asset_dragging_ = true;
        hide_tip();
        if (Rml::Element* g = by_id("ghost")) {
            g->SetInnerRML(std::format("{}<span>{}</span>", icon_img(kind_icon(e->kind), 14), w::esc(e->name)));
            g->SetProperty("display", "flex");
        }
    }
    if (!asset_dragging_) return;
    if (Rml::Element* g = by_id("ghost")) {
        g->SetProperty("left", std::format("{}px", static_cast<int>(at.x + 12)));
        g->SetProperty("top", std::format("{}px", static_cast<int>(at.y + 8)));
    }
    // Highlight the inspector text field under the pointer (a drop stores the path there).
    Rml::Element* target = nullptr;
    if (Rml::Element* f = find_attr(t, "data-field"); f != nullptr && inside(t, "inspector")) {
        if (target_value(f->GetAttribute<Rml::String>("data-field", "")).kind == scene::ValueKind::Str) target = f;
    }
    if (target != asset_drop_el_) {
        if (asset_drop_el_ != nullptr) asset_drop_el_->SetClass("drop", false);
        asset_drop_el_ = target;
        if (target != nullptr) target->SetClass("drop", true);
    }
    const bool over_view = host_.world_at && host_.world_at(at.x, at.y).has_value();
    if (Rml::Element* v = by_id("canvas")) v->SetClass("drop", over_view);
}

void Shell::assets_mouseup(Rml::Element* t, Rml::Vector2f at) {
    const bool dragged = asset_dragging_;
    const std::string path = asset_press_;
    asset_press_.clear();
    asset_dragging_ = false;
    if (Rml::Element* g = by_id("ghost")) g->SetProperty("display", "none");
    if (Rml::Element* v = by_id("canvas")) v->SetClass("drop", false);
    if (asset_drop_el_ != nullptr) asset_drop_el_->SetClass("drop", false);
    Rml::Element* target = asset_drop_el_;
    asset_drop_el_ = nullptr;
    if (!dragged || path.empty()) return;
    if (target != nullptr) {
        const std::string key = target->GetAttribute<Rml::String>("data-field", "");
        with_target(key, [&](scene::Value& v) { if (v.kind == scene::ValueKind::Str) v.text = path; }, false);
        return;
    }
    if (inside(t, "canvas") || inside(t, "view")) {
        if (const auto world = host_.world_at ? host_.world_at(at.x, at.y) : std::nullopt) ctl_.drop_asset_on_scene(path, *world);
    }
}

void Shell::open_asset_menu(const std::string& path, Rml::Vector2f at) {
    Rml::Element* p = by_id("popup");
    const AssetEntry* e = ctl_.assets().find(path);
    if (p == nullptr || e == nullptr) return;
    const bool folder = e->kind == AssetKind::Folder;
    std::vector<w::MenuItem> items;
    if (e->kind == AssetKind::Scene) {
        items.push_back({"open", "Open", ""});
        items.push_back({"instance", "Instance in the scene", ""});
    }
    if (e->kind == AssetKind::Replay) items.push_back({"replay", "Play replay", ""});
    if (e->kind == AssetKind::Script || folder) items.push_back({"code", "Open in code editor", ""});
    items.push_back({"folder", "New folder...", ""});
    items.push_back({"rename", "Rename...", ""});
    items.push_back({"delete", "Delete...", ""});
    items.push_back({"copy", "Copy path", ""});
    asset_ctx_ = path;
    p->SetInnerRML(w::popup_menu("data-actx", items));
    p->SetProperty("width", "200dp");
    p->SetProperty("left", std::format("{}px", static_cast<int>(at.x)));
    p->SetProperty("top", std::format("{}px", static_cast<int>(at.y)));
    p->SetProperty("display", "block");
    popup_open_ = true;
    palette_ = false;
}

void Shell::run_asset_menu(const std::string& key) {
    const std::string path = asset_ctx_;
    const AssetEntry* e = ctl_.assets().find(path);
    if (e == nullptr) return;
    if (key == "open") ctl_.request_open_scene(path);
    else if (key == "replay") play_replay(path);
    else if (key == "instance") ctl_.instance_scene_asset(path);
    else if (key == "code") open_script(ctl_.assets().absolute(path));
    else if (key == "folder") { asset_op_path_ = e->kind == AssetKind::Folder ? path : folder_of(path); open_dialog(Dialog::AssetFolder); }
    else if (key == "rename") { asset_op_path_ = path; open_dialog(Dialog::AssetRename); }
    else if (key == "delete") { asset_op_path_ = path; open_dialog(Dialog::AssetDelete); }
    else if (key == "copy") { SDL_SetClipboardText(path.c_str()); ctl_.notify_ok("Copied the path."); }
}

std::string Shell::asset_dialog_html(Dialog d) {
    const auto buttons = [](const char* ok) {
        return std::format("<div class=\"dbtns\"><button class=\"btn\" data-dlg=\"cancel\"><span>Cancel</span></button><button class=\"btn primary\" data-dlg=\"ok\"><span>{}</span></button></div>", ok);
    };
    asset_error_.clear();
    if (d == Dialog::AssetFolder) {
        dialog_text_ = "new_folder";
        return std::format("<div class=\"dt\">New folder</div><div class=\"dm\">In {}.</div><input id=\"dlg-field\" type=\"text\" value=\"{}\"/><div id=\"dlg-err\" class=\"derr\"></div>{}",
                           asset_op_path_.empty() ? std::string("the project folder") : w::esc(asset_op_path_), dialog_text_, buttons("Create"));
    }
    if (d == Dialog::AssetImport) {
        dialog_text_.clear();
        return std::format("<div class=\"dt\">Import file</div><div class=\"dm\">Path of an image, SVG, sound or font. It is copied into the project (art, sounds or fonts); you can also drop files on the window.</div><input id=\"dlg-field\" type=\"text\" value=\"\" placeholder=\"/path/to/file.png\"/><div id=\"dlg-err\" class=\"derr\"></div>{}", buttons("Import"));
    }
    if (d == Dialog::AssetRename) {
        const AssetEntry* e = ctl_.assets().find(asset_op_path_);
        dialog_text_ = e != nullptr ? e->name : std::string();
        return std::format("<div class=\"dt\">Rename</div><div class=\"dm\">{}</div><input id=\"dlg-field\" type=\"text\" value=\"{}\"/><div id=\"dlg-err\" class=\"derr\"></div>{}", w::esc(asset_op_path_), w::esc(dialog_text_), buttons("Rename"));
    }
    const AssetEntry* e = ctl_.assets().find(asset_op_path_);
    const bool folder = e != nullptr && e->kind == AssetKind::Folder;
    return std::format("<div class=\"dt\">Delete {}?</div><div class=\"dm\">{}{} is removed from disk. This cannot be undone.</div>{}", folder ? "folder" : "file", w::esc(asset_op_path_), folder ? " and everything in it" : "", buttons("Delete"));
}

bool Shell::asset_dialog_confirm(Dialog d) {
    std::string error;
    bool ok = false;
    if (d == Dialog::AssetImport) {
        const EditorController::ImportSummary sum = ctl_.import_files({platform::path_from_utf8(dialog_text_)});
        if (sum.imported == 0) {
            if (Rml::Element* x = by_id("dlg-err")) x->SetInnerRML(w::esc(sum.error.empty() ? std::string("Enter the path of a file.") : sum.error));
            return false;
        }
        assets_sel_ = sum.first_dest;
        build_assets();
        return true;
    }
    if (d == Dialog::AssetFolder) ok = !dialog_text_.empty() && ctl_.assets().create_folder(asset_op_path_, dialog_text_, error);
    else if (d == Dialog::AssetRename) ok = ctl_.assets().rename(asset_op_path_, dialog_text_, error);
    else ok = ctl_.assets().remove(asset_op_path_, error);
    if (!ok) {
        if (error.empty()) error = "Enter a name.";
        if (Rml::Element* x = by_id("dlg-err")) x->SetInnerRML(w::esc(error));
        else ctl_.report_failure(error);
        return false;
    }
    if (d == Dialog::AssetRename) assets_sel_ = (folder_of(asset_op_path_).empty() ? std::string() : folder_of(asset_op_path_) + "/") + dialog_text_;
    if (d == Dialog::AssetDelete && assets_sel_ == asset_op_path_) assets_sel_.clear();
    if (d == Dialog::AssetFolder) assets_open_[asset_op_path_] = true;
    build_assets();
    return true;
}

void Shell::import_dropped(const std::vector<std::filesystem::path>& files) {
    const EditorController::ImportSummary sum = ctl_.import_files(files);
    if (sum.imported == 0) return;
    assets_sel_ = sum.first_dest;
    show_rail("assets", false);
    build_assets();
}

} // namespace cuervo::editor

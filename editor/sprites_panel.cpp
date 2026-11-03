// The Sprites panel (bottom dock; DESIGN.md 4.13, milestone 11.7c): image
// preview, the atlas and sprite-sheet slice tools, and the frame / 9-slice
// helpers for the selected Sprite2D, AnimatedSprite2D or NinePatchRect.
//
//  - It shows the image or atlas frame the selected node names (or the one
//    double-clicked in Assets, or typed / dropped in the path field), through
//    the SpriteLibrary the scene view draws with, at any zoom, with the slice
//    grid or the 9-slice margins drawn over it.
//  - An image can be sliced by a grid (frame size, margin, spacing) into an
//    `.atlas` source file; an `.atlas` lists its sprites and sheets, takes
//    more, and saves (AtlasEditor in sprite_tools.hpp, the same code the
//    `atlas` command-script commands run).
//  - The selected node gets a frame picker, an AnimatedSprite2D frame list
//    built from the atlas, and sliders for the 9-slice margins. All of it goes
//    through EditorDocument::set_field, so it is undoable and saved in the scene.

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "editor/app.hpp"
#include "editor/panels.hpp"
#include "editor/sprite_tools.hpp"
#include "editor/imgui_theme.hpp"

namespace cuervo::editor {

namespace {

bool ends_with_any(const std::string& s, std::initializer_list<const char*> exts) {
    for (const char* e : exts) {
        if (s.ends_with(e)) return true;
    }
    return false;
}

class SpritesPanel final : public Panel {
public:
    const char* title() const override { return "Sprites"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        EditorDocument& doc = app.doc();
        follow(app);

        ImGui::SetNextItemWidth(-130.0f * theme::ui_scale());
        if (ImGui::InputTextWithHint("##asset", "image, atlas or SVG (drop one from Assets)", &path_)) changed_ = true;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(asset_payload)) {
                path_ = static_cast<const char*>(p->Data);
                changed_ = true;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Button("Use selection##sel")) adopt_selection(app);
        if (changed_) reload(app);
        if (path_.empty()) {
            ImGui::TextDisabled("Select a Sprite2D, AnimatedSprite2D or NinePatchRect, or double-click an image in Assets.");
            return;
        }

        const NodeId sel = doc.selection();
        const ModelNode* node = doc.model().get(sel);
        const ClassRegistry& reg = app.env().classes();
        const bool is_sprite = node != nullptr && reg.is_a(node->type, "Sprite2D");
        const bool is_anim = node != nullptr && reg.is_a(node->type, "AnimatedSprite2D");
        const bool is_nine = node != nullptr && reg.is_a(node->type, "NinePatchRect");

        const float left_w = std::max(ImGui::GetContentRegionAvail().x * 0.55f, 200.0f);
        ImGui::BeginChild("##preview", {left_w, 0.0f}, ImGuiChildFlags_Borders);
        draw_preview(app, is_nine ? sel : NodeId{0});
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##tools", {0.0f, 0.0f}, ImGuiChildFlags_Borders);
        if (is_sprite || is_anim || is_nine) node_tools(app, sel, is_anim, is_nine);
        if (atlas_) {
            atlas_tools(app);
        } else if (ends_with_any(path_, {".png", ".jpg", ".jpeg", ".qoi"})) {
            slice_tools(app);
        }
        ImGui::EndChild();
    }

private:
    // The selection changing to a sprite node points the panel at its image;
    // a request from Assets wins.
    void follow(EditorApp& app) {
        if (std::string req = app.take_sprite_request(); !req.empty()) {
            path_ = std::move(req);
            changed_ = true;
        }
        const NodeId sel = app.doc().selection();
        if (sel != last_selection_) {
            last_selection_ = sel;
            adopt_selection(app);
        }
        if (app.assets().revision() != assets_revision_) {
            assets_revision_ = app.assets().revision();
            changed_ = true;
        }
    }

    void adopt_selection(EditorApp& app) {
        const EditorDocument& doc = app.doc();
        const ModelNode* n = doc.model().get(doc.selection());
        if (n == nullptr) return;
        const ClassRegistry& reg = app.env().classes();
        if (!reg.is_a(n->type, "Sprite2D") && !reg.is_a(n->type, "AnimatedSprite2D") && !reg.is_a(n->type, "NinePatchRect")) {
            return;
        }
        const scene::Value a = doc.field_value(doc.selection(), "asset");
        if (a.kind == scene::ValueKind::Str && a.text != path_) {
            path_ = a.text;
            changed_ = true;
        }
    }

    // Re-reads everything derived from the path: size, frame names, the atlas.
    void reload(EditorApp& app) {
        changed_ = false;
        info_ = ImageInfo{};
        frames_.clear();
        atlas_.reset();
        status_.clear();
        if (path_.empty()) return;
        const auto dir = app.env().project_dir();
        if (ends_with_any(path_, {".png", ".jpg", ".jpeg", ".qoi"})) {
            info_ = image_info(dir, path_);
            if (!info_.ok) status_ = info_.error;
            if (atlas_target_.empty()) atlas_target_ = path_.substr(0, path_.rfind('.')) + ".atlas";
        } else if (path_.ends_with(".atlas")) {
            std::string error;
            atlas_ = AtlasEditor::open(dir, path_, error);
            if (!atlas_) {
                status_ = error;
            } else {
                frames_ = atlas_->frame_names(&status_);
            }
        } else if (path_.ends_with(".catlas")) {
            std::string error;
            frames_ = atlas_frame_names(dir, path_, error);
            status_ = error;
        }
        if (frame_.empty() || std::find(frames_.begin(), frames_.end(), frame_) == frames_.end()) {
            frame_ = frames_.empty() ? std::string() : frames_.front();
        }
    }

    void draw_preview(EditorApp& app, NodeId nine_patch) {
        ImGui::TextDisabled("Zoom");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f * theme::ui_scale());
        ImGui::SliderFloat("##zoom", &zoom_, 0.25f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
        if (!frames_.empty()) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f * theme::ui_scale());
            if (ImGui::BeginCombo("##frame", frame_.c_str())) {
                for (const std::string& f : frames_) {
                    if (ImGui::Selectable(f.c_str(), f == frame_)) frame_ = f;
                }
                ImGui::EndCombo();
            }
        }
        const bool whole = frames_.empty();
        const render::SpriteRef ref = app.sprite_preview(path_, whole ? std::string_view{} : std::string_view(frame_));
        if (!ref.ok) {
            ImGui::TextColored(theme::im(theme::colors().danger), "%s", status_.empty() ? "Cannot load this file." : status_.c_str());
            return;
        }
        const float w = ref.size.x() * zoom_;
        const float h = ref.size.y() * zoom_;
        ImGui::TextDisabled("%s%s%s  %.0f x %.0f", path_.c_str(), whole ? "" : "  ", whole ? "" : frame_.c_str(), ref.size.x(), ref.size.y());
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy({std::max(w, 1.0f), std::max(h, 1.0f)});
        ImDrawList* dl = ImGui::GetWindowDrawList();
        checkerboard(dl, origin, {origin.x + w, origin.y + h});
        const ImTextureRef tex(static_cast<ImTextureID>(ref.texture.id));
        dl->AddImage(tex, origin, {origin.x + w, origin.y + h}, {ref.uv.u0, ref.uv.v0}, {ref.uv.u1, ref.uv.v1});
        dl->AddRect(origin, {origin.x + w, origin.y + h}, ImGui::GetColorU32(ImGuiCol_Border));

        const ImU32 grid = IM_COL32(255, 200, 40, 200);
        if (slice_preview_ && ends_with_any(path_, {".png", ".jpg", ".jpeg", ".qoi"}) && info_.ok) {
            assets::GridOptions g = grid_options();
            const auto sheet = assets::slice_grid(info_.width, info_.height, g);
            if (sheet.sheet) {
                for (const assets::SheetFrame& f : sheet.sheet->frames) {
                    dl->AddRect({origin.x + static_cast<float>(f.x) * zoom_, origin.y + static_cast<float>(f.y) * zoom_},
                                {origin.x + static_cast<float>(f.x + f.w) * zoom_, origin.y + static_cast<float>(f.y + f.h) * zoom_}, grid);
                }
            }
        }
        if (nine_patch != 0) {
            // The four margins as guide lines (texels of the frame).
            const EditorDocument& doc = app.doc();
            const auto m = [&](const char* f) { return static_cast<float>(doc.field_value(nine_patch, f).f); };
            const float l = m("margin_left") * zoom_;
            const float t = m("margin_top") * zoom_;
            const float r = m("margin_right") * zoom_;
            const float b = m("margin_bottom") * zoom_;
            const ImU32 c = IM_COL32(80, 200, 255, 230);
            dl->AddLine({origin.x + l, origin.y}, {origin.x + l, origin.y + h}, c);
            dl->AddLine({origin.x + w - r, origin.y}, {origin.x + w - r, origin.y + h}, c);
            dl->AddLine({origin.x, origin.y + t}, {origin.x + w, origin.y + t}, c);
            dl->AddLine({origin.x, origin.y + h - b}, {origin.x + w, origin.y + h - b}, c);
        }
    }

    static void checkerboard(ImDrawList* dl, ImVec2 a, ImVec2 b) {
        const float cell = 8.0f * theme::ui_scale();
        const ImU32 c0 = IM_COL32(60, 60, 66, 255);
        const ImU32 c1 = IM_COL32(86, 86, 94, 255);
        dl->PushClipRect(a, b, true);
        int row = 0;
        for (float y = a.y; y < b.y; y += cell, ++row) {
            int col = row;
            for (float x = a.x; x < b.x; x += cell, ++col) {
                dl->AddRectFilled({x, y}, {x + cell, y + cell}, (col & 1) != 0 ? c1 : c0);
            }
        }
        dl->PopClipRect();
    }

    assets::GridOptions grid_options() const {
        assets::GridOptions g;
        g.frame_w = fw_;
        g.frame_h = fh_;
        g.margin = margin_;
        g.spacing = spacing_;
        g.prefix = prefix_;
        return g;
    }

    // Slice an image sheet into an atlas.
    void slice_tools(EditorApp& app) {
        ImGui::SeparatorText("Slice sheet");
        if (!info_.ok) return;
        ImGui::Checkbox("Show grid", &slice_preview_);
        ImGui::SetNextItemWidth(90.0f * theme::ui_scale());
        ImGui::DragInt("Frame W", &fw_, 0.5f, 1, std::max(info_.width, 1));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f * theme::ui_scale());
        ImGui::DragInt("H", &fh_, 0.5f, 1, std::max(info_.height, 1));
        ImGui::SetNextItemWidth(90.0f * theme::ui_scale());
        ImGui::DragInt("Margin", &margin_, 0.25f, 0, 256);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f * theme::ui_scale());
        ImGui::DragInt("Spacing", &spacing_, 0.25f, 0, 256);
        ImGui::SetNextItemWidth(160.0f * theme::ui_scale());
        ImGui::InputText("Name prefix", &prefix_);
        const auto sheet = assets::slice_grid(info_.width, info_.height, grid_options());
        if (sheet.sheet) {
            ImGui::TextDisabled("%zu frames (%s_0 ... )", sheet.sheet->frames.size(), prefix_.c_str());
        } else {
            ImGui::TextColored(theme::im(theme::colors().danger), "%s", sheet.error.c_str());
        }
        ImGui::SetNextItemWidth(-90.0f * theme::ui_scale());
        ImGui::InputText("##target", &atlas_target_);
        ImGui::SameLine();
        if (ImGui::Button("Add to atlas") && sheet.sheet) {
            std::string error;
            const auto dir = app.env().project_dir();
            const bool exists = std::filesystem::exists(dir / atlas_target_);
            std::optional<AtlasEditor> target = exists ? AtlasEditor::open(dir, atlas_target_, error)
                                                       : std::optional<AtlasEditor>(AtlasEditor::create(dir, atlas_target_));
            if (target && target->add_sheet(path_, grid_options(), error) && target->save(error)) {
                app.notify_ok(std::format("Added {} frames to {}", sheet.sheet->frames.size(), atlas_target_));
                app.assets().poll();
                path_ = atlas_target_;
                changed_ = true;
            } else {
                app.report_failure(error);
            }
        }
        if (ImGui::Button("Add whole image as a sprite")) {
            std::string error;
            const auto dir = app.env().project_dir();
            const bool exists = std::filesystem::exists(dir / atlas_target_);
            std::optional<AtlasEditor> target = exists ? AtlasEditor::open(dir, atlas_target_, error)
                                                       : std::optional<AtlasEditor>(AtlasEditor::create(dir, atlas_target_));
            if (target && target->add_sprite(path_, "", 1.0f, error) && target->save(error)) {
                app.notify_ok(std::format("Added {} to {}", path_, atlas_target_));
                app.assets().poll();
                path_ = atlas_target_;
                changed_ = true;
            } else {
                app.report_failure(error);
            }
        }
    }

    void atlas_tools(EditorApp& app) {
        ImGui::SeparatorText("Atlas");
        const assets::AtlasSpec& spec = atlas_->spec();
        bool dirty = false;
        for (std::size_t i = 0; i < spec.sprites.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("sprite  %s%s", spec.sprites[i].path.c_str(), spec.sprites[i].name.empty() ? "" : std::format("  as {}", spec.sprites[i].name).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove") && atlas_->remove_sprite(i)) dirty = true;
            ImGui::PopID();
            if (dirty) break;
        }
        for (std::size_t i = 0; i < spec.sheets.size() && !dirty; ++i) {
            ImGui::PushID(1000 + static_cast<int>(i));
            const assets::GridOptions& g = spec.sheets[i].grid;
            ImGui::Text("sheet   %s  %dx%d  %s_*", spec.sheets[i].image.c_str(), g.frame_w, g.frame_h, g.prefix.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove") && atlas_->remove_sheet(i)) dirty = true;
            ImGui::PopID();
        }
        if (dirty) frames_ = atlas_->frame_names(&status_);
        ImGui::Text("%zu frames", frames_.size());
        if (atlas_->dirty()) {
            ImGui::SameLine();
            if (ImGui::Button("Save atlas")) {
                std::string error;
                if (atlas_->save(error)) {
                    app.notify_ok("Saved " + path_);
                    app.assets().poll();
                    changed_ = false;
                } else {
                    app.report_failure(error);
                }
            }
        }
    }

    // Frame picker / frame list / 9-slice margins of the selected node.
    void node_tools(EditorApp& app, NodeId id, bool is_anim, bool is_nine) {
        EditorDocument& doc = app.doc();
        ImGui::SeparatorText("Selected node");
        if (!frames_.empty() && !is_anim) {
            const std::string current = doc.field_value(id, "frame").text;
            ImGui::SetNextItemWidth(160.0f * theme::ui_scale());
            if (ImGui::BeginCombo("Frame", current.c_str())) {
                for (const std::string& f : frames_) {
                    if (ImGui::Selectable(f.c_str(), f == current)) app.report(doc.set_field(id, "frame", scene::Value::str(f)));
                }
                ImGui::EndCombo();
            }
        }
        if (is_anim) {
            const scene::Value frames = doc.field_value(id, "frames");
            ImGui::Text("%zu frames in the animation", frames.items.size());
            if (!frames_.empty()) {
                if (ImGui::Button("Use all frames")) set_frames(app, id, frames_);
                ImGui::SameLine();
                if (ImGui::Button("Add this frame") && !frame_.empty()) {
                    std::vector<std::string> now;
                    for (const scene::Value& v : frames.items) now.push_back(v.text);
                    now.push_back(frame_);
                    set_frames(app, id, now);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear")) set_frames(app, id, {});
        }
        if (is_nine) {
            const char* names[] = {"margin_left", "margin_top", "margin_right", "margin_bottom"};
            const float limit = static_cast<float>(std::max(info_.width, 1));
            for (const char* name : names) {
                float v = static_cast<float>(doc.field_value(id, name).f);
                ImGui::SetNextItemWidth(120.0f * theme::ui_scale());
                if (ImGui::DragFloat(name, &v, 0.25f, 0.0f, limit, "%.0f")) {
                    app.report(doc.set_field(id, name, scene::Value::number(v), true));
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) doc.seal();
            }
        }
    }

    void set_frames(EditorApp& app, NodeId id, const std::vector<std::string>& names) {
        std::vector<scene::Value> items;
        for (const std::string& n : names) items.push_back(scene::Value::str(n));
        app.report(app.doc().set_field(id, "frames", scene::Value::list(std::move(items))));
        app.doc().seal();
    }

    std::string path_;
    std::string frame_;
    std::string status_;
    std::vector<std::string> frames_;
    ImageInfo info_;
    std::optional<AtlasEditor> atlas_;
    bool changed_ = false;
    float zoom_ = 2.0f;
    NodeId last_selection_ = 0;
    std::uint64_t assets_revision_ = 0;
    // slice tool
    bool slice_preview_ = true;
    int fw_ = 16;
    int fh_ = 16;
    int margin_ = 0;
    int spacing_ = 0;
    std::string prefix_ = "frame";
    std::string atlas_target_;
};

} // namespace

std::unique_ptr<Panel> make_sprites_panel() { return std::make_unique<SpritesPanel>(); }

} // namespace cuervo::editor

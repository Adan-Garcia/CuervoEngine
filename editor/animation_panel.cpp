// The animation timeline (bottom dock; DESIGN.md 4.17 "Clarified (Milestone
// 9.8)"): edits the clips of the selected AnimationPlayer (or the player under
// the selected node). A clip picker with length and loop mode, one row per
// track with its keys on a tick ruler, a playhead that poses the scene view
// (Preview), and an inspector for the selected key. Every change rewrites the
// player's `animations` field through the document, so it is one undo step
// (a key drag merges into one) and saves with the scene. "Key" records the
// track's field as the document currently stores it, at the playhead.

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/anim_edit.hpp"
#include "editor/app.hpp"
#include "editor/panels.hpp"
#include "editor/theme.hpp"

namespace cuervo::editor {

namespace {

ImU32 col(rhi::Color c, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a * alpha));
}

bool combo(const char* label, std::string& value, const std::vector<std::string_view>& names) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.c_str())) {
        for (std::string_view n : names) {
            const bool selected = n == value;
            if (ImGui::Selectable(std::string(n).c_str(), selected)) {
                value = std::string(n);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

class AnimationPanel final : public Panel {
public:
    const char* title() const override { return "Animation"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        EditorDocument& doc = app.doc();
        const NodeId player = find_player(app);
        EditorApp::AnimPreview& preview = app.anim_preview();
        if (player == 0) {
            if (preview.on) stop_preview(app);
            ImGui::TextDisabled("Select an AnimationPlayer (or a node with one as a child) to edit its animations.");
            return;
        }
        if (player != player_) {
            player_ = player;
            clip_ = track_ = key_ = -1;
            if (preview.on) stop_preview(app);
        }
        std::vector<anim::Clip> clips = anim::read(doc.field_value(player, "animations"));
        if (clip_ >= static_cast<int>(clips.size())) clip_ = -1;
        if (clip_ < 0 && !clips.empty()) clip_ = 0;

        draw_clip_bar(app, clips);
        if (clip_ < 0) {
            ImGui::TextDisabled("No animations yet: add one with New.");
            return;
        }
        anim::Clip& clip = clips[static_cast<std::size_t>(clip_)];
        if (track_ >= static_cast<int>(clip.tracks.size())) track_ = -1;
        if (track_ < 0 || key_ >= static_cast<int>(clip.tracks[static_cast<std::size_t>(track_)].keys.size())) key_ = -1;
        playhead_ = std::clamp(playhead_, 0, std::max(clip.length, 0));
        if (preview.on) {
            preview.player = player;
            preview.clip = clip.name;
            preview.tick = playhead_;
        }

        const float bottom = ImGui::GetFrameHeightWithSpacing() * 3.0f + 8.0f;
        if (ImGui::BeginChild("##timeline", ImVec2(0.0f, -bottom), ImGuiChildFlags_Borders)) {
            draw_timeline(app, clips);
        }
        ImGui::EndChild();
        draw_track_tools(app, clips);
        draw_key_inspector(app, clips);
    }

private:
    // The selected AnimationPlayer, or the first one among the selected node's children.
    NodeId find_player(EditorApp& app) const {
        const EditorDocument& doc = app.doc();
        const NodeId sel = doc.selection();
        const ModelNode* n = doc.model().get(sel);
        if (n == nullptr) return 0;
        if (is_player(app, n->type)) return sel;
        for (NodeId c : n->children) {
            if (const ModelNode* cn = doc.model().get(c); cn != nullptr && is_player(app, cn->type)) return c;
        }
        return 0;
    }

    static bool is_player(EditorApp& app, const std::string& type) {
        return type == "AnimationPlayer" || app.env().classes().is_a(type, "AnimationPlayer");
    }

    bool commit(EditorApp& app, const std::vector<anim::Clip>& clips, bool merge = false) {
        const bool ok = app.doc().set_field(player_, "animations", anim::write(clips), merge);
        app.report(ok);
        return ok;
    }

    void stop_preview(EditorApp& app) {
        app.anim_preview().on = false;
        app.refresh_preview();
    }

    void draw_clip_bar(EditorApp& app, std::vector<anim::Clip>& clips) {
        const char* current = clip_ >= 0 ? clips[static_cast<std::size_t>(clip_)].name.c_str() : "(none)";
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##clip", current)) {
            for (std::size_t i = 0; i < clips.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(clips[i].name.c_str(), static_cast<int>(i) == clip_)) {
                    clip_ = static_cast<int>(i);
                    track_ = key_ = -1;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("New")) ImGui::OpenPopup("##new_clip");
        if (ImGui::BeginPopup("##new_clip")) {
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("name", &new_name_, ImGuiInputTextFlags_EnterReturnsTrue);
            if ((ImGui::Button("Add") || enter) && !new_name_.empty()) {
                const bool taken = std::any_of(clips.begin(), clips.end(), [&](const anim::Clip& c) {
                    return c.name == new_name_;
                });
                if (taken) {
                    app.report_failure(std::format("an animation named '{}' already exists", new_name_));
                } else {
                    anim::Clip c;
                    c.name = new_name_;
                    clips.push_back(c);
                    if (commit(app, clips)) clip_ = static_cast<int>(clips.size()) - 1;
                    new_name_.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        if (clip_ < 0) return;
        anim::Clip& clip = clips[static_cast<std::size_t>(clip_)];
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            clips.erase(clips.begin() + clip_);
            commit(app, clips);
            clip_ = track_ = key_ = -1;
            return;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        int length = clip.length;
        if (ImGui::InputInt("ticks", &length) && length >= 1 && length != clip.length) {
            clip.length = length;
            commit(app, clips, true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (combo("loop", clip.mode, anim::loop_names())) commit(app, clips);
        ImGui::SameLine();
        EditorApp::AnimPreview& preview = app.anim_preview();
        bool on = preview.on;
        if (ImGui::Checkbox("Preview", &on)) {
            if (on) {
                preview = {true, player_, clip.name, playhead_};
            } else {
                stop_preview(app);
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##playhead", &playhead_, 0, std::max(clip.length, 1), "tick %d");
    }

    void draw_timeline(EditorApp& app, std::vector<anim::Clip>& clips) {
        anim::Clip& clip = clips[static_cast<std::size_t>(clip_)];
        const brand::ThemeColors& t = theme::colors();
        constexpr float label_w = 210.0f;
        const float row_h = ImGui::GetFrameHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float full_w = ImGui::GetContentRegionAvail().x;
        const float x0 = origin.x + label_w;
        const float w = std::max(full_w - label_w - 8.0f, 40.0f);
        const float length = static_cast<float>(std::max(clip.length, 1));
        const auto tick_x = [&](float tick) { return x0 + tick / length * w; };
        const auto x_tick = [&](float x) {
            return std::clamp(static_cast<int>(std::lround((x - x0) / w * length)), 0, clip.length);
        };

        // Ruler: a mark every `step` ticks, labelled every 2 steps; click/drag scrubs.
        ImGui::InvisibleButton("##ruler", ImVec2(full_w, row_h));
        if (ImGui::IsItemActive()) playhead_ = x_tick(ImGui::GetIO().MousePos.x);
        int step = 1;
        while (w / length * static_cast<float>(step) < 12.0f) step *= 2;
        for (int k = 0; k <= clip.length; k += step) {
            const float x = tick_x(static_cast<float>(k));
            const bool major = (k / step) % 4 == 0;
            dl->AddLine(ImVec2(x, origin.y + row_h * (major ? 0.35f : 0.65f)), ImVec2(x, origin.y + row_h),
                        col(t.text_muted, major ? 0.8f : 0.4f));
            if (major) dl->AddText(ImVec2(x + 2.0f, origin.y), col(t.text_muted), std::to_string(k).c_str());
        }

        // Tracks.
        int clicked_track = -1;
        for (std::size_t ti = 0; ti < clip.tracks.size(); ++ti) {
            anim::Track& track = clip.tracks[ti];
            ImGui::PushID(static_cast<int>(ti));
            const ImVec2 row = ImGui::GetCursorScreenPos();
            const bool selected = static_cast<int>(ti) == track_;
            if (selected) {
                dl->AddRectFilled(row, ImVec2(row.x + full_w, row.y + row_h), col(t.accent, 0.15f));
            }
            const std::string label = track.kind == "Property" ? track.path : std::format("{} ({})", track.path, track.kind);
            ImGui::SetCursorScreenPos(row);
            if (ImGui::Selectable(label.empty() ? "(event)" : label.c_str(), selected, 0, ImVec2(label_w - 8.0f, row_h))) {
                track_ = static_cast<int>(ti);
                key_ = -1;
            }
            ImGui::SetCursorScreenPos(ImVec2(x0, row.y));
            ImGui::InvisibleButton("##lane", ImVec2(w, row_h));
            const bool lane_hovered = ImGui::IsItemHovered();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            dl->AddLine(ImVec2(x0, row.y + row_h * 0.5f), ImVec2(x0 + w, row.y + row_h * 0.5f),
                        col(t.text_muted, 0.25f));
            int hovered_key = -1;
            for (std::size_t ki = 0; ki < track.keys.size(); ++ki) {
                const float x = tick_x(static_cast<float>(track.keys[ki].time));
                const float cy = row.y + row_h * 0.5f;
                const float r = 5.0f;
                if (lane_hovered && std::abs(mouse.x - x) <= r + 2.0f) hovered_key = static_cast<int>(ki);
                const bool key_selected = selected && static_cast<int>(ki) == key_;
                const ImU32 fill = key_selected ? col(t.accent) : col(t.text, 0.85f);
                const ImVec2 pts[4] = {{x, cy - r}, {x + r, cy}, {x, cy + r}, {x - r, cy}};
                dl->AddConvexPolyFilled(pts, 4, fill);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                clicked_track = static_cast<int>(ti);
                track_ = static_cast<int>(ti);
                key_ = hovered_key;
                dragging_ = hovered_key >= 0;
                if (hovered_key < 0) playhead_ = x_tick(mouse.x);
            }
            if (hovered_key >= 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                track.keys.erase(track.keys.begin() + hovered_key);
                commit(app, clips);
                track_ = static_cast<int>(ti);
                key_ = -1;
            }
            ImGui::PopID();
        }
        // Dragging the selected key moves it in time (one merged undo step).
        if (dragging_ && ImGui::IsMouseDown(ImGuiMouseButton_Left) && clicked_track < 0 && track_ >= 0 && key_ >= 0) {
            anim::Track& track = clip.tracks[static_cast<std::size_t>(track_)];
            const int tick = x_tick(ImGui::GetIO().MousePos.x);
            if (tick != track.keys[static_cast<std::size_t>(key_)].time) {
                const bool occupied = std::any_of(track.keys.begin(), track.keys.end(),
                                                  [&](const anim::Key& k) { return k.time == tick; });
                if (!occupied) {
                    anim::Key moved = track.keys[static_cast<std::size_t>(key_)];
                    moved.time = tick;
                    track.keys.erase(track.keys.begin() + key_);
                    key_ = static_cast<int>(anim::put_key(track, moved));
                    playhead_ = tick;
                    commit(app, clips, true);
                }
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging_ = false;

        // The playhead over everything.
        const float px = tick_x(static_cast<float>(playhead_));
        const float bottom = ImGui::GetCursorScreenPos().y;
        dl->AddLine(ImVec2(px, origin.y), ImVec2(px, std::max(bottom, origin.y + row_h)), col(t.danger), 2.0f);
        if (clip.tracks.empty()) ImGui::TextDisabled("No tracks: add one below (a path like \"Visual/Arm:rotation\").");
    }

    void draw_track_tools(EditorApp& app, std::vector<anim::Clip>& clips) {
        anim::Clip& clip = clips[static_cast<std::size_t>(clip_)];
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##track_path", "Node/Path:field", &new_track_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        combo("##track_kind", new_kind_, anim::track_kind_names());
        ImGui::SameLine();
        if (ImGui::Button("Add Track")) {
            std::string node_path, field;
            if (new_kind_ == "Property" && !anim::split_path(new_track_, node_path, field)) {
                app.report_failure("a property track's path is \"Node/Path:field\" (\":field\" for the root)");
            } else if (anim::find_track(clip, new_track_, new_kind_) >= 0) {
                app.report_failure(std::format("the clip already has a {} track '{}'", new_kind_, new_track_));
            } else {
                clip.tracks.push_back({new_track_, new_kind_, {}});
                if (commit(app, clips)) track_ = static_cast<int>(clip.tracks.size()) - 1;
                key_ = -1;
            }
        }
        if (track_ < 0) return;
        anim::Track& track = clip.tracks[static_cast<std::size_t>(track_)];
        ImGui::SameLine();
        if (ImGui::Button("Delete Track")) {
            clip.tracks.erase(clip.tracks.begin() + track_);
            commit(app, clips);
            track_ = key_ = -1;
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Key")) {
            anim::Key key;
            key.time = playhead_;
            if (track.kind == "Property") {
                if (auto v = record(app, track.path)) {
                    key.value = *v;
                } else {
                    app.report_failure(std::format("'{}' isn't a field a track can record; the key is zero", track.path));
                }
                // Hold the interpolation the previous key used.
                for (const anim::Key& k : track.keys) {
                    if (k.time < key.time) key.interp = k.interp;
                }
            }
            key_ = static_cast<int>(anim::put_key(track, key));
            commit(app, clips);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("right-click a key to delete it, drag to move it");
    }

    void draw_key_inspector(EditorApp& app, std::vector<anim::Clip>& clips) {
        if (track_ < 0 || key_ < 0) {
            ImGui::TextDisabled("Select a key to edit it.");
            return;
        }
        anim::Clip& clip = clips[static_cast<std::size_t>(clip_)];
        anim::Track& track = clip.tracks[static_cast<std::size_t>(track_)];
        anim::Key key = track.keys[static_cast<std::size_t>(key_)];
        bool changed = false;
        bool merge = false;
        ImGui::SetNextItemWidth(80.0f);
        int time = key.time;
        if (ImGui::InputInt("tick", &time, 0) && time >= 0 && time != key.time) {
            key.time = time;
            changed = true;
        }
        if (track.kind == "Property") {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::DragFloat4("value", key.value.data(), 0.01f)) {
                changed = true;
                merge = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            changed |= combo("interp", key.interp, anim::interp_names());
            if (key.interp == "EaseIn" || key.interp == "EaseOut" || key.interp == "EaseInOut") {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                changed |= combo("ease", key.ease_kind, anim::ease_names());
            } else if (key.interp == "Curve") {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::DragFloat("curve", &key.curve, 0.01f)) {
                    changed = true;
                    merge = true;
                }
            }
        } else {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::InputText(track.kind == "Event" ? "event" : "text", &key.text)) {
                changed = true;
                merge = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Key")) {
            track.keys.erase(track.keys.begin() + key_);
            commit(app, clips);
            key_ = -1;
            return;
        }
        if (changed) {
            track.keys.erase(track.keys.begin() + key_);
            key_ = static_cast<int>(anim::put_key(track, key));
            commit(app, clips, merge);
        }
    }

    // The value `path` ("Node/Path:field" from the player's root) has in the
    // document, packed for a key.
    std::optional<std::array<float, 4>> record(EditorApp& app, const std::string& path) const {
        std::string node_path, field;
        if (!anim::split_path(path, node_path, field)) return std::nullopt;
        const EditorDocument& doc = app.doc();
        const SceneModel& model = doc.model();
        const ModelNode* p = model.get(player_);
        if (p == nullptr) return std::nullopt;
        // root_path: ".." (the default) is the parent, "." the player itself.
        const scene::Value root_path = doc.field_value(player_, "root_path");
        NodeId at = root_path.kind == scene::ValueKind::Str && root_path.text == "." ? player_ : p->parent;
        std::size_t pos = 0;
        while (at != 0 && pos < node_path.size()) {
            std::size_t end = node_path.find('/', pos);
            if (end == std::string::npos) end = node_path.size();
            const std::string seg = node_path.substr(pos, end - pos);
            pos = end + 1;
            if (seg.empty() || seg == ".") continue;
            const ModelNode* n = model.get(at);
            if (seg == "..") {
                at = n->parent;
                continue;
            }
            NodeId next = 0;
            for (NodeId c : n->children) {
                if (model.get(c)->name == seg) {
                    next = c;
                    break;
                }
            }
            at = next;
        }
        if (at == 0) return std::nullopt;
        if (doc.field_type(at, field).empty()) return std::nullopt;
        return anim::pack(doc.field_value(at, field));
    }

    NodeId player_ = 0;
    int clip_ = -1;
    int track_ = -1;
    int key_ = -1;
    int playhead_ = 0;
    bool dragging_ = false;
    std::string new_name_;
    std::string new_track_;
    std::string new_kind_ = "Property";
};

} // namespace

std::unique_ptr<Panel> make_animation_panel() { return std::make_unique<AnimationPanel>(); }

} // namespace cuervo::editor

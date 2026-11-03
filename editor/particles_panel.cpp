// The Particles panel (bottom dock; DESIGN.md 4.13, milestone 11.7c): live
// tuning of the selected Emitter2D. Sliders for the fields that shape the
// effect, grouped (Emission, Motion, Life, Look), a few presets, and a preview
// of the emitter running next to them (EmitterPreview, particle_preview.hpp).
// Every change is EditorDocument::set_field: one undo step per drag, saved in
// the scene with the node, so what is tuned here is what the game fires.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "editor/app.hpp"
#include "editor/panels.hpp"
#include "editor/particle_preview.hpp"
#include "editor/imgui_theme.hpp"

namespace cuervo::editor {

namespace {

class ParticlesPanel final : public Panel {
public:
    const char* title() const override { return "Particles"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        EditorDocument& doc = app.doc();
        const NodeId id = doc.selection();
        const EmitterParams params = read_emitter_params(doc, id);
        if (!params.valid) {
            ImGui::TextDisabled("Select an Emitter2D to tune it here.");
            if (ImGui::Button("Add an Emitter2D")) {
                const NodeId parent = id != 0 ? id : doc.model().root();
                const NodeId added = doc.add_child(parent, "Emitter2D", "emitter");
                if (added != 0) doc.select(added);
                app.report(added != 0);
            }
            return;
        }
        if (id != last_id_) {
            last_id_ = id;
            preview_.restart();
        }

        const float left_w = std::max(ImGui::GetContentRegionAvail().x * 0.6f, 280.0f);
        ImGui::BeginChild("##sliders", {left_w, 0.0f}, ImGuiChildFlags_Borders);
        presets(app, id);
        sliders(app, id, params);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##preview", {0.0f, 0.0f}, ImGuiChildFlags_Borders);
        canvas(params);
        ImGui::EndChild();
    }

private:
    using Edit = std::pair<const char*, scene::Value>;

    static scene::Value num(double v) { return scene::Value::number(v); }

    void apply(EditorApp& app, NodeId id, const std::vector<Edit>& edits) {
        for (const Edit& e : edits) app.report(app.doc().set_field(id, e.first, e.second));
        app.doc().seal();
        preview_.restart();
    }

    void presets(EditorApp& app, NodeId id) {
        ImGui::TextDisabled("Presets");
        ImGui::SameLine();
        if (ImGui::SmallButton("Sparks")) {
            apply(app, id, {{"pattern", scene::Value::enum_value("EmitPattern", "Burst")}, {"count", scene::Value::integer(30)},
                            {"spread", num(3.1416)}, {"speed", num(220.0)}, {"speed_jitter", num(80.0)}, {"life", num(0.6)},
                            {"life_jitter", num(0.2)}, {"size", num(2.5)}, {"gravity", scene::Value::vector({0.0f, 260.0f})},
                            {"fade", scene::Value::boolean(true)}, {"end_scale", num(0.3)}, {"interval", num(0.6)}});
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Smoke")) {
            apply(app, id, {{"pattern", scene::Value::enum_value("EmitPattern", "Arc")}, {"count", scene::Value::integer(6)},
                            {"direction", num(-1.5708)}, {"spread", num(0.6)}, {"speed", num(40.0)}, {"life", num(2.0)},
                            {"size", num(6.0)}, {"gravity", scene::Value::vector({0.0f, -20.0f})}, {"drag", num(0.8)},
                            {"fade", scene::Value::boolean(true)}, {"end_scale", num(3.0)}, {"interval", num(0.2)}});
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Ring burst")) {
            apply(app, id, {{"pattern", scene::Value::enum_value("EmitPattern", "Ring")}, {"count", scene::Value::integer(24)},
                            {"speed", num(160.0)}, {"speed_jitter", num(0.0)}, {"life", num(1.0)}, {"size", num(3.0)},
                            {"gravity", scene::Value::vector({0.0f, 0.0f})}, {"drag", num(0.0)},
                            {"fade", scene::Value::boolean(true)}, {"end_scale", num(1.0)}, {"interval", num(0.8)}});
        }
    }

    // A float field as a drag; one undo step per drag.
    void drag(EditorApp& app, NodeId id, const char* field, const char* label, float v, float speed, float lo, float hi) {
        ImGui::SetNextItemWidth(160.0f * theme::ui_scale());
        if (ImGui::DragFloat(label, &v, speed, lo, hi, "%.3f")) {
            app.report(app.doc().set_field(id, field, scene::Value::number(v), true));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) app.doc().seal();
    }

    void toggle(EditorApp& app, NodeId id, const char* field, const char* label, bool v) {
        if (ImGui::Checkbox(label, &v)) {
            app.report(app.doc().set_field(id, field, scene::Value::boolean(v)));
            app.doc().seal();
        }
    }

    void color(EditorApp& app, NodeId id, const char* field, const char* label, std::array<float, 4> c) {
        ImGui::SetNextItemWidth(160.0f * theme::ui_scale());
        if (ImGui::ColorEdit4(label, c.data(), ImGuiColorEditFlags_AlphaBar)) {
            scene::Value v = app.doc().field_value(id, field);
            const char* names[] = {"r", "g", "b", "a"};
            for (scene::Field& f : v.fields) {
                for (int i = 0; i < 4; ++i) {
                    if (f.name == names[i]) f.value = scene::Value::number(c[static_cast<std::size_t>(i)]);
                }
            }
            app.report(app.doc().set_field(id, field, std::move(v), true));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) app.doc().seal();
    }

    void sliders(EditorApp& app, NodeId id, const EmitterParams& p) {
        EditorDocument& doc = app.doc();
        if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* patterns[] = {"Ring", "Arc", "Burst", "Point"};
            ImGui::SetNextItemWidth(160.0f * theme::ui_scale());
            if (ImGui::BeginCombo("Pattern", p.pattern.c_str())) {
                for (const char* n : patterns) {
                    if (ImGui::Selectable(n, p.pattern == n)) {
                        app.report(doc.set_field(id, "pattern", scene::Value::enum_value("EmitPattern", n)));
                        doc.seal();
                        preview_.restart();
                    }
                }
                ImGui::EndCombo();
            }
            int count = p.count;
            ImGui::SetNextItemWidth(160.0f * theme::ui_scale());
            if (ImGui::DragInt("Count", &count, 0.25f, 0, 2000)) {
                app.report(doc.set_field(id, "count", scene::Value::integer(count), true));
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) doc.seal();
            drag(app, id, "interval", "Interval (s)", p.interval, 0.01f, 0.02f, 30.0f);
            toggle(app, id, "emitting", "Emitting", doc.field_value(id, "emitting").b);
            ImGui::SameLine();
            toggle(app, id, "one_shot", "One shot", p.one_shot);
            drag(app, id, "direction", "Direction (rad)", p.direction, 0.02f, -6.2832f, 6.2832f);
            drag(app, id, "spread", "Spread (rad)", p.spread, 0.02f, 0.0f, 6.2832f);
        }
        if (ImGui::CollapsingHeader("Motion", ImGuiTreeNodeFlags_DefaultOpen)) {
            drag(app, id, "speed", "Speed", p.speed, 1.0f, 0.0f, 2000.0f);
            drag(app, id, "speed_jitter", "Speed jitter", p.speed_jitter, 1.0f, 0.0f, 1000.0f);
            drag(app, id, "drag", "Drag", p.drag, 0.01f, 0.0f, 20.0f);
            drag(app, id, "spin", "Spin (rad/s)", p.spin, 0.05f, -50.0f, 50.0f);
            float g[2] = {p.gravity.x(), p.gravity.y()};
            ImGui::SetNextItemWidth(160.0f * theme::ui_scale());
            if (ImGui::DragFloat2("Gravity", g, 1.0f, -2000.0f, 2000.0f, "%.0f")) {
                app.report(doc.set_field(id, "gravity", scene::Value::vector({g[0], g[1]}), true));
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) doc.seal();
        }
        if (ImGui::CollapsingHeader("Life", ImGuiTreeNodeFlags_DefaultOpen)) {
            drag(app, id, "life", "Life (s)", p.life, 0.02f, 0.05f, 60.0f);
            drag(app, id, "life_jitter", "Life jitter", p.life_jitter, 0.02f, 0.0f, 30.0f);
        }
        if (ImGui::CollapsingHeader("Look", ImGuiTreeNodeFlags_DefaultOpen)) {
            drag(app, id, "size", "Size", p.size, 0.1f, 0.1f, 200.0f);
            drag(app, id, "end_scale", "Size at end (x)", p.end_scale, 0.02f, 0.0f, 20.0f);
            color(app, id, "color", "Color", p.color);
            toggle(app, id, "fade", "Fade to end color", p.fade);
            if (p.fade) color(app, id, "end_color", "End color", p.end_color);
        }
    }

    void canvas(const EmitterParams& p) {
        if (ImGui::Button(playing_ ? "Pause" : "Play")) playing_ = !playing_;
        ImGui::SameLine();
        if (ImGui::Button("Restart")) preview_.restart();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu particles, preview", preview_.particles().size());

        const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
        if (playing_) preview_.step(p, dt);
        // A one-shot is over once its particles are gone: fire it again after a beat.
        if (p.one_shot && preview_.shots() > 0 && preview_.particles().empty()) {
            idle_ += dt;
            if (idle_ > 0.8f) {
                idle_ = 0.0f;
                preview_.restart();
            }
        }

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size{std::max(avail.x, 40.0f), std::max(avail.y, 40.0f)};
        ImGui::InvisibleButton("##canvas", size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(16, 18, 26, 255));
        dl->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
        const ImVec2 mid{origin.x + size.x * 0.5f, origin.y + size.y * 0.5f};
        dl->AddLine({mid.x - 6, mid.y}, {mid.x + 6, mid.y}, IM_COL32(120, 120, 140, 160));
        dl->AddLine({mid.x, mid.y - 6}, {mid.x, mid.y + 6}, IM_COL32(120, 120, 140, 160));
        const float k = theme::ui_scale();
        for (const PreviewParticle& q : preview_.particles()) {
            const std::array<float, 4> c = EmitterPreview::color_of(p, q);
            const float r = std::max(EmitterPreview::radius_of(p, q) * k, 0.5f);
            const ImVec2 at{mid.x + q.position.x() * k, mid.y + q.position.y() * k};
            dl->AddCircleFilled(at, r, ImGui::GetColorU32(ImVec4(c[0], c[1], c[2], c[3])), 16);
        }
        dl->PopClipRect();
    }

    EmitterPreview preview_;
    NodeId last_id_ = 0;
    bool playing_ = true;
    float idle_ = 0.0f;
};

} // namespace

std::unique_ptr<Panel> make_particles_panel() { return std::make_unique<ParticlesPanel>(); }

} // namespace cuervo::editor

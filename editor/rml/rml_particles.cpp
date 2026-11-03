// The drawer's Particles tab: presets and field rows (inspector rows) next to a live preview of the
// selected Emitter2D (EmitterPreview, the same CPU simulation the ImGui panel draws). The preview is a
// custom element that submits one small polygon per particle through RmlUi's geometry path.
#include <RmlUi/Core.h>
#include <RmlUi/Core/Mesh.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "editor/particle_preview.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"

namespace cuervo::editor {

namespace {

using scene::Value;

Value num(double v) { return Value::number(v); }

struct Preset {
    const char* id;
    const char* label;
    std::vector<std::pair<const char*, Value>> edits;
};

std::vector<Preset> presets() {
    return {
        {"sparks", "Sparks", {{"pattern", Value::enum_value("EmitPattern", "Burst")}, {"count", Value::integer(30)}, {"spread", num(3.1416)}, {"speed", num(220.0)}, {"speed_jitter", num(80.0)}, {"life", num(0.6)}, {"life_jitter", num(0.2)}, {"size", num(2.5)}, {"gravity", Value::vector({0.0f, 260.0f})}, {"fade", Value::boolean(true)}, {"end_scale", num(0.3)}, {"interval", num(0.6)}}},
        {"smoke", "Smoke", {{"pattern", Value::enum_value("EmitPattern", "Arc")}, {"count", Value::integer(6)}, {"direction", num(-1.5708)}, {"spread", num(0.6)}, {"speed", num(40.0)}, {"life", num(2.0)}, {"size", num(6.0)}, {"gravity", Value::vector({0.0f, -20.0f})}, {"drag", num(0.8)}, {"fade", Value::boolean(true)}, {"end_scale", num(3.0)}, {"interval", num(0.2)}}},
        {"ring", "Ring burst", {{"pattern", Value::enum_value("EmitPattern", "Ring")}, {"count", Value::integer(24)}, {"speed", num(160.0)}, {"speed_jitter", num(0.0)}, {"life", num(1.0)}, {"size", num(3.0)}, {"gravity", Value::vector({0.0f, 0.0f})}, {"drag", num(0.0)}, {"fade", Value::boolean(true)}, {"end_scale", num(1.0)}, {"interval", num(0.8)}}},
    };
}

// <particles> element: draws what the shell hands it each frame (the shell owns the simulation).
class ParticleView final : public Rml::Element {
public:
    explicit ParticleView(const Rml::String& tag_name) : Rml::Element(tag_name) {}
    void bind(const EmitterPreview* preview, const EmitterParams* params) { preview_ = preview; params_ = params; }

protected:
    void OnRender() override {
        if (preview_ == nullptr || params_ == nullptr) return;
        Rml::RenderManager* rm = GetRenderManager();
        if (rm == nullptr) return;
        const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Border);
        const Rml::Vector2f mid = GetAbsoluteOffset(Rml::BoxArea::Border) + size * 0.5f;
        const float k = GetContext() != nullptr ? GetContext()->GetDensityIndependentPixelRatio() : 1.0f;
        Rml::Mesh mesh;
        constexpr int seg = 10;
        const auto add = [&](Rml::Vector2f c, float r, const std::array<float, 4>& col) {
            const float a = std::clamp(col[3], 0.0f, 1.0f);
            const Rml::ColourbPremultiplied p(static_cast<Rml::byte>(std::clamp(col[0], 0.0f, 1.0f) * a * 255.0f), static_cast<Rml::byte>(std::clamp(col[1], 0.0f, 1.0f) * a * 255.0f),
                                              static_cast<Rml::byte>(std::clamp(col[2], 0.0f, 1.0f) * a * 255.0f), static_cast<Rml::byte>(a * 255.0f));
            const int base = static_cast<int>(mesh.vertices.size());
            mesh.vertices.push_back({c, p, {0, 0}});
            for (int i = 0; i < seg; ++i) {
                const float t = 6.2831853f * static_cast<float>(i) / seg;
                mesh.vertices.push_back({{c.x + std::cos(t) * r, c.y + std::sin(t) * r}, p, {0, 0}});
            }
            for (int i = 0; i < seg; ++i) {
                mesh.indices.push_back(base);
                mesh.indices.push_back(base + 1 + i);
                mesh.indices.push_back(base + 1 + (i + 1) % seg);
            }
        };
        // Origin cross, then the particles; positions in the same units as the ImGui panel (scaled by dp).
        for (const PreviewParticle& q : preview_->particles()) {
            const Rml::Vector2f at{mid.x + q.position.x() * k, mid.y + q.position.y() * k};
            if (at.x < mid.x - size.x * 0.5f - 8 || at.x > mid.x + size.x * 0.5f + 8 || at.y < mid.y - size.y * 0.5f - 8 || at.y > mid.y + size.y * 0.5f + 8) continue;
            add(at, std::max(EmitterPreview::radius_of(*params_, q) * k, 0.6f), EmitterPreview::color_of(*params_, q));
        }
        const std::array<float, 4> cross{0.47f, 0.47f, 0.55f, 0.7f};
        add(mid, 1.5f * k, cross);
        Rml::Geometry g = rm->MakeGeometry(std::move(mesh));
        g.Render({0.0f, 0.0f});
    }

private:
    const EmitterPreview* preview_ = nullptr;
    const EmitterParams* params_ = nullptr;
};

} // namespace

void register_particle_view() {
    static Rml::ElementInstancerGeneric<ParticleView> instancer;
    Rml::Factory::RegisterElementInstancer("particles", &instancer);
}

bool Shell::animating() const {
    return script_wants_frames() || (layout_.drawer_open && layout_.drawer == "node" && ((node_tab_ == "Particles" && particles_play_) || (node_tab_ == "Animation" && timeline_play_)));
}

void Shell::build_particle_drawer() {
    Rml::Element* e = by_id("drawer-body");
    set_drawer_head(std::format("<span class=\"dh-title\">Particles</span><span class=\"dh-note\">{}</span>", particles_count_text_));
    std::string out = "<div class=\"dsplit\"><div class=\"dright scroll\"><div class=\"section\"><span>Presets</span></div><div class=\"opts\">";
    for (const Preset& p : presets()) out += std::format("<div class=\"btn sm\" data-preset=\"{}\"><span>{}</span></div>", p.id, p.label);
    out += "</div>";
    out += field_group("Emission", {"emitting", "one_shot", "pattern", "count", "interval", "direction", "spread"});
    out += field_group("Motion", {"speed", "speed_jitter", "drag", "spin", "gravity"});
    out += field_group("Life", {"life", "life_jitter"});
    out += field_group("Look", {"size", "end_scale", "color", "fade", "end_color", "shape"});
    out += std::format("</div><div class=\"dleft pright\"><div class=\"pvbar\"><div class=\"btn sm\" data-pv=\"play\" title=\"Pause or resume the preview\">{}<span>{}</span></div><div class=\"btn sm\" data-pv=\"restart\" title=\"Start the preview over\">{}<span>Restart</span></div><span id=\"pv-count\" class=\"mono\"></span></div><particles id=\"pv-canvas\" class=\"pvview\"></particles></div></div>",
                       w::icon(particles_play_ ? "pause" : "play", 12), particles_play_ ? "Pause" : "Play", w::icon("reset", 12));
    e->SetInnerRML(out);
    bind_particles();
}

void Shell::bind_particles() {
    if (auto* v = dynamic_cast<ParticleView*>(by_id("pv-canvas"))) v->bind(&emitter_, &emitter_params_);
}

// Once per frame while the tab shows: read the node's parameters, advance the simulation.
void Shell::step_particles() {
    const std::uint32_t now = SDL_GetTicks();
    const float dt = std::min(static_cast<float>(now - particles_ms_) / 1000.0f, 0.05f);
    particles_ms_ = now;
    emitter_params_ = read_emitter_params(ctl_.doc(), selected_);
    if (selected_ != emitter_node_) { emitter_node_ = selected_; emitter_.restart(); }
    if (particles_play_) emitter_.step(emitter_params_, dt);
    if (emitter_params_.one_shot && emitter_.shots() > 0 && emitter_.particles().empty()) {
        particles_idle_ += dt;
        if (particles_idle_ > 0.8f) { particles_idle_ = 0.0f; emitter_.restart(); }
    }
    if (Rml::Element* c = by_id("pv-count")) {
        const std::string t = std::format("{} particles", emitter_.particles().size());
        if (c->GetInnerRML() != t) c->SetInnerRML(t);
    }
    bind_particles();
}

bool Shell::particles_click(Rml::Element* t) {
    for (Rml::Element* e = t; e != nullptr; e = e->GetParentNode()) {
        if (!e->HasAttribute("data-pv")) continue;
        const std::string what = e->GetAttribute<Rml::String>("data-pv", "");
        if (what == "play") { particles_play_ = !particles_play_; build_node_drawer(); }
        else if (what == "restart") emitter_.restart();
        return true;
    }
    for (Rml::Element* e = t; e != nullptr; e = e->GetParentNode()) {
        if (!e->HasAttribute("data-preset")) continue;
        const std::string id = e->GetAttribute<Rml::String>("data-preset", "");
        for (const Preset& p : presets()) {
            if (id != p.id) continue;
            for (const auto& ed : p.edits) ctl_.report(ctl_.doc().set_field(selected_, ed.first, ed.second, true)); // merged: one undo step
            ctl_.doc().seal();
            emitter_.restart();
        }
        return true;
    }
    return false;
}

} // namespace cuervo::editor

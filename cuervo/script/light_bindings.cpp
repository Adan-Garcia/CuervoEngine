// ScriptHost natives for 2D lighting (DESIGN.md 4.17, milestone 9.7; prelude
// lighting.plm): the Canvas calls PointLight2D, DirectionalLight2D,
// AmbientLight2D and LightOccluder2D make while the tree draws, and
// Canvas.apply_lights, which shades what was drawn so far.
//
// Lighting is render only: nothing here reads or writes simulation state, so
// lights never change a state hash or a replay.

#include <algorithm>

#include "cuervo/script/script_host.hpp"

namespace cuervo::script {

namespace {

namespace nv = pluma::native;
using pluma::NativeCall;
using pluma::Value;

vec2 to_vec2(const float* d) { return {d[0], d[1]}; }

rhi::Color color_arg(const NativeCall& c, std::uint32_t i) {
    const Value& v = c.args[i];
    if (!v.is_obj()) return {1.0f, 1.0f, 1.0f, 1.0f};
    const auto& f = static_cast<const pluma::InstanceObj*>(v.o)->fields;
    return {f[0].f, f[1].f, f[2].f, f[3].f};
}

std::uint32_t mask_arg(const NativeCall& c, std::uint32_t i) { return static_cast<std::uint32_t>(nv::integer(c, i)); }

} // namespace

void ScriptHost::bind_lighting() {
    pluma::Bindings& b = bindings_;
    auto host = this;
    auto lights = [host]() -> render::LightBatcher* {
        return host->targets_ != nullptr ? host->targets_->lights : nullptr;
    };

    // Canvas.point_light(pos, radius, color, energy, falloff, rotation, spot_angle, spot_feather,
    //                    source_radius, shadows, shadow_softness, shadow_color, mask, subtract, height)
    b.add_native("Canvas.point_light", [lights](NativeCall& c) {
        render::LightBatcher* const l = lights();
        if (l == nullptr) return Value{};
        render::LightDef d;
        d.kind = render::LightKind::Point;
        d.position = to_vec2(nv::vec(c, 1));
        d.radius = std::max(nv::f32(c, 2), 0.0f);
        d.color = color_arg(c, 3);
        d.energy = std::max(nv::f32(c, 4), 0.0f);
        d.falloff = std::max(nv::f32(c, 5), 0.0f);
        d.rotation = nv::f32(c, 6);
        d.spot_angle_rad = nv::f32(c, 7);
        d.spot_feather_rad = nv::f32(c, 8);
        d.source_radius = std::max(nv::f32(c, 9), 0.0f);
        d.shadow_enabled = nv::boolean(c, 10);
        d.shadow_softness = std::max(nv::f32(c, 11), 0.0f);
        d.shadow_color = color_arg(c, 12);
        d.shadow_mask = mask_arg(c, 13);
        d.blend = nv::boolean(c, 14) ? render::LightBlend::Subtract : render::LightBlend::Add;
        d.height = std::max(nv::f32(c, 15), 0.0f);
        l->add_light(d);
        return Value{};
    });
    // Canvas.directional_light(rotation, color, energy, max_distance, shadows, shadow_softness,
    //                          shadow_color, mask)
    b.add_native("Canvas.directional_light", [lights](NativeCall& c) {
        render::LightBatcher* const l = lights();
        if (l == nullptr) return Value{};
        render::LightDef d;
        d.kind = render::LightKind::Directional;
        d.rotation = nv::f32(c, 1);
        d.color = color_arg(c, 2);
        d.energy = std::max(nv::f32(c, 3), 0.0f);
        d.radius = std::max(nv::f32(c, 4), 0.0f);
        d.source_radius = 0.0f;
        d.shadow_enabled = nv::boolean(c, 5);
        d.shadow_softness = std::max(nv::f32(c, 6), 0.0f);
        d.shadow_color = color_arg(c, 7);
        d.shadow_mask = mask_arg(c, 8);
        l->add_light(d);
        return Value{};
    });
    // Canvas.ambient_light(color, darkness, glow)
    b.add_native("Canvas.ambient_light", [lights](NativeCall& c) {
        render::LightBatcher* const l = lights();
        if (l == nullptr) return Value{};
        render::AmbientLightDef a;
        a.enabled = true;
        a.color = color_arg(c, 1);
        a.darkness = std::clamp(nv::f32(c, 2), 0.0f, 1.0f);
        a.light_glow = std::max(nv::f32(c, 3), 0.0f);
        l->set_ambient(a);
        return Value{};
    });
    b.add_native("Canvas.occluder_circle", [lights](NativeCall& c) {
        if (render::LightBatcher* const l = lights()) {
            l->add_occluder_circle(to_vec2(nv::vec(c, 1)), nv::f32(c, 2), mask_arg(c, 3), nv::f32(c, 4));
        }
        return Value{};
    });
    // Canvas.occluder_rect(center, size, rotation, corner_radius, mask, bevel)
    b.add_native("Canvas.occluder_rect", [lights](NativeCall& c) {
        if (render::LightBatcher* const l = lights()) {
            l->add_occluder_box(to_vec2(nv::vec(c, 1)), to_vec2(nv::vec(c, 2)) * 0.5f, nv::f32(c, 3), nv::f32(c, 4),
                                mask_arg(c, 5), nv::f32(c, 6));
        }
        return Value{};
    });
    // Canvas.occluder_capsule(center, half_length, radius, rotation, mask, bevel)
    b.add_native("Canvas.occluder_capsule", [lights](NativeCall& c) {
        if (render::LightBatcher* const l = lights()) {
            l->add_occluder_capsule(to_vec2(nv::vec(c, 1)), nv::f32(c, 2), nv::f32(c, 3), nv::f32(c, 4),
                                    mask_arg(c, 5), nv::f32(c, 6));
        }
        return Value{};
    });
    // Canvas.occluder_polygon(points, closed, thickness, pos, rotation, scale, mask, bevel): each edge a
    // capsule `thickness` wide, the points placed like Canvas.polygon's.
    b.add_native("Canvas.occluder_polygon", [lights](NativeCall& c) {
        render::LightBatcher* const l = lights();
        if (l == nullptr || !c.args[1].is_obj() || c.args[1].o->kind != pluma::ObjKind::List) return Value{};
        const Affine2 place = Affine2::translate(to_vec2(nv::vec(c, 4))) * Affine2::rotate(nv::f32(c, 5)) *
                              Affine2::scale(to_vec2(nv::vec(c, 6)));
        std::vector<vec2> points;
        for (const Value& p : static_cast<const pluma::ListObj*>(c.args[1].o)->items) {
            points.push_back(place.apply(to_vec2(pluma::vec_data(p))));
        }
        l->add_occluder_polygon(points, nv::boolean(c, 2), nv::f32(c, 3), mask_arg(c, 7), nv::f32(c, 8));
        return Value{};
    });
    b.add_native("Canvas.apply_lights", [host, lights](NativeCall&) {
        render::LightBatcher* const l = lights();
        if (l == nullptr || !host->targets_->apply_lights) return Value{};
        if (l->has_lights()) {
            // Lights cover everything drawn before them, in this view.
            host->flush_draw(DrawFlush::Layer);
            host->targets_->apply_lights(host->view_, host->view_zoom_);
        }
        l->clear();
        return Value{};
    });
}

} // namespace cuervo::script

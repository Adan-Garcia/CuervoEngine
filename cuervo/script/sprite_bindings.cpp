// ScriptHost natives for raster sprites (DESIGN.md 4.4 "Raster pipeline",
// milestone 11.2a; prelude nodes.plm): Canvas.sprite and Canvas.nine_patch,
// the calls Sprite2D, AnimatedSprite2D and NinePatchRect make while the tree
// draws. Images load through the SpriteLibrary and queue into the
// SpriteBatcher in call order (settle_for_sprites flushes shapes and vector
// meshes drawn before them).
//
// Render only: nothing here reads or writes simulation state, so sprites never
// change a state hash or a replay.

#include <algorithm>
#include <cmath>

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

render::SpriteFilter filter_arg(const NativeCall& c, std::uint32_t i) {
    const std::string_view n = nv::name(c, i);
    return n == "nearest" ? render::SpriteFilter::Nearest
           : n == "linear" ? render::SpriteFilter::Linear
                           : render::SpriteFilter::Default;
}

// The center of a rectangle `size` big whose top-left corner (or center, when
// `centered`) is at `pos`, the rectangle turned about `pos` by `rotation`.
vec2 rect_center(vec2 pos, vec2 size, float rotation, bool centered) {
    if (centered) return pos;
    const float c = std::cos(rotation), s = std::sin(rotation);
    const vec2 h = size * 0.5f;
    return vec2(pos.x() + h.x() * c - h.y() * s, pos.y() + h.x() * s + h.y() * c);
}

} // namespace

void ScriptHost::bind_sprites() {
    pluma::Bindings& b = bindings_;
    auto host = this;

    // Canvas.sprite(asset, pos, frame, rotation, scale, centered, flip_h, flip_v, tint, filter, size)
    b.add_native("Canvas.sprite", [host](NativeCall& c) {
        const DrawTargets* t = host->targets_;
        if (t == nullptr || t->sprites == nullptr || t->sprite_batcher == nullptr || !host->options_.vfs) {
            return Value{};
        }
        const vec2 pos = to_vec2(nv::vec(c, 2));
        const float rotation = nv::f32(c, 4);
        const vec2 scale = to_vec2(nv::vec(c, 5));
        const float s = std::max(std::abs(scale.x()), std::abs(scale.y()));
        const render::SpriteRef ref =
            t->sprites->resolve(*host->options_.vfs, nv::str(c, 1), nv::str(c, 3), filter_arg(c, 10),
                                t->pixels_per_unit * host->view_zoom_ * s);
        if (!ref.ok) return Value{};
        vec2 natural = to_vec2(nv::vec(c, 11));
        if (!(natural.x() > 0.0f) || !(natural.y() > 0.0f)) natural = ref.size;
        const vec2 dest(natural.x() * std::abs(scale.x()), natural.y() * std::abs(scale.y()));
        const bool flip_x = nv::boolean(c, 7) != (scale.x() < 0.0f);
        const bool flip_y = nv::boolean(c, 8) != (scale.y() < 0.0f);
        render::SpriteUv uv = ref.uv;
        if (flip_x) std::swap(uv.u0, uv.u1);
        if (flip_y) std::swap(uv.v0, uv.v1);
        host->settle_for_sprites();
        t->sprite_batcher->add(ref.texture, rect_center(pos, dest, rotation, nv::boolean(c, 6)), dest, rotation, uv,
                               color_arg(c, 9));
        host->sprites_pending_ = true;
        return Value{};
    });

    // Canvas.nine_patch(asset, pos, size, frame, margin_left, margin_top, margin_right, margin_bottom,
    //                   rotation, scale, centered, draw_center, tint, filter)
    b.add_native("Canvas.nine_patch", [host](NativeCall& c) {
        const DrawTargets* t = host->targets_;
        if (t == nullptr || t->sprites == nullptr || t->sprite_batcher == nullptr || !host->options_.vfs) {
            return Value{};
        }
        const vec2 pos = to_vec2(nv::vec(c, 2));
        const vec2 size_arg = to_vec2(nv::vec(c, 3));
        const float rotation = nv::f32(c, 9);
        const vec2 scale = to_vec2(nv::vec(c, 10));
        const float s = std::max(std::abs(scale.x()), std::abs(scale.y()));
        const render::SpriteRef ref =
            t->sprites->resolve(*host->options_.vfs, nv::str(c, 1), nv::str(c, 4), filter_arg(c, 14),
                                t->pixels_per_unit * host->view_zoom_ * s);
        if (!ref.ok || !(ref.size.x() > 0.0f) || !(ref.size.y() > 0.0f)) return Value{};
        const vec2 dest(std::abs(size_arg.x() * scale.x()), std::abs(size_arg.y() * scale.y()));
        // Margins are in the image's natural units (texels; SVG document units);
        // the frame in texels of the texture may be bigger (SVG raster scale).
        const float tx = ref.frame_w / ref.size.x();
        const float ty = ref.frame_h / ref.size.y();
        const render::NinePatchMargins m{std::max(nv::f32(c, 5), 0.0f) * tx, std::max(nv::f32(c, 6), 0.0f) * ty,
                                         std::max(nv::f32(c, 7), 0.0f) * tx, std::max(nv::f32(c, 8), 0.0f) * ty};
        host->settle_for_sprites();
        // Borders keep their natural size on screen; `scale` stretches the rectangle.
        const int pieces = render::add_nine_patch(
            *t->sprite_batcher, ref.texture, ref.uv, ref.frame_w, ref.frame_h, m,
            rect_center(pos, dest, rotation, nv::boolean(c, 11)), dest, rotation, color_arg(c, 13),
            nv::boolean(c, 12), 1.0f / tx);
        if (pieces > 0) host->sprites_pending_ = true;
        return Value{};
    });
}

} // namespace cuervo::script

#include "cuervo/script/script_host.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "cuervo/core/hash.hpp"
#include "cuervo/core/profiler.hpp"
#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/core/name.hpp"
#include "cuervo/math/dmath.hpp"
#if CUERVO_FEATURE_PARTICLES
#include "cuervo/particles/particle_render.hpp"
#endif
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/scene_bridge.hpp"
#include "cuervo/script/vector_cache.hpp"
#if CUERVO_FEATURE_AUDIO
#include "cuervo/script/audio_bindings.hpp"
#endif
#include "pluma/aot.hpp"
#include "pluma/checker.hpp"
#include "pluma/compiler.hpp"
#include "pluma/parser.hpp"
#include "pluma/reload.hpp"

namespace cuervo::script {

namespace {

namespace nv = pluma::native;
using pluma::NativeCall;
using pluma::Value;

// The prelude sources (cuervo/script/prelude/*.plm), embedded at build time;
// a long file is split over consecutive entries with the same path
// (cuervo_append_prelude in cuervo/CMakeLists.txt).
struct PreludeSource {
    const char* path;
    const char* text;
};
constexpr PreludeSource prelude_sources[] = {
#include "cuervo_script_preludes.inc"
};

// One part per prelude file, its split entries joined back.
std::vector<pluma::SourceFile::Part> joined_preludes() {
    std::vector<pluma::SourceFile::Part> parts;
    for (const PreludeSource& p : prelude_sources) {
        if (parts.empty() || parts.back().path != p.path) parts.push_back({p.path, {}});
        parts.back().text += p.text;
    }
    return parts;
}

vec2 to_vec2(const float* d) { return {d[0], d[1]}; }

// Reads a `Color` struct value (fields r, g, b, a).
rhi::Color to_color(const Value& v) {
    if (!v.is_obj()) return {1.0f, 1.0f, 1.0f, 1.0f};
    const auto& f = static_cast<const pluma::InstanceObj*>(v.o)->fields;
    return {f[0].f, f[1].f, f[2].f, f[3].f};
}

bool parse_hex(std::string_view s, rhi::Color& out) {
    if (!s.empty() && s[0] == '#') s.remove_prefix(1);
    if (s.size() != 6 && s.size() != 8) return false;
    float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (std::size_t i = 0; i < s.size() / 2; ++i) {
        int v = 0;
        for (std::size_t k = 0; k < 2; ++k) {
            char ch = s[i * 2 + k];
            int d = ch >= '0' && ch <= '9'   ? ch - '0'
                    : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                    : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                             : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        c[i] = static_cast<float>(v) / 255.0f;
    }
    out = {c[0], c[1], c[2], c[3]};
    return true;
}

#if CUERVO_FEATURE_PARTICLES
// Fields of a payload enum value or struct.
const std::vector<Value>& fields_of(const Value& v) {
    if (v.o->kind == pluma::ObjKind::Enum) return static_cast<const pluma::EnumObj*>(v.o)->fields;
    return static_cast<const pluma::InstanceObj*>(v.o)->fields;
}

std::uint32_t variant_of(const Value& v) { return static_cast<const pluma::EnumObj*>(v.o)->variant; }

std::uint32_t count_arg(const Value& v) { return static_cast<std::uint32_t>(std::max<std::int64_t>(0, v.i)); }

// Shot struct -> ParticleTemplate (field order of prelude/particles.plm).
particles::ParticleTemplate to_template(const Value& v) {
    particles::ParticleTemplate t;
    if (!v.is_obj()) return t;
    const auto& f = fields_of(v);
    t.speed = f[0].f;
    t.speed_jitter = f[1].f;
    t.life = f[2].f;
    t.life_jitter = f[3].f;
    t.size = f[4].f;
    t.color = to_color(f[5]);
    t.spin = f[6].f;
    t.offset = f[7].f;
    t.face_heading = f[8].b;
    return t;
}

particles::Pattern to_pattern(const Value& v) {
    const auto& f = fields_of(v);
    switch (variant_of(v)) {
        case 0: return particles::Ring{count_arg(f[0]), f[1].f};
        case 1: return particles::Arc{count_arg(f[0]), f[1].f, f[2].f};
        case 2: return particles::Aimed{count_arg(f[0]), f[1].f};
        default: return particles::Burst{count_arg(f[0]), f[1].f, f[2].f};
    }
}

particles::Module to_module(const Value& v) {
    const auto& f = fields_of(v);
    switch (variant_of(v)) {
        case 0: return particles::Accelerate{to_vec2(pluma::vec_data(f[0]))};
        case 1: return particles::Drag{f[0].f};
        case 2: return particles::SpeedRamp{f[0].f, f[1].f, f[2].f};
        case 3: return particles::Turn{f[0].f};
        case 4: return particles::Homing{f[0].f, f[1].f, f[2].f};
        case 5: return particles::Orbit{f[0].f, f[1].f};
        case 6: return particles::Wave{f[0].f, f[1].f, f[2].f};
        case 7: return particles::FaceVelocity{};
        case 8: return particles::ColorOverLife{to_color(f[0]), to_color(f[1])};
        default: return particles::SizeOverLife{f[0].f, f[1].f, particles::Ease::Linear};
    }
}

render::SdfShape shape_for(std::string_view s) {
    if (s == "capsule") return render::SdfShape::Capsule;
    if (s == "rect") return render::SdfShape::RoundedRect;
    if (s == "ring") return render::SdfShape::Ring;
    if (s == "sector") return render::SdfShape::Sector;
    return render::SdfShape::Circle;
}
#endif

} // namespace

#if CUERVO_FEATURE_PARTICLES
struct ScriptHost::EmitterData {
    ScriptHost* host = nullptr;
    particles::EmitterId id = 0;
};
#endif

ScriptHost::ScriptHost(HostOptions options, const InputMap& input_map)
    : options_(options), input_(input_map)
#if CUERVO_FEATURE_PARTICLES
      , particles_(options.seed)
#else
      , rng_(options.seed)
#endif
{
    if (!options_.vfs) options_.vfs = assets::Vfs::dev(options_.project_dir);
    if (options_.profile_scripts) set_script_profiling(true);
#if CUERVO_FEATURE_AUDIO
    audio_ = options_.audio;
    if (audio_ == nullptr) {
        audio::AudioOptions ao;
        ao.backend = audio::Backend::Null;
        ao.tick_rate = options_.tick_rate;
        own_audio_ = std::make_unique<audio::AudioSystem>(ao);
        audio_ = own_audio_.get();
    }
#endif
    for (pluma::SourceFile::Part& p : joined_preludes()) bindings_.add_prelude(std::move(p.path), std::move(p.text));
    bind();
#if CUERVO_FEATURE_AUDIO
    bind_audio(bindings_, *audio_);
#endif
}

void ScriptHost::set_script_profiling(bool on) {
    profiling_scripts_ = on;
    if (on && !script_profiler_) script_profiler_ = std::make_unique<pluma::ScriptProfiler>();
    if (session_ == nullptr || !script_profiler_) return;
    pluma::VM& vm = session_->vm();
    // Called between ticks: no script frame is open, so the hooks stay balanced.
    if (on && vm.debug_hooks() == nullptr) vm.set_debug_hooks(script_profiler_.get());
    if (!on && vm.debug_hooks() == script_profiler_.get()) vm.set_debug_hooks(nullptr);
}

ScriptHost::~ScriptHost() {
#if CUERVO_FEATURE_COLLISION
    for (Hitbox& h : hitboxes_) pluma::release(h.owner);
    hitboxes_.clear();
#endif
    pluma::release(root_);
    pluma::release(canvas_);
    root_ = canvas_ = Value{};
    session_.reset();
}

pluma::VmOptions ScriptHost::vm_options() {
    pluma::VmOptions o;
    o.tick_rate = options_.tick_rate;
    // Deterministic transcendental math for scripts (DESIGN.md 4.1).
    o.math.sin = &dmath::sin;
    o.math.cos = &dmath::cos;
    o.math.tan = &dmath::tan;
    o.math.asin = &dmath::asin;
    o.math.acos = &dmath::acos;
    o.math.atan2 = &dmath::atan2;
    o.math.exp = &dmath::exp;
    o.math.log = &dmath::log;
    o.math.pow = &dmath::pow;
    // `shuffle` draws from the simulation's seeded PCG32, the same stream as
    // `Random` (DESIGN.md 4.1, LANGUAGE.md 9.17).
    o.random_below = [this](std::uint32_t bound) { return rng().next_below(bound); };
    o.out = [](std::string_view s) {
        std::string line(s);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        LOG_INFO("[script] {}", line);
    };
    return o;
}

Value ScriptHost::name_value(std::string_view text) {
    Value v;
    v.tag = pluma::Tag::Name;
    v.i = session_->vm().intern(text);
    return v;
}

#if CUERVO_FEATURE_PARTICLES
void ScriptHost::free_emitter(particles::EmitterId id) {
    particles_.emitter(id).clear();
    particles_.emitter(id).render().visible = false;
    particles_.emitter(id).collision().enabled = false;
    emitters_[id].used = false;
    free_emitters_.push_back(id);
}
#endif

#if CUERVO_FEATURE_COLLISION
// Game shapes: `user` is the hitbox index (bit 63 clear), in registration order.
void ScriptHost::add_hitboxes(collision::CollisionWorld& world) const {
    for (std::size_t i = 0; i < hitboxes_.size(); ++i) {
        const Hitbox& h = hitboxes_[i];
        if (h.capsule) {
            world.add_capsule({h.start, h.end, h.radius}, h.layer, i);
        } else {
            world.add_circle({h.center, h.radius}, h.layer, i);
        }
    }
}

// Area overlaps (prelude collision.plm): every area, in registration order,
// is told the other areas on its mask layers that overlap it (in proxy id
// order, i.e. their registration order), then compares them with the last
// tick's and emits its signals. Results come from the built world's sorted
// queries, so the order is the same in every run (DESIGN.md 4.7).
bool ScriptHost::resolve_areas(const collision::CollisionWorld& world) {
    if (!world.built()) return true;
    pluma::VM& vm = session_->vm();
    std::vector<collision::ProxyId> found;
    for (std::size_t i = 0; i < hitboxes_.size(); ++i) {
        const Hitbox& h = hitboxes_[i];
        if (!h.area) continue;
        found.clear();
        if (h.capsule) {
            world.overlap(collision::Capsule{h.start, h.end, h.radius}, h.mask, found);
        } else {
            world.overlap(collision::Circle{h.center, h.radius}, h.mask, found);
        }
        for (collision::ProxyId p : found) {
            const std::uint64_t user = world.user(p);
#if CUERVO_FEATURE_PARTICLES
            if (particles::ParticleSystem::is_particle(user)) continue;
#endif
            if (user >= hitboxes_.size() || user == i || !hitboxes_[user].area) continue;
            if (hitboxes_[user].owner.o == h.owner.o) continue; // registered twice this tick
            const Value other = hitboxes_[user].owner;
            if (!check(vm.call_method(h.owner, "area_touch", std::span<const Value>(&other, 1)), "Area")) return false;
        }
    }
    for (const Hitbox& h : hitboxes_) {
        if (h.area && !check(vm.call_method(h.owner, "area_end_tick"), "an Area signal handler")) return false;
    }
    return true;
}
#endif

// ------------------------------------------------------------ natives

void ScriptHost::bind() {
    pluma::Bindings& b = bindings_;
    auto host = this;

    // --- Color
    b.add_native("Color.hex", [](NativeCall& c) {
        rhi::Color col;
        if (!parse_hex(nv::str(c, 0), col)) c.vm.panic(std::format("Color.hex: '{}' is not a hex color", nv::str(c, 0)));
        const pluma::RecordRT* rt = c.vm.find_struct("Color");
        auto* o = pluma::new_instance(rt, 4);
        o->fields[0] = Value::f32(col.r);
        o->fields[1] = Value::f32(col.g);
        o->fields[2] = Value::f32(col.b);
        o->fields[3] = Value::f32(col.a);
        return nv::owned(Value::object(o));
    });

    // --- Random: the simulation's RNG. Emitters spawn from their own child
    // streams (DESIGN.md 4.15), so scripts and particles never shift each other.
    b.add_native("Random.range", [host](NativeCall& c) {
        return Value::f32(host->rng().range(nv::f32(c, 0), nv::f32(c, 1)));
    });
    b.add_native("Random.range_int", [host](NativeCall& c) {
        auto lo = static_cast<std::int32_t>(nv::integer(c, 0));
        auto hi = static_cast<std::int32_t>(nv::integer(c, 1));
        if (hi < lo) c.vm.panic("Random.range_int: hi < lo");
        return Value::integer(pluma::NumKind::I32, host->rng().range(lo, hi));
    });
    b.add_native("Random.chance", [host](NativeCall& c) {
        return Value::boolean(host->rng().next_float() < nv::f32(c, 0));
    });

    // --- Input (DESIGN.md 4.8)
    b.add_native("Input.held", [host](NativeCall& c) { return Value::boolean(host->input_.held(Name(nv::name(c, 0)))); });
    b.add_native("Input.pressed",
                 [host](NativeCall& c) { return Value::boolean(host->input_.pressed(Name(nv::name(c, 0)))); });
    b.add_native("Input.released",
                 [host](NativeCall& c) { return Value::boolean(host->input_.released(Name(nv::name(c, 0)))); });
    b.add_native("Input.axis", [host](NativeCall& c) {
        std::string n(nv::name(c, 0));
        float x = host->input_.axis(Name(n + ".x"));
        float y = host->input_.axis(Name(n + ".y"));
        float len2 = x * x + y * y;
        if (len2 > 1.0f) {
            float len = dmath::sqrt(len2);
            x /= len;
            y /= len;
        }
        return nv::vec2_result(x, y);
    });
    b.add_native("Input.idle", [host](NativeCall& c) {
        return Value::boolean(static_cast<float>(host->idle_ticks_) >= nv::f32(c, 0) * host->options_.tick_rate);
    });

    // --- Canvas (render only)
    auto stroke_fill = [](NativeCall& c, std::uint32_t fill_i, rhi::Color& fill, render::SdfOutline& outline,
                          float& inset) {
        const Value& f = c.args[fill_i];
        const Value& s = c.args[fill_i + 1];
        float width = c.args[fill_i + 2].f;
        fill = f.is_obj() ? to_color(f) : rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
        inset = 0.0f;
        if (s.is_obj()) {
            // The stroke is centered on the outline; SDF outlines grow outwards.
            outline = {width, to_color(s)};
            inset = width * 0.5f;
        } else if (!f.is_obj()) {
            fill = {1.0f, 1.0f, 1.0f, 1.0f};
        }
    };
    b.add_native("Canvas.circle", [host, stroke_fill](NativeCall& c) {
        render::SdfBatcher* const shapes = host->shape_target();
        if (shapes == nullptr) return Value{};
        rhi::Color fill;
        render::SdfOutline outline;
        float inset = 0.0f;
        stroke_fill(c, 3, fill, outline, inset);
        float r = std::max(nv::f32(c, 2) - inset, 0.0f);
        shapes->circle(to_vec2(nv::vec(c, 1)), r, fill, outline);
        return Value{};
    });
    b.add_native("Canvas.rect", [host, stroke_fill](NativeCall& c) {
        render::SdfBatcher* const shapes = host->shape_target();
        if (shapes == nullptr) return Value{};
        rhi::Color fill;
        render::SdfOutline outline;
        float inset = 0.0f;
        stroke_fill(c, 3, fill, outline, inset);
        vec2 pos = to_vec2(nv::vec(c, 1));
        vec2 size = to_vec2(nv::vec(c, 2));
        vec2 half = size * 0.5f;
        vec2 h{std::max(half.x() - inset, 0.0f), std::max(half.y() - inset, 0.0f)};
        shapes->rounded_rect(pos + half, h, nv::f32(c, 6), nv::f32(c, 7), fill, outline);
        return Value{};
    });
    b.add_native("Canvas.line", [host](NativeCall& c) {
        render::SdfBatcher* const shapes = host->shape_target();
        if (shapes == nullptr) return Value{};
        vec2 a = to_vec2(nv::vec(c, 1));
        vec2 d = to_vec2(nv::vec(c, 2)) - a;
        float len = dmath::sqrt(d.x() * d.x() + d.y() * d.y());
        shapes->capsule(a + d * 0.5f, len * 0.5f, nv::f32(c, 4) * 0.5f, dmath::atan2(d.y(), d.x()),
                               to_color(c.args[3]));
        return Value{};
    });
    b.add_native("Canvas.arc", [host, stroke_fill](NativeCall& c) {
        render::SdfBatcher* const shapes = host->shape_target();
        if (shapes == nullptr) return Value{};
        vec2 center = to_vec2(nv::vec(c, 1));
        float radius = nv::f32(c, 2);
        float start_angle = nv::f32(c, 3);
        float end_angle = nv::f32(c, 4);
        rhi::Color fill;
        render::SdfOutline outline;
        float inset = 0.0f;
        stroke_fill(c, 5, fill, outline, inset);
        rhi::Color stroke = outline.width > 0.0f ? outline.color : rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
        float width = outline.width > 0.0f ? outline.width : 0.0f;
        shapes->arc(center, radius, start_angle, end_angle, fill, stroke, width);
        return Value{};
    });
    b.add_native("Canvas.text", [host](NativeCall& c) {
        if (host->text_ == nullptr || !host->font_) return Value{};
        render::TextStyle style;
        style.size = nv::f32(c, 3);
        std::string_view align = nv::name(c, 5);
        style.align = align == "center" ? render::TextAlign::Center
                      : align == "right" ? render::TextAlign::Right
                                         : render::TextAlign::Left;
        std::string_view base = nv::name(c, 6);
        style.baseline = base == "middle"   ? render::TextBaseline::Middle
                         : base == "bottom" ? render::TextBaseline::Bottom
                                            : render::TextBaseline::Top;
        style.max_width = std::max(nv::f32(c, 7), 0.0f);
        host->text_->draw(host->font_, nv::str(c, 2), to_vec2(nv::vec(c, 1)), style, to_color(c.args[4]));
        return Value{};
    });
    b.add_native("Canvas.capsule", [host, stroke_fill](NativeCall& c) {
        render::SdfBatcher* const shapes = host->shape_target();
        if (shapes == nullptr) return Value{};
        rhi::Color fill;
        render::SdfOutline outline;
        float inset = 0.0f;
        stroke_fill(c, 5, fill, outline, inset);
        shapes->capsule(to_vec2(nv::vec(c, 1)), std::max(nv::f32(c, 2), 0.0f), std::max(nv::f32(c, 3) - inset, 0.0f),
                        nv::f32(c, 4), fill, outline);
        return Value{};
    });
    b.add_native("Canvas.ring", [host, stroke_fill](NativeCall& c) {
        render::SdfBatcher* const shapes = host->shape_target();
        if (shapes == nullptr) return Value{};
        rhi::Color fill;
        render::SdfOutline outline;
        float inset = 0.0f;
        stroke_fill(c, 4, fill, outline, inset);
        // Both edges are outlined: the band shrinks by a stroke width so each
        // stroke stays centered on its edge.
        shapes->ring(to_vec2(nv::vec(c, 1)), nv::f32(c, 2), std::max(nv::f32(c, 3) - inset * 2.0f, 0.0f), fill,
                     outline);
        return Value{};
    });
    b.add_native("Canvas.vector", [host](NativeCall& c) {
        if (host->targets_ == nullptr || host->targets_->vectors == nullptr) return Value{};
        const vec2 pos = to_vec2(nv::vec(c, 2));
        const float rotation = nv::f32(c, 3);
        const vec2 scale = to_vec2(nv::vec(c, 4));
        const Affine2 m = Affine2::translate(pos) * Affine2::rotate(rotation) * Affine2::scale(scale);
        const float s = std::max(std::abs(scale.x()), std::abs(scale.y()));
        // Shapes drawn before it stay below it (call order).
        host->flush_draw(DrawFlush::Shapes);
        if (host->targets_->vectors->draw(*host->options_.vfs, nv::str(c, 1), m, nv::boolean(c, 5),
                                          to_color(c.args[6]), host->targets_->pixels_per_unit * host->view_zoom_ * s)) {
            host->meshes_pending_ = true;
        }
        return Value{};
    });
    // Polygon2D / Line2D: shape-space points placed by pos/rotation/scale.
    auto shape_draw = [host](NativeCall& c, ProceduralShape& shape, std::uint32_t pos_arg) {
        if (host->targets_ == nullptr || host->targets_->vectors == nullptr) return Value{};
        const vec2 pos = to_vec2(nv::vec(c, pos_arg));
        const float rotation = nv::f32(c, pos_arg + 1);
        const vec2 scale = to_vec2(nv::vec(c, pos_arg + 2));
        const Affine2 m = Affine2::translate(pos) * Affine2::rotate(rotation) * Affine2::scale(scale);
        const float s = std::max(std::abs(scale.x()), std::abs(scale.y()));
        host->flush_draw(DrawFlush::Shapes);
        if (host->targets_->vectors->draw_shape(shape, m, host->targets_->pixels_per_unit * host->view_zoom_ * s)) {
            host->meshes_pending_ = true;
        }
        return Value{};
    };
    auto read_points = [](const Value& list, ProceduralShape& shape) {
        if (!list.is_obj() || list.o->kind != pluma::ObjKind::List) return;
        for (const Value& p : static_cast<const pluma::ListObj*>(list.o)->items) {
            shape.points.push_back(to_vec2(pluma::vec_data(p)));
        }
    };
    auto vcolor = [](const Value& v) {
        const rhi::Color col = to_color(v);
        return vector::Color{col.r, col.g, col.b, col.a};
    };
    // Canvas.polygon(points, fill, stroke, width, pos, rotation, scale)
    b.add_native("Canvas.polygon", [shape_draw, read_points, vcolor](NativeCall& c) {
        ProceduralShape shape;
        read_points(c.args[1], shape);
        shape.closed = true;
        if (c.args[2].is_obj()) {
            shape.has_fill = true;
            shape.fill = vcolor(c.args[2]);
        }
        if (c.args[3].is_obj() && nv::f32(c, 4) > 0.0f) {
            shape.has_stroke = true;
            shape.stroke = vcolor(c.args[3]);
            shape.stroke_width = nv::f32(c, 4);
            shape.join = vector::LineJoin::Miter;
        }
        return shape_draw(c, shape, 5);
    });
    // Canvas.polyline(points, color, width, closed, join, cap, gradient, pos, rotation, scale)
    b.add_native("Canvas.polyline", [shape_draw, read_points, vcolor](NativeCall& c) {
        ProceduralShape shape;
        read_points(c.args[1], shape);
        shape.has_stroke = true;
        shape.stroke = vcolor(c.args[2]);
        shape.stroke_width = std::max(nv::f32(c, 3), 0.0f);
        shape.closed = nv::boolean(c, 4);
        const std::int64_t join = nv::integer(c, 5);
        shape.join = join == 0 ? vector::LineJoin::Miter : join == 2 ? vector::LineJoin::Bevel : vector::LineJoin::Round;
        const std::int64_t cap = nv::integer(c, 6);
        shape.cap = cap == 0 ? vector::LineCap::Butt : cap == 2 ? vector::LineCap::Square : vector::LineCap::Round;
        if (c.args[7].is_obj() && c.args[7].o->kind == pluma::ObjKind::List) {
            for (const Value& col : static_cast<const pluma::ListObj*>(c.args[7].o)->items) {
                shape.gradient.push_back(vcolor(col));
            }
        }
        return shape_draw(c, shape, 8);
    });
    // DESIGN.md 4.17 (9.1): off = every Node2D transform is a world transform.
    b.add_native("Scene.transform_hierarchy", [host](NativeCall&) { return Value::boolean(host->options_.transform_hierarchy); });
    b.add_native("Canvas.set_view", [host](NativeCall& c) {
        if (host->targets_ == nullptr || !host->targets_->cameras) return Value{};
        host->flush_draw(DrawFlush::Layer);
        render::Camera2D cam;
        cam.position = to_vec2(nv::vec(c, 1));
        cam.zoom = nv::f32(c, 2) > 0.0f ? nv::f32(c, 2) : 1.0f;
        cam.rotation = nv::f32(c, 3);
        // Camera2D maps world to virtual [0, size]; the screen camera is the identity.
        host->view_ = cam.world_to_virtual(host->options_.virtual_size);
        host->view_zoom_ = cam.zoom;
        return Value{};
    });
    b.add_native("Canvas.reset_view", [host](NativeCall&) {
        if (host->targets_ == nullptr || !host->targets_->cameras) return Value{};
        host->flush_draw(DrawFlush::Layer);
        host->view_ = Affine2{};
        host->view_zoom_ = 1.0f;
        return Value{};
    });

    // --- Scene
    b.add_native("Scene.cancel_tasks", [](NativeCall& c) {
        if (c.args[0].is_obj()) c.vm.cancel_owned(c.args[0].o);
        return Value{};
    });

#if CUERVO_FEATURE_PARTICLES
    // --- Emitter (cuervo/particles)
    auto data = [](NativeCall& c) -> EmitterData* {
        auto* d = static_cast<EmitterData*>(nv::self(c)->native);
        if (d == nullptr) c.vm.panic("this Emitter has no particle emitter (was 'init' skipped?)");
        return d;
    };
    b.add_native("Emitter.init", [host](NativeCall& c) {
        particles::EmitterConfig cfg;
        cfg.record_death_events = false;
        auto layer = nv::integer(c, 2);
        if (layer >= 32) c.vm.panic("Emitter: layer must be < 32");
        if (layer >= 0) {
            cfg.collision.enabled = true;
            cfg.collision.layer = static_cast<std::uint32_t>(layer);
            cfg.collision.hits = static_cast<collision::LayerMask>(nv::integer(c, 3));
            cfg.collision.radius = nv::f32(c, 4);
            cfg.collision.kill_on_hit = nv::boolean(c, 5);
        }
        cfg.render.shape = shape_for(nv::name(c, 6));
        cfg.render.outline_width = nv::f32(c, 7);
        cfg.render.outline_color = to_color(c.args[8]);
        if (nv::boolean(c, 9)) {
            const vec2 margin{64.0f, 64.0f};
            cfg.modules.push_back(particles::KillOutside{margin * -1.0f, host->options_.virtual_size + margin});
        }
        particles::EmitterId id = 0;
        if (!host->free_emitters_.empty()) {
            id = host->free_emitters_.back();
            host->free_emitters_.pop_back();
            host->particles_.replace_emitter(id, std::move(cfg));
        } else {
            id = host->particles_.add_emitter(std::move(cfg));
            host->emitters_.resize(id + 1);
        }
        host->emitters_[id] = {true, std::string(nv::name(c, 1))};
        auto* self = nv::self(c);
        if (self->native != nullptr) host->free_emitter(static_cast<EmitterData*>(self->native)->id);
        delete static_cast<EmitterData*>(self->native);
        self->native = new EmitterData{host, id};
        return Value{};
    });
    b.add_finalizer("Emitter", [](void* p) {
        auto* d = static_cast<EmitterData*>(p);
        d->host->free_emitter(d->id);
        delete d;
    });
    b.add_native("Emitter.fire", [host, data](NativeCall& c) {
        particles::Emitter& e = host->particles_.emitter(data(c)->id);
        e.target = to_vec2(nv::vec(c, 4));
        particles::Pattern p = to_pattern(c.args[1]);
        auto n = e.emit(p, to_vec2(nv::vec(c, 2)), to_template(c.args[3])); // the emitter's own stream
        return Value::integer(pluma::NumKind::I32, n);
    });
    b.add_native("Emitter.spawn", [host, data](NativeCall& c) {
        particles::Emitter& e = host->particles_.emitter(data(c)->id);
        particles::ParticleTemplate t = to_template(c.args[3]);
        particles::ParticleSpawn s;
        s.position = to_vec2(nv::vec(c, 1));
        s.velocity = to_vec2(nv::vec(c, 2));
        s.life = t.life;
        s.size = t.size;
        s.color = render::pack_rgba8(t.color);
        s.spin = t.spin;
        s.rotation = t.face_heading ? dmath::atan2(s.velocity.y(), s.velocity.x()) : 0.0f;
        e.spawn(s);
        return Value{};
    });
    b.add_native("Emitter.add", [host, data](NativeCall& c) {
        host->particles_.emitter(data(c)->id).modules().push_back(to_module(c.args[1]));
        return Value{};
    });
    b.add_native("Emitter.count", [host, data](NativeCall& c) {
        return Value::integer(pluma::NumKind::I32, static_cast<std::int64_t>(host->particles_.emitter(data(c)->id).size()));
    });
    b.add_native("Emitter.kill_near", [host, data](NativeCall& c) {
        particles::Emitter& e = host->particles_.emitter(data(c)->id);
        particles::ParticlePool& pool = e.pool();
        vec2 center = to_vec2(nv::vec(c, 1));
        float r = nv::f32(c, 2);
        std::int64_t killed = 0;
        for (std::size_t i = 0; i < pool.size_live(); ++i) {
            float dx = pool.x[i] - center.x();
            float dy = pool.y[i] - center.y();
            if (dx * dx + dy * dy <= r * r) {
                pool.kill(i);
                ++killed;
            }
        }
        e.remove_dead();
        return Value::integer(pluma::NumKind::I32, killed);
    });
    b.add_native("Emitter.clear", [host, data](NativeCall& c) {
        host->particles_.emitter(data(c)->id).clear();
        return Value{};
    });
    b.add_native("Emitter.draw", [host, data](NativeCall& c) {
        if (render::SdfBatcher* shapes = host->shape_target()) {
            particles::draw_emitter(host->particles_.emitter(data(c)->id), *shapes, 1.0f);
        }
        return Value{};
    });
#endif

    bind_scene_and_ui();
    bind_animation();

#if CUERVO_FEATURE_COLLISION
    // --- Collision (DESIGN.md 4.7)
    b.add_native("Collision.hitbox", [host](NativeCall& c) {
        auto layer = nv::integer(c, 3);
        if (layer < 0 || layer >= 32) c.vm.panic("Collision.hitbox: layer must be 0..31");
        if (!c.args[0].is_obj()) c.vm.panic("Collision.hitbox: owner is null");
        Hitbox h;
        h.owner = c.args[0];
        pluma::retain(h.owner);
        h.center = to_vec2(nv::vec(c, 1));
        h.radius = nv::f32(c, 2);
        h.layer = static_cast<std::uint32_t>(layer);
        host->hitboxes_.push_back(h);
        return Value{};
    });
    // Area(owner, capsule, center, radius, half_length, rotation, layer, mask)
    b.add_native("Collision.area", [host](NativeCall& c) {
        auto layer = nv::integer(c, 6);
        if (layer < 0 || layer >= 32) c.vm.panic("Collision.area: layer must be 0..31");
        if (!c.args[0].is_obj()) c.vm.panic("Collision.area: owner is null");
        Hitbox h;
        h.owner = c.args[0];
        pluma::retain(h.owner);
        h.center = to_vec2(nv::vec(c, 2));
        h.radius = std::max(nv::f32(c, 3), 0.0f);
        h.layer = static_cast<std::uint32_t>(layer);
        h.area = true;
        h.mask = static_cast<std::uint32_t>(nv::integer(c, 7));
        const float half = nv::f32(c, 4);
        h.capsule = nv::boolean(c, 1) && half > 0.0f;
        if (h.capsule) {
            // Simulation geometry: dmath, not libm (DESIGN.md 4.1).
            const float rot = nv::f32(c, 5);
            const vec2 dir{dmath::cos(rot) * half, dmath::sin(rot) * half};
            h.start = h.center - dir;
            h.end = h.center + dir;
        }
        host->hitboxes_.push_back(h);
        return Value{};
    });
    b.add_native("Collision.count", [host](NativeCall& c) {
#if CUERVO_FEATURE_PARTICLES
        const collision::CollisionWorld& w = host->particles_.collision();
#else
        const collision::CollisionWorld& w = host->collision_;
#endif
        if (!w.built()) return Value::integer(pluma::NumKind::I32, 0);
        std::vector<collision::ProxyId> out;
        w.overlap(collision::Circle{to_vec2(nv::vec(c, 0)), nv::f32(c, 1)},
                  static_cast<collision::LayerMask>(nv::integer(c, 2)), out);
        return Value::integer(pluma::NumKind::I32, static_cast<std::int64_t>(out.size()));
    });
#endif
}

// ------------------------------------------------------------ lifecycle

bool ScriptHost::check(const pluma::RunResult& r, const char* what) {
    if (r.ok) return true;
    panicked_ = true;
    last_panic_ = r.panic;
    error_ = std::format("panic in {}: {}", what, r.panic->render());
    LOG_ERROR("{}", error_);
    return false;
}

void ScriptHost::set_time_globals() {
    session_->set_global("Time.tick", Value::integer(pluma::NumKind::I32, static_cast<std::int64_t>(tick_)));
    session_->set_global("Time.dt", Value::f32(tick_seconds()));
    session_->set_global("Time.seconds", Value::f32(static_cast<float>(static_cast<double>(tick_) / options_.tick_rate)));
}

bool ScriptHost::load(const std::vector<pluma::SourceFile::Part>& files, const std::string& main_class) {
    notes_.clear();
    auto next = std::make_unique<pluma::Session>(vm_options(), &bindings_);
    install_debug_hooks(*next);
    // Profiling follows the game onto each new VM, global initializers included.
    // A VM has one DebugHooks: under a debugger the game isn't profiled.
    if (profiling_scripts_ && options_.debug_hooks == nullptr) next->vm().set_debug_hooks(script_profiler_.get());
    if (!next->prelude_result().ok) {
        error_ = "engine prelude failed to compile:\n" + next->prelude_result().rendered;
        LOG_ERROR("{}", error_);
        return false;
    }
    pluma::SessionResult r = next->load_files(files);
    if (!r.compiled) {
        error_ = r.rendered;
        LOG_ERROR("Script errors:\n{}", r.rendered);
        return false;
    }
    if (!r.ok) {
        error_ = "panic while initializing globals: " + r.panic->render();
        last_panic_ = r.panic;
        LOG_ERROR("{}", error_);
        return false;
    }
    return finish_load(std::move(next), main_class);
}

bool ScriptHost::load_native(const std::filesystem::path& so_path, const std::string& main_class) {
    notes_.clear();
    std::string err;
    auto mod = pluma::AotModule::open(so_path, err);
    if (!mod) {
        error_ = std::move(err);
        LOG_ERROR("{}", error_);
        return false;
    }
    auto next = std::make_unique<pluma::Session>(vm_options(), &bindings_, /*compile_preludes=*/false);
    install_debug_hooks(*next);
    pluma::SessionResult r = next->load_aot(std::move(mod));
    if (!r.compiled) {
        error_ = r.rendered;
        LOG_ERROR("{}", error_);
        return false;
    }
    if (!r.ok) {
        error_ = "panic while initializing globals: " + r.panic->render();
        last_panic_ = r.panic;
        LOG_ERROR("{}", error_);
        return false;
    }
    return finish_load(std::move(next), main_class);
}

bool ScriptHost::load_native(std::span<const std::byte> so_bytes, const std::string& main_class) {
    notes_.clear();
    std::string err;
    auto mod = pluma::AotModule::open_bytes(so_bytes, err);
    if (!mod) {
        error_ = std::move(err);
        LOG_ERROR("{}", error_);
        return false;
    }
    auto next = std::make_unique<pluma::Session>(vm_options(), &bindings_, /*compile_preludes=*/false);
    install_debug_hooks(*next);
    pluma::SessionResult r = next->load_aot(std::move(mod));
    if (!r.compiled) {
        error_ = r.rendered;
        LOG_ERROR("{}", error_);
        return false;
    }
    if (!r.ok) {
        error_ = "panic while initializing globals: " + r.panic->render();
        last_panic_ = r.panic;
        LOG_ERROR("{}", error_);
        return false;
    }
    return finish_load(std::move(next), main_class);
}

bool ScriptHost::finish_load(std::unique_ptr<pluma::Session> next, const std::string& main_class) {
    pluma::VM& vm = next->vm();
    next->set_global("Screen.size", pluma::vec2_value(options_.virtual_size.x(), options_.virtual_size.y()));

    const pluma::RecordRT* canvas_rt = vm.find_class("Canvas");
    Value canvas;
    if (!vm.instantiate(canvas_rt, &canvas).ok) return false;
    static_cast<pluma::InstanceObj*>(canvas.o)->native = this;

    Value root;
    const bool has_scene = !options_.main_scene.empty() || options_.main_scene_doc != nullptr;
    if (session_ == nullptr && main_class.empty() && !has_scene) {
        // Compile only (the editor): the caller supplies a root with set_root.
        session_ = std::move(next);
        activated();
        canvas_ = canvas;
        panicked_ = false;
        error_.clear();
        last_panic_.reset();
        set_time_globals();
        return true;
    }
    if (session_ == nullptr && has_scene) {
        // project.toml `main_scene`: the root is a scene file (DESIGN.md 4.12,
        // "Clarified (Phase 6A)"), built like Scene.load, then entered.
        session_ = std::move(next);
        activated();
        canvas_ = canvas;
        panicked_ = false;
        error_.clear();
        last_panic_.reset();
        set_time_globals();
        std::string err;
        std::optional<scene::SceneDoc> doc;
        if (options_.main_scene_doc) {
            doc = *options_.main_scene_doc;
        } else {
            doc = load_scene(options_.main_scene, err, nullptr);
        }
        if (!doc) {
            error_ = "main_scene: " + err;
            LOG_ERROR("{}", error_);
            panicked_ = true;
            return true;
        }
        std::vector<std::string> warnings;
        pluma::RunResult made = instantiate_tree(session_->vm(), *doc, &root, warnings);
        for (const std::string& w : warnings) LOG_WARN("main_scene '{}': {}", options_.main_scene, w);
        if (!check(made, "main_scene")) return true;
        set_root(root, true);
        pluma::release(root);
        return true;
    }
    if (session_ == nullptr) {
        const pluma::RecordRT* cls = vm.find_class(main_class);
        if (cls == nullptr || !cls->is_subclass_of(vm.find_class("Node"))) {
            error_ = std::format("main class '{}' is not a class extending Node", main_class);
            LOG_ERROR("{}", error_);
            pluma::release(canvas);
            return false;
        }
        pluma::RunResult made = vm.instantiate(cls, &root);
        if (made.ok) {
            if (auto init = vm.proto_by_key(main_class + ".init")) {
                Value self = root;
                made = vm.call(*init, std::span<const Value>(&self, 1));
            }
        }
        session_ = std::move(next);
        activated();
        canvas_ = canvas;
        root_ = root;
        panicked_ = false;
        error_.clear();
        last_panic_.reset();
        set_time_globals();
        session_->set_global("Scene.root", root_);
        if (!check(made, "the main class's initializer")) return true;
        check(session_->vm().call_method(root_, "enter_tree"), "ready()");
        return true;
    }

    // Hot reload (DESIGN.md 5.13).
    pluma::Migrator m(session_->vm(), vm);
    m.begin();
    root = m.migrate(root_);
    m.migrate_globals();
    pluma::RunResult restarted = m.finish();
    notes_ = m.warnings();
    for (const std::string& w : notes_) LOG_WARN("hot reload: {}", w);
    LOG_INFO("Hot reload: {} objects migrated, {} coroutines restarted", m.objects_migrated(),
             m.coroutines_restarted());
    pluma::release(root_);
    pluma::release(canvas_);
    std::unique_ptr<pluma::Session> old = std::move(session_);
    session_ = std::move(next);
    activated();
    old.reset();
    root_ = root;
    canvas_ = canvas;
    ++reloads_;
    panicked_ = false;
    error_.clear();
    last_panic_.reset();
    set_time_globals();
    check(restarted, "a restarted coroutine");
    return true;
}

bool ScriptHost::set_root(const Value& root, bool enter_tree) {
    if (session_ == nullptr) return false;
    pluma::retain(root);
    if (root_.is_obj() && !(root.is_obj() && root.o == root_.o)) session_->vm().reset_scene_arena();
    pluma::release(root_);
    root_ = root;
    session_->set_global("Scene.root", root_);
    if (enter_tree && root_.is_obj()) return check(session_->vm().call_method(root_, "enter_tree"), "ready()");
    return true;
}

void ScriptHost::clear_error() {
    panicked_ = false;
    error_.clear();
    last_panic_.reset();
}

void ScriptHost::tick(const InputFrame& frame) {
    if (!running() || !root_.is_obj()) return;
    CUERVO_PROFILE_ZONE("scripts");
    tick_scripts(frame);
    CUERVO_PROFILE_ZONE("gc");
    end_tick();
}

// Step 6 (see script_host.hpp): after every script and coroutine of the tick.
void ScriptHost::end_tick() {
    if (session_ == nullptr) return;
    pluma::VM& vm = session_->vm();
    vm.reset_frame_arena();
    check(vm.collect_cycles(options_.gc_budget), "the cycle collector");
}

void ScriptHost::tick_scripts(const InputFrame& frame) {
    pluma::VM& vm = session_->vm();
    ++tick_;
#if CUERVO_FEATURE_AUDIO
    // Whatever path the tick leaves by, its audio triggers go out as one batch.
    struct AudioFlush {
        audio::AudioSystem& a;
        std::uint64_t t;
        ~AudioFlush() { a.end_tick(t); }
    } audio_flush{*audio_, tick_};
#endif
    set_time_globals();
    input_.push(frame);
    text_wanted_ = false; // UI.wants_text sets it again while a field is being edited
    bool any = frame.held != 0 || frame.text != 0;
    for (std::int16_t a : frame.axes) any = any || a != 0;
    idle_ticks_ = any ? 0 : idle_ticks_ + 1;

    const bool pause = paused();
    pluma::VM::FrozenFn frozen_fn;
    if (pause) frozen_fn = [this](const pluma::Object* owner) { return frozen(owner); };
    {
        CUERVO_PROFILE_ZONE("coroutines");
        if (!check(vm.tick_coroutines(frozen_fn), "a coroutine")) return;
    }
    Value dt = Value::f32(tick_seconds());
    {
        CUERVO_PROFILE_ZONE("update");
        if (!check(vm.call_method(root_, "update_tree", std::span<const Value>(&dt, 1)), "update()")) return;
    }
    if (pause) {
        // Gameplay particles stand still during a pause; hitboxes registered
        // by nodes that still process are dropped.
#if CUERVO_FEATURE_COLLISION
        for (Hitbox& h : hitboxes_) pluma::release(h.owner);
        hitboxes_.clear();
#endif
        if (auto flush = vm.proto_by_key("Scene.flush")) check(vm.call(*flush, {}), "Scene.flush()");
        return;
    }

#if CUERVO_FEATURE_PARTICLES
    // Hitboxes are game shapes: `user` is the hitbox index (bit 63 clear).
    particles_.tick(tick_seconds(), [this](collision::CollisionWorld& w) {
        (void)w;
#if CUERVO_FEATURE_COLLISION
        add_hitboxes(w);
#endif
    });
    // on_hit(tag, count) per (hitbox, emitter), in hit order.
    struct Count {
        std::size_t hitbox;
        particles::EmitterId emitter;
        std::int64_t n;
    };
    std::vector<Count> counts;
    for (const particles::Hit& h : particles_.hits()) {
        if (particles::ParticleSystem::is_particle(h.target_user) || h.target_user >= hitboxes_.size()) continue;
        auto it = std::find_if(counts.begin(), counts.end(), [&](const Count& c) {
            return c.hitbox == h.target_user && c.emitter == h.emitter;
        });
        if (it != counts.end()) {
            ++it->n;
        } else {
            counts.push_back({static_cast<std::size_t>(h.target_user), h.emitter, 1});
        }
    }
    bool ok = true;
    {
        CUERVO_PROFILE_ZONE("on_hit");
        for (const Count& c : counts) {
            const Value args[2] = {name_value(emitters_[c.emitter].tag), Value::integer(pluma::NumKind::I32, c.n)};
            if (!check(vm.call_method(hitboxes_[c.hitbox].owner, "on_hit", args), "on_hit()")) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        CUERVO_PROFILE_ZONE("areas");
        ok = resolve_areas(particles_.collision());
    }
#elif CUERVO_FEATURE_COLLISION
    collision_.clear();
    add_hitboxes(collision_);
    collision_.build();
    bool ok = resolve_areas(collision_);
#else
    bool ok = true;
#endif

#if CUERVO_FEATURE_COLLISION
    for (Hitbox& h : hitboxes_) pluma::release(h.owner);
    hitboxes_.clear();
#endif
    if (!ok) return;
    CUERVO_PROFILE_ZONE("scene flush");
    if (auto flush = vm.proto_by_key("Scene.flush")) check(vm.call(*flush, {}), "Scene.flush()");
}

void ScriptHost::draw(render::SdfBatcher& shapes, render::TextRenderer& text, render::FontHandle font,
                      const std::function<void()>& layer_break) {
    DrawTargets t;
    t.shapes = &shapes;
    t.text = &text;
    t.font = font;
    t.cameras = false;
    if (layer_break) {
        t.flush = [&layer_break](DrawFlush what, const Affine2&, float) {
            if (what == DrawFlush::Layer) layer_break();
        };
    }
    draw(t);
}

DrawTargets make_draw_targets(render::SdfBatcher& shapes, render::TextRenderer& text, render::FontHandle font,
                              VectorCache* vectors, const Affine2& to_ndc, float pixels_per_unit, bool cameras) {
    DrawTargets t;
    t.shapes = &shapes;
    t.text = &text;
    t.font = font;
    t.vectors = vectors;
    t.pixels_per_unit = pixels_per_unit;
    t.cameras = cameras;
    render::SdfBatcher* s = &shapes;
    render::TextRenderer* tx = &text;
    t.flush = [s, tx, vectors, to_ndc, pixels_per_unit](DrawFlush what, const Affine2& view, float zoom) {
        const Affine2 m = to_ndc * view;
        const float px = pixels_per_unit * zoom;
        if (what != DrawFlush::Meshes) s->flush(m, px > 0.0f ? 1.0f / px : 1.0f);
        if (what != DrawFlush::Shapes && vectors != nullptr) {
            vectors->batcher().flush(m);
            vectors->batcher().clear();
        }
        if (what == DrawFlush::Layer) tx->flush(m);
    };
    return t;
}

render::SdfBatcher* ScriptHost::shape_target() {
    if (meshes_pending_) flush_draw(DrawFlush::Meshes);
    return shapes_;
}

void ScriptHost::flush_draw(DrawFlush what) {
    if (targets_ == nullptr) return;
    if (what != DrawFlush::Shapes) meshes_pending_ = false;
    if (targets_->flush) targets_->flush(what, view_, view_zoom_);
}

void ScriptHost::draw(const DrawTargets& targets) {
    if (session_ == nullptr || !root_.is_obj()) return;
    shapes_ = targets.shapes;
    text_ = targets.text;
    font_ = targets.font;
    targets_ = &targets;
    view_ = Affine2{};
    view_zoom_ = 1.0f;
    meshes_pending_ = false;
    if (targets.vectors != nullptr) targets.vectors->begin_shapes();
    pluma::VM& vm = session_->vm();
    pluma::RunResult r;
    if (auto frame = vm.proto_by_key("Scene.draw_frame")) {
        r = vm.call(*frame, std::span<const Value>(&canvas_, 1));
    } else {
        r = vm.call_method(root_, "draw_tree", std::span<const Value>(&canvas_, 1));
    }
    if (!r.ok && !panicked_) check(r, "draw()");
    // Everything goes out with the view it was drawn in.
    flush_draw(DrawFlush::Layer);
    shapes_ = nullptr;
    text_ = nullptr;
    targets_ = nullptr;
    meshes_pending_ = false;
}

std::uint64_t ScriptHost::state_hash() const {
    StateHasher h;
#if CUERVO_FEATURE_PARTICLES
    h.value(particles_.state_hash());
#else
    h.value(0ULL);
#endif
    h.value(tick_);
    if (anim_digest_ != 0) h.value(anim_digest_);
    if (session_ != nullptr) {
        // Script globals that are numbers or vectors (scores, timers...).
        pluma::VM& vm = session_->vm();
        for (const Value& g : vm.globals) {
            if (pluma::is_num_tag(g.tag) || g.tag == pluma::Tag::Bool) h.value(static_cast<std::uint64_t>(g.i));
            if (pluma::is_vec(g)) {
                for (std::uint32_t i = 0; i < pluma::vec_size(g); ++i) h.value(pluma::vec_data(g)[i]);
            }
        }
    }
    return h.digest();
}

std::optional<std::string> emit_aot_c(const std::vector<pluma::SourceFile::Part>& files, std::string& error) {
    InputMap empty_map;
    ScriptHost host(HostOptions{}, empty_map);
    const pluma::Bindings& bindings = host.bindings();

    pluma::Program prog;
    pluma::VM vm;
    for (const pluma::Bindings::Native& n : bindings.natives()) {
        prog.native_names.insert(n.name);
        vm.register_native(n.name, n.fn);
    }
    for (const pluma::Bindings::Finalizer& f : bindings.finalizers()) vm.set_native_finalizer(f.class_name, f.fn);

    std::vector<pluma::SourceFile::Part> prelude_parts;
    for (const pluma::Bindings::Prelude& p : bindings.preludes()) prelude_parts.push_back({p.path, p.source});
    auto prelude_src = pluma::SourceFile::combine("<prelude>", prelude_parts);
    pluma::Arena prelude_arena;
    pluma::DiagnosticSink pdiags(*prelude_src);
    pluma::Module pmod = pluma::parse_module(*prelude_src, prelude_arena, pdiags);
    if (pdiags.has_errors()) {
        pdiags.sort_by_position();
        error = "engine prelude failed to compile:\n" + pluma::render_all(pdiags.all(), *prelude_src);
        return std::nullopt;
    }
    pluma::CheckedUnit punit = pluma::check_module(prog, *prelude_src, pmod, pdiags);
    if (!punit.ok) {
        pdiags.sort_by_position();
        error = "engine prelude failed to compile:\n" + pluma::render_all(pdiags.all(), *prelude_src);
        return std::nullopt;
    }
    pluma::compile_program(prog, vm);

    std::string proj_name = files.size() == 1 ? files[0].path : "<project>";
    auto proj_src = pluma::SourceFile::combine(std::move(proj_name), files);
    pluma::Arena proj_arena;
    pluma::DiagnosticSink diags(*proj_src);
    pluma::Module mod = pluma::parse_module(*proj_src, proj_arena, diags);
    if (diags.has_errors()) {
        diags.sort_by_position();
        error = pluma::render_all(diags.all(), *proj_src);
        return std::nullopt;
    }
    pluma::CheckedUnit unit = pluma::check_module(prog, *proj_src, mod, diags);
    if (!unit.ok) {
        diags.sort_by_position();
        error = pluma::render_all(diags.all(), *proj_src);
        return std::nullopt;
    }
    try {
        pluma::compile_program(prog, vm);
    } catch (const std::runtime_error& e) {
        error = e.what();
        return std::nullopt;
    }
    return pluma::emit_c(prog, vm);
}

std::optional<std::vector<std::byte>> compile_aot_scripts(const std::vector<pluma::SourceFile::Part>& files,
                                                          std::string& error, int opt_level) {
    auto c_src = emit_aot_c(files, error);
    if (!c_src) return std::nullopt;
    static std::atomic<std::uint64_t> counter{0};
#if defined(_WIN32)
    const auto pid = static_cast<unsigned>(::_getpid());
#else
    const auto pid = static_cast<unsigned>(::getpid());
#endif
    std::filesystem::path so_path =
        std::filesystem::temp_directory_path() / std::format("cuervo_aot_{}_{}.so", pid, ++counter);
    if (!pluma::compile_c_module(*c_src, so_path, error, opt_level)) return std::nullopt;
    std::ifstream in(so_path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::error_code ec;
        std::filesystem::remove(so_path, ec);
        error = std::format("can't read compiled AOT module '{}'", so_path.string());
        return std::nullopt;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(std::max<std::streamsize>(0, size)));
    bool ok = size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(bytes.data()), size));
    in.close();
    std::error_code ec;
    std::filesystem::remove(so_path, ec);
    if (!ok) {
        error = std::format("can't read compiled AOT module '{}'", so_path.string());
        return std::nullopt;
    }
    return bytes;
}

std::vector<pluma::SourceFile::Part> engine_preludes() { return joined_preludes(); }

pluma::lsp::ServerOptions default_lsp_options(std::string root_path, bool check_style) {
    pluma::lsp::ServerOptions opts;
    opts.preludes = engine_preludes();
    opts.root_path = std::move(root_path);
    opts.check_style = check_style;
    return opts;
}

} // namespace cuervo::script

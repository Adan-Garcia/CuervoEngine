#include "editor/particle_preview.hpp"

#include <algorithm>
#include <cmath>

namespace cuervo::editor {

namespace {

float number_of(const scene::Value& v, float fallback) {
    if (v.kind == scene::ValueKind::Float) return static_cast<float>(v.f);
    if (v.kind == scene::ValueKind::Int) return static_cast<float>(v.i);
    return fallback;
}

std::array<float, 4> color_from(const scene::Value& v, std::array<float, 4> fallback) {
    if (v.kind != scene::ValueKind::Struct) return fallback;
    for (const scene::Field& f : v.fields) {
        if (f.name == "r") fallback[0] = number_of(f.value, fallback[0]);
        if (f.name == "g") fallback[1] = number_of(f.value, fallback[1]);
        if (f.name == "b") fallback[2] = number_of(f.value, fallback[2]);
        if (f.name == "a") fallback[3] = number_of(f.value, fallback[3]);
    }
    return fallback;
}

constexpr float pi = 3.14159265358979f;

} // namespace

EmitterParams read_emitter_params(const EditorDocument& doc, NodeId id) {
    EmitterParams p;
    const SceneModel& model = doc.model();
    const ModelNode* node = id != 0 ? model.get(id) : nullptr;
    if (node == nullptr || node->type != "Emitter2D") return p;
    p.valid = true;
    const auto num = [&](const char* field, float fallback) { return number_of(doc.field_value(id, field), fallback); };
    p.one_shot = doc.field_value(id, "one_shot").b;
    p.interval = std::max(num("interval", p.interval), 0.001f);
    const scene::Value pattern = doc.field_value(id, "pattern");
    if (pattern.kind == scene::ValueKind::Enum && !pattern.text.empty()) p.pattern = pattern.text;
    p.count = std::clamp(static_cast<int>(num("count", 12.0f)), 0, 2000);
    p.direction = num("direction", 0.0f);
    p.spread = num("spread", 1.0f);
    p.speed = num("speed", 120.0f);
    p.speed_jitter = num("speed_jitter", 0.0f);
    p.life = num("life", 1.5f);
    p.life_jitter = num("life_jitter", 0.0f);
    p.size = num("size", 4.0f);
    p.color = color_from(doc.field_value(id, "color"), p.color);
    p.fade = doc.field_value(id, "fade").b;
    p.end_color = color_from(doc.field_value(id, "end_color"), p.end_color);
    p.end_scale = num("end_scale", 1.0f);
    p.spin = num("spin", 0.0f);
    const scene::Value g = doc.field_value(id, "gravity");
    if (g.kind == scene::ValueKind::Vec && g.vec.size() >= 2) p.gravity = {g.vec[0], g.vec[1]};
    p.drag = num("drag", 0.0f);
    const scene::Value shape = doc.field_value(id, "particle_shape");
    if (!shape.text.empty()) p.shape = shape.text;
    return p;
}

float EmitterPreview::random01() {
    // xorshift64*: local to the preview.
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    const std::uint64_t r = rng_ * 2685821657736338717ull;
    return static_cast<float>(r >> 40) / static_cast<float>(1u << 24);
}

void EmitterPreview::restart() {
    particles_.clear();
    since_shot_ = 1.0e9f;
    shots_ = 0;
    done_ = false;
}

void EmitterPreview::fire(const EmitterParams& p) {
    ++shots_;
    const int n = p.count;
    for (int i = 0; i < n; ++i) {
        float heading = p.direction;
        if (p.pattern == "Ring") {
            heading += 2.0f * pi * static_cast<float>(i) / static_cast<float>(std::max(n, 1));
        } else if (p.pattern == "Arc") {
            const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) - 0.5f : 0.0f;
            heading += p.spread * t;
        } else if (p.pattern == "Burst") {
            heading += p.spread * (random01() * 2.0f - 1.0f);
        } // Point: along `direction`
        const float speed = p.speed + p.speed_jitter * (random01() * 2.0f - 1.0f);
        PreviewParticle q;
        q.velocity = {std::cos(heading) * speed, std::sin(heading) * speed};
        q.life = std::max(p.life + p.life_jitter * (random01() * 2.0f - 1.0f), 0.05f);
        particles_.push_back(q);
    }
}

void EmitterPreview::step(const EmitterParams& p, float dt) {
    if (!p.valid) return;
    if (!done_) {
        since_shot_ += dt;
        if (since_shot_ >= p.interval) {
            since_shot_ = 0.0f;
            fire(p);
            if (p.one_shot) done_ = true;
        }
    }
    for (PreviewParticle& q : particles_) {
        q.velocity = q.velocity + p.gravity * dt;
        if (p.drag > 0.0f) q.velocity = q.velocity * std::max(0.0f, 1.0f - p.drag * dt);
        q.position = q.position + q.velocity * dt;
        q.angle += p.spin * dt;
        q.age += dt;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const PreviewParticle& q) { return q.age >= q.life; }),
                     particles_.end());
}

std::array<float, 4> EmitterPreview::color_of(const EmitterParams& p, const PreviewParticle& q) {
    if (!p.fade) return p.color;
    const float t = std::clamp(q.age / q.life, 0.0f, 1.0f);
    std::array<float, 4> c{};
    for (int i = 0; i < 4; ++i) c[static_cast<std::size_t>(i)] = p.color[static_cast<std::size_t>(i)] + (p.end_color[static_cast<std::size_t>(i)] - p.color[static_cast<std::size_t>(i)]) * t;
    return c;
}

float EmitterPreview::radius_of(const EmitterParams& p, const PreviewParticle& q) {
    const float t = std::clamp(q.age / q.life, 0.0f, 1.0f);
    return p.size * (1.0f + (p.end_scale - 1.0f) * t);
}

} // namespace cuervo::editor

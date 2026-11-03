#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "cuervo/math/vec.hpp"
#include "editor/editor_document.hpp"

// The particle editor's live preview (DESIGN.md 4.13, milestone 11.7c): an
// `Emitter2D`'s exported fields (prelude particles.plm), read from the
// document, drive a small CPU simulation that the Particles panel draws, so
// tuning a slider shows its effect at once. It is editor-only render state: it
// follows the node's pattern, speed, life, gravity, drag, size, spin and
// color/size over life closely, but the game's own Emitter (the engine's
// particle system, which owns determinism) stays the reference, and a played
// scene is what gets hashed. Its random numbers come from a local generator,
// never the simulation's.
namespace cuervo::editor {

struct EmitterParams {
    bool valid = false;
    bool one_shot = false;
    float interval = 0.25f;
    std::string pattern = "Ring"; // Ring, Arc, Burst or Point
    int count = 12;
    float direction = 0.0f;
    float spread = 1.0f;
    float speed = 120.0f;
    float speed_jitter = 0.0f;
    float life = 1.5f;
    float life_jitter = 0.0f;
    float size = 4.0f;
    std::array<float, 4> color{1, 1, 1, 1};
    bool fade = false;
    std::array<float, 4> end_color{1, 1, 1, 0};
    float end_scale = 1.0f;
    float spin = 0.0f;
    vec2 gravity{0.0f, 0.0f};
    float drag = 0.0f;
    std::string shape = "circle";
};

// The selected node's parameters (valid = false unless it is an Emitter2D).
EmitterParams read_emitter_params(const EditorDocument& doc, NodeId id);

struct PreviewParticle {
    vec2 position{0.0f, 0.0f};
    vec2 velocity{0.0f, 0.0f};
    float age = 0.0f;
    float life = 1.0f;
    float angle = 0.0f;
};

class EmitterPreview {
public:
    explicit EmitterPreview(std::uint64_t seed = 1) : rng_(seed * 2862933555777941757ull + 3037000493ull) {}

    // Advances by `dt` seconds with `params`: fires every `interval` (one shot:
    // once), moves, ages and drops dead particles. Changed parameters apply to
    // the next step; live particles keep flying.
    void step(const EmitterParams& params, float dt);
    // Starts over: no particles, the first shot at the next step.
    void restart();

    const std::vector<PreviewParticle>& particles() const { return particles_; }
    std::uint32_t shots() const { return shots_; }

    // The color and radius of `p` under `params` (fade and end_scale applied).
    static std::array<float, 4> color_of(const EmitterParams& params, const PreviewParticle& p);
    static float radius_of(const EmitterParams& params, const PreviewParticle& p);

private:
    float random01();
    void fire(const EmitterParams& params);

    std::vector<PreviewParticle> particles_;
    std::uint64_t rng_;
    float since_shot_ = 1.0e9f; // fire right away
    std::uint32_t shots_ = 0;
    bool done_ = false;
};

} // namespace cuervo::editor

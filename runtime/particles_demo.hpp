#pragma once

#include <cstdint>
#include <string>

#include "cuervo/platform/window.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/rhi/rhi.hpp"

// `cuervo --demo particles`: a bullet-hell pattern on the engine particle
// system (cuervo/particles) and collision world (cuervo/collision). A boss
// fires spirals, flower rings, wavy curtains, aimed fans, homing missiles and
// orbiters (10k+ gameplay particles); the player's hitbox is a small circle
// on its own layer, bullets inside the graze radius are highlighted, hits
// flash and throw visual-mode sparks. Player shots hit the boss through a
// second layer pair. The simulation is deterministic (fixed tick, one seed).
//   arrows / left stick   move          Shift / Pad RB   focus (slow)
//   (no input: autopilot) Space / Pad A  toggle autopilot  Escape  quit
//
// --bench <s> turns VSync off, waits for the bullet count to settle, then
// logs frame and simulation timings (sim split into particles / collision
// build / queries+hits).
struct ParticlesDemoOptions {
    std::string font_path;    // HUD font (TTF/OTF or .efont); empty = no HUD
    std::string screenshot;   // capture at tick `screenshot_tick` and exit
    std::uint64_t screenshot_tick = 240;
    double quit_after = 0.0;
    double bench_seconds = 0.0;
    float intensity = 1.0f;   // scales pattern counts (1 = ~12k bullets)
    std::uint64_t seed = 0xB0551E5ULL;
    cuervo::render::ScaleMode scale = cuervo::render::ScaleMode::Fit;
};

int run_particles_demo(cuervo::Window& window, cuervo::rhi::Device& device, const ParticlesDemoOptions& options);

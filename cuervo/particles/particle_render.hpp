#pragma once

#include <span>

#include "cuervo/particles/emitter.hpp"
#include "cuervo/render/sdf_batcher.hpp"

// Draws emitters through the instanced SDF batcher (DESIGN.md 4.6
// "Rendering": SDF shapes are the default). Rendering only reads the pool,
// so it never affects the simulation.
namespace cuervo::jobs {
class JobSystem;
}

namespace cuervo::particles {

class ParticleSystem;

// Appends one SdfInstance per live particle of `emitter` (in pool order),
// using its RenderConfig. Positions are interpolated between the previous and
// current tick by `alpha` (FixedTimestep::Step::alpha). Returns the number of
// instances added.
std::size_t draw_emitter(const Emitter& emitter, render::SdfBatcher& batch, float alpha);

// Number of instances draw_emitter would add (0 when invisible).
std::size_t instance_count(const Emitter& emitter);

// Writes particles [begin, end) of `emitter` as instances to out[0, end - begin)
// (SDF instance packing, the SIMD/job unit of render preparation).
void pack_instances(const Emitter& emitter, std::size_t begin, std::size_t end, float alpha,
                    std::span<render::SdfInstance> out);

// draw_emitter for every emitter of `system` in id order, packed in parallel
// ranges on `jobs` (DESIGN.md 4.15 "render preparation"); nullptr = inline.
// Same instances in the same order as calling draw_emitter in a loop.
std::size_t draw_emitters(const ParticleSystem& system, render::SdfBatcher& batch, float alpha,
                          jobs::JobSystem* jobs);

} // namespace cuervo::particles

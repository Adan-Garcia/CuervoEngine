#pragma once

#include <cstddef>

#include "cuervo/math/vec.hpp"
#include "cuervo/particles/modules.hpp"
#include "cuervo/particles/particle_pool.hpp"

// Explicit SIMD versions of the particle hot loops (DESIGN.md 4.15):
// integration, every native module, the lifetime check. Each kernel processes
// particles [begin, end) in whole SIMD blocks and returns the index of the
// first particle it did not process; the caller's scalar loop (emitter.cpp)
// finishes the remainder. Every kernel mirrors its scalar loop operation for
// operation, so results are bit-identical (tests/cuervo/simd_tests.cpp).
// Without a SIMD ISA (CUERVO_SIMD=none) every kernel processes nothing.
namespace cuervo::particles::kernels {

struct Frame {
    float dt;
    vec2 position; // emitter.position (Orbit)
    vec2 target;   // emitter.target (Homing)
};

std::size_t integrate(ParticlePool& p, float dt, std::size_t begin, std::size_t end);
std::size_t lifetime(ParticlePool& p, std::size_t begin, std::size_t end);
// One module; dispatches on the variant.
std::size_t module(ParticlePool& p, const Module& m, const Frame& f, std::size_t begin, std::size_t end);

} // namespace cuervo::particles::kernels

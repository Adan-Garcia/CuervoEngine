#pragma once

#include <cstdint>

namespace cuervo {

// PCG32 (pcg-random.org, XSH-RR variant). The simulation's only source of
// randomness: same seed + same calls = same sequence on every platform.
class Pcg32 {
public:
    constexpr Pcg32() { seed(0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL); }
    constexpr Pcg32(std::uint64_t init_state, std::uint64_t sequence = 0xda3e39cb94b95bdbULL) {
        seed(init_state, sequence);
    }

    constexpr void seed(std::uint64_t init_state, std::uint64_t sequence) {
        state_ = 0;
        inc_ = (sequence << 1u) | 1u;
        next_u32();
        state_ += init_state;
        next_u32();
    }

    constexpr std::uint32_t next_u32() {
        std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    // Uniform in [0, bound), without modulo bias.
    constexpr std::uint32_t next_below(std::uint32_t bound) {
        if (bound == 0) {
            return 0;
        }
        std::uint32_t threshold = (0u - bound) % bound;
        for (;;) {
            std::uint32_t r = next_u32();
            if (r >= threshold) {
                return r % bound;
            }
        }
    }

    // Uniform integer in [lo, hi] (inclusive).
    constexpr std::int32_t range(std::int32_t lo, std::int32_t hi) {
        auto span = static_cast<std::uint32_t>(static_cast<std::int64_t>(hi) - lo + 1);
        return static_cast<std::int32_t>(static_cast<std::int64_t>(lo) + next_below(span));
    }

    // Uniform float in [0, 1), built from the top 24 bits (exact in f32).
    constexpr float next_float() {
        return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f);
    }

    // Uniform float in [lo, hi).
    constexpr float range(float lo, float hi) { return lo + (hi - lo) * next_float(); }

    friend constexpr bool operator==(const Pcg32&, const Pcg32&) = default;

private:
    std::uint64_t state_ = 0;
    std::uint64_t inc_ = 0;
};

// --- sub-seeded streams (DESIGN.md 4.15 "Sub-seeding the RNG") -------------
//
// A simulation has one parent seed. Systems that need randomness off the main
// sequence (each particle emitter, each parallel job) take a child stream
// derived from the parent seed and a stable key, instead of sharing one
// stream. A child depends only on (parent seed, key): never on how many other
// streams exist, the order they were created in, or which thread draws from
// them. So jobs can draw in parallel, and adding one system's stream (a
// visual emitter) can't shift another's draws.

// What a child stream belongs to. `system` names the engine system (see
// rng_stream), `id` the object inside it (an emitter id), `index` a further
// split (a job index, or the generation of a reused id).
struct StreamKey {
    std::uint32_t system = 0;
    std::uint32_t id = 0;
    std::uint32_t index = 0;
};

namespace rng_stream {
// Registered system keys. Append only: changing a value changes every replay.
inline constexpr std::uint32_t particles = 1; // one stream per emitter (id = emitter, index = generation)
inline constexpr std::uint32_t jobs = 2;      // free for parallel jobs that need their own draws
} // namespace rng_stream

// SplitMix64's finalizer: a bijective avalanche mix, so nearby keys give
// unrelated seeds.
constexpr std::uint64_t mix64(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// The child stream of `parent_seed` for `key`. Both the PCG state and its
// stream selector (increment) come from the key, so two children are
// different sequences, not offsets into one.
constexpr Pcg32 derive_stream(std::uint64_t parent_seed, StreamKey key) {
    std::uint64_t h = mix64(parent_seed);
    h = mix64(h ^ key.system);
    h = mix64(h ^ ((std::uint64_t{key.id} << 32) | key.index));
    return Pcg32(h, mix64(h ^ 0x6a09e667f3bcc909ULL));
}

} // namespace cuervo

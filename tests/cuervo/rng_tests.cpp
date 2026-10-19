#include <doctest.h>

#include <array>

#include "cuervo/core/rng.hpp"

using namespace cuervo;

TEST_CASE("Pcg32 matches the reference implementation") {
    // First outputs of pcg32-demo with seed 42, sequence 54.
    Pcg32 rng(42u, 54u);
    constexpr std::array<std::uint32_t, 6> expected{
        0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu, 0xcbed606eu,
    };
    for (std::uint32_t value : expected) {
        CHECK(rng.next_u32() == value);
    }
}

TEST_CASE("Pcg32 is deterministic per seed") {
    Pcg32 a(1234u);
    Pcg32 b(1234u);
    Pcg32 c(1235u);
    bool all_same_as_c = true;
    for (int i = 0; i < 1000; ++i) {
        std::uint32_t va = a.next_u32();
        CHECK(va == b.next_u32());
        all_same_as_c = all_same_as_c && va == c.next_u32();
    }
    CHECK_FALSE(all_same_as_c);
}

TEST_CASE("Pcg32 ranges") {
    Pcg32 rng(7u);
    for (int i = 0; i < 10000; ++i) {
        float f = rng.next_float();
        CHECK(f >= 0.0f);
        CHECK(f < 1.0f);
        std::int32_t n = rng.range(-3, 3);
        CHECK(n >= -3);
        CHECK(n <= 3);
    }
}

TEST_CASE("derive_stream: child streams depend only on the parent seed and the key") {
    const StreamKey key{rng_stream::particles, 3, 0};
    Pcg32 a = derive_stream(99, key);
    Pcg32 b = derive_stream(99, key);
    CHECK(a == b);
    // Creating other streams in between changes nothing (pure function).
    (void)derive_stream(99, {rng_stream::particles, 4, 0});
    CHECK(derive_stream(99, key) == a);
    // Any change of seed or key component gives a different sequence.
    const std::array<Pcg32, 5> others{derive_stream(100, key), derive_stream(99, {rng_stream::jobs, 3, 0}),
                                      derive_stream(99, {rng_stream::particles, 2, 0}),
                                      derive_stream(99, {rng_stream::particles, 3, 1}), Pcg32(99)};
    for (Pcg32 other : others) {
        Pcg32 mine = a;
        int same = 0;
        for (int i = 0; i < 64; ++i) same += mine.next_u32() == other.next_u32() ? 1 : 0;
        CHECK(same < 4);
    }
    // Pinned value: a change here changes every replay (DESIGN.md 4.15).
    Pcg32 pinned = derive_stream(1, {rng_stream::particles, 0, 0});
    CHECK(pinned.next_u32() == 0x47e0b7dbu);
    CHECK(pinned.next_u32() == 0xc588a056u);
}

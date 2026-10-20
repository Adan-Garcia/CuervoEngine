#include <doctest.h>

#include <vector>

#include "cuervo/rhi/frame_fence_ring.hpp"

using cuervo::rhi::FenceRing;

TEST_CASE("FenceRing never keeps more than its limit outstanding") {
    FenceRing<int> ring(2);
    std::vector<int> waited;
    for (int frame = 1; frame <= 100; ++frame) {
        ring.push(frame, [&](int fence) { waited.push_back(fence); });
        CHECK(ring.outstanding() <= 2);
    }
    CHECK(ring.peak() == 2);
    // Fences are waited on oldest first, one per frame once the ring is full.
    REQUIRE(waited.size() == 98);
    for (std::size_t i = 0; i < waited.size(); ++i) {
        CHECK(waited[i] == static_cast<int>(i) + 1);
    }
}

TEST_CASE("FenceRing waits on the fence from `limit` frames ago, not the newest") {
    FenceRing<int> ring(2);
    int last_waited = 0;
    for (int frame = 1; frame <= 10; ++frame) {
        ring.push(frame, [&](int fence) { last_waited = fence; });
        if (frame > 2) {
            CHECK(last_waited == frame - 2);
        }
    }
}

TEST_CASE("FenceRing drain releases everything, a limit of zero acts as one") {
    FenceRing<int> ring(0);
    CHECK(ring.limit() == 1);
    int waits = 0;
    ring.push(1, [&](int) { ++waits; });
    ring.push(2, [&](int) { ++waits; });
    CHECK(waits == 1);

    FenceRing<int> three(3);
    for (int i = 0; i < 3; ++i) {
        three.push(i, [&](int) { FAIL("must not wait below the limit"); });
    }
    std::vector<int> released;
    three.drain([&](int fence) { released.push_back(fence); });
    CHECK(released == std::vector<int>{0, 1, 2});
    CHECK(three.outstanding() == 0);
}

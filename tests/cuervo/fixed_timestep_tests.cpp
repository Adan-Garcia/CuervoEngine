#include <doctest.h>

#include "cuervo/core/fixed_timestep.hpp"

using namespace cuervo;

TEST_CASE("FixedTimestep runs whole ticks and carries the remainder") {
    FixedTimestep ts(100.0); // 10 ms ticks

    auto step = ts.advance(0.025);
    CHECK(step.ticks == 2);
    CHECK(step.alpha == doctest::Approx(0.5));
    CHECK_FALSE(step.clamped);

    step = ts.advance(0.005); // 5 ms left over + 5 ms = one more tick
    CHECK(step.ticks == 1);
    CHECK(step.alpha == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(ts.tick_count() == 3);
}

TEST_CASE("FixedTimestep clamps after a long hitch") {
    FixedTimestep ts(60.0, 4);
    auto step = ts.advance(2.0); // 120 ticks owed
    CHECK(step.ticks == 4);
    CHECK(step.clamped);
    CHECK(step.alpha == 0.0f);

    step = ts.advance(0.0);
    CHECK(step.ticks == 0); // backlog was dropped
}

TEST_CASE("FixedTimestep ignores negative frame times") {
    FixedTimestep ts(60.0);
    CHECK(ts.advance(-1.0).ticks == 0);
}

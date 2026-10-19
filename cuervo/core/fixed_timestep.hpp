#pragma once

#include <cstdint>

namespace cuervo {

// Fixed-timestep accumulator (DESIGN.md 4.1). The simulation always advances in
// whole ticks of `tick_seconds`; rendering uses `alpha` to interpolate between
// the last two simulated states.
class FixedTimestep {
public:
    struct Step {
        int ticks = 0;       // simulation ticks to run this frame
        float alpha = 0.0f;  // [0, 1) interpolation factor for rendering
        bool clamped = false; // true if ticks were dropped (spiral-of-death guard)
    };

    explicit FixedTimestep(double tick_rate_hz = 60.0, int max_ticks_per_frame = 8);

    // Feed the real time elapsed since the previous frame.
    Step advance(double frame_seconds);

    double tick_seconds() const { return tick_seconds_; }
    double tick_rate() const { return 1.0 / tick_seconds_; }
    std::uint64_t tick_count() const { return tick_count_; }

private:
    double tick_seconds_;
    int max_ticks_per_frame_;
    double accumulator_ = 0.0;
    std::uint64_t tick_count_ = 0;
};

} // namespace cuervo

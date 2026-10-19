#include "cuervo/core/fixed_timestep.hpp"

#include <algorithm>

namespace cuervo {

FixedTimestep::FixedTimestep(double tick_rate_hz, int max_ticks_per_frame)
    : tick_seconds_(1.0 / tick_rate_hz), max_ticks_per_frame_(max_ticks_per_frame) {}

FixedTimestep::Step FixedTimestep::advance(double frame_seconds) {
    Step step;
    accumulator_ += std::max(frame_seconds, 0.0);

    while (accumulator_ >= tick_seconds_) {
        if (step.ticks == max_ticks_per_frame_) {
            // Too far behind (debugger pause, long hitch): drop the backlog
            // instead of trying to catch up and falling further behind.
            accumulator_ = 0.0;
            step.clamped = true;
            break;
        }
        accumulator_ -= tick_seconds_;
        ++step.ticks;
    }

    tick_count_ += static_cast<std::uint64_t>(step.ticks);
    step.alpha = static_cast<float>(accumulator_ / tick_seconds_);
    return step;
}

} // namespace cuervo

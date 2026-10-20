#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>

namespace cuervo::rhi {

// Bounds how many submitted frames a backend lets the driver queue (DESIGN.md
// 4.4). GL has no fence per frame of its own: without a bound, a CPU that
// outruns the GPU piles up frames, and every queued frame keeps its orphaned
// stream buffers alive (hundreds of MB at 1M particles). The device pushes one
// fence per presented frame; once more than `limit` are outstanding the oldest
// is handed to `wait` (which blocks until the GPU passed it and releases it).
// Backend independent so the bound is unit-testable without a GPU.
template <class Fence>
class FenceRing {
public:
    explicit FenceRing(std::size_t limit) : limit_(std::max<std::size_t>(limit, 1)) {}

    std::size_t limit() const { return limit_; }
    std::size_t outstanding() const { return fences_.size(); }
    // Most fences ever outstanding at once, after the wait of each push.
    std::size_t peak() const { return peak_; }

    // Records the fence of the frame just submitted, then blocks (through
    // `wait`) on the oldest ones until at most `limit` frames are in flight.
    template <class Wait>
    void push(Fence fence, Wait&& wait) {
        fences_.push_back(std::move(fence));
        while (fences_.size() > limit_) {
            Fence oldest = std::move(fences_.front());
            fences_.pop_front();
            wait(std::move(oldest));
        }
        peak_ = std::max(peak_, fences_.size());
    }

    // Hands every outstanding fence to `release` (device teardown).
    template <class Release>
    void drain(Release&& release) {
        while (!fences_.empty()) {
            Fence oldest = std::move(fences_.front());
            fences_.pop_front();
            release(std::move(oldest));
        }
    }

private:
    std::size_t limit_;
    std::size_t peak_ = 0;
    std::deque<Fence> fences_;
};

} // namespace cuervo::rhi

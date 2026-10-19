#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "cuervo/audio/mixer.hpp"

// Single-producer / single-consumer ring of mixer commands: the main thread
// pushes one simulation tick's triggers as a batch, the audio thread drains
// whole batches at the start of a callback. Lock-free, so the audio thread
// never waits on the simulation.
namespace cuervo::audio {

class CommandQueue {
public:
    explicit CommandQueue(std::uint32_t capacity = 4096) : ring_(capacity) {}

    // Producer: appends a batch atomically (all or nothing). False if full.
    bool push_batch(const Command* commands, std::size_t count) {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail + count > ring_.size()) return false;
        for (std::size_t i = 0; i < count; ++i) ring_[(head + i) % ring_.size()] = commands[i];
        head_.store(head + count, std::memory_order_release);
        return true;
    }

    // Consumer: calls f(command) for everything published so far.
    template<typename F>
    void drain(F&& f) {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        for (; tail != head; ++tail) f(ring_[tail % ring_.size()]);
        tail_.store(tail, std::memory_order_release);
    }

private:
    std::vector<Command> ring_;
    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> tail_{0};
};

} // namespace cuervo::audio

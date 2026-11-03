#include "cuervo/net/rollback.hpp"

#include <algorithm>

namespace cuervo::net {

RollbackSession::RollbackSession(Transport& transport, RollbackHost& host, std::uint32_t player_count,
                                 std::uint32_t max_rollback, std::uint32_t input_delay)
    : lockstep_(transport, player_count, input_delay, std::min(max_rollback, max_window)), host_(host),
      players_(player_count), window_(std::min(max_rollback, max_window)) {}

// Real frame when we hold it, otherwise the player's most recent earlier frame
// (repeat last). Predictions only differ between peers, never the confirmed
// outcome: a wrong guess is replaced when the real frame lands.
std::vector<InputFrame> RollbackSession::inputs_for(std::uint32_t tick) const {
    std::vector<InputFrame> out(players_);
    for (std::uint32_t p = 0; p < players_; ++p) {
        for (std::uint32_t t = tick; t >= 1; --t) {
            if (const InputFrame* f = lockstep_.frame(p, t)) {
                out[p] = *f;
                break;
            }
            if (tick - t > 2 * max_window + 256) break; // frames this old are pruned anyway
        }
    }
    return out;
}

void RollbackSession::simulate(std::uint32_t tick) {
    std::vector<InputFrame> in = inputs_for(tick);
    host_.step(in);
    hashes_[tick] = host_.hash();
    host_.save(tick % (window_ + 1), tick);
    used_[tick] = std::move(in);
}

bool RollbackSession::update(const InputFrame& local) {
    if (!saved_zero_) {
        host_.save(0, 0);
        saved_zero_ = true;
    }
    lockstep_.pump();

    // Peek at ticks whose real frames are all in; the first one that ran with a
    // different guess is where we rewind to.
    std::uint32_t first_bad = 0;
    for (std::uint32_t t = lockstep_.sim_tick() + 1; t <= sim_tick_; ++t) {
        bool known = true;
        bool same = true;
        const auto& used = used_.at(t);
        for (std::uint32_t p = 0; p < players_ && known; ++p) {
            const InputFrame* f = lockstep_.frame(p, t);
            if (!f) known = false;
            else if (!(*f == used[p])) same = false;
        }
        if (known && !same) {
            first_bad = t;
            break;
        }
    }
    if (first_bad) {
        std::uint32_t back = first_bad - 1;
        host_.restore(back % (window_ + 1), back);
        ++rollbacks_;
        deepest_ = std::max(deepest_, sim_tick_ - back);
        for (std::uint32_t t = first_bad; t <= sim_tick_; ++t) {
            simulate(t);
            ++resimulated_;
        }
    }

    // Confirm ticks we have simulated and now know exactly: lockstep compares
    // the hash exchange on these.
    std::vector<InputFrame> frames;
    while (lockstep_.sim_tick() < sim_tick_) {
        std::uint32_t t = lockstep_.sim_tick() + 1;
        if (!lockstep_.advance(frames)) break;
        lockstep_.report_hash(hashes_.at(t));
    }
    std::uint32_t confirmed = lockstep_.sim_tick();
    std::erase_if(used_, [&](auto& kv) { return kv.first <= confirmed; });
    std::erase_if(hashes_, [&](auto& kv) { return kv.first + hash_history < confirmed; });

    if (sim_tick_ - confirmed >= window_ || !lockstep_.can_submit()) return false;
    lockstep_.submit_local(local);
    simulate(++sim_tick_);
    return true;
}

} // namespace cuervo::net

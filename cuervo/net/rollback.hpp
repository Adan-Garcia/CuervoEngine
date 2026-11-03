#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "cuervo/net/lockstep.hpp"

// Rollback netcode (DESIGN.md 4.1, 4.19; 11.1 slice 3). The simulation runs
// ahead on predicted remote input (repeat the player's last known frame); when
// the real frame arrives and differs, the session restores the snapshot taken
// before that tick and re-simulates up to the present. Input exchange, acks and
// per-tick hash comparison are LockstepSession's: this class only drives it with
// the *confirmed* ticks (all players' real frames present), so desync detection
// compares hashes of ticks that were simulated from real input alone, and a
// session replays offline from the confirmed frames exactly as lockstep does.
//
// The session never touches sockets or the sim directly; the sim is reached
// through RollbackHost, which the game wires to ScriptHost::save/restore/
// snapshot_hash. Prediction is bounded by `max_rollback` ticks: at the limit
// update() stalls (returns false) until confirmations catch up.
namespace cuervo::net {

class RollbackHost {
public:
    virtual ~RollbackHost() = default;
    // One fixed tick from one frame per player (index = player).
    virtual void step(std::span<const InputFrame> frames) = 0;
    // Keep the state after `tick` ticks in `slot`; restore() returns to it.
    // The session uses slots 0..max_rollback and never needs more than one
    // live snapshot per slot.
    virtual void save(std::uint32_t slot, std::uint32_t tick) = 0;
    virtual void restore(std::uint32_t slot, std::uint32_t tick) = 0;
    // ScriptHost::snapshot_hash() of the current state (full-state hash).
    virtual std::uint64_t hash() = 0;
};

class RollbackSession {
public:
    static constexpr std::uint32_t max_window = 16; // bounded by LockstepSession's frames per packet

    RollbackSession(Transport& transport, RollbackHost& host, std::uint32_t player_count,
                    std::uint32_t max_rollback, std::uint32_t input_delay = 0);

    // One host step: receive, queue `local`, roll back if a prediction was
    // wrong, confirm ticks, then simulate one new tick. False when stalled at
    // the prediction limit (nothing was simulated this call, local input
    // was not consumed).
    bool update(const InputFrame& local);

    std::uint32_t predicted_tick() const { return sim_tick_; }
    std::uint32_t confirmed_tick() const { return lockstep_.sim_tick(); }
    const std::optional<DesyncReport>& desync() const { return lockstep_.desync(); }
    const InputFrame* frame(std::uint32_t player, std::uint32_t tick) const { return lockstep_.frame(player, tick); }

    // State hash after `tick` ticks as simulated here (kept for the last
    // `hash_history` confirmed ticks and the predicted ones); only confirmed
    // ticks' hashes are final.
    std::optional<std::uint64_t> hash_at(std::uint32_t tick) const {
        auto it = hashes_.find(tick);
        return it == hashes_.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
    }
    static constexpr std::uint32_t hash_history = 64;

    std::uint64_t rollbacks() const { return rollbacks_; }
    std::uint64_t resimulated_ticks() const { return resimulated_; }
    std::uint32_t deepest_rollback() const { return deepest_; }

private:
    void simulate(std::uint32_t tick);
    std::vector<InputFrame> inputs_for(std::uint32_t tick) const;

    LockstepSession lockstep_;
    RollbackHost& host_;
    std::uint32_t players_;
    std::uint32_t window_;
    std::uint32_t sim_tick_ = 0;
    bool saved_zero_ = false;
    std::map<std::uint32_t, std::vector<InputFrame>> used_; // inputs each simulated tick ran with
    std::map<std::uint32_t, std::uint64_t> hashes_;         // state hash after each simulated tick
    std::uint64_t rollbacks_ = 0;
    std::uint64_t resimulated_ = 0;
    std::uint32_t deepest_ = 0;
};

} // namespace cuervo::net

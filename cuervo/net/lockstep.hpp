#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "cuervo/input/input_map.hpp"
#include "cuervo/net/transport.hpp"

// Lockstep input exchange (DESIGN.md 4.1, 4.19). Every peer simulates the
// same ticks from the same per-player InputFrames (the replay frame format),
// so a networked session is replayable offline from the frames the session
// delivered. The session never touches the sim: the host asks `advance()` for
// the next tick's frames, steps its own sim, then reports the snapshot hash.
//
// Inputs are sent redundantly every pump (all frames the peer has not acked)
// together with an ack of the peer's contiguous frames, so loss and reorder
// need no reliable channel. Ticks are 1-based like Replay::frames[i] = tick
// i + 1. A fixed input delay shifts local input to tick (sim tick + delay);
// the first `delay` ticks run with empty input on every peer.
namespace cuervo::net {

struct DesyncReport {
    std::uint32_t tick = 0; // first tick whose hashes differ
    std::uint64_t local_hash = 0;
    std::uint64_t remote_hash = 0;
    PeerId peer = 0;
};

class LockstepSession {
public:
    // Players are numbered 0..player_count-1 and a player's PeerId equals
    // its number. `input_delay` is in ticks. `extra_lead` lets local input run
    // that many ticks further ahead of the confirmed tick (RollbackSession
    // predicts through it; plain lockstep leaves it 0).
    LockstepSession(Transport& transport, std::uint32_t player_count, std::uint32_t input_delay,
                    std::uint32_t extra_lead = 0);

    PeerId local_player() const { return local_; }

    // Receives packets, then sends our unacked inputs and recent hashes.
    // Call once per host step, before can_submit/advance.
    void pump();

    // True while local input for the next unsubmitted tick may be queued
    // (bounded to `input_delay` ticks ahead of the sim; this is the stall).
    bool can_submit() const { return next_submit_ <= sim_tick_ + 1 + delay_ + lead_; }
    void submit_local(const InputFrame& frame);

    // If every player's frame for the next tick is present, fills `frames`
    // (index = player) and returns true; the caller simulates that tick, then
    // calls report_hash(). False means stall: waiting on a peer.
    bool advance(std::vector<InputFrame>& frames);
    // The sim's snapshot_hash after the tick advance() last returned.
    void report_hash(std::uint64_t hash);

    std::uint32_t sim_tick() const { return sim_tick_; }
    // First divergence seen (sticky); every peer that compares reports it.
    const std::optional<DesyncReport>& desync() const { return desync_; }

    // The delivered frames of `player` for `tick`, if still retained (for
    // recording a replay that reproduces the networked session offline).
    const InputFrame* frame(std::uint32_t player, std::uint32_t tick) const;

    static constexpr std::uint32_t hash_window = 16; // hashes resent per packet
    static constexpr std::uint32_t max_frames_per_packet = 24;
    static constexpr std::uint32_t resend_pumps = 8; // pumps between identical packets (loss recovery)

private:
    struct PeerState {
        std::uint32_t acked_ours = 0; // peer has all our frames up to this tick
        std::uint32_t have_theirs = 0; // we have all of the peer's frames up to this
        // What the last packet to this peer said, and how many pumps ago: an unchanged state is only
        // resent every `resend_pumps` (a stalled loop would otherwise flood the link, and a relay).
        std::uint32_t sent_ack = ~0u, sent_next = ~0u, sent_tick = ~0u, since_sent = 0;
    };
    bool decode(PeerId from, std::span<const std::byte> data);
    void check_hash(PeerId from, std::uint32_t tick, std::uint64_t hash);
    void send_to(PeerId peer);

    Transport& transport_;
    PeerId local_;
    std::uint32_t players_;
    std::uint32_t delay_;
    std::uint32_t lead_;
    std::uint32_t sim_tick_ = 0;
    std::uint32_t next_submit_ = 1;
    std::vector<std::map<std::uint32_t, InputFrame>> frames_; // per player
    std::vector<PeerState> peers_;
    std::map<std::uint32_t, std::uint64_t> local_hashes_;
    std::map<std::pair<std::uint32_t, PeerId>, std::uint64_t> remote_hashes_; // (tick, peer)
    std::optional<DesyncReport> desync_;
};

} // namespace cuervo::net

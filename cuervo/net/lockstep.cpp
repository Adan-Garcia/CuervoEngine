#include "cuervo/net/lockstep.hpp"

#include <algorithm>

namespace cuervo::net {

namespace {

constexpr std::uint16_t magic = 0x4c43; // "CL"
constexpr std::uint32_t max_lead = 256; // frames accepted ahead of what we hold (decoder hardening)
constexpr std::uint32_t keep_ticks = 256;

class Writer {
public:
    void u8(std::uint8_t v) { out.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) { le(v, 2); }
    void u32(std::uint32_t v) { le(v, 4); }
    void u64(std::uint64_t v) { le(v, 8); }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    std::vector<std::byte> out;

private:
    void le(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
    }
};

// Every read is bounds-checked; a short packet sets `ok` false and yields 0.
class Reader {
public:
    explicit Reader(std::span<const std::byte> d) : d_(d) {}
    std::uint8_t u8() {
        if (pos_ >= d_.size()) {
            ok = false;
            return 0;
        }
        return static_cast<std::uint8_t>(d_[pos_++]);
    }
    std::uint16_t u16() { return static_cast<std::uint16_t>(le(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(le(4)); }
    std::uint64_t u64() { return le(8); }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    bool ok = true;

private:
    std::uint64_t le(int n) {
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        return v;
    }
    std::span<const std::byte> d_;
    std::size_t pos_ = 0;
};

void put_frame(Writer& w, const InputFrame& f) {
    w.u64(f.held);
    for (auto a : f.axes) w.i16(a);
    for (auto p : f.pointer) w.i16(p);
    w.u32(f.text);
}

InputFrame get_frame(Reader& r) {
    InputFrame f;
    f.held = r.u64();
    for (auto& a : f.axes) a = r.i16();
    for (auto& p : f.pointer) p = r.i16();
    f.text = r.u32();
    return f;
}

} // namespace

LockstepSession::LockstepSession(Transport& transport, std::uint32_t player_count, std::uint32_t input_delay,
                                 std::uint32_t extra_lead)
    : transport_(transport), local_(transport.local_id()), players_(player_count), delay_(input_delay),
      lead_(extra_lead),
      next_submit_(input_delay + 1), frames_(player_count), peers_(player_count) {
    // Ticks 1..delay have no input from anyone yet: every peer agrees on
    // empty frames, so the pipeline starts full instead of stalling.
    for (auto& per_player : frames_)
        for (std::uint32_t t = 1; t <= delay_; ++t) per_player[t] = InputFrame{};
    for (auto& p : peers_) p.acked_ours = p.have_theirs = delay_;
}

const InputFrame* LockstepSession::frame(std::uint32_t player, std::uint32_t tick) const {
    if (player >= players_) return nullptr;
    auto it = frames_[player].find(tick);
    return it == frames_[player].end() ? nullptr : &it->second;
}

void LockstepSession::submit_local(const InputFrame& frame) {
    if (!can_submit()) return;
    frames_[local_][next_submit_++] = frame;
}

bool LockstepSession::decode(PeerId from, std::span<const std::byte> data) {
    if (from >= players_ || from == local_) return false;
    Reader r(data);
    if (r.u16() != magic) return false;
    std::uint32_t ack = r.u32();
    std::uint32_t first = r.u32();
    std::uint8_t count = r.u8();
    if (!r.ok || count > max_frames_per_packet) return false;
    InputFrame in[max_frames_per_packet];
    for (std::uint8_t i = 0; i < count; ++i) in[i] = get_frame(r);
    std::uint8_t hcount = r.u8();
    if (!r.ok || hcount > hash_window) return false;
    std::uint32_t htick[hash_window];
    std::uint64_t hval[hash_window];
    for (std::uint8_t i = 0; i < hcount; ++i) {
        htick[i] = r.u32();
        hval[i] = r.u64();
    }
    if (!r.ok) return false; // nothing is applied from a truncated packet

    PeerState& ps = peers_[from];
    ps.acked_ours = std::max(ps.acked_ours, std::min(ack, next_submit_ - 1));
    auto& theirs = frames_[from];
    for (std::uint8_t i = 0; i < count; ++i) {
        std::uint32_t t = first + i;
        if (t <= ps.have_theirs || t > ps.have_theirs + max_lead) continue;
        theirs.emplace(t, in[i]); // a duplicate keeps the first copy
    }
    while (theirs.count(ps.have_theirs + 1)) ++ps.have_theirs;
    for (std::uint8_t i = 0; i < hcount; ++i) check_hash(from, htick[i], hval[i]);
    return true;
}

void LockstepSession::check_hash(PeerId from, std::uint32_t tick, std::uint64_t hash) {
    auto it = local_hashes_.find(tick);
    if (it == local_hashes_.end()) {
        if (tick > sim_tick_ && tick <= sim_tick_ + max_lead) remote_hashes_.emplace(std::pair{tick, from}, hash);
        return;
    }
    if (it->second != hash && (!desync_ || tick < desync_->tick)) desync_ = DesyncReport{tick, it->second, hash, from};
}

void LockstepSession::send_to(PeerId peer) {
    PeerState& ps = peers_[peer];
    if (ps.sent_ack == ps.have_theirs && ps.sent_next == next_submit_ && ps.sent_tick == sim_tick_ &&
        ++ps.since_sent < resend_pumps) {
        return; // nothing new to say
    }
    ps.sent_ack = ps.have_theirs;
    ps.sent_next = next_submit_;
    ps.sent_tick = sim_tick_;
    ps.since_sent = 0;
    Writer w;
    w.u16(magic);
    w.u32(ps.have_theirs);
    std::uint32_t first = ps.acked_ours + 1;
    std::uint32_t count = next_submit_ > first ? std::min(next_submit_ - first, max_frames_per_packet) : 0;
    w.u32(first);
    w.u8(static_cast<std::uint8_t>(count));
    for (std::uint32_t i = 0; i < count; ++i) put_frame(w, frames_[local_].at(first + i));
    std::size_t n = std::min<std::size_t>(local_hashes_.size(), hash_window);
    w.u8(static_cast<std::uint8_t>(n));
    auto it = local_hashes_.end();
    std::advance(it, -static_cast<std::ptrdiff_t>(n));
    for (; it != local_hashes_.end(); ++it) {
        w.u32(it->first);
        w.u64(it->second);
    }
    transport_.send(peer, w.out);
}

void LockstepSession::pump() {
    Packet p;
    while (transport_.recv(p)) decode(p.from, p.data);
    for (PeerId peer = 0; peer < players_; ++peer)
        if (peer != local_) send_to(peer);
}

bool LockstepSession::advance(std::vector<InputFrame>& out) {
    std::uint32_t t = sim_tick_ + 1;
    for (std::uint32_t p = 0; p < players_; ++p)
        if (!frames_[p].count(t)) return false;
    out.resize(players_);
    for (std::uint32_t p = 0; p < players_; ++p) out[p] = frames_[p].at(t);
    sim_tick_ = t;
    return true;
}

void LockstepSession::report_hash(std::uint64_t hash) {
    local_hashes_[sim_tick_] = hash;
    for (auto it = remote_hashes_.lower_bound({sim_tick_, 0}); it != remote_hashes_.end() && it->first.first == sim_tick_;) {
        check_hash(it->first.second, sim_tick_, it->second);
        it = remote_hashes_.erase(it);
    }
    if (sim_tick_ > keep_ticks) {
        std::uint32_t cut = sim_tick_ - keep_ticks;
        std::erase_if(local_hashes_, [&](auto& kv) { return kv.first < cut; });
        // Our own frames stay until every peer acked them; others' go once simulated.
        std::uint32_t min_ack = next_submit_;
        for (PeerId q = 0; q < players_; ++q)
            if (q != local_) min_ack = std::min(min_ack, peers_[q].acked_ours);
        for (std::uint32_t pl = 0; pl < players_; ++pl) {
            std::uint32_t lim = pl == local_ ? std::min(cut, min_ack) : cut;
            std::erase_if(frames_[pl], [&](auto& kv) { return kv.first < lim; });
        }
    }
}

} // namespace cuervo::net

#include "cuervo/net/loopback.hpp"

namespace cuervo::net {

LoopbackEndpoint& LoopbackNetwork::add_endpoint() {
    auto id = static_cast<PeerId>(endpoints_.size());
    endpoints_.push_back(std::unique_ptr<LoopbackEndpoint>(new LoopbackEndpoint(*this, id)));
    return *endpoints_.back();
}

bool LoopbackNetwork::route(PeerId from, PeerId to, std::span<const std::byte> data) {
    if (to >= endpoints_.size() || data.size() > Transport::max_packet_size) return false;
    ++sent_;
    if (rng_.next_below(1000) < cfg_.loss_permille) {
        ++dropped_;
        return true;
    }
    int copies = rng_.next_below(1000) < cfg_.dup_permille ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
        std::uint64_t at = now_ + cfg_.latency_min + (cfg_.jitter ? rng_.next_below(cfg_.jitter + 1) : 0);
        Packet p;
        p.from = from;
        p.data.assign(data.begin(), data.end());
        endpoints_[to]->inbox_.emplace(std::pair{at, seq_++}, std::move(p));
    }
    return true;
}

bool LoopbackEndpoint::send(PeerId to, std::span<const std::byte> data) {
    return net_->route(id_, to, data);
}

bool LoopbackEndpoint::recv(Packet& out) {
    if (inbox_.empty() || inbox_.begin()->first.first > net_->now_) return false;
    out = std::move(inbox_.begin()->second);
    inbox_.erase(inbox_.begin());
    return true;
}

} // namespace cuervo::net

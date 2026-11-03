#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "cuervo/core/rng.hpp"
#include "cuervo/net/transport.hpp"

namespace cuervo::net {

// Bad-network model for tests. Time is a virtual step counter the caller
// advances, so a run is a pure function of the seed and the call sequence.
struct LinkConfig {
    std::uint32_t latency_min = 0;   // steps before delivery
    std::uint32_t jitter = 0;        // extra 0..jitter steps; causes reordering
    std::uint32_t loss_permille = 0; // chance a packet is dropped
    std::uint32_t dup_permille = 0;  // chance a packet is delivered twice
};

class LoopbackNetwork;

class LoopbackEndpoint final : public Transport {
public:
    PeerId local_id() const override { return id_; }
    bool send(PeerId to, std::span<const std::byte> data) override;
    bool recv(Packet& out) override;

private:
    friend class LoopbackNetwork;
    LoopbackEndpoint(LoopbackNetwork& net, PeerId id) : net_(&net), id_(id) {}
    LoopbackNetwork* net_;
    PeerId id_;
    // Keyed (deliver_at, sequence): ties resolve in send order.
    std::map<std::pair<std::uint64_t, std::uint64_t>, Packet> inbox_;
};

class LoopbackNetwork {
public:
    explicit LoopbackNetwork(std::uint64_t seed, LinkConfig cfg = {}) : rng_(seed), cfg_(cfg) {}

    // Endpoints get ids 0, 1, 2... in creation order. Owned by the network.
    LoopbackEndpoint& add_endpoint();
    void set_config(LinkConfig cfg) { cfg_ = cfg; }
    // Advances virtual time by one step.
    void step() { ++now_; }
    std::uint64_t now() const { return now_; }

    std::uint64_t sent() const { return sent_; }
    std::uint64_t dropped() const { return dropped_; }

private:
    friend class LoopbackEndpoint;
    bool route(PeerId from, PeerId to, std::span<const std::byte> data);

    Pcg32 rng_;
    LinkConfig cfg_;
    std::uint64_t now_ = 0;
    std::uint64_t seq_ = 0;
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
    std::vector<std::unique_ptr<LoopbackEndpoint>> endpoints_;
};

} // namespace cuervo::net

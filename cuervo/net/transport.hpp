#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Datagram transport abstraction (DESIGN.md 4.19, networking). Packets are
// unreliable, unordered and size-limited; lockstep and rollback build their
// own redundancy on top (inputs resent every tick plus an ack), so no general
// reliable channel is needed. Implementations: the in-process LoopbackNetwork
// (tests), UDP on desktop and WebSocket via a relay on web (later slices).
// Networking never touches the simulation directly: sessions hand it recorded
// input frames only.
namespace cuervo::net {

using PeerId = std::uint32_t;

struct Packet {
    PeerId from = 0;
    std::vector<std::byte> data;
};

class Transport {
public:
    // Largest datagram send() accepts (a safe path MTU for UDP and relays).
    static constexpr std::size_t max_packet_size = 1200;

    virtual ~Transport() = default;
    virtual PeerId local_id() const = 0;
    // Best effort; false if the packet was refused (too large, unknown peer).
    // A true return does not mean it will arrive.
    virtual bool send(PeerId to, std::span<const std::byte> data) = 0;
    // Pops the next delivered packet, or false when none is ready.
    virtual bool recv(Packet& out) = 0;
};

} // namespace cuervo::net

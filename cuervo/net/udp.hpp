#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cuervo/net/transport.hpp"

// UDP Transport for desktop (DESIGN.md 4.19). Non-blocking IPv4 datagram socket (POSIX or
// Winsock), peers addressed as "host:port" strings. It lives outside the simulation: sessions
// pull packets with recv() and feed the sim recorded input frames only.
//
// PeerIds: 0 is this transport; add_peer() hands out 1, 2, ... A datagram from an address not
// added yet registers a new peer (so a host can accept joins), up to max_peers; further unknown
// senders are dropped, which bounds memory against spoofed traffic.
namespace cuervo::net {

class UdpTransport final : public Transport {
public:
    static constexpr std::size_t max_peers = 64;

    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Binds `address` ("0.0.0.0:7777", "127.0.0.1:0" for an ephemeral port). Null with `error` set on failure.
    static std::unique_ptr<UdpTransport> open(const std::string& address, std::string& error);

    // The address actually bound, with the real port when 0 was asked for.
    const std::string& local_address() const { return local_address_; }
    std::uint16_t local_port() const { return port_; }

    // Registers a remote ("host:port", numeric or resolvable IPv4). 0 when it cannot be resolved or the table is full.
    PeerId add_peer(const std::string& address);
    std::string peer_address(PeerId id) const;

    PeerId local_id() const override { return 0; }
    bool send(PeerId to, std::span<const std::byte> data) override;
    bool recv(Packet& out) override;

private:
    UdpTransport() = default;
    PeerId peer_for(std::uint32_t ip, std::uint16_t port, bool create);

    std::intptr_t socket_ = -1;
    std::uint16_t port_ = 0;
    std::string local_address_;
    struct Remote {
        std::uint32_t ip; // network order
        std::uint16_t port;
    };
    std::vector<Remote> peers_; // peers_[id - 1]
    std::map<std::pair<std::uint32_t, std::uint16_t>, PeerId> by_addr_;
};

} // namespace cuervo::net

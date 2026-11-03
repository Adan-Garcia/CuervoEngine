#include "cuervo/net/udp.hpp"

#include <charconv>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cuervo::net {

namespace {

#if defined(_WIN32)
using sock_t = SOCKET;
constexpr sock_t bad_socket = INVALID_SOCKET;
void close_socket(sock_t s) { closesocket(s); }
bool set_nonblocking(sock_t s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
void ensure_winsock() {
    static const bool started = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)started;
}
using addr_len_t = int;
#else
using sock_t = int;
constexpr sock_t bad_socket = -1;
void close_socket(sock_t s) { ::close(s); }
bool set_nonblocking(sock_t s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
void ensure_winsock() {}
using addr_len_t = socklen_t;
#endif

// "host:port" -> network-order IPv4 and host-order port.
bool parse_address(const std::string& text, std::uint32_t& ip, std::uint16_t& port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
    unsigned p = 0;
    const char* first = text.data() + colon + 1;
    const char* last = text.data() + text.size();
    const auto r = std::from_chars(first, last, p);
    if (r.ec != std::errc{} || r.ptr != last || p > 65535) return false;
    port = static_cast<std::uint16_t>(p);
    const std::string host = text.substr(0, colon);
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) {
        ip = a.s_addr;
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) return false;
    ip = reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

} // namespace

UdpTransport::~UdpTransport() {
    if (socket_ != -1) close_socket(static_cast<sock_t>(socket_));
}

std::unique_ptr<UdpTransport> UdpTransport::open(const std::string& address, std::string& error) {
    ensure_winsock();
    std::uint32_t ip = 0;
    std::uint16_t port = 0;
    if (!parse_address(address, ip, port)) {
        error = "bad address '" + address + "' (expected host:port)";
        return nullptr;
    }
    const sock_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == bad_socket) {
        error = "cannot create a UDP socket";
        return nullptr;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip;
    sa.sin_port = htons(port);
    if (!set_nonblocking(s) || ::bind(s, reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0) {
        close_socket(s);
        error = "cannot bind '" + address + "'";
        return nullptr;
    }
    addr_len_t len = sizeof sa;
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
        close_socket(s);
        error = "cannot read the bound address";
        return nullptr;
    }
    std::unique_ptr<UdpTransport> t(new UdpTransport());
    t->socket_ = static_cast<std::intptr_t>(s);
    t->port_ = ntohs(sa.sin_port);
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof buf);
    t->local_address_ = std::string(buf) + ":" + std::to_string(t->port_);
    return t;
}

PeerId UdpTransport::peer_for(std::uint32_t ip, std::uint16_t port, bool create) {
    const auto key = std::make_pair(ip, port);
    if (const auto it = by_addr_.find(key); it != by_addr_.end()) return it->second;
    if (!create || peers_.size() >= max_peers) return 0;
    peers_.push_back({ip, port});
    const PeerId id = static_cast<PeerId>(peers_.size());
    by_addr_.emplace(key, id);
    return id;
}

PeerId UdpTransport::add_peer(const std::string& address) {
    std::uint32_t ip = 0;
    std::uint16_t port = 0;
    if (!parse_address(address, ip, port)) return 0;
    return peer_for(ip, port, true);
}

std::string UdpTransport::peer_address(PeerId id) const {
    if (id == 0 || id > peers_.size()) return {};
    in_addr a{};
    a.s_addr = peers_[id - 1].ip;
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return std::string(buf) + ":" + std::to_string(peers_[id - 1].port);
}

bool UdpTransport::send(PeerId to, std::span<const std::byte> data) {
    if (to == 0 || to > peers_.size() || data.size() > max_packet_size) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = peers_[to - 1].ip;
    sa.sin_port = htons(peers_[to - 1].port);
    const auto n = ::sendto(static_cast<sock_t>(socket_), reinterpret_cast<const char*>(data.data()),
                            static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
    return n >= 0 && static_cast<std::size_t>(n) == data.size();
}

bool UdpTransport::recv(Packet& out) {
    // One extra byte so an oversized datagram is seen as such and dropped, not truncated into a valid one.
    std::byte buf[max_packet_size + 1];
    for (;;) {
        sockaddr_in sa{};
        addr_len_t len = sizeof sa;
        const auto n = ::recvfrom(static_cast<sock_t>(socket_), reinterpret_cast<char*>(buf), sizeof buf, 0,
                                  reinterpret_cast<sockaddr*>(&sa), &len);
        if (n < 0) return false; // would block (or a transient error): nothing ready
        if (static_cast<std::size_t>(n) > max_packet_size) continue;
        const PeerId from = peer_for(sa.sin_addr.s_addr, ntohs(sa.sin_port), true);
        if (from == 0) continue; // table full: unknown sender
        out.from = from;
        out.data.assign(buf, buf + n);
        return true;
    }
}

} // namespace cuervo::net

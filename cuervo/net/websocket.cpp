#include "cuervo/net/websocket.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <random>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten/websocket.h>
#else
#include "cuervo/net/tcp.hpp"
#endif

namespace cuervo::net {

#if defined(__EMSCRIPTEN__)

namespace {

class WebTransport final : public WebSocketTransport {
public:
    ~WebTransport() override {
        if (socket_ > 0) {
            emscripten_websocket_close(socket_, 1000, "bye");
            emscripten_websocket_delete(socket_);
        }
    }
    bool start(const std::string& url) {
        if (!emscripten_websocket_is_supported()) return false;
        EmscriptenWebSocketCreateAttributes attr;
        emscripten_websocket_init_create_attributes(&attr);
        attr.url = url.c_str();
        socket_ = emscripten_websocket_new(&attr);
        if (socket_ <= 0) return false;
        emscripten_websocket_set_onmessage_callback(socket_, this, &WebTransport::on_message);
        emscripten_websocket_set_onclose_callback(socket_, this, &WebTransport::on_close);
        emscripten_websocket_set_onerror_callback(socket_, this, &WebTransport::on_error);
        return true;
    }
    PeerId local_id() const override { return id_; }
    bool ready() const override { return have_id_ && !closed_; }
    bool closed() const override { return closed_; }
    bool send(PeerId to, std::span<const std::byte> data) override {
        if (!ready() || data.size() > max_packet_size) return false;
        std::array<std::byte, max_packet_size + 4> buf{};
        std::memcpy(buf.data(), &to, 4); // wasm is little endian
        if (!data.empty()) std::memcpy(buf.data() + 4, data.data(), data.size());
        return emscripten_websocket_send_binary(socket_, buf.data(), static_cast<std::uint32_t>(data.size() + 4)) >= 0;
    }
    bool recv(Packet& out) override {
        if (inbox_.empty()) return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }

private:
    static EM_BOOL on_message(int, const EmscriptenWebSocketMessageEvent* e, void* user) {
        auto* self = static_cast<WebTransport*>(user);
        if (e->isText || e->numBytes < 4 || e->numBytes > max_packet_size + 4) return EM_TRUE;
        std::uint32_t head = 0;
        std::memcpy(&head, e->data, 4);
        const auto* p = reinterpret_cast<const std::byte*>(e->data);
        if (!self->have_id_) {
            self->id_ = head;
            self->have_id_ = true;
        } else if (self->inbox_.size() < 4096) {
            self->inbox_.push_back({head, std::vector<std::byte>(p + 4, p + e->numBytes)});
        }
        return EM_TRUE;
    }
    static EM_BOOL on_close(int, const EmscriptenWebSocketCloseEvent*, void* user) {
        static_cast<WebTransport*>(user)->closed_ = true;
        return EM_TRUE;
    }
    static EM_BOOL on_error(int, const EmscriptenWebSocketErrorEvent*, void* user) {
        static_cast<WebTransport*>(user)->closed_ = true;
        return EM_TRUE;
    }
    EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
    PeerId id_ = 0;
    bool have_id_ = false;
    bool closed_ = false;
    std::deque<Packet> inbox_;
};

} // namespace

std::unique_ptr<WebSocketTransport> open_websocket_transport(const WebSocketOptions& options, std::string& error) {
    if (options.url.rfind("ws://", 0) != 0 && options.url.rfind("wss://", 0) != 0) {
        error = "bad relay URL '" + options.url + "' (expected ws:// or wss://)"; // the browser does TLS itself
        return nullptr;
    }
    std::string full = options.url;
    if (full.back() != '/') full += '/';
    full += options.room;
    auto t = std::make_unique<WebTransport>();
    if (!t->start(full)) {
        error = "the browser WebSocket is unavailable or the URL was rejected";
        return nullptr;
    }
    return t;
}

#else // desktop

namespace {

// ---- SHA-1 + base64, only to verify Sec-WebSocket-Accept (RFC 6455 4.2.2) ----
std::array<std::uint8_t, 20> sha1(const std::string& msg) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string m = msg;
    const std::uint64_t bits = static_cast<std::uint64_t>(msg.size()) * 8;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* b = reinterpret_cast<const std::uint8_t*>(m.data() + off + i * 4);
            w[i] = (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i)
        for (std::size_t j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - j * 8));
    return out;
}

std::string base64(const std::uint8_t* d, std::size_t n) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (std::uint32_t(d[i]) << 16) | (i + 1 < n ? std::uint32_t(d[i + 1]) << 8 : 0) |
                                (i + 2 < n ? d[i + 2] : 0);
        s.push_back(tbl[(v >> 18) & 63]);
        s.push_back(tbl[(v >> 12) & 63]);
        s.push_back(i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        s.push_back(i + 2 < n ? tbl[v & 63] : '=');
    }
    return s;
}

class TcpTransport final : public WebSocketTransport {
public:
    static constexpr std::size_t max_message = max_packet_size + 4;

    ~TcpTransport() override {
        if (conn_ && have_id_ && !closed_) send_frame(0x8, nullptr, 0); // close
    }

    bool connect_to(const ParsedUrl& u, const WebSocketOptions& options, std::string& error) {
        TcpOptions tcp;
        tcp.tls = u.scheme == "wss";
        tcp.ca_file = options.ca_file;
        tcp.insecure = options.insecure;
        conn_ = tcp_connect(u.host, u.port, tcp, error);
        if (!conn_) return false;

        std::random_device rd; // handshake nonce only; networking is outside the sim
        std::uint8_t nonce[16];
        for (auto& b : nonce) b = static_cast<std::uint8_t>(rd());
        const std::string key = base64(nonce, sizeof nonce);
        const std::string path = u.path.back() == '/' ? u.path + options.room : u.path + "/" + options.room;
        const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + u.host + ":" + u.port +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!send_all(req.data(), req.size())) {
            error = "the relay closed the connection during the handshake";
            return false;
        }
        std::string head;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (head.find("\r\n\r\n") == std::string::npos && head.size() < 8192) {
            char buf[1024];
            const long n = conn_->read(buf, sizeof buf);
            if (n < 0) { error = "the relay closed the connection during the handshake"; return false; }
            if (n == 0) {
                if (std::chrono::steady_clock::now() > deadline) { error = "handshake timed out"; return false; }
                conn_->wait(false, 20);
                continue;
            }
            head.append(buf, static_cast<std::size_t>(n));
        }
        const std::size_t end = head.find("\r\n\r\n");
        if (end == std::string::npos || head.compare(0, 12, "HTTP/1.1 101") != 0) {
            error = "the relay refused the WebSocket upgrade";
            return false;
        }
        const std::string expect_src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const auto digest = sha1(expect_src);
        const std::string expect = base64(digest.data(), digest.size());
        if (head.find(expect) == std::string::npos) {
            error = "bad Sec-WebSocket-Accept from the relay";
            return false;
        }
        rx_.assign(head.begin() + static_cast<std::ptrdiff_t>(end + 4), head.end());
        // The id frame can arrive in the same read as the 101 response (a busy machine); pump() only
        // parses after reading new bytes, so parse what the handshake already buffered first.
        if (!rx_.empty()) parse_rx();
        // Wait for the id message so local_id() is valid when open() returns.
        while (!have_id_ && !closed_) {
            pump();
            if (have_id_ || closed_) break;
            if (std::chrono::steady_clock::now() > deadline) { error = "the relay did not assign an id"; return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (closed_) { error = "the relay closed the connection"; return false; }
        return true;
    }

    PeerId local_id() const override { return id_; }
    bool ready() const override { return have_id_ && !closed_; }
    bool closed() const override { return closed_; }

    bool send(PeerId to, std::span<const std::byte> data) override {
        if (!ready() || data.size() > max_packet_size) return false;
        std::array<std::uint8_t, max_message> buf{};
        for (int i = 0; i < 4; ++i) buf[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(to >> (i * 8));
        if (!data.empty()) std::memcpy(buf.data() + 4, data.data(), data.size());
        return send_frame(0x2, buf.data(), data.size() + 4);
    }

    bool recv(Packet& out) override {
        pump();
        if (inbox_.empty()) return false;
        out = std::move(inbox_.front());
        inbox_.pop_front();
        return true;
    }

private:
    bool send_all(const void* data, std::size_t n) {
        const char* p = static_cast<const char*>(data);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (n > 0) {
            const long w = conn_->write(p, n);
            if (w > 0) { p += w; n -= static_cast<std::size_t>(w); continue; }
            // Socket full: wait briefly, else give up (datagram semantics: drop).
            if (w == 0 && !closed_ && std::chrono::steady_clock::now() < deadline && conn_->wait(true, 50)) continue;
            return false;
        }
        return true;
    }

    bool send_frame(std::uint8_t opcode, const std::uint8_t* payload, std::size_t n) {
        std::array<std::uint8_t, max_message + 14> f{};
        std::size_t len = 0;
        f[len++] = static_cast<std::uint8_t>(0x80 | opcode);
        if (n < 126) {
            f[len++] = static_cast<std::uint8_t>(0x80 | n);
        } else {
            f[len++] = 0x80 | 126;
            f[len++] = static_cast<std::uint8_t>(n >> 8);
            f[len++] = static_cast<std::uint8_t>(n);
        }
        std::uint8_t mask[4];
        std::random_device rd;
        for (auto& m : mask) m = static_cast<std::uint8_t>(rd());
        std::memcpy(f.data() + len, mask, 4);
        len += 4;
        for (std::size_t i = 0; i < n; ++i) f[len + i] = payload[i] ^ mask[i & 3];
        return send_all(f.data(), len + n);
    }

    void fail() {
        closed_ = true;
        conn_.reset();
    }

    void on_message(const std::vector<std::uint8_t>& m) {
        if (m.size() < 4 || m.size() > max_message) return; // hostile or garbled: ignore
        const PeerId head = PeerId(m[0]) | (PeerId(m[1]) << 8) | (PeerId(m[2]) << 16) | (PeerId(m[3]) << 24);
        if (!have_id_) {
            id_ = head;
            have_id_ = true;
            return;
        }
        if (inbox_.size() >= 4096) return;
        Packet p;
        p.from = head;
        p.data.resize(m.size() - 4);
        if (m.size() > 4) std::memcpy(p.data.data(), m.data() + 4, m.size() - 4);
        inbox_.push_back(std::move(p));
    }

    // Reads what the socket has, parsing complete frames out of rx_ after every chunk: a backlog (a
    // frame that ran long, a busy machine) is many whole messages, which is not a flood.
    void pump() {
        if (!conn_) return;
        for (;;) {
            char buf[2048];
            const long n = conn_->read(buf, sizeof buf);
            if (n > 0) {
                rx_.insert(rx_.end(), buf, buf + n);
                if (!parse_rx()) return;
                continue;
            }
            if (n < 0) { fail(); return; }
            break;
        }
    }

    // False when the connection failed.
    bool parse_rx() {
        std::size_t pos = 0;
        while (rx_.size() - pos >= 2) {
            const std::uint8_t b0 = rx_[pos], b1 = rx_[pos + 1];
            std::uint64_t n = b1 & 0x7F;
            std::size_t hdr = 2;
            if (n == 126) {
                if (rx_.size() - pos < 4) break;
                n = (std::uint64_t(rx_[pos + 2]) << 8) | rx_[pos + 3];
                hdr = 4;
            } else if (n == 127) {
                fail(); // far larger than any datagram we accept
                return false;
            }
            if (b1 & 0x80) { fail(); return false; } // servers must not mask
            if (n > max_message) { fail(); return false; }
            if (rx_.size() - pos < hdr + n) break;
            const std::uint8_t op = b0 & 0x0F;
            const std::uint8_t* p = rx_.data() + pos + hdr;
            if (op == 0x8) { fail(); return false; }
            if (op == 0x9) {
                send_frame(0xA, p, static_cast<std::size_t>(n));
            } else if (op == 0x0 || op == 0x1 || op == 0x2) {
                if (op != 0x0) frag_.clear();
                frag_.insert(frag_.end(), p, p + n);
                if (frag_.size() > max_message) { fail(); return false; }
                if (b0 & 0x80) {
                    if (op != 0x1) on_message(frag_);
                    frag_.clear();
                }
            }
            pos += hdr + static_cast<std::size_t>(n);
        }
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(pos));
        if (rx_.size() > 4 * max_message) { fail(); return false; } // an endless partial frame: flooding
        return true;
    }

    std::unique_ptr<TcpConn> conn_;
    PeerId id_ = 0;
    bool have_id_ = false;
    bool closed_ = false;
    std::vector<std::uint8_t> rx_;
    std::vector<std::uint8_t> frag_;
    std::deque<Packet> inbox_;
};

} // namespace

std::unique_ptr<WebSocketTransport> open_websocket_transport(const WebSocketOptions& options, std::string& error) {
    ParsedUrl u;
    if ((!parse_url(options.url, u, error)) || (u.scheme != "ws" && u.scheme != "wss")) {
        if (error.empty()) error = "bad relay URL '" + options.url + "' (expected ws:// or wss://)";
        return nullptr;
    }
    auto t = std::make_unique<TcpTransport>();
    if (!t->connect_to(u, options, error)) return nullptr;
    return t;
}

#endif

} // namespace cuervo::net

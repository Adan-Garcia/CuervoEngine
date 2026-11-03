#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

// Desktop TCP connection with optional TLS (DESIGN.md 4.19), shared by the WebSocket transport
// (wss://) and the HTTP client (https://). Networking is outside the simulation: nothing here
// reaches the state hash, and results enter the sim only as recorded input or script-copied state.
// Not built for Emscripten (the browser provides WebSocket and fetch, with its own TLS).
//
// TLS is Mbed TLS (CUERVO_TLS). Certificates are verified against the system trust store
// (Linux/BSD: the usual CA bundle files; Windows: the ROOT store) or `ca_file` when given, and the
// host name is checked. Verification is never skipped silently: `insecure` is an explicit
// test-only switch, and a build without CUERVO_TLS refuses TLS URLs with an error.
namespace cuervo::net {

struct ParsedUrl {
    std::string scheme; // lower case: http, https, ws, wss
    std::string host;
    std::string port;
    std::string path;   // always starts with '/', includes the query
};

// scheme://host[:port][/path]; false with `error` set when malformed or not one of the four schemes.
bool parse_url(const std::string& url, ParsedUrl& out, std::string& error);

struct TcpOptions {
    bool tls = false;
    std::string ca_file;   // PEM bundle to trust instead of the system store
    bool insecure = false; // TEST ONLY: skip certificate verification
    int timeout_ms = 5000; // connect and TLS handshake budget
    const std::atomic<bool>* cancel = nullptr; // polled during connect/handshake (set by another thread)
};

// TcpConn::error() after read() returned -1 because the peer closed in an orderly way.
inline constexpr const char* tcp_eof_message = "connection closed by the peer";

class TcpConn {
public:
    virtual ~TcpConn() = default;
    // Non-blocking after connect. read/write return bytes moved, 0 when it would block, -1 when
    // the connection is closed or failed (error() says why when known).
    virtual long read(void* buf, std::size_t n) = 0;
    virtual long write(const void* buf, std::size_t n) = 0;
    // Waits up to `ms` for the socket to be readable/writable (TLS-buffered data counts as readable).
    virtual bool wait(bool for_write, int ms) = 0;
    virtual const std::string& error() const = 0;
};

// Null with `error` set (resolve, connect, TLS handshake or certificate failure, timeout).
std::unique_ptr<TcpConn> tcp_connect(const std::string& host, const std::string& port, const TcpOptions& options,
                                     std::string& error);

} // namespace cuervo::net

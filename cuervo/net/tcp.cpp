#include "cuervo/net/tcp.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(CUERVO_HAS_TLS)
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#endif

namespace cuervo::net {

bool parse_url(const std::string& url, ParsedUrl& out, std::string& error) {
    const std::size_t sep = url.find("://");
    if (sep == std::string::npos) {
        error = "bad URL '" + url + "' (expected scheme://host[:port]/path)";
        return false;
    }
    std::string scheme = url.substr(0, sep);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string def_port;
    if (scheme == "http" || scheme == "ws") def_port = "80";
    else if (scheme == "https" || scheme == "wss") def_port = "443";
    else {
        error = "unsupported URL scheme '" + scheme + "' in '" + url + "'";
        return false;
    }
    const std::string rest = url.substr(sep + 3);
    const std::size_t slash = rest.find_first_of("/?");
    const std::string hostport = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (path[0] == '?') path.insert(path.begin(), '/');
    out.port = def_port;
    std::string host = hostport;
    if (!hostport.empty() && hostport[0] == '[') { // [v6]:port
        const std::size_t close = hostport.find(']');
        if (close == std::string::npos) {
            error = "bad URL '" + url + "' (unterminated [)";
            return false;
        }
        host = hostport.substr(1, close - 1);
        if (close + 1 < hostport.size() && hostport[close + 1] == ':') out.port = hostport.substr(close + 2);
    } else {
        const std::size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            out.port = hostport.substr(colon + 1);
            host = hostport.substr(0, colon);
        }
    }
    if (host.empty() || out.port.empty() || out.port.find_first_not_of("0123456789") != std::string::npos ||
        host.find_first_of(" \r\n\t") != std::string::npos) {
        error = "bad URL '" + url + "' (missing or invalid host or port)";
        return false;
    }
    out.scheme = scheme;
    out.host = host;
    out.path = path;
    return true;
}

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
using io_len_t = int;
bool would_block_now() { return WSAGetLastError() == WSAEWOULDBLOCK; }
bool connect_pending() { return WSAGetLastError() == WSAEWOULDBLOCK; }
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
using io_len_t = std::size_t;
bool would_block_now() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool connect_pending() { return errno == EINPROGRESS; }
#endif
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL; // a dead peer must not kill the game with SIGPIPE
#else
constexpr int send_flags = 0;
#endif

bool wait_socket(sock_t s, bool for_write, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int r = for_write ? ::select(static_cast<int>(s) + 1, nullptr, &set, nullptr, &tv)
                            : ::select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv);
    return r > 0;
}

using Clock = std::chrono::steady_clock;

class PlainConn : public TcpConn {
public:
    explicit PlainConn(sock_t s) : socket_(s) {}
    ~PlainConn() override {
        if (socket_ != bad_socket) close_socket(socket_);
    }
    long read(void* buf, std::size_t n) override {
        if (socket_ == bad_socket) return -1;
        const auto r = ::recv(socket_, static_cast<char*>(buf), static_cast<io_len_t>(n), 0);
        if (r > 0) return static_cast<long>(r);
        if (r < 0 && would_block_now()) return 0;
        error_ = r == 0 ? tcp_eof_message : "socket read failed";
        return -1;
    }
    long write(const void* buf, std::size_t n) override {
        if (socket_ == bad_socket) return -1;
        const auto w = ::send(socket_, static_cast<const char*>(buf), static_cast<io_len_t>(n), send_flags);
        if (w >= 0) return static_cast<long>(w);
        if (would_block_now()) return 0;
        error_ = "socket write failed";
        return -1;
    }
    bool wait(bool for_write, int ms) override { return socket_ != bad_socket && wait_socket(socket_, for_write, ms); }
    const std::string& error() const override { return error_; }

protected:
    sock_t socket_;
    std::string error_;
};

#if defined(CUERVO_HAS_TLS)

std::string tls_error_text(int code) {
    char buf[160];
    mbedtls_strerror(code, buf, sizeof buf);
    return buf;
}

// The system trust roots, loaded once per process and shared (read-only afterwards).
struct SystemRoots {
    mbedtls_x509_crt chain;
    int loaded = 0;
    SystemRoots() {
        mbedtls_x509_crt_init(&chain);
#if defined(_WIN32)
        if (HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT")) {
            for (const CERT_CONTEXT* c = CertEnumCertificatesInStore(store, nullptr); c != nullptr;
                 c = CertEnumCertificatesInStore(store, c)) {
                if (mbedtls_x509_crt_parse_der(&chain, c->pbCertEncoded, c->cbCertEncoded) == 0) ++loaded;
            }
            CertCloseStore(store, 0);
        }
#else
        static const char* const bundles[] = {"/etc/ssl/certs/ca-certificates.crt", // Debian, Arch, Alpine
                                              "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora, RHEL
                                              "/etc/ssl/ca-bundle.pem",             // openSUSE
                                              "/etc/ssl/cert.pem"};                 // macOS, BSD
        for (const char* b : bundles) {
            if (mbedtls_x509_crt_parse_file(&chain, b) >= 0) {
                loaded = 1;
                break;
            }
        }
#endif
    }
    ~SystemRoots() { mbedtls_x509_crt_free(&chain); }
    SystemRoots(const SystemRoots&) = delete;
    SystemRoots& operator=(const SystemRoots&) = delete;
};

const SystemRoots& system_roots() {
    static const SystemRoots roots;
    return roots;
}

class TlsConn final : public PlainConn {
public:
    explicit TlsConn(sock_t s) : PlainConn(s) {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&ca_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&drbg_);
    }
    ~TlsConn() override {
        if (handshaken_ && !failed_) mbedtls_ssl_close_notify(&ssl_);
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
    }

    bool handshake(const std::string& host, const TcpOptions& o, std::string& error) {
        static const bool psa_ok = psa_crypto_init() == PSA_SUCCESS; // TLS 1.3 uses PSA in 3.6
        if (!psa_ok) {
            error = "TLS: psa_crypto_init failed";
            return false;
        }
        int r = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_, nullptr, 0);
        if (r != 0) {
            error = "TLS: no entropy source: " + tls_error_text(r);
            return false;
        }
        r = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
        if (r != 0) {
            error = "TLS: config failed: " + tls_error_text(r);
            return false;
        }
        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
        mbedtls_ssl_conf_min_tls_version(&conf_, MBEDTLS_SSL_VERSION_TLS1_2);
        if (o.insecure) {
            mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_NONE);
        } else {
            mbedtls_ssl_conf_authmode(&conf_, MBEDTLS_SSL_VERIFY_REQUIRED);
            if (!o.ca_file.empty()) {
                r = mbedtls_x509_crt_parse_file(&ca_, o.ca_file.c_str());
                if (r < 0) {
                    error = "TLS: cannot read ca_file '" + o.ca_file + "': " + tls_error_text(r);
                    return false;
                }
                mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
            } else {
                const SystemRoots& roots = system_roots();
                if (roots.loaded == 0) {
                    error = "TLS: no system trust store found; pass ca_file to trust a certificate bundle";
                    return false;
                }
                mbedtls_ssl_conf_ca_chain(&conf_, const_cast<mbedtls_x509_crt*>(&roots.chain), nullptr);
            }
        }
        r = mbedtls_ssl_setup(&ssl_, &conf_);
        if (r != 0) {
            error = "TLS: setup failed: " + tls_error_text(r);
            return false;
        }
        r = mbedtls_ssl_set_hostname(&ssl_, host.c_str()); // SNI and the certificate name check
        if (r != 0) {
            error = "TLS: bad host name: " + tls_error_text(r);
            return false;
        }
        mbedtls_ssl_set_bio(&ssl_, this, &TlsConn::bio_send, &TlsConn::bio_recv, nullptr);

        const auto deadline = Clock::now() + std::chrono::milliseconds(o.timeout_ms);
        for (;;) {
            r = mbedtls_ssl_handshake(&ssl_);
            if (r == 0) break;
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if ((o.cancel != nullptr && o.cancel->load()) || Clock::now() > deadline) {
                    error = "TLS handshake timed out";
                    return false;
                }
                wait_socket(socket_, r == MBEDTLS_ERR_SSL_WANT_WRITE, 20);
                continue;
            }
            error = "TLS handshake failed: " + tls_error_text(r);
            if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                char info[256];
                mbedtls_x509_crt_verify_info(info, sizeof info, "", mbedtls_ssl_get_verify_result(&ssl_));
                std::string why = info;
                while (!why.empty() && (why.back() == '\n' || why.back() == ' ')) why.pop_back();
                error = "TLS certificate verification failed for '" + host + "': " + why +
                        " (pass ca_file to trust a private CA)";
            }
            return false;
        }
        handshaken_ = true;
        return true;
    }

    long read(void* buf, std::size_t n) override {
        if (failed_) return -1;
        const int r = mbedtls_ssl_read(&ssl_, static_cast<unsigned char*>(buf), n);
        if (r > 0) return r;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
            r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            return 0;
        failed_ = true;
        error_ = r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF ? tcp_eof_message
                                                                   : "TLS read failed: " + tls_error_text(r);
        return -1;
    }
    long write(const void* buf, std::size_t n) override {
        if (failed_) return -1;
        const int r = mbedtls_ssl_write(&ssl_, static_cast<const unsigned char*>(buf), n);
        if (r >= 0) return r;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        failed_ = true;
        error_ = "TLS write failed: " + tls_error_text(r);
        return -1;
    }
    bool wait(bool for_write, int ms) override {
        if (!for_write && mbedtls_ssl_get_bytes_avail(&ssl_) > 0) return true;
        return PlainConn::wait(for_write, ms);
    }

private:
    static int bio_send(void* ctx, const unsigned char* buf, std::size_t n) {
        auto* self = static_cast<TlsConn*>(ctx);
        const auto w = ::send(self->socket_, reinterpret_cast<const char*>(buf), static_cast<io_len_t>(n), send_flags);
        if (w >= 0) return static_cast<int>(w);
        return would_block_now() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    static int bio_recv(void* ctx, unsigned char* buf, std::size_t n) {
        auto* self = static_cast<TlsConn*>(ctx);
        const auto r = ::recv(self->socket_, reinterpret_cast<char*>(buf), static_cast<io_len_t>(n), 0);
        if (r > 0) return static_cast<int>(r);
        if (r == 0) return 0; // EOF: mbedtls reports it as a close
        return would_block_now() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }

    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config conf_;
    mbedtls_x509_crt ca_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context drbg_;
    bool handshaken_ = false;
    bool failed_ = false;
};

#endif // CUERVO_HAS_TLS

} // namespace

std::unique_ptr<TcpConn> tcp_connect(const std::string& host, const std::string& port, const TcpOptions& options,
                                     std::string& error) {
#if !defined(CUERVO_HAS_TLS)
    if (options.tls) {
        error = "this build has no TLS (configure with -DCUERVO_TLS=ON); use a plain ws:// or http:// URL";
        return nullptr;
    }
#endif
    ensure_winsock();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
        error = "cannot resolve '" + host + ":" + port + "'";
        return nullptr;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout_ms);
    sock_t s = bad_socket;
    for (addrinfo* a = res; a != nullptr && s == bad_socket; a = a->ai_next) {
        const sock_t c = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c == bad_socket) continue;
        set_nonblocking(c);
        bool ok = ::connect(c, a->ai_addr, static_cast<addr_len_t>(a->ai_addrlen)) == 0;
        if (!ok && connect_pending()) {
            for (;;) {
                if ((options.cancel != nullptr && options.cancel->load()) || Clock::now() > deadline) break;
                if (!wait_socket(c, true, 20)) continue;
                int soerr = 0;
                socklen_t len = sizeof soerr;
                ::getsockopt(c, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
                ok = soerr == 0;
                break;
            }
        }
        if (ok) s = c;
        else close_socket(c);
    }
    ::freeaddrinfo(res);
    if (s == bad_socket) {
        error = "cannot connect to '" + host + ":" + port + "'";
        return nullptr;
    }
    const int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);

#if defined(CUERVO_HAS_TLS)
    if (options.tls) {
        auto conn = std::make_unique<TlsConn>(s); // owns the socket from here on
        TcpOptions o = options;
        o.timeout_ms = (std::max)(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        deadline - Clock::now()).count()));
        if (!conn->handshake(host, o, error)) return nullptr;
        return conn;
    }
#endif
    return std::make_unique<PlainConn>(s);
}

} // namespace cuervo::net

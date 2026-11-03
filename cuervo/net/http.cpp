#include "cuervo/net/http.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <map>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten/fetch.h>
#else
#include "cuervo/net/tcp.hpp"
#endif

namespace cuervo::net {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool has_line_break(const std::string& s) { return s.find_first_of("\r\n") != std::string::npos; }

// Header names are RFC 9110 tokens; anything else could corrupt the request line framing.
bool valid_header_name(const std::string& n) {
    if (n.empty()) return false;
    return std::all_of(n.begin(), n.end(), [](unsigned char c) {
        return std::isalnum(c) || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
    });
}

} // namespace

struct HttpRequest::State {
    mutable std::mutex mutex;
    bool finished = false; // guarded by mutex
    int status = 0;
    std::string error;
    std::string body;
    std::map<std::string, std::string> headers; // lower-case names
    std::atomic<bool> cancel{false};
    std::thread worker; // desktop only; joined by ~HttpRequest

    void finish(int st, std::string err, std::string b, std::map<std::string, std::string> h) {
        std::lock_guard lock(mutex);
        if (finished) return;
        finished = true;
        status = st;
        error = std::move(err);
        body = std::move(b);
        headers = std::move(h);
    }
};

HttpRequest::HttpRequest(std::shared_ptr<State> state) : state_(std::move(state)) {}

HttpRequest::~HttpRequest() {
    state_->cancel = true;
    if (state_->worker.joinable()) state_->worker.join();
}

bool HttpRequest::done() const {
    std::lock_guard lock(state_->mutex);
    return state_->finished;
}
int HttpRequest::status() const {
    std::lock_guard lock(state_->mutex);
    return state_->status;
}
std::string HttpRequest::error() const {
    std::lock_guard lock(state_->mutex);
    return state_->error;
}
std::string HttpRequest::body() const {
    std::lock_guard lock(state_->mutex);
    return state_->body;
}
std::string HttpRequest::header(const std::string& name) const {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->headers.find(lower(name));
    return it == state_->headers.end() ? std::string() : it->second;
}
void HttpRequest::cancel() {
    state_->cancel = true;
    state_->finish(0, "cancelled", {}, {});
}

#if defined(__EMSCRIPTEN__)

namespace {

struct FetchCall {
    std::shared_ptr<HttpRequest::State> state;
    std::size_t max_bytes;
    std::vector<std::string> header_storage;
    std::vector<const char*> header_ptrs;
};

std::map<std::string, std::string> parse_header_block(const std::string& raw) {
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        std::size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos) eol = raw.size();
        const std::string line = raw.substr(pos, eol - pos);
        pos = eol + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string v = line.substr(colon + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        out[lower(line.substr(0, colon))] = v;
    }
    return out;
}

void fetch_done(emscripten_fetch_t* f, bool ok) {
    auto* call = static_cast<FetchCall*>(f->userData);
    std::string headers_raw;
    const std::size_t hl = emscripten_fetch_get_response_headers_length(f);
    if (hl > 0) {
        headers_raw.resize(hl + 1);
        emscripten_fetch_get_response_headers(f, headers_raw.data(), hl + 1);
        headers_raw.resize(std::strlen(headers_raw.c_str()));
    }
    if (ok && f->numBytes > call->max_bytes) {
        call->state->finish(f->status, "response larger than the size cap", {}, {});
    } else if (ok || f->status != 0) {
        // An HTTP error status is still a response; only status 0 is a transport failure.
        call->state->finish(f->status, "", std::string(f->data, static_cast<std::size_t>(f->numBytes)),
                            parse_header_block(headers_raw));
    } else {
        call->state->finish(0, "the browser fetch failed (network error, CORS or timeout)", {}, {});
    }
    emscripten_fetch_close(f);
    delete call;
}

} // namespace

std::shared_ptr<HttpRequest> http_start(const std::string& url, HttpOptions options, std::string& error) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        error = "bad URL '" + url + "' (expected http:// or https://)";
        return nullptr;
    }
    for (const auto& [k, v] : options.headers) {
        if (!valid_header_name(k) || has_line_break(v)) {
            error = "invalid header '" + k + "'";
            return nullptr;
        }
    }
    auto state = std::make_shared<HttpRequest::State>();
    auto* call = new FetchCall{state, options.max_response_bytes, {}, {}};
    for (const auto& [k, v] : options.headers) {
        call->header_storage.push_back(k);
        call->header_storage.push_back(v);
    }
    for (const auto& s : call->header_storage) call->header_ptrs.push_back(s.c_str());
    call->header_ptrs.push_back(nullptr);

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strncpy(attr.requestMethod, options.method.c_str(), sizeof(attr.requestMethod) - 1);
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attr.timeoutMSecs = static_cast<unsigned>(options.timeout_ms);
    attr.userData = call;
    attr.requestHeaders = call->header_ptrs.data();
    if (!options.body.empty()) {
        attr.requestData = options.body.data(); // fetch copies it before returning
        attr.requestDataSize = options.body.size();
    }
    attr.onsuccess = [](emscripten_fetch_t* f) { fetch_done(f, true); };
    attr.onerror = [](emscripten_fetch_t* f) { fetch_done(f, false); };
    attr.onprogress = [](emscripten_fetch_t* f) {
        // Stop a download that already exceeds the cap instead of buffering it all.
        auto* c = static_cast<FetchCall*>(f->userData);
        if (f->dataOffset + f->numBytes > c->max_bytes || c->state->cancel) emscripten_fetch_close(f);
    };
    emscripten_fetch(&attr, url.c_str());
    return std::make_shared<HttpRequest>(std::move(state));
}

#else // desktop

namespace {

using Clock = std::chrono::steady_clock;

struct Response {
    int status = 0;
    std::map<std::string, std::string> headers; // lower-case names
    std::string body;
};

bool is_redirect(int st) { return st == 301 || st == 302 || st == 303 || st == 307 || st == 308; }

bool parse_head(const std::string& head, Response& r, std::string& err) {
    const std::size_t eol = head.find("\r\n");
    const std::string status_line = head.substr(0, eol);
    if (status_line.rfind("HTTP/1.", 0) != 0 || status_line.size() < 12) {
        err = "malformed HTTP status line";
        return false;
    }
    int st = 0;
    const auto res = std::from_chars(status_line.data() + 9, status_line.data() + 12, st);
    if (res.ec != std::errc() || st < 100 || st > 599) {
        err = "malformed HTTP status code";
        return false;
    }
    r.status = st;
    r.headers.clear();
    std::size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        const std::string line = head.substr(pos, e - pos);
        pos = e + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        std::string v = line.substr(colon + 1);
        const std::size_t a = v.find_first_not_of(" \t");
        v = a == std::string::npos ? std::string() : v.substr(a);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        std::string name = lower(line.substr(0, colon));
        auto it = r.headers.find(name);
        if (it == r.headers.end()) r.headers.emplace(std::move(name), std::move(v));
        else it->second += ", " + v; // repeated headers fold like RFC 9110 says
    }
    return true;
}

// Reads one whole response from `conn`. A redirect with a Location returns right after its head
// (its body is not downloaded). False with `err` set on failure; `out.status` keeps the status
// when the head was read (size cap), so callers can still report it.
bool read_response(TcpConn& conn, const HttpOptions& o, const std::string& method, Clock::time_point deadline,
                   const std::atomic<bool>& cancel, Response& out, std::string& err) {
    std::string rx;
    bool head_ok = false;
    bool chunked = false;
    bool have_length = false;
    bool no_body = false;
    std::size_t length = 0;
    std::size_t cpos = 0; // chunk parse position in rx
    auto too_big = [&] {
        err = "response larger than the " + std::to_string(o.max_response_bytes) + " byte size cap";
        return false;
    };
    for (;;) {
        if (cancel) {
            err = "cancelled";
            return false;
        }
        if (Clock::now() > deadline) {
            err = "timed out";
            return false;
        }
        char buf[8192];
        const long n = conn.read(buf, sizeof buf);
        bool eof = false;
        if (n < 0) {
            if (conn.error() == tcp_eof_message) {
                eof = true;
            } else {
                err = conn.error();
                return false;
            }
        } else if (n > 0) {
            rx.append(buf, static_cast<std::size_t>(n));
        }

        while (!head_ok) {
            const std::size_t end = rx.find("\r\n\r\n");
            if (end == std::string::npos) break;
            if (!parse_head(rx.substr(0, end), out, err)) return false;
            rx.erase(0, end + 4);
            if (out.status < 200) continue; // interim 1xx: read the real head
            head_ok = true;
            const auto te = out.headers.find("transfer-encoding");
            chunked = te != out.headers.end() && lower(te->second).find("chunked") != std::string::npos;
            const auto cl = out.headers.find("content-length");
            if (!chunked && cl != out.headers.end()) {
                const auto res = std::from_chars(cl->second.data(), cl->second.data() + cl->second.size(), length);
                if (res.ec != std::errc() || res.ptr != cl->second.data() + cl->second.size()) {
                    err = "malformed Content-Length";
                    return false;
                }
                have_length = true;
            }
            no_body = method == "HEAD" || out.status == 204 || out.status == 304;
            if (is_redirect(out.status) && out.headers.contains("location")) return true;
            if (have_length && !no_body && length > o.max_response_bytes) return too_big();
        }
        if (!head_ok) {
            if (rx.size() > 65536) {
                err = "response head too large";
                return false;
            }
            if (eof) {
                err = "connection closed before the response head";
                return false;
            }
        } else if (no_body) {
            return true;
        } else if (chunked) {
            for (;;) {
                const std::size_t eol = rx.find("\r\n", cpos);
                if (eol == std::string::npos) {
                    if (rx.size() - cpos > 64) {
                        err = "malformed chunked body";
                        return false;
                    }
                    break;
                }
                std::string line = rx.substr(cpos, eol - cpos);
                line.resize(line.find(';') == std::string::npos ? line.size() : line.find(';'));
                std::size_t size = 0;
                const auto res = std::from_chars(line.data(), line.data() + line.size(), size, 16);
                if (line.empty() || res.ec != std::errc() || res.ptr != line.data() + line.size()) {
                    err = "malformed chunk size";
                    return false;
                }
                if (size == 0) return true; // trailers are ignored
                if (size > o.max_response_bytes || out.body.size() + size > o.max_response_bytes) return too_big();
                const std::size_t need = eol + 2 + size + 2;
                if (rx.size() < need) break;
                if (rx[need - 2] != '\r' || rx[need - 1] != '\n') {
                    err = "malformed chunk";
                    return false;
                }
                out.body.append(rx, eol + 2, size);
                cpos = need;
            }
            rx.erase(0, cpos);
            cpos = 0;
            if (eof) {
                err = "connection closed before the body was complete";
                return false;
            }
        } else if (have_length) {
            if (rx.size() >= length) {
                out.body = rx.substr(0, length);
                return true;
            }
            if (eof) {
                err = "connection closed before the body was complete";
                return false;
            }
        } else { // close-delimited
            if (rx.size() > o.max_response_bytes) return too_big();
            if (eof) {
                out.body = std::move(rx);
                return true;
            }
        }
        if (n == 0) conn.wait(false, 20);
    }
}

bool write_all(TcpConn& conn, const std::string& data, Clock::time_point deadline, const std::atomic<bool>& cancel,
               std::string& err) {
    std::size_t off = 0;
    while (off < data.size()) {
        if (cancel || Clock::now() > deadline) {
            err = cancel ? "cancelled" : "timed out";
            return false;
        }
        const long w = conn.write(data.data() + off, data.size() - off);
        if (w < 0) {
            err = conn.error().empty() ? "connection closed while sending the request" : conn.error();
            return false;
        }
        if (w == 0) conn.wait(true, 20);
        else off += static_cast<std::size_t>(w);
    }
    return true;
}

std::string authority(const ParsedUrl& u) {
    const bool def = (u.scheme == "http" && u.port == "80") || (u.scheme == "https" && u.port == "443");
    std::string a = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    if (!def) a += ":" + u.port;
    return a;
}

// Resolves a Location header against the URL it came from.
std::string resolve_location(const ParsedUrl& base, const std::string& loc) {
    if (loc.find("://") != std::string::npos) return loc;
    const std::string origin = base.scheme + "://" + authority(base);
    if (loc.rfind("//", 0) == 0) return base.scheme + ":" + loc;
    if (!loc.empty() && loc[0] == '/') return origin + loc;
    std::string dir = base.path.substr(0, base.path.find('?'));
    dir.resize(dir.rfind('/') + 1);
    return origin + dir + loc;
}

void run_request(const std::shared_ptr<HttpRequest::State>& st, std::string url, HttpOptions o) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(o.timeout_ms);
    std::string method = o.method;
    std::string body = o.body;
    auto headers = o.headers;
    for (int hops = 0;; ++hops) {
        std::string err;
        ParsedUrl u;
        if (!parse_url(url, u, err) || (u.scheme != "http" && u.scheme != "https")) {
            st->finish(0, err.empty() ? "unsupported URL scheme in '" + url + "'" : err, {}, {});
            return;
        }
        TcpOptions t;
        t.tls = u.scheme == "https";
        t.ca_file = o.ca_file;
        t.insecure = o.insecure;
        t.cancel = &st->cancel;
        t.timeout_ms = std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        deadline - Clock::now()).count()));
        auto conn = tcp_connect(u.host, u.port, t, err);
        if (!conn) {
            st->finish(0, st->cancel ? "cancelled" : err, {}, {});
            return;
        }
        std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + authority(u) +
                          "\r\nUser-Agent: CuervoEngine\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
        if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH")
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        for (const auto& [k, v] : headers) {
            const std::string name = lower(k);
            if (name == "host" || name == "content-length" || name == "connection" || name == "transfer-encoding") continue;
            req += k + ": " + v + "\r\n";
        }
        req += "\r\n";
        req += body;
        Response r;
        if (!write_all(*conn, req, deadline, st->cancel, err) ||
            !read_response(*conn, o, method, deadline, st->cancel, r, err)) {
            st->finish(r.status, std::move(err), {}, std::move(r.headers));
            return;
        }
        const auto loc = r.headers.find("location");
        if (is_redirect(r.status) && loc != r.headers.end()) {
            if (hops >= o.max_redirects) {
                st->finish(r.status, "too many redirects", {}, std::move(r.headers));
                return;
            }
            const std::string next = resolve_location(u, loc->second);
            ParsedUrl nu;
            std::string perr;
            if (!parse_url(next, nu, perr) || (nu.scheme != "http" && nu.scheme != "https")) {
                st->finish(r.status, "redirect to an unsupported URL '" + next + "'", {}, std::move(r.headers));
                return;
            }
            if (u.scheme == "https" && nu.scheme == "http") {
                st->finish(r.status, "redirect from https to http refused", {}, std::move(r.headers));
                return;
            }
            if (nu.host != u.host || nu.port != u.port) { // never send credentials to another origin
                std::erase_if(headers, [](const auto& h) {
                    const std::string n = lower(h.first);
                    return n == "authorization" || n == "cookie" || n == "proxy-authorization";
                });
            }
            if (r.status != 307 && r.status != 308 && method != "GET" && method != "HEAD") {
                method = "GET"; // the browsers' behaviour for 301/302/303
                body.clear();
                std::erase_if(headers, [](const auto& h) {
                    const std::string n = lower(h.first);
                    return n == "content-type" || n == "content-length";
                });
            }
            url = next;
            continue;
        }
        st->finish(r.status, "", std::move(r.body), std::move(r.headers));
        return;
    }
}

} // namespace

std::shared_ptr<HttpRequest> http_start(const std::string& url, HttpOptions options, std::string& error) {
    ParsedUrl u;
    if (!parse_url(url, u, error)) return nullptr;
    if (u.scheme != "http" && u.scheme != "https") {
        error = "bad URL '" + url + "' (expected http:// or https://)";
        return nullptr;
    }
    std::transform(options.method.begin(), options.method.end(), options.method.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (!valid_header_name(options.method)) {
        error = "invalid HTTP method";
        return nullptr;
    }
    for (const auto& [k, v] : options.headers) {
        if (!valid_header_name(k) || has_line_break(v)) {
            error = "invalid header '" + k + "'";
            return nullptr;
        }
    }
    if (has_line_break(url)) {
        error = "URL contains line breaks";
        return nullptr;
    }
    auto state = std::make_shared<HttpRequest::State>();
    state->worker = std::thread([state, url, options = std::move(options)]() mutable {
        run_request(state, url, std::move(options));
    });
    return std::make_shared<HttpRequest>(std::move(state));
}

#endif

} // namespace cuervo::net

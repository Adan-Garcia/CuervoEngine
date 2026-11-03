#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Small HTTP/HTTPS client for game services (DESIGN.md 4.19, milestone 11.1). It lives outside
// the simulation: a request runs on its own worker thread (desktop) or the browser's fetch
// (web), and the result is polled with done()/status()/body(); there are no callbacks into the
// sim. Nothing here is hashed or replayed: a script that lets a response change simulation state
// must copy it into recorded state itself (and the replay will not reproduce a different answer).
//
// Desktop: HTTP/1.1 with `Connection: close` over cuervo/net/tcp.hpp (TLS verified against the
// system trust store, or `ca_file`; `insecure` is test-only). Content-Length, chunked and
// close-delimited bodies; a bounded number of redirects (https -> http is refused, credentials
// are dropped when a redirect changes host); a total timeout and a response size cap.
// Web: emscripten_fetch. The browser follows redirects and applies CORS itself, so `max_redirects`
// is not enforced there and a server must allow cross-origin requests.
namespace cuervo::net {

struct HttpOptions {
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_ms = 10000;                 // whole request, including redirects
    std::size_t max_response_bytes = 4u << 20; // response bodies larger than this fail the request
    int max_redirects = 5;
    std::string ca_file;                    // PEM bundle to trust instead of the system store
    bool insecure = false;                  // TEST ONLY: skip certificate verification
};

class HttpRequest {
public:
    struct State; // shared with the worker / fetch callbacks
    ~HttpRequest();
    HttpRequest(const HttpRequest&) = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;

    // True once the request finished, failed, timed out or was cancelled.
    bool done() const;
    // The HTTP status, or 0 while pending and when the request failed before a response (see error()).
    int status() const;
    // Empty on success and while pending; otherwise why the request failed (also set for
    // responses that exceeded the size cap or ran out of redirects, though status() may be set).
    std::string error() const;
    // The response body (empty until done()). Not interpreted: bytes as sent by the server.
    std::string body() const;
    // A response header (case-insensitive name) of the final response, or "".
    std::string header(const std::string& name) const;
    // Stops the request; done() becomes true with error() "cancelled" if it was still pending.
    void cancel();

    explicit HttpRequest(std::shared_ptr<State> state);

private:
    std::shared_ptr<State> state_;
};

// Starts `method url`. Null with `error` set when the URL is malformed or not http(s), or a
// header contains line breaks; failures after that are reported through the request.
std::shared_ptr<HttpRequest> http_start(const std::string& url, HttpOptions options, std::string& error);

} // namespace cuervo::net

#include <doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "cuervo/net/http.hpp"
#include "net_http_fixture.hpp"
#include "net_relay_fixture.hpp"

// The HTTP client against tools/net/http_test_server.py on an ephemeral port (plain and TLS).
// Skipped when python3 (or openssl, for the TLS cases) is unavailable, and on Windows (popen).
using namespace cuervo::net;

namespace {

bool wait_done(HttpRequest& r, int ms = 8000) {
    for (int i = 0; i < ms / 2 && !r.done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return r.done();
}

} // namespace

TEST_CASE("http: malformed requests are refused before any I/O") {
    std::string err;
    CHECK_FALSE(http_start("ftp://example.com/x", {}, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(http_start("not a url", {}, err));
    CHECK_FALSE(http_start("http://", {}, err));
    HttpOptions bad;
    bad.headers.emplace_back("X-Ok", "line\r\nInjected: yes");
    CHECK_FALSE(http_start("http://127.0.0.1:1/", bad, err));
    CHECK(err.find("invalid header") != std::string::npos);
    HttpOptions bad_name;
    bad_name.headers.emplace_back("Bad Name", "v");
    CHECK_FALSE(http_start("http://127.0.0.1:1/", bad_name, err));
}

TEST_CASE("http: a refused connection reports an error, not a hang") {
    std::string err;
    auto r = http_start("http://127.0.0.1:1/", {}, err);
    REQUIRE_MESSAGE(r, err);
    REQUIRE(wait_done(*r));
    CHECK(r->status() == 0);
    CHECK_FALSE(r->error().empty());
}

#if !defined(_WIN32)
TEST_CASE("http: GET, POST, headers, redirects, chunked and size cap against a local server") {
    HttpServer server;
    if (!server.ok()) {
        MESSAGE("python3 unavailable; skipped");
        return;
    }
    std::string err;

    SUBCASE("GET returns status, headers and body") {
        auto r = http_start(server.url("/hello"), {}, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->error().empty());
        CHECK(r->status() == 200);
        CHECK(r->body() == "hello GET");
        CHECK(r->header("x-method") == "GET");
        CHECK(r->header("X-Method") == "GET");
    }
    SUBCASE("POST sends the body and custom headers") {
        HttpOptions o;
        o.method = "POST";
        o.body = std::string("payload\0with-nul", 16);
        o.headers.emplace_back("X-Token", "abc123");
        o.headers.emplace_back("Content-Type", "application/octet-stream");
        auto r = http_start(server.url("/echo"), o, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->status() == 200);
        CHECK(r->body() == o.body);
        CHECK(r->header("x-seen-x-token") == "abc123");
        CHECK(r->header("x-seen-length") == "16");
    }
    SUBCASE("HTTP error statuses are responses, not failures") {
        auto r = http_start(server.url("/status/404"), {}, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->status() == 404);
        CHECK(r->error().empty());
        CHECK(r->body() == "status page");
    }
    SUBCASE("redirects are followed, a 303 turns POST into GET, a 307 keeps the body") {
        auto r = http_start(server.url("/redirect"), {}, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->status() == 200);
        CHECK(r->body() == "hello GET");

        HttpOptions post;
        post.method = "POST";
        post.body = "abc";
        auto r303 = http_start(server.url("/redirect303"), post, err);
        REQUIRE_MESSAGE(r303, err);
        REQUIRE(wait_done(*r303));
        CHECK(r303->body() == "method=GET len=0");

        auto r307 = http_start(server.url("/redirect307"), post, err);
        REQUIRE_MESSAGE(r307, err);
        REQUIRE(wait_done(*r307));
        CHECK(r307->body() == "abc");
    }
    SUBCASE("redirects are limited") {
        HttpOptions o;
        o.max_redirects = 3;
        auto ok = http_start(server.url("/chain/3"), o, err);
        REQUIRE_MESSAGE(ok, err);
        REQUIRE(wait_done(*ok));
        CHECK(ok->status() == 200);
        auto too_far = http_start(server.url("/chain/4"), o, err);
        REQUIRE_MESSAGE(too_far, err);
        REQUIRE(wait_done(*too_far));
        CHECK(too_far->error() == "too many redirects");
        auto loop = http_start(server.url("/loop"), {}, err);
        REQUIRE_MESSAGE(loop, err);
        REQUIRE(wait_done(*loop));
        CHECK(loop->error() == "too many redirects");
    }
    SUBCASE("chunked and close-delimited bodies are decoded") {
        auto r = http_start(server.url("/chunked"), {}, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->error().empty());
        CHECK(r->body().rfind("alpha-beta-gamma", 0) == 0);
        CHECK(r->body().size() == 6 + 5 + 2000);
        auto c = http_start(server.url("/close"), {}, err);
        REQUIRE_MESSAGE(c, err);
        REQUIRE(wait_done(*c));
        CHECK(c->body() == "until-close");
    }
    SUBCASE("the size cap fails the request") {
        HttpOptions o;
        o.max_response_bytes = 1000;
        auto small = http_start(server.url("/big?n=1000"), o, err);
        REQUIRE_MESSAGE(small, err);
        REQUIRE(wait_done(*small));
        CHECK(small->body().size() == 1000);
        auto big = http_start(server.url("/big?n=1001"), o, err);
        REQUIRE_MESSAGE(big, err);
        REQUIRE(wait_done(*big));
        CHECK(big->error().find("size cap") != std::string::npos);
        CHECK(big->body().empty());
        auto chunky = http_start(server.url("/chunked"), o, err);
        REQUIRE_MESSAGE(chunky, err);
        REQUIRE(wait_done(*chunky));
        CHECK(chunky->error().find("size cap") != std::string::npos);
    }
    SUBCASE("the timeout and cancel stop a slow request") {
        HttpOptions o;
        o.timeout_ms = 200;
        const auto t0 = std::chrono::steady_clock::now();
        auto r = http_start(server.url("/slow"), o, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->error() == "timed out");
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));

        auto c = http_start(server.url("/slow"), {}, err);
        REQUIRE_MESSAGE(c, err);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        c->cancel();
        CHECK(c->done());
        CHECK(c->error() == "cancelled");
        c.reset(); // joins the worker promptly
    }
    SUBCASE("destroying a pending request does not hang") {
        auto r = http_start(server.url("/slow"), {}, err);
        REQUIRE_MESSAGE(r, err);
        const auto t0 = std::chrono::steady_clock::now();
        r.reset();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    }
}

#if defined(CUERVO_HAS_TLS)
TEST_CASE("http: https verifies the certificate") {
    TestCert cert;
    if (!cert.ok()) {
        MESSAGE("openssl unavailable; skipped");
        return;
    }
    HttpServer server("--tls-cert \"" + cert.cert() + "\" --tls-key \"" + cert.key() + "\"");
    if (!server.ok()) {
        MESSAGE("python3 unavailable; skipped");
        return;
    }
    std::string err;
    SUBCASE("trusted through ca_file") {
        HttpOptions o;
        o.ca_file = cert.cert();
        auto r = http_start(server.url("/hello", true), o, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK_MESSAGE(r->error().empty(), r->error());
        CHECK(r->status() == 200);
        CHECK(r->body() == "hello GET");
        HttpOptions post = o;
        post.method = "POST";
        post.body = std::string(50000, 'q');
        auto p = http_start(server.url("/echo", true), post, err);
        REQUIRE_MESSAGE(p, err);
        REQUIRE(wait_done(*p));
        CHECK(p->body() == post.body);
        auto chunked = http_start(server.url("/chunked", true), o, err);
        REQUIRE_MESSAGE(chunked, err);
        REQUIRE(wait_done(*chunked));
        CHECK(chunked->body().size() == 2011);
    }
    SUBCASE("an untrusted self-signed certificate is refused by default") {
        auto r = http_start(server.url("/hello", true), {}, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->status() == 0);
        CHECK(r->error().find("verification failed") != std::string::npos);
        CHECK(r->body().empty());
    }
    SUBCASE("insecure is an explicit opt-out") {
        HttpOptions o;
        o.insecure = true;
        auto r = http_start(server.url("/hello", true), o, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->status() == 200);
    }
    SUBCASE("a plain http request to the TLS port fails cleanly") {
        auto r = http_start(server.url("/hello", false), {}, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->status() != 200);
    }
    SUBCASE("a redirect from https to http is refused") {
        HttpOptions o;
        o.ca_file = cert.cert();
        HttpServer plain;
        REQUIRE(plain.ok());
        auto r = http_start(server.url("/downgrade?to=" + plain.url("/hello"), true), o, err);
        REQUIRE_MESSAGE(r, err);
        REQUIRE(wait_done(*r));
        CHECK(r->error() == "redirect from https to http refused");
    }
}
#endif
#endif

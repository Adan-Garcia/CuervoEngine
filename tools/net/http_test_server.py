#!/usr/bin/env python3
"""Local HTTP/HTTPS server for the Cuervo HTTP client tests (cuervo/net/http.cpp).

Routes: /hello (text), /echo (POST: echoes the body; the request headers come back as
X-Seen-* headers), /redirect (302 -> /hello), /redirect303 (303 -> /echo-get), /chain/N
(redirects N more times then /hello), /loop (redirects to itself), /chunked (chunked body),
/big?n=BYTES (BYTES of 'x'), /status/CODE, /slow (never answers within the test timeout),
/close (no Content-Length; ends by closing), /downgrade?to=URL (302 to URL).

Usage: http_test_server.py [--port N] [--port-file F] [--tls-cert C --tls-key K] [--cors] [--exit-on-stdin-eof]

--cors adds the headers a browser needs to let a page on another origin read the responses
(tools/web/http_smoke.py): Access-Control-Allow-Origin/Expose-Headers on every response and an
answer to the OPTIONS preflight.
"""
import argparse, os, ssl, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    cors = False

    def end_headers(self):
        if self.cors:
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Expose-Headers", "*")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def log_message(self, *args):
        pass

    def send_text(self, code, text, extra=None):
        data = text if isinstance(text, bytes) else text.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def redirect(self, code, location):
        self.send_response(code)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def route(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        path = u.path
        length = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(length) if length else b""
        if path == "/hello":
            self.send_text(200, "hello " + self.command, {"X-Method": self.command})
        elif path == "/echo":
            seen = {"X-Seen-" + k.title(): v for k, v in self.headers.items() if k.lower() in ("x-token", "content-type", "authorization")}
            seen["X-Seen-Length"] = str(len(body))
            self.send_text(200, body, seen)
        elif path == "/echo-get":
            self.send_text(200, "method=" + self.command + " len=" + str(len(body)))
        elif path == "/redirect":
            self.redirect(302, "/hello")
        elif path == "/redirect303":
            self.redirect(303, "/echo-get")
        elif path == "/redirect307":
            self.redirect(307, "/echo")
        elif path.startswith("/chain/"):
            n = int(path.split("/")[2])
            self.redirect(302, "/chain/%d" % (n - 1) if n > 1 else "/hello")
        elif path == "/loop":
            self.redirect(302, "/loop")
        elif path == "/downgrade":
            self.redirect(302, q["to"][0])
        elif path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for part in (b"alpha-", b"beta-", b"gamma" * 400):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
            self.close_connection = True
        elif path == "/big":
            self.send_text(200, b"x" * int(q["n"][0]))
        elif path.startswith("/status/"):
            self.send_text(int(path.split("/")[2]), "status page")
        elif path == "/slow":
            time.sleep(5)
            self.send_text(200, "late")
        elif path == "/close":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"until-close")
            self.wfile.flush()
            self.close_connection = True
        else:
            self.send_text(404, "no such route")

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = route


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--port-file")
    ap.add_argument("--tls-cert")
    ap.add_argument("--tls-key")
    ap.add_argument("--cors", action="store_true")
    ap.add_argument("--exit-on-stdin-eof", action="store_true")
    a = ap.parse_args()
    Handler.cors = a.cors
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    srv.daemon_threads = True
    if a.tls_cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(a.tls_cert, a.tls_key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    port = srv.server_address[1]
    print("listening", port, flush=True)
    if a.port_file:
        with open(a.port_file + ".tmp", "w") as f:
            f.write(str(port))
        os.replace(a.port_file + ".tmp", a.port_file)
    if a.exit_on_stdin_eof:
        def watch():
            sys.stdin.read()
            os._exit(0)
        threading.Thread(target=watch, daemon=True).start()
    srv.serve_forever()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Browser HTTP client smoke (milestone 11.1): the web build's fetch glue against a local server.

Serves tools/net/http_test_server.py with CORS headers, then runs headless Chrome on the web build of
tools/web/http_game (tools/web/smoke_test.mjs). The game does Http.get("/hello") and Http.post("/echo")
and prints status, body and headers; this script checks them and prints the lines.

  python3 -I tools/web/http_smoke.py --web-dir build/http-web/runtime [--port 18433]

Build the web runtime with CUERVO_WEB_GAME=tools/web/http_game (docs/WEB.md). The game's URL has the
port compiled in (18433); a busy port fails the run. Exit 0 ok, 1 failed, 77 no Node/Chrome.
Not in ctest (needs Emscripten).
"""
import argparse, os, subprocess, sys, tempfile, time


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--web-dir", required=True)
    ap.add_argument("--port", type=int, default=18433)
    ap.add_argument("--smoke", default=os.path.join(here, "smoke_test.mjs"))
    ap.add_argument("--server", default=os.path.join(here, "..", "net", "http_test_server.py"))
    ap.add_argument("--timeout", type=float, default=30.0)
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="cuervo_webhttp_")
    server = None
    try:
        port_file = os.path.join(work, "server.port")
        server = subprocess.Popen([sys.executable, "-I", a.server, "--port", str(a.port), "--port-file", port_file,
                                   "--cors", "--exit-on-stdin-eof"], stdin=subprocess.PIPE,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(port_file):
                break
            if server.poll() is not None:
                print(f"the test server could not bind port {a.port}", file=sys.stderr)
                return 1
            time.sleep(0.05)
        r = subprocess.run(["node", a.smoke, "--dir", a.web_dir, "--seconds", str(int(a.timeout)),
                            "--args", f"--quit-after {int(a.timeout)}", "--expect", "http-smoke done"],
                           capture_output=True, text=True, timeout=a.timeout + 90)
        if r.returncode == 77:
            print("no browser; skipped")
            return 77
        lines = [l[l.find("http-smoke"):] for l in r.stdout.splitlines() if "http-smoke" in l]
        for l in lines:
            print(l)
        text = "\n".join(lines)
        ok = (r.returncode == 0
              and "http-smoke get status=200 body=hello GET method=GET error=" in text
              and "http-smoke post status=200 body=ping token=abc error=" in text)
        if not ok:
            print(r.stdout[-1500:] + r.stderr[-1500:], file=sys.stderr)
            print("FAILED: the browser HTTP requests did not return what the server sent", file=sys.stderr)
            return 1
        print("OK")
        return 0
    finally:
        if server is not None:
            try:
                server.stdin.close()
                server.wait(5)
            except Exception:
                server.kill()


if __name__ == "__main__":
    sys.exit(main())

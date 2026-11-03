#!/usr/bin/env python3
"""Native + browser netplay through the WebSocket relay (milestone 11.1, DESIGN.md 4.19).

A native `cuervo` joins a relay room as player 0 and headless Chrome (tools/web/smoke_test.mjs) runs the
web build of the same game as player 1; both simulate N ticks from the delivered inputs and must log the
same state hash and snapshot hash at the same tick.

  python3 -I tools/web/net_smoke.py --native build/debug/runtime/cuervo --web-dir build/web/runtime
          --game examples/netduel [--ticks 300] [--mode lockstep|rollback]

The web runtime must be built with CUERVO_WEB_GAME=examples/netduel (docs/WEB.md). Exit 0 ok, 1 failed,
77 when there is no Node/Chrome (smoke_test.mjs reports that itself).
Verified locally (emsdk 6.0.12, headless Chrome): lockstep and rollback give equal hashes at tick 300. Not in ctest (needs Emscripten).
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time


def parse(text):
    state = re.search(r"at tick (\d+) \(state hash ([0-9a-f]{16})\)", text)
    snap = re.search(r"Snapshot hash ([0-9a-f]{16}) at tick (\d+)", text)
    if not state or not snap:
        return None
    return (state.group(2), snap.group(1), int(state.group(1)), int(snap.group(2)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--native", required=True)
    ap.add_argument("--web-dir", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--smoke", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "smoke_test.mjs"))
    ap.add_argument("--relay", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "net", "relay.py"))
    ap.add_argument("--ticks", type=int, default=300)
    ap.add_argument("--mode", default="lockstep")
    ap.add_argument("--timeout", type=float, default=90.0)
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="cuervo_webnet_")
    env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", CUERVO_AUDIO="null")
    relay = native = None
    try:
        port_file = os.path.join(work, "relay.port")
        relay = subprocess.Popen([sys.executable, "-I", a.relay, "--port", "0", "--port-file", port_file,
                                  "--exit-on-stdin-eof"], stdin=subprocess.PIPE,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(port_file) and open(port_file).read().strip():
                break
            time.sleep(0.05)
        else:
            print("relay did not start", file=sys.stderr)
            return 1
        url = f"ws://127.0.0.1:{int(open(port_file).read().strip())}"
        common = ["--net-relay", url, "--net-room", "web1", "--net-mode", a.mode,
                  "--screenshot-tick", str(a.ticks)]
        nlog = os.path.join(work, "native.log")
        native = subprocess.Popen(
            [a.native, "--game", a.game, "--backend", "gl", "--no-audio", "--vsync", "off", "--screenshot",
             os.path.join(work, "n.ppm"), "--press", "20-200:right,40-400:shoot", "--quit-after", str(a.timeout)]
            + common, env=env, stdout=open(nlog, "w"), stderr=subprocess.STDOUT)
        time.sleep(1.0)  # the native peer joins first, so it is player 0
        web_args = " ".join(common + ["--screenshot", "/w.ppm", "--press", "10-150:left,30-400:up,50-400:shoot"])
        web = subprocess.run(["node", a.smoke, "--dir", a.web_dir, "--seconds", str(int(a.timeout)),
                              "--args", web_args, "--expect", "Snapshot hash"],
                             capture_output=True, text=True, timeout=a.timeout + 90)
        try:
            native.wait(30)
        except subprocess.TimeoutExpired:
            native.kill()
        if web.returncode == 77:
            print("no browser; skipped")
            return 77
        w, n = parse(web.stdout), parse(open(nlog, errors="replace").read())
        print("native:", n)
        print("web:   ", w)
        if web.returncode != 0 or w is None or n is None or w != n or n[2] != a.ticks:
            print(web.stdout[-1500:] + web.stderr[-1500:], file=sys.stderr)
            print("FAILED: the native and web peers disagree (or did not finish)", file=sys.stderr)
            return 1
        print("OK")
        return 0
    finally:
        if native is not None and native.poll() is None:
            native.kill()
        if relay is not None:
            try:
                relay.stdin.close()
                relay.wait(5)
            except Exception:
                relay.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

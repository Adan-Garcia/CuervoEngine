#!/usr/bin/env python3
"""End-to-end netplay check: `cuervo` processes (2 to 4) play examples/netduel or netarena against
each other (UDP, a UDP mesh for more than two players, or through tools/net/relay.py) for N ticks with
scripted input, and must print the same state hash and snapshot hash. Each process also records its
session; the replays must be identical and, when --editor is given, verify offline.

  netplay_test.py --cuervo build/debug/runtime/cuervo --game examples/netduel [--mode lockstep|rollback]
                  [--relay tools/net/relay.py] [--ticks 300] [--players 2] [--late 0]
                  [--editor build/debug/editor/cuervo-editor]

--late N makes the game itself start the session at tick N (a patched copy of the game calls
Net.host/Net.join/Net.relay/Net.mesh), so the replays include the offline ticks before it. Ports are
picked by the OS; there are no wall-clock assumptions beyond the generous timeouts.

Exit 0 on success. Headless: GL on the offscreen video driver, no audio (docs/NETWORK.md).
"""
import argparse, os, re, shutil, socket, subprocess, sys, tempfile, time


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def free_udp_ports(n):
    socks = []
    try:
        for _ in range(n):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind(("127.0.0.1", 0))
            socks.append(s)
        return [s.getsockname()[1] for s in socks]
    finally:
        for s in socks:
            s.close()


def patched_game(src, dst, call, tick):
    """A copy of the game that starts its network session by itself at `tick` (a prelude-free script edit)."""
    shutil.copytree(src, dst)
    scripts = os.path.join(dst, "scripts")
    for name in sorted(os.listdir(scripts)):
        path = os.path.join(scripts, name)
        text = open(path, encoding="utf-8").read()
        marker = "    fn update(dt: float) {\n"
        if marker in text:
            text = ("var late_ticks = 0\n" + text).replace(
                marker, marker + f"        late_ticks += 1\n        if late_ticks == {tick} {{ {call} }}\n", 1)
            open(path, "w", encoding="utf-8").write(text)
            return
    raise RuntimeError("no update() to patch in " + src)


PRESSES = ["{a}-200:right,{b}-400:shoot,{c}-160:down",
           "{d}-150:left,{b}-400:up,{c}-400:shoot",
           "{a}-300:up,{c}-400:shoot,{d}-250:right",
           "{d}-350:down,{a}-400:shoot,{b}-300:left"]


def parse(log):
    state = re.search(r"at tick (\d+) \(state hash ([0-9a-f]{16})\)", log)
    snap = re.search(r"Snapshot hash ([0-9a-f]{16}) at tick (\d+)", log)
    if not state or not snap:
        return None
    return state.group(2), snap.group(1), int(state.group(1)), int(snap.group(2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cuervo", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--mode", default="lockstep")
    ap.add_argument("--relay")
    ap.add_argument("--ticks", type=int, default=300)
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--late", type=int, default=0)
    ap.add_argument("--editor")
    ap.add_argument("--timeout", type=float, default=60.0)
    a = ap.parse_args()
    n = a.players

    work = tempfile.mkdtemp(prefix="cuervo_netplay_")
    env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", CUERVO_AUDIO="null")
    procs, logs, games = [], [], []
    relay = None
    try:
        calls = None  # per player: the Net.* call a --late game makes
        if a.relay:
            port_file = os.path.join(work, "relay.port")
            relay = subprocess.Popen([sys.executable, "-I", a.relay, "--port", "0", "--port-file", port_file,
                                      "--exit-on-stdin-eof"], stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(200):
                if os.path.exists(port_file) and open(port_file).read().strip():
                    break
                time.sleep(0.05)
            else:
                print("relay did not start", file=sys.stderr)
                return 1
            port = int(open(port_file).read().strip())
            url, room = f"ws://127.0.0.1:{port}", f"t{os.getpid()}"
            conn = [["--net-relay", url, "--net-room", room, "--net-players", str(n)]] * n
            calls = [f'Net.relay("{url}", "{room}", {n})'] * n
        elif n == 2:
            port = free_udp_port()
            conn = [["--net-host", str(port)], ["--net-join", f"127.0.0.1:{port}"]]
            calls = [f"Net.host({port})", f'Net.join("127.0.0.1:{port}")']
        else:
            addrs = ",".join(f"127.0.0.1:{p}" for p in free_udp_ports(n))
            conn = [["--net-mesh", addrs, "--net-player", str(i)] for i in range(n)]
            calls = [f'Net.mesh("{addrs}", {i})' for i in range(n)]
        # Different scripted input per player, so a wrong player mapping shows in the hashes. Presses are in
        # absolute ticks and start after the session does when it starts late.
        base = a.late + 10 if a.late else 0
        presses = [PRESSES[i % 4].format(a=base + 20, b=base + 40, c=base + 100, d=base + 10) for i in range(n)]
        for i in range(n):
            log = os.path.join(work, f"p{i}.log")
            game = a.game
            if a.late:
                game = os.path.join(work, f"game{i}")
                patched_game(a.game, game, calls[i], a.late)
            games.append(game)
            args = [a.cuervo, "--game", game, "--backend", "gl", "--no-audio", "--vsync", "off",
                    "--screenshot", os.path.join(work, f"p{i}.ppm"), "--screenshot-tick", str(a.ticks),
                    "--press", presses[i], "--record", os.path.join(work, f"p{i}.replay"),
                    "--net-mode", a.mode, "--quit-after", str(a.timeout)]
            if not a.late:
                args += conn[i]
            logs.append(log)
            with open(log, "w") as out:
                procs.append(subprocess.Popen(args, env=env, stdout=out, stderr=subprocess.STDOUT))
            if i == 0 and not a.relay and n == 2:
                time.sleep(0.5)  # the host binds before the joiner sends
        deadline = time.time() + a.timeout + 20
        for p in procs:
            try:
                p.wait(max(1.0, deadline - time.time()))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        results = []
        for i, log in enumerate(logs):
            with open(log, errors="replace") as f:
                text = f.read()
            r = parse(text)
            if procs[i].returncode != 0 or r is None:
                print(f"player {i} failed (exit {procs[i].returncode}):\n{text[-2000:]}", file=sys.stderr)
                for j, other in enumerate(logs):  # the other peers' logs say who never connected
                    if j != i and os.path.exists(other):
                        with open(other, errors="replace") as f:
                            print(f"--- player {j} log tail:\n{f.read()[-700:]}", file=sys.stderr)
                return 1
            results.append(r)
            print(f"player {i}:", r)
        if any(r != results[0] for r in results) or results[0][2] != a.ticks:
            print("MISMATCH between the peers (or wrong tick)", file=sys.stderr)
            return 1
        # The recorded sessions are the same match: identical replays.
        blobs = []
        for i in range(n):
            with open(os.path.join(work, f"p{i}.replay"), "rb") as f:
                blobs.append(f.read())
        if any(b != blobs[0] for b in blobs):
            print(f"the peers recorded different replays (sizes {[len(b) for b in blobs]})", file=sys.stderr)
            return 1
        if a.editor:
            for i in range(n if a.late else 1):
                r = subprocess.run([a.editor, "--project", games[i], "--replay-verify",
                                    os.path.join(work, f"p{i}.replay")],
                                   env=env, capture_output=True, text=True, timeout=240)
                if r.returncode != 0:
                    print("offline replay verification failed:\n" + r.stdout[-1500:] + r.stderr[-1500:],
                          file=sys.stderr)
                    return 1
            print("offline replay verified")
        print("OK")
        return 0
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
        if relay is not None:
            try:
                relay.stdin.close()
                relay.wait(5)
            except Exception:
                relay.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

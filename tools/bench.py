#!/usr/bin/env python3
"""Performance baselines (milestone 8.6, DESIGN.md 4.15, docs/PERFORMANCE.md).

    python3 -I tools/bench.py record   --build-dir build/release --out docs/perf/baseline-<id>.json
    python3 -I tools/bench.py capacity --build-dir build/release --out docs/perf/baseline-<id>.json
    python3 -I tools/bench.py compare  docs/perf/baseline-<id>.json new.json     # exit 1 on a regression
    python3 -I tools/bench.py check    docs/perf/baseline-<id>.json              # validate the schema
    python3 -I tools/bench.py report   docs/perf/baseline-<id>.json              # Markdown tables
    python3 -I tools/bench.py record   --build-dir build/release --web build/web-pack-threads/runtime \
                                       --backends webgpu webgl2 --out docs/perf/baseline-<id>.json

`record` runs examples/bench (`cuervo --game examples/bench --bench N`) at the
1k/10k/50k/100k workloads on a backend, several times, and keeps the median of
the runs: frame-time statistics, simulation time per tick, peak resident
memory, plus the state hash at a fixed tick (a determinism cross-check across
thread counts and backends) and the export sizes. `capacity` searches for the
largest particle count that holds 60 fps. Both merge into one JSON file per
machine, keyed by build type, so a baseline file holds debug and release.
`compare` checks two such files against THRESHOLDS below.

`--web <runtime dir>` (milestone 10.4) runs the same workloads in headless
Chrome on the real GPU instead (tools/web/smoke_test.mjs --gpu): the web
runtime in that directory plays a pack of the project, on WebGPU or on the
WebGL2 fallback (`--backends webgpu webgl2`), and the results go under
`builds.web`. `--build-dir` then only provides cuervo-tools (to pack).

Standard library only; POSIX-only parts (peak RSS) are skipped on Windows.
"""

import argparse
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_VERSION = 1

# The agreed regression thresholds, in one place (docs/PERFORMANCE.md "Thresholds").
# Percentages are the allowed change against the baseline on the same machine id.
THRESHOLDS = {
    "sim_tick_mean_pct": 15.0,   # CPU only, the strict check
    "sim_tick_p95_pct": 25.0,    # CPU only, but a tail statistic
    "frame_p95_pct": 25.0,       # includes the GPU and the driver: looser
    "peak_rss_pct": 10.0,
    "export_size_pct": 5.0,
    "capacity_cpu_drop_pct": 15.0,    # fewer particles within the 16.67 ms sim-tick budget (CPU only: strict)
    "capacity_frame_drop_pct": 25.0,  # fewer particles within the frame budget (GPU and driver: loose)
    # Timings below this floor (ms) are noise: a percentage of nothing.
    "floor_ms": 0.05,
    # Frame-time cliffs (absolute, every record that has them): the slowest one-second window of a
    # run over the median one, worst run of a workload. A steady run stays near 1.
    "frame_cliff_ratio": 1.5,
}
BUDGET_MS = 1000.0 / 60.0
FAILED_MS = 1.0e9  # recorded for a probe whose run crashed or timed out
WORKLOADS = [1000, 10000, 50000, 100000]
# examples/bench starts at 50k and `more` steps 50k -> 100k -> 1k -> 10k -> 50k.
PRESSES = {1000: "5:more,15:more", 10000: "5:more,15:more,25:more", 50000: "", 100000: "5:more"}
HASH_TICK = 600
# The two capacity scenes (docs/PERFORMANCE.md "Scenes"): the project that holds each one.
SCENES = {"dense": "bench", "sparse": "bench_spread"}
STEPS_LINE = "var steps = [1000, 10000, 50000, 100000]"

BENCH_RE = re.compile(
    r"bench \| (?P<name>[^|]*)\| (?P<backend>[^|]*)\| (?P<res>\S+) (?P<msaa>\d+)x MSAA \| "
    r"(?P<particles>[\d.]+) avg particles \| (?P<frames>\d+) frames in (?P<secs>[\d.]+) s \| (?P<fps>[\d.]+) FPS \| "
    r"frame ms avg (?P<avg>[\d.]+) p50 (?P<p50>[\d.]+) p95 (?P<p95>[\d.]+) p99 (?P<p99>[\d.]+) max (?P<max>[\d.]+) \| "
    r"sim (?P<sim>[\d.]+) ms/tick \(p95 (?P<sim_p95>[\d.]+) max (?P<sim_max>[\d.]+), (?P<ticks>\d+) ticks\) \| "
    r"render CPU (?P<render>[\d.]+) ms/frame")
WINDOWS_RE = re.compile(r"bench windows \| (?P<count>\d+) x 1 s \| frame ms mean (?P<means>[\d. ]+?) \| "
                        r"worst / median (?P<ratio>[\d.]+)")
HASH_RE = re.compile(r"at tick (\d+) \(state hash ([0-9a-f]{16})\)")
DEVICE_RE = re.compile(r"(?:OpenGL|Vulkan|WebGPU) device: (.+)")
JOBS_RE = re.compile(r"Job system: (\d+) thread")


# --- parsing -----------------------------------------------------------------------------------

def parse_bench_line(text):
    """The numbers of the `bench |` log line, or None."""
    m = BENCH_RE.search(text)
    if not m:
        return None
    g = m.groupdict()
    out = {
        "backend": g["backend"].strip(), "resolution": g["res"], "msaa": int(g["msaa"]),
        "particles": float(g["particles"]), "frames": int(g["frames"]), "seconds": float(g["secs"]),
        "fps": float(g["fps"]),
        "frame_ms": {"mean": float(g["avg"]), "p50": float(g["p50"]), "p95": float(g["p95"]),
                     "p99": float(g["p99"]), "max": float(g["max"])},
        "sim_tick_ms": {"mean": float(g["sim"]), "p95": float(g["sim_p95"]), "max": float(g["sim_max"])},
        "sim_ticks": int(g["ticks"]), "render_cpu_ms": float(g["render"]),
    }
    w = WINDOWS_RE.search(text)
    if w:  # engines from milestone 10.4 on
        out["frame_windows_ms"] = [float(v) for v in w.group("means").split()]
        out["cliff_ratio"] = float(w.group("ratio"))
    return out


def parse_hash(text):
    m = HASH_RE.search(text)
    return (int(m.group(1)), m.group(2)) if m else None


def parse_device(text):
    m = DEVICE_RE.search(text)
    return m.group(1).strip() if m else ""


def parse_threads(text):
    m = JOBS_RE.search(text)
    return int(m.group(1)) if m else 0


def median_stats(samples):
    """Median of each statistic over several runs' parsed bench lines, plus the observed spread."""
    def med(values):
        return round(statistics.median(values), 4)
    out = {"runs": len(samples)}
    for key in ("particles", "frames", "fps", "render_cpu_ms", "sim_ticks"):
        out[key] = med([s[key] for s in samples])
    for group in ("frame_ms", "sim_tick_ms"):
        out[group] = {k: med([s[group][k] for s in samples]) for k in samples[0][group]}
    if all("cliff_ratio" in s for s in samples):
        out["cliff_ratio"] = round(max(s["cliff_ratio"] for s in samples), 3)  # the worst run
    out["spread"] = {
        "frame_p95_ms": [min(s["frame_ms"]["p95"] for s in samples), max(s["frame_ms"]["p95"] for s in samples)],
        "sim_tick_mean_ms": [min(s["sim_tick_ms"]["mean"] for s in samples),
                             max(s["sim_tick_ms"]["mean"] for s in samples)],
        "fps": [min(s["fps"] for s in samples), max(s["fps"] for s in samples)],
    }
    return out


# --- the capacity search (pure; the measurer is injected) ---------------------------------------

def search_capacity(measure, key, lo=10000, hi=2000000, resolution=0.05, budget_ms=BUDGET_MS):
    """Largest N in [lo, hi] with measure(N)[key] <= budget_ms: doubling, then bisection to `resolution`.

    `measure(n)` returns a dict of milliseconds: `key` is the median over the repeated runs and
    `key + "_runs"` (optional) lists every run's value. Returns
    {"particles": N, "at_least": bool, "below_min": bool, "range": [low, high], "probes": [...]}.
    N is the largest *measured* passing count, so it always meets the budget (0 when even `lo` fails);
    `at_least` is set when the upper bound passed, so N is a floor, not a limit. The range is the
    spread the repeated runs leave: `low` is the largest probed count at which every run passed,
    `high` the smallest at which every run failed (None when nothing failed): the true limit of a
    noisy machine lies between them.
    """
    probes = []

    def ok(n):
        m = measure(n)
        runs = [round(r, 3) for r in m.get(key + "_runs", [m[key]])]
        probes.append({"particles": n, key: round(m[key], 3), "runs": runs, "ok": m[key] <= budget_ms})
        return m[key] <= budget_ms

    def finish(n, at_least, below_min=False):
        low = max([p["particles"] for p in probes if max(p["runs"]) <= budget_ms] or [0])
        fail = [p["particles"] for p in probes if min(p["runs"]) > budget_ms]
        high = min(fail) if fail else None
        low = min(low, n) if n else 0
        if high is not None:
            high = max(high, n)
        return {"particles": n, "at_least": at_least, "below_min": below_min, "range": [low, high],
                "probes": probes}

    if not ok(lo):
        return finish(0, False, True)
    good, bad = lo, None
    while bad is None:
        if good >= hi:
            return finish(good, True)
        n = min(good * 2, hi)
        if ok(n):
            good = n
        else:
            bad = n
    while (bad - good) > resolution * good:
        mid = (good + bad) // 2
        if mid <= good or mid >= bad:
            break
        if ok(mid):
            good = mid
        else:
            bad = mid
    return finish(good, False)


# --- validation and regression comparison (pure) ---------------------------------------------------

def validate_record(rec):
    """Problems with a record's shape (empty list when it is valid)."""
    errors = []
    if not isinstance(rec, dict):
        return ["not an object"]
    if rec.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION}, not {rec.get('schema_version')!r}")
    for k in ("machine_id", "host", "builds"):
        if k not in rec:
            errors.append(f"missing '{k}'")
    for bt, b in (rec.get("builds") or {}).items():
        for k in ("commit", "build_type", "compiler", "backends"):
            if k not in b:
                errors.append(f"builds.{bt}: missing '{k}'")
        for be, d in (b.get("backends") or {}).items():
            where = f"builds.{bt}.backends.{be}"
            for k in ("gpu", "threads"):
                if k not in d:
                    errors.append(f"{where}: missing '{k}'")
            for wl, w in (d.get("workloads") or {}).items():
                if not wl.isdigit():
                    errors.append(f"{where}.workloads: key '{wl}' is not a particle count")
                for k in ("frame_ms", "sim_tick_ms", "particles", "state_hash", "state_hash_jobs1"):
                    if k not in w:
                        errors.append(f"{where}.workloads.{wl}: missing '{k}'")
                if "frame_ms" in w and not all(x in w["frame_ms"] for x in ("mean", "p50", "p95", "p99", "max")):
                    errors.append(f"{where}.workloads.{wl}.frame_ms: needs mean, p50, p95, p99, max")
                if w.get("state_hash") != w.get("state_hash_jobs1"):
                    errors.append(f"{where}.workloads.{wl}: state hash differs between job counts")
            for scene, cap in (d.get("capacity_60fps") or {}).items():
                for k in ("particles", "binding_limit", "frame_limit", "cpu_limit", "context", "probes"):
                    if k not in cap:
                        errors.append(f"{where}.capacity_60fps.{scene}: missing '{k}'")
                for k in ("frame_limit", "cpu_limit"):
                    if k in cap and not all(x in cap[k] for x in ("particles", "range", "at_least")):
                        errors.append(f"{where}.capacity_60fps.{scene}.{k}: needs particles, range, at_least")
    return errors


def determinism_problems(rec):
    """Hashes that differ between backends of the same build for the same workload."""
    problems = []
    for bt, b in rec.get("builds", {}).items():
        seen = {}
        for be, d in b.get("backends", {}).items():
            for wl, w in d.get("workloads", {}).items():
                h = w.get("state_hash")
                if wl in seen and seen[wl][1] != h:
                    problems.append(f"{bt}/{wl}: {seen[wl][0]} {seen[wl][1]} != {be} {h}")
                seen.setdefault(wl, (be, h))
    return problems


def _pct(old, new):
    return 0.0 if old == 0 else (new - old) / old * 100.0


def compare_records(base, new, thresholds=None, allow_machine_mismatch=False):
    """Rows of (scope, metric, old, new, delta %, limit, status) and whether anything failed.

    status is "ok", "FAIL" or "info" (a timing on a different machine id, or below the noise floor).
    Hashes and export sizes are checked regardless of the machine; timings, memory and capacity only
    on the same machine id.
    """
    t = dict(THRESHOLDS, **(thresholds or {}))
    rows, failed = [], False
    same_machine = base.get("machine_id") == new.get("machine_id")
    if not same_machine and not allow_machine_mismatch:
        rows.append(("machine", "machine_id", base.get("machine_id"), new.get("machine_id"), None, "same", "FAIL"))
        return rows, True

    def add(scope, metric, old, new_v, limit_pct, timing=True, floor=0.0, higher_is_worse=True):
        nonlocal failed
        d = _pct(old, new_v)
        bad = d > limit_pct if higher_is_worse else -d > limit_pct
        if (timing and not same_machine) or max(old, new_v) < floor:
            status = "info"
        else:
            status = "FAIL" if bad else "ok"
        failed |= status == "FAIL"
        rows.append((scope, metric, old, new_v, d, f"{'+' if higher_is_worse else '-'}{limit_pct:g}%", status))

    for bt, bb in base.get("builds", {}).items():
        nb = new.get("builds", {}).get(bt)
        if nb is None:
            continue
        for be, bd in bb.get("backends", {}).items():
            nd = nb.get("backends", {}).get(be)
            if nd is None:
                continue
            for wl, bw in bd.get("workloads", {}).items():
                nw = nd.get("workloads", {}).get(wl)
                if nw is None:
                    continue
                scope = f"{bt}/{be}/{int(wl) // 1000}k"
                same_hash = bw["state_hash"] == nw["state_hash"]
                failed |= not same_hash
                rows.append((scope, "state_hash", bw["state_hash"], nw["state_hash"], None, "equal",
                             "ok" if same_hash else "FAIL"))
                add(scope, "sim_tick_mean_ms", bw["sim_tick_ms"]["mean"], nw["sim_tick_ms"]["mean"],
                    t["sim_tick_mean_pct"], floor=t["floor_ms"])
                add(scope, "sim_tick_p95_ms", bw["sim_tick_ms"]["p95"], nw["sim_tick_ms"]["p95"],
                    t["sim_tick_p95_pct"], floor=t["floor_ms"])
                add(scope, "frame_p95_ms", bw["frame_ms"]["p95"], nw["frame_ms"]["p95"], t["frame_p95_pct"],
                    floor=t["floor_ms"])
                if "peak_rss_mb" in bw and "peak_rss_mb" in nw:
                    add(scope, "peak_rss_mb", bw["peak_rss_mb"], nw["peak_rss_mb"], t["peak_rss_pct"])
                if "cliff_ratio" in nw:  # an absolute limit: no baseline value needed
                    cliff = nw["cliff_ratio"] > t["frame_cliff_ratio"]
                    failed |= cliff
                    rows.append((scope, "frame_cliff_ratio", bw.get("cliff_ratio"), nw["cliff_ratio"], None,
                                 f"<={t['frame_cliff_ratio']:g}", "FAIL" if cliff else "ok"))
            for scene, bc in (bd.get("capacity_60fps") or {}).items():
                nc = (nd.get("capacity_60fps") or {}).get(scene)
                if not nc:
                    continue
                add(f"{bt}/{be}/{scene}", "capacity_cpu", bc["cpu_limit"]["particles"],
                    nc["cpu_limit"]["particles"], t["capacity_cpu_drop_pct"], higher_is_worse=False)
                add(f"{bt}/{be}/{scene}", "capacity_frame", bc["frame_limit"]["particles"],
                    nc["frame_limit"]["particles"], t["capacity_frame_drop_pct"], higher_is_worse=False)
        for name, old_sizes in bb.get("exports", {}).items():
            new_sizes = nb.get("exports", {}).get(name)
            if not isinstance(old_sizes, dict) or not isinstance(new_sizes, dict):
                continue
            for k, old in old_sizes.items():
                if isinstance(old, int) and isinstance(new_sizes.get(k), int):
                    add(f"{bt}/export/{name}", k, old, new_sizes[k], t["export_size_pct"], timing=False)
    for p in determinism_problems(new):
        failed = True
        rows.append(("determinism", "backends", "", p, None, "equal", "FAIL"))
    return rows, failed


def format_rows(rows):
    def cell(v):
        if isinstance(v, float):
            return f"{v:.3f}"
        return "" if v is None else str(v)
    head = ("scope", "metric", "baseline", "new", "delta", "limit", "status")
    table = [head] + [(r[0], r[1], cell(r[2]), cell(r[3]),
                       "" if r[4] is None else f"{r[4]:+.1f}%", str(r[5]), r[6]) for r in rows]
    widths = [max(len(str(row[i])) for row in table) for i in range(len(head))]
    return "\n".join("  ".join(str(c).ljust(widths[i]) for i, c in enumerate(row)).rstrip() for row in table)


# --- running the engine --------------------------------------------------------------------------

def run_child(cmd, env, timeout):
    """Runs a command; returns (combined output, exit code, the child's peak RSS in MB or None).

    The peak comes from wait4's per-child rusage (POSIX); Windows reports None.
    """
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if not hasattr(os, "wait4"):
        try:
            out, _ = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            return out.decode("utf-8", "replace") + "\n(timed out)", -1, None
        return out.decode("utf-8", "replace"), proc.returncode, None
    timer = None
    if timeout:
        import threading
        timer = threading.Timer(timeout, proc.kill)
        timer.start()
    try:
        out = proc.stdout.read()
        _, status, usage = os.wait4(proc.pid, 0)
    finally:
        proc.stdout.close()
        if timer:
            timer.cancel()
    rc = os.waitstatus_to_exitcode(status)
    # Reaped by wait4, so Popen must not wait again.
    proc.returncode = rc
    rss = usage.ru_maxrss / (1024.0 if sys.platform == "darwin" else 1.0) / 1024.0
    return out.decode("utf-8", "replace"), rc, round(rss, 1)


class Engine:
    def __init__(self, build_dir, backend, video=None, jobs=None):
        self.build_dir = Path(build_dir).resolve()
        exe = "cuervo.exe" if os.name == "nt" else "cuervo"
        self.runtime = self.build_dir / "runtime" / exe
        if not self.runtime.exists():
            raise SystemExit(f"bench: {self.runtime} not found; build the runtime target first")
        self.backend = backend
        self.jobs = jobs
        self.env = dict(os.environ)
        video = video or ("offscreen" if backend == "gl" and sys.platform.startswith("linux") else "auto")
        self.video = video
        if video == "offscreen":
            self.env["SDL_VIDEO_DRIVER"] = "offscreen"
        elif sys.platform.startswith("linux") and "WAYLAND_DISPLAY" not in self.env and "DISPLAY" not in self.env:
            self.env["WAYLAND_DISPLAY"] = "wayland-0"  # the reference machine's session

    def base(self, project, jobs=None):
        cmd = [str(self.runtime), "--game", str(project), "--backend", self.backend]
        j = jobs if jobs is not None else self.jobs
        if j:
            cmd += ["--jobs", str(j)]
        return cmd

    def bench(self, project, presses, duration, warmup, timeout=300):
        cmd = self.base(project) + ["--bench", str(duration), "--bench-warmup", str(warmup)]
        if presses:
            cmd += ["--press", presses]
        out, rc, rss = run_child(cmd, self.env, timeout)
        parsed = parse_bench_line(out)
        if rc != 0 or parsed is None:
            raise RuntimeError(f"bench run failed (exit {rc}): {' '.join(cmd)}\n{out[-1500:]}")
        parsed["_rss"] = rss
        parsed["_gpu"] = parse_device(out)
        parsed["_threads"] = parse_threads(out)
        return parsed

    def state_hash(self, project, presses, jobs, tick=HASH_TICK, timeout=300):
        """The state hash at a fixed tick: a scripted capture run, independent of the machine's speed."""
        env = dict(self.env, CUERVO_SCREENSHOT_TICK=str(tick))
        with tempfile.TemporaryDirectory(prefix="cuervo-bench-") as tmp:
            cmd = self.base(project, jobs) + ["--screenshot", str(Path(tmp) / "h.png")]
            if presses:
                cmd += ["--press", presses]
            out, rc, _ = run_child(cmd, env, timeout)
        h = parse_hash(out)
        if rc != 0 or h is None or h[0] != tick:
            raise RuntimeError(f"hash run failed (exit {rc}): {' '.join(cmd)}\n{out[-1500:]}")
        return h[1]


def find_node():
    """Node 22+ for tools/web/smoke_test.mjs: $CUERVO_NODE, emsdk's, then PATH."""
    if os.environ.get("CUERVO_NODE"):
        return os.environ["CUERVO_NODE"]
    emsdk = Path(os.environ.get("EMSDK") or Path.home() / "emsdk")
    found = sorted(emsdk.glob("node/*/bin/node"))
    return str(found[-1]) if found else (shutil.which("node") or "node")


def browser_version():
    for name in ("google-chrome-stable", "google-chrome", "chromium", "chromium-browser"):
        path = shutil.which(name)
        if path:
            try:
                return subprocess.run([path, "--version"], capture_output=True, text=True, timeout=30).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                return name
    return "unknown"


def web_smoke_command(node, dist, backend, args, seconds, expect):
    """The smoke_test.mjs command that runs `args` in the page on the real GPU, on `backend`."""
    cmd = [node, str(ROOT / "tools" / "web" / "smoke_test.mjs"), "--dir", str(dist), "--gpu",
           "--expect-backend", "webgpu" if backend == "webgpu" else "gl", "--seconds", str(int(seconds)),
           "--args", " ".join(args), "--expect", expect]
    if backend == "webgl2":
        cmd.append("--no-webgpu")  # the page then behaves as in a browser without WebGPU
    return cmd


class WebEngine:
    """Engine's interface for a web runtime directory in headless Chrome (milestone 10.4).

    Each project is packed (cuervo-tools pack --platform web) next to a copy of the runtime's page, and
    every run is one page load: the runtime arguments go in `?args=`. Peak memory is the browser's, so
    it is not recorded.
    """

    def __init__(self, build_dir, web_dir, backend, jobs=None):
        exe = "cuervo-tools.exe" if os.name == "nt" else "cuervo-tools"
        self.tool = Path(build_dir).resolve() / "tools" / "cuervo_tools" / exe
        if not self.tool.exists():
            raise SystemExit(f"bench: {self.tool} not found; --web packs projects with the build dir's cuervo-tools")
        self.web_dir = Path(web_dir).resolve()
        if not all((self.web_dir / f).exists() for f in ("index.html", "index.js", "index.wasm")):
            raise SystemExit(f"bench: {self.web_dir} is not a web runtime (index.html, .js, .wasm; "
                             "the web-pack or web-pack-threads preset)")
        self.backend = backend
        self.jobs = jobs
        self.video = "browser"
        self.node = find_node()
        self.env = dict(os.environ)
        self.work = Path(tempfile.mkdtemp(prefix="cuervo-bench-web-"))
        self.dists = {}

    def dist(self, project):
        key = str(Path(project).resolve())
        if key not in self.dists:
            d = self.work / f"d{len(self.dists)}"
            d.mkdir()
            for f in ("index.html", "index.js", "index.wasm"):
                shutil.copy2(self.web_dir / f, d / f)
            p = subprocess.run([str(self.tool), "pack", key, "--platform", "web", "-o", str(d / "game.pak")],
                               capture_output=True, text=True, timeout=600)
            if p.returncode != 0:
                raise RuntimeError(f"packing {key} failed:\n{(p.stderr or p.stdout)[-1500:]}")
            self.dists[key] = d
        return self.dists[key]

    def run(self, project, args, seconds, expect, timeout):
        cmd = web_smoke_command(self.node, self.dist(project), self.backend, args, seconds, expect)
        out, rc, _ = run_child(cmd, self.env, timeout)
        return cmd, out, rc

    def base(self, jobs=None):
        j = jobs if jobs is not None else self.jobs
        return ["--jobs", str(j)] if j else []

    def bench(self, project, presses, duration, warmup, timeout=300):
        args = self.base() + ["--bench", str(duration), "--bench-warmup", str(warmup)]
        if presses:
            args += ["--press", presses]
        cmd, out, rc = self.run(project, args, duration + warmup + 30, "bench |", timeout)
        parsed = parse_bench_line(out)
        if rc != 0 or parsed is None:
            raise RuntimeError(f"web bench run failed (exit {rc}): {' '.join(cmd)}\n{out[-1500:]}")
        parsed["_rss"] = None
        parsed["_gpu"] = parse_device(out)
        parsed["_threads"] = parse_threads(out)
        return parsed

    def state_hash(self, project, presses, jobs, tick=HASH_TICK, timeout=300):
        args = self.base(jobs) + ["--screenshot", "/tmp/h.ppm", "--screenshot-tick", str(tick)]
        if presses:
            args += ["--press", presses]
        cmd, out, rc = self.run(project, args, 120, "state hash", timeout)
        h = parse_hash(out)
        if rc != 0 or h is None or h[0] != tick:
            raise RuntimeError(f"web hash run failed (exit {rc}): {' '.join(cmd)}\n{out[-1500:]}")
        return h[1]


def make_engine(a, backend):
    if a.web:
        return WebEngine(a.build_dir, a.web, backend, a.jobs)
    return Engine(a.build_dir, backend, a.video, a.jobs)


def capacity_project(source, particles, dest):
    """A copy of examples/bench whose every step is `particles` (the bench script stays the one workload)."""
    shutil.copytree(source, dest, dirs_exist_ok=True)
    script = next((Path(dest) / "scripts").glob("*.plm"))
    text = script.read_text(encoding="utf-8")
    if STEPS_LINE not in text:
        raise RuntimeError(f"{script}: the steps line changed; update tools/bench.py")
    script.write_text(text.replace(STEPS_LINE, f"var steps = [{particles}, {particles}, {particles}, {particles}]"),
                      encoding="utf-8")
    return Path(dest)


# --- host / build facts ----------------------------------------------------------------------------

def cpu_name():
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def cmake_cache(build_dir):
    cache = {}
    try:
        for line in (Path(build_dir) / "CMakeCache.txt").read_text().splitlines():
            m = re.match(r"([A-Za-z_]+)(?::[A-Z]+)?=(.*)", line)
            if m:
                cache[m.group(1)] = m.group(2)
    except OSError:
        pass
    return cache


def compiler_version(path):
    try:
        out = subprocess.run([path, "--version"], capture_output=True, text=True, timeout=20).stdout
        return out.splitlines()[0].strip() if out else path
    except (OSError, subprocess.SubprocessError):
        return path or "unknown"


def emscripten_version(cache):
    """'emscripten 6.0.12' for a build configured with emscripten's toolchain file, else ''."""
    toolchain = cache.get("CMAKE_TOOLCHAIN_FILE", "")
    if "emscripten" not in toolchain.lower():
        return ""
    try:  # <emsdk>/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake
        version = (Path(toolchain).parents[3] / "emscripten-version.txt").read_text().strip().strip('"')
        return f"emscripten {version}"
    except (OSError, IndexError):
        return "emscripten"


def build_facts(build_dir):
    cache = cmake_cache(build_dir)
    return {
        "commit": git("rev-parse", "--short=12", "HEAD") + ("+dirty" if git("status", "--porcelain", "--", ".", ":!docs/perf") else ""),
        "build_type": cache.get("CMAKE_BUILD_TYPE", "unknown"),
        "preset": Path(build_dir).resolve().name,
        "compiler": emscripten_version(cache) or compiler_version(cache.get("CMAKE_CXX_COMPILER", "")),
    }


def host_facts():
    return {"cpu": cpu_name(), "logical_cpus": os.cpu_count(), "os": platform.platform(),
            "python": platform.python_version()}


def load_record(path):
    p = Path(path)
    return json.loads(p.read_text(encoding="utf-8")) if p.exists() else None


def merge_target(path, machine_id, build_dir, key=None):
    """The record being extended (or a new one) and this build's entry in it (`key`: its build type)."""
    rec = load_record(path) or {"schema_version": SCHEMA_VERSION, "machine_id": machine_id, "builds": {}}
    if rec.get("machine_id") != machine_id:
        raise SystemExit(f"bench: {path} is for machine '{rec.get('machine_id')}', not '{machine_id}'")
    rec["host"] = host_facts()
    rec["timestamp"] = int(time.time())
    facts = build_facts(build_dir)
    entry = rec["builds"].setdefault(key or facts["build_type"].lower(), {"backends": {}})
    entry.update(facts)
    return rec, entry


def save(path, rec):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(rec, indent=2, sort_keys=True) + "\n", encoding="utf-8")


# --- exports -----------------------------------------------------------------------------------

def export_sizes(build_dir, projects, work, engines, player_cache=None):
    """Player and pack sizes of `cuervo-tools export` for each project and engine ('full', 'stripped')."""
    exe = "cuervo-tools.exe" if os.name == "nt" else "cuervo-tools"
    runtime = "cuervo.exe" if os.name == "nt" else "cuervo"
    tool = Path(build_dir) / "tools" / "cuervo_tools" / exe
    if not tool.exists():
        return {"skipped": "cuervo-tools not built in this build dir"}
    result = {}
    for project in projects:
        flat = {}
        for engine in engines:
            out = Path(work) / f"{project}-{engine}"
            cmd = [str(tool), "export", str(ROOT / "examples" / project), "--platform",
                   "windows" if os.name == "nt" else "linux", "-o", str(out), "--engine", engine]
            if engine == "stripped" and player_cache:
                cmd += ["--player-cache", str(player_cache)]
            env = dict(os.environ)
            if engine == "full":  # this build's own runtime, not the tool's compiled-in default
                env["CUERVO_EXPORT_RUNTIME"] = str(Path(build_dir).resolve() / "runtime" / runtime)
            p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=3600)
            if p.returncode != 0:
                flat[f"{engine}_skipped"] = (p.stderr or p.stdout)[-200:]
                continue
            files = [f for f in out.iterdir() if f.is_file() and f.name != "game.pak"]
            player = max(files, key=lambda f: f.stat().st_size) if files else None
            pak = out / "game.pak"
            flat[f"{engine}_player_bytes"] = player.stat().st_size if player else 0
            flat[f"{engine}_pack_bytes"] = pak.stat().st_size if pak.exists() else 0
        result[project] = flat
    return result


# --- commands ---------------------------------------------------------------------------------------

def check_backends(a):
    web = {"webgpu", "webgl2"}
    if a.web and not set(a.backends) <= web:
        raise SystemExit("bench: --web measures the webgpu and webgl2 backends")
    if not a.web and set(a.backends) & web:
        raise SystemExit("bench: the webgpu and webgl2 backends need --web <web runtime dir>")


def web_target(a):
    """With --web: the record's `web` entry, described by the web build (one directory up from runtime/)."""
    rec, entry = merge_target(a.out, a.machine_id, Path(a.web).resolve().parent, key="web")
    entry["browser"] = browser_version()
    return rec, entry


def cmd_record(a):
    check_backends(a)
    rec, entry = web_target(a) if a.web else merge_target(a.out, a.machine_id, a.build_dir)
    project = ROOT / "examples" / "bench"
    for backend in a.backends:
        eng = make_engine(a, backend)
        data = entry["backends"].setdefault(backend, {})
        data["video_driver"] = eng.video
        data["bench"] = {"runs": a.runs, "warmup_s": a.warmup, "duration_s": a.duration}
        workloads = {}
        for n in a.workloads:
            samples = []
            for i in range(a.runs):
                s = eng.bench(project, PRESSES[n], a.duration, a.warmup)
                samples.append(s)
                print(f"  {backend} {n}: run {i + 1}/{a.runs} frame p95 {s['frame_ms']['p95']:.3f} ms, "
                      f"sim {s['sim_tick_ms']['mean']:.3f} ms/tick, {s['fps']:.0f} fps", flush=True)
            w = median_stats(samples)
            data["gpu"] = samples[0]["_gpu"]
            data["threads"] = samples[0]["_threads"]
            data["resolution"] = samples[0]["resolution"]
            data["msaa"] = samples[0]["msaa"]
            rss = [s["_rss"] for s in samples if s["_rss"] is not None]
            if rss:
                w["peak_rss_mb"] = round(statistics.median(rss), 1)
            w["state_hash"] = eng.state_hash(project, PRESSES[n], None)
            w["state_hash_jobs1"] = eng.state_hash(project, PRESSES[n], 1)
            w["hash_tick"] = HASH_TICK
            workloads[str(n)] = w
        data["workloads"] = workloads
    if not a.no_export and not a.web:  # web export sizes: docs/EXPORT.md "Sizes"
        with tempfile.TemporaryDirectory(prefix="cuervo-export-") as work:
            entry["exports"] = export_sizes(a.build_dir, ["shmup", "bench"], work, ["full", "stripped"],
                                            a.player_cache)
    save(a.out, rec)
    problems = determinism_problems(rec) + validate_record(rec)
    for p in problems:
        print("bench: " + p, file=sys.stderr)
    print(f"wrote {a.out}")
    return 1 if problems else 0


def cmd_capacity(a):
    check_backends(a)
    rec, entry = web_target(a) if a.web else merge_target(a.out, a.machine_id, a.build_dir)
    for backend in a.backends:
        eng = make_engine(a, backend)
        data = entry["backends"].setdefault(backend, {})
        for scene in a.scenes:
            result = capacity_for_scene(eng, scene, a, rec["host"], entry)
            data.setdefault("capacity_60fps", {})[scene] = result
            data.setdefault("gpu", result["context"]["gpu"])
            data.setdefault("threads", result["context"]["threads"])
            f, c = result["frame_limit"], result["cpu_limit"]
            print(f"{backend}/{scene}: {describe(result)} ({result['binding_limit']} binds; frame limit "
                  f"{describe(f)}, cpu limit {describe(c)})")
    save(a.out, rec)
    return 0


def describe(limit):
    """'920000 (range 900000 to 980000)', 'at least 4000000' ..."""
    lo, hi = limit["range"]
    text = f"{'at least ' if limit['at_least'] else ''}{limit['particles']}"
    if not limit["at_least"] and hi is not None:
        text += f" (range {lo} to {hi})"
    return text


def capacity_for_scene(eng, scene, a, host, entry):
    base_project = ROOT / "examples" / SCENES[scene]
    cache = {}
    with tempfile.TemporaryDirectory(prefix="cuervo-cap-") as tmp:
        info = {}

        def measure(n):
            if n in cache:
                return cache[n]
            proj = capacity_project(base_project, n, Path(tmp) / f"p{n}")
            try:
                samples = [eng.bench(proj, "", a.duration, a.warmup) for _ in range(a.runs)]
            except RuntimeError as e:  # a crash or timeout at this load counts as a failed probe
                print(f"  {eng.backend}/{scene} probe {n}: FAILED ({str(e).splitlines()[0][:80]})", flush=True)
                runs = [FAILED_MS] * a.runs
                cache[n] = {"frame_p95": FAILED_MS, "frame_p95_runs": runs, "sim_tick_p95": FAILED_MS,
                            "sim_tick_p95_runs": runs, "fps": 0.0, "particles": 0.0}
                return cache[n]
            info.update(gpu=samples[0]["_gpu"], threads=samples[0]["_threads"], resolution=samples[0]["resolution"],
                        msaa=samples[0]["msaa"])
            m = {"frame_p95": statistics.median(s["frame_ms"]["p95"] for s in samples),
                 "frame_p95_runs": [s["frame_ms"]["p95"] for s in samples],
                 "sim_tick_p95": statistics.median(s["sim_tick_ms"]["p95"] for s in samples),
                 "sim_tick_p95_runs": [s["sim_tick_ms"]["p95"] for s in samples],
                 "fps": statistics.median(s["fps"] for s in samples),
                 "particles": statistics.median(s["particles"] for s in samples)}
            print(f"  {eng.backend}/{scene} probe {n}: frame p95 {m['frame_p95']:.2f} ms, sim tick p95 "
                  f"{m['sim_tick_p95']:.2f} ms, {m['fps']:.0f} fps, {m['particles']:.0f} live", flush=True)
            cache[n] = m
            return m
        frame = search_capacity(measure, "frame_p95", a.min, a.max, a.resolution)
        cpu = search_capacity(measure, "sim_tick_p95", a.min, a.max, a.resolution)
    binding = "frame" if frame["particles"] <= cpu["particles"] else "sim"
    bound = frame if binding == "frame" else cpu

    def limit(r):
        return {"particles": r["particles"], "at_least": r["at_least"], "range": r["range"]}
    return {
        "scene": scene, "project": SCENES[scene],
        "particles": bound["particles"], "at_least": bound["at_least"], "range": bound["range"],
        "binding_limit": binding,
        "frame_limit": limit(frame), "cpu_limit": limit(cpu),
        "budget_ms": round(BUDGET_MS, 3),
        "context": {"backend": eng.backend, "build_type": entry["build_type"], "gpu": info["gpu"],
                    "cpu": host["cpu"], "threads": info["threads"], "resolution": info["resolution"],
                    "msaa": info["msaa"], "video_driver": eng.video},
        "method": {"runs_per_probe": a.runs, "warmup_s": a.warmup, "duration_s": a.duration,
                   "resolution": a.resolution, "min": a.min, "max": a.max,
                   "statistic": "p95 over the steady-state window, median of the runs"},
        "probes": [{"particles": n, "live_particles": round(m["particles"]), "fps": round(m["fps"], 1),
                    "frame_p95_ms": round(m["frame_p95"], 3), "frame_p95_runs": m["frame_p95_runs"],
                    "sim_tick_p95_ms": round(m["sim_tick_p95"], 3), "sim_tick_p95_runs": m["sim_tick_p95_runs"]}
                   for n, m in sorted(cache.items())],
    }


def cmd_compare(a):
    base, new = load_record(a.baseline), load_record(a.new)
    if base is None or new is None:
        print("bench: file not found", file=sys.stderr)
        return 2
    bad = False
    for name, r in (("baseline", base), ("new", new)):
        for e in validate_record(r):
            print(f"bench: {name}: {e}", file=sys.stderr)
            bad = True
    if bad:
        return 2
    overrides = {}
    if a.scale != 1.0:  # loosen every percentage threshold (a smoke test, a noisy machine)
        overrides = {k: v * a.scale for k, v in THRESHOLDS.items() if k.endswith("_pct")}
    rows, failed = compare_records(base, new, overrides, a.allow_machine_mismatch)
    print(format_rows(rows))
    print("RESULT: " + ("REGRESSION" if failed else "ok"))
    return 1 if failed else 0


def cmd_check(a):
    rec = load_record(a.file)
    if rec is None:
        print("bench: file not found", file=sys.stderr)
        return 2
    errors = validate_record(rec) + determinism_problems(rec)
    for e in errors:
        print("bench: " + e, file=sys.stderr)
    return 1 if errors else 0


def cmd_report(a):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import bench_report
    return bench_report.main(["bench_report", a.file])


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(s):
        s.add_argument("--build-dir", required=True, help="a configured and built tree (runtime + cuervo-tools)")
        s.add_argument("--out", required=True, help="baseline JSON to create or extend")
        s.add_argument("--machine-id", default=os.environ.get("CUERVO_BENCH_MACHINE", "5800x3d-rtx3060ti-linux"))
        s.add_argument("--backends", nargs="+", default=None, choices=["gl", "vulkan", "webgpu", "webgl2"],
                       help="default: gl, or webgpu with --web")
        s.add_argument("--web", metavar="DIR", help="a web runtime directory (index.html/.js/.wasm): measure in "
                       "headless Chrome on the real GPU; results go under builds.web")
        s.add_argument("--video", choices=["offscreen", "auto"], help="SDL video driver (default: offscreen for gl)")
        s.add_argument("--jobs", type=int, help="job threads (default: the engine's auto)")

    r = sub.add_parser("record", help="measure the 1k/10k/50k/100k workloads")
    common(r)
    r.add_argument("--workloads", nargs="+", type=int, default=WORKLOADS, choices=WORKLOADS)
    r.add_argument("--runs", type=int, default=3)
    r.add_argument("--warmup", type=float, default=4.0)
    r.add_argument("--duration", type=float, default=6.0)
    r.add_argument("--no-export", action="store_true", help="skip the export sizes")
    r.add_argument("--player-cache", help="cuervo-tools player cache for the stripped player")
    r.set_defaults(fn=cmd_record)

    c = sub.add_parser("capacity", help="largest particle count holding 60 fps")
    common(c)
    c.add_argument("--runs", type=int, default=3)
    c.add_argument("--warmup", type=float, default=4.0)
    c.add_argument("--duration", type=float, default=4.0)
    c.add_argument("--min", type=int, default=10000)
    c.add_argument("--max", type=int, default=2000000)
    c.add_argument("--scenes", nargs="+", default=["dense", "sparse"], choices=list(SCENES))
    c.add_argument("--resolution", type=float, default=0.05)
    c.set_defaults(fn=cmd_capacity)

    m = sub.add_parser("compare", help="exit 1 when NEW regresses against BASELINE")
    m.add_argument("baseline")
    m.add_argument("new")
    m.add_argument("--scale", type=float, default=1.0, help="multiply every percentage threshold")
    m.add_argument("--allow-machine-mismatch", action="store_true",
                   help="compare hashes and sizes across machines (timings become informational)")
    m.set_defaults(fn=cmd_compare)

    k = sub.add_parser("check", help="validate a baseline file")
    k.add_argument("file")
    k.set_defaults(fn=cmd_check)

    rp = sub.add_parser("report", help="Markdown tables from a baseline file")
    rp.add_argument("file")
    rp.set_defaults(fn=cmd_report)

    a = p.parse_args(argv)
    if hasattr(a, "backends") and a.backends is None:
        a.backends = ["webgpu"] if a.web else ["gl"]
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())

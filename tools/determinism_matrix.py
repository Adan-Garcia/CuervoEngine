#!/usr/bin/env python3
"""Cross-hardware determinism matrix (milestone 11.1a, DESIGN.md 4.1, docs/PERFORMANCE.md).

    python3 -I tools/determinism_matrix.py host                                  # one JSON line about this machine
    python3 -I tools/determinism_matrix.py run --runtime build/release/runtime/cuervo \
                                               --out results/                    # every example x variant -> JSONL
    python3 -I tools/determinism_matrix.py web --tools build/host-tools/tools/cuervo_tools/cuervo-tools \
                                               --out results/                    # Chrome: WebGPU + WebGL2
    python3 -I tools/determinism_matrix.py golden results/*.jsonl \
                                               --out tests/determinism/golden.json   # reference hashes
    python3 -I tools/determinism_matrix.py report --golden tests/determinism/golden.json results/*.jsonl

`run` plays each reference example headless with the scripted input of
tests/CMakeLists.txt's examples_sweep (the same `--press` strings) to tick 240 and reads
the state hash the runtime prints there (`... at tick 240 (state hash <hex>)`), once per
variant: backend (gl, vulkan when the machine has it), `--jobs` 1 and 4, and `--simd off`.
The state hash does not depend on the backend, the job count or SIMD (DESIGN.md 4.1), so
every variant on every machine must print the same hash. `golden` takes the hashes the
reference machine produced; `report` checks every result against them and prints the
matrix as Markdown. Python standard library only.
"""
import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

TICK = 240
HASH_RE = re.compile(r"at tick (\d+) \(state hash ([0-9a-f]+)\)")
DEVICE_RE = re.compile(r"(OpenGL device|Vulkan device|WebGPU device): ([^\n]*)")

# name -> --press (mirrors tests/CMakeLists.txt, examples_sweep); `lantern_cave` needs physics.
_anim = ",".join(f"{t}:right" for t in range(20, 91)) + ",60:jump,150:jump,200:jump"
EXAMPLES = {
    "anim_character": _anim,
    "bench": "20:more,60:more,100:more",
    "editor_level": "30:shoot,31:shoot,90:shoot,120:shoot",
    "garden": "20:more,40:more,60:more",
    "node_level": "",
    "shmup": "30:ui_accept,90:shoot,91:shoot,92:shoot,93:shoot,150:bomb",
    "tictak": "10:ui_down,20:ui_accept,40:ui_right,50:ui_accept,70:ui_down,80:ui_accept,100:pause,110:ui_accept",
    "tutorial": "10:ui_accept,40:ui_accept,80:ui_right,120:ui_accept",
    "ui_menus": "5:ui_down,10:ui_down,15:ui_right,20:ui_accept,30:ui_tab_next,33:ui_down,36:ui_accept",
    "lantern_cave": "10-114:move_right,60:jump,124:jump,138-240:move_right",
}


def sh(cmd, timeout=20):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def host_info():
    """Who ran this: CPU, ISA extensions, GPUs, OS. Best effort, never fails."""
    info = {"os": platform.system(), "machine": platform.machine(), "kernel": platform.release(),
            "hostname": platform.node(), "cpu": platform.processor() or "", "isa": [], "gpus": []}
    if Path("/etc/os-release").exists():
        m = re.search(r'PRETTY_NAME="?([^"\n]*)', Path("/etc/os-release").read_text())
        info["distro"] = m.group(1) if m else ""
    if Path("/proc/cpuinfo").exists():
        text = Path("/proc/cpuinfo").read_text(errors="replace")
        m = re.search(r"model name\s*:\s*(.*)", text)
        if m:
            info["cpu"] = m.group(1).strip()
        flags = re.search(r"flags\s*:\s*(.*)", text)
        have = set(flags.group(1).split()) if flags else set()
        info["isa"] = [f for f in ("sse2", "sse4_2", "avx", "avx2", "fma", "fma4", "bmi2", "avx512f") if f in have]
    if platform.system() == "Windows":
        _windows_host_info(info)
    if shutil.which("lspci"):
        info["gpus"] = [re.sub(r"^\S+ [^:]*: ", "", line) for line in sh(["lspci"]).splitlines()
                        if re.search(r"VGA|3D controller|Display", line)]
    info["python"] = platform.python_version()
    return info


# kernel32 IsProcessorFeaturePresent codes for the x86 extensions that matter here; the API has
# no code for FMA or BMI2, so those are not recorded on Windows.
_WIN_ISA = (("sse2", 10), ("sse3", 13), ("ssse3", 36), ("sse4_1", 37), ("sse4_2", 38),
            ("avx", 39), ("avx2", 40), ("avx512f", 41))


def _windows_host_info(info):
    import ctypes
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as k:
            info["cpu"] = winreg.QueryValueEx(k, "ProcessorNameString")[0].strip()
    except OSError:
        pass
    present = ctypes.windll.kernel32.IsProcessorFeaturePresent
    info["isa"] = [name for name, code in _WIN_ISA if present(code)]
    info["isa_note"] = "from IsProcessorFeaturePresent: FMA and BMI2 are not reported"
    names = sh(["powershell", "-NoProfile", "-Command",
                "Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name }"])
    info["gpus"] = [n.strip() for n in names.splitlines() if n.strip()]
    ver = sh(["cmd", "/c", "ver"]).strip()
    if ver:
        info["distro"] = ver


def host_id(info):
    cpu = re.sub(r"[^A-Za-z0-9]+", "-", info.get("cpu", "cpu")).strip("-")
    return f"{info.get('hostname', 'host')}_{cpu}"[:80]


def run_variant(runtime, example, press, backend, jobs, simd_off, root, timeout):
    env = dict(os.environ, CUERVO_SCREENSHOT_TICK=str(TICK))
    headless = platform.system() == "Linux" and not (env.get("DISPLAY") or env.get("WAYLAND_DISPLAY"))
    if platform.system() != "Windows" and (backend == "gl" or headless):
        # GL always runs offscreen; Vulkan too when there is no display (Mesa drivers have a headless surface).
        env.setdefault("SDL_VIDEO_DRIVER", "offscreen")
    out = Path(root) / f"{example}_{backend}_j{jobs}_{'nosimd' if simd_off else 'simd'}.ppm"
    cmd = [str(runtime), "--game", f"examples/{example}", "--backend", backend, "--no-audio",
           "--jobs", str(jobs), "--screenshot", str(out)]
    if press:
        cmd += ["--press", press]
    if simd_off:
        cmd += ["--simd", "off"]
    start = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        text, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        text, code = f"timeout after {timeout}s\n{(e.stdout or b'').decode(errors='replace')}", -1
    m = HASH_RE.search(text)
    dev = DEVICE_RE.search(text)
    rec = {"example": example, "backend": backend, "jobs": jobs, "simd": not simd_off,
           "seconds": round(time.time() - start, 1), "exit": code,
           "device": dev.group(2).strip() if dev else "",
           "tick": int(m.group(1)) if m else None, "hash": m.group(2) if m else None}
    if not m:
        rec["status"] = "no-hash"
        rec["tail"] = "\n".join(text.strip().splitlines()[-4:])
    else:
        rec["status"] = "ok" if code == 0 else "exit-nonzero"
    return rec


def cmd_host(_args):
    print(json.dumps(host_info()))


def cmd_run(args):
    runtime = Path(args.runtime)
    if not runtime.exists():
        sys.exit(f"runtime not found: {runtime}")
    info = host_info()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    scratch = out_dir / "scratch"
    scratch.mkdir(exist_ok=True)
    label = args.label or host_id(info)
    results = out_dir / f"results-{label}-{'+'.join(args.backends)}.jsonl"
    names = args.examples or list(EXAMPLES)
    variants = []
    for backend in args.backends:
        for jobs in (1, 4):
            variants.append((backend, jobs, False))
        variants.append((backend, 4, True))  # SIMD off: the scalar paths must agree too
    with results.open("w") as f:
        f.write(json.dumps({"host": label, "info": info, "tick": TICK}) + "\n")
        for name in names:
            if name not in EXAMPLES or not Path("examples", name).is_dir():
                print(f"skip {name}: not in this checkout", file=sys.stderr)
                continue
            for backend, jobs, simd_off in variants:
                rec = run_variant(runtime, name, EXAMPLES[name], backend, jobs, simd_off, scratch, args.timeout)
                rec["host"] = label
                f.write(json.dumps(rec) + "\n")
                f.flush()
                print(f"{name:15} {backend:7} jobs={jobs} simd={'off' if simd_off else 'on '} "
                      f"{rec['status']:12} {rec['hash'] or '-'}")
    shutil.rmtree(scratch, ignore_errors=True)
    print(f"wrote {results}")


def cmd_web(args):
    """Each example exported for the web and run in headless Chrome (tools/web/smoke_test.mjs)
    on WebGPU and on the WebGL2 fallback; the hashes go in the same JSONL as native runs."""
    info = host_info()
    firefox = args.firefox
    if firefox:
        exe = args.browser or shutil.which("firefox") or "firefox"
        info["browser"] = sh([exe, "--version"]).strip()
    else:
        info["browser"] = sh([args.browser, "--version"]).strip() if args.browser else "chrome (smoke_test default)"
    label = args.label or f"{host_id(info)}-{'firefox' if firefox else 'chrome'}"
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    scratch = out_dir / "web-scratch"
    results = out_dir / f"results-{label}-web.jsonl"
    names = args.examples or list(EXAMPLES)
    with results.open("w") as f:
        f.write(json.dumps({"host": label, "info": info, "tick": TICK}) + "\n")
        for name in names:
            if name not in EXAMPLES or not Path("examples", name).is_dir():
                continue
            dist = scratch / name
            shutil.rmtree(dist, ignore_errors=True)
            ex = subprocess.run([args.tools, "export", f"examples/{name}", "--platform", "web", "-o", str(dist)],
                                capture_output=True, text=True)
            if ex.returncode != 0:
                rec = {"host": label, "example": name, "backend": "web", "jobs": 1, "simd": True, "device": "",
                       "tick": None, "hash": None, "status": "export-failed", "tail": ex.stderr[-300:]}
                f.write(json.dumps(rec) + "\n")
                print(f"{name:15} export failed")
                continue
            modes = (("webgl2", []),) if firefox else (("webgpu", []), ("webgl2", ["--no-webgpu"]))
            for mode, flags in modes:
                run_args = "--screenshot /tmp/w.ppm" + (f" --press {EXAMPLES[name]}" if EXAMPLES[name] else "")
                if firefox:  # no WebGPU in Firefox on Linux: the WebGL2 fallback
                    cmd = ["node", args.firefox_smoke, "--dir", str(dist), "--args", run_args,
                           "--expect", f"at tick {TICK}"]
                else:
                    cmd = ["node", args.smoke, "--dir", str(dist), "--args", run_args,
                           "--expect-backend", "webgpu" if mode == "webgpu" else "gl",
                           "--expect", f"at tick {TICK}"] + flags
                if args.browser:
                    cmd += ["--browser", args.browser]
                if not args.software and not firefox:
                    cmd.append("--gpu")  # the machine's GPU: WebGPU through Dawn on Vulkan
                start = time.time()
                try:
                    p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
                    text, code = p.stdout + p.stderr, p.returncode
                except subprocess.TimeoutExpired:
                    text, code = "timeout", -1
                m = HASH_RE.search(text)
                dev = DEVICE_RE.search(text)
                rec = {"host": label, "example": name, "backend": mode, "jobs": 1, "simd": True,
                       "seconds": round(time.time() - start, 1), "exit": code,
                       "device": dev.group(2).strip() if dev else "",
                       "tick": int(m.group(1)) if m else None, "hash": m.group(2) if m else None,
                       "status": "ok" if m and code == 0 else "no-hash"}
                if not m:
                    rec["tail"] = "\n".join(text.strip().splitlines()[-4:])
                f.write(json.dumps(rec) + "\n")
                f.flush()
                print(f"{name:15} {mode:7} {rec['status']:8} {rec['hash'] or '-'}")
    shutil.rmtree(scratch, ignore_errors=True)
    print(f"wrote {results}")


def load(paths):
    hosts, rows = {}, []
    for p in paths:
        for line in Path(p).read_text().splitlines():
            if not line.strip():
                continue
            d = json.loads(line)
            if "info" in d:
                hosts[d["host"]] = d["info"]
            else:
                rows.append(d)
    return hosts, rows


def cmd_golden(args):
    """Reference hashes: the value every ok result of the given files agrees on."""
    _, rows = load(args.results)
    by = {}
    for r in rows:
        if r.get("status") == "ok":
            by.setdefault(r["example"], set()).add(r["hash"])
    bad = {k: sorted(v) for k, v in by.items() if len(v) != 1}
    if bad:
        sys.exit(f"the reference results disagree with themselves (fix that first): {json.dumps(bad)}")
    golden = {"tick": TICK, "hashes": {k: next(iter(v)) for k, v in sorted(by.items())}}
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(golden, indent=2) + "\n")
    print(f"wrote {args.out}: {len(golden['hashes'])} examples")


def cmd_report(args):
    hosts, rows = load(args.results)
    golden = json.loads(Path(args.golden).read_text())["hashes"]
    by_host = {}
    for r in rows:
        by_host.setdefault(r["host"], []).append(r)
    print("# Determinism matrix\n")
    print(f"Every example at tick {TICK} must print the golden state hash on every row.\n")
    print("| Host | CPU | ISA | GPU / device | Backends | Variants | Examples | Result |")
    print("|---|---|---|---|---|---|---|---|")
    failures = 0
    for host, rs in sorted(by_host.items()):
        info = hosts.get(host, {})
        ok = [r for r in rs if r.get("status") == "ok" and r["hash"] == golden.get(r["example"])]
        mism = [r for r in rs if r.get("status") == "ok" and r["hash"] != golden.get(r["example"])]
        miss = [r for r in rs if r.get("status") != "ok"]
        failures += len(mism)
        devices = sorted({r["device"] for r in rs if r["device"]})
        backends = sorted({r["backend"] for r in rs if r.get("status") == "ok"})
        result = f"{len(ok)}/{len(rs)} match"
        if mism:
            result += f", **{len(mism)} MISMATCH**"
        if miss:
            result += f", {len(miss)} did not run"
        print(f"| {host} | {info.get('cpu', '')} | {' '.join(info.get('isa', []))} | "
              f"{'; '.join(devices) or ', '.join(info.get('gpus', []))} | {', '.join(backends)} | "
              f"{len({(r['jobs'], r['simd']) for r in rs})} | {len({r['example'] for r in rs})} | {result} |")
    for host, rs in sorted(by_host.items()):
        for r in rs:
            if r.get("status") == "ok" and r["hash"] != golden.get(r["example"]):
                print(f"\nMISMATCH {host}: {r['example']} {r['backend']} jobs={r['jobs']} simd={r['simd']}: "
                      f"{r['hash']} != {golden.get(r['example'])}")
    sys.exit(1 if failures else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("host").set_defaults(fn=cmd_host)
    r = sub.add_parser("run")
    r.add_argument("--runtime", required=True)
    r.add_argument("--out", default="results")
    r.add_argument("--label", help="host label in the results (default: hostname + CPU)")
    r.add_argument("--backends", nargs="+", default=["gl"], help="gl (default), vulkan")
    r.add_argument("--examples", nargs="*")
    r.add_argument("--timeout", type=int, default=300, help="seconds per run")
    r.set_defaults(fn=cmd_run)
    w = sub.add_parser("web")
    w.add_argument("--tools", required=True, help="cuervo-tools; its web export needs the web-pack runtime built")
    w.add_argument("--smoke", default="tools/web/smoke_test.mjs")
    w.add_argument("--browser", help="Chrome/Chromium binary (default: smoke_test's search)")
    w.add_argument("--out", default="results")
    w.add_argument("--firefox", action="store_true", help="use tools/web/firefox_smoke.mjs (WebGL2 only)")
    w.add_argument("--firefox-smoke", default="tools/web/firefox_smoke.mjs")
    w.add_argument("--software", action="store_true", help="SwiftShader instead of the machine's GPU")
    w.add_argument("--label")
    w.add_argument("--examples", nargs="*")
    w.add_argument("--timeout", type=int, default=300)
    w.set_defaults(fn=cmd_web)
    g = sub.add_parser("golden")
    g.add_argument("results", nargs="+")
    g.add_argument("--out", required=True)
    g.set_defaults(fn=cmd_golden)
    p = sub.add_parser("report")
    p.add_argument("results", nargs="+")
    p.add_argument("--golden", required=True)
    p.set_defaults(fn=cmd_report)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()

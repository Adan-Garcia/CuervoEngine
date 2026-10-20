#!/usr/bin/env python3
"""Markdown tables from a baseline file (docs/PERFORMANCE.md), `tools/bench.py report FILE`.

The tables in docs/PERFORMANCE.md are pasted from this output, so the numbers in the page are the
numbers in docs/perf/baseline-<machine>.json. Standard library only.
"""

import json
import sys


def fmt_ms(v):
    return f"{v:.3f}" if v < 10 else f"{v:.2f}"


def fmt_n(n):
    return f"{n:,}".replace(",", " ")


def limit_text(lim):
    lo, hi = lim["range"]
    if lim["at_least"]:
        return f"at least {fmt_n(lim['particles'])}"
    if hi is None:
        return f"{fmt_n(lim['particles'])}"
    return f"{fmt_n(lim['particles'])} ({fmt_n(lo)} to {fmt_n(hi)})"


def workload_table(rec):
    rows = ["| Build | Backend | Particles | FPS | frame p50 / p95 / p99 / max (ms) | sim ms/tick (mean / p95) | "
            "peak RSS (MB) | state hash |",
            "|---|---|---|---|---|---|---|---|"]
    for bt in sorted(rec["builds"]):
        b = rec["builds"][bt]
        for be in sorted(b["backends"]):
            for wl, w in sorted(b["backends"][be].get("workloads", {}).items(), key=lambda kv: int(kv[0])):
                f, s = w["frame_ms"], w["sim_tick_ms"]
                rows.append(f"| {bt} | {be} | {int(wl) // 1000}k | {w['fps']:.0f} | {fmt_ms(f['p50'])} / "
                            f"{fmt_ms(f['p95'])} / {fmt_ms(f['p99'])} / {fmt_ms(f['max'])} | "
                            f"{fmt_ms(s['mean'])} / {fmt_ms(s['p95'])} | {w.get('peak_rss_mb', '')} | "
                            f"`{w['state_hash']}` |")
    return "\n".join(rows)


def capacity_table(rec):
    rows = ["| Scene | Build | Backend | Particles at 60 fps (range of the runs) | Binds | Frame-time limit | "
            "CPU limit (sim tick p95) |",
            "|---|---|---|---|---|---|---|"]
    for scene in ("dense", "sparse"):
        for bt in sorted(rec["builds"]):
            b = rec["builds"][bt]
            for be in sorted(b["backends"]):
                cap = (b["backends"][be].get("capacity_60fps") or {}).get(scene)
                if not cap:
                    continue
                rows.append(f"| {scene} | {bt} | {be} | **{limit_text(cap)}** | {cap['binding_limit']} | "
                            f"{limit_text(cap['frame_limit'])} | {limit_text(cap['cpu_limit'])} |")
    return "\n".join(rows)


def context_table(rec):
    rows = ["| Build | Backend | GPU / driver | Resolution | MSAA | Threads | Video driver |",
            "|---|---|---|---|---|---|---|"]
    for bt in sorted(rec["builds"]):
        b = rec["builds"][bt]
        for be in sorted(b["backends"]):
            d = b["backends"][be]
            rows.append(f"| {bt} ({b['compiler']}) | {be} | {d['gpu']} | {d.get('resolution', '')} | "
                        f"{d.get('msaa', '')}x | {d['threads']} | {d.get('video_driver', '')} |")
    return "\n".join(rows)


def export_table(rec):
    rows = ["| Build | Project | Full player | Stripped player | Pack |", "|---|---|---|---|---|"]
    for bt in sorted(rec["builds"]):
        for name, e in sorted((rec["builds"][bt].get("exports") or {}).items()):
            if not isinstance(e, dict) or "full_player_bytes" not in e:
                continue
            rows.append(f"| {bt} | {name} | {e['full_player_bytes'] / 1e6:.2f} MB | "
                        f"{e.get('stripped_player_bytes', 0) / 1e6:.2f} MB | {e['full_pack_bytes'] / 1e6:.2f} MB |")
    return "\n".join(rows)


def main(argv):
    rec = json.load(open(argv[1], encoding="utf-8"))
    for title, fn in (("Context", context_table), ("Workloads", workload_table), ("Capacity", capacity_table),
                      ("Export sizes", export_table)):
        print(f"### {title}\n\n{fn(rec)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

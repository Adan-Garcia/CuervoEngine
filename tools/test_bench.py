#!/usr/bin/env python3
"""Unit tests for tools/bench.py (run by ctest as bench_compare_unit). Synthetic data only: no GPU, any OS."""

import copy
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench  # noqa: E402

LINE = ("[info ] x.cpp:424: bench | bench | OpenGL | 1280x720 4x MSAA | 99443 avg particles | 596 frames in 3.00 s | "
        "198.7 FPS | frame ms avg 5.034 p50 4.482 p95 6.439 p99 6.831 max 9.287 | "
        "sim 1.771 ms/tick (p95 2.100 max 2.500, 180 ticks) | render CPU 4.456 ms/frame")


def workload(frame_p95=6.0, sim_mean=1.0, sim_p95=1.2, h="00000000000000aa", rss=250.0):
    return {"frame_ms": {"mean": 4.0, "p50": 4.0, "p95": frame_p95, "p99": 7.0, "max": 8.0},
            "sim_tick_ms": {"mean": sim_mean, "p95": sim_p95, "max": 2.0}, "particles": 1000.0,
            "state_hash": h, "state_hash_jobs1": h, "peak_rss_mb": rss}


def limit(n, at_least=False):
    return {"particles": n, "at_least": at_least, "range": [int(n * 0.95), int(n * 1.05)]}


def capacity(cpu=500000, frame=100000):
    return {"particles": min(cpu, frame), "binding_limit": "frame", "frame_limit": limit(frame),
            "cpu_limit": limit(cpu), "context": {}, "probes": []}


def record(machine="m1", **kw):
    return {"schema_version": bench.SCHEMA_VERSION, "machine_id": machine, "host": {},
            "builds": {"release": {
                "commit": "abc", "build_type": "Release", "compiler": "gcc",
                "backends": {"gl": {"gpu": "gpu", "threads": 8, "workloads": {"1000": workload(**kw)},
                                    "capacity_60fps": {"dense": capacity()}}},
                "exports": {"shmup": {"stripped_player_bytes": 1000000, "stripped_pack_bytes": 500000}}}}}


class Parsing(unittest.TestCase):
    def test_bench_line(self):
        b = bench.parse_bench_line(LINE)
        self.assertEqual(b["frame_ms"]["p95"], 6.439)
        self.assertEqual(b["sim_tick_ms"], {"mean": 1.771, "p95": 2.1, "max": 2.5})
        self.assertEqual(b["sim_ticks"], 180)
        self.assertEqual(b["msaa"], 4)
        self.assertIsNone(bench.parse_bench_line("nothing here"))

    def test_hash_device_threads(self):
        self.assertEqual(bench.parse_hash("Screenshot written to 'x' at tick 600 (state hash 00ff00ff00ff00ff)"),
                         (600, "00ff00ff00ff00ff"))
        self.assertEqual(bench.parse_device("OpenGL device: 3.3 NVIDIA (RTX), 4x MSAA"), "3.3 NVIDIA (RTX), 4x MSAA")
        self.assertEqual(bench.parse_threads("Job system: 8 thread(s); SIMD"), 8)

    def test_median_of_runs(self):
        runs = []
        for p95 in (5.0, 9.0, 6.0):
            b = bench.parse_bench_line(LINE)
            b["frame_ms"]["p95"] = p95
            runs.append(b)
        m = bench.median_stats(runs)
        self.assertEqual(m["frame_ms"]["p95"], 6.0)
        self.assertEqual(m["spread"]["frame_p95_ms"], [5.0, 9.0])


class Validation(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(bench.validate_record(record()), [])

    def test_problems(self):
        r = record()
        del r["builds"]["release"]["backends"]["gl"]["gpu"]
        r["builds"]["release"]["backends"]["gl"]["workloads"]["1000"]["state_hash_jobs1"] = "ff"
        del r["builds"]["release"]["backends"]["gl"]["capacity_60fps"]["dense"]["cpu_limit"]
        e = bench.validate_record(r)
        self.assertTrue(any("gpu" in x for x in e))
        self.assertTrue(any("job counts" in x for x in e))
        self.assertTrue(any("cpu_limit" in x for x in e))
        self.assertTrue(bench.validate_record({"schema_version": 99}))


class Compare(unittest.TestCase):
    def run_compare(self, new, **kw):
        return bench.compare_records(record(), new, **kw)

    def test_identical_passes(self):
        rows, failed = self.run_compare(record())
        self.assertFalse(failed)
        self.assertIn("state_hash", bench.format_rows(rows))

    def test_sim_tick_regression(self):
        self.assertTrue(self.run_compare(record(sim_mean=1.0 * 1.16))[1])
        self.assertFalse(self.run_compare(record(sim_mean=1.0 * 1.10))[1])

    def test_frame_threshold_is_looser(self):
        self.assertFalse(self.run_compare(record(frame_p95=6.0 * 1.20))[1])
        self.assertTrue(self.run_compare(record(frame_p95=6.0 * 1.30))[1])

    def test_hash_change_fails(self):
        rows, failed = self.run_compare(record(h="00000000000000bb"))
        self.assertTrue(failed)
        self.assertTrue(any(r[1] == "state_hash" and r[6] == "FAIL" for r in rows))

    def test_backend_hash_mismatch_fails(self):
        new = record()
        gl = new["builds"]["release"]["backends"]["gl"]
        vk = copy.deepcopy(gl)
        vk["workloads"]["1000"]["state_hash"] = vk["workloads"]["1000"]["state_hash_jobs1"] = "01"
        new["builds"]["release"]["backends"]["vulkan"] = vk
        self.assertTrue(self.run_compare(new)[1])

    def test_export_size(self):
        new = record()
        new["builds"]["release"]["exports"]["shmup"]["stripped_player_bytes"] = 1060000  # +6%
        self.assertTrue(self.run_compare(new)[1])

    def test_capacity_thresholds(self):
        def with_cap(cpu, frame):
            r = record()
            r["builds"]["release"]["backends"]["gl"]["capacity_60fps"]["dense"] = capacity(cpu, frame)
            return r
        self.assertTrue(self.run_compare(with_cap(400000, 100000))[1])    # cpu limit -20%: strict
        self.assertFalse(self.run_compare(with_cap(450000, 100000))[1])   # -10%
        self.assertFalse(self.run_compare(with_cap(500000, 80000))[1])    # frame limit -20%: loose
        self.assertTrue(self.run_compare(with_cap(500000, 70000))[1])     # -30%
        self.assertFalse(self.run_compare(with_cap(900000, 300000))[1])   # faster is fine

    def test_noise_floor(self):
        base = record(sim_mean=0.01, sim_p95=0.01, frame_p95=0.01)
        new = record(sim_mean=0.04, sim_p95=0.04, frame_p95=0.04)
        self.assertFalse(bench.compare_records(base, new)[1])

    def test_machine_mismatch(self):
        self.assertTrue(self.run_compare(record(machine="other"))[1])
        rows, failed = self.run_compare(record(machine="other", frame_p95=60.0), allow_machine_mismatch=True)
        self.assertFalse(failed)  # timings are informational on another machine
        self.assertTrue(any(r[6] == "info" for r in rows))
        self.assertTrue(self.run_compare(record(machine="other", h="00000000000000bb"),
                                         allow_machine_mismatch=True)[1])  # hashes are not

    def test_scale_loosens(self):
        loose = {k: v * 10 for k, v in bench.THRESHOLDS.items() if k.endswith("_pct")}
        self.assertFalse(self.run_compare(record(sim_mean=1.5), thresholds=loose)[1])


def linear(limit_n, budget=bench.BUDGET_MS):
    """A fake machine whose p95 is exactly `budget` at `limit_n` particles; returns (measure, calls)."""
    calls = []

    def measure(n):
        calls.append(n)
        return {"t": budget * n / limit_n}
    return measure, calls


class CapacitySearch(unittest.TestCase):
    def test_finds_limit_within_resolution(self):
        for limit_n in (12000, 73000, 311111, 1234567):
            measure, calls = linear(limit_n)
            r = bench.search_capacity(measure, "t", 10000, 4000000, 0.05)
            self.assertLessEqual(r["particles"], limit_n)                 # meets the budget
            self.assertGreaterEqual(r["particles"], limit_n * 0.94)       # within the resolution
            self.assertFalse(r["at_least"])
            self.assertEqual(len(r["probes"]), len(calls))
            self.assertTrue(any(p["particles"] == r["particles"] and p["ok"] for p in r["probes"]))
            self.assertTrue(all(p["ok"] == (p["t"] <= bench.BUDGET_MS) for p in r["probes"]))

    def test_upper_bound_hit_is_at_least(self):
        measure, _ = linear(10 ** 9)
        r = bench.search_capacity(measure, "t", 10000, 100000)
        self.assertEqual((r["particles"], r["at_least"]), (100000, True))
        self.assertIsNone(r["range"][1])  # nothing failed: no upper end
        self.assertEqual(r["probes"][-1]["particles"], 100000)

    def test_below_minimum(self):
        measure, _ = linear(1000)
        r = bench.search_capacity(measure, "t", 10000, 100000)
        self.assertEqual((r["particles"], r["below_min"], r["range"]), (0, True, [0, 10000]))

    def test_monotonic_failure_probes_never_pass_above_a_failure(self):
        measure, _ = linear(250000)
        r = bench.search_capacity(measure, "t", 10000, 4000000)
        failed = [p["particles"] for p in r["probes"] if not p["ok"]]
        passed = [p["particles"] for p in r["probes"] if p["ok"]]
        self.assertLess(max(passed), min(failed))
        self.assertLess(len(r["probes"]), 25)

    def test_noisy_probes_still_return_a_passing_count(self):
        rng = random.Random(7)
        for seed in range(30):
            rng.seed(seed)
            noise = {}

            def measure(n):
                if n not in noise:  # a probe is measured once and cached, as the tool does
                    noise[n] = 1.0 + rng.uniform(-0.08, 0.08)
                return {"t": bench.BUDGET_MS * n / 200000 * noise[n]}
            r = bench.search_capacity(measure, "t", 10000, 4000000, 0.05)
            self.assertTrue(any(p["particles"] == r["particles"] and p["ok"] for p in r["probes"]))
            self.assertGreater(r["particles"], 150000)
            self.assertLess(r["particles"], 260000)

    def test_range_from_repeated_runs(self):
        # Three runs per probe: the median decides pass/fail, the spread gives the range.
        def measure(n):
            base = bench.BUDGET_MS * n / 200000
            runs = [base * 0.95, base, base * 1.05]
            return {"t": sorted(runs)[1], "t_runs": runs}
        r = bench.search_capacity(measure, "t", 10000, 4000000, 0.02)
        low, high = r["range"]
        self.assertLessEqual(low, r["particles"])
        self.assertGreaterEqual(high, r["particles"])
        # every run passes at `low`, every run fails at `high`
        self.assertLessEqual(max(p["runs"] for p in r["probes"] if p["particles"] == low)[-1], bench.BUDGET_MS)
        self.assertGreater(min(p["runs"] for p in r["probes"] if p["particles"] == high)[0], bench.BUDGET_MS)
        self.assertLess(low, high)

    def test_describe(self):
        self.assertEqual(bench.describe({"particles": 920000, "at_least": False, "range": [900000, 980000]}),
                         "920000 (range 900000 to 980000)")
        self.assertEqual(bench.describe({"particles": 4000000, "at_least": True, "range": [4000000, None]}),
                         "at least 4000000")


class CapacityProject(unittest.TestCase):
    def test_steps_line_exists_in_both_scenes(self):
        for project in bench.SCENES.values():
            text = (bench.ROOT / "examples" / project / "scripts" / f"{project}.plm").read_text(encoding="utf-8")
            self.assertIn(bench.STEPS_LINE, text)


if __name__ == "__main__":
    unittest.main()

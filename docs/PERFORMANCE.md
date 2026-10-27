# Performance baselines

Cuervo Engine publishes repeatable measurements of the simulation, the renderer, memory and
export size for representative particle loads. `tools/bench.py` records them into one JSON file
per machine, `docs/perf/baseline-<machine-id>.json`, and `tools/bench.py compare` flags
regressions against it. This page says how to run it, how to read the files, which thresholds
decide a regression, and what the committed baseline shows.

**Every number here is a measured fact about one machine and these two scenes, not a promise
about the engine.** Your game, your GPU and your driver will differ, sometimes a lot.

## What is measured

The workload is `examples/bench`: four seeded emitters that fire rings of SDF shapes (outlined
circles, capsules, spinning rounded rectangles, rings), topped up to a target count. The
`--bench` option of the runtime runs it uncapped (VSync off), waits out a warm-up, then samples
every frame of a steady-state window and logs frame-time statistics and the time of each
simulation tick (`cuervo --game examples/bench --bench 6 --bench-warmup 4`).

**Decision:** frame times mean GPU completion on both backends. Vulkan keeps two frames in
flight on fences. The GL backend now does the same: it puts a fence after each presented frame
and waits on the one from two frames ago. Before that, GL drivers queued hundreds of frames at
small loads, so the GL frame rate measured how fast the CPU could submit, not how fast the
GPU finished (and the queued frames held hundreds of MB, see "Findings"). The GL figures in
the baseline from before that change were optimistic; the table below was re-recorded.

`tools/bench.py record` runs the 1k, 10k, 50k and 100k workloads on a backend, three times
each by default, and keeps the median of the runs:

- frame time: mean, p50, p95, p99 and max in milliseconds, and frames per second;
- simulation time per tick: mean, p95 and max in milliseconds (the CPU-only figure: it is
  measured around the tick, outside rendering);
- the live particle count (one SDF shape per particle);
- peak resident memory of the process (POSIX `wait4`; skipped on Windows);
- the **frame-time cliff ratio**: the slowest one-second window's mean frame time over the
  median window's, worst of the runs (the runtime's `bench windows |` line, milestone 10.4). A
  steady run stays near 1; a stretch of slow seconds that the percentiles average away shows
  up here;
- the **state hash** at tick 600 of a scripted capture run, once with the default job count
  and once with `--jobs 1`;
- export sizes: `cuervo-tools export` of the shmup and bench examples, full and stripped
  player, and the pack.

The state hash is the determinism cross-check (DESIGN.md section 3): a workload must reach the
same hash on every thread count, on both backends and in debug and release builds. `record`
fails when two job counts disagree, `compare` fails when a hash differs from the baseline or
between backends, and the hashes in the committed file are identical across all four
build/backend combinations.

Resolution (1280x720), 4x MSAA, the GPU and driver string, the CPU, the thread count, the
build type and the compiler are stored with the numbers.

## Running it

```sh
cmake --preset release -B build/release && cmake --build build/release --target runtime cuervo_tools
python3 tools/bench.py record   --build-dir build/release --backends gl vulkan \
    --out docs/perf/baseline-<machine-id>.json
python3 tools/bench.py capacity --build-dir build/release --backends gl vulkan \
    --out docs/perf/baseline-<machine-id>.json
python3 tools/bench.py compare docs/perf/baseline-<machine-id>.json new.json   # exit 1 on a regression
python3 tools/bench.py check   docs/perf/baseline-<machine-id>.json            # validate a file
python3 tools/bench.py report  docs/perf/baseline-<machine-id>.json            # Markdown tables
```

`record` and `capacity` merge into the file they are given, keyed by build type, so one file holds
debug and release. GL runs use the `offscreen` SDL driver on Linux (no window, no compositor
in the way); Vulkan needs a real display (`WAYLAND_DISPLAY` defaults to `wayland-0` when no
display is set). `--machine-id` (or `CUERVO_BENCH_MACHINE`) names the machine; the stripped
player build is cached with `--player-cache <dir>`. Close other work first and look at `uptime`:
a loaded machine shows up as noise.

**Decision:** a baseline is only comparable on the machine id it was taken on. `compare` refuses
two different ids unless `--allow-machine-mismatch`, which still checks hashes and export sizes
but reports timings as information.

### Web

Milestone 10.4: `--web <web runtime dir>` runs the same workloads in headless Chrome on the real
GPU (`tools/web/smoke_test.mjs --gpu`), on WebGPU or on the WebGL2 fallback (`--no-webgpu`), and
stores them under `builds.web`. `--build-dir` then only provides `cuervo-tools`, which packs each
project next to a copy of the page. It needs the emsdk's Node (or `CUERVO_NODE`) and Chrome.

```sh
cmake --preset web-pack-threads && cmake --build --preset web-pack-threads
python3 tools/bench.py record --build-dir build/release --web build/web-pack-threads/runtime \
    --backends webgpu webgl2 --out docs/perf/baseline-<machine-id>.json
python3 tools/bench.py record --build-dir build/release --web build/web-pack-threads/runtime \
    --backends webgpu webgl2 --out new.json
python3 tools/bench.py compare docs/perf/baseline-<machine-id>.json new.json
```

What differs from a native run:

- the canvas, not the window, sets the resolution (1054x593 in the smoke test's 1280x800 page);
- no peak memory (the browser's processes are not the engine's);
- the state-hash run passes `--screenshot-tick 600` (a page has no environment variables);
- uncapped means `setImmediate` frames (`run_main_loop` with VSync off): browsers clamp
  `setTimeout(0)` to about 4 ms, which had capped WebGL2 runs at 250 FPS, and WebGPU has no swap
  interval at all (it ran at the 60 Hz animation-frame rate). Chrome still presents WebGL2 at
  most 240 times a second, so the WebGL2 frame rate is the browser's ceiling, not the engine's;
- the web clock is `performance.now()`: SDL's counter advanced in whole milliseconds in the
  browser, so every web frame used to measure 16 or 17 ms.

## Reading a baseline file## Reading a baseline file

```json
{
  "schema_version": 1,
  "machine_id": "5800x3d-rtx3060ti-linux",
  "timestamp": 0,
  "host": { "cpu": "...", "logical_cpus": 16, "os": "...", "python": "..." },
  "builds": {
    "release": {
      "commit": "<git short hash>", "build_type": "Release", "preset": "<build dir name>", "compiler": "...",
      "backends": {
        "gl": {
          "gpu": "<GPU / driver string>", "threads": 8, "resolution": "1280x720", "msaa": 4,
          "video_driver": "offscreen",
          "bench": { "runs": 3, "warmup_s": 4.0, "duration_s": 6.0 },
          "workloads": {
            "100000": {
              "frame_ms": { "mean": 0, "p50": 0, "p95": 0, "p99": 0, "max": 0 },
              "sim_tick_ms": { "mean": 0, "p95": 0, "max": 0 },
              "fps": 0, "particles": 0, "render_cpu_ms": 0, "peak_rss_mb": 0,
              "state_hash": "<hex>", "state_hash_jobs1": "<hex>", "hash_tick": 600,
              "spread": { "frame_p95_ms": [0, 0], "sim_tick_mean_ms": [0, 0], "fps": [0, 0] }
            }
          },
          "capacity_60fps": {
            "dense": {
              "particles": 0, "at_least": false, "range": [0, 0], "binding_limit": "frame",
              "frame_limit": { "particles": 0, "at_least": false, "range": [0, 0] },
              "cpu_limit":   { "particles": 0, "at_least": false, "range": [0, 0] },
              "context": { "backend": "...", "build_type": "...", "gpu": "...", "cpu": "...",
                           "threads": 8, "resolution": "...", "msaa": 4, "video_driver": "..." },
              "method": { "runs_per_probe": 3, "warmup_s": 4.0, "duration_s": 4.0, "resolution": 0.05 },
              "probes": [ { "particles": 0, "live_particles": 0, "frame_p95_ms": 0,
                            "frame_p95_runs": [0, 0, 0], "sim_tick_p95_ms": 0, "sim_tick_p95_runs": [0, 0, 0] } ]
            }
          }
        }
      },
      "exports": { "shmup": { "full_player_bytes": 0, "stripped_player_bytes": 0, "full_pack_bytes": 0, "stripped_pack_bytes": 0 } }
    }
  }
}
```

`commit` is the engine commit the build was made from (`+dirty` when the tree had changes). A
`spread` pair is the smallest and largest of the repeated runs. `tools/bench.py check` validates
the shape and the determinism rules. A web entry uses the same keys under `builds.web`.

## Thresholds

`tools/bench.py compare <baseline> <new>` prints a table and exits 1 when any check fails
(2 for unreadable or invalid files). The limits live in one place, the `THRESHOLDS` table at
the top of `tools/bench.py`, and `--scale N` multiplies every percentage (a noisy machine, a
smoke test).

| Check | Limit against the baseline | Why |
|---|---|---|
| State hash, any workload | must be equal; backends must agree | determinism is not a tolerance |
| Simulation tick time, mean | +15% | CPU only: the strict timing check |
| Simulation tick time, p95 | +25% | CPU only, but a tail statistic |
| Frame time p95 | +25% | includes the GPU, the driver and the display path |
| Peak RSS | +10% | |
| Export sizes (player, pack) | +5% | sizes are exact; growth is a decision |
| Capacity, CPU limit | -15% | strict: it is machine-comparable |
| Capacity, frame-time limit | -25% | GPU bound, so loose |
| Frame-time cliff ratio | at most 1.5 (absolute, the new record only) | a steady run has no slow stretch (milestone 10.4) |

Timings under 0.05 ms are reported as information only: a percentage of a few microseconds is
noise. Timings, memory and capacity are only enforced on the same machine id.

**Decision:** CPU-only checks are the strict ones, because simulation time does not depend on
the GPU, the driver or the display. Frame-time and frame-capacity limits are looser because they
include all three. The numbers come from the observed spread below: each limit is several times
the run-to-run variation seen on the reference machine.

Re-record a baseline (with a note in the commit) when a change moves a hash on purpose, or when
a regression is accepted. `compare` also fails on a hash difference, which is how a change to
simulation behavior is noticed.

## The capacity test

`tools/bench.py capacity` answers "how many particles can this machine run at 60 fps?". For each
scene it searches, by doubling from 10 000 and then bisecting to 5%, for the largest particle count
that stays within a 16.67 ms budget, up to a ceiling of 2 000 000. Every probe is a copy of the scene
whose target count is that number, warmed up (4 s) and measured over a 4 s steady-state window, three
runs, p95, median of the runs. Two limits are searched and reported separately:

- the **frame-time limit**: p95 frame time of the whole frame (simulation, recording, GPU
  and driver, VSync off);
- the **CPU limit**: p95 simulation tick time, with no rendering in it (`sim_tick_ms`). There is no
  windowless runner, so it is measured inside the rendering process, but around the tick only.

The smaller of the two is the capacity, and the page says which one **binds**. The range is what
the repeated runs leave open: the low end is the largest probed count at which every run
passed and the high end the smallest at which every run failed. A result of the form "at
least N" means the ceiling passed.

A probe whose run crashes or times out counts as a failed probe.

### Scenes

| Scene | Project | What it stresses |
|---|---|---|
| dense | `examples/bench` | Four emitters on Lissajous paths around the middle of the screen, size-5 shapes (the circles outlined), speeds of 60 to 210 px/s: the particles pile up in a few dense, overlapping sprays, so it is the worst case for overdraw and fill rate as well as for the per-shape cost |
| sparse | `examples/bench_spread` | The same emitters and shapes, but rings start at random points over the whole screen, move at 10 to 40 px/s and are 1 pixel in size: the particles cover the screen thinly, so fill-rate cost is far lower and the per-shape simulation and instance cost remain |

A game that fires a few bright, large bullets over a small area is nearer the dense scene; a game
that scatters many small, fast bits over the whole screen is nearer the sparse one. The pair is
a bracket, not a guarantee. **On the reference machine the two scenes land close together** (see
the table): at these sizes the frame is bound by the per-shape cost, not by fill rate. A GPU with
less fill-rate headroom (an integrated GPU, a laptop part) can separate them much more.

## The committed baseline

`docs/perf/baseline-5800x3d-rtx3060ti-linux.json`: Ryzen 7 5800X3D (8 cores, 16 threads), RTX 3060 Ti,
Linux, GCC. The commit stored in the file is the engine commit the builds were made from. It
was re-recorded after the GL frame limit and the stream-buffer changes (see "Findings"), with
nothing else running; the release workloads were recorded twice because the first pass started
while the load average was still 4 to 7 from the previous step (it showed the simulation tick up
by 20% at 50k, which is what the strict 15% check is for), and the second, at a load average of
0.9, is the one committed. Check `uptime` first.

| Build | Backend | GPU / driver | Resolution | MSAA | Threads | Video driver |
|---|---|---|---|---|---|---|
| debug (c++ (GCC) 16.2.1 20260810) | gl | 3.3.0 NVIDIA 615.78.08 (NVIDIA GeForce RTX 3060 Ti/PCIe/SSE2), 4x MSAA, compatible tier | 1280x720 | 4x | 8 | offscreen |
| debug (c++ (GCC) 16.2.1 20260810) | vulkan | Vulkan 1.4.351 (NVIDIA GeForce RTX 3060 Ti), compatible tier | 1280x720 | 4x | 8 | auto |
| release (c++ (GCC) 16.2.1 20260810) | gl | 3.3.0 NVIDIA 615.78.08 (NVIDIA GeForce RTX 3060 Ti/PCIe/SSE2), 4x MSAA, compatible tier | 1280x720 | 4x | 8 | offscreen |
| release (c++ (GCC) 16.2.1 20260810) | vulkan | Vulkan 1.4.351 (NVIDIA GeForce RTX 3060 Ti), compatible tier | 1280x720 | 4x | 8 | auto |

### Web (WebGPU and WebGL2)

Milestone 10.4, `builds.web` of the same file: the `web-pack-threads` runtime (emscripten
6.0.12, 8 job threads) in headless Google Chrome 156 on the RTX 3060 Ti, 1054x593, 4x MSAA.

| Backend | Particles | FPS | frame p50 / p95 / p99 / max (ms) | sim ms/tick (mean / p95) | cliff ratio | state hash |
|---|---|---|---|---|---|---|
| webgpu | 1k | 404 | 2.435 / 5.555 / 6.555 / 13.16 | 0.042 / 0.050 | 1.066 | `77e955bfebed7613` |
| webgpu | 10k | 435 | 2.310 / 3.415 / 4.410 / 11.21 | 0.077 / 0.090 | 1.212 | `bff47994b0264192` |
| webgpu | 50k | 476 | 2.035 / 2.900 / 3.890 / 11.47 | 0.166 / 0.195 | 1.095 | `08a148babe2ddac5` |
| webgpu | 100k | 390 | 2.325 / 3.850 / 4.750 / 12.59 | 0.282 / 0.340 | 1.085 | `d6d6a623a66410b3` |
| webgl2 | 1k | 240 | 4.145 / 4.205 / 4.620 / 6.275 | 0.039 / 0.045 | 1.004 | `77e955bfebed7613` |
| webgl2 | 10k | 236 | 4.180 / 4.295 / 6.140 / 7.840 | 0.073 / 0.090 | 1.009 | `bff47994b0264192` |
| webgl2 | 50k | 222 | 4.340 / 5.555 / 6.155 / 7.570 | 0.175 / 0.205 | 1.019 | `08a148babe2ddac5` |
| webgl2 | 100k | 210 | 4.535 / 6.075 / 6.595 / 7.445 | 0.285 / 0.335 | 1.005 | `d6d6a623a66410b3` |

Every web hash equals the native one for its workload, on both backends and with 1 or 8 jobs.
**Is the browser benchtest stable on WebGPU?** Yes: no one-second window of any run was more than
1.21x the median one (the 10k workload; 1.07 to 1.10 elsewhere), and a second, independent record
compared against this one passed every check (frame p95 within 3.4%, simulation within 3%,
cliff ratios 1.05 to 1.16). WebGPU frames are bimodal at light loads (p50 about 2.4 ms, p95 up to
5.6 ms at 1k: the browser's presentation, not the engine, whose simulation tick is 0.04 ms), and
it runs 100k shapes at 390 FPS against WebGL2's 210 (which the browser caps at 240). The
simulation is 1.7x (100k) to 1.5x (1k) slower than the native release build's, the cost of WASM.

### Workloads

All builds and backends reach the same state hash at tick 600 for each workload, with 1 job and
with 8.

| Build | Backend | Particles | FPS | frame p50 / p95 / p99 / max (ms) | sim ms/tick (mean / p95) | peak RSS (MB) | state hash |
|---|---|---|---|---|---|---|---|
| debug | gl | 1k | 4695 | 0.206 / 0.218 / 0.340 / 2.235 | 0.127 / 0.152 | 270.1 | `77e955bfebed7613` |
| debug | gl | 10k | 1702 | 0.571 / 0.589 / 0.964 / 2.364 | 0.350 / 0.434 | 269.2 | `bff47994b0264192` |
| debug | gl | 50k | 431 | 2.171 / 3.211 / 3.380 / 5.184 | 0.977 / 1.166 | 277.7 | `08a148babe2ddac5` |
| debug | gl | 100k | 215 | 4.180 / 5.974 / 6.134 / 7.626 | 1.635 / 1.844 | 305.8 | `d6d6a623a66410b3` |
| debug | vulkan | 1k | 2456 | 0.398 / 0.429 / 0.534 / 1.471 | 0.099 / 0.121 | 270.4 | `77e955bfebed7613` |
| debug | vulkan | 10k | 1294 | 0.756 / 0.920 / 1.115 / 1.402 | 0.314 / 0.379 | 270.3 | `bff47994b0264192` |
| debug | vulkan | 50k | 401 | 2.352 / 3.276 / 3.433 / 3.774 | 0.880 / 1.064 | 274.6 | `08a148babe2ddac5` |
| debug | vulkan | 100k | 208 | 4.346 / 6.064 / 6.273 / 6.550 | 1.556 / 1.869 | 280.8 | `d6d6a623a66410b3` |
| release | gl | 1k | 16235 | 0.057 / 0.065 / 0.078 / 2.821 | 0.028 / 0.025 | 275.4 | `77e955bfebed7613` |
| release | gl | 10k | 8400 | 0.115 / 0.118 / 0.137 / 2.448 | 0.049 / 0.054 | 269.9 | `bff47994b0264192` |
| release | gl | 50k | 2301 | 0.431 / 0.439 / 0.463 / 2.258 | 0.105 / 0.118 | 292.7 | `08a148babe2ddac5` |
| release | gl | 100k | 1196 | 0.833 / 0.850 / 1.037 / 2.559 | 0.164 / 0.193 | 312.7 | `d6d6a623a66410b3` |
| release | vulkan | 1k | 9495 | 0.102 / 0.120 / 0.144 / 0.265 | 0.033 / 0.036 | 252.3 | `77e955bfebed7613` |
| release | vulkan | 10k | 7419 | 0.134 / 0.144 / 0.199 / 2.008 | 0.049 / 0.056 | 252.8 | `bff47994b0264192` |
| release | vulkan | 50k | 3176 | 0.309 / 0.347 / 0.558 / 2.140 | 0.099 / 0.115 | 256.5 | `08a148babe2ddac5` |
| release | vulkan | 100k | 1664 | 0.592 / 0.645 / 0.813 / 1.876 | 0.148 / 0.170 | 262.7 | `d6d6a623a66410b3` |

**Do 50k and 100k hold 60 fps?** Yes, on this machine, in both builds and on both backends, with a
wide margin: the worst p95 frame time at 100k is 6.06 ms in the debug build (Vulkan) and 0.85 ms in
the release build; the debug figure is a fraction of the 16.67 ms budget. (At several hundred frames
per second most frames contain no simulation tick, so the p95 of the frame time understates the cost of
a tick; the simulation columns and the capacity test report the tick on its own.)

### Capacity (60 fps)

| Scene | Build | Backend | Particles at 60 fps (range of the runs) | Binds | Frame-time limit | CPU limit (sim tick p95) |
|---|---|---|---|---|---|---|
| dense | debug | gl | **270 000 (270 000 to 320 000)** | frame | 270 000 (270 000 to 320 000) | 1 160 000 (1 160 000 to 1 200 000) |
| dense | debug | vulkan | **290 000 (290 000 to 300 000)** | frame | 290 000 (290 000 to 300 000) | 1 160 000 (1 120 000 to 1 200 000) |
| dense | release | gl | **1 505 000 (1 460 000 to 1 640 000)** | frame | 1 505 000 (1 460 000 to 1 640 000) | at least 2 000 000 |
| dense | release | vulkan | **1 820 000 (1 820 000 to 1 910 000)** | frame | 1 820 000 (1 820 000 to 1 910 000) | at least 2 000 000 |
| sparse | debug | gl | **290 000 (290 000 to 300 000)** | frame | 290 000 (290 000 to 300 000) | 1 160 000 (1 160 000 to 1 280 000) |
| sparse | debug | vulkan | **240 000 (240 000 to 250 000)** | frame | 240 000 (240 000 to 250 000) | 1 160 000 (1 160 000 to 1 280 000) |
| sparse | release | gl | **1 550 000 (1 550 000 to 1 640 000)** | frame | 1 550 000 (1 550 000 to 1 640 000) | at least 2 000 000 |
| sparse | release | vulkan | **1 910 000 (1 910 000 to 2 000 000)** | frame | 1 910 000 (1 910 000 to 2 000 000) | at least 2 000 000 |

Capacity is p95 frame or tick time within 16.67 ms with VSync off at 1280x720 and 4x MSAA. The
frame-time limit binds in every row: the CPU limit is well above it (1.1 million particles or more
in the debug build and above the 2 million ceiling in the release build). The ranges come from three
runs per probe and the 5% bisection.

**What changed against the previous baseline** (same machine, same scenes; release builds unless
stated): Vulkan dense went from 1 200 000 to 1 820 000 particles and Vulkan sparse from 1 160 000
to 1 910 000, because the render-side cliff near 1.9 million particles is gone (the previous
Vulkan figures sat on the near side of it, with isolated 40 to 50 ms runs). GL dense went from
1 415 000 to 1 505 000 and GL sparse from 1 460 000 to 1 550 000: GL's figures moved little
because they were already limited by the GPU, but they now also hold with the GL queue bounded.
Debug builds: Vulkan dense 230 000 to 290 000 and sparse 190 000 to 240 000, GL dense 280 000
to 270 000 and sparse 280 000 to 290 000 (inside the spread). Vulkan is now ahead of GL at the
capacity limit; before it was behind.

### Export sizes

| Build | Project | Full player | Stripped player | Pack |
|---|---|---|---|---|
| debug | bench | 52.99 MB | 3.63 MB | 1.15 MB |
| debug | shmup | 52.99 MB | 4.45 MB | 1.89 MB |
| release | bench | 5.24 MB | 3.63 MB | 1.15 MB |
| release | shmup | 5.24 MB | 4.45 MB | 1.89 MB |

The full player is the build's own runtime (a debug runtime is large); the stripped player is
built without the subsystems the project does not use (`--engine stripped`, release).

## Observed spread (reproducibility)

Each workload runs three times in one invocation. The committed numbers were taken on the idle
machine; the release record was also taken a second time in a separate invocation, and the
previous baseline (same machine, before the changes under "Findings") is the reference for the
comparisons below:

- Within one invocation (smallest to largest of the three runs, over all 16 workload rows): frames per
  second varied by at most 3.0% (debug) and 2.1% (release), frame p95 by at most 8.8% (debug,
  Vulkan 1k) and 2.2% (release), and the mean simulation tick by at most 4.7% (debug) and 6%
  (release, 50k and above; up to 13% at 1k and 10k, where a tick is 0.03 to 0.06 ms).
- Against the previous baseline, with the same code paths: the simulation mean tick moved by
  4.8% or less in debug and by 6.5% or less in release at 10k and above (under 10% at 1k), and the
  state hashes are identical. The first release pass, taken while the machine was still loaded from the previous
  step, moved the mean tick by +20% (GL 50k) and +22% (Vulkan 10k), which is exactly what the
  15% check exists to catch: it is a loaded-machine artifact, not a result.
- The capacity result repeated within the printed range; near the limit a single run is noisy, which is why a probe is
  the median of three runs and why the range is reported.

These are the observations behind the thresholds: the strict 15% on the mean tick is about
twice the variation seen at the sizes that matter (50k and 100k), the 0.05 ms floor removes the
microsecond-scale noise, and the looser 25% limits are above the worst frame-time spread (8.8%).
Peak resident memory repeated within 3% between the two release passes (within 0.5% except GL 50k); **Decision:** the peak-RSS limit stays at 10%
(Vulkan moved by 3 to 6% with the stream buffers now in system memory, see "Findings").

## What to expect on your hardware

The figures above are for one machine. To see where yours lands, run the capacity test and compare
your row with the ones below. The two scenes bracket typical games, but not every game: your scripts,
shapes, collision work and UI add cost the bench does not have. Integrated GPUs and laptops
(power limits, shared memory) will differ a lot from a discrete desktop GPU, and the gap between
the dense and the sparse scene is where they differ most.

| Machine | Build | Backend | Scene | Particles at 60 fps (range) | Binds | CPU limit |
|---|---|---|---|---|---|---|
| Ryzen 7 5800X3D + RTX 3060 Ti, Linux | release | gl | dense | 1 505 000 (1 460 000 to 1 640 000) | frame | at least 2 000 000 |
| Ryzen 7 5800X3D + RTX 3060 Ti, Linux | release | vulkan | dense | 1 820 000 (1 820 000 to 1 910 000) | frame | at least 2 000 000 |
| Ryzen 7 5800X3D + RTX 3060 Ti, Linux | debug | gl | dense | 270 000 (270 000 to 320 000) | frame | 1 160 000 (1 160 000 to 1 200 000) |
| Ryzen 7 5800X3D + RTX 3060 Ti, Linux | debug | vulkan | dense | 290 000 (290 000 to 300 000) | frame | 1 160 000 (1 120 000 to 1 200 000) |
| Ryzen 7 5800X3D + RTX 3060 Ti, Linux | release | gl | sparse | 1 550 000 (1 550 000 to 1 640 000) | frame | at least 2 000 000 |
| Ryzen 7 5800X3D + RTX 3060 Ti, Linux | release | vulkan | sparse | 1 910 000 (1 910 000 to 2 000 000) | frame | at least 2 000 000 |
| Ryzen 7 5800X3D + RTX 3060 Ti, Linux | debug | gl | sparse | 290 000 (290 000 to 300 000) | frame | 1 160 000 (1 160 000 to 1 280 000) |
| Ryzen 7 5800X3D + RTX 3060 Ti, Linux | debug | vulkan | sparse | 240 000 (240 000 to 250 000) | frame | 1 160 000 (1 160 000 to 1 280 000) |
| *your machine* | release | gl / vulkan | dense / sparse | | | |

To contribute a row: build the release preset, run

```sh
python3 tools/bench.py capacity --build-dir build/release --backends gl vulkan \
    --machine-id <short-generic-name> --out perf-<short-generic-name>.json
python3 tools/bench.py report perf-<short-generic-name>.json
```

and send the JSON file. It records the CPU model, the GPU and driver string, the thread count, the
resolution, MSAA, the build type, the compiler and the commit, and nothing else about the
machine (no host name, no user name, no paths); choose a generic machine id such as the CPU and GPU
model. The row is that machine's measurement for these scenes, not an engine promise, and it
is not verified by the project.

## Findings, and what was done about them

Two things seen while taking the first baseline were investigated and fixed (render side only;
no simulation code changed, every state hash is the same as before).

**GL peak memory (resolved).** Release GL used 570 MB at 1k and 1.2 to 1.3 GB at 10k and above,
against 240 to 250 MB for Vulkan. Cause: nothing bounded how many frames the GL driver queued, so at
thousands of frames per second hundreds sat in the queue, each keeping the instance buffer that
`update_buffer` had orphaned for it (a whole-capacity `glBufferData(nullptr)` on the first update of
every frame). A temporary `glFinish()` after the swap confirmed it (peak RSS 494 MB, and the frame rate
halved, as the queue was no longer hiding the GPU time). Fix: a fence per presented frame with a
wait on the one from two frames ago (the GL equivalent of Vulkan's frames in flight; the web build keeps
none, as WebGL2 cannot block), Stream buffers as a ring of three GL buffers instead of orphaning, and
1 MB upload slices. Release GL peak RSS now:

| Particles | Before (MB) | After (MB) | Vulkan after (MB) |
|---|---|---|---|
| 1k | 567 | 275 | 252 |
| 10k | 1166 | 270 | 253 |
| 50k | 1286 | 293 | 256 |
| 100k | 1309 | 313 | 263 |
| 1M | 1534 | 705 | 562 |
| 2M | 1593 | 954 | 910 |

**The render-side cliff near 1.9 million particles (resolved; two causes, one per backend).**
Before, frame time rose smoothly to about 1.8 million particles and then jumped (GL 25 ms at 1.8M to
64 ms at 2.0M; Vulkan 34 to 87 ms, and the Vulkan result flipped between two values from run to run),
while the simulation stayed flat. Timing the pieces showed the cost was the copy of the instance
array into the GPU buffer (45 to 50 ms for 76 MB, against 3 ms below the cliff), not drawing, not packing,
not the simulation.

- *Vulkan:* `SdfBatcher::flush` uploads each layer's new range, so the second flush in a frame
  made `update_buffer` rename the stream buffer (a full-size second allocation and a copy of the first
  range), and with two frames in flight that is four 77 MB versions. The RTX 3060 Ti here has no
  Resizable BAR: the CPU-writable part of VRAM is 256 MB. Past three versions the fourth went to
  system memory and, with the window full, writes into it ran about 15 times slower. Fixes: an update
  beside the bytes already written this frame now writes in place (no draw recorded so far reads
  them) instead of renaming; the Stream buffer keeps no CPU shadow copy (it cost 4 to 7 ms a
  frame at 2M); a Stream buffer whose versions would take more than half of the CPU-writable
  device-local heap is placed in system memory; and the instance buffer grows by a quarter, not by
  double (past 2^21 instances doubling meant 168 MB per version). The array of instances is also no
  longer zero-filled before being filled.
- *GL:* one 76 MB `glBufferSubData`, plus a whole-capacity orphan every frame, made the NVIDIA driver
  stage the whole range (a jump to 45 ms render CPU at the same size). Fixes: three rotating GL buffers
  (no orphan, no reallocation; safe because the frame fence ring guarantees the one being written is done), and uploads in 1 MB
  slices. `glMapBufferRange` with the unsynchronized/invalidate-range flags is faster still (4 ms
  against 12 ms at 2M) but the driver pinned about 275 MB of staging memory even at 100k particles and the
  peak memory went back to 1 to 2 GB, so it was not used. **Decision:** keep the sliced `glBufferSubData`.

Frame time (mean ms, release, 1280x720, 4x MSAA, two runs each; Vulkan "before" flips between
two values at 1.8M to 2.2M, both shown):

| Particles | GL before | GL after | Vulkan before | Vulkan after |
|---|---|---|---|---|
| 1.0M | 8.2 | 8.4 | 9.8 | 5.9 |
| 1.6M | 18.8 | 15.9 | 28.0 | 11.3 |
| 1.8M | 24.7 | 21.2 | 55.4 / 34.2 | 14.7 |
| 2.0M | 63.5 | 25.8 | 87.4 / 35.0 | 17.8 |
| 2.2M | 77.8 | 32.6 | 98.9 / 104.7 | 21.3 |

There is no jump up to the 2M ceiling on either backend. The curve still bends upward past about
1M particles (render CPU time 3.2 to 7.2 ms from 1M to 1.6M on Vulkan): the instance array
is 40 B a particle, so around 1.6M it probably stops fitting the 5800X3D's 96 MB L3 and every pass
over it (packing, then the copy to the GPU) goes to main memory (not verified further than a
memcpy microbenchmark, which shows no cliff of its own). Getting rid of it would need
the particles packed straight into mapped GPU memory, which GLES 3.0 only offers through the
unsynchronized-map trade above.

Vulkan peak memory rose by 3 to 6% at small loads and by about 140 MB at 1M, as large stream buffers now live in
system memory, which counts as resident memory.

Still true from the first baseline: Vulkan was slower than GL at small loads (1k: 0.43 ms against
0.22 ms p95 in debug, 0.12 against 0.07 ms in release) and faster from 50k in release; at the
capacity limit Vulkan is now the faster backend.

## Ceiling and caps

The engine holds two million particles at once without hitting a cap (the particle pool's default
limit is 2^20 per pool and the scene spreads its particles over four emitters); the search
stops at 2 000 000 and reports "at least"; `--max` raises it. A single 4 000 000 probe on a loaded
machine was killed by the system (exit status -9) and was not investigated.

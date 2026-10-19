# Replays

The simulation is deterministic (DESIGN.md 4.1), so a replay is where a run
started, its seed and the input frame of every tick (4.8). Replay files also store
the state hash after every tick, so playing one back checks that the build still
reproduces the run bit for bit and finds the first tick where it doesn't.

## Making and checking replays

```sh
# Record with the runtime (--press holds actions on given ticks; any device input counts too)
cuervo --game examples/shmup --press "30:ui_accept,90:shoot" --record run.replay --screenshot shot.ppm

# Verify headless: exit 0 when every tick's state hash matches, 1 at the first mismatch
cuervo-editor --project examples/shmup --replay-verify run.replay --jobs 8
```

In the editor, *Play > Play and Record* (or the Replay panel) records a play
session; *Save Recording* writes it. Double-click a `.replay` in Assets, or *Load*
in the Replay panel, to play it back in the scene view with a timeline: drag to
seek, play/pause/step, speed. Every replayed tick is compared with the recorded
hash and the first mismatch is marked. Seeking backwards re-simulates from the
start (there are no VM snapshots yet); forward seeks simulate ahead. Command
scripts drive the same: `play-record`, `record save <path>`, `replay load <path>`,
`replay seek <tick>`, `replay verify` (editor/README.md).

A replay only plays in a project with the same input actions and axes, in the
same order (bit *i* of a frame is action *i*): adding, removing or reordering
actions makes older replays refuse to play. Changing scripts doesn't refuse
playback, it shows up as a hash mismatch (that is what verification is for). A
hot reload during a recording is not recorded; the replay plays the later ticks
with the scripts as they are then.

## File format (version 1)

Little-endian (`cuervo/core/binary_io.hpp`), defined in `cuervo/input/replay.hpp`.
`str` is a `u32` byte length then UTF-8 bytes.

| Field | Type | |
|---|---|---|
| magic | 4 bytes | `CRPL` |
| version | u32 | 1 |
| project | str | the project's name (informational) |
| seed | u64 | the simulation seed the run used |
| tick_rate | f32 | ticks per second; playback refuses another rate |
| actions | u32 n, then n × str | the input map's action names in index order |
| axes | u32 n, then n × str | axis names in index order (`move.x`, `move.y`...) |
| start_scene | str | empty: the project's main (`main_scene` or the `main` class), as `cuervo --game` starts; else the played scene's path/label |
| scene | u32 n, then n bytes | the played scene's file bytes (current scene format) when `start_scene` is set, so unsaved editor edits replay too |
| frames | u32 n, then n × 32 bytes | per tick: `held` u64 (bit i = action i), 8 × i16 axes, 2 × i16 pointer (virtual pixels) |
| hashes | u32 n, then n × u64 | `ScriptHost::state_hash()` after each tick (n ≤ frames) |

`frames[i]` is the input of tick `i + 1`; `hashes[i]` is the state after it. The
state hash fingerprints the particle system (pools and RNG streams), the tick and
the script globals' numbers; it doesn't depend on the job thread count or SIMD.

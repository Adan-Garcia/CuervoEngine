# Replays

The simulation is deterministic (DESIGN.md 4.1), so a replay is where a run
started, its seed and the input frame of every tick (4.8). Replay files also store
the state hash after every tick, so playing one back checks that the build still
reproduces the run bit for bit and finds the first tick where it doesn't.

## Making and checking replays

```sh
# Record with the runtime (--press holds actions on given ticks, "10-20:action" for a range; --pointer "tick:x:y" moves
# the pointer and --type "tick:text" types text; any device input counts too)
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

Input actions have stable IDs, so a replay keeps playing when the project's
actions are added, removed or reordered. In a file, bit *i* of a frame is the *i*-th
action it lists; on playback the actions are matched to the project's by ID (by name
for version 1 and 2 files, which have no IDs) and the frames are moved to the current
order. A replay that presses an action the project no longer has is refused ("the
replay presses the input action 'x', which the project no longer has"); an action it
never presses may be gone. An ID is optional in `project.toml`:

```toml
[input.action_ids]
shoot = 7        # pins the ID; without a line an action's ID is a hash of its name
```

Without a line, reordering, adding and removing actions are safe; renaming an action
changes its ID. The editor's input-map editor keeps the ID through a rename (it writes
the old name's ID into `[input.action_ids]`) and drops the line when an action is
removed. IDs are 1 to 4294967295 and unique; a duplicate is a project error. Axes are
matched by name the same way. Changing scripts doesn't refuse
playback, it shows up as a hash mismatch (that is what verification is for). A
hot reload during a recording is not recorded; the replay plays the later ticks
with the scripts as they are then.

## File format (version 4)

Little-endian (`cuervo/core/binary_io.hpp`), defined in `cuervo/input/replay.hpp`.
`str` is a `u32` byte length then UTF-8 bytes.

| Field | Type | |
|---|---|---|
| magic | 4 bytes | `CRPL` |
| version | u32 | 4 (version 1 to 3 files still load) |
| project | str | the project's name (informational) |
| seed | u64 | the simulation seed the run used |
| tick_rate | f32 | ticks per second; playback refuses another rate |
| actions | u32 n, then n × (u32 id, str name) (version 2 and older: n × str) | the input map's actions in index order, with their stable IDs |
| axes | u32 n, then n × str | axis names in index order (`move.x`, `move.y`...) |
| start_scene | str | empty: the project's main (`main_scene` or the `main` class), as `cuervo --game` starts; else the played scene's path/label |
| scene | u32 n, then n bytes | the played scene's file bytes (current scene format) when `start_scene` is set, so unsaved editor edits replay too |
| frames | u32 n, then n × 32 bytes (version 1: n × 28) | per tick: `held` u64 (bit i = action i), 8 × i16 axes, 2 × i16 pointer (virtual pixels), and from version 2 `text` u32 (the code point typed that tick, 0 for none; UI text fields) |
| hashes | u32 n, then n × u64 | `ScriptHost::state_hash()` after each tick (n ≤ frames) |
| players | (version 4) u32 m (0 to 15), then m × frames × 32 bytes | the other players of a networked session: player `p` (1 to m) has one frame per tick in the same layout as `frames`, which are player 0's |

`frames[i]` is the input of tick `i + 1`; `hashes[i]` is the state after it. The
state hash fingerprints the particle system (pools and RNG streams), the tick and
the script globals' numbers and, once a game uses animation (DESIGN.md 4.17, 9.4), a digest of
every value its tracks and tweens wrote into nodes; it doesn't depend on the job thread count or SIMD.

Version 2 added the typed-text code point to every frame so text
fields (`LineEdit`) replay; a version 1 file loads with `text` 0 everywhere.
Version 3 added the stable action IDs; a version 1 or 2 file loads with no IDs and
is matched by action name. Version 4 added the player dimension for networked play (below); a version 1 to 3 file loads with
no other players. Files are always written as version 4.

## Networked sessions

A networked match (`cuervo --game <dir> --net-host <port>` on one machine and `--net-join <host>:<port>`
on the other; `--net-relay <ws-url> --net-room <name>` for browsers and NAT; `--net-mode lockstep|rollback`)
simulates from one input frame per player per tick and nothing else, so the network is not part of the
simulation (DESIGN.md 4.19). With `--record`, each process writes the frames the session *delivered*:
player 0 in `frames`, players 1.. in the player section, and the state hash after every tick that was
confirmed on all sides (with rollback, a tick counts once all players' real input for it has arrived; the
speculative ticks are never recorded). Both peers write the same file, and `cuervo-editor --replay-verify`
(or `script::verify_replay`) plays it back offline, feeding every player's frames to `tick_players`, to the
same hashes. `tools/net/netplay_test.py` runs this end to end (two headless processes over UDP or the relay)
and is the `netplay_*` ctest family. A game that connects after tick 0 (it calls `Net.host`, `Net.join`, `Net.relay` or `Net.mesh` later)
is recorded from tick 0: the ticks played before the session keep this process's own input (the other players idle)
as a prefix, and the session's ticks follow, so the offline replay reaches the session start in the same state.
Every peer must be in the same game state when its session starts (they may differ in the prefix input), and a
peer's replay holds its own prefix, so replays are identical only when the prefixes are. The session itself is
numbered from 1 at its start; the replay numbers ticks from the run's start.

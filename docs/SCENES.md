# Scene files

Scene files (`*.scene`) store a node tree and the `@export` fields of its nodes, in a
text form by default and a versioned binary form on request (DESIGN.md 4.12; format details
in `cuervo/scene/scene_file.hpp`). Scripts load them with `Scene.load("scenes/x.scene")`
and write them with `Scene.save(node, "scenes/x.scene")` (text) or
`Scene.save_binary(node, path)`.

## Text by default

A scene file on disk is **either** form; every loader (`Scene.load`, the editor,
`cuervo-tools`, the tier check) tells them apart by the `ESCN` magic. `Scene.save`, the
editor's Save and `cuervo-tools scene load-text` write the text form, so scenes diff and
merge in version control like source. `cuervo-tools pack` and `export` compile text scenes
to binary inside the pack (a shipped game never parses text; files already binary are packed
as they are), so the pack bytes do not depend on which form the project uses. Scenes saved
by a game at run time are text too and load the same way.

Floats are written in the shortest form that reads back to the same bits, so
`dump -> parse -> dump` is the identity and a text and a binary scene load to the same
state. A saved text file starts with `scene version 2` (no schema hash, which would change
whenever either side of a merge adds a field) and nodes keep their pre-order, so edits to
different nodes or fields merge cleanly with an ordinary 3-way merge.

Convert a project's existing scenes with `cuervo-tools scene migrate <dir> --text`
(`--binary` goes back; `--dry-run` lists what would change).

## The text view

`cuervo-tools scene dump <file>` prints a stable, readable view:

```
scene version 2 schema cf017010c895b0b7
node CanvasLayer "pause" {
    always_process: bool = true
    layer: int = 20
    node Dimmer "dim" {
        anchor_max: vec2 = (1.0, 1.0)
        node Panel "panel" {
            ...
        }
    }
}
```

Each node is `node <Class> "<name>" { ... }` with one `field: type = value` line per
exported field, then its children. Floats use the shortest form that reads back
exactly, so equal scenes always dump to identical text.

The editor (`cuervo-editor`, editor/README.md) is the main way to author scenes; the text
view also works, e.g. for quick edits or merges:

```sh
cuervo-tools scene dump scenes/pause.scene > /tmp/pause.txt
$EDITOR /tmp/pause.txt
cuervo-tools scene load-text /tmp/pause.txt scenes/pause.scene   # canonical text
cuervo-tools scene build /tmp/pause.txt scenes/pause.scene       # binary
```

Text scenes can also be edited in place; `scene load-text a.scene b.scene` re-formats one
canonically and checks that it parses.

`cuervo-tools scene upgrade <file> [<out>]` rewrites a file written by an older format
version in the current one, keeping its text or binary form (loading migrates old files automatically either way).

`cuervo-tools` is built to `build/<preset>/tools/cuervo_tools/cuervo-tools`.

## `git diff` for scene files

The repository's `.gitattributes` marks scene files as text (LF) with a `scene` diff driver:

```
*.scene text eol=lf diff=scene
```

The driver only matters for scenes kept in binary form. Tell git how to turn them into
text (once per clone; use an absolute path if you run git from subdirectories):

```sh
git config diff.scene.textconv "$PWD/build/debug/tools/cuervo_tools/cuervo-tools scene dump"
git config diff.scene.cachetextconv true   # optional: cache dumps by blob
```

After that `git diff`, `git log -p` and `git show` print binary scene changes as text diffs
of the dump (text scenes diff directly).

## Merging

Text scenes merge with git's default line-based 3-way merge: one field per line and nodes
in pre-order mean changes to different nodes or fields merge cleanly, and a real conflict
shows as ordinary conflict markers to fix by hand (then run `cuervo-tools scene load-text`
on the file to check it parses). No custom merge driver is needed; the tests merge two
edits in a temporary repository. Do not set `merge=binary` or `-merge` on `*.scene`.
Binary scenes cannot be merged: convert them first with `scene migrate --text`.

## Transforms in scenes

A `Node2D`'s saved `position`, `rotation` and `scale` are relative to its parent when
the project's transform hierarchy is on (the default; `[scene] transform_hierarchy =
false` in project.toml makes them world values), and `top_level` (saved like any other
exported field) opts one node out. An instanced scene's root inherits the
`SceneInstance`'s transform with the hierarchy on. Existing scene files need no change:
what they contain is the same numbers, read as local ones.

## Animations in scenes

Animations are **part of the scene file**: an `AnimationPlayer` node exports its
`animations` list, an `AnimationTree` exports its `states` and `transitions`, and a track's
target is a path of node names under the player's parent (`"Torso/Head:rotation"`). There is
no separate `.anim` file and the scene format version does not change: the values are
ordinary exported structs, lists and enums, so scene files, the text view, migrations and
`scene dump` diffs treat them like any other field. A scene with animations instances them
with its nodes; renaming or moving a node means editing the track paths that name it.

```
node AnimationPlayer "Anim" {
    animations: List<Animation> = [Animation { name = "idle", length = 120, mode = AnimLoop.Loop, tracks = [AnimTrack { path = "Torso:position", kind = AnimTrackKind.Property, keys = [AnimKey { time = 0, value = (0.0, -70.0, 0.0, 0.0), text = "", interp = AnimInterp.EaseInOut, ease_kind = Ease.Sine, curve = 1.0 }, ...] }, ...] }]
    autoplay: str = ""
}
```

The dump prints the whole list on one line (the text view keeps a field to a line), so
animations are easier to write with the builders (`Animation.key_float(...)`) in a script
that calls `Scene.save`, or with a small generator that writes the text view:
`examples/anim_character/make_scene.py` writes `authoring/character.txt`, and
`cuervo-tools scene build` turns it into `scenes/character.scene` (the `anim_character`
ctest checks both directions). A key's `value` is a vec4 holding the field's value (a
float in `x`, a `vec2` in `xy`, a `Color` in `rgba`, an enum's discriminant in `x`);
`text` is the name for a `name` track and the event name of an event track.

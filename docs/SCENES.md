# Scene files

Scene files (`*.scene`) store a node tree and the `@export` fields of its nodes in a
versioned binary format (DESIGN.md 4.12; format details in
`cuervo/scene/scene_file.hpp`). Scripts load them with `Scene.load("scenes/x.scene")`
and write them with `Scene.save(node, "scenes/x.scene")`.

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
cuervo-tools scene build /tmp/pause.txt scenes/pause.scene
```

`cuervo-tools scene upgrade <file> [<out>]` rewrites a file written by an older format
version in the current one (loading migrates old files automatically either way).

`cuervo-tools` is built to `build/<preset>/tools/cuervo_tools/cuervo-tools`.

## `git diff` for scene files

The repository's `.gitattributes` marks scene files as binary with a `scene` diff driver:

```
*.scene binary diff=scene
```

Tell git how to turn them into text (once per clone; use an absolute path if you run git
from subdirectories):

```sh
git config diff.scene.textconv "$PWD/build/debug/tools/cuervo_tools/cuervo-tools scene dump"
git config diff.scene.cachetextconv true   # optional: cache dumps by blob
```

After that `git diff`, `git log -p` and `git show` print scene changes as text diffs of
the dump. Merges still treat scene files as binary: resolve a conflict by dumping both
sides, merging the text and building the result.

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

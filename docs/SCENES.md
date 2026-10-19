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

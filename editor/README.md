# cuervo-editor

The Cuervo Engine editor (DESIGN.md 4.13): a standalone Dear ImGui application built on
the engine. It opens, builds and saves scenes without writing code, and has an asset
browser, an input-map editor, play-in-editor with hot reload, replays, a debugger, a
profiler and the tier migration report. It is desktop only (verified on Linux; Windows is
experimental, see [STATUS.md](../docs/STATUS.md)); there is no web editor.

```sh
cmake --build build/<dir> --target editor
build/<dir>/editor/cuervo-editor --project examples/editor_level
build/<dir>/editor/cuervo-editor --project examples/shmup --scene scenes/title.scene --backend gl
```

| Option | |
|---|---|
| `--project <dir>` | the project (a `project.toml`). Without it the welcome screen picks one |
| `--scene <path>` | open this scene (relative to the project). Default: the project's `main_scene`, else a new scene |
| `--backend gl\|vulkan` | render backend. Default Vulkan, falling back to GL (GL under `SDL_VIDEO_DRIVER=offscreen`) |
| `--gl-es` | OpenGL ES 3.0 context (the web's API) |
| `--script-commands <file>` | headless: apply editor operations from a file, save, exit (below) |
| `--screenshot <file.ppm>` | render a few frames, write the whole window, exit (after `--script-commands` if both) |
| `--view <x>,<y>,<zoom>` | start the scene view there (for screenshots) instead of framing the game screen |
| `--size <w>x<h>` | window size (default 1600x900) |
| `--quit-after <s>`, `--vsync off`, `--font <file.ttf>` | as for `cuervo` |
| `--jobs <n>`, `--no-audio` | play sessions: job threads, null audio backend |
| `--replay-verify <file.replay>` | headless: replay it and compare every tick's state hash; exit 1 at the first mismatch (docs/REPLAYS.md) |
| `--new-project <dir>` | headless: create a project like the welcome screen's *New project* (`--template empty\|walker`, default `empty`; `--name`, default the folder name), print its path, exit |
| `--tabs <Panel,...>` | bring these panels' tabs to the front (screenshots) |
| `--profile` | start the Profiler panel recording (engine zones + script functions) |
| `--theme light\|dark` | the brand theme (Bone or Raven); default: the OS's preference. *View > Theme* switches it live |
| `--splash`, `--no-splash` | hold the splash until a click or key (screenshots of it) / never show it. By default it shows for about a second and a half at start (not with `--screenshot`) |
| `--accept-eula` | accept the license agreement for this user (stored; same as the check box). `CUERVO_ACCEPT_EULA=<version>` accepts one run without storing |
| `--screen eula\|welcome` | show that first-launch screen even when it is not needed (screenshots; nothing is stored) |
| `--layout <name>` | start in that saved layout (Default, Debugging, Minimal or yours) |
| `--tool select\|move\|rotate\|scale`, `--select </a,/b>`, `--palette [query]` | start with that tool, those nodes selected, or the command palette open (screenshots of the gizmos and the palette) |

`--font` sets the font of the scene view's text (the game's font); the editor UI uses the
brand fonts (below).

## Brand

The editor wears the Cuervo brand (DESIGN.md 4.16): its window icon is the Cuervo Mark
app icon (embedded in the engine, also `cuervo`'s icon), its colors are the design kit's
light (Bone) and dark (Raven) themes generated from `assets/brand/tokens.json`
(`cuervo/brand/brand_tokens.hpp`, no hex values in the editor), the UI type is Figtree
and the console is JetBrains Mono (TTFs fetched by CMake). At start a splash shows the
mark and wordmark as the kit's horizontal lockup, imported from `assets/brand/logo/*.svg`
by the engine's own SVG importer and drawn as vector meshes, with an Outfit caption;
click or press a key to skip it.

```sh
cuervo-editor --project examples/editor_level --theme light --splash --screenshot splash-light.ppm
cuervo-editor --project examples/editor_level --theme dark --screenshot editor-dark.ppm
```

The editor is desktop-only: the web build (Emscripten) never configures `editor/`.
`-DCUERVO_BUILD_EDITOR=OFF` skips it (and the Dear ImGui download) elsewhere too.

## First run, welcome screen

A fresh user account sees the **license agreement** before anything else opens: the
plain-English summary on top, the scrollable [agreement](../LICENSE.md) and the
third-party notices below, and an **unchecked** "I have read and agree to the Cuervo
Engine License Agreement" box. *Accept and continue* stays disabled until the box is
checked; *Decline and quit* is the same size and exits with status 1 without using the
engine. Acceptance is stored per user (`eula-accepted.txt` in the user data folder, with
the EULA version and date), and a new EULA version asks again. `cuervo-editor
--accept-eula` accepts for scripts (stored), `CUERVO_ACCEPT_EULA=<version>` accepts one run
(not stored); the headless modes (`--script-commands`, `--replay-verify`) and
`cuervo-tools`/`cuervo`/`plm` ask on a terminal or need one of those. **Exported games
never ask.** *Help > License Agreement* shows it again, read-only.

Without `--project` the **welcome screen** follows: recent projects (newest first, a
missing folder is flagged and can be removed), *Open Folder...* or a typed path, and *New
project* from a template (Empty 2D, Walker; `create_project()` in `projects.hpp`). The
choice opens the editor; *Project > Switch Project...* comes back here. Recents live in
`editor-recent-projects.txt` in the user data folder. Screenshot and test runs never
write any of the per-user files.

## Interface

- **Toolbar** under the menu bar: new/open/save, undo/redo, the tools, snapping, frame
  scene, Play/Record/Pause/Step/Stop, and the **command palette** box. Icons are vector
  paths drawn with ImGui's draw list (`icons.hpp`), one per node class (a class without
  its own icon uses its base class's, then the plain node icon).
- **Command palette**, Ctrl+Shift+P: fuzzy search over every menu and toolbar action,
  node-adding commands and layouts, with the shortcut shown; Enter runs the first
  enabled match.
- **Tools**, in the scene view (Q select, W move, E rotate, R scale): the move gizmo's
  arrows and square, a rotate ring (steps of 15 degrees with snapping), scale handles
  on the axes and a center square (both axes). Group transforms use the centroid as pivot.
  Ctrl while dragging inverts snapping, Shift drags without rounding.
- **Multi-select**: Ctrl-click toggles, Shift-click adds, drag in empty space draws a
  marquee, in the scene view and the scene tree. Transforms, duplicate, delete and
  Ctrl+G (group under a new Node2D) act on the whole selection; the inspector shows the
  primary (last picked) node.
- **History** panel: every undo step, newest on top; click one to jump there.
- **Messages**: failures use the kit's formula, *what happened, whether work is lost,
  what to do*, as a banner with the raw detail one click away under *Details*; successes
  are toasts that fade. The Console keeps the raw log lines.

| Shortcut | |
|---|---|
| Ctrl+Shift+P | command palette |
| Q / W / E / R | select / move / rotate / scale tool |
| Ctrl+G | group the selection |
| F | frame the game screen |

(The file, edit and play shortcuts are listed under "Files and undo" and "Play-in-editor".)

## Layout and panels

The editor uses **ImGui's docking branch** (`v1.92.9b-docking`, DESIGN.md 4.16
"Clarified (Milestone 8.3)"). Every panel is a dockable window inside the one editor
window: drag a tab to another dock or onto a dock's edge to split it, drag it into empty
space to float it, drag it back to redock, drag the thin separators to resize. Floating
panels stay inside the main window (multi-viewport, OS-level windows, is not enabled).
The default arrangement is built with the DockBuilder API and is Godot-like (DESIGN.md
7.2): **Scene** tree and **Assets** left, **Scene View** center, **Inspector** and
**Debugger** right, **Console**, **Replay**, **Input Map**, **Tier Report**,
**Profiler** and **History** bottom. Panels register in a `PanelRegistry` (`panels.hpp`)
with the dock the default layout puts them in; the close button on a tab hides a panel
and *View* shows it again.

*View > Layout* applies a saved arrangement: **Default**, **Debugging** (tall bottom dock)
and **Minimal** are built in (they are rebuilt from three size fractions), and *Save
Layout As...* stores the current one by name. A saved layout is ImGui's own ini text for the
dock tree and window placement plus the hidden panels, kept per user in
`editor-layouts.txt` next to the recents file; the window's last arrangement is saved at
exit and restored at the next start. ImGui's own `imgui.ini` is switched off.


- **Scene tree.** The node hierarchy. Click to select; *+ Add* (or Ctrl+A) opens the class
  picker with every node class the project's scripts and the engine prelude define
  (type to filter, double-click or Enter); right-click for Add Child, Rename (F2 or
  double-click a leaf), Duplicate, Move Up/Down and Delete. Drag a node onto another to
  make it the last child, or onto the upper/lower edge of a row to place it before/after.
- **Scene view.** The scene drawn by the engine's own renderer, exactly as the game
  draws it (nodes' `draw(g)` onto the SDF batcher and MSDF text), inside the panel, with a
  grid and the game screen's frame. The view never runs `ready()` or `update()`: field
  initializers and `init()` only, plus one layout pass for UI controls. Scripts that
  set things up in `ready()` show their initial state; draw from fields, not from
  objects `ready()` creates.
  - Wheel zooms at the cursor. Zoom is vector-aware: it changes the world-to-screen
    transform, and SDF shapes and MSDF text are re-evaluated per pixel at the new scale,
    so the scene stays sharp at any zoom (no bitmap scaling).
  - Right or middle drag pans; F frames the game screen.
  - Left click selects the node under the cursor (a control by its laid-out rect, a
    Node2D within a few pixels of its position).
  - The move gizmo: drag the red arrow (x), the green arrow (y) or the square (both), or
    drag the selected node itself. Moves round to whole units; Ctrl snaps to the grid;
    Shift moves freely. A Node2D moves its `position`; a UI control moves its
    `offset_min`/`offset_max` (layout computes its position).
- **Inspector.** The selected node's name, class and path, then every `@export` field of
  its class (LANGUAGE.md 9.13). Fields the file stores are bright with an **R** (reset)
  button; the others show the class default, dimmed, and editing one stores it. Widgets
  by value: checkbox (bool), drag (every number kind), drag N (vecN), text (str and
  resource paths), `#name`, combo (enums), color picker (Color), nested fields (structs),
  item lists with +/- (lists). Right-click any field to type its value in the scene text
  syntax, or reset it.
  - Drop a `.scene` from Assets into the view to instance it there: a `SceneInstance`
    node (prelude) that stores the scene's path and loads it when it enters the tree,
    so edits to that scene show up everywhere it is used. The view expands instances
    (the tree shows one node). A scene can't instance itself.
- **Console.** Every log line (engine, scripts' `print`, editor) with level toggles, a
  filter and auto-scroll.
- **Assets.** The project folder: scenes, scripts, fonts, sounds, vector art, images,
  replays. Double-click a scene to open it (asks first if the open one is unsaved), a
  replay to play it back. Drag any file onto an inspector text field to store its path,
  or a scene into the view to instance it. Right-click: open, instance, new folder,
  rename, delete (confirmed; project.toml can't be deleted or renamed), copy path.
  Changes made by other programs appear within a second (polling); a changed script
  recompiles the class registry and the view when not playing.
- **Input Map.** The project's `[input.actions]` and `[input.axes]`. *Add Action*,
  *Add Axis*, right-click an action to rename or remove it; **+ Bind** waits for the next
  key, mouse button, gamepad button or stick/trigger push (Esc cancels; type a binding
  such as `Key.Escape` in the field next to it instead); `x` removes a binding; axes take
  `Keys.Arrows`, `Keys.WASD`, `Pad.LeftStick`, `Pad.RightStick`, `Pad.Dpad`. The panel
  has its own Undo/Redo (Ctrl+Z stays the scene's). **Save** rewrites only the changed
  `key = [...]` lines of those sections in project.toml: comments, blank lines, other
  keys and sections stay byte for byte (`cuervo/script/project_edit.hpp`). Adding or
  removing actions renumbers them, so older replays of the project refuse to play.
- **Debugger.** While playing: state, tick, sim ms per tick, particles and emitters,
  colliders, node count, state hash, hot reloads; the live node tree with every field's
  value (read-only, read without running script code). When a script panics the game
  pauses, this tab comes to the front and shows the message and the stack, each frame
  with its file:line and locals (DESIGN.md 5.8).
- **Replay.** Record a play session (*Play and Record*), save it, load replay files and
  play them back with a timeline (seek, play/pause/step, speed); every tick's state hash
  is compared with the recording and the first mismatch is flagged (docs/REPLAYS.md).
- **Tier Report.** The `cuervo-tools tier check` report for the project's tier or a target
  tier, *Run Again*, *Copy as Text*; click an entry to open its scene or show its file in
  Assets (path copied to the clipboard).
- **Profiler.** (DESIGN.md 5.14 "Clarified (Phase 8 — Profiler)") *Record* turns on the
  engine zones (`cuervo/core/profiler.hpp`) and, while playing, the script profiler
  (`pluma/profiler.hpp`); *Reset* clears both. Left: graphs of the last 300 frames' frame
  time and simulation time (the `tick` zone). Middle: the zone tree (`editor frame` >
  `play` > `tick` > `scripts` > `update`, `particles`, ...) with avg and max ms per frame
  and entries per frame. Right: every script function since Play or Reset, by self time,
  with calls, self ms, inclusive ms and % of script time (hover for its file:line).
  Recording follows each new play session and survives hot reloads; it only observes, so
  state hashes don't change. A debugger session installs its own VM hooks, and its VM is
  then not profiled (one `DebugHooks` per VM). `--profile` starts with *Record* on.

## Play-in-editor

*Play* (F5) runs the project's main exactly as `cuervo --game` does; *Play Scene* (F6)
runs the open scene as edited, unsaved changes included. The game runs on
`script::GameSession` -- the runtime's own code path, in its own VM -- at the fixed tick
rate and is drawn in the scene view, letterboxed. Click the view to give the game the
keyboard, mouse and gamepad (green frame); the editor's shortcuts other than the play
keys are off then. *Pause* (F7), *Step* one tick (F10), *Stop* (Shift+F5). Stop drops the
session: the edited scene, its undo history and the view are exactly as before Play.
Saving a script while playing hot-reloads it (DESIGN.md 5.13). A panic pauses the game
(see Debugger); fix the script and save, then resume. Script errors at load go to the
console with file:line:column. For the same input the state hash equals the runtime's
(the `editor_replay` test checks this at tick 240).

## Files and undo

*File* has New Scene (pick the root class), Open, Save, Save As; Open and Save As take a
path relative to the project and list the project's `.scene` files. Scenes are written in
the current binary format (`cuervo/scene/scene_file.hpp`). The editor stores exactly the
fields the file had plus the ones you edit, in order, so opening and saving a scene
without edits writes the same bytes.

Every edit is a command on an undo stack: Ctrl+Z undoes, Ctrl+Shift+Z or Ctrl+Y redoes.
Continuous edits (dragging a value or the gizmo, typing in a text field, the color
picker) are merged into one step until released.

| Shortcut | |
|---|---|
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | new / open / save / save as |
| Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y | undo, redo |
| Ctrl+A | add a child to the selection |
| Ctrl+D | duplicate the selection |
| Delete | delete the selection |
| F2 | rename (scene tree focused) |
| F | frame the game screen |
| F5 / F6 | play the project / the open scene (F5 resumes when paused) |
| F7, F10, Shift+F5 | pause/resume, step one tick, stop |

## Headless command scripts

`--script-commands <file>` applies editor operations without a window, through the same
code the UI uses, then saves (if anything changed and the scene has a path) and exits
with status 0, or prints `file:line: error` and exits with 1. With `--screenshot` it
then opens the window and captures it.

One command per line; `#` starts a comment line; words may be `"quoted"`.

| Command | |
|---|---|
| `new <RootClass> [name]` | a new scene (no file yet) |
| `open <path>` | open a scene |
| `add <parent> <Class> [name]` | add a child (name defaults to the class, made unique) |
| `remove <node>` | delete a node and its children |
| `rename <node> <name>` | |
| `duplicate <node>` | deep copy, placed after the original, named `name2`, `name3`... |
| `reparent <node> <new parent> [index]` | move under another node (index among its children) |
| `set <node> <field> <value>` | set an `@export` field (or `name`) |
| `reset <node> <field>` | stop storing a field (class default) |
| `move <node> <dx> <dy>` | what a gizmo drag does |
| `select <node>` | |
| `undo`, `redo` | |
| `save [path]` | save, or save as `path` |
| `expect <node> <field> <value>` | fail unless the field has this value (checks, not edits) |

Nodes are paths from the root by name: `/` is the root, `/walls/wall_l` a grandchild;
`#N` picks the N-th child (`/#0`); `.` is the current selection (the node the previous
command added or edited). Values use the scene text view's syntax (docs/SCENES.md) and
are coerced to the field's type: `12`, `0.25`, `(640.0, 150.0)`, `"text"`, `#name`,
`true`, an enum as `Spiral` or `FirePattern.Spiral`, a Color as `"ff8000"` (rrggbb or
rrggbbaa) or `Color { r = 1.0, a = 0.5 }` (missing fields keep their current value), a
list as `[1, 2]`.

Play, replay, input-map and asset commands:

| Command | |
|---|---|
| `instance <parent> <scene> [x y]` | instance a scene as a child (a `SceneInstance`) |
| `play [scene]`, `play-record [scene]` | play the project's main, or the open scene as edited; optionally recording |
| `pause`, `resume`, `stop` | |
| `step [n]` | run n ticks now (default 1); stops early at a panic |
| `hold <action>`, `release <action>` | hold a project action on the following ticks |
| `expect-tick <n>`, `expect-hash <hex>` | checks on the running game |
| `print-hash` | logs `at tick N (state hash H)`, the runtime's wording, so a script can hand the hash to a test |
| `expect-state stopped\|playing\|paused`, `expect-panic <text>`, `expect-clean` | the play state; a panic mentioning text (message, trace or locals); no unsaved scene changes |
| `append-file <path> <text>`, `reload` | appends a line to a project file (a script edit); force a hot reload of the playing game (fails if it did not reload) |
| `record start\|stop`, `record save <path>` | recording, and saving it as a replay |
| `replay load <path>`, `replay seek <tick>`, `replay verify`, `replay expect-mismatch <tick>` | play a replay back, verify every hash to the end |
| `input add-action\|remove-action <a>`, `input rename-action <a> <b>`, `input bind\|unbind <a> <binding>` | input map edits |
| `input add-axis\|remove-axis <axis>`, `input axis-source\|remove-axis-source <axis> <source>` | |
| `input undo\|redo\|save`, `input expect <action> [bindings...]` | |
| `asset mkdir <path>`, `asset rename <path> <name>`, `asset delete <path>`, `asset expect[-missing] <path>` | file operations |

Gizmo, multi-select, palette and history commands:

| Command | |
|---|---|
| `rotate <node> <radians>`, `scale <node> <factor> [<factor y>]` | what the rotate and scale gizmos do to one node |
| `select+ <node>`, `select-toggle <node>`, `select-clear` | build a multi-selection (`select` replaces it) |
| `move-selection <dx> <dy>`, `rotate-selection <radians>`, `scale-selection <factor> [<factor y>]` | group transforms around the selection's centroid, one undo step each |
| `group [Class]` | wrap the selection in a new node (default Node2D), Ctrl+G |
| `palette <query>` | run the best enabled palette match, like Ctrl+Shift+P then Enter |
| `expect-palette <query> <title>`, `expect-selected <count>`, `expect-history <steps>` | checks |
| `jump <steps applied>` | click a row of the History panel |

Play in a command script is synchronous: `step` runs the ticks immediately, with no
device input (only `hold`) and the pointer at (0, 0), like a headless `cuervo --game`.

`examples/editor_level/level.cmds` builds that example's level this way; the
`editor_level` test rebuilds it with the shipped binary, checks the result against the
checked-in scene byte for byte and runs it with `cuervo --game`.
`examples/node_level/level.cmds` builds a level from the built-in nodes (no scripts); the `node_level` test also round-trips it through the scene text view and
plays the saved scene in the editor to the runtime's state hash.

## How it is built

- `scene_model` — the scene as a tree with stable node ids, converting losslessly to and
  from `scene::SceneDoc`.
- `commands` — `Command`s (set field, rename, insert, remove, move, group) and the
  `CommandStack` (undo/redo, merging, dirty tracking).
- `class_registry` — node classes and their `@export` fields, types and defaults, read
  from the compiled scripts the way `scene_bridge` saves and loads; value coercion.
- `editor_document` — the open scene: model + stack + selection + file; every operation
  the UI and command scripts perform.
- `project_env` — project.toml, the scripts compiled into a `ScriptHost` that never
  ticks, and the scene view's preview (the document instantiated as live nodes).
- `command_script` — the headless format above.
- `play_controller` — play-in-editor on `script::GameSession`: live and replay modes,
  fixed-rate ticking, step, recording, seeking, hash verification, panics.
- `asset_library`, `input_map_doc`, `live_tree` — the asset browser's model, the
  input-map editor's model and undo, and reading a running game's node tree.
- `imgui_rhi` — Dear ImGui rendered through `cuervo/rhi` (font atlas and textures,
  16-bit indices, a scissor per draw, shaders in `shaders/` through the engine's shader
  pipeline), so the editor runs on Vulkan and GL with no backend-specific code. The
  scene view renders into the window between ImGui's draws through a draw-list callback.
- `app`, `app_ui` and `*_panel` — the window, frame loop and rendering (`app.cpp`); the
  menu bar, toolbar, dock space and DockBuilder default layout, command palette,
  messages, shortcuts, layouts and the action registry (`app_ui.cpp`); and the panels.
  The panels include `asset_panel`, `input_map_panel`, `debugger_panel`, `replay_panel`,
  `tier_panel`, `profiler_panel` and `history_panel`.
- `theme`, `ui`, `icons`, `messages` — the brand-token ImGui style (every color and metric
  set from the tokens; `tests/editor/editor_ui_tests.cpp` compares it with ImGui's own
  defaults), shared widgets, the vector icons and the message formula.
- `actions`, `layouts`, `projects` — the action registry with fuzzy search (the menus, toolbar
  and palette all run it), saved layouts (dock text per user), recents and templates.
- `launcher`, `eula_view` — the first-launch window (agreement, then the welcome screen)
  with its own device and ImGui context, run before the editor window opens. The EULA
  gate itself is `cuervo/platform/eula.*`.

`editor_core` (everything but the window and ImGui) is a library that
`tests/editor/editor_tests.cpp`, `editor_6b_tests.cpp` and `editor_83_tests.cpp` link headless; the
`editor_level` and `editor_replay` ctests run the shipped binaries.

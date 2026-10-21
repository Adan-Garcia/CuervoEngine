# Cuervo Engine guide

This guide takes you from a fresh checkout to a small finished game: build the engine,
learn how a project is put together, write **Starfall** (dodge the rain, catch the gems)
step by step, then hot-reload it, open it in the editor, debug it and ship it. The
finished game is in [`examples/tutorial/`](../examples/tutorial); run it any time with
`cuervo --game examples/tutorial`.

Reference material:

| Document | What's in it |
|---|---|
| [STATUS.md](STATUS.md) | What is stable, experimental, planned or unsupported, by platform and system |
| [API.md](API.md) | Every engine class and function scripts can use (generated from the engine prelude) |
| [LANGUAGE.md](LANGUAGE.md) | The Pluma language: syntax, types, built-ins |
| [SCENES.md](SCENES.md) | Scene files and their text form |
| [REPLAYS.md](REPLAYS.md) | Recording and verifying runs |
| [EXPORT.md](EXPORT.md) | Packing and exporting a game |
| [editor/README.md](../editor/README.md) | The editor |
| [tools/vscode-pluma](../tools/vscode-pluma/README.md) | The VS Code extension |
| [DESIGN.md](DESIGN.md) | How the engine works inside, and why |

## 1. Install and build

### License

Cuervo Engine is licensed under the [MIT License](../LICENSE.md).

The engine also includes third-party components under their own licenses (zlib, MIT,
Apache-2.0, ...), listed in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) with
their full texts in `third_party/licenses/`. Exported games carry those notices
automatically (`licenses/` next to the player).

### Build

Cuervo builds with CMake presets. You need:

- CMake 3.24+ and Ninja, and GCC 13+ or Clang 17+
- `glslangValidator` (the engine's shaders are compiled with it)
- SDL3 (if it isn't installed, CMake downloads and builds it)
- A Vulkan 1.3 driver for the Vulkan renderer. It is optional: without one the engine
  uses OpenGL 3.3 (or OpenGL ES 3.0 / WebGL2).
- Network access the first time you configure (CMake fetches the other dependencies)

On Arch Linux (the engine's development machine):

```sh
sudo pacman -S cmake ninja glslang sdl3 vulkan-validation-layers   # validation layers: debug builds
```

On Ubuntu 25.04+ / Debian: `sudo apt install cmake ninja-build glslang-tools libsdl3-dev` (and
your GPU's Vulkan driver). Elsewhere, any Linux with the tools above works. Linux and the web are
the supported targets ([STATUS.md](STATUS.md)); Windows is experimental and still being
validated ([WINDOWS.md](WINDOWS.md)); the web build is in [WEB.md](WEB.md).

Build and run the tests:

```sh
cmake --preset debug            # or: release (faster games, no validation)
cmake --build --preset debug
ctest --preset debug
```

The build puts these programs under `build/debug/`:

| Command | Path | What it does |
|---|---|---|
| `cuervo` | `runtime/cuervo` | The player: `cuervo --game <project>` runs a game |
| `cuervo-editor` | `editor/cuervo-editor` | The editor: scenes, inspector, play-in-editor, replays, debugger |
| `cuervo-tools` | `tools/cuervo_tools/cuervo-tools` | Export, packing, scene files, render tiers, formatter, language server, API docs |
| `plm` | `pluma/plm` | The Pluma REPL; also runs and checks single `.plm` files |

The rest of this guide writes them as bare commands. Either use the full paths or put the
directories on your `PATH`:

```sh
export PATH="$PWD/build/debug/runtime:$PWD/build/debug/editor:$PWD/build/debug/tools/cuervo_tools:$PWD/build/debug/pluma:$PATH"
```

Check that everything works by running the example shmup:

```sh
cuervo --game examples/shmup                 # Vulkan, falling back to OpenGL
cuervo --game examples/shmup --backend gl    # force OpenGL
```

Useful `cuervo --game` options: `--backend vulkan|gl`, `--no-audio`, `--quit-after <s>`,
`--screenshot <file.ppm>` (renders tick 240 and exits), `--record <file.replay>`,
`--vsync off`, `--scale fit|integer|stretch`. Without a display, `SDL_VIDEO_DRIVER=offscreen`
runs the GL backend headless.

## 2. A project

**Make one.** The editor's welcome screen has *New project* (Empty 2D or Walker). The same
thing from a terminal, which is what scripts and CI use:

```sh
cuervo-editor --new-project starfall --template walker --name "Starfall"   # --template empty|walker
cuervo-editor --project starfall      # edit it; cuervo --game starfall runs it; section 9 ships it
```

It refuses a folder that already has files, so nothing is overwritten.
`tests/workflow_smoke.cmake` runs this whole path (new project, edit and save a
scene, play, add a script, hot reload, export, launch the shipped game from another
directory) on every build.

A game is a directory:

```
starfall/
  project.toml      the project file (below)
  scripts/          every .plm file under here is compiled, in path order
  scenes/           scene files (.scene): saved node trees
  assets/           sounds, fonts, SVG art; sounds are found by file name
  .import/          generated caches (don't commit)
```

`project.toml`:

| Key | Meaning |
|---|---|
| `name` | The game's name (exports are named after it) |
| `main` | The class of the root node: the engine creates one and adds it to the tree |
| `main_scene` | Instead of `main`: a scene file whose root becomes the root node |
| `virtual_size` | The coordinate space, in virtual pixels; the window scales it |
| `tick_rate` | Simulation ticks per second (default 60) |
| `seed` | The seed of the simulation's random numbers |
| `clear_color` | Background color, hex |
| `jobs` | Job-system threads (0 = automatic); `--jobs` and `CUERVO_JOBS` override it. Results are identical for any count |
| `[scene]` / `transform_hierarchy` | `true` (default): child nodes use parent-relative transforms; `false`: every transform is a world transform |
| `[features]` | Overrides which optional engine features (audio, ui, particles, collision) an export keeps ([EXPORT.md](EXPORT.md)) |
| `render_tier` | `"compatible"` (default) or `"full"`; what each promises and refuses is in [STATUS.md](STATUS.md) "Render tiers" |
| `[input.actions]` | Named actions and the keys/buttons that trigger them |
| `[input.axes]` | Named 2D axes: `Keys.Arrows`, `Keys.WASD`, `Pad.LeftStick`, `Pad.RightStick`, `Pad.Dpad` |

Bindings are spelled `Key.Z`, `Key.Space`, `Key.LeftShift`, `Pad.South` (the A/cross
button), `Pad.Start`, `Pad.LeftStickUp`, `Mouse.Left`, `Wheel.Up`. Scripts only ever
ask about actions (`Input.pressed(#jump)`), never about devices, so players can rebind
everything and replays can record it.

## 3. How a game runs

Everything in a game is a **node** in one tree. The root is your `main` class; nodes
add children with `add_child` and remove themselves with `queue_free()`. Your classes
extend `Node` (no position), `Node2D` (a `position`), `CanvasLayer` (drawn on top) or a
UI control, and override the **lifecycle** methods:

| Method | When |
|---|---|
| `ready()` | Once, when the node enters the tree |
| `update(dt: float)` | Every fixed tick (60 per second by default), parents before children. `dt` is constant |
| `draw(g: Canvas)` | Every rendered frame. Draw here; don't change game state |
| `exit()` | When the node leaves the tree |
| `on_hit(tag: name, count: int)` | Particles hit one of the node's hitboxes this tick |

The simulation is **deterministic**: the same inputs give bit-identical results, which
is what makes replays, hot reload and the debugger reliable. So game code keeps to a
few rules:

- Randomness comes from `Random` (seeded from `project.toml`), never anywhere else.
- Time comes from `dt`, `Time.tick` and `Time.seconds` (simulated time), never a clock.
- Gameplay happens in `update`; `draw` only reads.

Coordinates are virtual pixels with **y pointing down** and the origin at the top-left.
Angles are radians, positive turns clockwise on screen; write degrees as `45deg`.

## 4. Pluma in five minutes

Pluma is statically typed, with braces and no semicolons. The full language is in
[LANGUAGE.md](LANGUAGE.md); this is enough for the tutorial:

```pluma
const MAX_LIVES = 3                        // constants: SCREAMING_SNAKE_CASE
var GOLD = Color.hex("ffd866")             // globals; types are inferred

class Ship extends Node2D {                // single inheritance
    signal died(points: int)               // signals: emit died(10), ship.died.connect(f)
    var speed = 300.0                      // float (f32)
    var hp: int = 3
    var target: Ship                       // class references may be null

    fn hurt(amount = 1) -> bool {          // default arguments
        hp -= amount
        return hp <= 0
    }

    fn update(dt: float) {
        position += Input.axis(#move) * speed * dt      // vec2 math; #move is a name
        if target != null && position.distance(target.position) < 10.0 {
            print("close to {target.name}")             // string interpolation
        }
    }
}
```

- Calls take named arguments: `g.circle(p, 8.0, fill = GOLD, width = 2.0)`.
- `#name` literals are interned names: action names, tags, enum-like options.
- `vec2(x, y)` (up to `vec4` and beyond) with `+ - * /`, `.length()`, `.normalized()`,
  `.distance(b)`, `.clamp(lo, hi)`. `List<T>()` and `Map<K, V>()` are the containers.
- `x is Ship`, `x is Ship s` (type test with a binding), `x as Ship` (checked cast).
- Lambdas: `fn() { lose_life() }`, `fn(v: float) { volume = v }`, or `x => x * 2`.
- **Coroutines**: a method that uses `await` runs over many ticks.
  `start(f())` runs it (returning a `Task` you can `cancel()`); inside it,
  `await wait(1.5)`, `await wait_ticks(3)`, `await until(fn() { done })` or
  `await some_signal` suspend it. A node's coroutines stop when it leaves the tree.
- Panics (null use, index out of range, failed casts...) report `file:line:column`
  and a stack trace instead of crashing the engine.

## 5. Tutorial: Starfall

Rain falls from the sky; touch a drop and you lose a life. Gems fall too; catch them
for points. Three lives, then a game-over menu offers another round.

We'll build it in a new directory, `starfall/`. Every file below is the final version
in `examples/tutorial/`, except the two warm-up steps, which say so.

### Step 1: the project and a first node

Create `starfall/project.toml`:

```toml
# Starfall: dodge the rain, catch the gems. The game docs/GUIDE.md builds
# step by step.
#   cuervo --game examples/tutorial
name = "Starfall"
main = "Game"                  # the root node's class (scripts/game.plm)
virtual_size = [1280, 720]
tick_rate = 60
seed = 20261018
clear_color = "0b1026"
render_tier = "compatible"     # runs on Vulkan, GL and the web

[scene]
transform_hierarchy = false   # script-driven world-space scene (DESIGN.md 4.17)

[input.actions]
pause = ["Key.Escape", "Key.P", "Pad.Start"]
# The engine UI's actions (menus, DESIGN.md 4.10)
ui_up = ["Key.Up", "Key.W", "Pad.DpadUp", "Pad.LeftStickUp"]
ui_down = ["Key.Down", "Key.S", "Pad.DpadDown", "Pad.LeftStickDown"]
ui_left = ["Key.Left", "Key.A", "Pad.DpadLeft", "Pad.LeftStickLeft"]
ui_right = ["Key.Right", "Key.D", "Pad.DpadRight", "Pad.LeftStickRight"]
ui_accept = ["Key.Enter", "Key.Space", "Pad.South"]
ui_cancel = ["Key.Backspace", "Pad.East"]
ui_click = ["Mouse.Left"]

[input.axes]
move = ["Keys.Arrows", "Keys.WASD", "Pad.LeftStick", "Pad.Dpad"]
```

The `ui_*` actions drive the engine's menus (focus moves, accept, click); we'll need
them for the game-over screen. `move` is a 2D axis: `Input.axis(#move)` returns a
`vec2` of length at most 1.

Warm-up (replaced in step 3): `starfall/scripts/game.plm` with a root node that draws
some text.

```pluma
class Game extends Node {
    fn draw(g: Canvas) {
        g.text(Screen.size * 0.5, "Hello, Starfall", size = 48.0, align = #center, baseline = #middle)
    }
}
```

Run it:

```sh
cuervo --game starfall
```

`Screen.size` is the project's `virtual_size`. Escape normally quits, but this project
binds Escape to `pause`, so close the window instead. Leave the game running for the
next steps: it **hot-reloads** every time you save a script.

### Step 2: a player that moves

Add `starfall/scripts/player.plm` (warm-up version; step 4 completes it):

```pluma
class Player extends Node2D {
    var speed = 420.0

    fn ready() {
        position = vec2(Screen.size.x * 0.5, Screen.size.y - 60.0)
    }

    fn update(dt: float) {
        position += Input.axis(#move) * speed * dt
        position = position.clamp(vec2(20.0, 20.0), Screen.size - vec2(20.0, 20.0))
    }

    fn draw(g: Canvas) {
        g.circle(position, 16.0, fill = INK.with_alpha(0.2), stroke = INK, width = 3.0)
        g.circle(position, 5.0, fill = GEM_COLOR)
    }
}
```

and give the game a player (warm-up `game.plm`):

```pluma
var INK = Color.hex("e8ecff")
var GEM_COLOR = Color.hex("ffd866")

class Game extends Node {
    var player: Player

    fn ready() {
        player = Player()
        add_child(player)
    }
}
```

Save, and the ship appears; arrows, WASD or a gamepad move it. Speed times `dt` makes
movement independent of the tick rate. Every name is global across a project's files,
so `player.plm` uses `INK` from `game.plm`, in any file order.

`Canvas` draws vector shapes (sharp at any zoom): `circle`, `rect` (with corner
`radius` and `rotation`), `line`, `arc` and `text`. Colors are `Color.hex("rrggbb")`,
`Color.rgb(r, g, b)` and `.with_alpha(a)`. See [API.md](API.md#canvas).

### Step 3: rain, a root that runs the game

Now the real `game.plm`. Replace the file with the final version; we'll go through it
section by section. (It refers to `Gem` and `Hud`, which steps 5 and 6 add, so the
game reports "unknown type" errors until then. That's fine: a broken edit never stops
the running game, the old code keeps running.)

```pluma
// Starfall: the root node. It owns the rain, the score and the lives, and
// starts and ends rounds (docs/GUIDE.md builds it step by step).

// Collision layers: the bit numbers Emitter `layer`/`hits` and
// Collision.hitbox use.
const LAYER_PLAYER = 0
const LAYER_RAIN = 1

var INK = Color.hex("e8ecff")
var RAIN_COLOR = Color.hex("7fc8ff")
var GEM_COLOR = Color.hex("ffd866")
```

**Particles are the bullets.** The rain is an `Emitter`: a pool of particles the engine
moves, collides and draws natively, thousands at a time. An emitter with a collision
`layer` hits hitboxes on its `hits` layers (a bit mask):

```pluma
class Game extends Node {
    static var main: Game

    var score = 0
    var best = 0
    var lives = 3
    var playing = false
    var player: Player
    var hud: Hud
    var spawner: Task

    // The rain: gameplay particles on LAYER_RAIN that hit hitboxes on
    // LAYER_PLAYER (a hit calls the hitbox owner's on_hit).
    var rain = Emitter(tag = #rain, layer = LAYER_RAIN, hits = 1 << LAYER_PLAYER, shape = #capsule)
    // Sparkles for pickups and hits: particles without collision.
    var sparks = Emitter(tag = #sparks)

    fn ready() {
        Game.main = self
        rain.add(Module.Accelerate(by = vec2(0.0, 160.0)))
        sparks.add(Module.Drag(k = 3.0))
        sparks.add(Module.SizeOverLife(from = 1.0, to = 0.0))
        hud = Hud()
        add_child(hud)
        start_round()
    }
```

`Game.main` is a static field, so other nodes can reach the game. **Modules** are native
per-particle behaviors: the rain accelerates downward like gravity; sparks slow down
and shrink. Other modules turn, home in, orbit, wobble and fade
([API.md](API.md#module)).

A round removes what's left of the last one and starts a **coroutine** that spawns rain:

```pluma
    fn start_round() {
        for c in children {
            if c is Player || c is Gem {
                c.queue_free()
            }
        }
        rain.clear()
        Scene.paused = false
        score = 0
        lives = 3
        playing = true
        player = Player()
        add_child(player)
        spawner = start(run_spawner())
        hud.hide_game_over()
    }

    // A coroutine: a shower of rain, then a pause that gets shorter every
    // time. It runs alongside update() until the round ends.
    fn run_spawner() {
        var delay = 0.7
        loop {
            var x = Random.range(40.0, Screen.size.x - 40.0)
            rain.fire(Pattern.Arc(count = 4, direction = PI / 2.0, spread = 0.6), vec2(x, -20.0),
                      Shot { speed = 140.0, speed_jitter = 40.0, life = 0.0, size = 7.0, color = RAIN_COLOR })
            if Random.chance(0.35) {
                var gem = Gem()
                gem.position = vec2(Random.range(60.0, Screen.size.x - 60.0), -20.0)
                add_child(gem)
            }
            await wait(delay)
            delay = max(delay * 0.97, 0.2)
        }
    }
```

`fire` shoots a **pattern** (`Ring`, `Arc`, `Aimed`, `Burst`) from an origin; `Shot`
says what each particle gets: speed, size, color, lifetime (`0` here: until it leaves the
screen). `direction = PI / 2.0` is straight down, since y points down. The loop would
run forever in a normal function; with `await wait(delay)` it runs one shower per wait,
as part of the simulation, and stops when the round ends (`spawner.cancel()`) or the node
leaves the tree. All randomness is `Random`, so a run replays exactly.

The rest of the game: score, damage, game over and drawing.

```pluma
    fn add_score(points: int) {
        score += points
        best = max(best, score)
    }

    fn burst(at: vec2, color: Color) {
        sparks.fire(Pattern.Burst(count = 24, direction = 0.0, spread = TAU), at,
                    Shot { speed = 160.0, speed_jitter = 60.0, life = 0.5, size = 4.0, color = color })
    }

    // Player.on_hit calls this when the rain gets through.
    fn hurt() {
        lives -= 1
        Audio.play(#hurt, volume = 0.6)
        rain.kill_near(player.position, 160.0)
        burst(player.position, RAIN_COLOR)
        if lives <= 0 {
            game_over()
        }
    }

    fn game_over() {
        playing = false
        spawner.cancel()
        player.queue_free()
        hud.show_game_over(score, best)
    }

    fn draw(g: Canvas) {
        var ground = Screen.size.y - 8.0
        g.line(vec2(0.0, ground), vec2(Screen.size.x, ground), color = INK.with_alpha(0.2), width = 2.0)
        rain.draw(g)
        sparks.draw(g)
    }
}
```

Emitters don't draw themselves: a node's `draw` calls `rain.draw(g)`, which also decides
what they appear above or below (the tree draws parents before children, so the player
and gems are on top of the rain).

### Step 4: getting hit

Replace `player.plm` with the final version. The player adds a **hitbox** every tick
(hitboxes last one tick) unless it's invulnerable, and the engine calls `on_hit` when
rain overlaps it:

```pluma
// The player: moves with the `move` axis, carries a hitbox, and blinks
// while invulnerable after a hit.
class Player extends Node2D {
    var speed = 420.0
    var invulnerable = 0.0

    fn ready() {
        position = vec2(Screen.size.x * 0.5, Screen.size.y - 60.0)
    }

    fn update(dt: float) {
        position += Input.axis(#move) * speed * dt
        position = position.clamp(vec2(20.0, 20.0), Screen.size - vec2(20.0, 20.0))
        // Hitboxes last one tick, so add one every tick you want to be hit.
        if invulnerable > 0.0 {
            invulnerable = max(invulnerable - dt, 0.0)
        } else {
            Collision.hitbox(self, position, 12.0, LAYER_PLAYER)
        }
    }

    fn on_hit(tag: name, count: int) {
        if tag == #rain && invulnerable == 0.0 {
            invulnerable = 1.5
            Game.main.hurt()
        }
    }

    fn draw(g: Canvas) {
        // Blink: hidden 4 ticks out of 8 while invulnerable.
        if invulnerable > 0.0 && (Time.tick / 4) % 2 == 0 {
            return
        }
        g.circle(position, 16.0, fill = INK.with_alpha(0.2), stroke = INK, width = 3.0)
        g.circle(position, 5.0, fill = GEM_COLOR)
    }
}
```

The rain particles that hit die (`kill_on_hit` is on by default), and `on_hit` gets the
emitter's tag and how many hit. The blink uses `Time.tick`, not a clock, so it looks the
same in a replay.

### Step 5: gems, a node per pickup

Gems are few and each has a little behavior, so they're nodes rather than particles.
`starfall/scripts/gem.plm`:

```pluma
// A falling gem: touch it for a point.
class Gem extends Node2D {
    var fall_speed = 110.0
    var spin = 0.0

    fn update(dt: float) {
        position.y += fall_speed * dt
        spin += 3.0 * dt
        var game = Game.main
        if game.player.is_alive() && position.distance(game.player.position) < 30.0 {
            game.add_score(1)
            game.burst(position, GEM_COLOR)
            Audio.play(#pickup, volume = 0.5)
            queue_free()
        } else if position.y > Screen.size.y + 20.0 {
            queue_free()
        }
    }

    fn draw(g: Canvas) {
        g.rect(position - vec2(9.0, 9.0), vec2(18.0, 18.0), fill = GEM_COLOR, radius = 3.0, rotation = PI / 4.0 + spin)
    }
}
```

`queue_free()` removes the node at the end of the tick. `is_alive()` is false once the
player has been removed (after a game over).

**Sound.** `Audio.play(#pickup)` plays `assets/**/pickup.wav`: every sound under
`assets/` is registered by its file name (WAV, OGG, FLAC or MP3; files under
`assets/music/` stream and loop). Copy `examples/tutorial/assets/sfx/` (CC0 sounds) into
`starfall/assets/sfx/`. Unknown sounds only log a warning, and games behave the same
with `--no-audio`: scripts never read back what the audio is doing. Music is
`Audio.music(#track)`; buses and volumes are in [API.md](API.md#audio).

### Step 6: the HUD and a game-over menu

The HUD is a `CanvasLayer`: it and its children draw after (on top of) the rest of the
tree. The game-over menu is made of UI nodes saved in a **scene file**.

Scene files are binary and usually made in the editor (section 7). They also have a
text form; save this as `starfall/game_over.txt`:

```
scene version 2 schema 0000000000000000
node CanvasLayer "game_over" {
    layer: int = 20
    node Control "screen" {
        anchor_max: vec2 = (1.0, 1.0)
        node Panel "panel" {
            anchor_min: vec2 = (0.5, 0.5)
            anchor_max: vec2 = (0.5, 0.5)
            node VBox "items" {
                min_size: vec2 = (360.0, 0.0)
                node Label "heading" {
                    text: str = "GAME OVER"
                    style: name = #title
                    align: name = #center
                }
                node Label "score" {
                    text: str = "SCORE 0"
                    align: name = #center
                }
                node Button "retry" {
                    text: str = "Play again"
                }
            }
        }
    }
}
```

and build it (the schema hash is filled in):

```sh
mkdir -p starfall/scenes
cuervo-tools scene build starfall/game_over.txt starfall/scenes/game_over.scene
cuervo-tools scene dump starfall/scenes/game_over.scene     # back to text
```

The `screen` control's anchors stretch it over the whole screen; the panel's anchors
(0.5, 0.5) center it, sized to fit its content. A `VBox` stacks its children. Fields
like `text` and `style` are the nodes' `@export` fields ([API.md](API.md#label)).

`starfall/scripts/hud.plm`:

```pluma
// The HUD and the game-over menu: a CanvasLayer, so it draws on top of the
// game. The menu's layout is a scene file made of UI nodes.
class Hud extends CanvasLayer {
    var menu: CanvasLayer

    fn ready() {
        // Keeps updating while the game is paused, so it can unpause it.
        always_process = true
        layer = 10
        menu = Scene.load("scenes/game_over.scene") as CanvasLayer
        menu.visible = false
        add_child(menu)
        (menu.find("retry") as Button).pressed.connect(fn() { Game.main.start_round() })
    }

    fn update(dt: float) {
        if Game.main.playing && Input.pressed(#pause) {
            Scene.paused = !Scene.paused
        }
    }

    fn show_game_over(score: int, best: int) {
        (menu.find("score") as Label).text = "SCORE {score}   BEST {best}"
        menu.visible = true
        // The key that ended the round mustn't also press the button.
        UI.consume_input()
        (menu.find("retry") as Control).grab_focus()
    }

    fn hide_game_over() {
        menu.visible = false
        UI.focused = null
    }

    fn draw(g: Canvas) {
        var game = Game.main
        g.text(vec2(24.0, 18.0), "SCORE {game.score}", size = 28.0, color = INK)
        g.text(vec2(24.0, 54.0), "BEST {game.best}", size = 16.0, color = INK.with_alpha(0.6))
        for i in 0..game.lives {
            g.circle(vec2(Screen.size.x - 30.0 - (i as float) * 28.0, 34.0), 9.0, fill = GEM_COLOR)
        }
        if Scene.paused {
            g.text(Screen.size * 0.5, "PAUSED", size = 48.0, color = INK, align = #center, baseline = #middle)
        }
    }
}
```

- `Scene.load` builds the saved tree; `find` looks nodes up by name; the button's
  `pressed` **signal** is connected to a lambda.
- **Pause.** While `Scene.paused` is true only nodes with `always_process` (and their
  children) update, their coroutines alone advance and particles freeze. The HUD sets
  `always_process` so that it can unpause.
- The focused button responds to `ui_accept` (Enter, Space, the gamepad's South button)
  and to a mouse click.

Save everything: the game compiles again and Starfall is complete. Run it from the start
with `cuervo --game starfall` (or press F6 in the running game to restart it).

## 6. Hot reload

`cuervo --game` watches `scripts/`: save a file and the game reloads it **without
restarting**. Objects keep their fields; new fields get their initializers; changed
methods run from the next tick. Try it: change `RAIN_COLOR` or the rain's
`speed = 140.0` while playing and save. A file that doesn't compile is reported with
`file:line:column` and the game keeps running the old code. F5 forces a reload and F6
restarts the game from scratch. `--no-hot-reload` turns watching off.

## 7. The editor

```sh
cuervo-editor --project starfall
cuervo-editor --project starfall --scene scenes/game_over.scene
```

Godot-style: the **Scene** tree on the left, the **Scene View** in the middle (drawn by
the engine's renderer, so it looks like the game), the **Inspector** on the right with
every `@export` field of the selected node, and **Console**, **Assets**, **Replay**,
**Input Map** and **Tier Report** at the bottom. Open `scenes/game_over.scene`, select
the `retry` button and change its text, or drag the panel; Ctrl+S saves. *+ Add* lists
every node class, including the ones your scripts define. The Input Map panel edits
`[input.actions]` in place.

**F5** plays the game inside the editor (exactly as `cuervo --game` runs it), F6 plays
the open scene, F7 pauses, F10 steps one tick, Shift+F5 stops and puts the scene back as
it was. Saving a script while playing hot-reloads it. *Play and Record* records a replay
you can scrub through later ([REPLAYS.md](REPLAYS.md)). Details: [editor/README.md](../editor/README.md).

### UI widgets

Menus are `Control` trees on a `CanvasLayer`. Besides `VBox`, `HBox`, `Margin`, `Panel`,
`Center`, `Label`, `Button`, `Toggle` and `Slider`, the widget set also has:

| Widget | Use |
|---|---|
| `GridContainer` | `columns`; inventory slots, forms (label, field pairs) |
| `FlowContainer` | tags and wrapping rows of equal things |
| `SplitContainer` | two panes with a draggable, focusable bar |
| `AspectRatioContainer` | keeps a child at `ratio` (`#fit` or `#cover`) |
| `ScrollContainer` | one child in a clipped view; wheel, `ui_scroll_up/down`, bar, drag, focus follows |
| `Image` / `TextureRect` | a vector asset in a layout rect (`stretch`, `tint`) |
| `ProgressBar` | `value` between `min_value` and `max_value` |
| `LineEdit` | one line of text typed from the input frame (so it replays) |
| `OptionButton` | a dropdown of `items`; `item_selected(index)` |
| `TabBar`, `TabContainer` | tabs; children of a `TabContainer` are the pages |
| `Popup`, `PopupMenu`, `ConfirmDialog` | floating panels that trap the focus while open |

```
var tabs = TabContainer()
tabs.add_child(audio_page())   // a VBox named "Audio"
var name = LineEdit()
name.max_length = 12
name.text_changed.connect(fn(t: str) { Player.name = t })
var dialog = ConfirmDialog()
dialog.message = "Apply these settings?"
dialog.confirmed.connect(fn() { apply() })
layer.add_child(dialog)
dialog.popup_centered()
```

Bind the optional actions you need in `project.toml`: `ui_scroll_up = ["Wheel.Up",
"Pad.RightStickUp"]`, `ui_backspace`, `ui_delete`, `ui_home`, `ui_end`, `ui_tab_prev`,
`ui_tab_next`. Keep `ui_*` navigation on keys that type no characters (arrows, Enter, Escape)
if the menu has a `LineEdit`. Give controls an explicit `min_size` where recorded replays
click them: text-measured sizes differ between hosts with and without a font.
`examples/ui_menus` is a settings menu and inventory built from all of them; the
`tests/ui_menus.cmake` test drives it by keyboard, gamepad and pointer.

### Built-in nodes

Besides `Node`, `Node2D`, `SceneInstance`, `CanvasLayer` and the UI controls, the
engine ships nodes you configure entirely in the Inspector (every field is an
`@export`), so a level can be built without writing code. `examples/node_level` uses
all of them and has no scripts at all.

| Node | What it does | Main fields / signals |
|---|---|---|
| `Shape` | an SDF circle, rect (rounded), capsule, ring, arc or sector | `kind`, `radius`, `size`, `corner_radius`, `thickness`, `start_angle`/`end_angle`, `filled`, `fill`, `stroke`, `stroke_width` |
| `Path` | a stroked polyline or cubic Bézier | `points` (offsets from `position`), `curve`, `closed`, `width`, `color`, `segments` |
| `Sprite` | an SVG (or exported `.evec`), sharp at any zoom | `asset`, `centered`, `flip_h`/`flip_v`, `tint` |
| `Text` | MSDF text in the world | `text`, `size`, `color`, `align`, `baseline`, `max_width` |
| `Camera2D` | the view the game draws with (HUD layers ignore it) | `zoom`, `current`; `make_current()`, `screen_to_world(p)` |
| `Timer` | fires after whole ticks | `wait_time`, `one_shot`, `autostart`, `paused`; `signal timeout`, `restart()` |
| `Area` | a collision circle/capsule (needs collision) | `shape`, `radius`, `length`, `layer`, `mask`; `area_entered`/`area_exited(other)`, `hit(tag, count)` |
| `Emitter2D` | a particle emitter (needs particles) | `pattern`, `count`, `interval`, `speed`, `life`, `color`/`fade`/`end_color`, `gravity`, `layer`/`hits` |
| `AudioPlayer` | plays a sound (needs audio) | `sound` or `file`, `volume`, `pitch`, `pan`, `bus`, `looping`, `autoplay`; `play()`, `stop()` |
| `Line2D` | a polyline with round or mitered joints | `points`, `width`, `color`, `gradient`, `closed`, `joint` |
| `Polygon2D` | a filled polygon (concave is fine) with optional outline | `points`, `color`, `outline`, `outline_width`, `filled` |
| `Marker2D` | a point in the tree, drawn as a cross in the editor only | `gizmo_size` |
| `VisibleOnScreenNotifier2D` | tells when a rectangle is on screen | `rect_position`, `rect_size`, `enabled`; `signal screen_entered`/`screen_exited`, `is_on_screen()` |

Child nodes inherit their parent's transform: a `Node2D`'s `position`, `rotation` and
`scale` are relative to its parent, so moving a parent moves everything under it.
`global_position()`, `global_rotation()`, `global_scale()`, `to_global(p)` and
`to_local(p)` convert, and `set_global_position(p)` places a node by world position.
Set `top_level = true` on a node to ignore its parent, or put
`[scene]` / `transform_hierarchy = false` in project.toml to make every transform a
world transform (the old behavior; the example games written that way set it).
`Line2D` and `Polygon2D` (concave shapes are fine) draw a polyline and a polygon,
`Marker2D` marks a point in the editor, and `VisibleOnScreenNotifier2D` emits
`screen_entered` / `screen_exited` as its rectangle crosses the camera view. In a script you connect
their signals like any other: `(find("beat") as Timer).timeout.connect(fn() { ... })`. If you subclass one and
override `ready()` or `on_hit()`, call `super.ready()` / `super.on_hit(tag, count)`.
The full reference is [API.md](API.md).

### Animation

Everything animated is measured in **ticks** (60 per second by default), so animations
replay bit for bit. `Anim.ticks(0.5)` turns seconds into ticks. `examples/anim_character`
is a character whose body is a node hierarchy (torso, head, arms, legs) animated by an
`AnimationPlayer` and blended by an `AnimationTree`.

**`AnimationPlayer`** plays `Animation`s: tracks over any `@export` field of any node
under the player's parent (`root_path` changes the root). A property track is
`"NodePath:field"`, e.g. `"Torso/Head:rotation"`; a key has a tick, a value and how it
reaches the next key (`Step`, `Linear`, `EaseIn`/`EaseOut`/`EaseInOut` with an `Ease`
family, or `Curve` with a power exponent), so each key can ease differently. `float`,
`int`, `bool`, `vec2`/`vec3`/`vec4`, `Color`, enum and `name` fields work; the field's own
type decides how the value is written.

```
var wave = Animation { name = "wave", length = 60, mode = AnimLoop.Loop }
wave.key_float("Arm:rotation", 0, -0.5, AnimInterp.EaseInOut, Ease.Sine)
wave.key_float("Arm:rotation", 30, 0.5, AnimInterp.EaseInOut, Ease.Sine)
wave.key_float("Arm:rotation", 60, -0.5)
wave.key_color("Arm/Hand:fill", 0, Color.hex("ffffff"))
wave.key_call("Arm:flash", 30)           // calls Arm.flash() when tick 30 is crossed
wave.key_event(30, "wave_peak")          // emits player.event("wave_peak")
player.animations.push(wave)
player.play("wave")                      // or set `autoplay = "wave"` before it enters the tree
```

`mode` is `None` (stops on the last tick and emits `animation_finished(name)`), `Loop` or
`PingPong`. `speed` scales ticks (negative plays backwards), `play(name, speed)` scales
one play, `queue(name)` plays after the current animation finishes, and there are
`seek(t)` and `stop()`. The player advances before its siblings' `update()`, so scripts see
the animated values.

**`Tween`**: `create_tween()` on a node returns a chainable tween that runs with that node
and stops when it leaves the tree. Steps run in sequence, `parallel()` runs the next one
with the previous (or `set_parallel(true)` runs all together), `set_loops(n)` repeats,
`kill()` cancels, and a coroutine can `await tween.finished`.

```
create_tween()
    .tween_vec2(coin, "position", vec2(300.0, 200.0), 30).with_ease(AnimInterp.EaseOut, Ease.Cubic)
    .tween_float(coin, "rotation", TAU, 30).parallel()
    .interval(10)
    .tween_callback(fn() { coin.queue_free() })
```

**`AnimationTree`** is a state machine over a player's animations. Each state plays one
animation; transitions carry conditions on parameters and a cross-fade length in ticks, and
the animated properties of the outgoing and incoming animations are blended while it runs
(numbers, vectors and colors blend; bools, enums and names switch half way).

```
tree.set_bool("moving", true)    // parameters: set_param / set_bool
tree.trigger("jump")             // a one-evaluation pulse that a Trigger condition consumes
tree.goto("idle", 6)             // jump to a state, fading over 6 ticks (0 = cut)
tree.travel("run")               // walk the shortest chain of transitions
```

Animations, the player and the tree are saved in the scene file as exported fields, so
they can be built in code and saved, or written in the scene's text view
([SCENES.md](SCENES.md)). A track that one state animates and another doesn't keeps the
other side's value while blending, so give the animations of one character the same set
of tracks. The rules, and how other track kinds (cutout, skeletal, morph) plug in, are in
DESIGN.md 4.17.

## 8. Debugging and profiling

**Panics** print the message and the script stack:

```
[error] panic in update(): scripts/game.plm:6:19: panic: null reference: 'target' is null
    at Game.update (scripts/game.plm:6:19)
    at Node.update_tree (cuervo/scene.plm:129:13)
```

The game stops updating; fix the script and save (or F6 to restart). In the editor a
panic pauses the game and the **Debugger** tab shows the stack with every frame's local
variables, plus the live node tree with all fields. `print(x)` writes to the log and the
editor's Console.

**Breakpoints in VS Code.** Install the Pluma extension
([tools/vscode-pluma](../tools/vscode-pluma/README.md)), open the project folder and add
`.vscode/launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "pluma",
      "request": "launch",
      "name": "Run game (Pluma)",
      "project": "${workspaceFolder}"
    }
  ]
}
```

Optional attributes: `"backend": "vulkan"` or `"gl"`, and `"stopOnEntry": true` to pause
at the first script line that runs. Set the `pluma.runtimePath` setting to your `cuervo` if it isn't on
`PATH`. Click in the gutter of a `.plm` file to set a breakpoint and press F5: the
extension runs `cuervo --dap` and the game stops on the line, with the call stack, locals
and stepping. A panic also stops there (the "Panics" exception filter, on by default),
before the stack unwinds, so you see the locals of the frame that failed. Watch and hover
evaluate variable paths such as `self.lives` or `Game.main.score`; they never run script
code, so inspecting can't change the game. Debugging works on the bytecode VM (`cuervo
--game` and the editor), not on AOT-native exports (`--aot`).

**Profiling.** `cuervo --game <dir> --profile` prints a per-function script profile
and engine phase timings at exit; the editor has a Profiler panel.
`tools/bench.py` records repeatable frame, simulation, memory and export-size baselines and
finds how many particles hold 60 fps ([PERFORMANCE.md](PERFORMANCE.md)).

**Editor support.** The same extension gives `.plm` files highlighting and the language
server (`cuervo-tools lsp`): errors as you type, completion including the engine API,
hover documentation, go to definition, rename. `cuervo-tools fmt <file>` prints a
script in the standard style ([LANGUAGE.md](LANGUAGE.md) §6, §9.15).

## 9. Shipping

```sh
cuervo-tools export starfall --platform linux -o dist/ --strip
./dist/starfall
```

`dist/` holds the player (named after the project, lowercased, spaces as `-`: `My Game` ships as
`my-game`; `--name` picks another) and `game.pak`, one archive with the
scripts, scenes and sounds; nothing else is needed. On Linux the player is built for the
game: subsystems it doesn't use (audio, UI, particles, Vulkan, ...) are left out, which
makes it 20-40% smaller ([EXPORT.md](EXPORT.md) "Stripped players"; `--engine full`
ships the complete player instead). Export from a **release** build
(`cmake --preset release && cmake --build --preset release`, then use
`build/release/tools/cuervo_tools/cuervo-tools`) so you ship an optimized player. The
export checks the project's render tier and target first (`cuervo-tools tier check starfall --platform web`) and
leaves out engine features the game doesn't use.

- `--aot` compiles the scripts to native code (faster; needs a C compiler).
- `--platform web` produces an `index.html` + WebAssembly build (see
  [EXPORT.md](EXPORT.md) and [WEB.md](WEB.md) for the one-time web runtime build).

## 10. Where next

- [API.md](API.md): everything the engine offers scripts, generated from the source.
- The examples: `examples/shmup` (waves, bosses, menus from scene files, options, music),
  `examples/tictak` (a board game with mouse input), `examples/editor_level` (a level made
  only in the editor, no main script), `examples/node_level` (every built-in node, no
  scripts), `examples/ui_menus` (a settings and inventory menu from the UI widgets),
  `examples/bench` (a stress test) and `examples/tutorial` (the game from this guide).
- [STATUS.md](STATUS.md): what is stable, experimental, planned or unsupported.
- [LANGUAGE.md](LANGUAGE.md) for enums and `match`, structs, traits, generics, `foreach`
  batch loops and manual memory.

# The Pluma guide

Pluma is the scripting language of Cuervo Engine: statically typed, brace-delimited, and
built for games that must play the same way every time. This guide teaches it in the
order you meet things when you write a game. It favors short examples over completeness:
[the language reference](LANGUAGE.md) is the exact specification (every rule, every
corner), and [the engine API](API.md) lists what the engine gives your scripts.

If you would rather learn by building a game, start with the
[Starfall tutorial](GUIDE.md#5-tutorial-starfall) and come back here when you want to
know why something works the way it does. [Pluma in five minutes](GUIDE.md#4-pluma-in-five-minutes)
is the short tour.

Examples that use only the language run as they are in the REPL (`plm`) or as
`plm file.plm` (which calls `main()`). Examples that draw or read input need the engine:
put them in a project's `scripts/` folder (see [Getting started](GUIDE.md#2-a-project)).

## 1. Files, projects and nodes

A Pluma file ends in `.plm`. A game is a folder with a `project.toml` and a `scripts/`
folder; every `.plm` file in it is checked as **one program**, so names are global and
files may refer to each other in any order. `project.toml` names the **main class**, the
root of the game's node tree:

```toml
name = "Hello"
main = "Game"
```

```pluma
class Game extends Node {
    fn ready() {
        print("hello from the first tick")
    }

    fn update(dt: float) {
        // called every tick, 60 times a second by default
    }
}
```

A **node** is an object in the game's tree. `Node` has three hooks you override:
`ready()` once when the node enters the tree, `update(dt)` on every simulation tick with
the tick length in seconds, and `draw(g: Canvas)` on every rendered frame. Keep game logic
in `update` and drawing in `draw`: the simulation is deterministic (the same inputs give
the same game, tick for tick), and drawing is not part of it.

Scripts hot-reload while the game runs: save a file and the new code takes over, keeping
your nodes' state. A broken edit never stops the running game; the old code keeps running
and the error shows in the console.

## 2. Values and variables

```pluma
fn main() {
    var hp = 100                // an int, inferred
    var speed = 2.5             // a float (32-bit)
    var alive = true
    var title = "Hello"
    var tag = #player_hit       // an interned name: cheap to compare, great for events
    const MAX_HP = 400          // a compile-time constant

    hp -= 30
    print("hp is {hp}, {hp * 100 / MAX_HP}% of max")
}
```

There is no `let`: `var` is mutable and `const` is constant. Types are inferred from the
initializer, and you can always write one: `var dir: vec2`. A variable declared without a
value holds the zero value of its type (`0`, `0.0`, `false`, `""`, `null`).

**Numbers.** `int` is 64-bit, `float` is 32-bit; there are also sized types (`i8` to `i64`,
`u8` to `u64`, `f32`, `f64`) when you need them. Literals take the type their context
expects, so `var x: float = 3` works. Integer overflow panics in development builds and
wraps in release builds: it finds your bugs without costing you speed later. Convert
between numbers with `as`: `hp as float`.

**Angles are radians.** The `deg` suffix converts at compile time: `45deg` is a `float`
holding 0.7853982. World coordinates are Y-down with the origin at the top left, so
`vec2(0.0, -1.0)` points up the screen and a positive rotation turns clockwise.

**Strings** are immutable UTF-8 and compared by content. `"{expr}"` interpolates any
expression; write `{{` and `}}` for literal braces. `r"raw\n"` has no escapes, and
`"""` ... `"""` is a multiline string that strips its indentation.

**Names** (`#shoot`) are interned strings. Use them for actions, tags and states; the
engine's input API takes them: `Input.held(#shoot)`.

**Vectors.** `vec2` to `vec64` hold floats and do component-wise math:

```pluma
fn main() {
    var p = vec2(1.0, 2.0)
    var v = vec2(3.0, 0.5)
    p += v * 0.5                 // 2.5, 2.25
    print(p.length())
    print(p.yx)                  // swizzle: vec2(2.25, 2.5)
    var q = vec3(p, 5.0)         // compose
    print(q.xy.normalized())
}
```

**Colors** are not literals: `Color.hex("ff4060")`, `Color.rgb(1.0, 0.25, 0.4)`, and
`.with_alpha(0.5)`.

## 3. Functions

```pluma
fn add(a: int, b: int) -> int {
    return a + b
}

fn ring(count: int, speed = 150.0, spin = 0.0) {
    print("{count} shots at {speed}, spinning {spin}")
}

fn main() {
    print(add(2, 3))
    ring(36)
    ring(36, spin = 12.0)
    ring(count = 8, speed = 200.0)
}
```

Parameters need a type unless a default value gives them one. Defaults go after the
required parameters. Callers pass positional arguments first and named ones after; naming
an argument is how a call like `g.circle(pos, 16.0, fill = INK, width = 3.0)` stays readable.
A function that returns nothing has no `->`.

**Lambdas** are values. `fn(x: int) -> int { return x * 2 }` is the full form; `x => x * 2`
is the shorthand when the type is known from context:

```pluma
fn main() {
    var enemies = [3, 0, 5, 0, 1]
    var alive = enemies.filter(hp => hp > 0)
    var doubled = alive.map(hp => hp * 2)
    print(doubled)                          // [6, 10, 2]
    var total = doubled.reduce(0, (acc, x) => acc + x)
    print(total)                            // 18
}
```

A lambda captures the variables around it: a `var` by reference (one shared cell), so a
lambda that increments a counter changes the counter everyone sees.

Methods can be used as values too: `var f = boss.hurt` captures `boss` and calls `hurt`
on it later.

## 4. Control flow

```pluma
fn describe(hp: int) -> str {
    if hp <= 0 {
        return "dead"
    } else if hp < 20 {
        return "hurt"
    }
    return "fine"
}

fn main() {
    for i in 0..3 {                    // 0, 1, 2
        print("{i}: {describe(i * 10)}")
    }
    for i in 10..=0 step -5 {          // 10, 5, 0
        print(i)
    }
    var n = 3
    while n > 0 {
        n -= 1
    }
    outer: for y in 0..4 {             // labels name the loop to break
        for x in 0..4 {
            if x * y == 6 { break outer }
        }
    }
}
```

Conditions are plain booleans (no truthiness), and `=` is a statement, never an
expression, so `if x = 1` is an error with a fix-it to `==`. Loops are `for` (over
ranges and collections), `while` and `loop`. `defer` runs a statement when the block
exits, however it exits:

```pluma
fn countdown() {
    print("start")
    defer print("done")                // runs however the function exits
    for i in 0..5 {
        if i == 2 { return }
        print(i)
    }
}
```

### Match

`match` checks a value against patterns and must be exhaustive, so adding an enum variant
makes the compiler point at every `match` that forgot it:

```pluma
enum Phase {
    Opening,
    Spiral(arms: int),
    Rage,
}

fn run(phase: Phase) {
    match phase {
        Phase.Opening => print("opening")
        Phase.Spiral(arms) if arms > 3 => print("hard spiral of {arms}")
        Phase.Spiral(arms) => print("spiral of {arms}")
        Phase.Rage => print("rage!")
    }
}

fn bucket(n: int) -> str {
    match n {
        0 | 1 => return "low"
        2..=9 => return "mid"
        _ => return "high"
    }
}
```

### Nullable values

A reference (a class, a `List`) can always be `null`; a value type becomes optional with
`T?`. Three operators deal with it, and they are spelled so you can see which is which:

```pluma
class Target {
    var hp = 10
}

fn main() {
    var target: Target = null
    print(target?.hp)                  // null: ?. stops at null
    var hp = target?.hp ?? 0           // ?? supplies a default
    target = Target()
    if target? t {                     // binds t only when it is not null
        t.hp -= 1
    }
    print(hp)
}
```

Spacing matters: `a ?? b` and `a ? b : c` have spaces, `x?.y` and a propagating `x?` do
not (more on `?` in [Errors](#6-errors-without-exceptions)). The formatter fixes it for you.

## 5. Classes, structs, enums and traits

### Classes

A class is a reference type: copies of a variable share one object.

```pluma
class Boss {
    signal defeated
    signal hp_changed(value: int)

    @export(range = 0..1000)
    var max_hp = 400

    var hp = 400 {
        set(v) {
            hp = clamp(v, 0, max_hp)       // in its own setter, `hp` is the raw field
            emit hp_changed(hp)
            if hp == 0 { emit defeated }
        }
    }

    var ratio: float {                     // computed: no storage
        get { return hp as float / max_hp as float }
    }

    var _phase = 0                         // leading _ makes it private to the class

    fn hurt(amount: int) {
        hp -= amount
    }
}
```

- `fn init(...)` is the constructor; `Boss()` runs the field initializers and then `init`.
- `class Boss extends Enemy` inherits; methods are virtual unless `final`, and
  `super.method()` calls the parent's.
- `@export` shows a field in the editor's inspector and saves it in scene files.
- Properties (`get` / `set`) look like fields from the outside, so you can add validation later
  without touching the callers.
- `static var` and `static fn` belong to the class, not an instance.

### Structs

A struct is a value: assigning it copies it. Use structs for small bundles of data.

```pluma
struct Bullet {
    var pos: vec2
    var vel: vec2
    var ttl = 3.0
}

fn main() {
    var a = Bullet { pos = vec2(0.0, 0.0), vel = vec2(1.0, 0.0) }
    var b = a                      // a copy
    b.ttl = 1.0
    print(a.ttl)                   // 3.0: a is unchanged
}
```

### Enums

Plain enums are integers underneath; enums with payloads carry data per variant (as `Phase`
above). `enum Dir: u8 { Up = 0, Down = 1 }` fixes the numbers.

### Traits and operators

A trait is a set of methods a class or struct promises to have. Operators are traits too,
so your types can use `+`, `==` and `<`:

```pluma
trait Damageable {
    fn hurt(amount: int)
    fn is_dead() -> bool { return false }     // a default
}

struct Complex: Add, Eq {
    var re: float
    var im: float
    fn add(o: Complex) -> Complex {
        return Complex { re = re + o.re, im = im + o.im }
    }
    fn eq(o: Complex) -> bool {
        return re == o.re && im == o.im
    }
}

fn main() {
    var a = Complex { re = 1.0, im = 2.0 }
    var b = a + a
    print(b.re)                              // 2.0
    print(b == a)                            // false
}
```

### Generics

```pluma
fn biggest<T: Ord>(a: T, b: T) -> T {
    return a > b ? a : b
}

fn main() {
    print(biggest(3, 9))
    print(biggest("apple", "pear"))
}
```

## 6. Errors without exceptions

Pluma has no exceptions. A **bug** (an index out of range, a null you did not expect, a
failed `assert`) panics with a file, line and stack trace. An **expected failure** returns a
`Result<T, E>`:

```pluma
fn parse_port(text: str) -> Result<int, str> {
    var n = parse_int(text) ?? 0            // parse_int gives an int?: null if it isn't a number
    if n < 1 || n > 65535 {
        return Err("not a port number: {text}")
    }
    return Ok(n)
}

fn describe(text: str) -> Result<str, str> {
    var port = parse_port(text)?            // on Err, return it from this function
    return Ok("port {port}")
}

fn main() {
    print(describe("8080"))                 // Ok("port 8080")
    print(describe("http"))                 // Err("not a port number: http")
    match parse_port("70000") {
        Ok(p) => print("fine {p}")
        Err(e) => print("failed: {e}")
    }
}
```

`?` right after an expression (no space) means "unwrap, or return early". It works on
`Result` and on nullable values. Number parsing returns an optional rather than a `Result`,
because "not a number" is the only way it can fail.

## 7. Collections

```pluma
fn main() {
    var xs = [5, 3, 8]
    xs.push(1)
    xs.sort()
    print(xs)                               // [1, 3, 5, 8]

    var scores = Map<str, int>()
    scores["ana"] = 10
    scores["ana"] += 5
    print(scores.get_or("bo", 0))           // 0
    for name, score in scores {             // insertion order, always
        print("{name}: {score}")
    }

    var seen = Set<int>()
    print(seen.add(3))                      // true: it was new
    print(seen.add(3))                      // false

    var q = PriorityQueue<int>()
    q.push(9)
    q.push(2)
    print(q.pop())                          // 2: smallest first
}
```

`List<T>`, `Map<K, V>` and `Set<T>` are references; `[T; N]` fixed arrays are values like
structs. There is also `Deque<T>`, `PriorityQueue<T>` and `StrBuilder`. Iteration order is
always defined (insertion order for maps and sets), so a script that loops over a map is
still deterministic. Changing a map or set while looping over it panics at the next step:
loop over `m.keys()` when you want to remove entries.

## 8. Time: coroutines and signals

A function that uses `await` is a **coroutine**: it pauses and the game keeps running.
This makes sequences read top to bottom instead of as a state machine.

```pluma
class Player extends Node2D {
    signal died
    var lives = 3
    var invulnerable = false

    fn hurt() {
        if invulnerable { return }
        lives -= 1
        if lives <= 0 {
            emit died
            return
        }
        start(blink())              // run it alongside everything else
    }

    fn blink() {
        invulnerable = true
        for i in 0..10 {
            visible = !visible
            await wait(0.1)         // 0.1 s of game time
        }
        visible = true
        invulnerable = false
    }
}
```

- `await wait(seconds)`, `await wait_ticks(n)`, `await until(() => cond)`, another coroutine
  call, a `Task`, or a signal (`await boss.defeated`).
- `start(f(args))` runs a coroutine and returns a `Task` you can `cancel()`. Calling a
  coroutine directly is an error: you either `start` it or `await` it.
- `wait` counts **simulation time**, not wall-clock time, so pausing the game pauses your
  coroutines and a replay runs them identically.
- A `signal` is declared in a class and fired with `emit`; other code subscribes with
  `obj.name.connect(handler)`. Signals are how a node tells the rest of the game something
  happened without knowing who is listening.

## 9. Memory and speed

Most code never thinks about memory: objects are **reference counted**, and a periodic
**cycle collector** (at the end of each tick) frees objects that only point at each other.
Two tools keep that cheap:

- A `weak var owner: Boss` field does not keep its target alive and reads as `null` after
  the target is freed. Use it for back-pointers (a child pointing at its parent).
- `@acyclic` on a class promises its instances never form cycles, so the collector skips
  them. Debug builds verify the promise.

For the few places that need control, there is manual memory with checked references:
`new(frame) List<vec2>()` allocates in an arena that is cleared at the end of the frame,
`alloc` / `free` allocate and release one object explicitly, and a use after free panics
with the place it was freed (see the [reference](LANGUAGE.md#914-memory)).

For work over thousands of things, such as particles, use **`foreach`**. It runs a batch loop over
a particle pool or a `Pool<T>`, and the compiler lowers it to a tight loop when the body
allows it (no allocation, no `await`, no calls it cannot inline). When it cannot, the loop
still works and the compiler warns you with the line and the reason; `@allow_slow` silences
the warning.

```pluma
fn update(dt: float) {
    foreach p in trail.pool {
        p.pos += p.vel * dt
        if p.age >= p.life { trail.pool.kill(p) }
    }
}
```

## 10. Tools

- **The formatter**: `cuervo-tools fmt <file-or-project> --write` (or `plm fmt`) applies the
  [style guide](LANGUAGE.md#6-style-guide-enforced-by-the-formatter): four spaces,
  braces on the same line, trailing commas in multiline lists. `--check` is for CI.
- **The REPL**: `plm` evaluates declarations and statements as you type them. `:check`
  parses without running.
- **The editor and VS Code**: both run the same language server: diagnostics as you type,
  hover with the doc comments of the engine API, go to definition, rename, completion and
  signature help. See [the VS Code extension](../tools/vscode-pluma/README.md) and
  [the editor](../editor/README.md).
- **The debugger and profiler**: breakpoints, stepping and a profiler that shows script time
  per function; see [Debugging and profiling](GUIDE.md#8-debugging-and-profiling).

## 11. Where to go next

- Build something: the [Starfall tutorial](GUIDE.md#5-tutorial-starfall) uses almost everything
  above in about 250 lines.
- Look things up: [the API reference](API.md) for the engine, [the language
  reference](LANGUAGE.md) for the rules, with the built-in functions in §9.9 to §9.11 and the
  collection methods in §9.16 and §9.17.
- Read why it is this way: [DESIGN.md](DESIGN.md) §5 explains the memory model, types and
  execution, and the reasoning behind them.

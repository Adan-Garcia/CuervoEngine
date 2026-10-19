# Pluma Grammar Specification

> Status: v0.2. Companion to [DESIGN.md](DESIGN.md) §5, which covers
> semantics (memory, types, execution). This document covers **syntax**.
> Pluma is the scripting language of Cuervo Engine; script files use the extension `.plm`.
>
> Notes marked **Decided:** record design decisions and syntax rules.
> Notes marked **Clarified:** resolve ambiguities found while implementing the parser
> (`pluma/parser.cpp`).
> §9 records the semantics clarified while implementing the type checker and VM
> (`pluma/checker.cpp`, `pluma/vm.cpp`).

## 1. Lexical structure

### 1.1 Source text
UTF-8. Identifiers are ASCII letters, digits and `_`, not starting with a digit.
Identifiers starting with `_` are private to their class.

### 1.2 Comments
```
// line comment
/* block comment /* can nest */ still a comment */
```

### 1.3 Statement termination
A newline ends a statement. `;` is optional and lets several statements share a line.
A newline is **not** a terminator when:
- the line ends with a binary operator, `,`, `(`, `[`, `{`, `=>`, `=`, or `.`
- the next line starts with `.`, `?.`, or a closing `)` / `]`
- it is inside `(` `)` or `[` `]`

So method chains can continue on the next line:
```
g.path()
    .move_to(a)
    .cubic_to(c1, c2, b)
    .stroke(width = 2.0)
```

**Clarified:**
- "Line ends with a binary operator" covers every token after which an operand is
  required: binary operators, `as`, `is`, `..`, `..=`, `step`, the ternary `?` and `:`,
  and assignment operators (`+=` etc.).
- Inside `{` `}` of a struct literal, map literal or enum body, newlines are not
  terminators either (elements are separated by `,`). Inside a block they are, even
  when the block belongs to a lambda inside parentheses.
- A `(` or `[` at the start of a line begins a new statement; it does not call or
  index the previous line.
- Where a `{` is required (the body of a `fn`, `if`, `else`, loop, `match`, class,
  struct, enum or trait), newlines before it are allowed. Accessor blocks (§2.5) are
  optional, so their `{` must be on the same line as the `var`. `else` may start the
  line after the closing `}` of the `if` body. The formatter still emits `} else {`.

### 1.4 Keywords
```
var const fn class struct enum trait extends manual unsafe weak
if else match for foreach in while loop break continue return step
await yield start signal emit
new alloc free defer
true false null self super is as
pub final static get set
```

**Clarified:** keywords are accepted as member names after `.` and `?.`
(`map.get(k)`, `task.start()`), as attribute names, and as named-argument names when
they can't start an expression (`@export(range = 0..1.0, step = 0.1)`). `step` ends a
range only when it is not followed by `=`. `_` is the wildcard pattern.

**Clarified (Phase 8):** `step` also names the built-in `step(edge, x)` (§9.11): where
an expression can start, `step` is read as that identifier, so `step(0.5, x)` is a call.
Right after a range's upper bound it is still the range keyword (`0..n step (k)` steps
by `k`). `step` still cannot be declared as a variable or member name.

### 1.5 Literals
| Kind | Examples |
|---|---|
| Integer | `42`, `1_000_000`, `0xff`, `0b1010`, `42u8`, `7i64` |
| Float | `1.0`, `0.5`, `1e-3`, `2.5f64` (default `float` = f32) |
| Angle | `45deg`, `12.5deg`, `-90deg` — a `float` in radians |
| String | `"hello"`, `"Score: {score}"`, `"brace: {{"`, `r"raw\n no escapes"` |
| Multiline string | `"""` … `"""` (leading indentation stripped) |
| Interned name | `#shoot`, `#player_hit` |
| Bool / null | `true`, `false`, `null` |
| List | `[1, 2, 3]` |
| Map | `{"a": 1, "b": 2}` — only where an expression is expected and the type is a map |
| Range | `0..8`, `0..=8`, `0..8 step 2` |

Escapes in strings: `\n \t \r \\ \" \0 \u{1F600}`. Interpolation takes any expression:
`"{player.name} has {hp * 10}%"`. Write `{{` and `}}` for literal braces.

Colors are not literals: `Color.hex("ff4060")`.

**Decided:** angles are radians everywhere. The `deg` suffix works on integer and float
literals (`45deg`, `12.5deg`) and produces a `float` holding the value in radians,
converted at compile time (`45deg` is `0.7853982`). It does not combine with other
suffixes or with hex/binary literals. Use it as `-90deg` for negative angles (unary
minus applies as usual).

**Decided:** integer overflow (arithmetic on any integer type, including literal
suffixes like `u8`) panics in development builds and wraps (two's complement) in
release builds. The type checker and VM implement this; the parser only rejects
literals that don't fit in 64 bits.

**Clarified (strings):**
- `\u{...}` takes 1 to 6 hex digits naming a Unicode scalar value (no surrogates).
  Any other escape is an error.
- A lone `}` in a non-raw string is an error ("write `}}`"), mirroring `{{`. An empty
  interpolation `{}` is an error.
- Raw strings `r"..."` have no escapes and no interpolation.
- Multiline strings: a line break right after the opening `"""` is dropped. If the
  closing `"""` is on its own line, that line is dropped too, and its indentation is
  removed from the start of every line (lines with less indentation lose what they
  have). Otherwise no indentation is removed. `r"""..."""` is a raw multiline string.
  CRLF line breaks become `\n`.
```
var help = """
    Usage:
      move with {key_names}
    """                // == "Usage:\n  move with {key_names}"
```

### 1.6 The `?` whitespace rule
`?` has three meanings, separated by spacing:

| Form | Meaning | Spacing |
|---|---|---|
| `expr?` | Propagate `Result` error | No space before `?` |
| `expr?.member` | Safe member access (null → null) | No spaces |
| `a ?? b` | Null default | Spaces around |
| `a ? b : c` | Ternary | Spaces around `?` |

`load()? + 1` is propagation followed by addition; `ok ? 1 : 2` is a ternary.
`a?b:c` is a syntax error with a fix-it suggestion. The formatter normalizes spacing.

**Clarified:** the parser decides by the space *before* `?`. With whitespace before it,
`?` is a ternary. Without, it is postfix propagation, unless it is directly followed
by the start of an operand (a name, literal, `!`, `~`, or a keyword such as `self`),
as in `a?b:c` or `ready? 1 : 2`; that is reported as a mis-spaced ternary with a fix-it
(`a ? b : c`). The one exception is the if-binding `if x? t {` (§3). `a??b` and `a?.b`
are always the `??` and `?.` operators, whatever the spacing.

## 2. Declarations

### 2.1 Variables and constants
```
var hp = 100                 // mutable, type inferred (int)
var dir: vec2                // explicit type, default-initialized (zero / null)
const MAX_HP = 400           // compile-time constant; must be a constant expression
```
There is no `let`. Function parameters are immutable bindings.

### 2.2 Functions
```
fn add(a: int, b: int) -> int {
    return a + b
}

fn ring(count: int, speed = 150.0, spin = 0.0) { ... }   // types inferred from defaults

fn log_all(items: List<str>) { ... }                      // no return type = void
```
- Parameter types are required unless a default value fixes them.
- Defaulted parameters may appear only after all required parameters.
- Return type is required when the function returns a value.

### 2.3 Calls and arguments
```
ring(36)
ring(36, spin = 12.0)
ring(count = 36, speed = 200.0)
```
Positional arguments come first, then named ones. Each parameter may be supplied once.
Assignment is a statement, never an expression, so `name = value` inside a call is
always a named argument.

**Clarified:** the parser reports a positional argument after a named one, and the
same name given twice. Attribute arguments (§2.10) and `emit` arguments use the same
rules.

### 2.4 Lambdas
```
var f = fn(x: int) -> int { return x * 2 }
var g = fn() { print("hi") }
enemies.filter(e => e.hp > 0)          // shorthand: single expression body
pairs.sort((a, b) => a.score > b.score)
```
Lambda parameter types are inferred from the expected function type when possible.
Lambdas capture variables by reference (refcounted for automatic memory).

**Clarified:**
- An arrow lambda (`x => e`, `(a, b) => e`, `() => e`) is accepted wherever a full
  expression is expected: arguments, initializers, the branches of `? :`, the body of
  another arrow lambda. As an operand of a binary operator it needs parentheses
  (`a ?? (x => x)`), and its body extends as far right as possible. Not allowed bare in
  a `match` guard, where `=>` ends the guard.
- `fn(...)` lambdas take the same parameter syntax as functions, but parameter types
  may be omitted (inferred). They are self-delimiting: `fn() { ... }()` is a call.
- In a block lambda without a `-> T` return type whose expected function type returns
  a value, a final expression statement is the result: `until(fn() { hp < 150 })`
  (DESIGN.md §5.2). This is checked by the type checker, not the parser.

### 2.5 Classes
```
@export_group("Combat")
class Boss extends Enemy: Damageable, Saveable {
    signal defeated
    signal hp_changed(value: int)

    @export(range = 0..1000)
    var max_hp = 400

    var hp = 400 {
        set(v) {
            hp = clamp(v, 0, max_hp)      // inside its own setter, `hp` is the raw field
            emit hp_changed(hp)
        }
    }

    var ratio: float {                     // computed property: no storage
        get { return hp as float / max_hp }
    }

    static var count = 0
    var _phase = Phase.Opening             // private

    fn ready() { ... }
    final fn hurt(amount: int) { hp -= amount }
    static fn spawn_at(p: vec2) -> Boss { ... }
}
```
- At most one `extends`. Traits are listed after `:`.
- Methods are virtual unless `final`. `super.method()` calls the parent implementation.
- Construction: `Boss()` runs field initializers, then `init(...)` if one is declared:
  ```
  fn init(pos: vec2, hp = 400) { self.position = pos; self.hp = hp }
  ```
- `self` is implicit for member access; write it to disambiguate.

**Clarified:**
- Accessors are allowed only on fields (`var` members of a class or struct), not on
  locals or top-level variables. `get` and `set(v)` may appear in either order, each at
  most once. A computed property has a type and a `get` but no initializer.
- Member modifiers: `static` on class and struct members; `final` on class members;
  not both. `pub` only on top-level declarations. `signal` only in classes. Type
  declarations (`class`, `struct`, `enum`, `trait`) only at the top level.
- Functions need a body everywhere except in traits.

### 2.6 Structs
```
struct Bullet {
    var pos: vec2
    var vel: vec2
    var ttl = 3.0
}

var b = Bullet { pos = p, vel = v }        // struct literal; omitted fields use defaults
var c = b                                  // copy
```
Structs are values: no inheritance, but they can implement traits. Struct literals
are not allowed directly in `if`/`while`/`for`/`match` headers (wrap them in parentheses)
to avoid confusion with the following block.

**Clarified:** `Type {` starts a struct literal only when the `{` is followed by `}` or
by `field =`. So `var hp = max_hp { get { ... } }` is a field with an accessor, not a
struct literal. The header rule also covers `foreach` and map literals, and it applies
only outside brackets: `if hits(Point { x = 1 }) { }` is fine. Each field may be
initialized once. Generic structs: `Pair<int> { a = 1, b = 2 }`.

### 2.7 Enums
```
enum Phase {
    Opening,
    Spiral(arms: int),
    Rage,
}

enum Dir: u8 { Up = 0, Down = 1, Left = 2, Right = 3 }   // plain enum with explicit backing
```
**Clarified:** variants are separated by commas even when written one per line (a
missing comma gets a fix-it); a trailing comma is allowed. Variant fields need types.

### 2.8 Traits and operator overloading
```
trait Damageable {
    fn hurt(amount: int)
    fn is_dead() -> bool { return false }   // default implementation
}

struct Complex: Add, Mul, Eq {
    var re: float
    var im: float
    fn add(o: Complex) -> Complex { return Complex { re = re + o.re, im = im + o.im } }
    fn mul(o: Complex) -> Complex { ... }
    fn eq(o: Complex) -> bool { ... }
}
```
| Operator | Trait | Method |
|---|---|---|
| `+` `-` `*` `/` `%` | `Add` `Sub` `Mul` `Div` `Rem` | `add` `sub` `mul` `div` `rem` |
| unary `-` | `Neg` | `neg` |
| `==` `!=` | `Eq` | `eq` |
| `<` `<=` `>` `>=` | `Ord` | `cmp -> int` |
| `a[i]` / `a[i] = v` | `Index` / `IndexSet` | `index` / `index_set` |
| `for x in a` | `Iterable` | `iter` |
| `"{a}"` | `Display` | `to_str` |

`+=` and similar compound assignments use the same traits. `&&`, `||`, `??`, `?:` and
`is` / `as` cannot be overloaded.

### 2.9 Generics
```
class Stack<T> {
    var _items = List<T>()
    fn push(x: T) { _items.push(x) }
}

fn max<T: Ord>(a: T, b: T) -> T { return a > b ? a : b }
fn sum<T: Add + Default>(xs: List<T>) -> T { ... }
```
**Clarified:** in an expression, `name<...>` is read as type arguments only when the
closing `>` is directly followed by `(` (a call: `List<vec2>()`, `max<int>(a, b)`,
`x.cast<Boss>()`) or by a struct literal; otherwise `<` is less-than. So
`f(a < b, c > d)` is two comparisons. Nested generics close with `>>` as expected
(`Map<str, List<int>>`); `>>` and `>>=` as operators need their characters adjacent.

### 2.10 Attributes
`@name` or `@name(args)` before a declaration. Attributes in v1:

| Attribute | On | Meaning |
|---|---|---|
| `@export`, `@export(range, step, hint)` | field | Show in the inspector and serialize |
| `@tooltip("…")` | field | Inspector tooltip |
| `@export_group("…")` | class / field | Inspector section |
| `@acyclic` | class | Promise no cycles; skipped by the cycle collector (checked in debug) |
| `@inline`, `@cold` | fn | Optimization hints |
| `@deprecated("…")` | any | Warning on use |
| `@allow_slow` | `foreach` | Silence the slow-path warning (§5) |

Attributes may be written on the same line as the declaration or on the lines before
it. **Clarified:** statements take attributes only before `foreach`.

### 2.11 Manual memory declarations
```
manual class ParticleSystem { ... }
var tmp  = new(frame) List<vec2>()
var boss = new(scene) Boss()
var buf  = alloc Buffer(4096)
defer free(buf)
weak var owner: Boss
unsafe { ... }
```

## 3. Statements

```
// assignment (statement only)
x = 1
x += 1;  x -= 1;  x *= 2;  x /= 2;  x %= 3
pos.x = 0.0
list[i] = v

// if / else (statements)
if hp <= 0 {
    die()
} else if hp < 20 {
    flash()
} else {
    heal(1)
}

// if-binding: unwraps a nullable or matches a pattern
if target? t { t.hurt(1) }
if phase is Phase.Spiral(arms) { print(arms) }

// match (statement; must be exhaustive)
match phase {
    Phase.Opening => start_opening()
    Phase.Spiral(arms) if arms > 3 => {
        hard_spiral(arms)
    }
    Phase.Spiral(arms) => spiral(arms)
    Phase.Rage => rage()
}

match key {
    0 | 1 => low()
    2..=9 => mid()
    _ => high()
}

// loops
for i in 0..8 { }
for i in 0..=8 step 2 { }
for e in enemies { }
for i, e in enemies { }
while hp > 0 { }
loop { if done { break } }
break; continue
outer: for y in 0..h { for x in 0..w { if hit { break outer } } }

// batch loop over pools (see §5)
foreach p in sparks.pool { p.age += dt }

// functions / coroutines
return value
await wait(0.5)
yield                          // wait one tick
start(run_phases())
emit defeated
defer close(file)              // runs at scope exit, in reverse order
```

**Clarified:**
- `match` arms are separated by newlines or `;` (an optional `,` after an arm is
  accepted). An arm body is a block or a single statement (`x = v`, `return`, a call).
- Patterns: `_`; literals (integers and floats, optionally negative, strings without
  interpolation, `#names`, `true`/`false`/`null`); literal ranges `2..=9`, `0..10`;
  paths with optional payload patterns `Phase.Spiral(arms)`; a bare name, which binds
  (or names a constant; resolved by the checker); and `|` alternatives.
- `x is P` takes a single pattern (no `|`, which is the bit-or operator there).
  After a bare type path it may bind the narrowed value: `node is Boss b`.
- `if x? t { }` binds `t` only when `? IDENT {` follows the condition directly;
  otherwise `?` is propagation (`if load()? { }`).
- `=` in a condition is reported with a fix-it to `==`.
- Labels are allowed only on `for`, `while` and `loop`; `break`/`continue` take an
  optional label on the same line.
- A `{` at the start of a statement opens a nested block (a scope).
- `defer` takes one statement or a block.
- Assignment targets are names, members (`a.b`, not `a?.b`) and index expressions.

## 4. Expressions

### 4.1 Precedence (high → low)
Bitwise operators bind **tighter** than comparisons, fixing C's classic `a & b == 0` trap.

| Level | Operators | Assoc. |
|---|---|---|
| 1 | `()` call, `[]` index, `.` member, `?.` safe member, postfix `?` | left |
| 2 | unary `-` `!` `~` | right |
| 3 | `as` | left |
| 4 | `*` `/` `%` | left |
| 5 | `+` `-` | left |
| 6 | `<<` `>>` | left |
| 7 | `&` | left |
| 8 | `^` | left |
| 9 | `\|` | left |
| 10 | `..` `..=` (with optional `step`) | none |
| 11 | `<` `<=` `>` `>=` `is` | none |
| 12 | `==` `!=` | none |
| 13 | `&&` | left |
| 14 | `\|\|` | left |
| 15 | `??` | right |
| 16 | `? :` ternary | right |
| 17 | `=>` lambda | right |

Comparisons don't chain: `a < b < c` is an error.

**Clarified:**
- `==`/`!=` and ranges don't chain either. `a < b < c` gets a fix-it to
  `a < b && b < c`. Parentheses make any of them legal: `(a < b) == c`.
- `await` is a prefix operator at the unary level: `await wait(0.5)`, and
  `await a + 1` is `(await a) + 1`.
- `step` binds like the range operator: `0..n step k` (operands at the `|` level).
- `?:` and `??` are right-associative: `a ?? b ?? c` is `a ?? (b ?? c)`.

### 4.2 Vectors
```
var p = vec2(1.0, 2.0)
var s = vec6(0.0)              // splat
var q = vec3(p, 5.0)           // compose
p.x; p.yx; q.xy                // swizzles, N <= 4
s[4]                           // index, any N
p * 2.0;  p + q.xy;  p.dot(q.xy);  p.length();  p.normalized()
```
**Decided:** world coordinates are Y-down with the origin at the top left
(DESIGN.md §7.1): `vec2(0.0, -1.0)` points up the screen, and a positive rotation
turns clockwise. Examples in this document follow that convention.

### 4.3 Casts and type tests
```
var f = hp as float
if node is Boss { }            // type test
if node is Boss b { b.rage() } // test + bind
```

## 5. `foreach` batch loops
**Decided:** there is no dedicated bullet system. The engine has a general particle
system (DESIGN.md §4.6) whose emitters own SoA particle pools; bullets and other
projectiles are built on it. `foreach` is how scripts run custom per-particle behavior.
```
fn update(dt: float) {
    foreach p in trail.pool {
        p.vel = p.vel.rotate(sin(time * 4.0) * 0.02)
        p.pos += p.vel * dt
        if p.age >= p.life { trail.pool.kill(p) }
    }
}
```
A script-side pool of structs works the same way:
```
var shards = Pool<Shard>(capacity = 512)
foreach s in shards { s.ttl -= dt; if s.ttl <= 0.0 { shards.kill(s) } }
```
- Works over `Pool<T>` and native SoA pools (an emitter's particle pool). `p` is a
  mutable reference to the element.
- The compiler tries to lower the body to a tight loop (VM: specialized bytecode;
  AOT: a plain C loop over the SoA arrays).
- The fast path requires: no allocation, no `await`, no calls to functions it can't inline
  or prove pure, no reads of other elements of the same pool, and changes to the pool only
  through `kill`.
- If the body breaks a rule, the loop still works, as an ordinary loop, and the
  compiler **emits a warning** naming the line and the reason:
  `boss.plm:42: foreach runs on the slow path: call to 'spawn' may allocate`.
- `@allow_slow` on the loop silences the warning.

## 6. Style guide (enforced by the formatter)
- Indent every block's contents by **4 spaces**, even though braces define structure.
- Opening brace on the same line. `} else {` on one line.
- One statement per line; no `;` except when sharing a line on purpose.
- Spaces around binary operators and the ternary `?` / `:`; none before postfix `?`.
- `PascalCase` for types, enum variants and traits; `snake_case` for functions, variables
  and fields; `SCREAMING_SNAKE` for `const`; `#snake_case` for names.
- Trailing commas in multiline lists, arguments and enums.

## 7. Grammar (EBNF sketch)
```ebnf
file        = { decl } ;
decl        = { attribute } [ "pub" ] ( class | struct | enum | trait | fn_decl | var_decl | const_decl ) ;
attribute   = "@" IDENT [ "(" args ")" ] ;

class       = [ "manual" ] "class" IDENT [ generics ] [ "extends" type ] [ ":" type_list ] "{" { member } "}" ;
struct      = "struct" IDENT [ generics ] [ ":" type_list ] "{" { member } "}" ;
enum        = "enum" IDENT [ ":" type ] "{" variant { "," variant } [ "," ] "}" ;
variant     = IDENT [ "(" param_list ")" ] [ "=" expr ] ;
trait       = "trait" IDENT [ generics ] [ ":" type_list ] "{" { fn_decl | fn_sig } "}" ;
member      = { attribute } [ "static" | "final" ] ( var_decl | const_decl | fn_decl | signal ) ;
signal      = "signal" IDENT [ "(" param_list ")" ] ;

var_decl    = [ "weak" ] "var" IDENT [ ":" type ] [ "=" expr ] [ accessors ] ;
accessors   = "{" [ "get" block ] [ "set" "(" IDENT ")" block ] "}" ;
const_decl  = "const" IDENT [ ":" type ] "=" expr ;
fn_decl     = fn_sig block ;
fn_sig      = "fn" IDENT [ generics ] "(" [ param_list ] ")" [ "->" type ] ;
param_list  = param { "," param } [ "," ] ;
param       = IDENT [ ":" type ] [ "=" expr ] ;
generics    = "<" gparam { "," gparam } ">" ;
gparam      = IDENT [ ":" type { "+" type } ] ;

type        = IDENT [ "<" type_list ">" ] | "[" type ";" expr "]" | fn_type | type "?" ;
fn_type     = "fn" "(" [ type_list ] ")" [ "->" type ] ;
type_list   = type { "," type } ;

block       = "{" { stmt } "}" ;
stmt        = var_decl | const_decl | assign | if_stmt | match_stmt | for_stmt
            | foreach_stmt | while_stmt | loop_stmt | jump | defer | unsafe_blk
            | block | "yield" | "emit" IDENT [ "(" args ")" ] | expr ;   (* await: see unary *)
assign      = lvalue assign_op expr ;
assign_op   = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
if_stmt     = "if" cond block { "else" "if" cond block } [ "else" block ] ;
cond        = expr [ "?" IDENT ] | expr "is" pattern ;
match_stmt  = "match" expr "{" { pattern [ "if" expr ] "=>" ( block | stmt ) } "}" ;
pattern     = "_" | lit_pat | lit_pat ( ".." | "..=" ) lit_pat
            | path [ "(" pattern { "," pattern } ")" ] | pattern "|" pattern ;
lit_pat     = [ "-" ] ( INT | FLOAT ) | STRING | NAME | "true" | "false" | "null" ;
path        = IDENT { "." IDENT } ;      (* a bare IDENT binds *)
for_stmt    = [ IDENT ":" ] "for" IDENT [ "," IDENT ] "in" expr block ;
foreach_stmt= { attribute } "foreach" IDENT "in" expr block ;
while_stmt  = [ IDENT ":" ] "while" expr block ;
loop_stmt   = [ IDENT ":" ] "loop" block ;
jump        = "return" [ expr ] | "break" [ IDENT ] | "continue" [ IDENT ] ;
defer       = "defer" ( stmt | block ) ;
unsafe_blk  = "unsafe" block ;

expr        = lambda | ternary ;
lambda      = "fn" "(" [ param_list ] ")" [ "->" type ] block
            | ( IDENT | "(" [ IDENT { "," IDENT } ] ")" ) "=>" expr ;
ternary     = null_coal [ "?" expr ":" expr ] ;
(* binary levels follow the table in §4.1; "is" takes a single pattern, then an
   optional binding IDENT; ".." / "..=" take an optional "step" operand *)
unary       = ( "-" | "!" | "~" | "await" ) unary | postfix ;
postfix     = primary { call | index | member | safe_member | "?" } ;
member      = "." ( IDENT | KEYWORD ) [ "<" type_list ">" (* only before "(" *) ] ;
call        = "(" [ args ] ")" ;
args        = arg { "," arg } [ "," ] ;
arg         = [ IDENT "=" ] expr ;
primary     = literal | IDENT [ "<" type_list ">" (* only before "(" or "{" *) ]
            | "self" | "super" | "(" expr ")" | list_lit | map_lit
            | struct_lit | "new" "(" IDENT ")" type call | "alloc" type call
            | "start" "(" expr ")" | "free" "(" expr ")" ;
struct_lit  = type "{" [ IDENT "=" expr { "," IDENT "=" expr } [ "," ] ] "}" ;
literal     = INT | FLOAT | STRING | NAME | "true" | "false" | "null" ;
               (* INT/FLOAT may carry a suffix: i8..u64, f32, f64, deg *)
```

## 8. Example: a complete player
```
class Player extends Node2D: Damageable {
    signal died

    @export(range = 50.0..400.0)
    var speed = 200.0
    @export
    var focus_speed = 80.0

    var lives = 3 {
        set(v) {
            lives = v
            if lives <= 0 { emit died }
        }
    }

    var _invuln = 0.0

    fn update(dt: float) {
        var s = Input.held(#focus) ? focus_speed : speed
        position += Input.axis(#move) * s * dt
        position = position.clamp(Screen.min, Screen.max)

        if Input.held(#shoot) {
            shots.aimed(at = position + vec2(0.0, -1.0), speed = 600.0)
        }
        _invuln = max(_invuln - dt, 0.0)
    }

    fn hurt(amount: int) {
        if _invuln > 0.0 { return }
        lives -= amount
        _invuln = 2.0
        start(blink())
    }

    fn blink() {
        for i in 0..10 {
            visible = !visible
            await wait(0.1)
        }
        visible = true
    }

    fn draw(g: Canvas) {
        if Input.held(#focus) {
            g.circle(position, 3.0, fill = Color.hex("ffffff"))
        }
    }
}
```

## 9. Semantics clarified by the checker and VM (Phase 1B)

These follow DESIGN.md §5 and the decisions above; each can be revisited.

### 9.1 Literals and inference
- An integer literal without a suffix takes the numeric type its context expects (a
  declared type, a parameter, the other operand of a binary operator), including a float
  type (DESIGN.md 5.4); otherwise it is `int`. It must fit (`var b: u8 = 300` is an
  error). A float literal without a suffix is `f64` where `f64` is expected, else `float`.
- In `a op b` an untyped literal adapts to the other operand; with two literals an
  integer literal adapts to a float one (`1 + 2.0` is a `float`).
- `var x = null` needs a type annotation. `var x: T` without an initializer holds the
  zero value: `0`, `0.0`, `false`, `""`, `#` (the empty name), `null`, a struct built
  from its field defaults, or the first variant of an enum (an error if that variant has
  a payload). A local of a generic parameter type needs an initializer.
- A lambda without `-> T` takes its return type from the expected function type, else
  from its first `return` (or the arrow body), else it returns nothing.

### 9.2 Types and conversions
- Reference types (classes, `List<T>`, function types, trait types) always admit `null`
  (DESIGN.md 5.7); `T?` on them is allowed and changes nothing. On a value type (numbers,
  `bool`, `str`, `name`, structs, enums) `T?` is an optional value: it accepts a `T` or
  `null`, and is used through `??`, `if x? v`, `== null`, or a `match` with a `null` arm.
  Arithmetic and member access on a `T?` are errors. `a?.field` of a value type gives `T?`.
- `str` is an immutable value-like type: never null, compared by content; `+` joins two
  strings.
- Conversions use `as` only: integer to integer wraps (two's complement); float to integer
  truncates toward zero and saturates, NaN gives 0; to float rounds to nearest; `bool` to
  an integer gives 0/1; a plain enum to an integer gives its discriminant; `x as Sub`
  downcasts a class reference and panics if `x` is not a `Sub` (`null` stays `null`).
- Float division by zero follows IEEE 754 (inf/NaN); `%` on floats is `fmod`.
- A shift count outside `0 ..< bit width` panics in development and is masked in release.
  `>>` is arithmetic on signed types. `&`, `|`, `^` also apply to `bool` (no
  short-circuit).
- `==` is structural for structs and enums and by identity for classes, unless the type
  implements `Eq`; lists compare by identity; functions cannot be compared. `<`, `<=`,
  `>`, `>=` apply to numbers, `str` (bytewise) and `Ord` types.

### 9.3 Names
- Lookup order: locals (innermost first, through enclosing lambdas), then members of the
  enclosing class or struct (fields and methods through implicit `self`, then statics,
  inherited ones included), then globals, then built-in functions.
- Built-in type names (`int`, `str`, `name`, ...) cannot name a type declaration but may
  name variables and functions (`var name = #shoot`).
- `_private` members are visible only inside the declaring type (not subclasses).
- Duplicate global names are errors across REPL inputs too. An input with any error is
  rolled back as a whole, so its names stay free.

### 9.4 Functions and closures
- Struct arguments are copied. Parameters are immutable: neither they nor (for structs)
  their fields can be assigned.
- A default parameter value is evaluated at each call, in the declaration's scope (it
  cannot see the other parameters).
- Closures capture `var`s by reference (one shared cell) and immutable bindings
  (parameters, loop variables, pattern bindings, `self`) by value. Each loop iteration
  binds a fresh loop variable.
- Methods can be used as values since Milestone 9.2a (`f = obj.hurt`, 9.19).

### 9.5 Structs and classes
- Struct values are copied whenever they are read out of a variable, field or list element
  to be stored, passed or returned; the copy is deep for nested structs. A method called
  on a struct variable works on it in place and may change `self`'s fields. `p.x = 1`
  needs a mutable place (a `var`, a field of one, a list element, a global).
- Struct literals: omitted fields take their initializer or zero value; a field of a
  generic parameter type without an initializer must be given. Generic arguments are
  inferred from the expected type or the field values (`Pair { a = 1, b = 2 }`).
  Structs have no `init` and no call syntax.
- `C(args)` runs field initializers (base class first; they cannot use `self`), then the
  nearest `init` in the class chain with the arguments. A subclass `init` does not call
  the base one implicitly: use `super.init(...)`. A class with no `init` in its chain
  takes no arguments.
- An override must have exactly the overridden signature. Static methods and `init` are
  not virtual. `super.name()` calls the parent implementation directly.
- Static variables are initialized with the file's (or REPL input's) top-level code, in
  declaration order. Generic types cannot have static variables yet.

### 9.6 Traits
- A trait can be used as a type: any class or struct that lists it (structs are copied in).
  Calls on it dispatch by method name.
- A default method is compiled once for all implementors; inside it `self` has the trait
  type and calls other trait methods implicitly.
- To overload an operator a type lists the trait (`struct Complex: Add`) and defines the
  method with the right shape (`eq -> bool`, `cmp -> int`, `to_str -> str`, one operand
  for binary operators, none for `neg`).
- Bounds: every type satisfies `Eq` and `Display`; numbers satisfy the arithmetic traits,
  `Ord` and `Default` (`Neg` only for signed and float types); `str` satisfies `Add`
  and `Ord`. Generic code calls operators through these bounds (dynamic dispatch in the
  VM, DESIGN.md 5.5).
- `"{x}"` and `print(x)` use `to_str` when the type has one; otherwise a debug form:
  `Point { x = 1, y = 2 }`, `Phase.Spiral(arms = 4)`, `[1, 2]` (list elements and
  nested strings are shown quoted).

### 9.7 Enums and match
- A plain enum (no payload in any variant) is an integer at run time. Discriminants count
  up from 0, or from the previous explicit value. Only plain enums may have explicit
  discriminants and a backing type.
- Variants are written `Phase.Rage`, `Phase.Spiral(4)` or `Phase.Spiral(arms = 4)`; a
  payload variant needs its payload. Payloads are immutable.
- The pattern `Phase.Spiral` (no parentheses) matches any payload. A bare name in a
  pattern names a `const` or a class if one exists, and otherwise binds. Bindings are not
  allowed inside `|` alternatives. An `is` pattern may bind only directly in an `if`
  condition; its bindings are also visible to the right of `&&`.
- Exhaustiveness: for an enum, each variant needs an arm without a guard whose payload
  patterns are all `_` or bindings; for `bool`, `true` and `false`; a `T?` also needs
  `null`; other types need a `_` or binding arm. Nested refutable payload patterns are
  not counted (conservative).

### 9.8 Statements
- `for` ranges are over integers only. The bounds and `step` are evaluated once; `step`
  must not be zero, and a negative step counts down (`for i in 3..=1 step -1`). A range
  ending at the type's maximum does not overflow.
- `for x in list` evaluates the list once and checks its length on every iteration. Loop
  variables are immutable.
- `defer` runs when its block exits, including through `break`, `continue` and `return`,
  in reverse order; it does not run on a panic. A deferred statement cannot `return`,
  `break` or `continue`.
- `unsafe { }` skips generation checks on manual references inside its block (§9.14).
- In a script (REPL input), top-level `var`s are globals and `return` is an error. A file
  run as a module initializes its globals in declaration order and then calls `fn main()`
  if it declares one.

### 9.9 Built-ins and panics
- Free functions: `print(x)`, `assert(cond, message = "")`, `panic(message)`,
  `min`, `max`, `abs`, `clamp`, `sqrt` (numbers; `sqrt` is correctly rounded, so
  deterministic). `List<T>`: `len`, `push`, `pop`, `clear`, `insert`, `remove`,
  `is_empty`, `contains`, and `list[i]`. `str`: `len` (bytes), `contains`,
  `starts_with`, `ends_with`. The collections (`Map`, `Set`, `[T; N]`, `StrBuilder`,
  `Deque`, `PriorityQueue`) are in 9.16, the further `List`/`str`/number methods and
  conversions (`parse_int`, `parse_float`) in 9.17, `Result` and `?` in 9.18.
- Panics report `file:line:col` and a stack trace: integer overflow (development),
  division by zero, a bad shift count, null use (naming the expression, `'boss.target'
  is null`), an index out of bounds, `pop()` on an empty list, a failed `assert`,
  `panic()`, a failed downcast, and recursion deeper than 4096 calls.

### 9.10 Coroutines and signals (Phase 3)
DESIGN.md 5.9 has the model; these are the language rules.
- A function or method whose body uses `await` or `yield` (outside nested lambdas) is a
  **coroutine**. It can only run as `start(f(args))` (any function; returns a `Task`) or
  `await f(args)` (inside another coroutine; the value is `f`'s return value). Any other
  call of it is an error. `start` needs a direct call (`start(x?.f())` is rejected).
- `await` operands: `wait(seconds: float)`, `wait_ticks(n: int)`, `until(cond: fn() ->
  bool)` (these three exist only after `await`), a coroutine call, a `Task`, or a signal
  (`await boss.defeated`; its value is the signal's argument when it has one parameter, else
  nothing; more parameters are not supported yet).
- `await`/`yield` inside a lambda, a `defer`, top-level code, an initializer or a default
  value is an error. Trait methods cannot be coroutines yet. An override must match the
  overridden method's coroutine-ness.
- `Task` is a reference type with `cancel()` and `is_done()`. Cancelling stops the coroutine
  at once if suspended, else at its next `await`; its `defer`s do not run (like a panic).
- `wait_ticks(0)` does not suspend; `wait(s)` waits at least one tick.
- A panic inside a coroutine resumed by the scheduler stops that coroutine only.
- `signal name(p: T, ...)` (classes only) declares a field of type `Signal<T, ...>`, created
  with the object; it cannot be assigned. `emit name(args)` (own signals, in methods;
  arguments by position or name) calls every handler connected with
  `obj.name.connect(f: fn(T, ...))`; `disconnect(f)` removes the first handler that is the
  same function object.

### 9.11 Vectors (Phase 3)
- `vec1` ... `vec64` are value types of `float` components (integer vectors `ivec2`..`ivec4` are prelude structs, 9.20). Construction: `vec3(x, y, z)`, `vec3(v2, z)` (floats and vectors whose sizes
  add up), `vec4(0.0)` (splat); int literals adapt to `float`. A `vecN` variable without an
  initializer is zero.
- Arithmetic: `+ - * /` componentwise between vectors of the same size; `v * s`, `s * v`,
  `v / s` with a `float`; unary `-`; `==`/`!=` compare every component. Compound
  assignment works as usual (`pos += vel * dt`).
- Components: `v.x .y .z .w` (components 0..3) and swizzles of up to 4 (`v.yx`, `v.xyz`);
  one component is a `float`, several a vector. `v[i]` reads any component (bounds-checked); `v[i] = e` sets one (9.20).
  `v.x = e`, `v.xy += e` replace components of a mutable place (a `var`, a field of one, a
  list element, a global).
- Methods: `length`, `length_squared`, `dot`, `normalized` (zero stays zero), `distance`,
  `distance_squared`, `lerp(to, t)`, `abs`, `min`, `max`, `clamp(lo, hi)`, `reflect(normal)`,
  `project(onto)`, `slide(normal)`, `clamp_length(min, max)`, and the 9.2b methods in 9.20 (`floor`, `ceil`, `round`, `fract`, `sign`, `snapped`, `bounce`, `move_toward`, `direction_to`, `limit_length`); `vec2` also has `rotated(angle)`
  (positive = clockwise on screen, y down), `angle()`, `perpendicular()`, `angle_to(other)`,
  `slerp(target, t)` (spherical linear arc interpolation), and `cross(other)` (perp-dot returning `float`);
  `vec3` has `cross(other) -> vec3`.
- Math built-ins: `floor`, `ceil`, `round`, `sign` (numeric), `sin`, `cos`, `tan`, `asin`, `acos`,
  `atan`, `atan2(y, x)`, `exp`, `log`, `log2`, `log10`, `pow(base, exp)`, `lerp(a, b, t)`,
  `inverse_lerp(a, b, v)`, `remap(in_min, in_max, out_min, out_max, v)`, `smoothstep(e0, e1, x)`,
  `step(edge, x)`, `move_toward(from, to, max_delta)`, `rad_to_deg(rad)`, `deg_to_rad(deg)`,
  `wrap(val, min, max)`, `wrap_angle(angle)`, `from_angle(angle, length = 1.0) -> vec2`. The
  transcendental ones go through the host's math hooks, so the engine makes them deterministic
  (DESIGN.md 4.1).
- Vectors cannot be constants yet (`const` needs a constant expression).

### 9.12 Native bindings and projects (Phase 3)
- `@native` on a function or method declares one implemented by the host
  (`pluma/bindings.hpp`); its body must be empty and a host function must be bound under its
  qualified name (`Canvas.circle`), or it is an error. Everything else (types, defaults,
  named arguments, statics, inheritance) is ordinary checking. Native classes are ordinary
  classes whose instances may carry host data, set by a native `init`.
- The engine's API is such a prelude (`cuervo/script/prelude/*.plm`), compiled before the
  game's scripts. `start` and `emit` are keywords, so the emitter API uses `fire`.
- A project's files are checked as one unit (names are global, so files may refer to each
  other in any order); diagnostics and panics name the original file and line.

### 9.13 `@export` (Phase 4)
DESIGN.md 4.12 has the scene-file side; these are the language rules.
- `@export` marks an **instance field** of a class or struct as saved in scene files (and,
  later, shown in the inspector). On a static variable, constant, signal, function,
  top-level declaration or type it is an error.
- Exportable types: `bool`, every number type, `str`, `name`, `vecN`, plain enums,
  non-generic structs whose fields are all exportable, and `List<T>` of exportable `T`.
  Class references, functions, tasks, signals, payload enums and `T?` are rejected with
  the reason (save child nodes instead of references).
- Arguments are named and literal: `range = lo..hi` (numbers, `hi >= lo`),
  `step = s` (a positive number), `hint = "text"`. `range`/`step` need a number field.
  They are validated and kept for the editor; loading does not clamp.
- `@export` fields are inherited like any field; the runtime type records them
  (`RecordRT::field_exported`). `Node.name` is exported and becomes the node's name in
  the file. Other attributes (`@tooltip`, `@export_group`, `@deprecated`, ...) are still
  accepted without effect.
- `step` is a keyword, so it can't name a field (`Slider` uses `increment`).

### 9.14 Memory (Phase 7A)
DESIGN.md 5.10 has the runtime model; these are the language rules.
- `weak var` is allowed only on **instance fields of a class** whose type is a class or
  `List<T>` (not locals, globals, statics, or struct fields). A weak field does not keep its
  target alive and reads as `null` once the target is freed (by refcount, cycle collection,
  `free`, or arena reset).
- `@acyclic` applies only to classes (no arguments). In release builds the compiler trusts
  it and never traces instances of the class; in development builds unproven `@acyclic`
  classes are still traced and panic if an instance is found in a garbage cycle.
- `manual class C` makes every `C(...)` construction manual (equivalent to `alloc C(...)`).
  A subclass of a manual class must also be declared `manual class`.
- `alloc T(args)`, `new(frame) T(args)` and `new(scene) T(args)` allocate a manual instance
  of a class `T` or a `List<E>`; any other arena name or type is an error.
- `free(x)` takes a class or `List<T>` expression (`free(null)` is a no-op). Passing an
  obvious automatic allocation (`free(C())`, `free(List<int>())`) is a compile error;
  passing an automatic object at run time or freeing an already-freed manual object panics.
- Every field access, method call or list operation on a manual reference checks its
  generation and panics on use-after-free, naming the expression and where it was freed
  (`file:line:col` of `free`, or the `frame`/`scene` arena reset). Inside `unsafe { ... }`
  the generation check is skipped.
- **Escape analysis:** the compiler tracks expressions and locals known to come from
  `new(frame)` or `new(scene)` (through casts, ternaries, `??`, struct/list/enum literals,
  and lambda captures) and rejects storing a `new(frame)` value into a longer-lived place
  (a global, a captured variable, a coroutine `start` argument/receiver, a signal handler,
  or a field/list element of a non-`frame` object) or a `new(scene)` value into a global.

### 9.15 Formatter & Language Server (Phase 8)
- **Formatter (`pluma/format.hpp`, `plm fmt`, `cuervo-tools fmt`):** AST-based pretty-printer
  that enforces §6 (4-space indentation, K&R braces on the same line, no trailing semicolons,
  preserved intentional same-line `;` statements, trailing commas on multiline lists/maps/calls/enums/struct
  literals, preserved single-line enums, preserved `//` and `/* ... */` comments, and up to
  one blank line between items). Recoverable fix-it syntax errors (`a?b:c` without spaces
  around `?` and missing commas between enum variants) are normalized automatically; other
  parse errors leave the file untouched and return a non-zero exit code.
- **Style checker (`pluma::check_style`, `plm fmt --lint`):** warns with `FixIt` replacements
  when classes/structs/enums/traits/variants/type parameters are not `PascalCase`, functions/methods/variables/fields/parameters/signals/labels
  are not `snake_case` (with optional leading `_`), `const` declarations are not
  `SCREAMING_SNAKE_CASE`, or `#name` literals are not `#snake_case`.
- **Language server (`pluma/lsp.hpp`, `plm lsp`, `cuervo-tools lsp`):** JSON-RPC 2.0 over
  stdio (`Content-Length` framing) and direct C++ API (`pluma::lsp::Server`). Supports live
  diagnostics (lexer, parser, type checker, and style warnings) across multi-file projects
  plus engine preludes, hover (types, signatures, and leading `//` doc comments), go-to-definition,
  find references, document highlights, prepareRename/rename (refusing prelude symbols),
  context-aware completion (`expr.`, `expr?.`, `Type.`, `self.`, `#name`, `@attribute`,
  scope locals/fields/globals/keywords), signature help, document and workspace symbols,
  formatting, and quickfix code actions from diagnostic `FixIt`s.

### 9.16 Collections (Milestone 9.2a)
DESIGN.md 4.17 (9.2a) lists them; these are the rules. Iteration order is always defined
(insertion order for maps and sets), so scripts stay deterministic (DESIGN.md 4.1).
- **`Map<K, V>`** (reference type): `Map<str, int>()`, or a literal `{"a": 1, "b": 2}` (types
  from the expected type, else from the first entry; `{}` needs an annotation). `m[k]` reads
  (panics `key "x" is not in the map`), `m[k] = v` inserts or replaces, `m[k] += 1` works.
  Methods: `len`, `is_empty`, `has(k)`, `get(k) -> V?`, `get_or(k, default)`, `remove(k) -> bool`,
  `clear`, `keys() -> List<K>`, `values() -> List<V>`. `for k in m` and `for k, v in m`.
- **`Set<T>`** (reference type): `Set<int>()`, or a list literal where a set is expected
  (`var s: Set<str> = ["a", "b"]`; duplicates collapse). `len`, `is_empty`, `add(x) -> bool`
  (true when new), `remove(x) -> bool`, `contains(x)`, `clear`, `to_list`, `union(o)`,
  `intersection(o)`, `difference(o)` (new sets, left operand's order first). `for x in s`
  (no index form).
- **Keys** of maps and element types of sets must be hashable: integer types, `bool`, `str`,
  `name`, plain enums (by value) and classes (by identity). Floats, vectors, structs and
  optionals are rejected with a hint (round floats to an integer key).
  **Clarified:** order is insertion order; removing a key and inserting it again moves it to the
  end; replacing a value keeps its position. Hashes only place entries in the lookup table,
  which is never iterated, so class keys hashed by address are still deterministic.
- **Changing a map, set or deque while a `for` loop iterates over it** (adding or removing an
  element) panics at the loop's next step: `the map changed (an element was added or removed)
  while a 'for' loop iterates over it`. Assigning `m[k] = v` to an existing key is allowed.
  Collect the keys first (`for k in m.keys() { ... }`) to remove while iterating.
- **`[T; N]` fixed arrays** are **value types** like structs: assignment, argument passing and
  storing copy them (deeply, also when nested in structs or other arrays); `==` compares
  elements. `N` is an integer constant expression from 1 to 65535 (`const`s allowed). A
  variable or field without an initializer holds `N` zero values; a list literal where an
  array is expected must have exactly `N` elements. `a[i]` / `a[i] = v` are bounds-checked;
  `for x in a`, `for i, x in a`. Methods: the List methods that don't change the length
  (`len`, `is_empty`, `contains`, `sort`, `sort_by`, `reverse`, `index_of`, `slice`, `swap`,
  `first`, `last`, `map`, `filter`, `reduce`, `any`, `all`, `count`, `unique`, `shuffle`, `min`,
  `max`, `sum`, `zip`) plus `fill(x)` and `to_list()`; `push`, `pop`, `insert`, `remove`,
  `clear` and `extend` are errors.
- **`StrBuilder`** (reference type): `StrBuilder()`, `append(x)` (any value, converted like
  `"{x}"`; returns the builder so calls chain), `len` (bytes), `clear`, `is_empty`, `to_str()`.
- **`Deque<T>`** (reference type, ring buffer): `Deque<int>()` or a list literal where a deque is
  expected; `push_back`, `push_front`, `pop_back`, `pop_front`, `front`, `back` (all panic on an
  empty deque), `d[i]`, `d[i] = v`, `len`, `is_empty`, `clear`, `contains`, `to_list`;
  `for x in d`, `for i, x in d`.
- **`PriorityQueue<T>`** (reference type, binary min-heap): `PriorityQueue<T>()` orders by `<`
  (numbers, `str`, `Ord` types); `PriorityQueue<T>((a, b) => a.cost < b.cost)` by a "less"
  function. `push`, `pop` and `peek` (the smallest; panic when empty), `len`, `is_empty`,
  `clear`, `to_list()` (pop order). **Clarified:** equal elements come out in push order (a
  push counter breaks ties), so A* frontiers and event queues are deterministic. Not iterable.
- Printing: `{"a": 1}`, `Set[1, 2]`, `Deque[1, 2]`, arrays like lists, `StrBuilder("...")`,
  `PriorityQueue(len = 3)`.
- `List<T>()`-style constructors infer type arguments from the expected type
  (`var m: Map<str, int> = Map()`), else they must be written.

### 9.17 List, str and number methods, conversions (Milestone 9.2a)
- **List** (also see 9.9): `sort()` (by `<`) or `sort(less)` with a `fn(T, T) -> bool`;
  `sort_by(key)` (key computed once per element; key needs `<`); `reverse`; `index_of(x)` (-1 if
  absent); `slice(start, end = len)` (a copy, bounds clamped); `extend(other)`; `swap(i, j)`;
  `first()` / `last()` -> `T?` (null when empty); `map(f)`; `filter(pred)`;
  `reduce(init, f)` (`f(acc, x)`, the accumulator type is `init`'s); `any`, `all`, `count`
  (predicates); `unique()` (first occurrences, in order); `shuffle()`; `min()` / `max()` -> `T?`
  (first of equal elements); `sum()` (numbers or vectors; zero when empty; integer overflow
  panics as usual); `zip(other, f)`; `fill(x)`; `to_list()`.
  **Clarified:**
  - Sorting is a stable merge sort implemented by the language, so the result for a given
    comparator is identical on every platform and in VM and AOT, even when the comparator is
    not a strict order (it never crashes; the order is then unspecified but deterministic).
  - Float order for `sort()`, `min()`, `max()`: `<`, with every NaN after every number (NaNs
    equal to each other); `-0.0` and `0.0` are equal (stable order kept).
  - There are no tuples, so `zip` takes a combining function: `a.zip(b, (x, y) => x + y)`;
    its length is the shorter list's.
  - `shuffle()` uses the host's seeded stream (`VmOptions::random_below`; the engine installs
    its sub-seeded PCG32, DESIGN.md 4.15). In plain Pluma (REPL, tests) the VM has its own PCG32
    stream seeded with `VmOptions::random_seed` (default `0x853c49e6748fea9b`), so a fresh VM
    always shuffles the same way. A hot reload makes a new VM, which restarts that stream.
  - `map`/`filter`/... take any `fn` value, so lambdas get their parameter types:
    `xs.map(x => x * 2)`. Struct elements are copied out as usual (DESIGN.md 5.4).
- **str** (byte-indexed UTF-8; `len` is bytes): `split(sep)` (empty parts kept; an empty `sep`
  panics: use `chars()`), `sep.join(parts)`, `replace(from, to)` (all; empty `from` returns the
  string unchanged), `trim`, `trim_start`, `trim_end` (ASCII whitespace), `to_upper`, `to_lower`
  (ASCII letters only), `find(sub, start = 0) -> int` (byte index or -1),
  `substr(start, length = all)` (bytes, clamped; panics if it would split a UTF-8 character),
  `byte_at(i) -> int`, `chars() -> List<str>` (code points), `char_count()`, `repeat(n)`,
  `pad_left(width, fill = " ")` / `pad_right` (width counts code points; `fill` is one
  character), `lines()` (splits on `\n`, drops a `\r` before it and a final empty line),
  `is_empty`, `parse_int(radix = 10)`, `parse_float()`.
- **Conversions:** `parse_int(s, radix = 10) -> int?` and `parse_float(s) -> float?` (also as
  `str` methods) return null when the whole string is not a number: an optional `+`/`-` then
  digits of the radix (2..36, case-insensitive), no spaces or `0x` prefixes; values outside
  `int` are null. `parse_float` takes decimal and exponent forms, `inf` and `nan`, correctly
  rounded to `float`. **Decided by this milestone:** they return an optional, not a Result
  (the only failure is "not a number"); `?`, `??`, `if x? v` and `unwrap_or` all apply.
- **Number formatting:** integers have `to_hex()` (lower case) and `to_radix(base)` (2..36; a
  negative number gets a leading `-`); floats have `to_fixed(digits)` (0..32), exact and
  locale-independent (`std::to_chars`), so `2.5.to_fixed(0)` is `"2"` (round half to even on the
  exact binary value) on every platform. Default `"{x}"` formatting is unchanged (shortest
  round-trip).

### 9.18 Result, `?` and optionals (Milestone 9.2a)
DESIGN.md 5.8: expected failures return `Result<T, E>`; bugs panic.
- `Result<T, E>` is a **value type** (immutable; never null unless written `Result<T, E>?`).
  `Ok(v)` and `Err(e)` build one; their type comes from the expected type (a return value, an
  annotated variable, a parameter), else it is an error asking for one. `Ok`/`Err` are
  ordinary names otherwise (a user function `Ok` shadows them).
- Patterns: `Ok(p)` and `Err(p)` (`match`, `is`, `if r is Ok(v)`). A `match` on a Result is
  exhaustive with an irrefutable `Ok` and `Err` arm (or `_`).
- Methods: `is_ok`, `is_err`, `unwrap()` (panics `unwrap() on Err(...)`), `unwrap_err()`,
  `unwrap_or(default)`, `expect(message)` (panics `message: error`), `ok() -> T?`, `err() -> E?`.
  `==` compares variant and payload; printing gives `Ok(5)` / `Err("bad")`.
- **`x?`** (postfix, no space, LANGUAGE.md 1.6): on a `Result<T, E>` it is the `Ok` value, or it
  returns the `Err` (the same value: errors keep their identity) from the enclosing function,
  whose return type must be `Result<U, E2>` with `E` assignable to `E2`. On a nullable value
  (`T?` or a reference) it is the non-null value, or it returns `null` from a function whose
  return type is nullable. **Clarified:** the early return runs the function's `defer`s like
  `return`; `?` is an error at the top level of a script, inside `defer`, and in a lambda
  without a declared return type (annotate it: `fn(x: str) -> Result<int, str> { ... }`).
- **Optional helpers** on `T?` of a value type: `unwrap_or(default)` (same as `??`), `unwrap()`
  (panics `unwrap() on null`), `expect(message)`, `is_some()`, `is_none()`. References are
  always nullable (DESIGN.md 5.7) and keep using `??`, `?.` and `if x? v`; they don't get these
  methods (they could clash with the class's own).

### 9.19 Property accessors and method values (Milestone 9.2a)
- **Accessors** (syntax in 2.5) work on instance fields of classes and structs. A read of the
  field calls `get` when there is one; an assignment calls `set(v)`; a compound assignment
  (`x.hp -= 3`) reads through the getter and writes through the setter, evaluating the object
  once. Inside its own `get` or `set` (and lambdas in them) the field's name is its storage
  (`hp = clamp(v, 0, max_hp)`); everywhere else, including the class's other methods and
  `init`, the accessors run. The field initializer and struct literals set the storage
  directly (no setter). A **computed property** (a `get` and no initializer, type required) has
  no meaningful storage: assigning one without a `set` is an error (`property 'ratio' is
  read-only`). `@export` on a computed property and `weak` with accessors are errors; accessors
  on statics are errors. Accessors are not virtual (fields can't be overridden).
- **Method values:** `obj.method` (and an implicit `method` inside the class) without a call is
  a function value of the method's type (`fn(int) -> int`) that captures the receiver **now**
  (a struct receiver is copied) and calls the method virtually later, so overrides apply
  (`var f = b.who; f()`). Generic methods, coroutines and `init` can't be values; static methods
  already were (`Boss.spawn_at`). Built-in methods (`list.push`) are not values; wrap them in a
  lambda. Equality of function values is still not allowed. Hot reload keeps a method value
  when the class still has the method (its thunk's stable key is `<bound Class.method on
  Type>`).

### 9.20 Math library (Milestone 9.2b)
DESIGN.md 4.17 (9.2b) lists the scope. Everything here is deterministic (DESIGN.md 4.1): the
exact operations (`+ - * / sqrt floor ceil round`) and integer arithmetic are identical
everywhere, and the transcendentals (`sin`, `cos`, `atan2`, `exp`, `log`, `pow`) go through
the host's math (the engine's `dmath`). Results are bit-identical in the VM and in AOT-to-C.
The library is Pluma source, `pluma/std/math.plm`, compiled before user code (the engine embeds it
as the first prelude; `plm` and the language tests load it with `pluma::add_math_prelude`). Its
names are global like any prelude's, so a script cannot declare its own `gcd`, `Rect2`, `hash`...
Its API reference is in `docs/API.md` (generated from the doc comments).
- **Constants:** `PI`, `TAU`, `E`, `SQRT2`, `INF`, `NAN`, `FLOAT_EPSILON` (`1.1920929e-7`, the
  gap above 1.0), `INT_MAX`, `INT_MIN` (`int` is 32-bit). `NAN` has an unspecified sign; test it
  with `is_nan`, never `==`.
- **Scalar** (`float`): `trunc`, `fract` (`x - floor(x)`, so `fract(-0.25)` is 0.75), `fposmod`
  (floored modulo with the sign of the divisor), `hypot`, `copysign`, `snapped(x, increment)`,
  `pingpong(t, length)`, `angle_difference(from, to)` (in `[-PI, PI)`), `lerp_angle`,
  `rotate_toward(from, to, max_delta)`, `is_nan`, `is_inf`, `is_finite`,
  `is_equal_approx(a, b, epsilon = 0.00001)` (relative to the larger magnitude, absolute below 1),
  `bezier(p0, p1, p2, p3, t)`, `bezier_quad`, and `bezier_vec2` / `bezier_quad_vec2` for points,
  the hyperbolics `sinh cosh tanh asinh acosh atanh` (built from `exp` and `log`; small `|x|` use
  a series) and `cbrt` (exact for perfect cubes of moderate size; Newton steps from `exp`/`log`).
- **Easing:** `enum Ease { Sine, Quad, Cubic, Quart, Quint, Expo, Circ, Back, Elastic, Bounce }`
  with `ease_in(kind, t)`, `ease_out(kind, t)` (`1 - ease_in(kind, 1 - t)`) and
  `ease_in_out(kind, t)` (the in-curve scaled into each half). All are 0 at `t = 0` and 1 at
  `t = 1`; `t` is not clamped. `ease(x, curve)` is the Godot-style power ease
  (`curve > 1` in, `0 < curve < 1` out, `curve < 0` in-out; `x` clamped to [0, 1]).
  **Clarified:** the symmetric in-out construction means Elastic and Bounce in-out differ
  slightly from easings.net's hand-tuned formulas.
- **Integers** (`int`; intermediates are 64-bit, only a result that doesn't fit panics): `gcd`
  (never negative), `lcm`, `ipow(base, exp)` (the name `pow` is the float one), `isqrt`,
  `divmod(a, b) -> DivMod` (a struct with `quot` and `rem`, truncating like `/` and `%`; there
  are no tuples), `floor_div`, `floor_mod` (sign of the divisor), `popcount`, `clz`, `ctz` (as
  32 bits; `clz(0)` and `ctz(0)` are 32).
- **Overflow arithmetic** (built-ins on any one integer type, both operands the same type):
  `wrapping_add/sub/mul` wrap around, `checked_add/sub/mul` return `T?` (null on overflow),
  `saturating_add/sub/mul` clamp to the type's range. Plain `+ - *` still panic on overflow in
  development builds (9.2).
- **Vector methods** (all `vecN`): `floor()`, `ceil()`, `round()`, `fract()`, `sign()` per
  component, `snapped(step)`, `bounce(normal)` (the opposite of `reflect`), `move_toward(to,
  delta)` (never overshoots), `direction_to(to)` (a unit vector, zero when equal),
  `limit_length(max)` (keeps the direction). **`v[i] = e`** and `v[i] += e` assign one
  component of a mutable vector place (a `var`, a field, a list element), bounds-checked like
  `v[i]`; the index is evaluated once.
- **`ivec2`, `ivec3`, `ivec4`** are prelude structs with `int` components (grid cells), not a
  built-in kind: `ivec2.of(1, 2)`, `ivec2.floor_of(v)`, `to_vec()`, `+ - *` and unary `-`
  componentwise (`*` multiplies two vectors; `scaled(k)` by an `int`), `dot`, `length_squared`,
  `abs`, `min`, `max`, `clamp`, `==`. No swizzles, no indexing, and no `ivec5`+ (**cut**).

### 9.21 Rect2 and Transform2D (Milestone 9.2b)
- **`Rect2`** (struct: `pos`, `size`; y down): `Rect2.of(x, y, w, h)`, `Rect2.from_corners(a, b)`,
  `end()`, `center()`, `area()`, `has_area()`, `contains(p)` (half-open: `pos` is in, `end()`
  is out), `encloses(r)`, `intersects(r)` (touching edges don't count), `intersection(r)` (a
  zero-size rectangle when disjoint), `merge(r)`, `expand(point)`, `grow(amount)`, `abs()`.
- **`Transform2D`** (struct `a b c d tx ty`, default identity) is `cuervo/math/affine2.hpp`'s
  `Affine2` written in Pluma with the same operation order: `p' = (a*x + c*y + tx, b*x + d*y +
  ty)`, rotation `{cos, sin, -sin, cos}` (positive turns clockwise on screen). `Transform2D.identity()`,
  `.translate(v)`, `.scale(v)`, `.rotate(radians)`, `.from_trs(position, rotation, scale)`;
  `apply(p)`, `apply_vector(v)`, `determinant()`, `inverse()` (the identity when singular),
  `t1 * t2` (applies `t2` first), `origin()`, `rotation()`, `==`. `rotate` uses `cos`/`sin`, so in
  the engine it is bit-for-bit `Affine2::rotate` (tests/cuervo/math_tests.cpp).

### 9.22 Geometry (Milestone 9.2b)
Functions over `vec2` and `List<vec2>` polygons (the closing edge is implied; "clockwise" is on
screen, y down): `cross3(a, b, c)`, `closest_point_on_segment(p, a, b)`, `point_in_polygon(p,
poly)` (even-odd; points on an edge may go either way), `segment_intersection(p1, p2, p3, p4) ->
vec2?` (null when parallel, collinear or apart), `ray_circle_intersection(origin, dir, center,
radius) -> float?` (the distance along `dir` to the entry point, `0.0` if the origin is inside,
null on a miss or when the circle is behind), `polygon_signed_area` (positive = clockwise),
`polygon_area`, `convex_hull(points)` (monotone chain, clockwise, no repeated or collinear
points), `triangulate(poly) -> List<int>` (ear clipping of a **simple** polygon of either
winding: triangle index triples into `poly`, each clockwise; collinear vertices are skipped).
**Clarified / cut:** triangulation of polygons with holes or self-intersections would need
libtess2, which lives on the engine side; it is **not** exposed to scripts yet (the ear clipper
needs no engine code, so `pluma/` stays free of `cuervo/`).

### 9.23 Hashing and noise (Milestone 9.2b)
- `hash(x, seed = 0)`, `hash2(x, y, seed)`, `hash3(x, y, z, seed)`, `hash_str(s, seed)` give a
  stable 32-bit hash as an `int` (it can be negative; murmur3's finalizer, FNV-1a for strings);
  `hash_float(x, seed)` is the same in `[0, 1)`.
- **Noise** returns about `[-1, 1]` and is built only from `floor` and `+ - * /` on top of the
  integer hash, so it is bit-identical everywhere: `perlin1(x, seed)`, `perlin2(x, y, seed)`,
  `perlin3(x, y, z, seed)` (Perlin's improved noise; 0 at lattice points), `simplex2(x, y,
  seed)`, and `fractal_perlin2(x, y, octaves = 4, lacunarity = 2.0, gain = 0.5, seed)`
  (normalized). **Cut:** 3D simplex and other noise variants.

### 9.24 Random streams (Milestone 9.2b)
- **`Rng`** (class) is the engine's PCG32 (`cuervo/core/rng.hpp`) as a seeded local stream beside
  the global `Random`, never touching it: `Rng.seeded(seed)`, `Rng.with_sequence(state,
  sequence)`, and `Rng.derive(parent_seed, system, id, index)` which is exactly `derive_stream`
  (DESIGN.md 4.15 sub-streaming: a child depends only on its key). Methods: `next_u32()`,
  `below(bound)`, `range_int(lo, hi)` (inclusive), `next_float()` (`[0, 1)`), `range(lo, hi)`,
  `chance(p)`, `gaussian(mean, deviation)` (Box-Muller, always two draws), `in_circle(radius)`,
  `on_circle(radius)`, `pick(list)`, `weighted_pick(weights) -> int` (an index; negative weights
  count as 0), `shuffle(list)` (the same Fisher-Yates as `List.shuffle`). `Rng.seeded(s)`
  matches `Pcg32(s)` draw for draw; the one difference is `range_int(INT_MIN, INT_MAX)`, which
  draws a full 32-bit value where `Pcg32::range` returns `lo` without drawing.
- The engine's global `Random` (prelude `core.plm`) gains the same helpers as static functions:
  `Random.pick`, `weighted_pick`, `gaussian`, `in_circle`, `on_circle`, and `Random.fork() ->
  Rng` (a stream seeded from two draws). They use only `Random.range` / `range_int`, so they
  advance the simulation's one seeded PCG32. In the engine `List.shuffle` also draws from it
  (`VmOptions::random_below`); without a host it uses the VM's own PCG32 (9.17). Wall-clock
  time and unseeded randomness do not exist in the language.

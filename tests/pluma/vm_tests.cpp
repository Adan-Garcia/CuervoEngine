// Runtime behavior: programs are checked, compiled and run on the VM.

#include <doctest.h>

#include "tests/pluma/run_helpers.hpp"

using test::panics;
using test::prints;

TEST_CASE("vm: arithmetic, literals and printing") {
    prints("print(1 + 2 * 3)", "7\n");
    prints("print(7 / 2)\nprint(-7 / 2)\nprint(-7 % 3)\nprint(7 % -3)", "3\n-3\n-1\n1\n");
    prints("print(1.5 * 2.0)\nprint(1 + 2.0)\nprint(0.1 + 0.2)", "3.0\n3.0\n0.3\n");
    prints("print(0.1f64 + 0.2f64)", "0.30000000000000004\n");
    prints("var x: f64 = 2\nprint(x / 4)", "0.5\n");
    prints("print(true && !false)\nprint(1 < 2 && 2 <= 2)", "true\ntrue\n");
    prints("print(0xff & 0x0f | 0x30)\nprint(1 << 4)\nprint(-16 >> 2)\nprint(~0)", "63\n16\n-4\n-1\n");
    prints("print(255u8)\nprint(18446744073709551615u64)", "255\n18446744073709551615\n");
    prints("print(180deg)", "3.1415927\n");
    prints("print(7 as float / 2.0)\nprint(3.99 as int)\nprint(-1 as u8)\nprint(300 as u8)", "3.5\n3\n255\n44\n");
    prints("print(1e30 as int)\nprint(true as int)", "2147483647\n1\n");
    prints("print(min(3, 1))\nprint(max(2.5, 4.0))\nprint(abs(-5))\nprint(clamp(15, 0, 10))\nprint(sqrt(16.0))",
           "1\n4.0\n5\n10\n4.0\n");
}

TEST_CASE("vm: integer overflow panics in development, wraps in release") {
    panics("var x: u8 = 250\nx += 10", "2:1: integer overflow in '+' (u8)");
    panics("var x = 2147483647\nvar y = x + 1", "2:9: integer overflow in '+' (int)");
    panics("var x = -2147483647 - 1\nprint(x)\nprint(-x)", "3:7: integer overflow in '-' (int)", "-2147483648\n");
    panics("var big = 3037000500i64\nprint(big * big)", "2:7: integer overflow in '*' (i64)");
    panics("var z = 0\nprint(10 / z)", "2:7: division by zero");
    panics("var s = 40\nprint(1 << s)", "2:7: shift amount 40 is out of range for 'int'");
    test::Ran r = test::run("var x: u8 = 250\nx += 10\nprint(x)\nvar y = 2147483647\nprint(y + 1)", true);
    CHECK(r.ok);
    CHECK(r.out == "4\n-2147483648\n");
}

TEST_CASE("vm: strings and interpolation") {
    prints(R"(var name = "Ada"
var hp = 42
print("{name} has {hp * 10}%")
print("braces: {{}}")
print("a" + "b" == "ab")
print("abc" < "abd")
print("hello".len())
print("hello".contains("ell"))
print("hello".starts_with("he") && "hello".ends_with("lo")))",
           "Ada has 420%\nbraces: {}\ntrue\ntrue\n5\ntrue\ntrue\n");
    prints(R"(print("x = {1.5} {true} {#tag} {[1, 2]} {"q"}"))", "x = 1.5 true #tag [1, 2] q\n");
}

TEST_CASE("vm: closures capture variables by reference") {
    prints(R"(fn make_counter() -> fn() -> int {
    var n = 0
    return fn() -> int { n += 1; return n }
}
var a = make_counter()
var b = make_counter()
a()
a()
print(a())
print(b())
)",
           "3\n1\n");
    // The closure and the enclosing function share the variable.
    prints(R"(var log = List<str>()
fn run() {
    var x = 1
    var read = () => x
    x = 5
    log.push("{read()}")
    var assign = fn(v: int) { x = v }
    assign(9)
    log.push("{x}")
}
run()
print(log)
)",
           "[\"5\", \"9\"]\n");
    // Nested lambdas capture through the intermediate one.
    prints(R"(fn adder(a: int) -> fn(int) -> fn(int) -> int {
    return b => c => a + b + c
}
print(adder(1)(2)(3))
)",
           "6\n");
    // Each iteration's loop variable is a separate binding.
    prints(R"(var fs = List<fn() -> int>()
for i in 0..3 { fs.push(() => i * 10) }
for f in fs { print(f()) }
)",
           "0\n10\n20\n");
    // Lambda parameter types are inferred from the expected function type;
    // a block lambda's final expression is its result.
    prints(R"(fn apply(x: int, f: fn(int) -> int) -> int { return f(x) }
var k = 3
print(apply(4, x => x * k))
print(apply(4, fn(x) { x + k }))
)",
           "12\n7\n");
}

TEST_CASE("vm: structs are values") {
    prints(R"(struct V { var x: float; var y = 2.0 }
var a = V { x = 1.0 }
var b = a
b.x = 5.0
print(a)
print(b)
var list = [a, b]
list[0].y = 9.0
print(a.y)
print(list[0].y)
)",
           "V { x = 1.0, y = 2.0 }\nV { x = 5.0, y = 2.0 }\n2.0\n9.0\n");
    // Nested structs are deep-copied; methods may mutate `self`.
    prints(R"(struct Inner { var v = 1 }
struct Outer {
    var inner = Inner {}
    fn bump() { inner.v += 1 }
}
var o = Outer {}
var p = o
p.bump()
p.inner.v += 10
print(o.inner.v)
print(p.inner.v)
fn take(s: Outer) -> int {
    var c = s
    c.bump()
    return c.inner.v
}
print(take(o))
print(o.inner.v)
)",
           "1\n12\n2\n1\n");
    // Structural equality by default.
    prints("struct P { var a = 1; var b = \"x\" }\nprint(P {} == P { a = 1 })\nprint(P {} != P { b = \"y\" })",
           "true\ntrue\n");
}

TEST_CASE("vm: classes, inheritance and virtual methods") {
    prints(R"(class Enemy {
    var hp = 10
    var name = "enemy"
    fn hurt(n: int) { hp -= n }
    fn describe() -> str { return "{name}:{hp}" }
}
class Boss extends Enemy {
    var phase = 1
    fn init(hp: int) {
        self.hp = hp
        name = "boss"
    }
    fn hurt(n: int) {
        super.hurt(n * 2)
        phase = 2
    }
}
var e: Enemy = Boss(100)
e.hurt(5)
print(e.describe())
var plain = Enemy()
plain.hurt(1)
print(plain.describe())
print(e is Boss)
print(plain is Boss)
if e is Boss b { print(b.phase) }
var again = e as Boss
print(again.phase)
)",
           "boss:90\nenemy:9\ntrue\nfalse\n2\n2\n");
    // References are shared; identity comparison.
    prints(R"(class Box { var v = 0 }
var a = Box()
var b = a
b.v = 7
print(a.v)
print(a == b)
print(a == Box())
)",
           "7\ntrue\nfalse\n");
    // Static members.
    prints(R"(class Counter {
    static var count = 0
    static fn bump() -> int {
        count += 1
        return count
    }
}
Counter.bump()
print(Counter.bump())
print(Counter.count)
)",
           "2\n2\n");
    panics("class A { }\nclass B extends A { }\nvar a = A()\nvar b = a as B", "4:9: cannot cast 'A' to 'B'");
}

TEST_CASE("vm: enums and match") {
    prints(R"(enum Phase {
    Opening,
    Spiral(arms: int),
    Rage,
}
fn describe(p: Phase) -> str {
    match p {
        Phase.Opening => { return "opening" }
        Phase.Spiral(arms) if arms > 3 => { return "big spiral {arms}" }
        Phase.Spiral(arms) => { return "spiral {arms}" }
        Phase.Rage => { return "rage" }
    }
}
print(describe(Phase.Opening))
print(describe(Phase.Spiral(5)))
print(describe(Phase.Spiral(arms = 2)))
print(describe(Phase.Rage))
var p = Phase.Spiral(4)
print(p)
print(p == Phase.Spiral(4))
if p is Phase.Spiral(n) && n == 4 { print("four arms") }
print(Phase.Rage is Phase.Spiral)
)",
           "opening\nbig spiral 5\nspiral 2\nrage\nPhase.Spiral(arms = 4)\ntrue\nfour arms\nfalse\n");
    prints(R"(enum Dir: u8 { Up = 1, Down = 2, Left = 4 }
var d = Dir.Left
print(d)
print(d as int)
match d {
    Dir.Up | Dir.Down => print("vertical")
    Dir.Left => print("left")
}
)",
           "Dir.Left\n4\nleft\n");
    prints(R"(fn classify(n: int) -> str {
    match n {
        0 | 1 => { return "low" }
        2..=9 => { return "mid" }
        -5..0 => { return "negative" }
        _ => { return "other" }
    }
}
print(classify(1))
print(classify(9))
print(classify(-3))
print(classify(10))
match "b" {
    "a" => print("A")
    other => print("got {other}")
}
match true {
    true => print("yes")
    false => print("no")
}
)",
           "low\nmid\nnegative\nother\ngot b\nyes\n");
}

TEST_CASE("vm: lists and loops") {
    prints(R"(var xs = [3, 1, 4]
xs.push(1)
xs.insert(0, 9)
print(xs)
print(xs.len())
print(xs.pop())
print(xs.remove(0))
print(xs)
print(xs.contains(4))
var total = 0
for i, x in xs { total += i * x }
print(total)
for i in 0..10 step 3 { print(i) }
for i in 3..=1 step -1 { print(i) }
var n = 0
while n < 3 { n += 1 }
loop {
    n += 1
    if n > 5 { break }
}
print(n)
outer: for y in 0..3 {
    for x in 0..3 {
        if x == 2 { continue outer }
        if y == 2 { break outer }
        print("{x},{y}")
    }
}
)",
           "[9, 3, 1, 4, 1]\n5\n1\n9\n[3, 1, 4]\ntrue\n9\n0\n3\n6\n9\n3\n2\n1\n6\n0,0\n1,0\n0,1\n1,1\n");
    panics("var xs = [1, 2]\nprint(xs[2])", "2:7: index 2 is out of bounds for a list of length 2");
    panics("var xs = List<int>()\nxs.pop()", "2:1: pop() on an empty list");
    panics("for i in 0..3 step 0 { }", "1:1: a range step cannot be zero");
    // A range ending at the type's maximum does not overflow.
    prints("var c = 0\nfor i in 250u8..=255u8 { c += 1 }\nprint(c)", "6\n");
}

TEST_CASE("vm: generics are type-erased") {
    prints(R"(fn max_of<T: Ord>(a: T, b: T) -> T { return a > b ? a : b }
print(max_of(3, 7))
print(max_of(2.5, 1.0))
print(max_of("abc", "abd"))
print(max_of<i64>(1, 2))
class Stack<T> {
    var _items = List<T>()
    fn push(x: T) { _items.push(x) }
    fn pop() -> T { return _items.pop() }
    fn size() -> int { return _items.len() }
}
var s = Stack<str>()
s.push("a")
s.push("b")
print(s.pop() + s.pop())
print(s.size())
struct Pair<T> { var a: T; var b: T }
var p = Pair { a = 1, b = 2 }
print(p.a + p.b)
fn sum<T: Add>(xs: List<T>, zero: T) -> T {
    var t = zero
    for x in xs { t = t + x }
    return t
}
print(sum([1, 2, 3], 0))
print(sum(["x", "y"], ""))
)",
           "7\n2.5\nabd\n2\nba\n0\n3\n6\nxy\n");
}

TEST_CASE("vm: traits and operator overloading") {
    prints(R"(struct Complex: Add, Neg, Eq, Display {
    var re: float
    var im: float
    fn add(o: Complex) -> Complex { return Complex { re = re + o.re, im = im + o.im } }
    fn neg() -> Complex { return Complex { re = -re, im = -im } }
    fn eq(o: Complex) -> bool { return re == o.re && im == o.im }
    fn to_str() -> str { return "{re}+{im}i" }
}
var z = Complex { re = 1.0, im = 2.0 } + Complex { re = 0.5, im = 0.5 }
print(z)
print(z == Complex { re = 1.5, im = 2.5 })
z += Complex { re = 1.0, im = 0.0 }
print("z = {z}")
print(-z)
)",
           "1.5+2.5i\ntrue\nz = 2.5+2.5i\n-2.5+-2.5i\n");
    prints(R"(struct Money: Ord { var cents: int
    fn cmp(o: Money) -> int { return cents - o.cents }
}
print(Money { cents = 5 } < Money { cents = 7 })
fn biggest<T: Ord>(a: T, b: T) -> T { return a >= b ? a : b }
print(biggest(Money { cents = 9 }, Money { cents = 3 }).cents)
)",
           "true\n9\n");
    prints(R"(trait Damageable {
    fn hurt(amount: int)
    fn is_dead() -> bool { return health() <= 0 }
    fn health() -> int
}
class Crate: Damageable {
    var hp = 3
    fn hurt(amount: int) { hp -= amount }
    fn health() -> int { return hp }
}
struct Dummy: Damageable {
    fn hurt(amount: int) { }
    fn health() -> int { return 1 }
}
var c = Crate()
var targets = List<Damageable>()
targets.push(c)
targets.push(Dummy {})
for t in targets { t.hurt(2) }
print(c.hp)
print(c.is_dead())
c.hurt(1)
print(c.is_dead())
fn kill<T: Damageable>(x: T) { x.hurt(100) }
kill(c)
print(c.hp)
)",
           "1\nfalse\ntrue\n-100\n");
}

TEST_CASE("vm: null handling") {
    prints(R"(class Node {
    var next: Node
    var value = 1
}
var n = Node()
print(n.next?.value ?? -1)
n.next = Node()
n.next.value = 5
print(n.next?.value ?? -1)
var maybe: int? = null
print(maybe ?? 42)
maybe = 7
if maybe? v { print("got {v}") }
print(maybe == null)
var none: Node = null
print(none?.value)
)",
           "-1\n5\n42\ngot 7\nfalse\nnull\n");
    panics("class N { var next: N; var v = 0 }\nvar n = N()\nprint(n.next.v)", "3:7: null reference: 'n.next' is null");
    panics("class N { fn f() { } }\nvar n: N = null\nn.f()", "3:1: null reference: 'n' is null");
}

TEST_CASE("vm: defer, panics and assert") {
    prints(R"(fn f() -> int {
    defer print("first registered, runs last")
    defer { print("runs first") }
    for i in 0..2 {
        defer print("iteration {i} done")
        if i == 1 { return i }
    }
    return -1
}
print(f())
)",
           "iteration 0 done\niteration 1 done\nruns first\nfirst registered, runs last\n1\n");
    panics("assert(1 + 1 == 3, \"math\")", "1:1: assertion failed: math");
    panics("fn boom() { panic(\"kaboom\") }\nfn mid() { boom() }\nmid()", "1:13: kaboom");
    panics("fn r(n: int) -> int { return r(n + 1) }\nr(0)", "1:30: stack overflow (recursion too deep)");
}

TEST_CASE("vm: panics carry a stack trace") {
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    pluma::Session session(options);
    pluma::SessionResult r = session.run_script("trace.plm", "fn boom() {\n    panic(\"x\")\n}\nfn mid() { boom() }\nmid()\n");
    REQUIRE(r.panic);
    CHECK(r.panic->render() ==
          "trace.plm:2:5: panic: x\n"
          "    at boom (trace.plm:2:5)\n"
          "    at mid (trace.plm:4:12)\n"
          "    at <top level> (trace.plm:5:1)\n");
}

TEST_CASE("vm: panics carry each frame's live locals (DESIGN.md 5.8)") {
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    pluma::Session session(options);
    const char* src =
        "class Boss {\n"
        "    var hp = 3\n"
        "    fn to_str() -> str { return \"to_str must not run\" }\n"
        "    fn hurt(amount: int) {\n"
        "        var left = hp - amount\n"
        "        var label = \"boss\"\n"
        "        if left < 0 {\n"
        "            var xs = [1, 2, 3]\n"
        "            print(xs[left + 10])\n"
        "        }\n"
        "    }\n"
        "}\n"
        "fn outer(n: int) {\n"
        "    if n > 100 {\n"
        "        var hidden = 1\n"
        "        print(hidden)\n"
        "    }\n"
        "    var b = Boss()\n"
        "    var ok = true\n"
        "    b.hurt(n)\n"
        "    var after = 2\n"
        "    print(after)\n"
        "}\n"
        "outer(5)\n";
    pluma::SessionResult r = session.run_script("locals.plm", src);
    REQUIRE(r.panic);
    const pluma::PanicInfo& p = *r.panic;
    REQUIRE(p.frames.size() == p.trace.size());
    REQUIRE(p.frames.size() >= 2);
    const pluma::PanicFrame& hurt = p.frames[0];
    CHECK(hurt.function == "Boss.hurt");
    CHECK(hurt.file == "locals.plm");
    CHECK(hurt.line == 9);
    auto text = [](const pluma::PanicFrame& f) {
        std::string s;
        for (const pluma::PanicLocal& l : f.locals) s += l.name + ": " + l.type + " = " + l.value + "\n";
        return s;
    };
    CHECK(text(hurt) ==
          "self: Boss = Boss { hp = 3 }\n"
          "amount: int = 5\n"
          "left: int = -2\n"
          "label: str = \"boss\"\n"
          "xs: List<int> = [1, 2, 3]\n");
    const pluma::PanicFrame& outer = p.frames[1];
    CHECK(outer.function == "outer");
    CHECK(outer.line == 20);
    // `hidden` (a closed scope) and `after` (not declared yet) are not live.
    CHECK(text(outer) == "n: int = 5\nb: Boss = Boss { hp = 3 }\nok: bool = true\n");
    CHECK(p.render_with_locals().find("        left: int = -2\n") != std::string::npos);
    CHECK(out.empty());
}

TEST_CASE("vm: describe never runs script code and stops at a length") {
    pluma::Session session;
    pluma::SessionResult r = session.run_script(
        "d.plm", "class C { var xs = List<int>()\n fn to_str() -> str { panic(\"no\") } }\nvar c = C()\n"
                 "for i in 0..1000 { c.xs.push(i) }\n");
    REQUIRE(r.ok);
    pluma::VM& vm = session.vm();
    auto g = vm.global_index("c");
    REQUIRE(g);
    const std::string d = vm.describe(vm.globals[*g], 40);
    CHECK(d.starts_with("C { xs = [0, 1, 2"));
    CHECK(d.ends_with("..."));
    CHECK(d.size() <= 40 + 3);
}

TEST_CASE("vm: REPL sessions keep declarations and globals") {
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    pluma::Session s(options);
    CHECK(s.run_script("1", "var count = 1\nfn bump() { count += 1 }", true).ok);
    CHECK(s.run_script("2", "bump()\ncount", true).ok);
    CHECK(s.run_script("3", "class P { var hp = 3 }\nvar p = P()", true).ok);
    CHECK(s.run_script("4", "p.hp + count", true).ok);
    // A failed input changes nothing; the names stay free.
    pluma::SessionResult bad = s.run_script("5", "fn f() -> int { return \"x\" }", true);
    CHECK_FALSE(bad.compiled);
    CHECK(s.run_script("6", "fn f() -> int { return 4 }\nf()", true).ok);
    CHECK(s.run_script("7", "\"text\"\n[1, 2]", true).ok);
    CHECK(out == "2\n5\n4\n\"text\"\n[1, 2]\n");
}

TEST_CASE("vm: modules run main") {
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    pluma::Session s(options);
    pluma::SessionResult r = s.run_module("m.plm", R"(
const GREETING = "hi"
var calls = 0
fn main() {
    helper()
    print("{GREETING} {calls}")
}
fn helper() { calls += 1 }
)");
    CHECK(r.ok);
    CHECK(out == "hi 1\n");
}

TEST_CASE("vm: reference counting frees objects") {
    std::size_t before = pluma::live_objects();
    {
        test::Ran r = test::run(R"(class Node { var next: Node; var data = List<int>() }
fn build(n: int) -> Node {
    var head: Node = null
    for i in 0..n {
        var node = Node()
        node.data.push(i)
        node.next = head
        head = node
    }
    return head
}
var list = build(1000)
var s = "a"
for i in 0..100 { s = s + "b" }
var f = () => list
list = null
f = () => null
print(s.len())
)");
        CHECK(r.out == "101\n");
        CHECK(r.ok);
    }
    CHECK(pluma::live_objects() == before);
}

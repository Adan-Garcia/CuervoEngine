// Milestone 9.2a (DESIGN.md 4.17, LANGUAGE.md 9.16 - 9.19): collections,
// List / str / number methods, conversions, Result and `?`, optional
// helpers, property accessors and method values, in the VM. AOT parity of
// the same program is in aot_tests.cpp.

#include <doctest.h>

#include "pluma/format.hpp"
#include "pluma/heap.hpp"
#include "pluma/reload.hpp"
#include "pluma/session.hpp"
#include "tests/pluma/library_program.hpp"
#include "tests/pluma/run_helpers.hpp"

using test::panics;
using test::prints;
using test::rejects;

TEST_CASE("library: the 9.2a done-when program") { prints(test::library_program, test::library_expected); }

TEST_CASE("library: Map and Set keep insertion order through removals and growth") {
    prints(R"(
var m = Map<int, int>()
for i in 0..200 { m[i] = i * i }
for i in 0..200 { if i % 3 != 0 { m.remove(i) } }
m[1] = 1
var ks = m.keys()
print(ks.len())
print(ks.slice(0, 5))
print(ks.last() ?? 0)
print(m[198])
m.clear()
print(m.len())
m[5] = 6
print(m)
)",
           "68\n[0, 3, 6, 9, 12]\n1\n39204\n0\n{5: 6}\n");
    prints(R"(
enum Color { Red, Green, Blue }
class Node { var id = 0 }
var a = Node()
var b = Node()
var owners = Map<Node, str>()
owners[b] = "b"
owners[a] = "a"
print(owners[a] + owners[b])
var tags: Set<name> = [#x, #y, #x]
print(tags)
var cs = Set<Color>()
cs.add(Color.Blue)
cs.add(Color.Red)
print(cs.to_list())
var flags = {true: 1, false: 0}
print(flags[false])
)",
           "ab\nSet[#x, #y]\n[Color.Blue, Color.Red]\n0\n");
}

TEST_CASE("library: arrays are values") {
    prints(R"(
fn bump(a: [int; 3]) -> [int; 3] {
    var b = a
    b[0] += 1
    return b
}
var x: [int; 3] = [1, 2, 3]
var y = bump(x)
print(x)
print(y)
var rows = List<[int; 2]>()
var r: [int; 2]
rows.push(r)
r[0] = 5
rows.push(r)
print(rows)
var nested: [[int; 2]; 2]
nested[1][0] = 7
var n2 = nested
n2[1][0] = 8
print(nested)
print(n2)
for i, v in x { print("{i}={v}") }
const N = 3
var z: [str; N]
z.fill("-")
print(z)
print(z.to_list().len())
)",
           "[1, 2, 3]\n[2, 2, 3]\n[[0, 0], [5, 0]]\n[[0, 0], [7, 0]]\n[[0, 0], [8, 0]]\n0=1\n1=2\n2=3\n"
           "[\"-\", \"-\", \"-\"]\n3\n");
}

TEST_CASE("library: sort is stable, NaN goes last, comparators may be anything") {
    prints(R"(
struct P {
    var k: int
    var tag: str
}
var ps = [P { k = 2, tag = "a" }, P { k = 1, tag = "b" }, P { k = 2, tag = "c" }, P { k = 1, tag = "d" }]
ps.sort_by(p => p.k)
print(ps.map(p => p.tag))
ps.sort((a, b) => a.k > b.k)
print(ps.map(p => p.tag))
var fs = [2.0, 0.0 / 0.0, -1.0, 0.0 / 0.0, 1.0]
fs.sort()
print(fs.slice(0, 3))
print(fs[3] != fs[3] && fs[4] != fs[4])
var weird = [3, 1, 2, 5, 4, 9, 8, 7, 6, 0, 11, 13, 12, 10, 15, 14, 19, 18, 17, 16]
weird.sort((a, b) => true)
print(weird.len())
var strs = ["b", "B", "a", "ab"]
strs.sort()
print(strs)
)",
           "[\"b\", \"d\", \"a\", \"c\"]\n[\"a\", \"c\", \"b\", \"d\"]\n[-1.0, 1.0, 2.0]\ntrue\n20\n"
           "[\"B\", \"a\", \"ab\", \"b\"]\n");
}

TEST_CASE("library: shuffle uses the VM's seeded stream") {
    // Two fresh VMs give the same permutation (deterministic default seed).
    const char* src = "var d = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\nd.shuffle()\nprint(d)\nd.shuffle()\nprint(d)\n";
    test::Ran a = test::run(src);
    test::Ran b = test::run(src);
    REQUIRE(a.compiled);
    CHECK(a.out == b.out);
    // A host stream replaces it.
    std::string out;
    pluma::VmOptions o;
    o.out = [&out](std::string_view s) { out += s; };
    o.random_below = [](std::uint32_t bound) { return bound - 1; }; // never swaps
    pluma::Session s(o);
    REQUIRE(s.run_script("t.plm", src).ok);
    CHECK(out == "[0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\n[0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\n");
}

TEST_CASE("library: Result, ? and optionals") {
    prints(R"(
fn div(a: int, b: int) -> Result<int, str> {
    if b == 0 { return Err("div by zero") }
    return Ok(a / b)
}
fn calc(a: int, b: int, c: int) -> Result<int, str> {
    defer print("calc done")
    var x = div(a, b)?
    var y = div(x, c)?
    return Ok(x + y)
}
print(calc(8, 2, 2))
print(calc(8, 0, 2))
var r: Result<int, str> = Err("nope")
match r {
    Ok(v) if v > 0 => print("positive")
    Ok(_) => print("other")
    Err(e) => print(e)
}
print(r.ok() ?? 7)
print(r.err() ?? "")
print(div(1, 1) == div(2, 2))
var maybe: str? = null
print(maybe.unwrap_or("default"))
print(maybe.is_none())
)",
           "calc done\nOk(6)\ncalc done\nErr(\"div by zero\")\nnope\n7\nnope\ntrue\ndefault\ntrue\n");
}

TEST_CASE("library: Deque, PriorityQueue, StrBuilder") {
    prints(R"(
var d = Deque<int>()
for i in 0..20 { d.push_back(i) }
for i in 0..15 { d.pop_front() }
for i in 0..10 { d.push_front(-i) }
print(d.len())
print(d.to_list())
print(d.contains(-9))
var q = PriorityQueue<str>()
for w in ["pear", "apple", "fig"] { q.push(w) }
print(q.pop() + " " + q.peek())
var sb = StrBuilder()
sb.append(1.5).append(" ").append(true).append(" ").append([1, 2])
print(sb.to_str())
sb.clear()
print(sb.is_empty())
)",
           "15\n[-9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 15, 16, 17, 18, 19]\ntrue\napple fig\n1.5 true [1, 2]\ntrue\n");
}

TEST_CASE("library: panics") {
    panics("var m = {\"a\": 1}\nprint(m[\"b\"])", "2:7: key \"b\" is not in the map");
    panics("var m = {1: 1, 2: 2}\nfor k in m { m.remove(k) }",
           "2:1: the map changed (an element was added or removed) while a 'for' loop iterates over it");
    panics("var d = Deque<int>()\nd.pop_front()", "2:1: pop_front() on an empty deque");
    panics("var q = PriorityQueue<int>()\nq.pop()", "2:1: pop() on an empty priority queue");
    panics("var r: Result<int, str> = Err(\"bad\")\nr.unwrap()", "2:1: unwrap() on Err(\"bad\")");
    panics("var r: Result<int, str> = Err(\"bad\")\nr.expect(\"loading\")", "2:1: loading: \"bad\"");
    panics("var o: int? = null\no.unwrap()", "2:1: unwrap() on null");
    panics("print(\"héllo\".substr(2))", "1:7: substr(2, 2147483647) splits a UTF-8 character");
    panics("print(\"a\".split(\"\"))", "1:7: split: the separator is empty (use chars() to split into characters)");
    panics("var a: [int; 2]\nprint(a[2])", "2:7: index 2 is out of bounds for a list of length 2");
    panics("var l = [1]\nl.swap(0, 3)", "2:1: index 3 is out of bounds for a list of length 1");
    panics("print(5.to_radix(1))", "1:7: to_radix: base 1 is not in 2..36");
}

TEST_CASE("library: checker diagnostics for misuse") {
    rejects("var m = Map<float, int>()",
            "1:9: a map key of type 'float' cannot be hashed; use an integer, bool, str, name, plain enum or class "
            "type (round floats to an integer key)\n");
    rejects("var e = {}", "1:9: cannot infer the types of an empty map; write 'Map<K, V>()' or annotate the variable\n");
    rejects("var r = Ok(1)",
            "1:9: cannot infer the Result type of 'Ok(...)'; use it where a 'Result<T, E>' is expected (a return "
            "value, an annotated variable)\n");
    rejects("fn f() -> int {\n    var x = parse_int(\"1\")?\n    return x\n}",
            "2:13: '?' on 'int?' returns null when it is null, but this function returns 'int'\n");
    rejects("fn g() -> Result<int, str> {\n    var y: Result<int, int> = Ok(1)\n    return Ok(y?)\n}",
            "3:15: '?' would return error type 'int', but this function's error type is 'str'\n");
    rejects("var x = 1?", "1:9: '?' can only be used inside a function\n");
    rejects("var a: [int; 0]", "1:14: the size of '[T; N]' must be between 1 and 65535, got 0\n");
    rejects("var a: [int; 2] = [1, 2, 3]", "1:19: '[int; 2]' needs 2 elements, got 3\n");
    rejects("var a: [int; 2]\na.push(1)", "2:3: a fixed-size array has no method 'push' (its length is fixed; use a List)\n");
    rejects("var l = [vec2(0.0, 0.0)]\nl.sort()",
            "2:1: 'List.sort' needs elements with '<' (numbers, str, or a type implementing Ord); 'vec2' has none; "
            "pass an ordering: 'sort((a, b) => ...)'\n");
    rejects("for i, x in Set<int>() { }", "1:8: a set has no indices; iterate with 'for x in set'\n");
    rejects("var m = {1: 2}\nprint(m.contains(1))", "2:9: 'Map' has no method 'contains'; use 'has(key)'\n");
    rejects("class K {\n    var p: int {\n        get { return 1 }\n    }\n}\nvar k = K()\nk.p = 3",
            "7:1: property 'p' is read-only (it has a getter but no setter)\n");
    rejects("class K {\n    fn run() { yield }\n}\nvar f = K().run",
            "4:9: 'run' is a coroutine and cannot be used as a value; 'start' it instead\n");
    rejects("var l = [1]\nvar m = l.map(x => print(x))", "2:15: the function given to 'List.map' must return a value\n");
    rejects("var l = [1]\nvar n = l.filter(x => x)",
            "2:23: mismatched types: expected 'bool', found 'int'\n");
}

TEST_CASE("library: accessors run outside their own body only") {
    prints(R"(
struct Temp {
    var c = 0.0 {
        set(v) { c = max(v, -273.0) }
    }
    var f: float {
        get { return c * 1.8 + 32.0 }
        set(v) { c = (v - 32.0) / 1.8 }
    }
}
var t = Temp {}
t.f = 212.0
print(t.c)
t.c = -500.0
print(t.f)
var ts = [Temp {}]
ts[0].c += 10.0
print(ts[0].f)
)",
           "100.0\n-459.4\n50.0\n");
}

TEST_CASE("library: method values capture the receiver and dispatch virtually") {
    prints(R"(
class A {
    var n = 1
    fn who() -> str { return "A{n}" }
    fn caller() -> fn() -> str { return who }
}
class B extends A {
    fn who() -> str { return "B{n}" }
}
var b = B()
var f = b.who
var g = b.caller()
b.n = 5
print(f() + g())
var handlers = List<fn() -> str>()
handlers.push(A().who)
handlers.push(b.who)
print(handlers.map(h => h()))
)",
           "B5B5\n[\"A1\", \"B5\"]\n");
}

TEST_CASE("library: containers are traced by the cycle collector") {
    std::size_t before = pluma::live_objects();
    {
        std::string out;
        pluma::VmOptions o;
        o.out = [&out](std::string_view s) { out += s; };
        pluma::Session s(o);
        // Cycles through a Map value, a Set key, a Deque and a PriorityQueue.
        pluma::SessionResult r = s.run_script("t.plm", R"(
class N {
    var links = Map<str, N>()
    var members = Set<N>()
    var dq = Deque<N>()
    var pq = PriorityQueue<N>((a, b) => true)
    var res: Result<N, str> = Err("")
}
fn make() {
    for i in 0..50 {
        var a = N()
        var b = N()
        a.links["b"] = b
        b.links["a"] = a
        a.members.add(a)
        b.dq.push_back(b)
        a.pq.push(a)
        b.res = Ok(b)
    }
}
make()
)");
        REQUIRE_MESSAGE(r.ok, r.rendered);
        std::size_t garbage = pluma::live_objects();
        pluma::collect_all_cycles();
        CHECK(pluma::live_objects() < garbage);
    }
    pluma::collect_all_cycles();
    CHECK(pluma::live_objects() == before);
}

TEST_CASE("library: hot reload migrates the new containers") {
    auto load = [](pluma::Session& s, const char* src) {
        pluma::SessionResult r = s.load_files({{"game.plm", src}});
        INFO(r.rendered);
        REQUIRE(r.ok);
    };
    pluma::VmOptions o;
    pluma::Session v1(o);
    load(v1, R"(
class Bag {
    var counts = {"a": 1}
    var tags: Set<str> = ["x"]
    var q: Deque<int> = [1, 2]
    var pq = PriorityQueue<int>()
    var grid: [int; 2]
    var sb = StrBuilder()
    var res: Result<int, str> = Ok(3)
}
var bag = Bag()
fn setup() {
    bag.counts["b"] = 2
    bag.tags.add("y")
    bag.q.push_front(0)
    bag.pq.push(9)
    bag.pq.push(4)
    bag.grid[1] = 8
    bag.sb.append("hi")
    bag.res = Err("e")
}
)");
    pluma::VM& a = v1.vm();
    REQUIRE(a.call(*a.proto_by_key("setup"), {}).ok);
    std::string out;
    pluma::VmOptions o2;
    o2.out = [&out](std::string_view s) { out += s; };
    pluma::Session v2(o2);
    load(v2, R"(
class Bag {
    var counts = {"a": 1}
    var tags: Set<str> = ["x"]
    var q: Deque<int> = [1, 2]
    var pq = PriorityQueue<int>()
    var grid: [int; 2]
    var sb = StrBuilder()
    var res: Result<int, str> = Ok(3)
    fn show() { print("{counts} {tags} {q} {pq.pop()} {grid} {sb.to_str()} {res}") }
}
var bag = Bag()
)");
    pluma::VM& b = v2.vm();
    pluma::Migrator m(a, b);
    m.begin();
    m.migrate_globals();
    REQUIRE(m.finish().ok);
    CHECK(m.warnings().empty());
    REQUIRE(b.call_method(v2.global("bag"), "show").ok);
    CHECK(out == "{\"a\": 1, \"b\": 2} Set[\"x\", \"y\"] Deque[0, 1, 2] 4 [0, 8] hi Err(\"e\")\n");
}

TEST_CASE("library: debugger children and describe") {
    pluma::VmOptions o;
    pluma::Session s(o);
    pluma::SessionResult r = s.load_files({{"g.plm", R"(
var m = {"k": 1, "j": 2}
var st: Set<int> = [4, 5]
var d: Deque<str> = ["x"]
var arr: [int; 2] = [7, 8]
var res: Result<int, str> = Ok(1)
)"}});
    REQUIRE_MESSAGE(r.ok, r.rendered);
    pluma::VM& vm = s.vm();
    auto names = [&](const char* g) {
        std::string out;
        for (const pluma::ChildInfo& c : vm.children(s.global(g))) out += c.name + "=" + vm.describe(c.value) + " ";
        return out;
    };
    CHECK(names("m") == "[\"k\"]=1 [\"j\"]=2 ");
    CHECK(names("st") == "[0]=4 [1]=5 ");
    CHECK(names("d") == "[0]=\"x\" ");
    CHECK(names("arr") == "[0]=7 [1]=8 ");
    CHECK(names("res") == "value=1 ");
    CHECK(vm.describe(s.global("m")) == "{\"k\": 1, \"j\": 2}");
    CHECK(vm.describe(s.global("res")) == "Ok(1)");
}

TEST_CASE("library: the formatter round-trips the 9.2a syntax") {
    // Map literals, `[T; N]`, `?` and accessors already had syntax; the
    // formatted program is stable and still prints the same.
    pluma::FormatOptions fo;
    fo.script_mode = true;
    fo.check_style = false;
    pluma::FormatResult once = pluma::format_source("lib.plm", test::library_program, fo);
    REQUIRE_MESSAGE(once.ok, once.rendered);
    pluma::FormatResult twice = pluma::format_source("lib.plm", once.formatted, fo);
    REQUIRE(twice.ok);
    CHECK(twice.formatted == once.formatted);
    prints(once.formatted, test::library_expected);
}

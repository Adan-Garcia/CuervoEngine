// Declarations (LANGUAGE.md 2, EBNF 7) in module mode.

#include <doctest.h>

#include "tests/pluma/parse_helpers.hpp"

using test::check_round_trip;
using test::Root;

namespace {

void accepts(std::string_view text, std::string_view expected) { test::accepts(text, expected, Root::Module); }
void rejects(std::string_view text, std::string_view errors) { test::rejects(text, errors, Root::Module); }

} // namespace

TEST_CASE("variables and constants") {
    accepts("var hp = 100", "(var hp (init 100))");
    accepts("var dir: vec2", "(var dir (type vec2))");
    accepts("var t: Boss? = null", "(var t (type (? Boss)) (init null))");
    accepts("var grid: [int; 16]", "(var grid (type (array int 16)))");
    accepts("var cb: fn(int, str) -> bool", "(var cb (type (fn (params int str) bool)))");
    accepts("var nested: List<List<int>>", "(var nested (type (List (List int))))");
    accepts("weak var owner: Boss", "(var owner weak (type Boss))");
    accepts("const MAX_HP = 400", "(const MAX_HP 400)");
    accepts("pub const SPEED: float = 2.0", "(const SPEED pub (type float) (float 2))");
    rejects("const X", "1:8: expected '=' and a value for constant 'X', found end of file\n");
    rejects("var = 3", "1:5: expected variable name, found '='\n");
    rejects("var x: = 3", "1:8: expected type, found '='\n");
}

TEST_CASE("functions: parameters, defaults, return types") {
    accepts("fn add(a: int, b: int) -> int {\n    return a + b\n}",
            "(fn add (params (a int) (b int)) (ret int) (block (return (+ a b))))");
    accepts("fn ring(count: int, speed = 150.0, spin = 0.0) { }",
            "(fn ring (params (count int) (speed (= (float 150))) (spin (= (float 0)))) (block))");
    accepts("fn log_all(items: List<str>) { }", "(fn log_all (params (items (List str))) (block))");
    accepts("fn f(\n    a: int,\n    b: int,\n) { }", "(fn f (params (a int) (b int)) (block))");
    accepts("fn f()\n{\n}", "(fn f (params) (block))");
    rejects("fn f(a) { }", "1:6: parameter 'a' needs a type or a default value\n");
    rejects("fn f(a = 1, b: int) { }", "1:13: required parameter 'b' follows a parameter with a default value\n");
    rejects("fn f() -> int", "1:14: expected '{' to start the body of 'f', found end of file\n");
    rejects("fn (a: int) { }", "1:4: expected function name, found '('\n");
}

TEST_CASE("generics") {
    accepts("class Stack<T> {\n    var _items = List<T>()\n    fn push(x: T) { _items.push(x) }\n}",
            "(class Stack (generics T) (var _items (init (call (generic List (types T))))) "
            "(fn push (params (x T)) (block (call (. _items push) x))))");
    accepts("fn max<T: Ord>(a: T, b: T) -> T { return a > b ? a : b }",
            "(fn max (generics (T Ord)) (params (a T) (b T)) (ret T) (block (return (?: (> a b) a b))))");
    accepts("fn sum<T: Add + Default>(xs: List<T>) -> T { }",
            "(fn sum (generics (T Add Default)) (params (xs (List T))) (ret T) (block))");
    accepts("fn pair<A, B>(a: A, b: B) { }", "(fn pair (generics A B) (params (a A) (b B)) (block))");
}

TEST_CASE("classes: extends, traits, signals, accessors, modifiers") {
    accepts("@export_group(\"Combat\")\n"
            "class Boss extends Enemy: Damageable, Saveable {\n"
            "    signal defeated\n"
            "    signal hp_changed(value: int)\n"
            "\n"
            "    @export(range = 0..1000)\n"
            "    var max_hp = 400\n"
            "\n"
            "    var hp = 400 {\n"
            "        set(v) {\n"
            "            hp = clamp(v, 0, max_hp)\n"
            "            emit hp_changed(hp)\n"
            "        }\n"
            "    }\n"
            "\n"
            "    var ratio: float {\n"
            "        get { return hp as float / max_hp }\n"
            "    }\n"
            "\n"
            "    static var count = 0\n"
            "    var _phase = Phase.Opening\n"
            "\n"
            "    fn init(pos: vec2, hp = 400) { self.position = pos; self.hp = hp }\n"
            "    final fn hurt(amount: int) { hp -= amount }\n"
            "    static fn spawn_at(p: vec2) -> Boss { return Boss() }\n"
            "}",
            "(class Boss (attrs (@export_group () (str \"Combat\"))) (extends Enemy) (traits Damageable Saveable) "
            "(signal defeated) (signal hp_changed (params (value int))) "
            "(var max_hp (attrs (@export () (named range (.. 0 1000)))) (init 400)) "
            "(var hp (init 400) (accessors (set v (block (= hp (call clamp v 0 max_hp)) (emit hp_changed () hp))))) "
            "(var ratio (type float) (accessors (get (block (return (/ (as hp float) max_hp)))))) "
            "(var count static (init 0)) (var _phase (init (. Phase Opening))) "
            "(fn init (params (pos vec2) (hp (= 400))) (block (= (. self position) pos) (= (. self hp) hp))) "
            "(fn hurt final (params (amount int)) (block (-= hp amount))) "
            "(fn spawn_at static (params (p vec2)) (ret Boss) (block (return (call Boss)))))");
    accepts("manual class ParticleSystem { }", "(class ParticleSystem manual)");
    accepts("class A {}", "(class A)");
    // `{ get` is an accessor block, not a struct literal `max_hp { ... }`.
    accepts("class A {\n    var hp = max_hp { get { return 1 } }\n}",
            "(class A (var hp (init max_hp) (accessors (get (block (return 1))))))");
    rejects("var g = 1 { get { return 1 } }", "1:1: accessors are only allowed on fields\n");
}

TEST_CASE("class errors") {
    rejects("class { }", "1:7: expected class name, found '{'\n");
    rejects("class A {\n    var x = \n}", "3:1: expected expression, found '}'\n");
    rejects("class A {\n    print(1)\n    var ok = 1\n}", "2:5: expected declaration, found 'print'\n");
    rejects("class A {\n    class B { }\n}", "2:5: 'class' declarations are only allowed at the top level\n");
    rejects("class A {\n    pub var x = 1\n}", "2:5: 'pub' is only allowed on top-level declarations\n");
    rejects("class A {\n    static final fn f() { }\n}", "2:5: a member cannot be both 'static' and 'final'\n");
    rejects("class A {\n    final final fn f() { }\n}", "2:11: duplicate 'final' modifier\n");
    rejects("class A {\n    fn f()\n}", "2:11: expected '{' to start the body of 'f', found newline\n");
    rejects("class A {\n    var x = 1 {\n        set(v) { }\n        set(w) { }\n    }\n}", "4:9: duplicate setter\n");
    rejects("class A {\n    var x = 1 { put { } }\n}", "2:17: expected 'get' or 'set', found 'put'\n");
    rejects("static var x = 1", "1:1: 'static' is only allowed on class and struct members\n");
    rejects("signal s", "1:1: signals can only be declared in a class\n");
    rejects("class A {\n    signal s(x: int = 1)\n}", "2:14: signal parameters cannot have default values\n");
    rejects("class A {\n    signal s(x)\n}", "2:14: parameter 'x' needs a type\n");
    rejects("manual struct S { }", "1:8: expected 'class' after 'manual', found 'struct'\n");
}

TEST_CASE("structs") {
    accepts("struct Bullet {\n    var pos: vec2\n    var vel: vec2\n    var ttl = 3.0\n}",
            "(struct Bullet (var pos (type vec2)) (var vel (type vec2)) (var ttl (init (float 3))))");
    accepts("struct Complex: Add, Mul, Eq {\n"
            "    var re: float\n"
            "    var im: float\n"
            "    fn add(o: Complex) -> Complex { return Complex { re = re + o.re, im = im + o.im } }\n"
            "}",
            "(struct Complex (traits Add Mul Eq) (var re (type float)) (var im (type float)) "
            "(fn add (params (o Complex)) (ret Complex) (block (return (struct Complex (re (+ re (. o re))) "
            "(im (+ im (. o im))))))))");
    accepts("struct P<T> { static fn zero() -> P<T> { return P<T> { } } }",
            "(struct P (generics T) (fn zero static (params) (ret (P T)) (block (return (struct (P T))))))");
    rejects("struct S extends B { }", "1:10: structs cannot inherit; use a class, or implement traits with ':'\n");
    rejects("struct S {\n    signal s\n}", "2:5: signals can only be declared in a class\n");
    rejects("struct S {\n    final fn f() { }\n}", "2:5: 'final' is only allowed on class members\n");
}

TEST_CASE("enums") {
    accepts("enum Phase {\n    Opening,\n    Spiral(arms: int),\n    Rage,\n}",
            "(enum Phase Opening (Spiral (params (arms int))) Rage)");
    accepts("enum Dir: u8 { Up = 0, Down = 1, Left = 2, Right = 3 }",
            "(enum Dir (backing u8) (Up (= 0)) (Down (= 1)) (Left (= 2)) (Right (= 3)))");
    accepts("enum Empty {}", "(enum Empty)");
    accepts("enum Shape { Circle(r: float), Rect(w: float, h: float) }",
            "(enum Shape (Circle (params (r float))) (Rect (params (w float) (h float))))");
    test::Parsed p = test::parse("enum E {\n    A\n    B\n}", Root::Module);
    CHECK(p.errors == "3:5: expected ',' between enum variants\n");
    CHECK(p.rendered.find("fix-it: insert ','") != std::string::npos);
    CHECK(p.first == "(enum E A B)");
    rejects("enum E { A, 3 }", "1:13: expected enum variant name, found '3'\n");
    rejects("enum E { A(x) }", "1:12: parameter 'x' needs a type\n");
}

TEST_CASE("traits") {
    accepts("trait Damageable {\n    fn hurt(amount: int)\n    fn is_dead() -> bool { return false }\n}",
            "(trait Damageable (fn hurt (params (amount int))) (fn is_dead (params) (ret bool) (block (return false))))");
    accepts("trait Ord2: Eq { fn cmp(o: Self) -> int }", "(trait Ord2 (traits Eq) (fn cmp (params (o Self)) (ret int)))");
    rejects("trait T {\n    var x = 1\n}", "2:5: traits can only contain functions\n");
}

TEST_CASE("attributes") {
    accepts("@inline\nfn f() { }", "(fn f (attrs @inline) (params) (block))");
    accepts("@cold @deprecated(\"use g\")\nfn f() { }",
            "(fn f (attrs @cold (@deprecated () (str \"use g\"))) (params) (block))");
    accepts("@acyclic\nclass Leaf { }", "(class Leaf (attrs @acyclic))");
    accepts("class A {\n    @export(range = 50.0..400.0, step = 0.5)\n    @tooltip(\"speed\")\n    var speed = 200.0\n}",
            "(class A (var speed (attrs (@export () (named range (.. (float 50) (float 400))) (named step (float 0.5))) "
            "(@tooltip () (str \"speed\"))) (init (float 200))))");
    rejects("@\nfn f() { }", "1:2: expected attribute name after '@', found newline\n");
}

TEST_CASE("module mode only takes declarations") {
    rejects("print(1)\nvar ok = 1", "1:1: expected declaration, found 'print'\n");
    rejects("}\nvar ok = 1", "1:1: expected declaration, found '}'\n");
    test::Parsed p = test::parse("fn a( {\n    x\n}\nfn b() { }\nclass C { }", Root::Module);
    CHECK(p.errors == "1:7: expected parameter name, found '{'\n");
    CHECK(p.dump.find("(fn b") != std::string::npos);
    CHECK(p.dump.find("(class C)") != std::string::npos);
}

TEST_CASE("round trip: declarations") {
    check_round_trip("@export_group(\"Combat\")\n"
                     "pub class Boss<T: Ord + Eq> extends Enemy: Damageable {\n"
                     "    signal hp_changed(value: int)\n"
                     "    @export(range = 0..1000) var max_hp = 400\n"
                     "    var hp = 400 { get { return 1 } set(v) { hp = v } }\n"
                     "    weak var owner: Boss?\n"
                     "    static var count = 0\n"
                     "    final fn hurt(amount: int, scale = 1.0) -> bool { return true }\n"
                     "}\n"
                     "manual class Pool { }\n"
                     "struct V: Add { var x: float; fn add(o: V) -> V { return V { x = x + o.x } } }\n"
                     "enum Dir: u8 { Up = 0, Down = 1 }\n"
                     "enum Phase { Opening, Spiral(arms: int), Rage }\n"
                     "trait T: U { fn f(); fn g() -> int { return 1 } }\n"
                     "const N: [int; 4] = [1, 2, 3, 4]\n"
                     "var f: fn(int) -> fn() -> bool\n"
                     "var s = \"\"\"\n    multi\n      line {x}\n    \"\"\"\n"
                     "var r = r\"raw {x}\"\n",
                     Root::Module);
}

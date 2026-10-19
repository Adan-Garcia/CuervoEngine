// Expressions: precedence (LANGUAGE.md 4.1), literals (1.5), the `?` rule
// (1.6), lambdas (2.4), calls (2.3), struct literals (2.6), manual memory (2.11).

#include <doctest.h>

#include <cmath>

#include "tests/pluma/parse_helpers.hpp"

using test::accepts;
using test::accepts_expr;
using test::rejects;
using test::Root;

TEST_CASE("precedence: arithmetic, shifts, bitwise") {
    accepts_expr("1 + 2 * 3", "(+ 1 (* 2 3))");
    accepts_expr("(1 + 2) * 3", "(* (+ 1 2) 3)");
    accepts_expr("a - b - c", "(- (- a b) c)");
    accepts_expr("a - (b - c)", "(- a (- b c))");
    accepts_expr("a / b % c", "(% (/ a b) c)");
    accepts_expr("x << 1 + 2", "(<< x (+ 1 2))");
    accepts_expr("x >> 2", "(>> x 2)");
    accepts_expr("a | b ^ c & d", "(| a (^ b (& c d)))");
    accepts_expr("a & b | c", "(| (& a b) c)");
}

TEST_CASE("precedence: bitwise binds tighter than comparison (fixes C's a & b == 0)") {
    accepts_expr("a & b == 0", "(== (& a b) 0)");
    accepts_expr("a | 1 < b", "(< (| a 1) b)");
}

TEST_CASE("precedence: unary, as, logic, ??, ternary") {
    accepts_expr("-a * b", "(* (neg a) b)");
    accepts_expr("-a as int * 2", "(* (as (neg a) int) 2)");
    accepts_expr("-(a as int)", "(neg (as a int))");
    accepts_expr("hp as float / max_hp", "(/ (as hp float) max_hp)");
    accepts_expr("!x.y", "(not (. x y))");
    accepts_expr("~mask & 0xff", "(& (bitnot mask) 255)");
    accepts_expr("- -x", "(neg (neg x))");
    accepts_expr("a || b && c", "(|| a (&& b c))");
    accepts_expr("(a || b) && c", "(&& (|| a b) c)");
    accepts_expr("a == b && c != d", "(&& (== a b) (!= c d))");
    accepts_expr("a ?? b ?? c", "(?? a (?? b c))");
    accepts_expr("(a ?? b) ?? c", "(?? (?? a b) c)");
    accepts_expr("a ?? b || c", "(?? a (|| b c))");
    accepts_expr("c ? a : b ? d : e", "(?: c a (?: b d e))");
    accepts_expr("x ?? 0 ? 1 : 2", "(?: (?? x 0) 1 2)");
    accepts_expr("(c ? a : b) ? d : e", "(?: (?: c a b) d e)");
    accepts_expr("a as int as float", "(as (as a int) float)");
    accepts_expr("x as Boss? ?? fallback", "(?? (as x (? Boss)) fallback)");
}

TEST_CASE("precedence: ranges and is") {
    accepts_expr("0..8", "(.. 0 8)");
    accepts_expr("0..=8 step 2", "(..= 0 8 (step 2))");
    accepts_expr("a + 1..b * 2", "(.. (+ a 1) (* b 2))");
    accepts_expr("x == 0..8", "(== x (.. 0 8))");
    accepts_expr("x is Boss", "(is x Boss)");
    accepts_expr("x is Boss && y", "(&& (is x Boss) y)");
    accepts_expr("x is Phase.Spiral(arms)", "(is x (Phase.Spiral arms))");
    accepts_expr("node is Boss b", "(is node Boss (bind b))");
    accepts_expr("(a < b) == (c < d)", "(== (< a b) (< c d))");
}

TEST_CASE("comparisons, equality and ranges don't chain") {
    rejects("a < b < c", "1:7: comparison operators cannot be chained\n", Root::Expr);
    rejects("a == b != c", "1:8: comparison operators cannot be chained\n", Root::Expr);
    rejects("0..1..2", "1:5: range operators cannot be chained\n", Root::Expr);
    rejects("x is A is B", "1:8: comparison operators cannot be chained\n", Root::Expr);
    // The fix-it rewrites the chain as a conjunction.
    test::Parsed p = test::parse("lo <= x < hi", Root::Expr);
    CHECK(p.rendered == "test.plm:1:9: error: comparison operators cannot be chained\n"
                        " 1 | lo <= x < hi\n"
                        "   |         ^\n"
                        "   = fix-it: replace 'lo <= x < hi' with 'lo <= x && x < hi'\n");
    // Explicit parentheses are fine.
    accepts_expr("(a < b) < c", "(< (< a b) c)");
}

TEST_CASE("postfix: calls, index, members, safe access, propagation") {
    accepts_expr("a.b?.c[0](1)?", "(try (call (index (?. (. a b) c) 0) 1))");
    accepts_expr("load()? + 1", "(+ (try (call load)) 1)");
    accepts_expr("f()?.x", "(?. (call f) x)");
    accepts_expr("p.yx", "(. p yx)");
    accepts_expr("s[4]", "(index s 4)");
    accepts_expr("map.get(k)", "(call (. map get) k)"); // keywords are fine as member names
    accepts_expr("x.start().free", "(. (call (. x start)) free)");
    accepts_expr("(-x).y", "(. (neg x) y)");
    accepts_expr("(f)()", "(call f)");
}

TEST_CASE("the ? whitespace rule (LANGUAGE.md 1.6)") {
    accepts_expr("ok ? 1 : 2", "(?: ok 1 2)");
    accepts_expr("a ?? b", "(?? a b)");
    accepts_expr("a?.b", "(?. a b)");
    accepts_expr("load()?", "(try (call load))");
    test::Parsed p = test::parse("a?b:c", Root::Expr);
    CHECK(p.errors == "1:2: ternary '?' needs spaces around it (a '?' right after an expression propagates an "
                      "error)\n");
    CHECK(p.rendered.find("= fix-it: replace 'a?b:c' with 'a ? b : c'") != std::string::npos);
    rejects("x = ready? 1 : 2",
            "1:10: ternary '?' needs spaces around it (a '?' right after an expression propagates an error)\n");
    rejects("x = f(n)?g(n):0",
            "1:9: ternary '?' needs spaces around it (a '?' right after an expression propagates an error)\n");
}

TEST_CASE("integer and float literals") {
    accepts_expr("42", "42");
    accepts_expr("1_000_000", "1000000");
    accepts_expr("0xff", "255");
    accepts_expr("0b1010", "10");
    accepts_expr("42u8", "(int 42 u8)");
    accepts_expr("7i64", "(int 7 i64)");
    accepts_expr("1.5", "(float 1.5)");
    accepts_expr("1e-3", "(float 0.001)");
    accepts_expr("2.5f64", "(float 2.5 f64)");
    accepts_expr("0..8", "(.. 0 8)");
    rejects("99999999999999999999", "1:1: integer literal is too large\n", Root::Expr);
    rejects("0x10deg", "1:1: invalid number literal\n", Root::Expr);
    rejects("1.5u8", "1:1: integer suffix on a float literal\n", Root::Expr);
    // Printing keeps the source spelling.
    CHECK(test::parse("0xFF_FF", Root::Expr).printed == "0xFF_FF");
}

TEST_CASE("deg literals are floats in radians") {
    test::Parsed p = test::parse("45deg", Root::Expr);
    CHECK(p.errors.empty());
    CHECK(p.dump == "(float 0.7853981633974483 deg)");
    accepts_expr("12.5deg", "(float 0.2181661564992912 deg)");
    accepts_expr("-90deg", "(neg (float 1.5707963267948966 deg))");
    accepts_expr("180deg", "(float 3.141592653589793 deg)");
    CHECK(p.printed == "45deg");
}

TEST_CASE("strings: escapes, interpolation, raw, multiline") {
    accepts_expr(R"("hello")", R"((str "hello"))");
    accepts_expr(R"("")", "(str)");
    accepts_expr(R"("Score: {score}")", R"((str "Score: " ({} score)))");
    accepts_expr(R"("{player.name} has {hp * 10}%")", R"((str ({} (. player name)) " has " ({} (* hp 10)) "%"))");
    accepts_expr(R"("brace: {{ and }}")", R"((str "brace: { and }"))");
    accepts_expr(R"("a\n\t\"\\\0b")", R"((str "a\n\t\"\\\0b"))");
    accepts_expr(R"("\u{41}\u{e9}")", "(str \"A\xC3\xA9\")");
    accepts_expr(R"(r"raw\n {no} escapes")", R"((str raw "raw\\n {no} escapes"))");
    accepts_expr(R"("nested {f("x{y}")} done")", R"((str "nested " ({} (call f (str "x" ({} y)))) " done"))");
    accepts_expr(R"("{a ? "yes" : "no"}")", R"((str ({} (?: a (str "yes") (str "no")))))");
    accepts_expr("\"\"\"\n    line one\n      indented\n    \"\"\"",
                 R"((str multiline "line one\n  indented"))");
    accepts_expr("\"\"\"\n    a\n\n    b\n    \"\"\"", R"((str multiline "a\n\nb"))");
    accepts_expr("\"\"\"inline\"\"\"", R"((str multiline "inline"))");
    accepts_expr("\"\"\"\n    v = {x}\n    \"\"\"", R"((str multiline "v = " ({} x)))");
}

TEST_CASE("strings: errors point into the string") {
    rejects(R"(x = "bad \q escape")", "1:10: invalid escape sequence '\\q'\n");
    rejects(R"(x = "a}b")", "1:7: unmatched '}' in string; write '}}' for a literal brace\n");
    rejects(R"(x = "{}")", "1:7: empty interpolation in string\n");
    rejects(R"(x = "v: {a b}")", "1:12: expected '}' to end the interpolation, found 'b'\n");
    rejects(R"(x = "\u{zz}")", "1:6: invalid unicode escape; expected '\\u{XXXX}'\n");
    rejects("x = \"open", "1:5: unterminated string\n");
    rejects("x = \"open\ny = 1", "1:5: unterminated string (strings cannot span lines)\n");
}

TEST_CASE("names, bools, null, self, super") {
    accepts_expr("#shoot", "#shoot");
    accepts_expr("true && !false", "(&& true (not false))");
    accepts_expr("x ?? null", "(?? x null)");
    accepts_expr("self.hp", "(. self hp)");
    accepts_expr("super.ready()", "(call (. super ready))");
}

TEST_CASE("list and map literals") {
    accepts_expr("[1, 2, 3]", "(list 1 2 3)");
    accepts_expr("[]", "(list)");
    accepts_expr("[\n    1,\n    2,\n]", "(list 1 2)");
    accepts_expr(R"({"a": 1, "b": 2})", R"((map (entry (str "a") 1) (entry (str "b") 2)))");
    accepts_expr("{}", "(map)");
    rejects("[1, 2", "1:6: expected ',' or ']' in list literal, found end of file\n", Root::Expr);
}

TEST_CASE("calls and named arguments (LANGUAGE.md 2.3)") {
    accepts_expr("ring(36)", "(call ring 36)");
    accepts_expr("ring(36, spin = 12.0)", "(call ring 36 (named spin (float 12)))");
    accepts_expr("ring(count = 36, speed = 200.0)", "(call ring (named count 36) (named speed (float 200)))");
    accepts_expr("f(a == b)", "(call f (== a b))");
    accepts_expr("f(\n    1,\n    2,\n)", "(call f 1 2)");
    rejects("ring(spin = 1.0, 36)", "1:18: positional argument after named arguments\n", Root::Expr);
    rejects("ring(spin = 1.0, spin = 2.0)", "1:18: argument 'spin' is given more than once\n", Root::Expr);
    rejects("f(a b)", "1:5: expected ',' or ')' in argument list, found 'b'\n", Root::Expr);
}

TEST_CASE("generic calls vs comparisons") {
    accepts_expr("List<vec2>()", "(call (generic List (types vec2)))");
    accepts_expr("max<int>(a, b)", "(call (generic max (types int)) a b)");
    accepts_expr("Map<str, List<int>>()", "(call (generic Map (types str (List int))))");
    accepts_expr("x.cast<Boss>()", "(call (. x cast (types Boss)))");
    accepts_expr("a < b", "(< a b)");
    accepts_expr("f(a < b, c > d)", "(call f (< a b) (> c d))");
    accepts_expr("a < b && c > d", "(&& (< a b) (> c d))");
}

TEST_CASE("lambdas (LANGUAGE.md 2.4)") {
    accepts_expr("e => e.hp > 0", "(=> (params e) (> (. e hp) 0))");
    accepts_expr("(a, b) => a.score > b.score", "(=> (params a b) (> (. a score) (. b score)))");
    accepts_expr("() => 1", "(=> (params) 1)");
    accepts_expr("enemies.filter(e => e.hp > 0)", "(call (. enemies filter) (=> (params e) (> (. e hp) 0)))");
    accepts_expr("pairs.sort((a, b) => a.score > b.score)",
                 "(call (. pairs sort) (=> (params a b) (> (. a score) (. b score))))");
    accepts_expr("fn(x: int) -> int { return x * 2 }",
                 "(lambda (params (x int)) (ret int) (block (return (* x 2))))");
    accepts_expr("fn() { print(\"hi\") }", "(lambda (params) (block (call print (str \"hi\"))))");
    accepts_expr("until(fn() { hp < 150 })", "(call until (lambda (params) (block (< hp 150))))");
    accepts_expr("x => y => x + y", "(=> (params x) (=> (params y) (+ x y)))");
    accepts_expr("a ?? (x => x)", "(?? a (=> (params x) x))");
    accepts_expr("(x => x)(1)", "(call (=> (params x) x) 1)");
    accepts_expr("fn(a, b = 2) { }", "(lambda (params a (b (= 2))) (block))");
}

TEST_CASE("struct literals (LANGUAGE.md 2.6)") {
    accepts_expr("Bullet { pos = p, vel = v }", "(struct Bullet (pos p) (vel v))");
    accepts_expr("Bullet {}", "(struct Bullet)");
    accepts_expr("Pair<int> { a = 1, b = 2 }", "(struct (Pair int) (a 1) (b 2))");
    accepts_expr("Bullet {\n    pos = p,\n    vel = v,\n}", "(struct Bullet (pos p) (vel v))");
    accepts_expr("Complex { re = re + o.re, im = im + o.im }",
                 "(struct Complex (re (+ re (. o re))) (im (+ im (. o im))))");
    rejects("Bullet { pos = p, pos = q }", "1:19: field 'pos' is initialized more than once\n", Root::Expr);
}

TEST_CASE("manual memory, coroutines") {
    accepts_expr("new(frame) List<vec2>()", "(new frame (List vec2))");
    accepts_expr("new(scene) Boss(pos, hp = 3)", "(new scene Boss pos (named hp 3))");
    accepts_expr("alloc Buffer(4096)", "(alloc Buffer 4096)");
    accepts_expr("free(buf)", "(free buf)");
    accepts_expr("start(run_phases())", "(start (call run_phases))");
    accepts_expr("await wait(0.5)", "(await (call wait (float 0.5)))");
    accepts_expr("await a + 1", "(+ (await a) 1)");
    rejects("new Boss()", "1:5: expected '(' after 'new' (e.g. 'new(frame) T()'), found 'Boss'\n", Root::Expr);
    rejects("alloc Buffer", "1:13: expected '(' with constructor arguments, found end of file\n", Root::Expr);
}

TEST_CASE("vectors and casts (LANGUAGE.md 4.2, 4.3)") {
    accepts_expr("vec3(p, 5.0)", "(call vec3 p (float 5))");
    accepts_expr("p * 2.0 + q.xy", "(+ (* p (float 2)) (. q xy))");
    accepts_expr("p.dot(q.xy)", "(call (. p dot) (. q xy))");
    accepts_expr("hp as float", "(as hp float)");
}

TEST_CASE("expression errors") {
    rejects("1 +", "1:4: expected expression, found end of file\n", Root::Expr);
    rejects("(1 + 2", "1:7: expected ')', found end of file\n", Root::Expr);
    rejects("a.", "1:3: expected member name after '.', found end of file\n", Root::Expr);
    rejects("1 2", "1:3: expected end of expression, found '2'\n", Root::Expr);
    rejects("x ? 1", "1:6: expected ':' in ternary expression, found end of file\n", Root::Expr);
    rejects("a $ b", "1:3: unexpected character\n", Root::Expr);
}

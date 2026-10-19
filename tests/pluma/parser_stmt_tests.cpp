// Statements (LANGUAGE.md 3, 5), newline rules (1.3) and error recovery.

#include <doctest.h>

#include "tests/pluma/parse_helpers.hpp"

using test::accepts;
using test::check_round_trip;
using test::rejects;
using test::Root;

TEST_CASE("assignment is a statement") {
    accepts("x = 1", "(= x 1)");
    accepts("x += 1", "(+= x 1)");
    accepts("x -= 1", "(-= x 1)");
    accepts("x *= 2", "(*= x 2)");
    accepts("x /= 2", "(/= x 2)");
    accepts("x %= 3", "(%= x 3)");
    accepts("x &= m", "(&= x m)");
    accepts("x |= m", "(|= x m)");
    accepts("x ^= m", "(^= x m)");
    accepts("x <<= 1", "(<<= x 1)");
    accepts("x >>= 1", "(>>= x 1)");
    accepts("pos.x = 0.0", "(= (. pos x) (float 0))");
    accepts("list[i] = v", "(= (index list i) v)");
    accepts("self.hp = hp", "(= (. self hp) hp)");
    rejects("f() = 1", "1:1: invalid assignment target\n");
    rejects("a?.b = 1", "1:1: invalid assignment target\n");
    rejects("x = = 1", "1:5: expected expression, found '='\n");
    rejects("var y = (x = 1)", "1:12: expected ')', found '='\n");
}

TEST_CASE("if / else if / else") {
    accepts("if hp <= 0 {\n    die()\n} else if hp < 20 {\n    flash()\n} else {\n    heal(1)\n}",
            "(if (branch (<= hp 0) (block (call die))) (branch (< hp 20) (block (call flash))) "
            "(else (block (call heal 1))))");
    accepts("if a { }", "(if (branch a (block)))");
    accepts("if a {\n} \n else {\n}", "(if (branch a (block)) (else (block)))");
    rejects("if x = 1 { }", "1:6: assignment is not allowed in a condition; did you mean '=='?\n");
    test::Parsed p = test::parse("if x = 1 { }");
    CHECK(p.rendered.find("fix-it: replace '=' with '=='") != std::string::npos);
    rejects("if a\nb()", "2:1: expected '{', found 'b'\n");
}

TEST_CASE("if-binding: nullable unwrap and patterns") {
    accepts("if target? t { t.hurt(1) }", "(if (branch target (unwrap t) (block (call (. t hurt) 1))))");
    accepts("if self.find(#boss)? b { }", "(if (branch (call (. self find) #boss) (unwrap b) (block)))");
    accepts("if phase is Phase.Spiral(arms) { print(arms) }",
            "(if (branch (is phase (Phase.Spiral arms)) (block (call print arms))))");
    accepts("if node is Boss b { b.rage() }", "(if (branch (is node Boss (bind b)) (block (call (. b rage)))))");
    accepts("if node is Boss { }", "(if (branch (is node Boss) (block)))");
    accepts("if load()? ok { }", "(if (branch (call load) (unwrap ok) (block)))");
    accepts("if x? { }", "(if (branch (try x) (block)))"); // `?` propagation in a condition
}

TEST_CASE("struct literals are not allowed directly in headers") {
    // `{ x = 1 }` is the if body here, not a struct literal.
    accepts("if p == Point { x = 1 }", "(if (branch (== p Point) (block (= x 1))))");
    accepts("if p == (Point { x = 1 }) { }", "(if (branch (== p (struct Point (x 1))) (block)))");
    accepts("for b in (Pool { size = 4 }) { }", "(for b (struct Pool (size 4)) (block))");
    accepts("while f(Point { x = 1 }) { }", "(while (call f (struct Point (x 1))) (block))");
    accepts("match (Point {}) { _ => go() }", "(match (struct Point) (arm _ (call go)))");
    // Printing re-inserts the parentheses the header needs.
    CHECK(test::parse("if p == (Point { x = 1 }) { }").printed == "if p == (Point { x = 1 }) {}\n");
}

TEST_CASE("match: literals, ranges, or-patterns, guards, enum payloads") {
    accepts("match phase {\n"
            "    Phase.Opening => start_opening()\n"
            "    Phase.Spiral(arms) if arms > 3 => {\n"
            "        hard_spiral(arms)\n"
            "    }\n"
            "    Phase.Spiral(arms) => spiral(arms)\n"
            "    Phase.Rage => rage()\n"
            "}",
            "(match phase (arm Phase.Opening (call start_opening)) "
            "(arm (Phase.Spiral arms) (if (> arms 3)) (block (call hard_spiral arms))) "
            "(arm (Phase.Spiral arms) (call spiral arms)) (arm Phase.Rage (call rage)))");
    accepts("match key {\n    0 | 1 => low()\n    2..=9 => mid()\n    _ => high()\n}",
            "(match key (arm (| (lit 0) (lit 1)) (call low)) (arm (range= 2 9) (call mid)) "
            "(arm _ (call high)))");
    accepts("match v { -1 => neg(); 0..10 => small(); \"s\" => str(); #n => name(); null => none() }",
            "(match v (arm (lit (neg 1)) (call neg)) (arm (range 0 10) (call small)) "
            "(arm (lit (str \"s\")) (call str)) (arm (lit #n) (call name)) (arm (lit null) (call none)))");
    accepts("match r {\n    Result.Ok(v) => x = v\n    Result.Err(_) => return\n}",
            "(match r (arm (Result.Ok v) (= x v)) (arm (Result.Err _) (return)))");
    accepts("match p { Pair(a, Inner.B(c)) => use(a, c) }",
            "(match p (arm (Pair a (Inner.B c)) (call use a c)))");
    accepts("match x { y if ready => go() }", "(match x (arm y (if ready) (call go)))");
    accepts("match x {}", "(match x)");
    rejects("match x {\n    1 go()\n}", "2:7: expected '=>' after the match pattern, found 'go'\n");
    rejects("match x {\n    + => a()\n    1 => b()\n}", "2:5: expected pattern, found '+'\n");
    rejects("match x { \"{y}\" => a() }", "1:11: interpolated strings are not allowed in patterns\n");
}

TEST_CASE("loops, labels and jumps") {
    accepts("for i in 0..8 { }", "(for i (.. 0 8) (block))");
    accepts("for i in 0..=8 step 2 { }", "(for i (..= 0 8 (step 2)) (block))");
    accepts("for e in enemies { }", "(for e enemies (block))");
    accepts("for i, e in enemies { }", "(for i e enemies (block))");
    accepts("while hp > 0 { }", "(while (> hp 0) (block))");
    accepts("loop { if done { break } }", "(loop (block (if (branch done (block (break))))))");
    accepts("outer: for y in 0..h { for x in 0..w { if hit { break outer } } }",
            "(for (label outer) y (.. 0 h) (block (for x (.. 0 w) (block (if (branch hit (block (break outer))))))))");
    accepts("scan: while true { continue scan }", "(while (label scan) true (block (continue scan)))");
    accepts("spin: loop { break spin }", "(loop (label spin) (block (break spin)))");
    accepts("loop { break; continue }", "(loop (block (break) (continue)))");
    rejects("outer: if x { }", "1:1: only 'for', 'while' and 'loop' can have a label\n");
    rejects("for i 0..8 { }", "1:7: expected 'in', found '0'\n");
    rejects("for in xs { }", "1:5: expected loop variable name, found 'in'\n");
}

TEST_CASE("foreach over particle pools (LANGUAGE.md 5)") {
    accepts("foreach p in sparks.pool { p.age += dt }",
            "(foreach p (. sparks pool) (block (+= (. p age) dt)))");
    accepts("@allow_slow\nforeach p in pool {\n    spawn(p.pos)\n}",
            "(foreach (attrs @allow_slow) p pool (block (call spawn (. p pos))))");
    rejects("@inline\nx = 1", "2:1: expected 'foreach' or a declaration after attributes, found 'x'\n");
}

TEST_CASE("return, yield, await, start, emit, defer, unsafe") {
    accepts("return", "(return)");
    accepts("return value", "(return value)");
    accepts("fn f() { return }", "(fn f (params) (block (return)))");
    accepts("yield", "(yield)");
    accepts("await wait(0.5)", "(await (call wait (float 0.5)))");
    accepts("start(run_phases())", "(start (call run_phases))");
    accepts("emit defeated", "(emit defeated)");
    accepts("emit hp_changed(hp)", "(emit hp_changed () hp)");
    accepts("defer close(file)", "(defer (call close file))");
    accepts("defer free(buf)", "(defer (free buf))");
    accepts("defer { a(); b() }", "(defer (block (call a) (call b)))");
    accepts("unsafe { x = 1 }", "(unsafe (block (= x 1)))");
    rejects("emit 3", "1:6: expected signal name after 'emit', found '3'\n");
    rejects("defer", "1:6: expected statement or block after 'defer', found end of file\n");
}

TEST_CASE("local declarations") {
    accepts("var hp = 100", "(var hp (init 100))");
    accepts("var dir: vec2", "(var dir (type vec2))");
    accepts("const MAX_HP = 400", "(const MAX_HP 400)");
    accepts("fn f() { var x = 1; const Y: int = 2 }",
            "(fn f (params) (block (var x (init 1)) (const Y (type int) 2)))");
    rejects("fn f() { fn g() { } }", "1:10: functions cannot be declared inside a block; use a lambda\n");
    rejects("fn f() { class C { } }", "1:10: 'class' declarations are only allowed at the top level\n");
    rejects("fn f() { var x = 1 { get { return 1 } } }", "1:10: accessors are only allowed on fields\n");
}

TEST_CASE("newline rules (LANGUAGE.md 1.3)") {
    // Method chains continue on lines starting with `.` or `?.`.
    accepts("g.path()\n    .move_to(a)\n    .cubic_to(c1, c2, b)\n    .stroke(width = 2.0)",
            "(call (. (call (. (call (. (call (. g path)) move_to) a) cubic_to) c1 c2 b) stroke) "
            "(named width (float 2)))");
    accepts("a\n    ?.b", "(?. a b)");
    // A line ending with a binary operator, `=`, `,` or `(` continues.
    accepts("x = 1 +\n    2", "(= x (+ 1 2))");
    accepts("var y =\n    3", "(var y (init 3))");
    accepts("f(1,\n  2\n)", "(call f 1 2)");
    accepts("ok = a &&\n    b", "(= ok (&& a b))");
    accepts("var z = c ?\n    1 :\n    2", "(var z (init (?: c 1 2)))");
    // Otherwise a newline ends the statement.
    test::Parsed p = test::parse("a\n-b");
    CHECK(p.errors.empty());
    CHECK(p.dump == "(script a (neg b))");
    p = test::parse("f\n(1)");
    CHECK(p.dump == "(script f 1)");
    // `;` lets statements share a line.
    p = test::parse("a(); b(); c()");
    CHECK(p.dump == "(script (call a) (call b) (call c))");
    rejects("a b", "1:3: expected newline or ';' after statement, found 'b'\n");
}

TEST_CASE("error recovery: many errors per file, valid code still parsed") {
    test::Parsed p = test::parse("x = 1 +\n"
                                 "y = (2\n"
                                 "z = 3\n"
                                 "foo(a b)\n"
                                 "w = )\n"
                                 "ok()\n");
    // `x = 1 +` continues onto line 2: `1 + y`, then `=` is unexpected.
    CHECK(p.errors == "2:3: expected newline or ';' after statement, found '='\n"
                      "4:7: expected ',' or ')' in argument list, found 'b'\n"
                      "5:5: expected expression, found ')'\n");
    CHECK(p.dump.find("(call ok)") != std::string::npos);

    p = test::parse("fn a() {\n"
                    "    x = \n"
                    "}\n"
                    "fn b() {\n"
                    "    if { }\n"
                    "    good()\n"
                    "}\n",
                    Root::Module);
    CHECK(p.errors == "3:1: expected expression, found '}'\n"
                      "5:8: expected expression, found '{'\n");
    CHECK(p.dump.find("(call good)") != std::string::npos);

    // A broken call spanning lines doesn't swallow the closing brace.
    p = test::parse("fn a() {\n    f(1,\n      2 3)\n    g()\n}\nfn b() {}\n", Root::Module);
    CHECK(p.errors == "3:9: expected ',' or ')' in argument list, found '3'\n");
    CHECK(p.dump.find("(call g)") != std::string::npos);
    CHECK(p.dump.find("(fn b") != std::string::npos);
}

TEST_CASE("lexer errors are reported once, and parsing goes on") {
    test::Parsed p = test::parse("x = 1 $ 2\ny = #\nz = 3\n");
    CHECK(p.errors == "1:7: unexpected character\n"
                      "2:5: expected a name after '#'\n");
    CHECK(p.dump.find("(= z 3)") != std::string::npos);
    rejects("x = 1 /* never closed", "1:7: unterminated block comment\n");
}

TEST_CASE("diagnostic rendering") {
    test::Parsed p = test::parse("var x = 1\nvar y = a?b:c\n");
    CHECK(p.rendered == "test.plm:2:10: error: ternary '?' needs spaces around it (a '?' right after an "
                        "expression propagates an error)\n"
                        " 2 | var y = a?b:c\n"
                        "   |          ^\n"
                        "   = fix-it: replace 'a?b:c' with 'a ? b : c'\n");
    p = test::parse("foo(bar baz)");
    CHECK(p.rendered == "test.plm:1:9: error: expected ',' or ')' in argument list, found 'baz'\n"
                        " 1 | foo(bar baz)\n"
                        "   |         ^~~\n");
}

TEST_CASE("round trip: statements") {
    check_round_trip("x = a ? b : c\n"
                     "if a { b() } else if c { d() } else { e() }\n"
                     "outer: for i, e in xs { if e { continue outer } }\n"
                     "match k { 0 | 1 => a(); 2..=9 if z => { b() }; _ => c() }\n"
                     "@allow_slow\nforeach p in pool { p.pos += p.vel * dt }\n"
                     "defer { close(f) }\n"
                     "var s = \"a{b}c{{\"\n"
                     "var m = {\"k\": [1, 2]}\n"
                     "({\"k\": 1}).size()\n"
                     "var f = fn(x: int) -> int {\n    return x * 2\n}\n"
                     "var g = xs.map(x => \"{fn() { return x }()}\")\n");
}

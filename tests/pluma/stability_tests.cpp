// Milestone 8.7 regression tests: language rules from LANGUAGE.md that had no
// test, and the bugs found while writing them.

#include <doctest.h>

#include "pluma/format.hpp"
#include "pluma/session.hpp"
#include "tests/pluma/run_helpers.hpp"

using test::prints;
using test::rejects;

TEST_CASE("stability: a NaN prints as nan whatever its sign bit") {
    prints("var n = 0.0 / 0.0\nprint(n)\nprint(-n)\nprint(\"{n}\")\nprint(5.0 % 0.0)\nprint([n])\nprint(vec2(n, 1.0))\n",
           "nan\nnan\nnan\nnan\n[nan]\nvec2(nan, 1.0)\n");
    prints("var n = 0.0 / 0.0\nvar d: f64 = 0.0 / 0.0\nprint(n.to_fixed(2))\nprint((-n).to_fixed(1))\nprint(d.to_fixed(1))\n"
           "var sb = StrBuilder()\nsb.append(-n)\nprint(sb.to_str())\nprint((1.0 / 0.0).to_fixed(1))\n",
           "nan\nnan\nnan\nnan\ninf\n");
}

TEST_CASE("stability: enum discriminants must fit the backing type") {
    rejects("enum L: u8 { A = 255, B }\n",
            "1:23: the discriminant 256 of variant 'B' does not fit in 'u8'; use a wider backing type\n");
    rejects("enum L { A = 2147483647, B }\n",
            "1:26: the discriminant 2147483648 of variant 'B' does not fit in 'int'; use a wider backing type\n");
    prints("enum L: u8 { A = 254, B }\nprint(L.B as int)\n", "255\n");
    prints("enum L: i8 { A = -128, B }\nprint(L.B as int)\n", "-127\n");
    prints("enum L: u64 { A = 5, B }\nprint(L.B as int)\n", "6\n");
}

TEST_CASE("stability: a struct cannot contain itself by value") {
    rejects("struct A { var b: B }\nstruct B { var a: A }\n",
            "1:16: struct 'A' contains itself through field 'b' and would have infinite size; use an optional ('A?'), a class or a List\n"
            "2:16: struct 'B' contains itself through field 'a' and would have infinite size; use an optional ('B?'), a class or a List\n");
    rejects("struct A { var xs: [A; 2] }\n",
            "1:16: struct 'A' contains itself through field 'xs' and would have infinite size; use an optional ('A?'), a class or a List\n");
    rejects("struct Bx<T> { var v: T }\nstruct R { var b: Bx<R> }\n",
            "2:16: struct 'R' contains itself through field 'b' and would have infinite size; use an optional ('R?'), a class or a List\n");
    // References, lists and optionals end the chain.
    prints("struct A { var x: A? }\nstruct B { var xs: List<B> }\nclass C { var c: C = null }\nstruct Bx<T> { var v: List<T> }\n"
           "struct R { var b: Bx<R> }\nprint(A {})\nprint(R {})\n",
           "A { x = null }\nR { b = Bx { v = null } }\n");
}

TEST_CASE("stability: top-level code cannot read a global before its declaration") {
    rejects("var a = b + 1\nvar b = 2\n", "1:9: 'b' is used before its declaration\n");
    rejects("print(c)\nvar c = 1\n", "1:7: 'c' is used before its declaration\n");
    // Functions and lambdas run later, so they may name any global.
    prints("fn f() -> int { return g }\nvar g = 5\nprint(f())\n", "5\n");
    prints("var h = fn() -> int { return k }\nvar k = 6\nprint(h())\n", "6\n");
    // The declaration itself and earlier ones are fine.
    prints("var a = 1\nvar b = a + 1\nprint(b)\n", "2\n");
}

TEST_CASE("stability: private members are not visible in subclasses, by name or through super") {
    rejects("class A { var _p = 1 }\nclass B extends A { fn f() -> int { return _p } }\n",
            "2:44: '_p' is private to 'A'\n");
    rejects("class A { fn _h() -> int { return 1 } }\nclass B extends A {\n    fn f() -> int { return _h() }\n"
            "    fn g() -> int { return super._h() }\n}\n",
            "3:28: '_h' is private to 'A'\n4:34: '_h' is private to 'A'\n");
    rejects("class A { static var _n = 1\n  static fn _s() -> int { return 2 } }\n"
            "class B extends A { fn f() -> int { return _n + _s() } }\n",
            "3:44: '_n' is private to 'A'\n3:49: '_s' is private to 'A'\n");
    // The declaring type, and another instance of it, still see them.
    prints("class A { var _p = 1\n  fn _h() -> int { return _p + 1 }\n  fn same(o: A) -> int { return o._p + _h() } }\n"
           "print(A().same(A()))\n",
           "3\n");
}

TEST_CASE("stability: parse_float rounds out-of-range numbers like IEEE 754") {
    prints("print(parse_float(\"1e39\"))\nprint(parse_float(\"-1e39\"))\nprint(parse_float(\"1e-50\"))\n"
           "print(parse_float(\"-1e-50\"))\nprint(parse_float(\"3.4028235e38\"))\nprint(parse_float(\"3.4028236e38\"))\n"
           "print(parse_float(\"1e999\"))\nprint(parse_float(\"1e-999\"))\nprint(parse_float(\"-1e999\"))\n"
           "print(parse_float(\"1e999x\"))\nprint(parse_float(\"0x1p3\"))\nprint(parse_float(\" 1\"))\n"
           "print(parse_float(\"Infinity\"))\nprint(parse_float(\"-NaN\"))\nprint(parse_float(\"0.5\"))\n",
           "inf\n-inf\n0.0\n-0.0\n3.4028235e+38\ninf\ninf\n0.0\n-inf\nnull\nnull\nnull\ninf\nnan\n0.5\n");
}

TEST_CASE("stability: REPL inputs roll back whole, redefinitions are errors, panics keep earlier effects") {
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    pluma::Session s(options);
    CHECK(s.run_script("1", "var a = 1", true).ok);
    // The second line fails to compile: neither of the first two lines' names exist afterwards.
    pluma::SessionResult bad = s.run_script("2", "var b = 2\nvar c = missing", true);
    CHECK_FALSE(bad.compiled);
    CHECK(s.run_script("3", "var b = 30\nb + a", true).ok);
    // A second declaration of the same global is an error, also across inputs.
    pluma::SessionResult dup = s.run_script("4", "var a = 5", true);
    CHECK_FALSE(dup.compiled);
    CHECK(s.run_script("5", "a", true).ok);
    // A panic stops the input but keeps what ran before it; the session stays usable.
    pluma::SessionResult boom = s.run_script("6", "a = 7\nvar z = 1 / (a - 7)\nprint(\"unreached\")", true);
    CHECK(boom.compiled);
    CHECK_FALSE(boom.ok);
    CHECK(s.run_script("7", "a", true).ok);
    CHECK(out == "31\n1\n7\n");
}

TEST_CASE("stability: a float literal that rounds to infinity is an error") {
    rejects("print(1e39)\n", "1:7: float literal is out of range for 'float'\n");
    rejects("print(3.5e38f32)\n", "1:7: float literal is out of range for 'float'\n");
    prints("print(3.4e38)\nvar d: f64 = 1e300\nprint(d)\nprint(1e-50)\nprint(3.5e38f64 as float)\n", "3.4e+38\n1e+300\n0.0\ninf\n");
}

TEST_CASE("stability: jumps inside defer say so") {
    rejects("for i in 0..3 { defer { break } }\n",
            "1:25: 'break' is not allowed inside 'defer' (it cannot leave the deferred code)\n");
    rejects("for i in 0..3 { defer { continue } }\n",
            "1:25: 'continue' is not allowed inside 'defer' (it cannot leave the deferred code)\n");
    rejects("break\n", "1:1: 'break' outside of a loop\n");
    // A loop of its own inside the deferred code is fine.
    prints("fn f() {\n    defer {\n        for i in 0..3 {\n            if i == 1 { break }\n            print(i)\n        }\n    }\n}\nf()\n",
           "0\n");
}

TEST_CASE("stability: a byte order mark and CRLF line endings are accepted") {
    prints("\xEF\xBB\xBF" "print(1)\nprint(2)\n", "1\n2\n");
    prints("var x = 1\r\nvar t = x +\r\n  2\r\nprint(t)\r\nvar m = \"\"\"\r\n  hi\r\n  there\r\n  \"\"\"\r\nprint(m.len())\r\n",
           "3\n8\n");
    // Diagnostics keep their line and column with CRLF endings.
    rejects("var a = 1\r\nvar b = \"x\" + 1\r\n", "2:9: operator '+' is not defined for 'str' and 'int'\n");
    pluma::FormatOptions options;
    options.script_mode = true;
    options.check_style = false;
    pluma::FormatResult f = pluma::format_source("test.plm", "\xEF\xBB\xBF" "print( 1 )\n", options);
    CHECK(f.ok);
    CHECK(f.formatted == "print(1)\n");
}

TEST_CASE("stability: lexer diagnostics name the problem once") {
    rejects("var é = 1\n",
            "1:5: unexpected non-ASCII character (identifiers are ASCII; other text belongs in strings and comments)\n");
    rejects("print(0b102)\n", "1:7: invalid digit in a binary literal (only 0 and 1)\n");
    prints("print(\"é ☃ 😀\".char_count())\nprint(0b1_0u8)\n", "5\n2\n");
    // The end of an interpolation is its '}', not the end of the file.
    rejects("print(\"{1 +}\")\n", "1:12: expected expression, found '}'\n");
}

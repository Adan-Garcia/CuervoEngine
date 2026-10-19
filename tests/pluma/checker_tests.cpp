// Type checker: accepted programs (inference) and rejected ones (exact
// diagnostics with positions).

#include <doctest.h>

#include "tests/pluma/run_helpers.hpp"

using test::prints;
using test::rejects;

TEST_CASE("checker: local inference and literal typing") {
    prints("var a = 1\nvar b = 2.5\nvar c = a as float + b\nprint(c)", "3.5\n");
    prints("var x: u8 = 200\nvar y = x + 55\nprint(y)", "255\n");
    prints("var f: f64 = 1\nprint(f / 3)", "0.3333333333333333\n");
    prints("var n: int? = null\nprint(n ?? 3)", "3\n");
    rejects("var a: int = \"hello\"", "1:14: mismatched types: expected 'int', found 'str'\n");
    rejects("var b = 1\nvar c = b + 2.0",
            "2:9: mismatched operand types 'int' and 'float' for '+'; convert one with 'as'\n");
    rejects("var x: float = 1\nvar i: int = x",
            "2:14: mismatched types: expected 'int', found 'float'; convert with 'as int'\n");
    rejects("var big: u8 = 300", "1:15: integer literal 300 does not fit in 'u8'\n");
    rejects("var neg: u8 = -1", "1:15: integer literal -1 does not fit in 'u8'\n");
    rejects("var huge = 3000000000", "1:12: integer literal 3000000000 does not fit in 'int'\n");
    rejects("var n: int = null", "1:14: 'int' cannot be null; use 'int?' for an optional value\n");
    rejects("var e = []", "1:9: cannot infer the element type of an empty list; write 'List<T>()' or annotate the "
                          "variable\n");
    rejects("var z = null", "1:9: cannot infer a type from 'null'; add a type annotation\n");
    rejects("var v\n", "1:5: 'v' needs a type or an initializer\n");
    rejects("var m: int? = 1\nprint(m + 1)", "2:7: operator '+' is not defined for 'int?' and 'int'\n");
}

TEST_CASE("checker: names and scopes") {
    rejects("print(nope)", "1:7: unknown name 'nope'\n");
    rejects("nope()", "1:1: unknown function 'nope'\n");
    rejects("var q: Nope = 1", "1:8: unknown type 'Nope'\n");
    rejects("{ var inner = 1 }\nprint(inner)", "2:7: unknown name 'inner'\n");
    rejects("fn f() { }\nfn f() { }", "2:4: duplicate name 'f': names are global (DESIGN.md 5.11)\n");
    rejects("class int { }", "1:7: 'int' is a built-in type name\n");
    rejects("const LIMIT = 3\nLIMIT = 4", "2:1: cannot assign to constant 'LIMIT'\n");
    rejects("var x = 1\nconst C = x + 1", "2:11: the value of constant 'C' must be a constant expression\n");
    prints("const A = 2\nconst B = A * 3 + 1\nprint(B)", "7\n");
    rejects("const O = 2147483647 + 1", "1:11: constant expression overflows 'int'\n");
}

TEST_CASE("checker: functions and calls") {
    rejects("fn f(x: int) -> int { if x > 0 { return 1 } }",
            "1:4: 'f' may reach the end without returning a value\n");
    rejects("fn f(x: int) { }\nf(\"no\")", "2:3: mismatched types: expected 'int', found 'str'\n");
    rejects("fn f(x: int) { }\nf(1, 2)", "2:6: too many arguments to 'f': expected 1\n");
    rejects("fn f(x: int) { }\nf(y = 3)", "2:1: missing argument 'x' in call to 'f'\n2:3: 'f' has no parameter named 'y'\n");
    rejects("fn f(x: int) { x = 5 }", "1:16: cannot assign to parameter 'x' (parameters are immutable)\n");
    rejects("fn f() { return 1 }", "1:17: this function returns nothing; remove the value or declare a return type\n");
    rejects("fn f() -> int { return }", "1:17: 'return' needs a value of type 'int'\n");
    rejects("return 1", "1:1: 'return' outside of a function\n");
    rejects("break", "1:1: 'break' outside of a loop\n");
    rejects("for i in 0..3 { break nope }", "1:23: no enclosing loop is labeled 'nope'\n");
    rejects("var f = x => x", "1:9: cannot infer the type of parameter 'x'; annotate it\n");
    rejects("fn apply(f: fn(int) -> int) { }\napply((a, b) => a)",
            "2:7: expected a function with 1 parameter, but this lambda takes 2\n");
    prints("fn ring(count: int, speed = 150.0, spin = 0.0) -> str { return \"{count} {speed} {spin}\" }\n"
           "print(ring(36))\nprint(ring(36, spin = 12.0))\nprint(ring(speed = 2.0, count = 1))",
           "36 150.0 0.0\n36 150.0 12.0\n1 2.0 0.0\n");
    rejects("fn id<T>(x: T) -> T { return x }\nvar f = id", "2:9: generic function 'id' can only be called, not used as a value\n");
    rejects("fn make<T>() -> List<T> { return List<T>() }\nvar l = make()",
            "2:9: cannot infer type argument 'T' of 'make'; write it explicitly\n");
    rejects("fn m<T: Ord>(a: T) { }\nclass C { }\nm(C())", "3:1: type 'C' does not implement 'Ord' (required by 'T')\n");
    rejects("fn g<T>(a: T, b: T) -> bool { return a < b }", "1:38: operator '<' is not defined for 'T'\n");
}

TEST_CASE("checker: structs and classes") {
    rejects("struct S { var x = 1 }\nfn h(s: S) { s.x = 2 }",
            "2:14: cannot assign to a field of parameter 's' (parameters are immutable; copy it into a 'var')\n");
    rejects("struct S { var x = 1 }\nvar s = S { y = 2 }", "2:13: struct 'S' has no field 'y'\n");
    rejects("struct S { var x = 1 }\nvar s = S()", "2:9: structs are built with a literal: 'S { field = value }'\n");
    rejects("class C { var x = 1 }\nvar c = C { x = 2 }", "2:9: 'C' is a class; construct it with 'C(...)'\n");
    rejects("class C { var _secret = 1 }\nprint(C()._secret)", "2:11: '_secret' is private to 'C'\n");
    rejects("class C { fn init(a: int) { } }\nvar c = C()", "2:9: missing argument 'a' in call to 'init'\n");
    rejects("class C { }\nvar c = C(1)", "2:9: 'C' takes no arguments (it declares no 'init')\n");
    rejects("class C { var hp = 1 }\nprint(C().mp)", "2:11: 'C' has no field 'mp'\n");
    rejects("class C { }\nC().go()", "2:5: 'C' has no method 'go'\n");
    rejects("class B { final fn m() { } }\nclass D extends B { fn m() { } }",
            "2:24: cannot override final method 'B.m'\n");
    rejects("class B { fn m(x: int) { } }\nclass D extends B { fn m(x: str) { } }",
            "2:24: 'D.m' overrides 'B.m' with a different signature\n");
    rejects("class A extends B { }\nclass B extends A { }", "1:17: 'A' inherits from itself\n");
    rejects("struct Pair<T> { var a: T; var b: T }\nvar p = Pair<int> { a = 1 }",
            "2:9: field 'b' of generic type 'T' must be given a value\n");
    rejects("struct S { }\nclass C extends S { }", "2:17: 'C' can only extend a class, not 'S'\n");
    rejects("class C { var hp = 1\n static fn s() { print(hp) } }",
            "2:24: instance field 'hp' cannot be used here (no 'self')\n");
    rejects("class C { }\nvar x: C = 1", "2:12: mismatched types: expected 'C', found 'int'\n");
    rejects("class A { }\nclass B { }\nvar x: A = B()", "3:12: mismatched types: expected 'A', found 'B'\n");
    prints("class A { }\nclass B extends A { }\nvar x: A = B()\nprint(x is B)", "true\n");
}

TEST_CASE("checker: traits") {
    rejects("trait T { fn need() }\nclass C: T { }", "2:7: 'C' does not implement 'need', required by trait 'T'\n");
    rejects("trait T { fn need() -> int }\nclass C: T { fn need() -> str { return \"\" } }",
            "2:17: 'C.need' does not match the signature required by trait 'T'\n");
    rejects("struct P: Add { var x = 1 }", "1:8: 'P' lists trait 'Add' but has no method 'add'\n");
    rejects("struct P { var x = 1 }\nvar p = P {} + P {}", "2:9: operator '+' is not defined for 'P'\n");
    rejects("struct P: Eq { var x = 1\n fn eq(o: P) -> int { return 0 } }", "2:5: 'P.eq' must return bool for trait 'Eq'\n");
}

TEST_CASE("checker: enums and match exhaustiveness") {
    const char* phase = "enum Phase { Opening, Spiral(arms: int), Rage }\n";
    rejects(std::string(phase) + "fn g(p: Phase) {\n    match p {\n        Phase.Opening => print(\"o\")\n    }\n}",
            "3:5: 'match' is not exhaustive: missing 'Phase.Spiral', 'Phase.Rage'\n");
    // A guarded arm doesn't count; a refutable payload pattern doesn't either.
    rejects(std::string(phase) +
                "fn g(p: Phase) {\n    match p {\n        Phase.Opening | Phase.Rage => print(\"x\")\n"
                "        Phase.Spiral(arms) if arms > 1 => print(\"y\")\n        Phase.Spiral(2) => print(\"z\")\n    }\n}",
            "3:5: 'match' is not exhaustive: missing 'Phase.Spiral'\n");
    prints(std::string(phase) + "match Phase.Rage {\n    Phase.Spiral(_) => print(\"s\")\n    _ => print(\"other\")\n}",
           "other\n");
    rejects("match 3 {\n    1 => print(\"one\")\n}", "1:1: 'match' on 'int' must be exhaustive: add a '_' arm\n");
    rejects("match true {\n    true => print(\"t\")\n}", "1:1: 'match' is not exhaustive: missing 'false'\n");
    rejects("var o: bool? = null\nmatch o {\n    true => print(\"t\")\n    false => print(\"f\")\n}",
            "2:1: 'match' is not exhaustive: missing 'null'\n");
    rejects(std::string(phase) + "var p = Phase.Spiral",
            "2:9: variant 'Phase.Spiral' needs its payload: 'Phase.Spiral(...)'\n");
    rejects(std::string(phase) + "var p = Phase.Spiral(\"x\")", "2:22: mismatched types: expected 'int', found 'str'\n");
    rejects(std::string(phase) + "var p = Phase.Nope", "2:15: enum 'Phase' has no variant 'Nope'\n");
    rejects(std::string(phase) + "match Phase.Rage {\n    Phase.Spiral(a, b) => print(\"s\")\n    _ => print(\"o\")\n}",
            "3:5: 'Phase.Spiral' has 1 payload field, but the pattern has 2\n");
    rejects(std::string(phase) + "var p = Phase.Rage\nvar ok = p is Phase.Spiral(n)",
            "3:10: pattern bindings are only allowed in an 'if' condition\n");
    rejects("enum E { A(x: int) = 3 }", "1:22: only enums without payloads can have explicit discriminants\n");
}

TEST_CASE("checker: null safety") {
    rejects("var n: int? = 3\nvar m: int = n", "2:14: mismatched types: expected 'int', found 'int?'\n");
    rejects("var x = 3\nif x? v { }", "2:4: '?' unwraps a nullable value, but 'int' is never null\n");
    rejects("var x = 3\nprint(x ?? 4)", "2:7: the left side of '?\?' has type 'int', which is never null\n");
    rejects("var x = 3\nprint(x == null)", "2:7: a value of type 'int' is never null\n");
    rejects("struct S { var v = 1 }\nvar s: S? = null\nprint(s.v)",
            "3:7: value of type 'S?' may be null; unwrap it with 'if x? v { }' or '?\?'\n");
}

TEST_CASE("checker: unsupported features are reported, not crashed on") {
    rejects("var m = {}", "1:9: cannot infer the types of an empty map; write 'Map<K, V>()' or annotate the variable\n");
    rejects("var b = alloc C()", "1:15: unknown type 'C'\n");
}

TEST_CASE("checker: several independent errors are all reported") {
    rejects("var a: int = \"x\"\nprint(b)\nfn f() -> int { }",
            "1:14: mismatched types: expected 'int', found 'str'\n2:7: unknown name 'b'\n"
            "3:4: 'f' may reach the end without returning a value\n");
}

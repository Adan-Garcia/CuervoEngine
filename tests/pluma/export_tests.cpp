// `@export` (LANGUAGE.md 2.10, 9.13) and the scheduler's freeze hook used by
// the engine's pause (pluma/vm.hpp tick_coroutines).

#include <doctest.h>

#include "pluma/session.hpp"
#include "pluma/value.hpp"
#include "run_helpers.hpp"

using test::prints;
using test::rejects;

TEST_CASE("@export: accepted field types and hints") {
    prints("struct Rgb {\n var r = 1.0\n var g = 0.5\n}\nenum Mode { A, B }\n"
           "class Thing {\n"
           " @export var hp = 3\n"
           " @export(range = 0..10, step = 0.5) var speed = 2.0\n"
           " @export(hint = \"display name\") var label = \"x\"\n"
           " @export var tag = #enemy\n"
           " @export var at = vec2(1.0, 2.0)\n"
           " @export var color = Rgb {}\n"
           " @export var mode = Mode.B\n"
           " @export var path = List<vec2>()\n"
           " @export var flag = false\n"
           " @export(range = -1..1) var small: i8 = 0\n"
           " var hidden = 0\n"
           "}\nprint(Thing().hp)",
           "3\n");
}

TEST_CASE("@export: misuse is reported") {
    rejects("class A {\n @export var other: A\n}",
            "2:14: field 'other' of type 'A' can't be exported: class references are not saved; save child nodes "
            "instead\n");
    rejects("enum P { X(n: int), Y }\nclass A {\n @export var p = P.Y\n}",
            "3:14: field 'p' of type 'P' can't be exported: enum 'P' has payloads\n");
    rejects("class A {\n @export var f = fn() {}\n}",
            "2:14: field 'f' of type 'fn()' can't be exported: 'fn()' values can't be saved\n");
    rejects("class B {}\nstruct S {\n var b: B\n}\nclass A {\n @export var s = S {}\n}",
            "6:14: field 's' of type 'S' can't be exported: field 'S.b': class references are not saved; save child "
            "nodes instead\n");
    rejects("class A {\n @export static var n = 1\n}",
            "2:2: '@export' applies only to instance fields, not to static variables\n");
    rejects("class A {\n @export fn f() {}\n}", "2:2: '@export' applies only to instance fields, not to functions\n");
    rejects("class A {\n @export signal s\n}", "2:2: '@export' applies only to instance fields, not to signals\n");
    rejects("@export var g = 1", "1:1: '@export' applies only to instance fields, not to top-level declarations\n");
    rejects("@export class C {}", "1:1: '@export' applies only to instance fields, not to type declarations\n");
    rejects("class A {\n @export(speed = 1) var x = 1\n}",
            "2:10: unknown '@export' argument 'speed' (expected range, step or hint)\n");
    rejects("class A {\n @export(range = 5..1) var x = 1\n}", "2:10: 'range' is empty (hi < lo)\n");
    rejects("class A {\n @export(step = 0) var x = 1.0\n}", "2:10: 'step' must be a positive number literal\n");
    rejects("class A {\n @export(range = 0..1) var x = \"s\"\n}",
            "2:28: 'range' and 'step' apply only to number fields; 'x' is 'str'\n");
}

TEST_CASE("@export: the runtime type records which fields are exported") {
    pluma::Session session;
    pluma::SessionResult r = session.run_module("m.plm", "class Base {\n @export var a = 1\n var b = 2\n}\n"
                                                         "class Derived extends Base {\n var c = 3\n @export var d = 4.0\n}\n");
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    const pluma::RecordRT* sub = session.vm().find_class("Derived");
    REQUIRE(sub != nullptr);
    REQUIRE(sub->field_exported.size() == 4);
    CHECK(sub->field_exported == std::vector<bool>{true, false, false, true});
    CHECK(sub->field_types[3] == "float");
}

TEST_CASE("tick_coroutines: frozen owners don't run and their waits don't elapse") {
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    pluma::Session session(options);
    pluma::SessionResult r = session.run_script("t.plm", "class Owner {\n var name = \"\"\n"
                                                         " fn run() {\n  await wait_ticks(2)\n  print(name)\n }\n fn go() {\n  start(run())\n }\n}\n"
                                                         "var a = Owner()\na.name = \"a\"\nvar b = Owner()\n"
                                                         "b.name = \"b\"\na.go()\nb.go()\n");
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    REQUIRE(r.ok);
    pluma::Value frozen_owner = session.global("b");
    REQUIRE(frozen_owner.is_obj());
    auto freeze_b = [&](const pluma::Object* owner) { return owner == frozen_owner.o; };
    // `a` wakes at tick 2; `b` is frozen for four ticks...
    for (int i = 0; i < 4; ++i) {
        REQUIRE(session.vm().tick_coroutines(freeze_b).ok);
        out += "|";
    }
    CHECK(out == "|a\n|||");
    // ...and then still needs its full two ticks.
    REQUIRE(session.vm().tick_coroutines().ok);
    out += "|";
    REQUIRE(session.vm().tick_coroutines().ok);
    CHECK(out == "|a\n||||b\n");
}

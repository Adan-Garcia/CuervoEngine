// Coroutines and signals (DESIGN.md 5.9, LANGUAGE.md 9.10).
//
// `ticks(src, n)` runs a script, then the scheduler for n ticks, writing "|"
// after each tick, so expected output shows on which tick each line printed.

#include <doctest.h>

#include "pluma/session.hpp"
#include "pluma/value.hpp"
#include "run_helpers.hpp"

using test::rejects;

namespace {

std::string ticks(std::string_view src, int n, std::string* panic = nullptr) {
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    options.tick_rate = 10.0f; // wait(0.3) = 3 ticks
    pluma::Session session(options);
    pluma::SessionResult r = session.run_script("test.plm", std::string(src));
    INFO("source:\n" << src);
    REQUIRE_MESSAGE(r.compiled, r.rendered);
    if (r.panic && panic != nullptr) *panic = r.panic->message;
    for (int i = 0; i < n; ++i) {
        pluma::RunResult t = session.vm().tick_coroutines();
        if (t.panic && panic != nullptr) *panic = t.panic->message;
        out += "|";
    }
    return out;
}

} // namespace

TEST_CASE("coroutines: start runs to the first await, then once per tick") {
    CHECK(ticks("fn run() {\n print(\"a\")\n yield\n print(\"b\")\n yield\n print(\"c\")\n}\n"
                "start(run())\nprint(\"started\")",
                4) == "a\nstarted\nb\n|c\n|||");
}

TEST_CASE("coroutines: wait, wait_ticks and until count ticks") {
    CHECK(ticks("fn run() {\n await wait(0.3)\n print(\"w\")\n await wait_ticks(2)\n print(\"t\")\n}\nstart(run())",
                6) == "||w\n||t\n||");
    CHECK(ticks("var go = false\nfn run() {\n await until(fn() { go })\n print(\"go\")\n}\n"
                "fn flip() {\n await wait_ticks(2)\n go = true\n}\nstart(run())\nstart(flip())",
                4) == "||go\n||"); // run is checked before flip sets `go` on tick 2
    // wait() never waits less than one tick; wait_ticks(0) does not suspend.
    CHECK(ticks("fn run() {\n await wait(0.0)\n print(\"a\")\n await wait_ticks(0)\n print(\"b\")\n}\nstart(run())",
                2) == "a\nb\n||");
}

TEST_CASE("coroutines: awaiting a coroutine call returns its value") {
    CHECK(ticks("fn twice(x: int) -> int {\n yield\n return x * 2\n}\n"
                "fn run() {\n var v = await twice(21)\n print(v)\n}\nstart(run())",
                2) == "42\n||");
}

TEST_CASE("coroutines: tasks can be awaited, cancelled and queried") {
    CHECK(ticks("fn slow() {\n await wait_ticks(3)\n print(\"slow done\")\n}\n"
                "fn run() {\n var t = start(slow())\n await t\n print(\"after\")\n}\nstart(run())",
                4) == "||slow done\n|after\n|"); // run was checked before slow finished on tick 3
    CHECK(ticks("fn loop_forever() {\n loop {\n print(\"x\")\n yield\n }\n}\n"
                "var t = start(loop_forever())\nfn stop() {\n yield\n t.cancel()\n print(t.is_done())\n}\n"
                "start(stop())",
                3) == "x\nx\ntrue\n|||");
}

TEST_CASE("coroutines: methods, locals and loops survive suspension") {
    CHECK(ticks("class Ship {\n var hp = 3\n fn run() {\n  for i in 0..3 {\n   hp -= 1\n   print(\"{i}:{hp}\")\n"
                "   yield\n  }\n }\n}\nvar s = Ship()\nstart(s.run())",
                3) == "0:2\n1:1\n|2:0\n||");
}

TEST_CASE("coroutines: panics stop only the panicking coroutine") {
    std::string panic;
    std::string out = ticks("fn bad() {\n yield\n var l = List<int>()\n print(l[0])\n}\n"
                            "fn good() {\n yield\n yield\n print(\"still here\")\n}\nstart(bad())\nstart(good())",
                            3, &panic);
    CHECK(out == "|still here\n||");
    CHECK(panic == "index 0 is out of bounds for a list of length 0");
}

TEST_CASE("coroutines: checker rules") {
    rejects("fn run() { yield }\nfn f() { run() }",
            "2:10: 'run' is a coroutine: run it with 'start(run(...))', or 'await run(...)' inside another coroutine\n");
    rejects("var x = 1\nawait wait(1.0)", "2:1: 'await' can only be used inside a function (which makes it a coroutine)\n");
    rejects("fn f() { wait(1.0) }", "1:10: 'wait' can only be awaited, inside a coroutine: 'await wait(...)'\n");
    rejects("fn f() { start(print(1)) }",
            "1:16: 'start' needs a call to a coroutine (a function that uses 'await' or 'yield')\n");
    rejects("fn f() {\n var g = fn() { yield }\n}",
            "2:10: 'await' and 'yield' cannot be used inside a lambda yet; move the code into a method and 'start' "
            "it\n");
    rejects("fn f() { await 3 }",
            "1:16: cannot await a value of type 'int'; await 'wait(seconds)', 'wait_ticks(n)', 'until(condition)', "
            "a coroutine call, a Task or a signal\n");
    rejects("class A { fn run() {} }\nclass B extends A { fn run() { yield } }",
            "2:24: 'B.run' is a coroutine but overrides 'A.run', which is not\n");
}

TEST_CASE("signals: emit, connect, disconnect") {
    test::prints("class Boss {\n signal hurt(amount: int)\n signal died\n var hp = 3\n"
                 " fn hit(n: int) {\n  hp -= n\n  emit hurt(n)\n  if hp <= 0 { emit died }\n }\n}\n"
                 "var b = Boss()\nvar total = 0\nb.hurt.connect(fn(n: int) { total += n })\n"
                 "var h = fn() { print(\"dead\") }\nb.died.connect(h)\nb.hit(1)\nb.hit(2)\nprint(total)\n"
                 "b.died.disconnect(h)\nb.hit(5)\nprint(total)",
                 "dead\n3\n8\n");
    rejects("class A { signal s }\nvar a = A()\na.s = a.s", "3:1: cannot assign to signal 's'\n");
    rejects("class A {\n signal s(x: int)\n fn f() { emit s(\"no\") }\n}",
            "3:18: mismatched types: expected 'int', found 'str'\n");
    rejects("class A {\n fn f() { emit nope }\n}", "2:16: unknown signal 'nope' in 'A'\n");
}

TEST_CASE("signals: awaiting a signal resumes on the next tick with its value") {
    CHECK(ticks("class Door {\n signal opened(by: str)\n fn open() { emit opened(\"key\") }\n}\nvar d = Door()\n"
                "fn wait_door() {\n var who = await d.opened\n print(\"opened by {who}\")\n}\n"
                "fn opener() {\n await wait_ticks(2)\n d.open()\n}\nstart(wait_door())\nstart(opener())",
                4) == "||opened by key\n||");
}

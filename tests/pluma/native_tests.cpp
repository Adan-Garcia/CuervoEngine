// Native binding layer (pluma/bindings.hpp) and multi-file projects.

#include <doctest.h>

#include "pluma/bindings.hpp"
#include "pluma/session.hpp"
#include "run_helpers.hpp"

namespace {

struct Counter {
    int value = 0;
};
int g_finalized = 0;

pluma::Bindings test_bindings() {
    pluma::Bindings b;
    b.add_prelude("prelude.plm", R"(
@native fn twice(x: float) -> float {}
class Counter {
    @native fn init(from = 0) {}
    @native fn bump(by = 1) -> int {}
    @native static fn describe(n: name, v: vec2) -> str {}
}
class Time { static var tick = 0 }
)");
    b.add_native("twice", [](pluma::NativeCall& c) { return pluma::Value::f32(pluma::native::f32(c, 0) * 2.0f); });
    b.add_native("Counter.init", [](pluma::NativeCall& c) {
        auto* counter = new Counter{static_cast<int>(pluma::native::integer(c, 1))};
        pluma::native::self(c)->native = counter;
        return pluma::Value{};
    });
    b.add_native("Counter.bump", [](pluma::NativeCall& c) {
        auto* counter = static_cast<Counter*>(pluma::native::self(c)->native);
        counter->value += static_cast<int>(pluma::native::integer(c, 1));
        return pluma::Value::integer(pluma::NumKind::I32, counter->value);
    });
    b.add_native("Counter.describe", [](pluma::NativeCall& c) {
        const float* v = pluma::native::vec(c, 1);
        return pluma::native::str_result(std::string(pluma::native::name(c, 0)) + "@" + std::to_string(int(v[0])) +
                                        "," + std::to_string(int(v[1])));
    });
    b.add_finalizer("Counter", [](void* p) {
        delete static_cast<Counter*>(p);
        ++g_finalized;
    });
    return b;
}

} // namespace

TEST_CASE("natives: functions, methods, statics, host data and finalizers") {
    pluma::Bindings b = test_bindings();
    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    g_finalized = 0;
    {
        pluma::Session s(options, &b);
        REQUIRE_MESSAGE(s.prelude_result().ok, s.prelude_result().rendered);
        pluma::SessionResult r = s.run_script("t.plm", R"(
print(twice(2.5))
var c = Counter(10)
c.bump()
print(c.bump(by = 5))
print(Counter.describe(#ship, vec2(3.0, 4.0)))
c = Counter()
print(Time.tick)
)");
        REQUIRE_MESSAGE(r.ok, r.rendered);
        CHECK(out == "5.0\n16\nship@3,4\n0\n");
        CHECK(g_finalized == 1); // the first counter
        CHECK(s.set_global("Time.tick", pluma::Value::integer(pluma::NumKind::I32, 7)));
        out.clear();
        r = s.run_script("t2.plm", "print(Time.tick)");
        CHECK(out == "7\n");
    }
    CHECK(g_finalized == 2);
}

TEST_CASE("natives: unbound declarations are errors") {
    pluma::Session s;
    pluma::SessionResult r = s.run_script("t.plm", "@native fn missing() {}");
    CHECK_FALSE(r.compiled);
    REQUIRE(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].message == "no native implementation is bound for 'missing'");
}

TEST_CASE("projects: files are checked together and report their own positions") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({{"a.plm", "class A { fn b() -> B { return B() } }"},
                                          {"b.plm", "class B {}\nvar made = A().b()"}});
    REQUIRE_MESSAGE(r.ok, r.rendered);
    pluma::Session bad;
    r = bad.load_files({{"a.plm", "class A {}"}, {"b.plm", "\n\nvar x: int = \"s\""}});
    CHECK_FALSE(r.compiled);
    REQUIRE(r.diagnostics.size() == 1);
    CHECK(r.diagnostics[0].file == "b.plm");
    CHECK(r.diagnostics[0].line == 3);
    CHECK(r.diagnostics[0].column == 14);
    CHECK(r.rendered.find("b.plm:3:14: error") != std::string::npos);
    CHECK(r.rendered.find("var x: int = \"s\"") != std::string::npos);
}

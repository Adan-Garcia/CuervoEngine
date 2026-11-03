#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "editor/script_debug.hpp"
#include "pluma/session.hpp"

namespace fs = std::filesystem;
using namespace cuervo::editor;

namespace {

const char* const kAdder = "fn add(a: int, b: int) -> int {\n" // 1
                           "    var c = a + b\n"               // 2
                           "    return c\n"                    // 3
                           "}\n"                               // 4
                           "var total = 0\n"                   // 5
                           "for i in 0..3 { total += add(i, 1) }\n" // 6
                           "print(total)\n";                   // 7

// Pumps the session until `done` or 5 s pass (the VM runs on another thread and answers every ~20 ms).
template <class F> bool until(DebugSession& s, F&& done) {
    for (int i = 0; i < 500; ++i) {
        s.pump();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST_CASE("debug session: breakpoint stop, frames, locals, watch, step and continue") {
    const fs::path dir = fs::temp_directory_path() / "cuervo_dbg_session";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "test.plm") << kAdder;

    DebugSession ds;
    std::thread vm([&] {
        REQUIRE(ds.start(dir, {{"test.plm", {2}}}));
        std::string out;
        pluma::VmOptions options;
        options.out = [&out](std::string_view s) { out += s; };
        pluma::Session session(options);
        ds.debugger().attach(session.vm());
        const pluma::SessionResult r = session.run_script("test.plm", kAdder);
        CHECK(r.ok);
        CHECK(out == "6\n");
        ds.debugger().exited(0);
    });

    REQUIRE(until(ds, [&] { return ds.stopped() && !ds.locals().empty() && !ds.frames().empty(); }));
    CHECK(ds.stop_reason() == "breakpoint");
    REQUIRE(ds.frames().size() == 2);
    CHECK(ds.frames()[0].name == "add");
    CHECK(ds.frames()[0].rel == "test.plm");
    CHECK(ds.frames()[0].line == 2);
    CHECK(ds.frames()[1].line == 6);
    bool has_a = false;
    for (const DebugVar& v : ds.locals()) has_a = has_a || (v.name == "a" && v.value == "0");
    CHECK(has_a);

    ds.add_watch("a");
    ds.add_watch("nope_missing");
    REQUIRE(until(ds, [&] { return !ds.watches()[0].pending && !ds.watches()[1].pending; }));
    CHECK(ds.watches()[0].ok);
    CHECK(ds.watches()[0].result == "0");
    CHECK_FALSE(ds.watches()[1].ok);

    // Clicking the caller frame shows its locals.
    ds.select_frame(1);
    REQUIRE(until(ds, [&] { bool t = false; for (const DebugVar& v : ds.locals()) t = t || v.name == "i"; return t; }));

    const std::uint64_t serial = ds.stop_serial();
    ds.select_frame(0);
    ds.step_over();
    CHECK_FALSE(ds.stopped());
    REQUIRE(until(ds, [&] { return ds.stopped() && ds.stop_serial() > serial && !ds.frames().empty(); }));
    CHECK(ds.stop_reason() == "step");
    CHECK(ds.frames()[0].line == 3);

    // Remove the breakpoint while stopped, then run to the end.
    ds.set_breakpoints("test.plm", {});
    ds.resume();
    REQUIRE(until(ds, [&] { return ds.terminated(); }));
    vm.join();
    fs::remove_all(dir);
}

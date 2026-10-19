// Ahead-of-time C compilation tests (DESIGN.md 5.12): programs are compiled to
// C, built into a native shared library, and loaded into a fresh VM without
// `.plm` sources. Results, panic traces, live locals, refcounting, cycle
// collection, weak references, manual memory, and coroutines must match the
// bytecode VM bit for bit.

#include <chrono>
#include <doctest.h>

#include "pluma/aot.hpp"
#include "pluma/bindings.hpp"
#include "pluma/checker.hpp"
#include "pluma/compiler.hpp"
#include "pluma/heap.hpp"
#include "pluma/parser.hpp"
#include "pluma/session.hpp"
#include "tests/pluma/library_program.hpp"
#include "tests/pluma/math_program.hpp"
#include "tests/pluma/math_run.hpp"
#include "tests/pluma/run_helpers.hpp"

namespace {

struct AotRan {
    bool ok = false;
    std::string out;
    std::optional<pluma::PanicInfo> panic;
    std::string panic_short;
};

// Compiles `text` to C, builds a native `.so`, and runs it in a brand-new
// source-less VM (`load_aot`) to test full standalone native execution.
AotRan run_aot(std::string_view text, bool wrap_overflow = false, const pluma::Bindings* bindings = nullptr) {
    pluma::VmOptions compile_opts;
    compile_opts.wrap_overflow = wrap_overflow;
    pluma::Program prog;
    pluma::VM compile_vm(compile_opts);
    std::vector<std::unique_ptr<pluma::SourceFile>> sources;
    std::vector<std::unique_ptr<pluma::Arena>> arenas;

    if (bindings != nullptr) {
        for (const auto& n : bindings->natives()) {
            prog.native_names.insert(n.name);
            compile_vm.register_native(n.name, n.fn);
        }
        for (const auto& f : bindings->finalizers()) compile_vm.set_native_finalizer(f.class_name, f.fn);
        if (!bindings->preludes().empty()) {
            std::vector<pluma::SourceFile::Part> parts;
            for (const auto& p : bindings->preludes()) parts.push_back({p.path, p.source});
            sources.push_back(pluma::SourceFile::combine("<prelude>", parts));
            arenas.push_back(std::make_unique<pluma::Arena>());
            pluma::DiagnosticSink pdiags(*sources.back());
            pluma::Module pmod = pluma::parse_module(*sources.back(), *arenas.back(), pdiags);
            REQUIRE_FALSE(pdiags.has_errors());
            pluma::CheckedUnit punit = pluma::check_module(prog, *sources.back(), pmod, pdiags);
            REQUIRE(punit.ok);
            pluma::compile_program(prog, compile_vm);
        }
    }

    sources.push_back(std::make_unique<pluma::SourceFile>("test.plm", std::string(text)));
    arenas.push_back(std::make_unique<pluma::Arena>());
    const pluma::SourceFile& src = *sources.back();
    pluma::DiagnosticSink diags(src);
    pluma::Script script = pluma::parse_script(src, *arenas.back(), diags);
    REQUIRE_FALSE_MESSAGE(diags.has_errors(), pluma::render_all(diags.all(), src));
    pluma::CheckedUnit unit = pluma::check_script(prog, src, script, diags);
    REQUIRE_MESSAGE(unit.ok, pluma::render_all(diags.all(), src));
    pluma::compile_program(prog, compile_vm);

    std::string err;
    auto mod = pluma::compile_aot_module(prog, compile_vm, err, 2);
    REQUIRE_MESSAGE(mod != nullptr, err);

    // Drop the compiler's SourceFiles before running to prove the native
    // module does not depend on in-memory source buffers.
    sources.clear();

    AotRan ran;
    pluma::VmOptions run_opts;
    run_opts.wrap_overflow = wrap_overflow;
    run_opts.out = [&ran](std::string_view s) { ran.out += s; };
    pluma::Session runtime_session(run_opts, bindings, /*compile_preludes=*/false);
    pluma::SessionResult sr = runtime_session.load_aot(std::move(mod));
    REQUIRE_MESSAGE(sr.compiled, sr.rendered);
    ran.ok = sr.ok;
    ran.panic = sr.panic;
    if (sr.panic) ran.panic_short = std::format("{}:{}: {}", sr.panic->line, sr.panic->column, sr.panic->message);
    return ran;
}

void check_aot_parity(std::string_view text, bool wrap_overflow = false) {
    INFO("source:\n" << text);
    test::Ran vm_r = test::run(text, wrap_overflow);
    REQUIRE_MESSAGE(vm_r.compiled, vm_r.rendered);
    AotRan aot_r = run_aot(text, wrap_overflow);
    CHECK(aot_r.ok == vm_r.ok);
    CHECK(aot_r.out == vm_r.out);
    CHECK(aot_r.panic_short == vm_r.panic);
}

} // namespace

TEST_CASE("aot: core language semantics match bytecode VM") {
    check_aot_parity(R"PLM(
// Arithmetic, casts, bitwise, loops, strings
print(1 + 2 * 3)
print(7 / 2)
print(-7 / 2)
print(-7 % 3)
print(7 % -3)
print(1.5 * 2.0)
print(0.1f64 + 0.2f64)
print(0xff & 0x0f | 0x30)
print(1 << 4)
print(-16 >> 2)
print(~0)
print(7 as float / 2.0)
print(3.99 as int)
print(-1 as u8)
print(300 as u8)

// Range loops (inclusive, exclusive, negative step)
var sum = 0
for i in 0..5 { sum += i }
for i in 1..=4 { sum += i * 10 }
for i in 10..0 step -3 { sum += i }
print("sum={sum}")

// Closures capturing mutable locals by cell
fn make_counter(start_val: int) -> fn(int) -> int {
    var n = start_val
    return fn(d: int) -> int {
        n += d
        return n
    }
}
var c1 = make_counter(10)
var c2 = make_counter(100)
print("{c1(5)} {c1(3)} {c2(20)}")

// Struct value semantics & operator overloading
struct Pair: Add, Display {
    var a: int
    var b = 2
    fn add(other: Pair) -> Pair { return Pair { a = a + other.a, b = b + other.b } }
    fn to_str() -> str { return "({a},{b})" }
}
var p1 = Pair { a = 1 }
var p2 = p1
p2.a = 9
print("{p1} {p2} {p1 + p2}")

// Classes, inheritance, virtual dispatch, traits, generics, downcast/is
trait Describable {
    fn desc() -> str
}
class Base: Describable {
    var x = 10
    fn init(v: int) { x = v }
    fn value() -> int { return x }
    fn desc() -> str { return "Base({x})" }
}
class Derived extends Base {
    var y = 3
    fn init(v: int, w: int) {
        super.init(v)
        y = w
    }
    fn value() -> int { return super.value() * y }
    fn desc() -> str { return "Derived({x},{y})" }
}
fn show<T: Describable>(item: T) -> str {
    return item.desc()
}
var b: Base = Derived(7, 4)
print("{b.value()} {show(b)} {b is Derived}")
if b is Derived {
    var d = b as Derived
    print(d.y)
}

// Enums (plain and payload) + pattern matching + defer
enum Shape {
    Circle(r: float),
    Rect(w: float, h: float),
    Empty,
}
fn area(s: Shape) -> float {
    match s {
        Shape.Circle(r) => { return r * r * 3.0 }
        Shape.Rect(w, h) if w > 0.0 => { return w * h }
        _ => { return 0.0 }
    }
}
print("{area(Shape.Circle(2.0))} {area(Shape.Rect(3.0, 4.0))} {area(Shape.Empty)}")

// Vectors and swizzles
var v = vec2(3.0, 4.0)
var w = v.yx * 2.0 + vec2(1.0, -1.0)
var v_mod = v
v_mod.x = 9.0
print("{v.length()} {w.x} {w.y} {v_mod}")
var v3 = vec3(1.0, 2.0, 3.0)
print("{v3.zyx} {v3[1]}")
)PLM");
}

TEST_CASE("aot: coroutines, signals, cycle collector, weak refs, and manual memory") {
    pluma::collect_all_cycles();
    std::size_t baseline = pluma::live_objects();
    {
        std::string code = R"(
// Cycle collector + weak var
class Node {
    var name: str
    var next: Node? = null
    weak var parent: Node? = null
    fn init(n: str) { name = n }
}
var weak_box = Node("holder")
fn make_cycle(box: Node) {
    var a = Node("a")
    var b = Node("b")
    a.next = b
    b.next = a
    b.parent = a
    box.parent = a
    print("before: {b.parent.name}")
}
make_cycle(weak_box)

// Manual memory: alloc/free, unsafe {}
manual class Particle {
    var x = 0.0
    fn init(px: float) { x = px }
}
var p = alloc Particle(42.5)
unsafe {
    p.x += 7.5
    print("p.x={p.x}")
}
free(p)
)";
        AotRan r = run_aot(code);
        REQUIRE(r.ok);
        CHECK(r.out == "before: a\np.x=50.0\n");
    }
    pluma::collect_all_cycles();
    CHECK(pluma::live_objects() == baseline);

    // Coroutines and signals ticked across multiple frames in AOT mode.
    std::string coro_code = R"(
class Bus {
    signal sig(v: int)
    fn send(v: int) { emit sig(v) }
}
var bus = Bus()
var log = List<str>()

fn worker(label: str) {
    log.push("{label}:start")
    await wait_ticks(2)
    log.push("{label}:after2")
    var v = await bus.sig
    log.push("{label}:sig={v}")
}

fn nested_wait() -> int {
    await wait_ticks(1)
    return 99
}

fn caller() {
    var r = await nested_wait()
    log.push("caller:{r}")
}

fn begin() {
    start(worker("w"))
    start(caller())
}

fn send_sig(v: int) {
    bus.send(v)
}
)";
    pluma::Program prog;
    pluma::VM cvm;
    pluma::SourceFile src("coro.plm", coro_code);
    pluma::Arena arena;
    pluma::DiagnosticSink diags(src);
    pluma::Script script = pluma::parse_script(src, arena, diags);
    REQUIRE_FALSE(diags.has_errors());
    pluma::CheckedUnit unit = pluma::check_script(prog, src, script, diags);
    REQUIRE_MESSAGE(unit.ok, pluma::render_all(diags.all(), src));
    pluma::compile_program(prog, cvm);

    std::string err;
    auto mod = pluma::compile_aot_module(prog, cvm, err, 2);
    REQUIRE_MESSAGE(mod != nullptr, err);

    pluma::Session sess({}, nullptr, false);
    REQUIRE(sess.load_aot(std::move(mod)).ok);
    CHECK(sess.vm().protos[*sess.vm().proto_by_key("worker")]->aot_fn != nullptr);
    CHECK(sess.vm().protos[*sess.vm().proto_by_key("nested_wait")]->aot_fn != nullptr);
    CHECK(sess.vm().protos[*sess.vm().proto_by_key("caller")]->aot_fn != nullptr);

    REQUIRE(sess.vm().call(*sess.vm().proto_by_key("begin"), {}).ok);
    REQUIRE(sess.vm().tick_coroutines().ok); // tick 1: caller resumes
    REQUIRE(sess.vm().tick_coroutines().ok); // tick 2: worker resumes from wait_ticks(2), awaits bus.sig
    const pluma::Value sig_val = pluma::Value::integer(pluma::NumKind::I32, 77);
    REQUIRE(sess.vm().call(*sess.vm().proto_by_key("send_sig"), std::span<const pluma::Value>(&sig_val, 1)).ok);
    REQUIRE(sess.vm().tick_coroutines().ok); // tick 3: worker wakes from bus.sig
    CHECK(sess.vm().format(sess.global("log"), false) == "[\"w:start\", \"caller:99\", \"w:after2\", \"w:sig=77\"]");
}

TEST_CASE("aot: panics, source-less stack traces, live locals, and use-after-free diagnostics") {
    // Overflow panic in dev, wrap in release
    check_aot_parity("var x = 2147483647\nvar y = x + 1", false);
    check_aot_parity("var x = 2147483647\nprint(x + 1)", true);

    // Null dereference slice text without SourceFile at runtime
    check_aot_parity(R"(
class Box { var v = 1 }
var b: Box? = null
print(b.v)
)");

    // Use-after-free with exact free site line:col and expression slice
    check_aot_parity(R"(
class Item { var hp = 10 }
var it = alloc Item()
free(it)
print(it.hp)
)");

    // Stack trace and live locals inspection during a panic in source-less AOT mode
    std::string trace_code = R"(
fn inner(scale: int) -> int {
    var tag = "boom"
    var zero = 0
    return scale / zero
}
fn outer(base: int) -> int {
    var boosted = base + 5
    return inner(boosted)
}
outer(10)
)";
    test::Ran vm_r = test::run(trace_code);
    AotRan aot_r = run_aot(trace_code);
    REQUIRE_FALSE(aot_r.ok);
    REQUIRE(aot_r.panic.has_value());
    CHECK(aot_r.panic_short == vm_r.panic);
    REQUIRE(aot_r.panic->frames.size() >= 2);
    CHECK(aot_r.panic->frames[0].function == "inner");
    CHECK(aot_r.panic->frames[0].file == "test.plm");
    CHECK(aot_r.panic->frames[0].line == 5);
    REQUIRE(aot_r.panic->frames[0].locals.size() >= 2);
    CHECK(aot_r.panic->frames[0].locals[0].name == "scale");
    CHECK(aot_r.panic->frames[0].locals[0].value == "15");
    CHECK(aot_r.panic->frames[0].locals[1].name == "tag");
    CHECK(aot_r.panic->frames[0].locals[1].value == "\"boom\"");
    CHECK(aot_r.panic->frames[1].function == "outer");
    CHECK(aot_r.panic->frames[1].locals[0].name == "base");
    CHECK(aot_r.panic->frames[1].locals[0].value == "10");
}

TEST_CASE("aot: runs measurably faster than the bytecode VM") {
    std::string bench_src = R"(
fn simulate(iters: int) -> float {
    var pos = vec2(1.0, 2.0)
    var vel = vec2(0.5, -0.25)
    var acc = 0
    for i in 0..iters {
        pos = pos + vel * 0.016
        vel = vec2(vel.x * 0.999 + 0.001, vel.y * 0.999 - 0.0005)
        acc = (acc + (i & 7)) ^ (i >> 2)
    }
    return pos.x + pos.y + (acc as float) * 0.0001
}
)";
    pluma::Session vm_sess;
    REQUIRE(vm_sess.load_files({{"bench.plm", bench_src}}).ok);
    auto sim_idx = vm_sess.vm().proto_by_key("simulate");
    REQUIRE(sim_idx.has_value());

    std::string err;
    auto mod = pluma::compile_aot_module(vm_sess.program(), vm_sess.vm(), err, 2);
    REQUIRE_MESSAGE(mod != nullptr, err);

    pluma::Session aot_sess({}, nullptr, false);
    REQUIRE(aot_sess.load_aot(std::move(mod)).ok);
    auto aot_sim_idx = aot_sess.vm().proto_by_key("simulate");
    REQUIRE(aot_sim_idx.has_value());

    const pluma::Value arg = pluma::Value::integer(pluma::NumKind::I32, 200000);
    pluma::Value vm_out{};
    pluma::Value aot_out{};

    // Warm up both paths.
    REQUIRE(vm_sess.vm().call(*sim_idx, std::span<const pluma::Value>(&arg, 1), &vm_out).ok);
    REQUIRE(aot_sess.vm().call(*aot_sim_idx, std::span<const pluma::Value>(&arg, 1), &aot_out).ok);
    CHECK(vm_out.f == aot_out.f);

    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(vm_sess.vm().call(*sim_idx, std::span<const pluma::Value>(&arg, 1), &vm_out).ok);
    auto t1 = std::chrono::steady_clock::now();
    REQUIRE(aot_sess.vm().call(*aot_sim_idx, std::span<const pluma::Value>(&arg, 1), &aot_out).ok);
    auto t2 = std::chrono::steady_clock::now();

    double vm_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double aot_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    INFO("VM time: " << vm_ms << " ms, AOT time: " << aot_ms << " ms (speedup: " << (vm_ms / aot_ms) << "x)");
    CHECK(vm_out.f == aot_out.f);
    CHECK(aot_ms < vm_ms);
}

// Milestone 9.2a (DESIGN.md 6 "done when"): the library program and the new
// control flow (`?`, Map/Set/Deque loops, method values) match the VM.
TEST_CASE("aot: 9.2a collections, strings and language match the bytecode VM") {
    check_aot_parity(test::library_program);
    CHECK(run_aot(test::library_program).out == test::library_expected);
    check_aot_parity(R"PLM(
fn find(m: Map<str, int>, k: str) -> Result<int, str> {
    defer print("looked up {k}")
    if m.get(k)? v { return Ok(v) }
    return Err("missing {k}")
}
fn sum2(m: Map<str, int>) -> Result<int, str> {
    return Ok(find(m, "a")? + find(m, "b")?)
}
var m = {"a": 1, "b": 2}
print(sum2(m))
m.remove("b")
print(sum2(m))
var total = 0
outer: for k, v in m {
    for x in Set<int>() { continue outer }
    if v > 100 { break }
    total += v
}
var d: Deque<int> = [1, 2, 3]
for i, x in d {
    if i == 1 { continue }
    total += x * 10
}
print(total)
class C {
    var k = 2
    fn mul(x: int) -> int { return x * k }
}
var f = C().mul
print([1, 2, 3].map(f))
)PLM");
    check_aot_parity("var m = {1: 1, 2: 2}\nfor k in m { m[k + 10] = 0 }");
    check_aot_parity("var r: Result<int, str> = Err(\"x\")\nprint(r.unwrap())");
}

// Milestone 9.2b: every function of the math library, and the new built-ins
// (arithmetic, vector methods, v[i] = e), print the same text in the VM and in
// AOT-to-C. Floats print with their shortest round-trip digits, so equal text
// is equal bits. (The engine-hooked values against dmath: tests/cuervo.)
TEST_CASE("aot: the math library matches the bytecode VM bit for bit") {
    for (const char* program : {test::math_exact_program, test::math_transcendental_program}) {
        test::Ran vm_r = test::run_math(program);
        REQUIRE_MESSAGE(vm_r.compiled, vm_r.rendered);
        AotRan aot_r = run_aot(program, false, &test::math_bindings());
        CHECK(aot_r.ok == vm_r.ok);
        CHECK(aot_r.out == vm_r.out);
    }
    CHECK(run_aot(test::math_exact_program, false, &test::math_bindings()).out == test::math_exact_expected);
}

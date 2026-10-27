// The Pluma FFI (`@extern`, pluma/ffi.hpp, LANGUAGE.md 9.28, milestone 10.5c):
// C functions of shared libraries with explicit marshaling, in the VM and in
// AOT modules.

#include <doctest.h>

#include <string>

#include "pluma/aot.hpp"
#include "pluma/ffi.hpp"
#include "pluma/session.hpp"

namespace {

std::string show(pluma::Session& s, std::string_view g) { return s.vm().format(s.global(g), false); }

std::string first_error(std::string src) {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({{"t.plm", std::move(src)}});
    return r.compiled ? std::string() : r.rendered;
}

#if defined(__linux__) || defined(_WIN32)
#if defined(__linux__)
#define LIBM "libm.so.6"
#else
#define LIBM "ucrtbase.dll" // the C runtime of MSVC and of MinGW-w64's UCRT builds
#endif
// ldexp, not ldexpf: the UCRT has no ldexpf export (it is inline in math.h).
const char* libc_src = "@extern(\"" LIBM "\", \"cbrtf\") fn c_cbrtf(x: float) -> float {}\n"
                       "@extern(\"" LIBM "\", \"fmax\") fn c_fmax(a: f64, b: f64) -> f64 {}\n"
                       "@extern(\"" LIBM "\", \"ldexp\") fn c_ldexp(x: f64, e: int) -> f64 {}\n"
                       R"(
@extern("", "strlen") fn c_strlen(s: str) -> u64 {}
@extern("", "abs") fn c_abs(x: int) -> int {}
@extern("", "strchr") fn c_strchr(s: str, c: int) -> str {}
@extern("", "atoi") fn c_atoi(s: str) -> i8 {}
var a = c_cbrtf(8.0)
var b = c_fmax(1.5f64, -2.0f64)
var c = c_ldexp(1.5f64, 4)
var d = c_strlen("cuervo")
var e = c_abs(-42)
var f = c_strchr("hello world", 119)
var g = c_atoi("300") // 300 truncated to i8
)";
#endif

} // namespace

#if defined(__linux__) || defined(_WIN32)
TEST_CASE("ffi: C functions with numbers, floats and strings, in the VM and AOT") {
    pluma::Session vm;
    pluma::SessionResult r = vm.load_files({{"ffi.plm", libc_src}});
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(show(vm, "a") == "2.0");
    CHECK(show(vm, "b") == "1.5");
    CHECK(show(vm, "c") == "24.0");
    CHECK(show(vm, "d") == "6");
    CHECK(show(vm, "e") == "42");
    CHECK(show(vm, "f") == "world");
    CHECK(show(vm, "g") == "44");

    std::string err;
    auto mod = pluma::compile_aot_module(vm.program(), vm.vm(), err, 1);
    REQUIRE_MESSAGE(mod != nullptr, err);
    pluma::Session aot({}, nullptr, false);
    r = aot.load_aot(std::move(mod));
    REQUIRE_MESSAGE(r.ok, r.rendered);
    for (const char* g : {"a", "b", "c", "d", "e", "f", "g"}) CHECK(show(aot, g) == show(vm, g));
}
#endif

TEST_CASE("ffi: a missing library or symbol fails when the function is called") {
    pluma::Session s;
    pluma::SessionResult r = s.load_files({{"t.plm", "@extern(\"libcuervo_nope.so\", \"x\") fn nope() -> int {}\n"
                                                    "var ok = 1\nfn call() -> int { return nope() }\n"}});
    REQUIRE_MESSAGE(r.ok, r.rendered); // declaring is fine
    auto idx = s.vm().proto_by_key("call");
    REQUIRE(idx.has_value());
    pluma::RunResult run = s.vm().call(*idx, {});
    CHECK_FALSE(run.ok);
    REQUIRE(run.panic.has_value());
    CHECK(run.panic->render().find("can't load library 'libcuervo_nope.so'") != std::string::npos);
}

TEST_CASE("ffi: @extern declarations are checked") {
    CHECK(first_error("@extern(\"libm.so.6\") fn f(a: int, b: int, c: int, d: int, e: int) -> int {}")
              .find("at most 4 parameters") != std::string::npos);
    CHECK(first_error("struct P { var x = 1 }\n@extern(\"\") fn f(p: P) {}").find("must be a number, bool or str") !=
          std::string::npos);
    CHECK(first_error("@extern(\"\") fn f() -> List<int> {}").find("must return a number, bool, str or nothing") !=
          std::string::npos);
    CHECK(first_error("class C {\n    @extern(\"\") fn f() {}\n}").find("only to top-level functions") !=
          std::string::npos);
    CHECK(first_error("@extern(lib) fn f() {}").find("as string literals") != std::string::npos);
    CHECK(first_error("@extern(\"\", \"x\") fn f() { print(1) }").find("must be empty") != std::string::npos);
    CHECK(pluma::ffi_key("libm.so.6", "cbrt", "d>d") == "ffi|libm.so.6|cbrt|d>d");
    std::string error;
    CHECK_FALSE(pluma::make_ffi_native("ffi|x|y|dddddd>d", error).has_value());
    CHECK(error.find("malformed") != std::string::npos);
}

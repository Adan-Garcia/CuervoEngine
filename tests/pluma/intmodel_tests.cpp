// Integer semantics against an independent reference model (LANGUAGE.md 9.2):
// every integer type, edge values, all operators and conversions. Release
// (wrapping) mode runs on the VM and in AOT; development mode (overflow, bad
// shifts and division by zero panic) runs on the VM, one operation per script.

#include <doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "tests/pluma/aot_helpers.hpp"

// The reference model needs a 128-bit integer, which MSVC lacks: the test runs on GCC/Clang builds.
#if defined(__SIZEOF_INT128__)

namespace {

__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;

struct Kind {
    const char* name;
    int width;
    bool is_signed;
    i128 lo() const { return is_signed ? -(static_cast<i128>(1) << (width - 1)) : 0; }
    i128 hi() const { return is_signed ? (static_cast<i128>(1) << (width - 1)) - 1 : (static_cast<i128>(1) << width) - 1; }
    bool fits(i128 v) const { return v >= lo() && v <= hi(); }
    i128 wrap(i128 v) const {
        const i128 mod = static_cast<i128>(1) << width;
        i128 r = ((v % mod) + mod) % mod;
        if (is_signed && r > hi()) r -= mod;
        return r;
    }
};

constexpr std::array<Kind, 8> kinds = {{{"i8", 8, true},   {"i16", 16, true},  {"i32", 32, true},  {"i64", 64, true},
                                        {"u8", 8, false},  {"u16", 16, false}, {"u32", 32, false}, {"u64", 64, false}}};

std::string to_text(i128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    u128 u = neg ? -static_cast<u128>(v) : static_cast<u128>(v);
    std::string s;
    while (u != 0) {
        s += static_cast<char>('0' + static_cast<int>(u % 10));
        u /= 10;
    }
    if (neg) s += '-';
    return std::string(s.rbegin(), s.rend());
}

// A literal of kind `k` with value `v`. The minimum is built without a literal
// that would not fit.
std::string lit(const Kind& k, i128 v) {
    if (v < 0 && v == k.lo()) return std::format("({}{} - 1{})", to_text(v + 1), k.name, k.name);
    return std::format("{}{}", to_text(v), k.name);
}

std::vector<i128> edge_values(const Kind& k) {
    std::vector<i128> out;
    for (i128 c : {k.lo(), k.lo() + 1, i128(-129), i128(-2), i128(-1), i128(0), i128(1), i128(2), i128(7), i128(127),
                   i128(128), i128(255), k.hi() / 2, k.hi() - 1, k.hi()}) {
        if (!k.fits(c)) continue;
        bool dup = false;
        for (i128 e : out) dup = dup || e == c;
        if (!dup) out.push_back(c);
    }
    return out;
}

i128 trunc_div(i128 a, i128 b) { return a / b; } // C++ division truncates toward zero

// Reference semantics for `a OP b` in release mode. Shift counts are masked.
i128 wrapped_binary(const Kind& k, char op, i128 a, i128 b) {
    switch (op) {
        case '+': return k.wrap(a + b);
        case '-': return k.wrap(a - b);
        case '*': return k.wrap(a * b);
        case '/': return k.wrap(trunc_div(a, b));
        case '%': return k.wrap(a - trunc_div(a, b) * b);
        case '&': return k.wrap(a & b);
        case '|': return k.wrap(a | b);
        case '^': return k.wrap(a ^ b);
        case 'l': return k.wrap(a << static_cast<int>(k.wrap(b) & (k.width - 1)));
        default: return a >> static_cast<int>(k.wrap(b) & (k.width - 1)); // 'r', arithmetic on negatives
    }
}

// Does the exact result overflow (development mode panics)?
bool overflows(const Kind& k, char op, i128 a, i128 b) {
    switch (op) {
        case '+': return !k.fits(a + b);
        case '-': return !k.fits(a - b);
        case '*': return !k.fits(a * b);
        case '/': return !k.fits(trunc_div(a, b));
        default: return false;
    }
}

std::string saturating_float_to_int(const Kind& k, long double v) {
    if (std::isnan(v)) return "0";
    long double t = std::trunc(v);
    if (t <= static_cast<long double>(k.lo())) return to_text(k.lo());
    if (t >= static_cast<long double>(k.hi())) return to_text(k.hi());
    return to_text(static_cast<i128>(t));
}

} // namespace

TEST_CASE("semantics: wrapping integer arithmetic matches the reference model on the VM and in AOT") {
    std::string program;
    std::string expected;
    for (const Kind& k : kinds) {
        std::vector<i128> vs = edge_values(k);
        std::string list;
        for (i128 v : vs) list += (list.empty() ? "" : ", ") + lit(k, v);
        program += std::format("fn run_{0}(xs: List<{0}>) {{\n  for a in xs {{\n    for b in xs {{\n", k.name);
        for (const char* op : {"+", "-", "*", "&", "|", "^", "<<", ">>", "<", "<=", "==", "!="}) {
            program += std::format("      print(a {} b)\n", op);
        }
        program += "      if b != 0 {\n        print(a / b)\n        print(a % b)\n      }\n    }\n";
        program += k.is_signed ? "    print(-a)\n" : "";
        program += "    print(~a)\n";
        for (const Kind& to : kinds) program += std::format("    print(a as {})\n", to.name);
        program += "  }\n}\n";
        program += std::format("run_{}([{}])\n", k.name, list);

        for (i128 a : vs) {
            for (i128 b : vs) {
                for (char op : {'+', '-', '*', '&', '|', '^', 'l', 'r'}) expected += to_text(wrapped_binary(k, op, a, b)) + "\n";
                expected += std::string(a < b ? "true" : "false") + "\n";
                expected += std::string(a <= b ? "true" : "false") + "\n";
                expected += std::string(a == b ? "true" : "false") + "\n";
                expected += std::string(a != b ? "true" : "false") + "\n";
                if (b != 0) {
                    expected += to_text(wrapped_binary(k, '/', a, b)) + "\n";
                    expected += to_text(wrapped_binary(k, '%', a, b)) + "\n";
                }
            }
            if (k.is_signed) expected += to_text(k.wrap(-a)) + "\n";
            expected += to_text(k.wrap(~a)) + "\n";
            for (const Kind& to : kinds) expected += to_text(to.wrap(a)) + "\n";
        }
    }
    test::Ran vm = test::run(program, /*wrap_overflow=*/true);
    REQUIRE_MESSAGE(vm.compiled, vm.rendered);
    CHECK(vm.panic.empty());
    CHECK(vm.out == expected);
    aot_test::AotRan native = aot_test::run_aot(program, /*wrap_overflow=*/true);
    CHECK(native.panic_short.empty());
    CHECK(native.out == expected);
}

TEST_CASE("semantics: float to integer conversions saturate, NaN is 0, on the VM and in AOT") {
    const std::vector<std::string> fvals = {"0.0",    "-0.0",   "0.5",   "-0.5",  "1.5",     "-1.5",   "127.9",
                                            "255.5",  "256.0",  "-129.0", "3.0e9", "-3.0e9", "4.3e9",  "1.0e19",
                                            "-1.0e19", "2.0e19", "1.0e30", "-1.0e30", "(1.0 / 0.0)", "(-1.0 / 0.0)", "(0.0 / 0.0)"};
    const std::vector<long double> fnum = {0.0L,   -0.0L,  0.5L,   -0.5L,   1.5L,   -1.5L,   127.9L,
                                           255.5L, 256.0L, -129.0L, 3.0e9L, -3.0e9L, 4.3e9L, 1.0e19L,
                                           -1.0e19L, 2.0e19L, 1.0e30L, -1.0e30L, std::numeric_limits<long double>::infinity(),
                                           -std::numeric_limits<long double>::infinity(), std::numeric_limits<long double>::quiet_NaN()};
    std::string program = "fn run(xs: List<float>) {\n  for a in xs {\n";
    for (const Kind& k : kinds) program += std::format("    print(a as {})\n", k.name);
    program += "  }\n}\nrun([";
    for (std::size_t i = 0; i < fvals.size(); ++i) program += (i ? ", " : "") + fvals[i];
    program += "])\n";
    std::string expected;
    for (std::size_t i = 0; i < fnum.size(); ++i) {
        // The values are f32: round them first, exactly like the language does.
        long double v = std::isnan(fnum[i]) || std::isinf(fnum[i]) ? fnum[i] : static_cast<long double>(static_cast<float>(fnum[i]));
        for (const Kind& k : kinds) expected += saturating_float_to_int(k, v) + "\n";
    }
    test::Ran vm = test::run(program);
    REQUIRE_MESSAGE(vm.compiled, vm.rendered);
    CHECK(vm.out == expected);
    aot_test::AotRan native = aot_test::run_aot(program);
    CHECK(native.out == expected);
}

TEST_CASE("semantics: development mode panics exactly when the exact result leaves the type") {
    int cases = 0;
    for (const Kind& k : kinds) {
        std::vector<i128> vs = edge_values(k);
        for (i128 a : vs) {
            for (i128 b : vs) {
                for (char op : {'+', '-', '*', '/'}) {
                    if (op == '/' && b == 0) continue;
                    const char opc[2] = {op, 0};
                    std::string src = std::format("fn f(a: {0}, b: {0}) -> {0} {{ return a {1} b }}\nprint(f({2}, {3}))\n",
                                                  k.name, opc, lit(k, a), lit(k, b));
                    test::Ran r = test::run(src);
                    REQUIRE_MESSAGE(r.compiled, r.rendered);
                    ++cases;
                    if (overflows(k, op, a, b)) {
                        INFO(src);
                        CHECK(r.panic.find("integer overflow in '") != std::string::npos);
                    } else {
                        INFO(src);
                        CHECK(r.panic.empty());
                        CHECK(r.out == to_text(wrapped_binary(k, op, a, b)) + "\n");
                    }
                }
                for (char op : {'l', 'r'}) {
                    std::string src = std::format("fn f(a: {0}, b: {0}) -> {0} {{ return a {1} b }}\nprint(f({2}, {3}))\n",
                                                  k.name, op == 'l' ? "<<" : ">>", lit(k, a), lit(k, b));
                    test::Ran r = test::run(src);
                    REQUIRE_MESSAGE(r.compiled, r.rendered);
                    ++cases;
                    INFO(src);
                    if (b < 0 || b >= k.width) {
                        CHECK(r.panic.find("shift amount") != std::string::npos);
                    } else {
                        CHECK(r.panic.empty());
                        // Shifts discard bits instead of panicking.
                        CHECK(r.out == to_text(wrapped_binary(k, op, a, b)) + "\n");
                    }
                }
            }
        }
    }
    CHECK(cases > 1000);
}

#endif // __SIZEOF_INT128__

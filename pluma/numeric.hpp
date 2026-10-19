#pragma once

// Integer and float arithmetic with the language's semantics, shared by
// constant folding (checker) and the VM so both agree bit for bit.
//
// Integer values of every kind travel as int64 bits: signed kinds sign-
// extended, unsigned kinds zero-extended (u64 uses all 64 bits). Overflow is
// detected portably (no compiler builtins, no __int128) and the wrapped
// two's-complement result is always produced, so release builds can wrap
// while development builds panic (LANGUAGE.md 1.5, DESIGN.md 7.1).
// `/` truncates toward zero, `%` takes the dividend's sign, division by zero
// is an error in both modes (DESIGN.md 7.2).

#include <cmath>
#include <cstdint>
#include <limits>

#include "pluma/ast.hpp"
#include "pluma/sema.hpp"

namespace pluma::num {

// Normalizes raw bits to kind `k` (truncate, then sign/zero-extend).
inline std::int64_t wrap(NumKind k, std::uint64_t bits) {
    switch (k) {
        case NumKind::I8: return static_cast<std::int8_t>(static_cast<std::uint8_t>(bits));
        case NumKind::I16: return static_cast<std::int16_t>(static_cast<std::uint16_t>(bits));
        case NumKind::I32: return static_cast<std::int32_t>(static_cast<std::uint32_t>(bits));
        case NumKind::U8: return static_cast<std::int64_t>(bits & 0xffu);
        case NumKind::U16: return static_cast<std::int64_t>(bits & 0xffffu);
        case NumKind::U32: return static_cast<std::int64_t>(bits & 0xffffffffu);
        default: return static_cast<std::int64_t>(bits);
    }
}

inline std::int64_t min_of(NumKind k) {
    switch (k) {
        case NumKind::I8: return std::numeric_limits<std::int8_t>::min();
        case NumKind::I16: return std::numeric_limits<std::int16_t>::min();
        case NumKind::I32: return std::numeric_limits<std::int32_t>::min();
        case NumKind::I64: return std::numeric_limits<std::int64_t>::min();
        default: return 0;
    }
}

// Max as uint64 (covers u64).
inline std::uint64_t max_of(NumKind k) {
    switch (k) {
        case NumKind::I8: return 0x7f;
        case NumKind::I16: return 0x7fff;
        case NumKind::I32: return 0x7fffffff;
        case NumKind::I64: return 0x7fffffffffffffffull;
        case NumKind::U8: return 0xff;
        case NumKind::U16: return 0xffff;
        case NumKind::U32: return 0xffffffffull;
        default: return 0xffffffffffffffffull;
    }
}

// Does the mathematical value (negative, magnitude) fit kind k?
inline bool fits(NumKind k, bool negative, std::uint64_t magnitude) {
    if (!negative) return magnitude <= max_of(k);
    if (!is_signed(k)) return magnitude == 0;
    return magnitude <= static_cast<std::uint64_t>(-(min_of(k) + 1)) + 1;
}

enum class Status : std::uint8_t { Ok, Overflow, DivByZero, BadShift };

namespace detail {

inline Status add_i64(std::int64_t a, std::int64_t b, std::int64_t& out) {
    out = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
        return Status::Overflow;
    }
    return Status::Ok;
}

inline Status sub_i64(std::int64_t a, std::int64_t b, std::int64_t& out) {
    out = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
    if ((b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) ||
        (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b)) {
        return Status::Overflow;
    }
    return Status::Ok;
}

inline Status mul_i64(std::int64_t a, std::int64_t b, std::int64_t& out) {
    out = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
    if (a == 0 || b == 0) return Status::Ok;
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t min = std::numeric_limits<std::int64_t>::min();
    if ((a == -1 && b == min) || (b == -1 && a == min)) return Status::Overflow;
    if (a > 0) {
        if (b > 0 ? a > max / b : b < min / a) return Status::Overflow;
    } else {
        if (b > 0 ? a < min / b : (a != 0 && b < max / a)) return Status::Overflow;
    }
    return Status::Ok;
}

} // namespace detail

// a op b for op in Add, Sub, Mul, Div, Rem on integer kind k.
inline Status int_arith(BinaryOp op, NumKind k, std::int64_t a, std::int64_t b, std::int64_t& out) {
    if ((op == BinaryOp::Div || op == BinaryOp::Rem) && b == 0) {
        out = 0;
        return Status::DivByZero;
    }
    if (k == NumKind::U64) {
        auto ua = static_cast<std::uint64_t>(a);
        auto ub = static_cast<std::uint64_t>(b);
        std::uint64_t r = 0;
        bool ovf = false;
        switch (op) {
            case BinaryOp::Add: r = ua + ub; ovf = r < ua; break;
            case BinaryOp::Sub: r = ua - ub; ovf = ub > ua; break;
            case BinaryOp::Mul: r = ua * ub; ovf = ua != 0 && r / ua != ub; break;
            case BinaryOp::Div: r = ua / ub; break;
            case BinaryOp::Rem: r = ua % ub; break;
            default: break;
        }
        out = static_cast<std::int64_t>(r);
        return ovf ? Status::Overflow : Status::Ok;
    }
    if (k == NumKind::I64) {
        switch (op) {
            case BinaryOp::Add: return detail::add_i64(a, b, out);
            case BinaryOp::Sub: return detail::sub_i64(a, b, out);
            case BinaryOp::Mul: return detail::mul_i64(a, b, out);
            case BinaryOp::Div:
                if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                    out = a;
                    return Status::Overflow;
                }
                out = a / b;
                return Status::Ok;
            case BinaryOp::Rem:
                if (b == -1) {
                    out = 0;
                    return Status::Ok;
                }
                out = a % b;
                return Status::Ok;
            default: out = 0; return Status::Ok;
        }
    }
    // Kinds of 32 bits or fewer: exact in int64, then range-check.
    std::int64_t r = 0;
    switch (op) {
        case BinaryOp::Add: r = a + b; break;
        case BinaryOp::Sub: r = a - b; break;
        case BinaryOp::Mul: r = a * b; break;
        case BinaryOp::Div: r = a / b; break;
        case BinaryOp::Rem: r = a % b; break;
        default: break;
    }
    out = wrap(k, static_cast<std::uint64_t>(r));
    return out == r ? Status::Ok : Status::Overflow;
}

// Bitwise ops; shifts check the count (0 <= n < width).
inline Status int_bits(BinaryOp op, NumKind k, std::int64_t a, std::int64_t b, std::int64_t& out) {
    auto ua = static_cast<std::uint64_t>(a);
    auto ub = static_cast<std::uint64_t>(b);
    int width = bit_width(k);
    switch (op) {
        case BinaryOp::BitAnd: out = wrap(k, ua & ub); return Status::Ok;
        case BinaryOp::BitOr: out = wrap(k, ua | ub); return Status::Ok;
        case BinaryOp::BitXor: out = wrap(k, ua ^ ub); return Status::Ok;
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            bool bad = (is_signed(k) && b < 0) || ub >= static_cast<std::uint64_t>(width);
            unsigned n = static_cast<unsigned>(ub & static_cast<std::uint64_t>(width - 1));
            if (op == BinaryOp::Shl) {
                out = wrap(k, ua << n);
            } else if (is_signed(k)) {
                out = a >> n; // arithmetic shift (C++20 defines it)
            } else {
                out = static_cast<std::int64_t>(ua >> n);
            }
            return bad ? Status::BadShift : Status::Ok;
        }
        default: out = 0; return Status::Ok;
    }
}

inline Status int_neg(NumKind k, std::int64_t a, std::int64_t& out) {
    std::int64_t r = 0;
    return int_arith(BinaryOp::Sub, k, 0, a, r) == Status::Ok ? (out = r, Status::Ok) : (out = r, Status::Overflow);
}

inline bool int_less(NumKind k, std::int64_t a, std::int64_t b) {
    if (is_signed(k)) return a < b;
    return static_cast<std::uint64_t>(a) < static_cast<std::uint64_t>(b);
}

// float -> int: truncate toward zero, saturate, NaN -> 0 (DESIGN.md 7.2).
inline std::int64_t float_to_int(NumKind k, double v) {
    if (std::isnan(v)) return 0;
    if (is_signed(k)) {
        auto lo = static_cast<double>(min_of(k));
        auto hi = static_cast<double>(static_cast<std::int64_t>(max_of(k)));
        if (v <= lo) return min_of(k);
        if (v >= hi) return static_cast<std::int64_t>(max_of(k));
        return static_cast<std::int64_t>(v);
    }
    if (v <= 0.0) return 0;
    auto hi = static_cast<double>(max_of(k));
    if (v >= hi) return static_cast<std::int64_t>(max_of(k));
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(v));
}

inline double int_to_double(NumKind k, std::int64_t v) {
    return k == NumKind::U64 ? static_cast<double>(static_cast<std::uint64_t>(v)) : static_cast<double>(v);
}

} // namespace pluma::num

#pragma once

// Pluma FFI (LANGUAGE.md 9.28, milestone 10.5c): calling C functions of
// shared libraries, declared in Pluma with explicit marshaling:
//
//     @extern("libm.so.6", "cbrt") fn cbrt(x: f64) -> f64 {}
//     @extern("", "strlen") fn c_strlen(s: str) -> u64 {}   // "" = this process
//
// Parameter and result types map to C one to one: i8..i64 / u8..u64 to the C
// integer of that width, bool to a C int (0/1), float to C float, f64 to
// double, str to a NUL-terminated `const char*` (valid during the call; a
// returned `const char*` is copied, NULL gives ""). At most 4 parameters.
//
// The binding is carried by the native's name (the "key"), so a compiled
// program (bytecode or an AOT module) binds it again wherever it runs.

#include <optional>
#include <string>
#include <string_view>

#include "pluma/vm.hpp"

namespace pluma {

// "ffi|<library>|<symbol>|<signature>": the signature is one code per
// parameter, then '>' and the result's code (FFI type codes:
// b s i l = i8 i16 i32 i64, B S I L = u8 u16 u32 u64, z = bool, f = float,
// d = f64, p = str, v = no result).
std::string ffi_key(std::string_view library, std::string_view symbol, std::string_view signature);
bool is_ffi_key(std::string_view name);

constexpr std::size_t ffi_max_params = 4;

// Opens the library and finds the symbol of `key`; a native calling it.
std::optional<NativeFn> make_ffi_native(std::string_view key, std::string& error);

} // namespace pluma

#include "pluma/ffi.hpp"

#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <vector>

#include "pluma/value.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <vector>
#elif !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#endif

namespace pluma {

namespace {

// Argument classes: what register a C argument travels in.
enum class Cls : std::uint8_t { Int, F32, F64 };

struct Slot {
    std::int64_t i = 0;
    float f = 0.0f;
    double d = 0.0;
};

Cls cls_of(char code) {
    if (code == 'f') return Cls::F32;
    if (code == 'd') return Cls::F64;
    return Cls::Int;
}

// Calls `fn` with the arguments' C types, chosen one at a time (the C++
// compiler applies the platform's calling convention to each combination).
template <class R, class... Done>
R dispatch(void* fn, const Slot* a, const Cls* c, std::size_t n, Done... done) {
    if constexpr (sizeof...(Done) >= ffi_max_params) {
        (void)a;
        (void)c;
        (void)n;
        R (*f)(Done...) = nullptr;
        std::memcpy(&f, &fn, sizeof f);
        return f(done...);
    } else {
        if (n == 0) {
            R (*f)(Done...) = nullptr;
            std::memcpy(&f, &fn, sizeof f);
            return f(done...);
        }
        switch (c[0]) {
            case Cls::F32: return dispatch<R>(fn, a + 1, c + 1, n - 1, done..., a[0].f);
            case Cls::F64: return dispatch<R>(fn, a + 1, c + 1, n - 1, done..., a[0].d);
            case Cls::Int: break;
        }
        return dispatch<R>(fn, a + 1, c + 1, n - 1, done..., a[0].i);
    }
}

struct Library {
    void* handle = nullptr;
};

std::mutex g_mutex;
std::map<std::string, Library, std::less<>> g_libraries; // never unloaded: natives may point in

void* open_library(const std::string& name, std::string& error) {
    std::lock_guard lock(g_mutex);
    if (auto it = g_libraries.find(name); it != g_libraries.end()) return it->second.handle;
#if defined(__EMSCRIPTEN__)
    (void)name;
    error = "the FFI is not available on the web";
    return nullptr;
#elif defined(_WIN32)
    void* h = name.empty() ? static_cast<void*>(::GetModuleHandleW(nullptr))
                           : static_cast<void*>(::LoadLibraryA(name.c_str()));
    if (h == nullptr) {
        error = std::format("can't load library '{}' (error {})", name, ::GetLastError());
        return nullptr;
    }
#else
    void* h = ::dlopen(name.empty() ? nullptr : name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        const char* why = ::dlerror();
        error = std::format("can't load library '{}': {}", name, why != nullptr ? why : "");
        return nullptr;
    }
#endif
    g_libraries.emplace(name, Library{h});
    return h;
}

void* find_symbol(void* lib, const std::string& symbol) {
#if defined(__EMSCRIPTEN__)
    (void)lib;
    (void)symbol;
    return nullptr;
#elif defined(_WIN32)
    auto* module = static_cast<HMODULE>(lib);
    if (FARPROC p = ::GetProcAddress(module, symbol.c_str())) return reinterpret_cast<void*>(p);
    if (module != ::GetModuleHandleW(nullptr)) return nullptr;
    // "" is the program and the libraries it links (LANGUAGE.md 9.28), as
    // dlopen(nullptr) gives on Linux. A PE only exports its own symbols, so
    // search every loaded module (the C runtime among them), in load order.
    std::vector<HMODULE> modules(256);
    DWORD needed = 0;
    while (::K32EnumProcessModules(::GetCurrentProcess(), modules.data(),
                                   static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed) &&
           needed > modules.size() * sizeof(HMODULE)) {
        modules.resize(needed / sizeof(HMODULE));
    }
    modules.resize(needed / sizeof(HMODULE));
    for (HMODULE m : modules) {
        if (FARPROC p = ::GetProcAddress(m, symbol.c_str())) return reinterpret_cast<void*>(p);
    }
    return nullptr;
#else
    return ::dlsym(lib, symbol.c_str());
#endif
}

// The C integer result, reduced to the declared width.
Value int_result(char code, std::int64_t v) {
    switch (code) {
        case 'b': return Value::integer(NumKind::I8, static_cast<std::int8_t>(v));
        case 's': return Value::integer(NumKind::I16, static_cast<std::int16_t>(v));
        case 'i': return Value::integer(NumKind::I32, static_cast<std::int32_t>(v));
        case 'l': return Value::integer(NumKind::I64, v);
        case 'B': return Value::integer(NumKind::U8, static_cast<std::uint8_t>(v));
        case 'S': return Value::integer(NumKind::U16, static_cast<std::uint16_t>(v));
        case 'I': return Value::integer(NumKind::U32, static_cast<std::uint32_t>(v));
        case 'L': return Value::integer(NumKind::U64, v);
        case 'z': return Value::boolean((v & 0xff) != 0); // a C bool defines its low byte only
        default: return Value{};
    }
}

} // namespace

std::string ffi_key(std::string_view library, std::string_view symbol, std::string_view signature) {
    return std::format("ffi|{}|{}|{}", library, symbol, signature);
}

bool is_ffi_key(std::string_view name) { return name.starts_with("ffi|"); }

std::optional<NativeFn> make_ffi_native(std::string_view key, std::string& error) {
    if (!is_ffi_key(key)) {
        error = std::format("'{}' is not an FFI binding", key);
        return std::nullopt;
    }
    std::string_view rest = key.substr(4);
    const std::size_t a = rest.find('|');
    const std::size_t b = a == std::string_view::npos ? a : rest.find('|', a + 1);
    if (b == std::string_view::npos) {
        error = std::format("malformed FFI binding '{}'", key);
        return std::nullopt;
    }
    const std::string library(rest.substr(0, a));
    const std::string symbol(rest.substr(a + 1, b - a - 1));
    const std::string sig(rest.substr(b + 1));
    const std::size_t arrow = sig.find('>');
    if (arrow == std::string::npos || arrow > ffi_max_params || arrow + 2 != sig.size()) {
        error = std::format("malformed FFI signature '{}'", sig);
        return std::nullopt;
    }
    void* lib = open_library(library, error);
    if (lib == nullptr) return std::nullopt;
    void* fn = find_symbol(lib, symbol);
    if (fn == nullptr) {
        error = std::format("no symbol '{}' in {}", symbol, library.empty() ? "this program" : "'" + library + "'");
        return std::nullopt;
    }
    std::vector<char> params(sig.begin(), sig.begin() + static_cast<std::ptrdiff_t>(arrow));
    const char ret = sig.back();
    return NativeFn([fn, params, ret](NativeCall& call) -> Value {
        Slot slots[ffi_max_params];
        Cls classes[ffi_max_params];
        for (std::size_t i = 0; i < params.size(); ++i) {
            const Value& v = call.args[i];
            classes[i] = cls_of(params[i]);
            switch (params[i]) {
                case 'f': slots[i].f = v.f; break;
                case 'd': slots[i].d = v.d; break;
                case 'z': slots[i].i = v.b ? 1 : 0; break;
                case 'p': {
                    const char* s = v.tag == Tag::Obj ? static_cast<const StrObj*>(v.o)->s.c_str() : "";
                    slots[i].i = static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(s));
                    break;
                }
                default: slots[i].i = v.i; break;
            }
        }
        const std::size_t n = params.size();
        switch (ret) {
            case 'v': dispatch<void>(fn, slots, classes, n); return Value{};
            case 'f': return Value::f32(dispatch<float>(fn, slots, classes, n));
            case 'd': return Value::f64(dispatch<double>(fn, slots, classes, n));
            case 'p': {
                const auto r = dispatch<std::int64_t>(fn, slots, classes, n);
                const char* s = reinterpret_cast<const char*>(static_cast<std::intptr_t>(r));
                Value out = str_value(std::string(s != nullptr ? s : ""));
                retain(out);
                return out;
            }
            default: return int_result(ret, dispatch<std::int64_t>(fn, slots, classes, n));
        }
    });
}

} // namespace pluma

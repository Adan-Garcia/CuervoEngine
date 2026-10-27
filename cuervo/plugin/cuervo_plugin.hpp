#pragma once

// C++ wrapper over the plugin ABI (cuervo/plugin/cuervo_plugin.h, DESIGN.md
// 4.18, docs/PLUGINS.md). Header-only: a plugin includes it and links nothing
// of the engine.
//
//     #include "cuervo/plugin/cuervo_plugin.hpp"
//
//     CUERVO_PLUGIN(orbit) {
//         // `orbit.angle_at`, declared `@native fn angle_at(t: float, speed: float) -> float {}`
//         plugin.function("angle_at", [](float t, float speed) { return t * speed; });
//         // a method: `self` comes first
//         plugin.function("Orbiter.radius_of", [](cuervo::plugin::Object self) {
//             return self.get_float("radius");
//         });
//     }
//
// Argument and result types follow the Pluma declaration: `int` is int32_t,
// `i64` int64_t, `float` float, `f64` double, `bool` bool, `str` and `name`
// std::string_view or std::string, `vec2`..`vec4` Vec<2>..Vec<4>, a class or
// struct cuervo::plugin::Object. A C++ exception becomes a script panic.

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "cuervo/plugin/cuervo_plugin.h"

namespace cuervo::plugin {

template <std::size_t N>
struct Vec {
    std::array<float, N> c{};
    float& operator[](std::size_t i) { return c[i]; }
    float operator[](std::size_t i) const { return c[i]; }
};
using Vec2 = Vec<2>;
using Vec3 = Vec<3>;
using Vec4 = Vec<4>;

// A script object passed to a native. Valid during the call.
class Object {
public:
    Object() = default;
    Object(const cuervo_host* host, cuervo_object* object) : host_(host), object_(object) {}

    explicit operator bool() const { return object_ != nullptr; }
    cuervo_object* raw() const { return object_; }
    std::string_view type() const { return object_ ? host_->object_type(object_) : std::string_view(); }

    double get_float(const char* field) const {
        double v = 0.0;
        host_->object_get_float(object_, field, &v);
        return v;
    }
    bool set_float(const char* field, double v) const { return host_->object_set_float(object_, field, v) == 0; }
    std::int64_t get_int(const char* field) const {
        std::int64_t v = 0;
        host_->object_get_int(object_, field, &v);
        return v;
    }
    bool set_int(const char* field, std::int64_t v) const { return host_->object_set_int(object_, field, v) == 0; }
    template <std::size_t N>
    Vec<N> get_vec(const char* field) const {
        Vec<N> out;
        std::uint32_t n = 0;
        host_->object_get_vec(object_, field, out.c.data(), N, &n);
        return out;
    }
    template <std::size_t N>
    bool set_vec(const char* field, const Vec<N>& v) const {
        return host_->object_set_vec(object_, field, v.c.data(), N) == 0;
    }

    // Host data (freed by the class's finalizer, Registrar::finalizer).
    template <class T>
    T* native() const {
        return static_cast<T*>(host_->object_native(object_));
    }
    void set_native(void* data) const { host_->object_set_native(object_, data); }

private:
    const cuervo_host* host_ = nullptr;
    cuervo_object* object_ = nullptr;
};

// One native call: raw access to the arguments and the result.
class Call {
public:
    Call(const cuervo_host* host, cuervo_call* call) : host_(host), call_(call) {}

    std::uint32_t size() const { return host_->arg_count(call_); }
    std::int64_t integer(std::uint32_t i) const { return host_->arg_int(call_, i); }
    double number(std::uint32_t i) const { return host_->arg_float(call_, i); }
    bool boolean(std::uint32_t i) const { return host_->arg_bool(call_, i) != 0; }
    bool is_null(std::uint32_t i) const { return host_->arg_is_null(call_, i) != 0; }
    std::string_view str(std::uint32_t i) const {
        std::size_t n = 0;
        const char* s = host_->arg_str(call_, i, &n);
        return {s, n};
    }
    template <std::size_t N>
    Vec<N> vec(std::uint32_t i) const {
        Vec<N> out;
        host_->arg_vec(call_, i, out.c.data(), N);
        return out;
    }
    Object object(std::uint32_t i) const { return Object(host_, host_->arg_object(call_, i)); }

    void ret(bool v) const { host_->ret_bool(call_, v ? 1 : 0); }
    void ret(std::int32_t v) const { host_->ret_i32(call_, v); }
    void ret(std::int64_t v) const { host_->ret_i64(call_, v); }
    void ret(float v) const { host_->ret_f32(call_, v); }
    void ret(double v) const { host_->ret_f64(call_, v); }
    void ret(std::string_view v) const { host_->ret_str(call_, v.data(), v.size()); }
    void ret(const std::string& v) const { host_->ret_str(call_, v.data(), v.size()); }
    void ret(const char* v) const { ret(std::string_view(v)); }
    template <std::size_t N>
    void ret(const Vec<N>& v) const {
        host_->ret_vec(call_, v.c.data(), static_cast<std::uint32_t>(N));
    }
    void ret(const Object& o) const { host_->ret_object(call_, o.raw()); }

    void panic(const char* message) const { host_->panic(call_, message); }
    const cuervo_host* host() const { return host_; }

private:
    const cuervo_host* host_;
    cuervo_call* call_;
};

namespace detail {

template <class T>
using bare = std::remove_cvref_t<T>;

template <class T>
struct is_vec : std::false_type {};
template <std::size_t N>
struct is_vec<Vec<N>> : std::true_type {
    static constexpr std::size_t size = N;
};

template <class T>
T arg(const Call& c, std::uint32_t i) {
    using U = bare<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return c.boolean(i);
    } else if constexpr (std::is_integral_v<U>) {
        return static_cast<U>(c.integer(i));
    } else if constexpr (std::is_floating_point_v<U>) {
        return static_cast<U>(c.number(i));
    } else if constexpr (std::is_same_v<U, std::string_view>) {
        return c.str(i);
    } else if constexpr (std::is_same_v<U, std::string>) {
        return std::string(c.str(i));
    } else if constexpr (std::is_same_v<U, Object>) {
        return c.object(i);
    } else if constexpr (is_vec<U>::value) {
        return c.vec<is_vec<U>::size>(i);
    } else {
        static_assert(sizeof(U) == 0, "unsupported native argument type");
    }
}

template <class Sig>
struct traits;
template <class R, class... A>
struct traits<R (*)(A...)> {
    using ret = R;
    using args = std::tuple<A...>;
};
template <class C, class R, class... A>
struct traits<R (C::*)(A...) const> : traits<R (*)(A...)> {};
template <class C, class R, class... A>
struct traits<R (C::*)(A...)> : traits<R (*)(A...)> {};

template <class F>
struct fn_traits : traits<decltype(&F::operator())> {};
template <class R, class... A>
struct fn_traits<R (*)(A...)> : traits<R (*)(A...)> {};

struct HolderBase {
    virtual ~HolderBase() = default;
};

// Registered callables live as long as the plugin library.
inline std::vector<std::unique_ptr<HolderBase>>& holders() {
    static std::vector<std::unique_ptr<HolderBase>> h;
    return h;
}

template <class F>
struct Holder : HolderBase {
    Holder(const cuervo_host* h, F fn) : host(h), f(std::move(fn)) {}
    const cuervo_host* host;
    F f;
};

template <class F, class Args, std::size_t... I>
decltype(auto) invoke(F& f, const Call& c, std::index_sequence<I...>) {
    return f(arg<std::tuple_element_t<I, Args>>(c, static_cast<std::uint32_t>(I))...);
}

template <class F>
void trampoline(cuervo_call* raw, void* user) {
    auto* h = static_cast<Holder<F>*>(user);
    Call call(h->host, raw);
    try {
        if constexpr (std::is_invocable_v<F&, const Call&>) {
            h->f(call);
        } else {
            using T = fn_traits<F>;
            using Args = typename T::args;
            constexpr std::size_t n = std::tuple_size_v<Args>;
            if (call.size() < n) {
                call.panic("native called with too few arguments");
                return;
            }
            if constexpr (std::is_void_v<typename T::ret>) {
                invoke<F, Args>(h->f, call, std::make_index_sequence<n>{});
            } else {
                call.ret(invoke<F, Args>(h->f, call, std::make_index_sequence<n>{}));
            }
        }
    } catch (const std::exception& e) {
        call.panic(e.what());
    } catch (...) {
        call.panic("unknown C++ exception in a plugin native");
    }
}

template <class F>
struct ImportHolder : HolderBase {
    explicit ImportHolder(const cuervo_host* h, F fn) : host(h), f(std::move(fn)) {}
    const cuervo_host* host;
    F f;
};

template <class F>
int import_trampoline(void* user, const char* path, const std::uint8_t* data, std::size_t size, cuervo_buffer* out) {
    auto* h = static_cast<ImportHolder<F>*>(user);
    try {
        std::vector<std::uint8_t> bytes = h->f(std::string_view(path), std::span<const std::uint8_t>(data, size));
        h->host->buffer_write(out, bytes.data(), bytes.size());
        return 0;
    } catch (const std::exception& e) {
        const std::string_view m = e.what();
        h->host->buffer_write(out, reinterpret_cast<const std::uint8_t*>(m.data()), m.size());
        return 1;
    }
}

} // namespace detail

// What a plugin registers, during CUERVO_PLUGIN's body. Names are relative to
// the plugin's namespace: "value" registers "<plugin>.value".
class Registrar {
public:
    Registrar(const cuervo_host* host, cuervo_registrar* reg, std::string_view name)
        : host_(host), reg_(reg), prefix_(std::string(name) + ".") {}

    // A native implemented by `f`: a callable taking the declaration's
    // arguments (converted), or `const Call&` for raw access.
    template <class F>
    void function(std::string_view name, F f) {
        auto holder = std::make_unique<detail::Holder<F>>(host_, std::move(f));
        void* user = holder.get();
        detail::holders().push_back(std::move(holder));
        const std::string full = prefix_ + std::string(name);
        if (host_->register_native(reg_, full.c_str(), &detail::trampoline<F>, user) != 0) failed_ = true;
    }

    // Deletes a `T*` set with Object::set_native when an instance of `cls`
    // (relative to the namespace) dies.
    template <class T>
    void finalizer(std::string_view cls) {
        const std::string full = prefix_ + std::string(cls);
        if (host_->register_finalizer(reg_, full.c_str(), [](void* p) { delete static_cast<T*>(p); }) != 0) {
            failed_ = true;
        }
    }

    // Converts `extension` files to `output_extension` (an engine format).
    // `f(path, bytes) -> std::vector<std::uint8_t>`; throw to fail.
    template <class F>
    void importer(const char* extension, const char* output_extension, F f) {
        auto holder = std::make_unique<detail::ImportHolder<F>>(host_, std::move(f));
        void* user = holder.get();
        detail::holders().push_back(std::move(holder));
        if (host_->register_importer(reg_, extension, output_extension, &detail::import_trampoline<F>, user) != 0) {
            failed_ = true;
        }
    }

    void log(const char* message, int level = CUERVO_LOG_INFO) const { host_->log(level, message); }
    const cuervo_host* host() const { return host_; }
    bool failed() const { return failed_; }

private:
    const cuervo_host* host_;
    cuervo_registrar* reg_;
    std::string prefix_;
    bool failed_ = false;
};

} // namespace cuervo::plugin

// Defines the plugin's entry point; the body registers through `plugin`.
#define CUERVO_PLUGIN(name)                                                                                      \
    static void cuervo_plugin_register_##name(::cuervo::plugin::Registrar& plugin);                             \
    extern "C" CUERVO_PLUGIN_EXPORT int cuervo_plugin_entry_##name(const cuervo_host* host,                     \
                                                                    cuervo_registrar* reg) {                     \
        if (host == nullptr || host->abi_version != CUERVO_PLUGIN_ABI_VERSION) return 1;                         \
        ::cuervo::plugin::Registrar registrar(host, reg, #name);                                                 \
        try {                                                                                                     \
            cuervo_plugin_register_##name(registrar);                                                            \
        } catch (...) {                                                                                           \
            return 2;                                                                                             \
        }                                                                                                         \
        return registrar.failed() ? 3 : 0;                                                                       \
    }                                                                                                             \
    static void cuervo_plugin_register_##name(::cuervo::plugin::Registrar& plugin)

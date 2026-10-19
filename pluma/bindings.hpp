#pragma once

// Native binding layer (DESIGN.md 3: "the engine registers its API with the
// language"). Generic: nothing here knows about the engine.
//
// A host API is described in the language itself, in prelude sources whose
// functions and methods carry `@native`:
//
//     class Canvas {
//         @native fn circle(center: vec2, radius: float, fill: Color? = null) {}
//     }
//     @native fn sin_deg(x: float) -> float {}
//
// so its static types, default and named arguments, classes, inheritance and
// statics all come from the ordinary checker. The body of a `@native`
// function must be empty; the compiler turns it into a call of the host
// function registered under the qualified name ("Canvas.circle",
// "sin_deg"). The checker reports a `@native` declaration with no host
// function bound.
//
// Host objects: an instance of a class with native methods can carry host
// data in InstanceObj::native (typically set by a native `init`); the class's
// finalizer (`add_finalizer`) frees it when the instance dies.
//
// Properties are plain fields or statics set by the host (Session::set_global),
// or native getter methods.

#include <string>
#include <string_view>
#include <vector>

#include "pluma/value.hpp"
#include "pluma/vm.hpp"

namespace pluma {

class Bindings {
public:
    struct Prelude {
        std::string path;
        std::string source;
    };
    struct Native {
        std::string name;
        NativeFn fn;
    };
    struct Finalizer {
        std::string class_name;
        void (*fn)(void*) = nullptr;
    };

    // A module of declarations compiled before any user code.
    void add_prelude(std::string path, std::string source) {
        preludes_.push_back({std::move(path), std::move(source)});
    }
    void add_native(std::string qualified_name, NativeFn fn) { natives_.push_back({std::move(qualified_name), std::move(fn)}); }
    void add_finalizer(std::string class_name, void (*fn)(void*)) { finalizers_.push_back({std::move(class_name), fn}); }

    const std::vector<Prelude>& preludes() const { return preludes_; }
    const std::vector<Native>& natives() const { return natives_; }
    const std::vector<Finalizer>& finalizers() const { return finalizers_; }

private:
    std::vector<Prelude> preludes_;
    std::vector<Native> natives_;
    std::vector<Finalizer> finalizers_;
};

// ---- helpers for writing native functions

namespace native {

inline float f32(const NativeCall& c, std::uint32_t i) { return c.args[i].f; }
inline std::int64_t integer(const NativeCall& c, std::uint32_t i) { return c.args[i].i; }
inline bool boolean(const NativeCall& c, std::uint32_t i) { return c.args[i].b; }
inline std::string_view str(const NativeCall& c, std::uint32_t i) {
    return static_cast<const StrObj*>(c.args[i].o)->s;
}
// The text of a `name` argument.
inline std::string_view name(const NativeCall& c, std::uint32_t i) {
    return c.vm.name_text(static_cast<std::uint32_t>(c.args[i].i));
}
inline const float* vec(const NativeCall& c, std::uint32_t i) { return vec_data(c.args[i]); }
// A class instance or struct argument (null for a null reference).
inline InstanceObj* object(const NativeCall& c, std::uint32_t i) {
    return c.args[i].is_obj() ? static_cast<InstanceObj*>(c.args[i].o) : nullptr;
}
// `self` of a method, which must be non-null.
inline InstanceObj* self(const NativeCall& c) { return static_cast<InstanceObj*>(c.args[0].o); }

// Results: natives return owned values.
inline Value owned(Value v) {
    retain(v);
    return v;
}
inline Value str_result(std::string s) { return owned(str_value(std::move(s))); }
inline Value vec2_result(float x, float y) { return owned(vec2_value(x, y)); }

} // namespace native

} // namespace pluma

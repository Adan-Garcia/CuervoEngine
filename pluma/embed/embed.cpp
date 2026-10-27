// The Pluma embedding API (pluma/embed/pluma_embed.h, DESIGN.md 4.18): a C
// layer over Session and Bindings. Values cross by the declared Pluma types,
// so a host passes PLUMA_INT for any integer and never names a width.

#include "pluma/embed/pluma_embed.h"

#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "pluma/bindings.hpp"
#include "pluma/session.hpp"

namespace {

using pluma::NumKind;
using pluma::Tag;
using pluma::Type;
using pluma::TypeTag;
using pluma::Value;

} // namespace

struct pluma_vm {
    pluma::Bindings bindings;
    std::unique_ptr<pluma::Session> session;
    std::string error;
    // Strings handed to the host; dropped when an outermost call starts.
    std::deque<std::string> scratch;
    int depth = 0; // script calls in progress (natives run inside them)
    bool panicked = false;
    std::string panic_message;
};

namespace {

// The checked function of a VM proto, when it is one (protos and functions
// share indices, pluma/sema.hpp Function::index).
const pluma::Function* function_of(const pluma_vm* vm, std::uint32_t proto) {
    const auto& fns = vm->session->program().functions;
    if (proto >= fns.size()) return nullptr;
    const pluma::Function* f = fns[proto].get();
    const auto& protos = vm->session->vm().protos;
    return proto < protos.size() && protos[proto]->key == f->name ? f : nullptr;
}

const pluma::Function* function_named(const pluma_vm* vm, std::string_view name) {
    for (const auto& f : vm->session->program().functions) {
        if (f->name == name) return f.get();
    }
    return nullptr;
}

void begin(pluma_vm* vm) {
    if (vm->depth == 0) vm->scratch.clear();
}

const char* keep(pluma_vm* vm, std::string s) { return vm->scratch.emplace_back(std::move(s)).c_str(); }

int fail(pluma_vm* vm, std::string message) {
    vm->error = std::move(message);
    return 0;
}

int succeed(pluma_vm* vm) {
    vm->error.clear();
    return 1;
}

// A host value as a Pluma value of type `t` (null: by the value's own kind).
// Objects come back retained; release them after use.
bool to_pluma(pluma_vm* vm, const pluma_value& in, const Type* t, Value& out, std::string& error) {
    if (t != nullptr && t->tag == TypeTag::Nullable) {
        if (in.type == PLUMA_NULL || in.type == PLUMA_NONE) {
            out = Value::null();
            return true;
        }
        t = t->args[0];
    }
    const bool num = t != nullptr && t->is_num();
    switch (in.type) {
        case PLUMA_NONE: out = t != nullptr && t->is_void() ? Value{} : Value::null(); return true;
        case PLUMA_NULL: out = Value::null(); return true;
        case PLUMA_BOOL: out = Value::boolean(in.as.boolean != 0); return true;
        case PLUMA_INT:
            if (num && t->is_floating()) {
                const double d = static_cast<double>(in.as.integer);
                out = t->num == NumKind::F32 ? Value::f32(static_cast<float>(d)) : Value::f64(d);
            } else {
                out = Value::integer(num ? t->num : NumKind::I32, in.as.integer);
            }
            return true;
        case PLUMA_FLOAT:
            if (num && t->is_int()) {
                out = Value::integer(t->num, static_cast<std::int64_t>(in.as.number));
            } else if (num && t->num == NumKind::F64) {
                out = Value::f64(in.as.number);
            } else {
                out = Value::f32(static_cast<float>(in.as.number));
            }
            return true;
        case PLUMA_STR: {
            const char* s = in.as.str != nullptr ? in.as.str : "";
            const std::size_t n = in.length != 0 ? in.length : std::strlen(s);
            if (t != nullptr && t->tag == TypeTag::Name) {
                out = Value{};
                out.tag = Tag::Name;
                out.i = vm->session->vm().intern(std::string_view(s, n));
            } else {
                out = pluma::str_value(std::string(s, n));
                pluma::retain(out);
            }
            return true;
        }
        case PLUMA_VEC:
            if (in.length == 0 || in.length > 4) {
                error = "a PLUMA_VEC value has 1 to 4 components";
                return false;
            }
            out = pluma::vec_value(in.as.vec, in.length);
            pluma::retain(out);
            return true;
        case PLUMA_OBJECT:
            if (in.as.object == nullptr) {
                out = Value::null();
                return true;
            }
            out = Value::object(reinterpret_cast<pluma::Object*>(in.as.object));
            pluma::retain(out);
            return true;
        case PLUMA_OTHER: break;
    }
    error = "this value can't be passed through the embedding API";
    return false;
}

// A Pluma value for the host. `owned`: objects are retained for the host
// and strings copied into the VM's scratch; otherwise both are borrowed.
pluma_value to_host(pluma_vm* vm, const Value& v, bool owned) {
    pluma_value out = pluma_none();
    if (pluma::is_vec(v)) {
        const std::uint32_t n = pluma::vec_size(v);
        if (n > 4) {
            out.type = PLUMA_OTHER;
            return out;
        }
        out.type = PLUMA_VEC;
        out.length = n;
        std::memcpy(out.as.vec, pluma::vec_data(v), n * sizeof(float));
        return out;
    }
    switch (v.tag) {
        case Tag::Nil: return out;
        case Tag::Null: return pluma_null();
        case Tag::Bool: return pluma_bool(v.b ? 1 : 0);
        case Tag::F32: return pluma_float(static_cast<double>(v.f));
        case Tag::F64: return pluma_float(v.d);
        case Tag::Name: {
            const std::string& s = vm->session->vm().name_text(static_cast<std::uint32_t>(v.i));
            out = pluma_str(s.c_str());
            out.length = static_cast<std::uint32_t>(s.size());
            return out;
        }
        case Tag::Enum:
            out.type = PLUMA_OTHER;
            out.as.integer = v.i; // the discriminant
            return out;
        case Tag::Obj: {
            if (v.o->kind == pluma::ObjKind::Str) {
                const std::string& s = static_cast<const pluma::StrObj*>(v.o)->s;
                out = pluma_str(owned ? keep(vm, s) : s.c_str());
                out.length = static_cast<std::uint32_t>(s.size());
                return out;
            }
            if (owned) pluma::retain(v);
            return pluma_object_value(reinterpret_cast<pluma_object*>(v.o));
        }
        default:
            if (pluma::is_num_tag(v.tag)) return pluma_int(v.i);
            out.type = PLUMA_OTHER;
            return out;
    }
}

int check_loaded(pluma_vm* vm) {
    if (vm->session == nullptr) return fail(vm, "nothing is loaded");
    return 1;
}

bool start_session(pluma_vm* vm) {
    if (vm->session != nullptr) return true;
    vm->session = std::make_unique<pluma::Session>(pluma::VmOptions{}, &vm->bindings);
    const pluma::SessionResult& pre = vm->session->prelude_result();
    if (!pre.ok) {
        vm->error = pre.rendered.empty() && pre.panic ? pre.panic->render() : pre.rendered;
        return false;
    }
    return true;
}

int finish(pluma_vm* vm, const pluma::SessionResult& r) {
    if (r.ok) return succeed(vm);
    if (!r.rendered.empty()) return fail(vm, r.rendered);
    return fail(vm, r.panic ? r.panic->render() : "the script failed");
}

// Converts `args` by `fn`'s parameter types and runs `body` with them,
// releasing the converted values after.
template <class Body>
int with_args(pluma_vm* vm, const pluma::Function* fn, const pluma_value* args, std::uint32_t count,
              std::vector<Value>& values, Body body) {
    if (fn != nullptr && fn->params.size() != count) {
        return fail(vm, std::format("'{}' takes {} arguments, not {}", fn->name, fn->params.size(), count));
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        Value v;
        std::string why;
        if (!to_pluma(vm, args[i], fn != nullptr ? fn->params[i].type : nullptr, v, why)) {
            for (const Value& done : values) pluma::release(done);
            return fail(vm, std::format("argument {}: {}", i + 1, why));
        }
        values.push_back(v);
    }
    ++vm->depth;
    const pluma::RunResult run = body();
    --vm->depth;
    for (const Value& v : values) pluma::release(v);
    if (!run.ok) return fail(vm, run.panic ? run.panic->render() : "the call failed");
    return succeed(vm);
}

int deliver(pluma_vm* vm, Value r, pluma_value* result) {
    if (result != nullptr) *result = to_host(vm, r, true);
    pluma::release(r);
    return 1;
}

} // namespace

extern "C" {

uint32_t pluma_embed_version(void) { return PLUMA_EMBED_API_VERSION; }

pluma_vm* pluma_new(void) { return new pluma_vm(); }

void pluma_free(pluma_vm* vm) { delete vm; }

int pluma_add_prelude(pluma_vm* vm, const char* path, const char* source) {
    if (vm->session != nullptr) return fail(vm, "preludes must be added before the first load");
    vm->bindings.add_prelude(path != nullptr ? path : "prelude.plm", source != nullptr ? source : "");
    return 1;
}

int pluma_add_native(pluma_vm* vm, const char* name, pluma_native_fn fn, void* user) {
    if (vm->session != nullptr) return fail(vm, "natives must be added before the first load");
    if (name == nullptr || fn == nullptr) return fail(vm, "a native needs a name and a function");
    std::string key(name);
    // The declaration's return type, looked up on the first call.
    auto ret = std::make_shared<const Type*>(nullptr);
    vm->bindings.add_native(key, [vm, fn, user, key, ret](pluma::NativeCall& nc) -> Value {
        if (*ret == nullptr) {
            const pluma::Function* f = function_named(vm, key);
            *ret = f != nullptr ? f->ret : nullptr;
        }
        std::vector<pluma_value> args(nc.count);
        for (std::uint32_t i = 0; i < nc.count; ++i) args[i] = to_host(vm, nc.args[i], false);
        vm->panicked = false;
        const pluma_value r = fn(vm, args.data(), nc.count, user);
        // Panics unwind only here, on the C++ side of the callback.
        if (vm->panicked) {
            vm->panicked = false;
            nc.vm.panic(std::move(vm->panic_message));
        }
        if (*ret != nullptr && (*ret)->is_void()) return Value{};
        Value out;
        std::string why;
        if (!to_pluma(vm, r, *ret, out, why)) nc.vm.panic(std::format("native '{}' returned {}", key, why));
        return out; // to_pluma retained it: the caller owns it
    });
    return 1;
}

void pluma_panic(pluma_vm* vm, const char* message) {
    vm->panicked = true;
    vm->panic_message = message != nullptr ? message : "panic in a native";
}

int pluma_load_files(pluma_vm* vm, const char* const* paths, const char* const* sources, size_t count) {
    begin(vm);
    if (!start_session(vm)) return 0;
    std::vector<pluma::SourceFile::Part> parts;
    for (std::size_t i = 0; i < count; ++i) {
        parts.push_back({paths[i] != nullptr ? paths[i] : "script.plm", sources[i] != nullptr ? sources[i] : "", {}, false});
    }
    return finish(vm, vm->session->load_files(parts));
}

int pluma_load(pluma_vm* vm, const char* path, const char* source) { return pluma_load_files(vm, &path, &source, 1); }

int pluma_run(pluma_vm* vm, const char* path, const char* source) {
    begin(vm);
    if (!start_session(vm)) return 0;
    return finish(vm, vm->session->run_module(path != nullptr ? path : "script.plm", source != nullptr ? source : ""));
}

const char* pluma_error(const pluma_vm* vm) { return vm->error.c_str(); }

int pluma_call(pluma_vm* vm, const char* function, const pluma_value* args, uint32_t count, pluma_value* result) {
    begin(vm);
    if (check_loaded(vm) == 0) return 0;
    pluma::VM& v = vm->session->vm();
    const auto proto = v.proto_by_key(function != nullptr ? function : "");
    if (!proto) return fail(vm, std::format("no function '{}'", function != nullptr ? function : ""));
    std::vector<Value> values;
    Value r;
    if (with_args(vm, function_of(vm, *proto), args, count, values,
                  [&] { return v.call(*proto, values, &r); }) == 0) {
        return 0;
    }
    return deliver(vm, r, result);
}

int pluma_call_method(pluma_vm* vm, pluma_object* self, const char* method, const pluma_value* args, uint32_t count,
                      pluma_value* result) {
    begin(vm);
    if (check_loaded(vm) == 0) return 0;
    if (self == nullptr) return fail(vm, "pluma_call_method on a null object");
    auto* obj = reinterpret_cast<pluma::Object*>(self);
    const std::string name = method != nullptr ? method : "";
    const pluma::Function* fn = nullptr;
    if (obj->kind == pluma::ObjKind::Instance) {
        const pluma::RecordRT* rt = static_cast<const pluma::InstanceObj*>(obj)->type;
        if (auto it = rt->methods.find(name); it != rt->methods.end()) fn = function_of(vm, it->second);
    }
    const Value self_value = Value::object(obj);
    std::vector<Value> values;
    Value r;
    if (with_args(vm, fn, args, count, values,
                  [&] { return vm->session->vm().call_method(self_value, name, values, &r); }) == 0) {
        return 0;
    }
    return deliver(vm, r, result);
}

int pluma_get_global(pluma_vm* vm, const char* name, pluma_value* out) {
    begin(vm);
    if (check_loaded(vm) == 0) return 0;
    const std::string_view key = name != nullptr ? name : "";
    if (!vm->session->vm().global_index(key)) return fail(vm, std::format("no global '{}'", key));
    if (out != nullptr) *out = to_host(vm, vm->session->global(key), true);
    return succeed(vm);
}

int pluma_set_global(pluma_vm* vm, const char* name, pluma_value value) {
    begin(vm);
    if (check_loaded(vm) == 0) return 0;
    const std::string_view key = name != nullptr ? name : "";
    const Type* type = nullptr;
    const auto& globals = vm->session->program().global_vars;
    for (const auto& g : globals) {
        if (g->qualified == key) type = g->type;
    }
    Value v;
    std::string why;
    if (!to_pluma(vm, value, type, v, why)) return fail(vm, why);
    const bool ok = vm->session->set_global(key, v);
    pluma::release(v);
    return ok ? succeed(vm) : fail(vm, std::format("no global '{}'", key));
}

void pluma_retain(pluma_vm*, pluma_object* object) {
    if (object != nullptr) pluma::retain(Value::object(reinterpret_cast<pluma::Object*>(object)));
}

void pluma_release(pluma_vm*, pluma_object* object) {
    if (object != nullptr) pluma::release(Value::object(reinterpret_cast<pluma::Object*>(object)));
}

const char* pluma_object_class(pluma_vm*, pluma_object* object) {
    auto* obj = reinterpret_cast<pluma::Object*>(object);
    if (obj == nullptr || obj->kind != pluma::ObjKind::Instance) return "";
    return static_cast<const pluma::InstanceObj*>(obj)->type->name.c_str();
}

const char* pluma_to_string(pluma_vm* vm, pluma_value value) {
    begin(vm);
    if (vm->session == nullptr) return "";
    Value v;
    std::string why;
    if (!to_pluma(vm, value, nullptr, v, why)) return "";
    std::string text;
    ++vm->depth;
    vm->session->vm().protect([&] { text = vm->session->vm().format(v, false); });
    --vm->depth;
    pluma::release(v);
    return keep(vm, std::move(text));
}

int pluma_tick(pluma_vm* vm) {
    begin(vm);
    if (check_loaded(vm) == 0) return 0;
    ++vm->depth;
    const pluma::RunResult r = vm->session->vm().tick_coroutines();
    --vm->depth;
    return r.ok ? succeed(vm) : fail(vm, r.panic ? r.panic->render() : "a coroutine failed");
}

int pluma_collect(pluma_vm* vm, uint64_t budget) {
    begin(vm);
    if (check_loaded(vm) == 0) return 0;
    const pluma::RunResult r = vm->session->vm().collect_cycles(budget);
    return r.ok ? succeed(vm) : fail(vm, r.panic ? r.panic->render() : "the collector failed");
}

} // extern "C"

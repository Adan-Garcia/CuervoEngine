#include "pluma/aot.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "pluma/numeric.hpp"
#include "pluma/tir.hpp"

namespace pluma {

namespace {

// C ABI structures mirrored in the generated C translation unit.
struct AotCtx {
    Value* R = nullptr;
    const Value* K = nullptr;
    Value* G = nullptr;
    std::uint32_t* pc_ptr = nullptr;
    std::uint8_t wrap_overflow = 0;
};

struct AotFieldDesc {
    const char* name;
    const char* type;
    std::uint8_t exported;
    std::uint8_t weak;
};

struct AotMethodDesc {
    const char* name;
    std::uint32_t proto;
};

struct AotRecordDesc {
    const char* name;
    std::uint8_t is_struct;
    std::int32_t base_index;
    std::uint32_t field_init;
    std::uint8_t gc_flags;
    std::uint32_t vtable_count;
    const std::uint32_t* vtable;
    std::uint32_t method_count;
    const AotMethodDesc* methods;
    std::uint32_t field_count;
    const AotFieldDesc* fields;
};

struct AotVariantDesc {
    const char* name;
    std::int64_t discriminant;
    std::uint32_t field_count;
    const char* const* field_names;
};

struct AotEnumDesc {
    const char* name;
    std::uint8_t plain;
    std::uint8_t gc_flags;
    std::uint32_t variant_count;
    const AotVariantDesc* variants;
};

struct AotGlobalDesc {
    const char* name;
    const char* type;
};

struct AotConstDesc {
    std::uint8_t tag;
    std::uint32_t aux;
    std::int64_t bits;
    const char* str;
};

struct AotLocalDesc {
    const char* name;
    const char* type;
    std::uint16_t reg;
    std::uint32_t begin;
    std::uint32_t end;
};

struct AotSpanLocDesc {
    const char* file;
    std::uint32_t line;
    std::uint32_t column;
    const char* text;
};

struct AotProtoDesc {
    const char* name;
    const char* key;
    std::uint32_t index;
    std::uint16_t param_count;
    std::uint16_t reg_count;
    std::uint16_t capture_count;
    std::uint32_t const_count;
    const AotConstDesc* consts;
    std::uint32_t local_count;
    const AotLocalDesc* locals;
    std::uint32_t span_count;
    const std::uint32_t* span_indices;
    Proto::AotFn fn;
};

struct AotImageDesc {
    std::uint32_t name_count;
    const char* const* names;
    std::uint32_t class_count;
    const AotRecordDesc* classes;
    std::uint32_t struct_count;
    const AotRecordDesc* structs;
    std::uint32_t enum_count;
    const AotEnumDesc* enums;
    std::uint32_t global_count;
    const AotGlobalDesc* globals;
    std::uint32_t native_count;
    const char* const* native_names;
    std::uint32_t* native_ids;
    std::uint32_t span_pool_count;
    const AotSpanLocDesc* span_pool;
    std::uint32_t proto_count;
    const AotProtoDesc* protos;
    std::uint32_t chunk_count;
    const std::uint32_t* chunks;
};

BinaryOp arith_binop(Op op) {
    switch (op) {
        case Op::Add: return BinaryOp::Add;
        case Op::Sub: return BinaryOp::Sub;
        case Op::Mul: return BinaryOp::Mul;
        case Op::Div: return BinaryOp::Div;
        case Op::Rem: return BinaryOp::Rem;
        case Op::BAnd: return BinaryOp::BitAnd;
        case Op::BOr: return BinaryOp::BitOr;
        case Op::BXor: return BinaryOp::BitXor;
        case Op::Shl: return BinaryOp::Shl;
        case Op::Shr: return BinaryOp::Shr;
        default: return BinaryOp::Add;
    }
}

std::string_view arith_symbol(Op op) {
    switch (op) {
        case Op::Add: return "+";
        case Op::Sub:
        case Op::Neg: return "-";
        case Op::Mul: return "*";
        case Op::Div: return "/";
        case Op::Rem: return "%";
        default: return "?";
    }
}

float scalar_op(Op op, float a, float b) {
    switch (op) {
        case Op::Add: return a + b;
        case Op::Sub: return a - b;
        case Op::Mul: return a * b;
        case Op::Div: return a / b;
        case Op::Rem: return std::fmod(a, b);
        default: return 0.0f;
    }
}

constexpr std::uint32_t max_vec = 64;

Value vec_arith(Op op, std::uint8_t k, const Value& a, const Value& b) {
    float out[max_vec];
    if (k == opk::Vec) {
        std::uint32_t n = vec_size(a);
        const float* x = vec_data(a);
        const float* y = vec_data(b);
        for (std::uint32_t i = 0; i < n; ++i) out[i] = scalar_op(op, x[i], y[i]);
        return vec_value(out, n);
    }
    if (k == opk::VecS) {
        std::uint32_t n = vec_size(a);
        const float* x = vec_data(a);
        float s = b.f;
        for (std::uint32_t i = 0; i < n; ++i) out[i] = scalar_op(op, x[i], s);
        return vec_value(out, n);
    }
    std::uint32_t n = vec_size(b);
    float s = a.f;
    const float* y = vec_data(b);
    for (std::uint32_t i = 0; i < n; ++i) out[i] = scalar_op(op, s, y[i]);
    return vec_value(out, n);
}

bool vec_equal(const Value& a, const Value& b) {
    std::uint32_t n = vec_size(a);
    const float* x = vec_data(a);
    const float* y = vec_data(b);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) return false;
    }
    return true;
}

Value checked_val(const Value& v) {
    if (v.tag == Tag::Obj && v.ref == 0 && is_manual(v.o)) return manual_ref(v.o);
    return v;
}

using AK = tir::AwaitKind;

} // namespace

// Runtime helpers exposed to generated C code via the function-pointer table.
struct AotRuntime {
    static void enter(VM& vm, AotCtx* ctx) {
        VM::Frame& f = vm.frames_.back();
        ctx->R = vm.stack_.data() + f.base;
        ctx->K = f.fn->consts.data();
        ctx->G = vm.globals.data();
        ctx->pc_ptr = &f.pc;
        ctx->wrap_overflow = vm.options_.wrap_overflow ? 1 : 0;
    }

    static void free_obj(Object* o) { free_object(o); }
    static void root_obj(Object* o) { possible_root(o); }

    static std::string span_text(VM& vm) {
        const VM::Frame& f = vm.frames_.back();
        return std::string(f.fn->slice_pc(f.pc - 1));
    }

    [[noreturn]] static void null_panic(VM& vm) {
        std::string text = span_text(vm);
        if (text.empty()) vm.panic("null reference");
        vm.panic(std::format("null reference: '{}' is null", text));
    }

    [[noreturn]] static void stale(VM& vm, const Value& v) {
        std::string text = span_text(vm);
        vm.stale_panic(v, text.empty() ? std::string("the object") : std::format("'{}'", text));
    }

    [[noreturn]] static void panic_cstr(VM& vm, const char* msg) { vm.panic(msg); }

    [[noreturn]] static void overflow(VM& vm, std::uint8_t op, std::uint8_t k) {
        vm.panic(std::format("integer overflow in '{}' ({})", arith_symbol(static_cast<Op>(op)),
                             to_string(static_cast<NumKind>(k))));
    }

    static InstanceObj* instance(VM& vm, const Value& v, std::uint8_t k) {
        if (!v.is_obj()) null_panic(vm);
        if (v.ref != 0 && (k & opk::Unchecked) == 0 && v.o->gen != v.aux) [[unlikely]]
            stale(vm, v);
        return static_cast<InstanceObj*>(v.o);
    }

    static ListObj* list(VM& vm, const Value& v, std::uint8_t k) {
        if (!v.is_obj()) null_panic(vm);
        if (v.ref != 0 && (k & opk::Unchecked) == 0 && v.o->gen != v.aux) [[unlikely]]
            stale(vm, v);
        return static_cast<ListObj*>(v.o);
    }

    static void get_up(VM& vm, Value* dst, std::uint16_t b) {
        assign(*dst, vm.frames_.back().closure->captures[b]);
    }

    static void get_up_c(VM& vm, Value* dst, std::uint16_t b) {
        auto* c = static_cast<CellObj*>(vm.frames_.back().closure->captures[b].o);
        assign(*dst, c->value);
    }

    static void set_up_c(VM& vm, std::uint16_t a, Value val) {
        auto* c = static_cast<CellObj*>(vm.frames_.back().closure->captures[a].o);
        assign(c->value, val);
    }

    static void new_cell_op(Value* dst, Value init, std::uint8_t k) {
        CellObj* c = new_cell(k != 0);
        assign(c->value, init);
        assign(*dst, Value::object(c));
    }

    static void get_cell(Value* dst, Value cell_val) {
        assign(*dst, static_cast<CellObj*>(cell_val.o)->value);
    }

    static void set_cell(Value cell_val, Value val) {
        assign(static_cast<CellObj*>(cell_val.o)->value, val);
    }

    static void new_closure_op(VM& vm, Value* dst, std::uint16_t b, const Value* caps) {
        const Proto* p = vm.protos[b].get();
        ClosureObj* c = new_closure(p, p->capture_count);
        for (std::uint16_t i = 0; i < p->capture_count; ++i) assign(c->captures[i], caps[i]);
        assign(*dst, Value::object(c));
    }

    static void get_f(VM& vm, Value* dst, Value obj, std::uint16_t c, std::uint8_t k) {
        InstanceObj* o = instance(vm, obj, k);
        if ((k & opk::WeakField) != 0) {
            assign(*dst, weak_load(o->fields[c]));
        } else {
            assign(*dst, o->fields[c]);
        }
    }

    static void set_f(VM& vm, Value obj, std::uint16_t b, Value val, std::uint8_t k) {
        InstanceObj* o = instance(vm, obj, k);
        if ((k & opk::WeakField) != 0) {
            assign_owned(o->fields[b], weak_store(val));
        } else {
            assign(o->fields[b], val);
        }
    }

    static void get_i(VM& vm, Value* dst, Value lst, std::int64_t i, std::uint8_t k) {
        ListObj* l = list(vm, lst, k);
        if (i < 0 || static_cast<std::size_t>(i) >= l->items.size()) {
            vm.panic(std::format("index {} is out of bounds for a list of length {}", i, l->items.size()));
        }
        assign(*dst, l->items[static_cast<std::size_t>(i)]);
    }

    static void set_i(VM& vm, Value lst, std::int64_t i, Value val, std::uint8_t k) {
        ListObj* l = list(vm, lst, k);
        if (i < 0 || static_cast<std::size_t>(i) >= l->items.size()) {
            vm.panic(std::format("index {} is out of bounds for a list of length {}", i, l->items.size()));
        }
        assign(l->items[static_cast<std::size_t>(i)], val);
    }

    static void new_list_op(VM& vm, Value* dst, const Value* items, std::uint16_t c, std::uint8_t k) {
        auto arena = static_cast<ArenaKind>(k & 3);
        ListObj* l = arena == ArenaKind::None ? new_list((k & opk::UntracedList) != 0) : manual_list(arena);
        if ((k & opk::ArrayList) != 0) l->kind = ObjKind::Array;
        l->items.resize(c);
        for (std::uint16_t i = 0; i < c; ++i) assign(l->items[i], items[i]);
        if (arena == ArenaKind::None) {
            assign(*dst, Value::object(l));
        } else {
            vm.register_manual(l);
            assign(*dst, manual_ref(l));
        }
    }

    static void new_obj_op(VM& vm, Value* dst, std::uint16_t b, std::uint8_t k) {
        const RecordRT* t = vm.classes[b].get();
        if (k == 0) {
            assign(*dst, Value::object(new_instance(t, t->field_names.size())));
        } else {
            InstanceObj* o = manual_instance(t, t->field_names.size(), static_cast<ArenaKind>(k));
            vm.register_manual(o);
            assign(*dst, manual_ref(o));
        }
    }

    static void new_struct_op(VM& vm, Value* dst, std::uint16_t b, const Value* fields) {
        const RecordRT* t = vm.structs[b].get();
        InstanceObj* o = new_instance(t, t->field_names.size());
        for (std::size_t i = 0; i < o->fields.size(); ++i) assign(o->fields[i], fields[i]);
        assign(*dst, Value::object(o));
    }

    static void new_enum_op(VM& vm, Value* dst, std::uint16_t b, std::uint8_t k, const Value* fields) {
        const EnumRT* t = vm.enums[b].get();
        const VariantRT& var = t->variants[k];
        EnumObj* o = new_enum(t, k, var.field_names.size());
        for (std::size_t i = 0; i < o->fields.size(); ++i) assign(o->fields[i], fields[i]);
        assign(*dst, Value::object(o));
    }

    static void copy_val(VM& vm, Value* dst, Value src) {
        assign(*dst, vm.copy_value(src));
    }

    static std::int64_t int_arith(VM& vm, std::uint8_t op_u, std::uint8_t k_u, std::int64_t a, std::int64_t b) {
        auto op = static_cast<Op>(op_u);
        auto k = static_cast<NumKind>(k_u);
        std::int64_t out = 0;
        num::Status st = num::int_arith(arith_binop(op), k, a, b, out);
        if (st != num::Status::Ok) {
            if (st == num::Status::DivByZero) vm.panic("division by zero");
            if (!vm.options_.wrap_overflow) overflow(vm, op_u, k_u);
        }
        return out;
    }

    static std::int64_t int_neg(VM& vm, std::uint8_t k_u, std::int64_t a) {
        auto k = static_cast<NumKind>(k_u);
        std::int64_t out = 0;
        if (num::int_neg(k, a, out) != num::Status::Ok && !vm.options_.wrap_overflow) {
            overflow(vm, static_cast<std::uint8_t>(Op::Neg), k_u);
        }
        return out;
    }

    static std::int64_t int_bits(VM& vm, std::uint8_t op_u, std::uint8_t k_u, std::int64_t a, std::int64_t b) {
        auto op = static_cast<Op>(op_u);
        auto k = static_cast<NumKind>(k_u);
        std::int64_t out = 0;
        if (num::int_bits(arith_binop(op), k, a, b, out) != num::Status::Ok && !vm.options_.wrap_overflow) {
            vm.panic(std::format("shift amount {} is out of range for '{}'", b, to_string(k)));
        }
        return out;
    }

    static void vec_arith_op(Value* dst, std::uint8_t op, std::uint8_t k, Value x, Value y) {
        assign(*dst, vec_arith(static_cast<Op>(op), k, x, y));
    }

    static void vec_neg_op(Value* dst, Value x) {
        float out[max_vec];
        std::uint32_t n = vec_size(x);
        for (std::uint32_t i = 0; i < n; ++i) out[i] = -vec_data(x)[i];
        assign(*dst, vec_value(out, n));
    }

    static void dyn_arith_op(VM& vm, AotCtx* ctx, std::uint16_t a, std::uint8_t op, Value p, Value q) {
        retain(p);
        retain(q);
        Value r = vm.dyn_arith(static_cast<Op>(op), p, q);
        release(p);
        release(q);
        enter(vm, ctx);
        assign_owned(ctx->R[a], r);
    }

    static void dyn_neg_op(VM& vm, AotCtx* ctx, std::uint16_t a, Value p) {
        retain(p);
        Value r{};
        if (is_num_tag(p.tag)) {
            Value zero = p;
            zero.i = 0;
            if (p.tag == Tag::F32) zero.f = 0.0f;
            if (p.tag == Tag::F64) zero.d = 0.0;
            r = vm.dyn_arith(Op::Sub, zero, p);
        } else if (is_vec(p)) {
            float out[max_vec];
            std::uint32_t n = vec_size(p);
            for (std::uint32_t i = 0; i < n; ++i) out[i] = -vec_data(p)[i];
            r = vec_value(out, n);
            retain(r);
        } else if (const std::uint32_t* m = vm.find_method(p, "neg")) {
            r = vm.invoke(*m, &p, 1);
        } else {
            vm.panic("operator '-' is not supported for this value");
        }
        release(p);
        enter(vm, ctx);
        assign_owned(ctx->R[a], r);
    }

    static bool cmp_val(VM& vm, AotCtx* ctx, std::uint8_t op_u, std::uint8_t k, Value x, Value y) {
        auto op = static_cast<Op>(op_u);
        bool is_eq = op == Op::Eq || op == Op::Ne;
        bool r = false;
        if (k == opk::Ref) {
            r = (x.is_obj() ? x.o : nullptr) == (y.is_obj() ? y.o : nullptr) &&
                !(x.ref != 0 && y.ref != 0 && x.aux != y.aux);
        } else if (k == opk::Str) {
            int c = static_cast<const StrObj*>(x.o)->s.compare(static_cast<const StrObj*>(y.o)->s);
            r = is_eq ? c == 0 : op == Op::Lt ? c < 0 : c <= 0;
        } else if (k == opk::Vec) {
            r = vec_equal(x, y);
        } else {
            retain(x);
            retain(y);
            r = is_eq ? vm.dyn_equal(x, y) : vm.dyn_less(x, y, op == Op::Le);
            release(x);
            release(y);
            enter(vm, ctx);
        }
        if (op == Op::Ne) r = !r;
        return r;
    }

    static void cast_val(Value* dst, Value x, std::uint8_t from, std::uint8_t to) {
        Value out;
        bool from_float =
            from == static_cast<std::uint8_t>(NumKind::F32) || from == static_cast<std::uint8_t>(NumKind::F64);
        double fv = from == static_cast<std::uint8_t>(NumKind::F32) ? static_cast<double>(x.f) : x.d;
        std::int64_t iv = from == opk::Bool ? (x.b ? 1 : 0) : x.i;
        NumKind fk = from < 8 ? static_cast<NumKind>(from) : NumKind::I64;
        if (to == static_cast<std::uint8_t>(NumKind::F32)) {
            out = Value::f32(from_float ? static_cast<float>(fv) : static_cast<float>(num::int_to_double(fk, iv)));
        } else if (to == static_cast<std::uint8_t>(NumKind::F64)) {
            out = Value::f64(from_float ? fv : num::int_to_double(fk, iv));
        } else {
            auto tk = static_cast<NumKind>(to);
            out = Value::integer(tk, from_float ? num::float_to_int(tk, fv)
                                                : num::wrap(tk, static_cast<std::uint64_t>(iv)));
        }
        assign_owned(*dst, out);
    }

    static void downcast(VM& vm, Value* dst, Value x, std::uint16_t c) {
        if (x.is_obj()) {
            const RecordRT* target = vm.classes[c].get();
            const RecordRT* t = x.o->kind == ObjKind::Instance ? static_cast<InstanceObj*>(x.o)->type : nullptr;
            if (t == nullptr || !t->is_subclass_of(target)) {
                vm.panic(std::format("cannot cast {} to '{}'", t != nullptr ? "'" + t->name + "'" : "this value",
                                     target->name));
            }
        }
        assign(*dst, x);
    }

    static bool inst_of(VM& vm, Value x, std::uint16_t c) {
        return x.is_obj() && x.o->kind == ObjKind::Instance &&
               static_cast<InstanceObj*>(x.o)->type->is_subclass_of(vm.classes[c].get());
    }

    static std::int64_t variant_of(VM& vm, Value x) {
        if (!x.is_obj()) null_panic(vm);
        return static_cast<EnumObj*>(x.o)->variant;
    }

    static void get_pay(Value* dst, Value x, std::uint16_t c) {
        assign(*dst, static_cast<EnumObj*>(x.o)->fields[c]);
    }

    static void to_str(VM& vm, AotCtx* ctx, std::uint16_t a, Value x) {
        retain(x);
        std::string s = vm.format(x, false);
        release(x);
        enter(vm, ctx);
        assign(ctx->R[a], str_value(std::move(s)));
    }

    static void concat(Value* dst, const Value* regs, std::uint16_t c) {
        std::string s;
        for (std::uint16_t i = 0; i < c; ++i) s += static_cast<const StrObj*>(regs[i].o)->s;
        assign(*dst, str_value(std::move(s)));
    }

    static bool invoke_pushed(VM& vm, AotCtx* ctx, const Proto* p) {
        if (p->aot_fn != nullptr) {
            if (!p->aot_fn(vm, nullptr)) return false;
            enter(vm, ctx);
            return true;
        }
        std::size_t depth = vm.frames_.size() - 1;
        vm.execute(depth);
        if (vm.suspended_) return false;
        enter(vm, ctx);
        return true;
    }

    static void start_coro(VM& vm, AotCtx* ctx, const Proto* p, std::uint16_t a, std::uint16_t c, std::uint8_t k) {
        Object* owner = nullptr;
        if ((k & 0x0f) == 2 && ctx->R[c - 1].is_obj()) owner = ctx->R[c - 1].o;
        CoroObj* co = vm.start_coroutine(p, ctx->R + c, owner);
        enter(vm, ctx);
        assign_owned(ctx->R[a], Value::object(co));
    }

    static bool call_proto(VM& vm, AotCtx* ctx, std::uint16_t b, std::uint16_t a, std::uint16_t c, std::uint8_t k) {
        const Proto* p = vm.protos[b].get();
        if (k != 0) {
            start_coro(vm, ctx, p, a, c, k);
            return true;
        }
        std::uint32_t ret = vm.frames_.back().base + a;
        vm.push_frame(p, nullptr, ctx->R + c, p->param_count, ret, false);
        return invoke_pushed(vm, ctx, p);
    }

    static bool call_vtable(VM& vm, AotCtx* ctx, std::uint16_t b, std::uint16_t a, std::uint16_t c, std::uint8_t k) {
        InstanceObj* o = instance(vm, ctx->R[c], k);
        const Proto* p = vm.protos[o->type->vtable[b]].get();
        if ((k & 0x0f) != 0) {
            start_coro(vm, ctx, p, a, c, k);
            return true;
        }
        std::uint32_t ret = vm.frames_.back().base + a;
        vm.push_frame(p, nullptr, ctx->R + c, p->param_count, ret, false);
        return invoke_pushed(vm, ctx, p);
    }

    static bool call_closure(VM& vm, AotCtx* ctx, std::uint16_t b, std::uint16_t a, std::uint16_t c) {
        const Value& cv = ctx->R[b];
        if (!cv.is_obj()) null_panic(vm);
        auto* cl = static_cast<ClosureObj*>(cv.o);
        std::uint32_t ret = vm.frames_.back().base + a;
        vm.push_frame(cl->fn, cl, ctx->R + c, cl->fn->param_count, ret, false);
        return invoke_pushed(vm, ctx, cl->fn);
    }

    static void call_builtin(VM& vm, AotCtx* ctx, std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint8_t k) {
        Value r = vm.call_native(b, ctx->R + c, k);
        enter(vm, ctx);
        assign_owned(ctx->R[a], r);
    }

    static void call_host(VM& vm, AotCtx* ctx, std::uint16_t a, std::uint32_t id, std::uint16_t c, std::uint8_t k) {
        if (id >= vm.natives_.size()) vm.panic("native function is not bound");
        std::array<Value, 16> small;
        std::vector<Value> big;
        Value* args = small.data();
        if (k > small.size()) {
            big.assign(ctx->R + c, ctx->R + c + k);
            args = big.data();
        } else {
            std::copy(ctx->R + c, ctx->R + c + k, small.begin());
        }
        for (std::uint8_t i = 0; i < k; ++i) {
            if (is_stale(args[i])) stale(vm, args[i]);
        }
        NativeCall nc{vm, args, k};
        Value r = vm.natives_[id].fn(nc);
        enter(vm, ctx);
        assign_owned(ctx->R[a], checked_val(r));
    }

    static bool call_dyn(VM& vm, AotCtx* ctx, std::uint16_t b, std::uint16_t a, std::uint16_t c) {
        const Value& recv = ctx->R[c];
        if (!recv.is_obj()) null_panic(vm);
        if (is_stale(recv)) stale(vm, recv);
        std::string_view name = static_cast<const StrObj*>(ctx->K[b].o)->s;
        const std::uint32_t* m = vm.find_method(recv, name);
        if (m == nullptr) vm.panic(std::format("value has no method '{}'", name));
        const Proto* p = vm.protos[*m].get();
        std::uint32_t ret = vm.frames_.back().base + a;
        vm.push_frame(p, nullptr, ctx->R + c, p->param_count, ret, false);
        return invoke_pushed(vm, ctx, p);
    }

    static void ret(VM& vm, Value result, Value* out) {
        bool discard = vm.frames_.back().discard;
        Value v = vm.pop_frame(result);
        if (discard && out != nullptr) *out = v;
    }

    static void ret0(VM& vm, Value* out) {
        bool discard = vm.frames_.back().discard;
        Value v = vm.pop_frame(Value{});
        if (discard && out != nullptr) *out = v;
    }

    static bool for_prep(VM& vm, Value* base, std::uint8_t flags) {
        auto k = static_cast<NumKind>(flags & 0x7f);
        bool inclusive = (flags & opk::Inclusive) != 0;
        std::int64_t i = base[0].i;
        std::int64_t hi = base[1].i;
        std::int64_t step = base[2].i;
        if (step == 0) vm.panic("a range step cannot be zero");
        bool up = is_signed(k) ? step > 0 : true;
        if (up) return inclusive ? !num::int_less(k, hi, i) : num::int_less(k, i, hi);
        return inclusive ? !num::int_less(k, i, hi) : num::int_less(k, hi, i);
    }

    static bool for_loop(Value* base, std::uint8_t flags) {
        auto k = static_cast<NumKind>(flags & 0x7f);
        bool inclusive = (flags & opk::Inclusive) != 0;
        std::int64_t hi = base[1].i;
        std::int64_t step = base[2].i;
        bool up = is_signed(k) ? step > 0 : true;
        std::int64_t next = 0;
        if (num::int_arith(BinaryOp::Add, k, base[0].i, step, next) != num::Status::Ok) return false;
        bool in_range = up ? (inclusive ? !num::int_less(k, hi, next) : num::int_less(k, next, hi))
                           : (inclusive ? !num::int_less(k, next, hi) : num::int_less(k, hi, next));
        if (!in_range) return false;
        base[0].i = next;
        return true;
    }

    // IterNext (Map / Set / Deque `for` loops): shared with the VM.
    static bool iter_next(VM& vm, Value* dst, Value coll, Value* cursor, std::uint8_t k) {
        retain(coll);
        bool found = false;
        try {
            found = vm.iter_next(dst, coll, *cursor, k);
        } catch (...) {
            release(coll);
            throw;
        }
        release(coll);
        return found;
    }

    static bool list_next(VM& vm, Value* dst, Value lst, std::int64_t idx_i, std::uint8_t k) {
        ListObj* l = list(vm, lst, k);
        auto idx = static_cast<std::size_t>(idx_i);
        if (idx < l->items.size()) {
            assign(*dst, l->items[idx]);
            return true;
        }
        return false;
    }

    static bool await_op(VM& vm, AotCtx* ctx, std::uint8_t k, std::uint16_t a, std::uint16_t b) {
        switch (static_cast<AK>(k)) {
            case AK::Ticks:
            case AK::Seconds: {
                std::int64_t n = ctx->R[b].i;
                if (static_cast<AK>(k) == AK::Seconds) {
                    float t = std::floor(ctx->R[b].f * vm.options_.tick_rate + 0.5f);
                    n = t >= 1.0f ? static_cast<std::int64_t>(std::min(t, 1e15f)) : 1;
                }
                if (n <= 0) {
                    assign_owned(ctx->R[a], Value{});
                    return true;
                }
                vm.suspend(CoroObj::Wait::Ticks, vm.tick_ + static_cast<std::uint64_t>(n), Value{}, a);
                return false;
            }
            case AK::Until: {
                Value cond = ctx->R[b];
                if (!cond.is_obj()) null_panic(vm);
                retain(cond);
                Value r = vm.invoke_closure(static_cast<ClosureObj*>(cond.o), nullptr, 0);
                enter(vm, ctx);
                if (r.b) {
                    release(cond);
                    assign_owned(ctx->R[a], Value{});
                    return true;
                }
                vm.suspend(CoroObj::Wait::Until, 0, cond, a);
                release(cond);
                return false;
            }
            case AK::Task: {
                const Value& t = ctx->R[b];
                if (!t.is_obj()) null_panic(vm);
                auto* tc = static_cast<CoroObj*>(t.o);
                if (tc->finished()) {
                    assign_owned(ctx->R[a], Value{});
                    return true;
                }
                if (tc->state == CoroObj::State::Running) vm.panic("a coroutine cannot await itself");
                vm.suspend(CoroObj::Wait::Task, 0, t, a);
                return false;
            }
            case AK::Signal: {
                if (!ctx->R[b].is_obj()) null_panic(vm);
                vm.suspend(CoroObj::Wait::Signal, 0, ctx->R[b], a);
                return false;
            }
            case AK::Call: break;
        }
        return true;
    }

    static void vec_new_op(Value* dst, const Value* parts, std::uint16_t c, std::uint8_t k) {
        float out[max_vec];
        std::uint32_t n = k;
        if (c == 1 && parts[0].tag == Tag::F32 && n > 1) {
            for (std::uint32_t i = 0; i < n; ++i) out[i] = parts[0].f;
        } else {
            std::uint32_t at = 0;
            for (std::uint16_t p = 0; p < c; ++p) {
                const Value& part = parts[p];
                if (part.tag == Tag::F32) {
                    out[at++] = part.f;
                } else {
                    std::uint32_t m = vec_size(part);
                    const float* d = vec_data(part);
                    for (std::uint32_t i = 0; i < m; ++i) out[at++] = d[i];
                }
            }
        }
        assign(*dst, vec_value(out, n));
    }

    static void swz_op(Value* dst, Value src, std::uint16_t c, std::uint8_t k) {
        const float* d = vec_data(src);
        if (c == 1) {
            assign_owned(*dst, Value::f32(d[k & 3]));
            return;
        }
        float out[4];
        for (std::uint16_t i = 0; i < c; ++i) out[i] = d[(k >> (2 * i)) & 3];
        assign(*dst, vec_value(out, c));
    }

    static void vec_with_op(Value* dst, Value src, Value v, std::uint8_t k) {
        float out[max_vec];
        std::uint32_t n = vec_size(src);
        std::copy(vec_data(src), vec_data(src) + n, out);
        if (v.tag == Tag::F32) {
            out[k & 3] = v.f;
        } else {
            std::uint32_t m = vec_size(v);
            for (std::uint32_t i = 0; i < m; ++i) out[(k >> (2 * i)) & 3] = vec_data(v)[i];
        }
        assign(*dst, vec_value(out, n));
    }

    static float vec_idx_op(VM& vm, Value src, std::int64_t i) {
        std::uint32_t n = vec_size(src);
        if (i < 0 || static_cast<std::uint64_t>(i) >= n) {
            vm.panic(std::format("index {} is out of bounds for a vec{}", i, n));
        }
        return vec_data(src)[i];
    }

    static void new_sig_op(Value* dst) { assign(*dst, Value::object(new_signal())); }

    static void free_op(VM& vm, Value v) {
        if (v.tag != Tag::Obj) return;
        if (!is_manual(v.o)) {
            vm.panic(std::format("free: '{}' is not a manual object (only objects from 'alloc', 'new(frame)', "
                                 "'new(scene)' or a manual class can be freed)",
                                 span_text(vm)));
        }
        if (v.ref != 0 ? v.o->gen != v.aux : (v.o->flags & objflag::Dead) != 0) {
            vm.panic(std::format("double free: '{}' was already freed; {}", span_text(vm), vm.free_text(v.o)));
        }
        const VM::Frame& f = vm.frames_.back();
        FreeSite site;
        site.proto = f.fn;
        site.pc = f.pc - 1;
        site.tick = vm.tick_;
        vm.free_manual(v.o, site);
    }

    static bool bind_image(VM& vm, const AotImageDesc* img) {
        for (std::uint32_t i = 0; i < img->native_count; ++i) {
            img->native_ids[i] = vm.native_index(img->native_names[i]);
        }
        if (vm.protos.size() < img->proto_count) return false;
        for (std::uint32_t i = 0; i < img->proto_count; ++i) {
            vm.protos[i]->aot_fn = img->protos[i].fn;
        }
        return true;
    }

    static bool load_image(VM& vm, const AotImageDesc* img, RunResult* out_res) {
        for (std::uint32_t i = 0; i < img->name_count; ++i) vm.intern(img->names[i]);
        for (std::uint32_t i = 0; i < img->native_count; ++i) {
            img->native_ids[i] = vm.native_index(img->native_names[i]);
        }
        vm.classes.clear();
        vm.classes.reserve(img->class_count);
        for (std::uint32_t i = 0; i < img->class_count; ++i) vm.classes.push_back(std::make_unique<RecordRT>());

        auto fill_record = [&](RecordRT& rt, const AotRecordDesc& d) {
            rt.name = d.name;
            rt.is_struct = d.is_struct != 0;
            rt.base = d.base_index >= 0 ? vm.classes[static_cast<std::size_t>(d.base_index)].get() : nullptr;
            rt.field_init = d.field_init;
            rt.gc_flags = d.gc_flags;
            rt.vtable.assign(d.vtable, d.vtable + d.vtable_count);
            for (std::uint32_t m = 0; m < d.method_count; ++m) {
                rt.methods[d.methods[m].name] = d.methods[m].proto;
            }
            rt.field_names.reserve(d.field_count);
            rt.field_types.reserve(d.field_count);
            rt.field_exported.reserve(d.field_count);
            rt.field_weak.reserve(d.field_count);
            for (std::uint32_t f = 0; f < d.field_count; ++f) {
                rt.field_names.emplace_back(d.fields[f].name);
                rt.field_types.emplace_back(d.fields[f].type);
                rt.field_exported.push_back(d.fields[f].exported != 0);
                rt.field_weak.push_back(d.fields[f].weak != 0);
            }
            rt.finalizer = vm.native_finalizer(rt.name);
        };
        for (std::uint32_t i = 0; i < img->class_count; ++i) fill_record(*vm.classes[i], img->classes[i]);
        // Inherit finalizers down the class hierarchy.
        for (std::uint32_t i = 0; i < img->class_count; ++i) {
            RecordRT* rt = vm.classes[i].get();
            for (const RecordRT* b = rt->base; rt->finalizer == nullptr && b != nullptr; b = b->base) {
                rt->finalizer = b->finalizer;
            }
        }

        vm.structs.clear();
        vm.structs.reserve(img->struct_count);
        for (std::uint32_t i = 0; i < img->struct_count; ++i) {
            auto rt = std::make_unique<RecordRT>();
            fill_record(*rt, img->structs[i]);
            vm.structs.push_back(std::move(rt));
        }

        vm.enums.clear();
        vm.enums.reserve(img->enum_count);
        for (std::uint32_t i = 0; i < img->enum_count; ++i) {
            const AotEnumDesc& d = img->enums[i];
            auto rt = std::make_unique<EnumRT>();
            rt->name = d.name;
            rt->plain = d.plain != 0;
            rt->gc_flags = d.gc_flags;
            rt->variants.reserve(d.variant_count);
            for (std::uint32_t v = 0; v < d.variant_count; ++v) {
                VariantRT vr;
                vr.name = d.variants[v].name;
                vr.discriminant = d.variants[v].discriminant;
                for (std::uint32_t f = 0; f < d.variants[v].field_count; ++f) {
                    vr.field_names.emplace_back(d.variants[v].field_names[f]);
                }
                rt->variants.push_back(std::move(vr));
            }
            vm.enums.push_back(std::move(rt));
        }

        vm.globals.assign(img->global_count, Value{});
        vm.global_names.clear();
        vm.global_types.clear();
        vm.global_names.reserve(img->global_count);
        vm.global_types.reserve(img->global_count);
        for (std::uint32_t i = 0; i < img->global_count; ++i) {
            vm.global_names.emplace_back(img->globals[i].name);
            vm.global_types.emplace_back(img->globals[i].type);
        }

        vm.protos.clear();
        vm.protos.reserve(img->proto_count);
        for (std::uint32_t i = 0; i < img->proto_count; ++i) {
            const AotProtoDesc& d = img->protos[i];
            auto p = std::make_unique<Proto>();
            p->name = d.name;
            p->key = d.key;
            p->index = d.index;
            p->param_count = d.param_count;
            p->reg_count = d.reg_count;
            p->capture_count = d.capture_count;
            p->compiled = true;
            p->aot_fn = d.fn;
            p->consts.reserve(d.const_count);
            for (std::uint32_t c = 0; c < d.const_count; ++c) {
                const AotConstDesc& cd = d.consts[c];
                if (cd.tag == static_cast<std::uint8_t>(Tag::Obj)) {
                    Value sv = str_value(cd.str != nullptr ? cd.str : "");
                    retain(sv);
                    p->consts.push_back(sv);
                } else {
                    Value v;
                    v.tag = static_cast<Tag>(cd.tag);
                    v.aux = cd.aux;
                    v.i = cd.bits;
                    p->consts.push_back(v);
                }
            }
            p->locals.reserve(d.local_count);
            for (std::uint32_t l = 0; l < d.local_count; ++l) {
                const AotLocalDesc& ld = d.locals[l];
                p->locals.push_back(LocalDebug{ld.name, ld.type, ld.reg, ld.begin, ld.end});
            }
            p->span_locs.reserve(d.span_count);
            for (std::uint32_t s = 0; s < d.span_count; ++s) {
                const AotSpanLocDesc& sd = img->span_pool[d.span_indices[s]];
                p->span_locs.push_back(SpanLoc{sd.file, sd.line, sd.column, sd.text});
            }
            vm.protos.push_back(std::move(p));
        }

        RunResult res;
        for (std::uint32_t i = 0; i < img->chunk_count; ++i) {
            res = vm.run(img->chunks[i]);
            if (!res.ok) {
                if (out_res != nullptr) *out_res = std::move(res);
                return false;
            }
        }
        if (out_res != nullptr) *out_res = std::move(res);
        return true;
    }

    static const std::vector<std::string>& vm_names(const VM& vm) { return vm.names_; }
    static std::size_t vm_native_count(const VM& vm) { return vm.natives_.size(); }
    static const std::string& vm_native_name(const VM& vm, std::size_t i) { return vm.natives_[i].name; }
};

namespace {

struct AotApiTable {
    void (*enter)(VM&, AotCtx*);
    void (*free_obj)(Object*);
    void (*root_obj)(Object*);
    void (*panic_cstr)(VM&, const char*);
    void (*overflow)(VM&, std::uint8_t, std::uint8_t);
    void (*get_up)(VM&, Value*, std::uint16_t);
    void (*get_up_c)(VM&, Value*, std::uint16_t);
    void (*set_up_c)(VM&, std::uint16_t, Value);
    void (*new_cell_op)(Value*, Value, std::uint8_t);
    void (*get_cell)(Value*, Value);
    void (*set_cell)(Value, Value);
    void (*new_closure_op)(VM&, Value*, std::uint16_t, const Value*);
    void (*get_f)(VM&, Value*, Value, std::uint16_t, std::uint8_t);
    void (*set_f)(VM&, Value, std::uint16_t, Value, std::uint8_t);
    void (*get_i)(VM&, Value*, Value, std::int64_t, std::uint8_t);
    void (*set_i)(VM&, Value, std::int64_t, Value, std::uint8_t);
    void (*new_list_op)(VM&, Value*, const Value*, std::uint16_t, std::uint8_t);
    void (*new_obj_op)(VM&, Value*, std::uint16_t, std::uint8_t);
    void (*new_struct_op)(VM&, Value*, std::uint16_t, const Value*);
    void (*new_enum_op)(VM&, Value*, std::uint16_t, std::uint8_t, const Value*);
    void (*copy_val)(VM&, Value*, Value);
    std::int64_t (*int_arith)(VM&, std::uint8_t, std::uint8_t, std::int64_t, std::int64_t);
    std::int64_t (*int_neg)(VM&, std::uint8_t, std::int64_t);
    std::int64_t (*int_bits)(VM&, std::uint8_t, std::uint8_t, std::int64_t, std::int64_t);
    void (*vec_arith_op)(Value*, std::uint8_t, std::uint8_t, Value, Value);
    void (*vec_neg_op)(Value*, Value);
    void (*dyn_arith_op)(VM&, AotCtx*, std::uint16_t, std::uint8_t, Value, Value);
    void (*dyn_neg_op)(VM&, AotCtx*, std::uint16_t, Value);
    bool (*cmp_val)(VM&, AotCtx*, std::uint8_t, std::uint8_t, Value, Value);
    void (*cast_val)(Value*, Value, std::uint8_t, std::uint8_t);
    void (*downcast)(VM&, Value*, Value, std::uint16_t);
    bool (*inst_of)(VM&, Value, std::uint16_t);
    std::int64_t (*variant_of)(VM&, Value);
    void (*get_pay)(Value*, Value, std::uint16_t);
    void (*to_str)(VM&, AotCtx*, std::uint16_t, Value);
    void (*concat)(Value*, const Value*, std::uint16_t);
    bool (*call_proto)(VM&, AotCtx*, std::uint16_t, std::uint16_t, std::uint16_t, std::uint8_t);
    bool (*call_vtable)(VM&, AotCtx*, std::uint16_t, std::uint16_t, std::uint16_t, std::uint8_t);
    bool (*call_closure)(VM&, AotCtx*, std::uint16_t, std::uint16_t, std::uint16_t);
    void (*call_builtin)(VM&, AotCtx*, std::uint16_t, std::uint16_t, std::uint16_t, std::uint8_t);
    void (*call_host)(VM&, AotCtx*, std::uint16_t, std::uint32_t, std::uint16_t, std::uint8_t);
    bool (*call_dyn)(VM&, AotCtx*, std::uint16_t, std::uint16_t, std::uint16_t);
    void (*ret)(VM&, Value, Value*);
    void (*ret0)(VM&, Value*);
    bool (*for_prep)(VM&, Value*, std::uint8_t);
    bool (*for_loop)(Value*, std::uint8_t);
    bool (*list_next)(VM&, Value*, Value, std::int64_t, std::uint8_t);
    bool (*await_op)(VM&, AotCtx*, std::uint8_t, std::uint16_t, std::uint16_t);
    void (*vec_new_op)(Value*, const Value*, std::uint16_t, std::uint8_t);
    void (*swz_op)(Value*, Value, std::uint16_t, std::uint8_t);
    void (*vec_with_op)(Value*, Value, Value, std::uint8_t);
    float (*vec_idx_op)(VM&, Value, std::int64_t);
    void (*new_sig_op)(Value*);
    void (*free_op)(VM&, Value);
    bool (*bind_image)(VM&, const AotImageDesc*);
    bool (*load_image)(VM&, const AotImageDesc*, RunResult*);
    bool (*iter_next)(VM&, Value*, Value, Value*, std::uint8_t);
};

const AotApiTable g_aot_api = {
    &AotRuntime::enter,
    &AotRuntime::free_obj,
    &AotRuntime::root_obj,
    &AotRuntime::panic_cstr,
    &AotRuntime::overflow,
    &AotRuntime::get_up,
    &AotRuntime::get_up_c,
    &AotRuntime::set_up_c,
    &AotRuntime::new_cell_op,
    &AotRuntime::get_cell,
    &AotRuntime::set_cell,
    &AotRuntime::new_closure_op,
    &AotRuntime::get_f,
    &AotRuntime::set_f,
    &AotRuntime::get_i,
    &AotRuntime::set_i,
    &AotRuntime::new_list_op,
    &AotRuntime::new_obj_op,
    &AotRuntime::new_struct_op,
    &AotRuntime::new_enum_op,
    &AotRuntime::copy_val,
    &AotRuntime::int_arith,
    &AotRuntime::int_neg,
    &AotRuntime::int_bits,
    &AotRuntime::vec_arith_op,
    &AotRuntime::vec_neg_op,
    &AotRuntime::dyn_arith_op,
    &AotRuntime::dyn_neg_op,
    &AotRuntime::cmp_val,
    &AotRuntime::cast_val,
    &AotRuntime::downcast,
    &AotRuntime::inst_of,
    &AotRuntime::variant_of,
    &AotRuntime::get_pay,
    &AotRuntime::to_str,
    &AotRuntime::concat,
    &AotRuntime::call_proto,
    &AotRuntime::call_vtable,
    &AotRuntime::call_closure,
    &AotRuntime::call_builtin,
    &AotRuntime::call_host,
    &AotRuntime::call_dyn,
    &AotRuntime::ret,
    &AotRuntime::ret0,
    &AotRuntime::for_prep,
    &AotRuntime::for_loop,
    &AotRuntime::list_next,
    &AotRuntime::await_op,
    &AotRuntime::vec_new_op,
    &AotRuntime::swz_op,
    &AotRuntime::vec_with_op,
    &AotRuntime::vec_idx_op,
    &AotRuntime::new_sig_op,
    &AotRuntime::free_op,
    &AotRuntime::bind_image,
    &AotRuntime::load_image,
    &AotRuntime::iter_next,
};

// ------------------------------------------------------------ C emitter

std::string c_escape(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 32 && c < 127) {
                    out.push_back(static_cast<char>(c));
                } else {
                    out += std::format("\\{:03o}", static_cast<unsigned>(c));
                }
                break;
        }
    }
    out.push_back('\"');
    return out;
}

bool type_may_hold_obj(const Type* t) {
    if (t == nullptr) return true;
    switch (t->tag) {
        case TypeTag::Num:
        case TypeTag::Bool:
        case TypeTag::Name:
        case TypeTag::Void:
        case TypeTag::Null: return false;
        case TypeTag::Enum: return !t->en()->plain;
        case TypeTag::Vec: return t->dim > 2;
        case TypeTag::Nullable: return type_may_hold_obj(t->args[0]);
        default: return true;
    }
}

// Conservative dataflow scan: returns `reg_obj[r] == false` only if register
// `r` can never contain a heap object (`Tag::Obj`) at any point in `p`.
std::vector<bool> analyze_non_obj_regs(const Proto& p, const Function* fn) {
    std::vector<bool> obj(p.reg_count, false);
    for (std::uint16_t i = 0; i < p.param_count && i < p.reg_count; ++i) {
        if (fn != nullptr && i < fn->param_vars.size() && !fn->param_vars[i]->captured) {
            obj[i] = type_may_hold_obj(fn->param_vars[i]->type);
        } else {
            obj[i] = true;
        }
    }
    std::vector<std::pair<std::uint16_t, std::uint16_t>> movs;
    for (const Instr& in : p.code) {
        switch (in.op) {
            case Op::Nop:
            case Op::SetG:
            case Op::SetUpC:
            case Op::SetCell:
            case Op::SetF:
            case Op::SetI:
            case Op::Jmp:
            case Op::JmpF:
            case Op::JmpT:
            case Op::JmpNull:
            case Op::JmpNN:
            case Op::Ret:
            case Op::Ret0:
            case Op::ForPrep:
            case Op::ForLoop:
            case Op::Inc:
            case Op::Free: break;

            case Op::LoadI:
            case Op::LoadNull:
            case Op::LoadBool:
            case Op::Clear:
            case Op::BAnd:
            case Op::BOr:
            case Op::BXor:
            case Op::Shl:
            case Op::Shr:
            case Op::BNot:
            case Op::Not:
            case Op::Eq:
            case Op::Ne:
            case Op::Lt:
            case Op::Le:
            case Op::Cast:
            case Op::InstOf:
            case Op::Variant:
            case Op::VecIdx: break;

            case Op::LoadK:
                if (in.b < p.consts.size() && p.consts[in.b].tag == Tag::Obj) obj[in.a] = true;
                break;
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Rem:
            case Op::Neg:
                if (in.k >= 10) obj[in.a] = true;
                break;
            case Op::VecNew:
                if (in.k > 2) obj[in.a] = true;
                break;
            case Op::Swz:
                if (in.c > 2) obj[in.a] = true;
                break;
            case Op::Mov:
                movs.emplace_back(in.a, in.b);
                break;
            case Op::IterNext:
                if (in.a < p.reg_count) obj[in.a] = true;
                if ((in.k & opk::IterPair) != 0 && in.a + 1 < p.reg_count) obj[in.a + 1] = true;
                break;
            default:
                if (in.a < p.reg_count) obj[in.a] = true;
                break;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto [dst, src] : movs) {
            if (dst < p.reg_count && src < p.reg_count && obj[src] && !obj[dst]) {
                obj[dst] = true;
                changed = true;
            }
        }
    }
    return obj;
}

bool needs_slice_text(Op op) {
    switch (op) {
        case Op::GetF:
        case Op::SetF:
        case Op::GetI:
        case Op::SetI:
        case Op::ListNext:
        case Op::IterNext:
        case Op::Variant:
        case Op::CallV:
        case Op::CallC:
        case Op::CallX:
        case Op::CallD:
        case Op::Await:
        case Op::Free: return true;
        default: return false;
    }
}

std::string wrap_c_expr(NumKind k, std::string_view expr) {
    switch (k) {
        case NumKind::I8: return std::format("(int64_t)(int8_t)(uint8_t)({})", expr);
        case NumKind::I16: return std::format("(int64_t)(int16_t)(uint16_t)({})", expr);
        case NumKind::I32: return std::format("(int64_t)(int32_t)(uint32_t)({})", expr);
        case NumKind::U8: return std::format("(int64_t)((uint64_t)({}) & 0xffu)", expr);
        case NumKind::U16: return std::format("(int64_t)((uint64_t)({}) & 0xffffu)", expr);
        case NumKind::U32: return std::format("(int64_t)((uint64_t)({}) & 0xffffffffull)", expr);
        default: return std::format("(int64_t)({})", expr);
    }
}

void emit_store_val(std::ostringstream& out, bool dst_may_be_obj, std::uint16_t a, std::string_view val_expr) {
    if (!dst_may_be_obj) {
        out << "    R[" << a << "] = " << val_expr << ";\n";
    } else {
        out << "    pluma_assign_owned(&R[" << a << "], " << val_expr << ");\n";
    }
}

void emit_proto_fn(std::ostringstream& out, const Proto& p, const Function* fn) {
    std::vector<bool> reg_obj = analyze_non_obj_regs(p, fn);
    std::size_t n = p.code.size();
    std::vector<bool> has_label(n + 1, false);
    std::vector<std::uint32_t> resume_pcs;

    for (std::size_t i = 0; i < n; ++i) {
        const Instr& in = p.code[i];
        switch (in.op) {
            case Op::Jmp:
            case Op::JmpF:
            case Op::JmpT:
            case Op::JmpNull:
            case Op::JmpNN:
            case Op::ForPrep:
            case Op::ForLoop:
                if (in.target() <= n) has_label[in.target()] = true;
                break;
            case Op::ListNext:
            case Op::IterNext:
                if (i + 2 <= n) has_label[i + 2] = true;
                break;
            case Op::Await:
                if (i + 1 <= n) {
                    has_label[i + 1] = true;
                    resume_pcs.push_back(static_cast<std::uint32_t>(i + 1));
                }
                break;
            case Op::Call:
            case Op::CallV:
                if ((in.k & 0x0f) == 0 && i + 1 <= n) {
                    has_label[i + 1] = true;
                    resume_pcs.push_back(static_cast<std::uint32_t>(i + 1));
                }
                break;
            case Op::CallC:
            case Op::CallD:
                if (i + 1 <= n) {
                    has_label[i + 1] = true;
                    resume_pcs.push_back(static_cast<std::uint32_t>(i + 1));
                }
                break;
            default: break;
        }
    }

    out << "// Proto " << p.index << ": " << p.name << "\n";
    out << "static bool aot_proto_" << p.index << "(PlumaVM* vm, PlumaValue* out) {\n";
    out << "    PlumaAotCtx ctx;\n";
    out << "    G_API->enter(vm, &ctx);\n";
    out << "    PlumaValue* R = ctx.R;\n";
    out << "    const PlumaValue* K = ctx.K;\n";
    out << "    PlumaValue* G = ctx.G;\n";
    out << "    (void)R; (void)K; (void)G;\n";

    if (!resume_pcs.empty()) {
        out << "    switch (*ctx.pc_ptr) {\n";
        out << "        case 0: break;\n";
        for (std::uint32_t rpc : resume_pcs) {
            out << "        case " << rpc << ": goto L_" << rpc << ";\n";
        }
        out << "        default: break;\n";
        out << "    }\n";
    }

    for (std::size_t i = 0; i < n; ++i) {
        if (has_label[i]) out << "L_" << i << ": ;\n";
        const Instr& in = p.code[i];
        const std::uint32_t next_pc = static_cast<std::uint32_t>(i + 1);
        const bool a_obj = in.a < reg_obj.size() ? reg_obj[in.a] : true;
        const bool b_obj = in.b < reg_obj.size() ? reg_obj[in.b] : true;
        const bool c_obj = in.c < reg_obj.size() ? reg_obj[in.c] : true;

        switch (in.op) {
            case Op::Nop: break;
            case Op::Mov:
                if (!a_obj && !b_obj) {
                    out << "    R[" << in.a << "] = R[" << in.b << "];\n";
                } else {
                    out << "    pluma_assign(&R[" << in.a << "], R[" << in.b << "]);\n";
                }
                break;
            case Op::LoadK:
                if (!a_obj && in.b < p.consts.size() && p.consts[in.b].tag != Tag::Obj) {
                    out << "    R[" << in.a << "] = K[" << in.b << "];\n";
                } else {
                    out << "    pluma_assign(&R[" << in.a << "], K[" << in.b << "]);\n";
                }
                break;
            case Op::LoadI: {
                auto tag = static_cast<unsigned>(num_tag(static_cast<NumKind>(in.k)));
                auto imm = static_cast<std::int64_t>(static_cast<std::int16_t>(in.b));
                emit_store_val(out, a_obj, in.a, std::format("pluma_val_int({}, {}LL)", tag, imm));
                break;
            }
            case Op::LoadNull:
                emit_store_val(out, a_obj, in.a, "pluma_val_null()");
                break;
            case Op::LoadBool:
                emit_store_val(out, a_obj, in.a, std::format("pluma_val_bool({})", in.b != 0 ? 1 : 0));
                break;
            case Op::Clear:
                if (!a_obj) {
                    out << "    R[" << in.a << "] = pluma_val_nil();\n";
                } else {
                    out << "    pluma_assign_owned(&R[" << in.a << "], pluma_val_nil());\n";
                }
                break;
            case Op::GetG:
                out << "    pluma_assign(&R[" << in.a << "], G[" << in.b << "]);\n";
                break;
            case Op::SetG:
                out << "    pluma_assign(&G[" << in.a << "], R[" << in.b << "]);\n";
                break;
            case Op::GetUp:
                out << "    G_API->get_up(vm, &R[" << in.a << "], " << in.b << ");\n";
                break;
            case Op::GetUpC:
                out << "    G_API->get_up_c(vm, &R[" << in.a << "], " << in.b << ");\n";
                break;
            case Op::SetUpC:
                out << "    G_API->set_up_c(vm, " << in.a << ", R[" << in.b << "]);\n";
                break;
            case Op::NewCell:
                out << "    G_API->new_cell_op(&R[" << in.a << "], R[" << in.b << "], " << unsigned(in.k) << ");\n";
                break;
            case Op::GetCell:
                out << "    G_API->get_cell(&R[" << in.a << "], R[" << in.b << "]);\n";
                break;
            case Op::SetCell:
                out << "    G_API->set_cell(R[" << in.a << "], R[" << in.b << "]);\n";
                break;
            case Op::Closure:
                out << "    G_API->new_closure_op(vm, &R[" << in.a << "], " << in.b << ", &R[" << in.c << "]);\n";
                break;
            case Op::GetF:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->get_f(vm, &R[" << in.a << "], R[" << in.b << "], " << in.c << ", "
                    << unsigned(in.k) << ");\n";
                break;
            case Op::SetF:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->set_f(vm, R[" << in.a << "], " << in.b << ", R[" << in.c << "], "
                    << unsigned(in.k) << ");\n";
                break;
            case Op::GetI:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->get_i(vm, &R[" << in.a << "], R[" << in.b << "], R[" << in.c << "].i, "
                    << unsigned(in.k) << ");\n";
                break;
            case Op::SetI:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->set_i(vm, R[" << in.a << "], R[" << in.b << "].i, R[" << in.c << "], "
                    << unsigned(in.k) << ");\n";
                break;
            case Op::NewList:
                out << "    G_API->new_list_op(vm, &R[" << in.a << "], &R[" << in.b << "], " << in.c << ", "
                    << unsigned(in.k) << ");\n";
                break;
            case Op::NewObj:
                out << "    G_API->new_obj_op(vm, &R[" << in.a << "], " << in.b << ", " << unsigned(in.k) << ");\n";
                break;
            case Op::NewStruct:
                out << "    G_API->new_struct_op(vm, &R[" << in.a << "], " << in.b << ", &R[" << in.c << "]);\n";
                break;
            case Op::NewEnum:
                out << "    G_API->new_enum_op(vm, &R[" << in.a << "], " << in.b << ", " << unsigned(in.k)
                    << ", &R[" << in.c << "]);\n";
                break;
            case Op::Copy:
                out << "    G_API->copy_val(vm, &R[" << in.a << "], R[" << in.b << "]);\n";
                break;
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Rem: {
                if (in.k == static_cast<std::uint8_t>(NumKind::F32)) {
                    std::string expr;
                    if (in.op == Op::Rem) {
                        expr = std::format("fmodf(R[{}].f, R[{}].f)", in.b, in.c);
                    } else {
                        expr = std::format("R[{}].f {} R[{}].f", in.b, arith_symbol(in.op), in.c);
                    }
                    emit_store_val(out, a_obj, in.a, std::format("pluma_val_f32({})", expr));
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F64)) {
                    std::string expr;
                    if (in.op == Op::Rem) {
                        expr = std::format("fmod(R[{}].d, R[{}].d)", in.b, in.c);
                    } else {
                        expr = std::format("R[{}].d {} R[{}].d", in.b, arith_symbol(in.op), in.c);
                    }
                    emit_store_val(out, a_obj, in.a, std::format("pluma_val_f64({})", expr));
                } else if (in.k < 8) {
                    auto nk = static_cast<NumKind>(in.k);
                    auto tag = static_cast<unsigned>(num_tag(nk));
                    bool sub32 = nk != NumKind::I64 && nk != NumKind::U64;
                    bool add_sub_mul = in.op == Op::Add || in.op == Op::Sub || in.op == Op::Mul;
                    if (sub32 && add_sub_mul) {
                        out << "    {\n";
                        out << "        int64_t r = R[" << in.b << "].i " << arith_symbol(in.op) << " R[" << in.c
                            << "].i;\n";
                        out << "        int64_t w = " << wrap_c_expr(nk, "r") << ";\n";
                        out << "        if (__builtin_expect(w != r && !ctx.wrap_overflow, 0)) { *ctx.pc_ptr = "
                            << next_pc << "; G_API->overflow(vm, " << unsigned(static_cast<std::uint8_t>(in.op))
                            << ", " << unsigned(in.k) << "); }\n";
                        emit_store_val(out, a_obj, in.a, std::format("pluma_val_int({}, w)", tag));
                        out << "    }\n";
                    } else {
                        out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                        emit_store_val(out, a_obj, in.a,
                                       std::format("pluma_val_int({}, G_API->int_arith(vm, {}, {}, R[{}].i, R[{}].i))",
                                                   tag, unsigned(static_cast<std::uint8_t>(in.op)), unsigned(in.k),
                                                   in.b, in.c));
                    }
                } else if (in.k == opk::Vec || in.k == opk::VecS || in.k == opk::SVec) {
                    if (!a_obj && !b_obj && !c_obj && in.op != Op::Rem) {
                        std::string sym(arith_symbol(in.op));
                        if (in.k == opk::Vec) {
                            out << "    R[" << in.a << "] = pluma_val_vecn(R[" << in.b << "].aux, R[" << in.b
                                << "].v2[0] " << sym << " R[" << in.c << "].v2[0], R[" << in.b << "].aux == 2 ? (R["
                                << in.b << "].v2[1] " << sym << " R[" << in.c << "].v2[1]) : 0.0f);\n";
                        } else if (in.k == opk::VecS) {
                            out << "    R[" << in.a << "] = pluma_val_vecn(R[" << in.b << "].aux, R[" << in.b
                                << "].v2[0] " << sym << " R[" << in.c << "].f, R[" << in.b << "].aux == 2 ? (R["
                                << in.b << "].v2[1] " << sym << " R[" << in.c << "].f) : 0.0f);\n";
                        } else {
                            out << "    R[" << in.a << "] = pluma_val_vecn(R[" << in.c << "].aux, R[" << in.b << "].f "
                                << sym << " R[" << in.c << "].v2[0], R[" << in.c << "].aux == 2 ? (R[" << in.b
                                << "].f " << sym << " R[" << in.c << "].v2[1]) : 0.0f);\n";
                        }
                    } else {
                        out << "    G_API->vec_arith_op(&R[" << in.a << "], "
                            << unsigned(static_cast<std::uint8_t>(in.op)) << ", " << unsigned(in.k) << ", R[" << in.b
                            << "], R[" << in.c << "]);\n";
                    }
                } else {
                    out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                    out << "    G_API->dyn_arith_op(vm, &ctx, " << in.a << ", "
                        << unsigned(static_cast<std::uint8_t>(in.op)) << ", R[" << in.b << "], R[" << in.c << "]);\n";
                    out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                }
                break;
            }
            case Op::Neg: {
                if (in.k == static_cast<std::uint8_t>(NumKind::F32)) {
                    emit_store_val(out, a_obj, in.a, std::format("pluma_val_f32(-R[{}].f)", in.b));
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F64)) {
                    emit_store_val(out, a_obj, in.a, std::format("pluma_val_f64(-R[{}].d)", in.b));
                } else if (in.k < 8) {
                    auto tag = static_cast<unsigned>(num_tag(static_cast<NumKind>(in.k)));
                    out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                    emit_store_val(out, a_obj, in.a,
                                   std::format("pluma_val_int({}, G_API->int_neg(vm, {}, R[{}].i))", tag,
                                               unsigned(in.k), in.b));
                } else if (in.k == opk::Vec) {
                    if (!a_obj && !b_obj) {
                        out << "    R[" << in.a << "] = pluma_val_vecn(R[" << in.b << "].aux, -R[" << in.b
                            << "].v2[0], R[" << in.b << "].aux == 2 ? -R[" << in.b << "].v2[1] : 0.0f);\n";
                    } else {
                        out << "    G_API->vec_neg_op(&R[" << in.a << "], R[" << in.b << "]);\n";
                    }
                } else {
                    out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                    out << "    G_API->dyn_neg_op(vm, &ctx, " << in.a << ", R[" << in.b << "]);\n";
                    out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                }
                break;
            }
            case Op::BAnd:
            case Op::BOr:
            case Op::BXor:
            case Op::Shl:
            case Op::Shr: {
                if (in.k == opk::Bool) {
                    std::string expr = in.op == Op::BAnd  ? std::format("(R[{}].b && R[{}].b)", in.b, in.c)
                                       : in.op == Op::BOr ? std::format("(R[{}].b || R[{}].b)", in.b, in.c)
                                                          : std::format("(R[{}].b != R[{}].b)", in.b, in.c);
                    emit_store_val(out, a_obj, in.a, std::format("pluma_val_bool({})", expr));
                } else if (in.op == Op::BAnd || in.op == Op::BOr || in.op == Op::BXor) {
                    auto nk = static_cast<NumKind>(in.k);
                    auto tag = static_cast<unsigned>(num_tag(nk));
                    const char* bop = in.op == Op::BAnd ? "&" : in.op == Op::BOr ? "|" : "^";
                    std::string raw = std::format("((uint64_t)R[{}].i {} (uint64_t)R[{}].i)", in.b, bop, in.c);
                    emit_store_val(out, a_obj, in.a, std::format("pluma_val_int({}, {})", tag, wrap_c_expr(nk, raw)));
                } else {
                    auto tag = static_cast<unsigned>(num_tag(static_cast<NumKind>(in.k)));
                    out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                    emit_store_val(out, a_obj, in.a,
                                   std::format("pluma_val_int({}, G_API->int_bits(vm, {}, {}, R[{}].i, R[{}].i))", tag,
                                               unsigned(static_cast<std::uint8_t>(in.op)), unsigned(in.k), in.b, in.c));
                }
                break;
            }
            case Op::BNot: {
                auto nk = static_cast<NumKind>(in.k);
                auto tag = static_cast<unsigned>(num_tag(nk));
                std::string raw = std::format("(~(uint64_t)R[{}].i)", in.b);
                emit_store_val(out, a_obj, in.a, std::format("pluma_val_int({}, {})", tag, wrap_c_expr(nk, raw)));
                break;
            }
            case Op::Not:
                emit_store_val(out, a_obj, in.a, std::format("pluma_val_bool(!R[{}].b)", in.b));
                break;
            case Op::Eq:
            case Op::Ne:
            case Op::Lt:
            case Op::Le: {
                std::string cmp;
                if (in.k < 8) {
                    auto nk = static_cast<NumKind>(in.k);
                    if (in.op == Op::Eq) {
                        cmp = std::format("(R[{}].i == R[{}].i)", in.b, in.c);
                    } else if (in.op == Op::Ne) {
                        cmp = std::format("(R[{}].i != R[{}].i)", in.b, in.c);
                    } else if (is_signed(nk)) {
                        cmp = std::format("(R[{}].i {} R[{}].i)", in.b, in.op == Op::Lt ? "<" : "<=", in.c);
                    } else {
                        cmp = std::format("((uint64_t)R[{}].i {} (uint64_t)R[{}].i)", in.b,
                                          in.op == Op::Lt ? "<" : "<=", in.c);
                    }
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F32)) {
                    cmp = in.op == Op::Eq   ? std::format("(R[{}].f == R[{}].f)", in.b, in.c)
                          : in.op == Op::Ne ? std::format("!(R[{}].f == R[{}].f)", in.b, in.c)
                          : in.op == Op::Lt ? std::format("(R[{}].f < R[{}].f)", in.b, in.c)
                                            : std::format("(R[{}].f <= R[{}].f)", in.b, in.c);
                } else if (in.k == static_cast<std::uint8_t>(NumKind::F64)) {
                    cmp = in.op == Op::Eq   ? std::format("(R[{}].d == R[{}].d)", in.b, in.c)
                          : in.op == Op::Ne ? std::format("!(R[{}].d == R[{}].d)", in.b, in.c)
                          : in.op == Op::Lt ? std::format("(R[{}].d < R[{}].d)", in.b, in.c)
                                            : std::format("(R[{}].d <= R[{}].d)", in.b, in.c);
                } else if (in.k == opk::Bool) {
                    cmp = std::format("(R[{}].b {} R[{}].b)", in.b, in.op == Op::Ne ? "!=" : "==", in.c);
                } else if (in.k == opk::Name || in.k == opk::Enum) {
                    cmp = std::format("(R[{}].i {} R[{}].i)", in.b, in.op == Op::Ne ? "!=" : "==", in.c);
                }
                if (!cmp.empty()) {
                    emit_store_val(out, a_obj, in.a, std::format("pluma_val_bool({})", cmp));
                } else {
                    out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                    out << "    {\n";
                    out << "        bool cr = G_API->cmp_val(vm, &ctx, " << unsigned(static_cast<std::uint8_t>(in.op))
                        << ", " << unsigned(in.k) << ", R[" << in.b << "], R[" << in.c << "]);\n";
                    out << "        R = ctx.R; K = ctx.K; G = ctx.G;\n";
                    emit_store_val(out, a_obj, in.a, "pluma_val_bool(cr)");
                    out << "    }\n";
                }
                break;
            }
            case Op::Cast:
                out << "    G_API->cast_val(&R[" << in.a << "], R[" << in.b << "], " << unsigned(in.c & 0xff) << ", "
                    << unsigned(in.k) << ");\n";
                break;
            case Op::Downcast:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->downcast(vm, &R[" << in.a << "], R[" << in.b << "], " << in.c << ");\n";
                break;
            case Op::InstOf:
                emit_store_val(out, a_obj, in.a,
                               std::format("pluma_val_bool(G_API->inst_of(vm, R[{}], {}))", in.b, in.c));
                break;
            case Op::Variant:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                emit_store_val(out, a_obj, in.a,
                               std::format("pluma_val_int({}, G_API->variant_of(vm, R[{}]))",
                                           unsigned(num_tag(NumKind::I32)), in.b));
                break;
            case Op::GetPay:
                out << "    G_API->get_pay(&R[" << in.a << "], R[" << in.b << "], " << in.c << ");\n";
                break;
            case Op::Jmp:
                out << "    goto L_" << in.target() << ";\n";
                break;
            case Op::JmpF:
                out << "    if (!R[" << in.a << "].b) goto L_" << in.target() << ";\n";
                break;
            case Op::JmpT:
                out << "    if (R[" << in.a << "].b) goto L_" << in.target() << ";\n";
                break;
            case Op::JmpNull:
                out << "    if (R[" << in.a << "].tag == PLUMA_TAG_NULL || R[" << in.a
                    << "].tag == PLUMA_TAG_NIL) goto L_" << in.target() << ";\n";
                break;
            case Op::JmpNN:
                out << "    if (R[" << in.a << "].tag != PLUMA_TAG_NULL && R[" << in.a
                    << "].tag != PLUMA_TAG_NIL) goto L_" << in.target() << ";\n";
                break;
            case Op::ToStr:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->to_str(vm, &ctx, " << in.a << ", R[" << in.b << "]);\n";
                out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                break;
            case Op::Concat:
                out << "    G_API->concat(&R[" << in.a << "], &R[" << in.b << "], " << in.c << ");\n";
                break;
            case Op::Call:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    if (!G_API->call_proto(vm, &ctx, " << in.b << ", " << in.a << ", " << in.c << ", "
                    << unsigned(in.k) << ")) return false;\n";
                out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                break;
            case Op::CallV:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    if (!G_API->call_vtable(vm, &ctx, " << in.b << ", " << in.a << ", " << in.c << ", "
                    << unsigned(in.k) << ")) return false;\n";
                out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                break;
            case Op::CallC:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    if (!G_API->call_closure(vm, &ctx, " << in.b << ", " << in.a << ", " << in.c
                    << ")) return false;\n";
                out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                break;
            case Op::CallN:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->call_builtin(vm, &ctx, " << in.a << ", " << in.b << ", " << in.c << ", "
                    << unsigned(in.k) << ");\n";
                out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                break;
            case Op::CallX:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->call_host(vm, &ctx, " << in.a << ", G_NATIVE_IDS[" << in.b << "], " << in.c << ", "
                    << unsigned(in.k) << ");\n";
                out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                break;
            case Op::CallD:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    if (!G_API->call_dyn(vm, &ctx, " << in.b << ", " << in.a << ", " << in.c
                    << ")) return false;\n";
                out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                break;
            case Op::Ret:
                out << "    G_API->ret(vm, R[" << in.a << "], out);\n";
                out << "    return true;\n";
                break;
            case Op::Ret0:
                out << "    G_API->ret0(vm, out);\n";
                out << "    return true;\n";
                break;
            case Op::ForPrep: {
                auto nk = static_cast<NumKind>(in.k & 0x7f);
                bool inc = (in.k & opk::Inclusive) != 0;
                if (is_signed(nk) && nk != NumKind::I64) {
                    const char* up_op = inc ? "<=" : "<";
                    const char* dn_op = inc ? ">=" : ">";
                    out << "    if (__builtin_expect(R[" << (in.a + 2) << "].i == 0, 0)) { *ctx.pc_ptr = " << next_pc
                        << "; G_API->panic_cstr(vm, \"a range step cannot be zero\"); }\n";
                    out << "    if (R[" << (in.a + 2) << "].i > 0 ? !(R[" << in.a << "].i " << up_op << " R["
                        << (in.a + 1) << "].i) : !(R[" << in.a << "].i " << dn_op << " R[" << (in.a + 1)
                        << "].i)) goto L_" << in.target() << ";\n";
                } else {
                    out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                    out << "    if (!G_API->for_prep(vm, &R[" << in.a << "], " << unsigned(in.k) << ")) goto L_"
                        << in.target() << ";\n";
                }
                break;
            }
            case Op::ForLoop: {
                auto nk = static_cast<NumKind>(in.k & 0x7f);
                bool inc = (in.k & opk::Inclusive) != 0;
                if (is_signed(nk) && nk != NumKind::I64) {
                    const char* up_op = inc ? "<=" : "<";
                    const char* dn_op = inc ? ">=" : ">";
                    out << "    {\n";
                    out << "        int64_t r = R[" << in.a << "].i + R[" << (in.a + 2) << "].i;\n";
                    out << "        int64_t next = " << wrap_c_expr(nk, "r") << ";\n";
                    out << "        if (next == r && (R[" << (in.a + 2) << "].i > 0 ? (next " << up_op << " R["
                        << (in.a + 1) << "].i) : (next " << dn_op << " R[" << (in.a + 1) << "].i))) {\n";
                    out << "            R[" << in.a << "].i = next;\n";
                    out << "            goto L_" << in.target() << ";\n";
                    out << "        }\n";
                    out << "    }\n";
                } else {
                    out << "    if (G_API->for_loop(&R[" << in.a << "], " << unsigned(in.k) << ")) goto L_"
                        << in.target() << ";\n";
                }
                break;
            }
            case Op::ListNext:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    if (G_API->list_next(vm, &R[" << in.a << "], R[" << in.b << "], R[" << in.c << "].i, "
                    << unsigned(in.k) << ")) goto L_" << (i + 2) << ";\n";
                break;
            case Op::Inc:
                out << "    R[" << in.a << "].i += 1;\n";
                break;
            case Op::IterNext:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    if (G_API->iter_next(vm, &R[" << in.a << "], R[" << in.b << "], &R[" << in.c << "], "
                    << unsigned(in.k) << ")) goto L_" << (i + 2) << ";\n";
                break;
            case Op::Await:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    if (!G_API->await_op(vm, &ctx, " << unsigned(in.k) << ", " << in.a << ", " << in.b
                    << ")) return false;\n";
                out << "    R = ctx.R; K = ctx.K; G = ctx.G;\n";
                break;
            case Op::VecNew:
                if (in.k == 2 && in.c == 2 && !a_obj && !b_obj &&
                    (static_cast<std::size_t>(in.b + 1) < reg_obj.size() && !reg_obj[in.b + 1])) {
                    out << "    R[" << in.a << "] = pluma_val_vecn(2, R[" << in.b << "].f, R[" << (in.b + 1)
                        << "].f);\n";
                } else {
                    out << "    G_API->vec_new_op(&R[" << in.a << "], &R[" << in.b << "], " << in.c << ", "
                        << unsigned(in.k) << ");\n";
                }
                break;
            case Op::Swz:
                if (in.c == 1 && !a_obj && !b_obj) {
                    emit_store_val(out, false, in.a, std::format("pluma_val_f32(R[{}].v2[{}])", in.b, in.k & 3));
                } else if (in.c == 2 && !a_obj && !b_obj) {
                    out << "    R[" << in.a << "] = pluma_val_vecn(2, R[" << in.b << "].v2[" << (in.k & 3) << "], R["
                        << in.b << "].v2[" << ((in.k >> 2) & 3) << "]);\n";
                } else {
                    out << "    G_API->swz_op(&R[" << in.a << "], R[" << in.b << "], " << in.c << ", "
                        << unsigned(in.k) << ");\n";
                }
                break;
            case Op::VecWith:
                out << "    G_API->vec_with_op(&R[" << in.a << "], R[" << in.b << "], R[" << in.c << "], "
                    << unsigned(in.k) << ");\n";
                break;
            case Op::VecIdx:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                emit_store_val(out, a_obj, in.a,
                               std::format("pluma_val_f32(G_API->vec_idx_op(vm, R[{}], R[{}].i))", in.b, in.c));
                break;
            case Op::NewSig:
                out << "    G_API->new_sig_op(&R[" << in.a << "]);\n";
                break;
            case Op::Free:
                out << "    *ctx.pc_ptr = " << next_pc << ";\n";
                out << "    G_API->free_op(vm, R[" << in.a << "]);\n";
                break;
        }
    }
    if (has_label[n]) out << "L_" << n << ": ;\n";
    out << "    G_API->ret0(vm, out);\n";
    out << "    return true;\n";
    out << "}\n\n";
}

} // namespace

std::string emit_c(const Program& program, const VM& vm) {
    std::ostringstream out;
    out << R"(// Generated by Pluma AOT compiler (DESIGN.md 5.12). Do not edit.
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct PlumaVM PlumaVM;
typedef struct PlumaRunResult PlumaRunResult;

enum {
    PLUMA_TAG_NIL = 0,
    PLUMA_TAG_NULL = 1,
    PLUMA_TAG_BOOL = 2,
    PLUMA_TAG_I8 = 3,
    PLUMA_TAG_I16 = 4,
    PLUMA_TAG_I32 = 5,
    PLUMA_TAG_I64 = 6,
    PLUMA_TAG_U8 = 7,
    PLUMA_TAG_U16 = 8,
    PLUMA_TAG_U32 = 9,
    PLUMA_TAG_U64 = 10,
    PLUMA_TAG_F32 = 11,
    PLUMA_TAG_F64 = 12,
    PLUMA_TAG_NAME = 13,
    PLUMA_TAG_ENUM = 14,
    PLUMA_TAG_OBJ = 15,
    PLUMA_TAG_VEC = 16
};

typedef struct PlumaObject {
    uint32_t rc;
    uint8_t kind;
    uint8_t flags;
    uint16_t gen;
} PlumaObject;

typedef struct PlumaValue {
    uint8_t tag;
    uint8_t ref;
    uint16_t pad1;
    uint32_t aux;
    union {
        int64_t i;
        float f;
        double d;
        bool b;
        PlumaObject* o;
        float v2[2];
    };
} PlumaValue;

typedef struct PlumaAotCtx {
    PlumaValue* R;
    const PlumaValue* K;
    PlumaValue* G;
    uint32_t* pc_ptr;
    uint8_t wrap_overflow;
} PlumaAotCtx;

typedef bool (*PlumaAotFn)(PlumaVM*, PlumaValue*);

typedef struct PlumaAotFieldDesc {
    const char* name;
    const char* type;
    uint8_t exported;
    uint8_t weak;
} PlumaAotFieldDesc;

typedef struct PlumaAotMethodDesc {
    const char* name;
    uint32_t proto;
} PlumaAotMethodDesc;

typedef struct PlumaAotRecordDesc {
    const char* name;
    uint8_t is_struct;
    int32_t base_index;
    uint32_t field_init;
    uint8_t gc_flags;
    uint32_t vtable_count;
    const uint32_t* vtable;
    uint32_t method_count;
    const PlumaAotMethodDesc* methods;
    uint32_t field_count;
    const PlumaAotFieldDesc* fields;
} PlumaAotRecordDesc;

typedef struct PlumaAotVariantDesc {
    const char* name;
    int64_t discriminant;
    uint32_t field_count;
    const char* const* field_names;
} PlumaAotVariantDesc;

typedef struct PlumaAotEnumDesc {
    const char* name;
    uint8_t plain;
    uint8_t gc_flags;
    uint32_t variant_count;
    const PlumaAotVariantDesc* variants;
} PlumaAotEnumDesc;

typedef struct PlumaAotGlobalDesc {
    const char* name;
    const char* type;
} PlumaAotGlobalDesc;

typedef struct PlumaAotConstDesc {
    uint8_t tag;
    uint32_t aux;
    int64_t bits;
    const char* str;
} PlumaAotConstDesc;

typedef struct PlumaAotLocalDesc {
    const char* name;
    const char* type;
    uint16_t reg;
    uint32_t begin;
    uint32_t end;
} PlumaAotLocalDesc;

typedef struct PlumaAotSpanLocDesc {
    const char* file;
    uint32_t line;
    uint32_t column;
    const char* text;
} PlumaAotSpanLocDesc;

typedef struct PlumaAotProtoDesc {
    const char* name;
    const char* key;
    uint32_t index;
    uint16_t param_count;
    uint16_t reg_count;
    uint16_t capture_count;
    uint32_t const_count;
    const PlumaAotConstDesc* consts;
    uint32_t local_count;
    const PlumaAotLocalDesc* locals;
    uint32_t span_count;
    const uint32_t* span_indices;
    PlumaAotFn fn;
} PlumaAotProtoDesc;

typedef struct PlumaAotImageDesc {
    uint32_t name_count;
    const char* const* names;
    uint32_t class_count;
    const PlumaAotRecordDesc* classes;
    uint32_t struct_count;
    const PlumaAotRecordDesc* structs;
    uint32_t enum_count;
    const PlumaAotEnumDesc* enums;
    uint32_t global_count;
    const PlumaAotGlobalDesc* globals;
    uint32_t native_count;
    const char* const* native_names;
    uint32_t* native_ids;
    uint32_t span_pool_count;
    const PlumaAotSpanLocDesc* span_pool;
    uint32_t proto_count;
    const PlumaAotProtoDesc* protos;
    uint32_t chunk_count;
    const uint32_t* chunks;
} PlumaAotImageDesc;

typedef struct PlumaAotApi {
    void (*enter)(PlumaVM*, PlumaAotCtx*);
    void (*free_obj)(PlumaObject*);
    void (*root_obj)(PlumaObject*);
    void (*panic_cstr)(PlumaVM*, const char*);
    void (*overflow)(PlumaVM*, uint8_t, uint8_t);
    void (*get_up)(PlumaVM*, PlumaValue*, uint16_t);
    void (*get_up_c)(PlumaVM*, PlumaValue*, uint16_t);
    void (*set_up_c)(PlumaVM*, uint16_t, PlumaValue);
    void (*new_cell_op)(PlumaValue*, PlumaValue, uint8_t);
    void (*get_cell)(PlumaValue*, PlumaValue);
    void (*set_cell)(PlumaValue, PlumaValue);
    void (*new_closure_op)(PlumaVM*, PlumaValue*, uint16_t, const PlumaValue*);
    void (*get_f)(PlumaVM*, PlumaValue*, PlumaValue, uint16_t, uint8_t);
    void (*set_f)(PlumaVM*, PlumaValue, uint16_t, PlumaValue, uint8_t);
    void (*get_i)(PlumaVM*, PlumaValue*, PlumaValue, int64_t, uint8_t);
    void (*set_i)(PlumaVM*, PlumaValue, int64_t, PlumaValue, uint8_t);
    void (*new_list_op)(PlumaVM*, PlumaValue*, const PlumaValue*, uint16_t, uint8_t);
    void (*new_obj_op)(PlumaVM*, PlumaValue*, uint16_t, uint8_t);
    void (*new_struct_op)(PlumaVM*, PlumaValue*, uint16_t, const PlumaValue*);
    void (*new_enum_op)(PlumaVM*, PlumaValue*, uint16_t, uint8_t, const PlumaValue*);
    void (*copy_val)(PlumaVM*, PlumaValue*, PlumaValue);
    int64_t (*int_arith)(PlumaVM*, uint8_t, uint8_t, int64_t, int64_t);
    int64_t (*int_neg)(PlumaVM*, uint8_t, int64_t);
    int64_t (*int_bits)(PlumaVM*, uint8_t, uint8_t, int64_t, int64_t);
    void (*vec_arith_op)(PlumaValue*, uint8_t, uint8_t, PlumaValue, PlumaValue);
    void (*vec_neg_op)(PlumaValue*, PlumaValue);
    void (*dyn_arith_op)(PlumaVM*, PlumaAotCtx*, uint16_t, uint8_t, PlumaValue, PlumaValue);
    void (*dyn_neg_op)(PlumaVM*, PlumaAotCtx*, uint16_t, PlumaValue);
    bool (*cmp_val)(PlumaVM*, PlumaAotCtx*, uint8_t, uint8_t, PlumaValue, PlumaValue);
    void (*cast_val)(PlumaValue*, PlumaValue, uint8_t, uint8_t);
    void (*downcast)(PlumaVM*, PlumaValue*, PlumaValue, uint16_t);
    bool (*inst_of)(PlumaVM*, PlumaValue, uint16_t);
    int64_t (*variant_of)(PlumaVM*, PlumaValue);
    void (*get_pay)(PlumaValue*, PlumaValue, uint16_t);
    void (*to_str)(PlumaVM*, PlumaAotCtx*, uint16_t, PlumaValue);
    void (*concat)(PlumaValue*, const PlumaValue*, uint16_t);
    bool (*call_proto)(PlumaVM*, PlumaAotCtx*, uint16_t, uint16_t, uint16_t, uint8_t);
    bool (*call_vtable)(PlumaVM*, PlumaAotCtx*, uint16_t, uint16_t, uint16_t, uint8_t);
    bool (*call_closure)(PlumaVM*, PlumaAotCtx*, uint16_t, uint16_t, uint16_t);
    void (*call_builtin)(PlumaVM*, PlumaAotCtx*, uint16_t, uint16_t, uint16_t, uint8_t);
    void (*call_host)(PlumaVM*, PlumaAotCtx*, uint16_t, uint32_t, uint16_t, uint8_t);
    bool (*call_dyn)(PlumaVM*, PlumaAotCtx*, uint16_t, uint16_t, uint16_t);
    void (*ret)(PlumaVM*, PlumaValue, PlumaValue*);
    void (*ret0)(PlumaVM*, PlumaValue*);
    bool (*for_prep)(PlumaVM*, PlumaValue*, uint8_t);
    bool (*for_loop)(PlumaValue*, uint8_t);
    bool (*list_next)(PlumaVM*, PlumaValue*, PlumaValue, int64_t, uint8_t);
    bool (*await_op)(PlumaVM*, PlumaAotCtx*, uint8_t, uint16_t, uint16_t);
    void (*vec_new_op)(PlumaValue*, const PlumaValue*, uint16_t, uint8_t);
    void (*swz_op)(PlumaValue*, PlumaValue, uint16_t, uint8_t);
    void (*vec_with_op)(PlumaValue*, PlumaValue, PlumaValue, uint8_t);
    float (*vec_idx_op)(PlumaVM*, PlumaValue, int64_t);
    void (*new_sig_op)(PlumaValue*);
    void (*free_op)(PlumaVM*, PlumaValue);
    bool (*bind_image)(PlumaVM*, const PlumaAotImageDesc*);
    bool (*load_image)(PlumaVM*, const PlumaAotImageDesc*, PlumaRunResult*);
    bool (*iter_next)(PlumaVM*, PlumaValue*, PlumaValue, PlumaValue*, uint8_t);
} PlumaAotApi;

static const PlumaAotApi* G_API = NULL;

static inline PlumaValue pluma_val_nil(void) {
    PlumaValue v = {0};
    return v;
}
static inline PlumaValue pluma_val_null(void) {
    PlumaValue v = {0};
    v.tag = PLUMA_TAG_NULL;
    return v;
}
static inline PlumaValue pluma_val_bool(bool b) {
    PlumaValue v = {0};
    v.tag = PLUMA_TAG_BOOL;
    v.b = b;
    return v;
}
static inline PlumaValue pluma_val_int(uint8_t tag, int64_t i) {
    PlumaValue v = {0};
    v.tag = tag;
    v.i = i;
    return v;
}
static inline PlumaValue pluma_val_f32(float f) {
    PlumaValue v = {0};
    v.tag = PLUMA_TAG_F32;
    v.f = f;
    return v;
}
static inline PlumaValue pluma_val_f64(double d) {
    PlumaValue v = {0};
    v.tag = PLUMA_TAG_F64;
    v.d = d;
    return v;
}
static inline PlumaValue pluma_val_vecn(uint32_t aux, float x, float y) {
    PlumaValue v = {0};
    v.tag = PLUMA_TAG_VEC;
    v.aux = aux;
    v.v2[0] = x;
    v.v2[1] = y;
    return v;
}

static inline void pluma_retain(PlumaValue v) {
    if (v.tag == PLUMA_TAG_OBJ) ++v.o->rc;
}
static inline void pluma_release(PlumaValue v) {
    if (v.tag != PLUMA_TAG_OBJ) return;
    PlumaObject* o = v.o;
    if (--o->rc == 0) {
        G_API->free_obj(o);
    } else if ((o->flags & (0x04 | 0x08)) == 0) {
        G_API->root_obj(o);
    }
}
static inline void pluma_assign(PlumaValue* dst, PlumaValue src) {
    pluma_retain(src);
    PlumaValue old = *dst;
    *dst = src;
    pluma_release(old);
}
static inline void pluma_assign_owned(PlumaValue* dst, PlumaValue src) {
    PlumaValue old = *dst;
    *dst = src;
    pluma_release(old);
}

)";

    // Host native table
    std::size_t native_count = AotRuntime::vm_native_count(vm);
    out << "static const char* const G_NATIVE_NAMES[" << (native_count > 0 ? native_count : 1) << "] = {\n";
    for (std::size_t i = 0; i < native_count; ++i) {
        out << "    " << c_escape(AotRuntime::vm_native_name(vm, i)) << ",\n";
    }
    if (native_count == 0) out << "    NULL\n";
    out << "};\n";
    out << "static uint32_t G_NATIVE_IDS[" << (native_count > 0 ? native_count : 1) << "];\n\n";

    // Emit all Proto functions
    for (std::size_t i = 0; i < vm.protos.size(); ++i) {
        const Function* fn = i < program.functions.size() ? program.functions[i].get() : nullptr;
        emit_proto_fn(out, *vm.protos[i], fn);
    }

    // Interned names
    const auto& names = AotRuntime::vm_names(vm);
    out << "static const char* const G_NAMES[" << (names.empty() ? 1 : names.size()) << "] = {\n";
    for (const std::string& s : names) out << "    " << c_escape(s) << ",\n";
    if (names.empty()) out << "    NULL\n";
    out << "};\n\n";

    // Map RecordRT* to index in vm.classes
    std::map<const RecordRT*, std::int32_t> class_idx;
    for (std::size_t i = 0; i < vm.classes.size(); ++i) {
        class_idx[vm.classes[i].get()] = static_cast<std::int32_t>(i);
    }

    auto emit_records = [&](const char* prefix, const std::vector<std::unique_ptr<RecordRT>>& list) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            const RecordRT& r = *list[i];
            if (!r.vtable.empty()) {
                out << "static const uint32_t " << prefix << "_VT_" << i << "[] = {";
                for (std::uint32_t v : r.vtable) out << v << ", ";
                out << "};\n";
            }
            if (!r.methods.empty()) {
                out << "static const PlumaAotMethodDesc " << prefix << "_METH_" << i << "[] = {\n";
                for (const auto& [mname, mproto] : r.methods) {
                    out << "    {" << c_escape(mname) << ", " << mproto << "},\n";
                }
                out << "};\n";
            }
            if (!r.field_names.empty()) {
                out << "static const PlumaAotFieldDesc " << prefix << "_FLD_" << i << "[] = {\n";
                for (std::size_t f = 0; f < r.field_names.size(); ++f) {
                    out << "    {" << c_escape(r.field_names[f]) << ", "
                        << c_escape(f < r.field_types.size() ? r.field_types[f] : "") << ", "
                        << (f < r.field_exported.size() && r.field_exported[f] ? 1 : 0) << ", "
                        << (f < r.field_weak.size() && r.field_weak[f] ? 1 : 0) << "},\n";
                }
                out << "};\n";
            }
        }
        out << "static const PlumaAotRecordDesc G_" << prefix << "S[" << (list.empty() ? 1 : list.size()) << "] = {\n";
        for (std::size_t i = 0; i < list.size(); ++i) {
            const RecordRT& r = *list[i];
            std::int32_t base_i = r.base != nullptr && class_idx.count(r.base) ? class_idx.at(r.base) : -1;
            out << "    {" << c_escape(r.name) << ", " << (r.is_struct ? 1 : 0) << ", " << base_i << ", "
                << r.field_init << ", " << unsigned(r.gc_flags) << ", " << r.vtable.size() << ", "
                << (r.vtable.empty() ? "NULL" : std::format("{}_VT_{}", prefix, i)) << ", " << r.methods.size()
                << ", " << (r.methods.empty() ? "NULL" : std::format("{}_METH_{}", prefix, i)) << ", "
                << r.field_names.size() << ", "
                << (r.field_names.empty() ? "NULL" : std::format("{}_FLD_{}", prefix, i)) << "},\n";
        }
        if (list.empty()) out << "    {NULL, 0, -1, 0, 0, 0, NULL, 0, NULL, 0, NULL}\n";
        out << "};\n\n";
    };

    emit_records("CLASS", vm.classes);
    emit_records("STRUCT", vm.structs);

    // Enums
    for (std::size_t i = 0; i < vm.enums.size(); ++i) {
        const EnumRT& e = *vm.enums[i];
        for (std::size_t v = 0; v < e.variants.size(); ++v) {
            const VariantRT& vr = e.variants[v];
            if (!vr.field_names.empty()) {
                out << "static const char* const ENUM_VF_" << i << "_" << v << "[] = {";
                for (const std::string& fn : vr.field_names) out << c_escape(fn) << ", ";
                out << "};\n";
            }
        }
        if (!e.variants.empty()) {
            out << "static const PlumaAotVariantDesc ENUM_VAR_" << i << "[] = {\n";
            for (std::size_t v = 0; v < e.variants.size(); ++v) {
                const VariantRT& vr = e.variants[v];
                out << "    {" << c_escape(vr.name) << ", " << vr.discriminant << "LL, " << vr.field_names.size()
                    << ", " << (vr.field_names.empty() ? "NULL" : std::format("ENUM_VF_{}_{}", i, v)) << "},\n";
            }
            out << "};\n";
        }
    }
    out << "static const PlumaAotEnumDesc G_ENUMS[" << (vm.enums.empty() ? 1 : vm.enums.size()) << "] = {\n";
    for (std::size_t i = 0; i < vm.enums.size(); ++i) {
        const EnumRT& e = *vm.enums[i];
        out << "    {" << c_escape(e.name) << ", " << (e.plain ? 1 : 0) << ", " << unsigned(e.gc_flags) << ", "
            << e.variants.size() << ", " << (e.variants.empty() ? "NULL" : std::format("ENUM_VAR_{}", i)) << "},\n";
    }
    if (vm.enums.empty()) out << "    {NULL, 0, 0, 0, NULL}\n";
    out << "};\n\n";

    // Globals
    out << "static const PlumaAotGlobalDesc G_GLOBALS[" << (vm.global_names.empty() ? 1 : vm.global_names.size())
        << "] = {\n";
    for (std::size_t i = 0; i < vm.global_names.size(); ++i) {
        out << "    {" << c_escape(vm.global_names[i]) << ", "
            << c_escape(i < vm.global_types.size() ? vm.global_types[i] : "") << "},\n";
    }
    if (vm.global_names.empty()) out << "    {NULL, NULL}\n";
    out << "};\n\n";

    // Deduplicated SpanLoc pool across all Protos
    struct SpanKey {
        std::string file;
        std::uint32_t line = 0;
        std::uint32_t col = 0;
        std::string text;
        bool operator<(const SpanKey& o) const {
            return std::tie(file, line, col, text) < std::tie(o.file, o.line, o.col, o.text);
        }
    };
    std::vector<SpanKey> span_pool;
    std::map<SpanKey, std::uint32_t> span_map;
    auto intern_span = [&](SpanKey k) -> std::uint32_t {
        auto it = span_map.find(k);
        if (it != span_map.end()) return it->second;
        auto idx = static_cast<std::uint32_t>(span_pool.size());
        span_pool.push_back(k);
        span_map.emplace(std::move(k), idx);
        return idx;
    };
    intern_span(SpanKey{"", 0, 0, ""});

    std::vector<std::vector<std::uint32_t>> proto_spans(vm.protos.size());
    for (std::size_t i = 0; i < vm.protos.size(); ++i) {
        const Proto& p = *vm.protos[i];
        proto_spans[i].resize(p.code.size(), 0);
        for (std::size_t pc = 0; pc < p.code.size(); ++pc) {
            std::string_view file;
            std::uint32_t line = 0;
            std::uint32_t col = 0;
            if (!p.resolve_pc(static_cast<std::uint32_t>(pc), file, line, col)) continue;
            std::string_view slice;
            if (needs_slice_text(p.code[pc].op)) slice = p.slice_pc(static_cast<std::uint32_t>(pc));
            proto_spans[i][pc] = intern_span(SpanKey{std::string(file), line, col, std::string(slice)});
        }
    }

    out << "static const PlumaAotSpanLocDesc G_SPAN_POOL[" << span_pool.size() << "] = {\n";
    for (const SpanKey& sk : span_pool) {
        out << "    {" << c_escape(sk.file) << ", " << sk.line << ", " << sk.col << ", " << c_escape(sk.text)
            << "},\n";
    }
    out << "};\n\n";

    // Proto descriptors (consts, locals, span indices)
    for (std::size_t i = 0; i < vm.protos.size(); ++i) {
        const Proto& p = *vm.protos[i];
        if (!p.consts.empty()) {
            out << "static const PlumaAotConstDesc PROTO_K_" << i << "[] = {\n";
            for (const Value& v : p.consts) {
                if (v.tag == Tag::Obj && v.o->kind == ObjKind::Str) {
                    out << "    {" << unsigned(static_cast<std::uint8_t>(v.tag)) << ", 0, 0, "
                        << c_escape(static_cast<const StrObj*>(v.o)->s) << "},\n";
                } else {
                    out << "    {" << unsigned(static_cast<std::uint8_t>(v.tag)) << ", " << v.aux << ", "
                        << static_cast<std::int64_t>(std::bit_cast<std::uint64_t>(v.i)) << "LL, NULL},\n";
                }
            }
            out << "};\n";
        }
        if (!p.locals.empty()) {
            out << "static const PlumaAotLocalDesc PROTO_LOC_" << i << "[] = {\n";
            for (const LocalDebug& l : p.locals) {
                out << "    {" << c_escape(l.name) << ", " << c_escape(l.type) << ", " << l.reg << ", " << l.begin
                    << ", " << l.end << "},\n";
            }
            out << "};\n";
        }
        if (!proto_spans[i].empty()) {
            out << "static const uint32_t PROTO_SP_" << i << "[] = {";
            for (std::uint32_t si : proto_spans[i]) out << si << ", ";
            out << "};\n";
        }
    }

    out << "static const PlumaAotProtoDesc G_PROTOS[" << (vm.protos.empty() ? 1 : vm.protos.size()) << "] = {\n";
    for (std::size_t i = 0; i < vm.protos.size(); ++i) {
        const Proto& p = *vm.protos[i];
        out << "    {" << c_escape(p.name) << ", " << c_escape(p.key) << ", " << p.index << ", " << p.param_count
            << ", " << p.reg_count << ", " << p.capture_count << ", " << p.consts.size() << ", "
            << (p.consts.empty() ? "NULL" : std::format("PROTO_K_{}", i)) << ", " << p.locals.size() << ", "
            << (p.locals.empty() ? "NULL" : std::format("PROTO_LOC_{}", i)) << ", " << proto_spans[i].size() << ", "
            << (proto_spans[i].empty() ? "NULL" : std::format("PROTO_SP_{}", i)) << ", &aot_proto_" << i << "},\n";
    }
    if (vm.protos.empty()) out << "    {NULL, NULL, 0, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL, NULL}\n";
    out << "};\n\n";

    std::vector<std::uint32_t> chunks;
    for (const auto& fn : program.functions) {
        if (fn->kind == FnKind::Chunk) chunks.push_back(fn->index);
    }
    out << "static const uint32_t G_CHUNKS[" << (chunks.empty() ? 1 : chunks.size()) << "] = {";
    for (std::uint32_t ch : chunks) out << ch << ", ";
    if (chunks.empty()) out << "0";
    out << "};\n\n";

    out << "static const PlumaAotImageDesc G_IMAGE = {\n";
    out << "    " << names.size() << ", G_NAMES,\n";
    out << "    " << vm.classes.size() << ", G_CLASSS,\n";
    out << "    " << vm.structs.size() << ", G_STRUCTS,\n";
    out << "    " << vm.enums.size() << ", G_ENUMS,\n";
    out << "    " << vm.global_names.size() << ", G_GLOBALS,\n";
    out << "    " << native_count << ", G_NATIVE_NAMES, G_NATIVE_IDS,\n";
    out << "    " << span_pool.size() << ", G_SPAN_POOL,\n";
    out << "    " << vm.protos.size() << ", G_PROTOS,\n";
    out << "    " << chunks.size() << ", G_CHUNKS\n";
    out << "};\n\n";

    out << R"(#ifdef _WIN32
#define PLUMA_AOT_EXPORT __declspec(dllexport)
#else
#define PLUMA_AOT_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

PLUMA_AOT_EXPORT bool pluma_aot_bind(PlumaVM* vm, const PlumaAotApi* api) {
    G_API = api;
    return api->bind_image(vm, &G_IMAGE);
}

PLUMA_AOT_EXPORT bool pluma_aot_load(PlumaVM* vm, const PlumaAotApi* api, PlumaRunResult* out_result) {
    G_API = api;
    return api->load_image(vm, &G_IMAGE, out_result);
}

#ifdef __cplusplus
}
#endif
)";
    return out.str();
}

// ------------------------------------------------------------ compiler & loader

namespace {

std::atomic<std::uint64_t> g_temp_counter{0};

unsigned current_pid() {
#if defined(_WIN32)
    return static_cast<unsigned>(::_getpid());
#else
    return static_cast<unsigned>(::getpid());
#endif
}

#if defined(_WIN32)
std::string shell_quote_str(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}
#else
std::string shell_quote_str(std::string_view s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}
#endif

std::string shell_quote(const std::filesystem::path& p) { return shell_quote_str(p.string()); }

std::string default_c_compiler() {
    const char* cc_env = std::getenv("CC"); // NOLINT(concurrency-mt-unsafe)
    if (cc_env != nullptr && cc_env[0] != '\0') return cc_env;
#if defined(PLUMA_DEFAULT_CC)
    if (PLUMA_DEFAULT_CC[0] != '\0') return PLUMA_DEFAULT_CC;
#endif
#if defined(_WIN32)
    return "clang";
#else
    return "cc";
#endif
}

void close_lib(void* h) {
    if (h == nullptr) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(h));
#else
    ::dlclose(h);
#endif
}

} // namespace

bool compile_c_module(std::string_view c_source, const std::filesystem::path& so_path, std::string& error,
                      int opt_level) {
    std::error_code ec;
    if (!so_path.parent_path().empty()) std::filesystem::create_directories(so_path.parent_path(), ec);

    std::uint64_t id = ++g_temp_counter;
    std::filesystem::path tmp_dir =
        std::filesystem::temp_directory_path() / std::format("pluma_aot_build_{}_{}", current_pid(), id);
    std::filesystem::create_directories(tmp_dir, ec);
    std::filesystem::path c_path = tmp_dir / "pluma_aot.c";
    std::filesystem::path log_path = tmp_dir / "pluma_aot.log";
    // Compile into a fixed filename inside tmp_dir so PE/ELF headers that
    // record the output basename stay byte-identical across runs.
    std::filesystem::path built_path = tmp_dir / "pluma_aot.so";

    {
        std::ofstream f(c_path, std::ios::binary);
        if (!f) {
            std::filesystem::remove_all(tmp_dir, ec);
            error = std::format("cannot write temporary C file '{}'", c_path.string());
            return false;
        }
        f.write(c_source.data(), static_cast<std::streamsize>(c_source.size()));
    }

    std::string cc = default_c_compiler();
    int clamped_opt = std::clamp(opt_level, 0, 3);
#if defined(_WIN32)
    std::string inner_cmd = std::format(
        "{} -std=c11 -O{} -g0 -fno-ident -ffile-prefix-map={}={} "
        "-ffp-contract=off -fno-fast-math -shared -Wall -Wextra -Wno-unused-label "
        "-Wno-unused-parameter -Wl,/BREPRO -Wl,/DEBUG:NONE -Wl,/NOIMPLIB -o {} {} >{} 2>&1",
        shell_quote_str(cc), clamped_opt, shell_quote(tmp_dir), shell_quote_str("."), shell_quote(built_path),
        shell_quote(c_path), shell_quote(log_path));
    // cmd.exe strips outer quotes when a command both starts and ends with '"'
    // unless the whole command is wrapped in an extra pair of quotes.
    std::string cmd = "\"" + inner_cmd + "\"";
#else
    std::string cmd = std::format(
        "{} -std=c11 -O{} -g0 -fno-ident -Wl,--build-id=none -ffile-prefix-map={}={} "
        "-ffp-contract=off -fno-fast-math -fPIC -shared -Wall -Wextra -Wno-unused-label "
        "-Wno-unused-parameter -o {} {} -lm >{} 2>&1",
        shell_quote_str(cc), clamped_opt, shell_quote(tmp_dir), shell_quote_str("."), shell_quote(built_path),
        shell_quote(c_path), shell_quote(log_path));
#endif

    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::ifstream lf(log_path);
        std::ostringstream ss;
        ss << lf.rdbuf();
        std::filesystem::remove_all(tmp_dir, ec);
        error = std::format("C compiler failed (exit {}): {}", rc, ss.str());
        return false;
    }
    std::filesystem::copy_file(built_path, so_path, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove_all(tmp_dir, ec);
    if (ec) {
        error = std::format("cannot copy compiled module to '{}': {}", so_path.string(), ec.message());
        return false;
    }
    return true;
}

AotModule::~AotModule() {
    close_lib(handle_);
    if (!temp_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(temp_path_, ec);
    }
}

AotModule::AotModule(AotModule&& other) noexcept
    : handle_(other.handle_),
      bind_fn_(other.bind_fn_),
      load_fn_(other.load_fn_),
      temp_path_(std::move(other.temp_path_)) {
    other.handle_ = nullptr;
    other.bind_fn_ = nullptr;
    other.load_fn_ = nullptr;
}

AotModule& AotModule::operator=(AotModule&& other) noexcept {
    if (this == &other) return *this;
    close_lib(handle_);
    if (!temp_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(temp_path_, ec);
    }
    handle_ = other.handle_;
    bind_fn_ = other.bind_fn_;
    load_fn_ = other.load_fn_;
    temp_path_ = std::move(other.temp_path_);
    other.handle_ = nullptr;
    other.bind_fn_ = nullptr;
    other.load_fn_ = nullptr;
    return *this;
}

std::unique_ptr<AotModule> AotModule::open(const std::filesystem::path& so_path, std::string& error) {
#if defined(_WIN32)
    HMODULE h = ::LoadLibraryW(so_path.c_str());
    if (h == nullptr) {
        DWORD err_code = ::GetLastError();
        error = std::format("LoadLibraryW('{}') failed (error {})", so_path.string(), err_code);
        return nullptr;
    }
    auto bind_sym = ::GetProcAddress(h, "pluma_aot_bind");
    auto load_sym = ::GetProcAddress(h, "pluma_aot_load");
    if (bind_sym == nullptr || load_sym == nullptr) {
        ::FreeLibrary(h);
        error = std::format("'{}' is missing pluma_aot_bind/pluma_aot_load symbols", so_path.string());
        return nullptr;
    }
    auto mod = std::make_unique<AotModule>();
    mod->handle_ = static_cast<void*>(h);
    std::memcpy(&mod->bind_fn_, &bind_sym, sizeof(bind_sym));
    std::memcpy(&mod->load_fn_, &load_sym, sizeof(load_sym));
    return mod;
#else
    void* h = ::dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        const char* dl_err = ::dlerror();
        error = std::format("dlopen('{}') failed: {}", so_path.string(), dl_err != nullptr ? dl_err : "unknown error");
        return nullptr;
    }
    void* bind_sym = ::dlsym(h, "pluma_aot_bind");
    void* load_sym = ::dlsym(h, "pluma_aot_load");
    if (bind_sym == nullptr || load_sym == nullptr) {
        ::dlclose(h);
        error = std::format("'{}' is missing pluma_aot_bind/pluma_aot_load symbols", so_path.string());
        return nullptr;
    }
    auto mod = std::make_unique<AotModule>();
    mod->handle_ = h;
    std::memcpy(&mod->bind_fn_, &bind_sym, sizeof(bind_sym));
    std::memcpy(&mod->load_fn_, &load_sym, sizeof(load_sym));
    return mod;
#endif
}

std::unique_ptr<AotModule> AotModule::open_bytes(std::span<const std::byte> so_bytes, std::string& error) {
    std::uint64_t id = ++g_temp_counter;
    std::filesystem::path so_path =
        std::filesystem::temp_directory_path() / std::format("pluma_aot_mem_{}_{}.so", current_pid(), id);
    {
        std::ofstream f(so_path, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(so_bytes.data()),
                           static_cast<std::streamsize>(so_bytes.size()))) {
            error = std::format("cannot write temporary AOT shared library '{}'", so_path.string());
            return nullptr;
        }
    }
    auto mod = open(so_path, error);
    if (!mod) {
        std::error_code ec;
        std::filesystem::remove(so_path, ec);
        return nullptr;
    }
    mod->temp_path_ = std::move(so_path);
    return mod;
}

bool AotModule::bind(VM& vm, std::string& error) const {
    if (bind_fn_ == nullptr) {
        error = "AOT module is not open";
        return false;
    }
    if (!bind_fn_(&vm, &g_aot_api)) {
        error = "AOT module proto count does not match the VM";
        return false;
    }
    return true;
}

RunResult AotModule::load(VM& vm, std::string& error) const {
    RunResult r;
    if (load_fn_ == nullptr) {
        r.ok = false;
        error = "AOT module is not open";
        return r;
    }
    load_fn_(&vm, &g_aot_api, &r);
    return r;
}

std::unique_ptr<AotModule> compile_aot_module(const Program& program, const VM& vm, std::string& error,
                                              int opt_level) {
    std::uint64_t id = ++g_temp_counter;
    std::filesystem::path so_path =
        std::filesystem::temp_directory_path() / std::format("pluma_aot_{}_{}.so", current_pid(), id);
    std::string c_src = emit_c(program, vm);
    if (!compile_c_module(c_src, so_path, error, opt_level)) return nullptr;
    auto mod = AotModule::open(so_path, error);
    if (!mod) {
        std::error_code ec;
        std::filesystem::remove(so_path, ec);
        return nullptr;
    }
    mod->temp_path_ = std::move(so_path);
    return mod;
}

} // namespace pluma

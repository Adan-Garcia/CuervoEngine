#pragma once

// Semantic model shared by the type checker, the typed IR (tir.hpp), the
// bytecode compiler and (later) the AOT-to-C emitter: interned types and the
// program entities (classes, structs, enums, traits, functions, variables).
//
// Types are interned in a TypeTable, so two types are equal iff their pointers
// are equal. Generic types are type-erased at run time (DESIGN.md 5.5): a
// generic function or class is checked and compiled once, with its type
// parameters as opaque `Param` types; uses substitute concrete arguments.

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/arena.hpp"
#include "pluma/ast.hpp"
#include "pluma/source.hpp"

namespace pluma {

namespace tir {
struct Expr;
struct Stmt;
struct Block;
} // namespace tir

// Where an object is allocated (DESIGN.md 5.10): automatic (refcounted) or
// one of the manual kinds. Shared by the typed IR and the runtime.
enum class ArenaKind : std::uint8_t { None = 0, Alloc = 1, Frame = 2, Scene = 3 };

// Numeric kinds (DESIGN.md 5.4). `int` is I32 and `float` is F32.
enum class NumKind : std::uint8_t { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

constexpr bool is_float(NumKind k) { return k == NumKind::F32 || k == NumKind::F64; }
constexpr bool is_signed(NumKind k) {
    return k == NumKind::I8 || k == NumKind::I16 || k == NumKind::I32 || k == NumKind::I64;
}
constexpr int bit_width(NumKind k) {
    switch (k) {
        case NumKind::I8:
        case NumKind::U8: return 8;
        case NumKind::I16:
        case NumKind::U16: return 16;
        case NumKind::I32:
        case NumKind::U32:
        case NumKind::F32: return 32;
        default: return 64;
    }
}
std::string_view to_string(NumKind k);

struct ClassInfo;
struct StructInfo;
struct EnumInfo;
struct TraitInfo;
struct GenericParamInfo;
struct Function;
struct FieldInfo;

enum class TypeTag : std::uint8_t {
    Error,    // after a reported error; compatible with everything
    Void,
    Null,     // type of the `null` literal
    Bool,
    Num,      // see Type::num
    Str,
    Name,     // interned `#name`
    Class,    // reference type; args = generic arguments
    Struct,   // value type; args = generic arguments
    Enum,
    Trait,    // a trait used as a type: any class/struct implementing it
    List,     // List<T>; args[0] = T. Reference type.
    Fn,       // args = parameter types, ret = return type. Reference type.
    Nullable, // T? for a value type T; args[0] = T (DESIGN.md 5.7, LANGUAGE.md clarification)
    Param,    // generic type parameter
    Vec,      // vecN: N floats (DESIGN.md 4.2, 5.4); a value type; see Type::dim
    Task,     // a started coroutine (DESIGN.md 5.9). Reference type.
    Signal,   // a class's signal; args = parameter types. Reference type.
    // Milestone 9.2a collections (LANGUAGE.md 9.16). Reference types except
    // Array and Result.
    Map,        // Map<K, V>; args = {K, V}. Insertion-ordered hash map.
    Set,        // Set<T>; args = {T}. Insertion-ordered hash set.
    Array,      // [T; N]; args = {T}, dim = N. A value type (copied like a struct).
    StrBuilder, // a mutable string buffer
    Deque,      // Deque<T>; args = {T}. Ring buffer.
    PQueue,     // PriorityQueue<T>; args = {T}. Binary min-heap, FIFO among equals.
    Result,     // Result<T, E>; args = {T, E}. A value type: Ok(T) or Err(E) (DESIGN.md 5.8).
};

struct Type {
    TypeTag tag = TypeTag::Error;
    NumKind num = NumKind::I32;
    std::uint32_t dim = 0;      // Vec: number of components
    const void* decl = nullptr; // ClassInfo / StructInfo / EnumInfo / TraitInfo / GenericParamInfo
    std::vector<const Type*> args;
    const Type* ret = nullptr;

    ClassInfo* cls() const { return tag == TypeTag::Class ? static_cast<ClassInfo*>(const_cast<void*>(decl)) : nullptr; }
    StructInfo* st() const { return tag == TypeTag::Struct ? static_cast<StructInfo*>(const_cast<void*>(decl)) : nullptr; }
    EnumInfo* en() const { return tag == TypeTag::Enum ? static_cast<EnumInfo*>(const_cast<void*>(decl)) : nullptr; }
    TraitInfo* tr() const { return tag == TypeTag::Trait ? static_cast<TraitInfo*>(const_cast<void*>(decl)) : nullptr; }
    GenericParamInfo* param() const {
        return tag == TypeTag::Param ? static_cast<GenericParamInfo*>(const_cast<void*>(decl)) : nullptr;
    }

    bool is_error() const { return tag == TypeTag::Error; }
    bool is_void() const { return tag == TypeTag::Void; }
    bool is_num() const { return tag == TypeTag::Num; }
    bool is_int() const { return tag == TypeTag::Num && !is_float(num); }
    bool is_floating() const { return tag == TypeTag::Num && is_float(num); }
    // Reference types hold null and panic on null use (DESIGN.md 5.7).
    bool is_reference() const {
        return tag == TypeTag::Class || tag == TypeTag::List || tag == TypeTag::Fn || tag == TypeTag::Trait ||
               tag == TypeTag::Task || tag == TypeTag::Signal || tag == TypeTag::Map || tag == TypeTag::Set ||
               tag == TypeTag::StrBuilder || tag == TypeTag::Deque || tag == TypeTag::PQueue;
    }
    bool is_vec() const { return tag == TypeTag::Vec; }
    // Can hold null: reference types and T?.
    bool is_nullable() const { return is_reference() || tag == TypeTag::Nullable || tag == TypeTag::Null; }
};

// Interns types. Owned by a Program; pointers stay valid for its lifetime.
class TypeTable {
public:
    TypeTable();
    TypeTable(const TypeTable&) = delete;
    TypeTable& operator=(const TypeTable&) = delete;

    const Type* error() const { return error_; }
    const Type* void_() const { return void_t_; }
    const Type* null() const { return null_; }
    const Type* bool_() const { return bool_t_; }
    const Type* str() const { return str_; }
    const Type* name() const { return name_; }
    const Type* num(NumKind k) const { return nums_[static_cast<int>(k)]; }
    const Type* int_() const { return num(NumKind::I32); }
    const Type* float_() const { return num(NumKind::F32); }

    const Type* class_(ClassInfo* c, std::vector<const Type*> args = {});
    const Type* struct_(StructInfo* s, std::vector<const Type*> args = {});
    const Type* enum_(EnumInfo* e);
    const Type* trait(TraitInfo* t);
    const Type* list(const Type* element);
    const Type* fn(std::vector<const Type*> params, const Type* ret);
    // T? : identity for types that are already nullable.
    const Type* nullable(const Type* inner);
    const Type* param(GenericParamInfo* p);
    const Type* vec(std::uint32_t dim);
    const Type* task();
    const Type* signal(std::vector<const Type*> params);
    const Type* map(const Type* key, const Type* value);
    const Type* set(const Type* element);
    const Type* array(const Type* element, std::uint32_t size);
    const Type* str_builder();
    const Type* deque(const Type* element);
    const Type* pqueue(const Type* element);
    const Type* result(const Type* ok, const Type* err);
    // The same kind of type as `t` (a generic container) with new arguments.
    const Type* with_args(const Type* t, std::vector<const Type*> args);

    // `T?` -> `T`; reference types are unchanged.
    const Type* non_null(const Type* t) const {
        return t->tag == TypeTag::Nullable ? t->args[0] : t;
    }

private:
    const Type* intern(Type t);
    struct Key {
        TypeTag tag;
        NumKind num;
        std::uint32_t dim;
        const void* decl;
        std::vector<const Type*> args;
        const Type* ret;
        bool operator<(const Key& o) const;
    };
    std::map<Key, std::unique_ptr<Type>> types_;
    const Type* error_ = nullptr;
    const Type* void_t_ = nullptr;
    const Type* null_ = nullptr;
    const Type* bool_t_ = nullptr;
    const Type* str_ = nullptr;
    const Type* name_ = nullptr;
    const Type* nums_[10] = {};
};

std::string type_name(const Type* t);

// Generic parameter substitution: param -> argument.
using Subst = std::map<const GenericParamInfo*, const Type*>;
const Type* substitute(TypeTable& types, const Type* t, const Subst& subst);
bool contains_param(const Type* t);

// ---------------------------------------------------------------- entities

// Operators traits (LANGUAGE.md 2.8) plus Display/Default. User traits use
// BuiltinTrait::None.
enum class BuiltinTrait : std::uint8_t {
    None, Add, Sub, Mul, Div, Rem, Neg, Eq, Ord, Index, IndexSet, Iterable, Display, Default,
};

struct GenericParamInfo {
    std::string_view name;
    std::vector<TraitInfo*> bounds;
    Span span;
    std::uint32_t index = 0;
};

struct LocalVar {
    std::string_view name;
    const Type* type = nullptr;
    Span span;
    bool is_mutable = false;
    bool is_param = false;
    // A mutable local captured by a lambda lives in a heap cell (closures
    // capture by reference, LANGUAGE.md 2.4). Immutable ones are copied.
    bool captured = false;
    // Escape analysis (DESIGN.md 5.10): holds a `new(frame)` / `new(scene)`
    // object (set from its initializer and later assignments).
    ArenaKind holds = ArenaKind::None;
    std::uint32_t id = 0;
};

// A constant known at compile time (consts, literals, folded expressions).
struct ConstValue {
    enum class Kind : std::uint8_t { None, Int, Float, Bool, Str, Name, Null } kind = Kind::None;
    std::int64_t i = 0; // Int: value as i64 (unsigned kinds stored bit-identically)
    double f = 0.0;
    bool b = false;
    std::string_view s; // Str / Name (arena-owned)
};

struct GlobalVar {
    std::string_view name;
    std::string qualified; // "Boss.count" for statics
    const Type* type = nullptr;
    Decl* decl = nullptr;
    Span span;
    const SourceFile* source = nullptr;
    bool is_const = false;
    bool is_mutable = true;
    ConstValue value;               // consts
    tir::Expr* init = nullptr;      // checked initializer, if any
    std::uint32_t index = 0;        // slot in the VM's global table
    enum class State : std::uint8_t { Unresolved, Resolving, Done } state = State::Unresolved;
};

struct ParamInfo {
    std::string_view name;
    const Type* type = nullptr;
    const Param* ast = nullptr;
    tir::Expr* default_value = nullptr; // checked in the declaration's scope
    Span span;
};

// How a lambda reaches one captured variable.
struct CaptureInfo {
    LocalVar* var = nullptr;    // the original variable
    bool from_outer_capture = false; // source is the enclosing lambda's capture `outer_index`
    std::uint32_t outer_index = 0;
    bool by_cell() const { return var->is_mutable; }
};

enum class FnKind : std::uint8_t {
    Free,        // top-level `fn`
    Method,      // instance method (params[0] is self)
    Static,      // static method
    Lambda,
    FieldInit,   // synthesized: runs a class's field initializers (self param)
    StructInit,  // synthesized: builds a struct from its field initializers
    Chunk,       // top-level statements of a script / REPL input, or module global init
};

struct Function {
    std::string name; // qualified: "Boss.hurt", "<lambda@12:5>"
    FnKind kind = FnKind::Free;
    FnDecl* decl = nullptr;
    LambdaExpr* lambda = nullptr;
    void* owner = nullptr; // ClassInfo* / StructInfo* / TraitInfo* (see owner_kind)
    enum class Owner : std::uint8_t { None, Class, Struct, Trait } owner_kind = Owner::None;
    std::vector<GenericParamInfo*> generics;
    std::vector<ParamInfo> params;   // declared parameters (no self)
    std::vector<LocalVar*> param_vars; // runtime parameters, self first for methods
    const Type* ret = nullptr;
    tir::Block* body = nullptr;
    std::vector<CaptureInfo> captures; // lambdas
    const SourceFile* source = nullptr;
    Span span;
    bool is_final = false;
    bool is_abstract = false; // trait method without a body
    int vslot = -1;           // virtual slot for class methods
    Function* overrides = nullptr;
    std::uint32_t index = 0;  // in Program::functions; also the VM function index
    std::uint32_t local_count = 0;
    bool signature_done = false;
    bool body_done = false;
    // Uses `await`/`yield`: runs only as a coroutine, through `start(f())` or
    // `await f()` (DESIGN.md 5.9, LANGUAGE.md 9.10).
    bool is_coroutine = false;
    // `@native`: the body is a host function registered under `name`
    // (pluma/bindings.hpp).
    bool is_native = false;
    // The host function's name when it isn't `name`: an `@extern` C function
    // (pluma/ffi.hpp key "ffi|lib|symbol|signature", LANGUAGE.md 9.28).
    std::string native_name;
    // Identifies a lambda across recompiles (hot reload, DESIGN.md 5.13):
    // the enclosing function's name and the lambda's ordinal in it.
    std::string stable_key;
    // A property's `get` or `set(v)` (LANGUAGE.md 2.5, 9.19): inside it the
    // field's name means its storage.
    FieldInfo* accessor_of = nullptr;

    ClassInfo* owner_class() const { return owner_kind == Owner::Class ? static_cast<ClassInfo*>(owner) : nullptr; }
    StructInfo* owner_struct() const { return owner_kind == Owner::Struct ? static_cast<StructInfo*>(owner) : nullptr; }
    TraitInfo* owner_trait() const { return owner_kind == Owner::Trait ? static_cast<TraitInfo*>(owner) : nullptr; }
    bool has_self() const { return kind == FnKind::Method || kind == FnKind::FieldInit; }
};

struct FieldInfo {
    std::string_view name;
    const Type* type = nullptr; // may mention the owner's generic params
    VarDecl* decl = nullptr;
    Span span;
    std::uint32_t index = 0; // slot in the instance (inherited fields first)
    void* owner = nullptr;   // declaring ClassInfo / StructInfo
    tir::Expr* init = nullptr;
    bool init_checked = false;
    bool resolving = false;
    // `signal name(params)`: a field holding the signal object
    // (DESIGN.md 5.9); its type is Signal<params>.
    bool is_signal = false;
    std::vector<std::string_view> signal_params;
    // `@export` (LANGUAGE.md 2.10, 9.13): saved in scene files and shown in
    // the inspector. The hints are validated and kept for the editor.
    bool exported = false;
    // `weak var` (LANGUAGE.md 2.11): holds its object without keeping it
    // alive; reads null once the object is freed. Class instance fields only.
    bool weak = false;
    // Property accessors (LANGUAGE.md 2.5, 9.19): reads call `getter` and
    // assignments call `setter`, except inside the accessors themselves. A
    // computed property (a getter and no initializer) has no meaningful
    // storage: assigning it without a setter is an error.
    Function* getter = nullptr;
    Function* setter = nullptr;
    bool computed = false;
    struct ExportHints {
        bool has_range = false;
        double range_min = 0.0;
        double range_max = 0.0;
        double step = 0.0; // 0 = none
        std::string_view hint;
    } export_hints;
};

// Common part of classes and structs ("record" types with fields and methods).
struct RecordInfo {
    std::string_view name;
    bool is_class = false;
    const SourceFile* source = nullptr;
    Span span;
    std::vector<GenericParamInfo*> generics;
    std::vector<TraitInfo*> traits;
    std::vector<FieldInfo*> fields;               // all instance fields incl. inherited, by slot
    std::map<std::string_view, FieldInfo*> field_map; // own + inherited
    std::map<std::string_view, Function*> methods;    // own methods (incl. static)
    std::map<std::string_view, GlobalVar*> statics;   // own static vars
    const Type* self_type = nullptr; // with its own generic params as arguments
    std::uint32_t rt_index = 0;      // VM type index
    // No instance can be part of a reference cycle, so the cycle collector
    // never traces them (pluma/acyclic.hpp, DESIGN.md 5.10).
    bool acyclic = false;
    bool header_done = false;
    bool resolving = false;
    bool members_done = false;
};

struct ClassInfo : RecordInfo {
    ClassDecl* decl = nullptr;
    ClassInfo* base = nullptr;
    const Type* base_type = nullptr; // with generic arguments
    std::vector<Function*> vtable;
    Function* field_init = nullptr; // synthesized FieldInit
    Function* init = nullptr;       // own `init`, if declared
    // Trait default methods adopted by this class (or a base): their slot.
    std::map<const Function*, int> trait_default_slots;
    // `manual class` (DESIGN.md 5.10): `C()` allocates like `alloc C()`.
    bool manual = false;
    // `@acyclic`: the author promises no instance is ever part of a cycle.
    bool promised_acyclic = false;
    bool is_subclass_of(const ClassInfo* other) const {
        for (const ClassInfo* c = this; c != nullptr; c = c->base) {
            if (c == other) return true;
        }
        return false;
    }
};

struct StructInfo : RecordInfo {
    StructDecl* decl = nullptr;
    Function* default_ctor = nullptr; // synthesized StructInit: S {} with all defaults
};

struct VariantInfo {
    std::string_view name;
    std::vector<std::string_view> field_names;
    std::vector<const Type*> field_types;
    std::int64_t discriminant = 0; // plain enums
    Span span;
};

struct EnumInfo {
    std::string_view name;
    EnumDecl* decl = nullptr;
    const SourceFile* source = nullptr;
    Span span;
    std::vector<VariantInfo> variants;
    // Plain enums (no payloads anywhere) are integers at run time; others are
    // immutable heap objects.
    bool plain = true;
    NumKind backing = NumKind::I32;
    bool acyclic = false; // payload enums: see RecordInfo::acyclic
    const Type* type = nullptr;
    std::uint32_t rt_index = 0;
    bool done = false;
    int variant_index(std::string_view n) const {
        for (std::size_t i = 0; i < variants.size(); ++i) {
            if (variants[i].name == n) return static_cast<int>(i);
        }
        return -1;
    }
};

struct TraitInfo {
    std::string_view name;
    TraitDecl* decl = nullptr; // null for builtin traits
    BuiltinTrait builtin = BuiltinTrait::None;
    std::string_view op_method; // builtin: the method the operator calls
    std::map<std::string_view, Function*> methods;
    const SourceFile* source = nullptr;
    Span span;
    bool done = false;
};

// Everything known about a program across compilation units (a REPL session
// adds a unit per input). Names are global (DESIGN.md 5.11) within a
// namespace: the root one, or a package's (LANGUAGE.md 9.26).
struct Program {
    Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    TypeTable types;
    Arena arena; // typed IR nodes and decoded strings

    enum class SymKind : std::uint8_t { Class, Struct, Enum, Trait, Function, Global };
    struct Symbol {
        SymKind kind;
        void* ptr;
        // LANGUAGE.md 9.26: a package's names are keyed "pkg.Name" and only
        // `pub` ones are visible outside it. Packages see the root names of
        // preludes, never the game's own.
        std::string_view module = {}; // empty: the root namespace
        bool is_pub = false;
        bool prelude = false;
    };
    std::map<std::string, Symbol, std::less<>> globals;
    // Every package with a file in a checked unit (import targets).
    std::set<std::string, std::less<>> modules;

    std::vector<std::unique_ptr<ClassInfo>> classes;
    std::vector<std::unique_ptr<StructInfo>> structs;
    std::vector<std::unique_ptr<EnumInfo>> enums;
    std::vector<std::unique_ptr<TraitInfo>> traits;
    std::vector<std::unique_ptr<Function>> functions;
    std::vector<std::unique_ptr<GlobalVar>> global_vars;
    // `@test` functions of units checked with CheckOptions::tests, in source order.
    std::vector<Function*> tests;
    std::vector<std::unique_ptr<FieldInfo>> fields;
    std::vector<std::unique_ptr<LocalVar>> locals;
    std::vector<std::unique_ptr<GenericParamInfo>> generic_params;

    // Builtin operator traits, by BuiltinTrait.
    TraitInfo* builtin_trait(BuiltinTrait t) const { return builtin_traits_[static_cast<int>(t)]; }

    // Entities of units that failed to check: unregistered, but kept alive
    // because interned types may still point at them.
    std::vector<std::shared_ptr<void>> graveyard;

    // Host functions available to `@native` declarations (qualified names,
    // e.g. "Canvas.circle"); filled in from pluma::Bindings.
    std::set<std::string, std::less<>> native_names;

private:
    TraitInfo* builtin_traits_[16] = {};
};

} // namespace pluma

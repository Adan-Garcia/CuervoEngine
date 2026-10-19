#pragma once

// Typed IR: the checked, fully resolved program (DESIGN.md 5.12, "the same
// typed IR is emitted as C"). The type checker lowers the AST into it; the
// bytecode compiler consumes it today and an AOT-to-C emitter can later.
//
// Everything is explicit here: every expression has a type, names point at
// their entities, call arguments are in parameter order with defaults filled
// in, implicit `self` is spelled out, struct copies (value semantics) are
// `Copy` nodes, operator overloads are method calls, compound assignments read
// the target through a `Hole`, and lambdas list their captures.
//
// Nodes live in the Program's arena and are trivially destructible.

#include <cstdint>

#include "pluma/arena.hpp"
#include "pluma/ast.hpp"
#include "pluma/builtins.hpp"
#include "pluma/sema.hpp"

namespace pluma::tir {

struct Pattern;
struct Stmt;
struct Block;

enum class EK : std::uint8_t {
    Const,        // literal / folded constant
    Local,        // a local variable or parameter of the current function
    Capture,      // a captured variable inside a lambda
    Global,       // global or static variable
    FnRef,        // a function used as a value
    Unary,
    Binary,
    Logical,      // && ||
    Coalesce,     // ??
    Ternary,
    Cast,
    Call,         // direct call of a known function
    MethodCall,   // virtual (vslot >= 0) or direct method call; receiver first
    DynMethodCall,// by name, on a generic or trait-typed receiver
    ClosureCall,
    BuiltinCall,
    New,          // class construction
    StructNew,
    EnumNew,
    Field,
    Index,
    Lambda,
    ToStr,
    Concat,
    Is,           // pattern test; may bind
    Copy,         // copy of a struct value (value semantics)
    ListLit,
    SafeNav,      // a?.b : `body` evaluated with `tmp` = object when non-null
    Default,      // zero value of a type
    Hole,         // current value of an Assign's target (compound assignment)
    VecNew,       // vecN(parts...)
    Swizzle,      // v.x, v.yx
    VecWith,      // v with some components replaced (assignment to v.x / v.xy)
    Await,        // suspends the running coroutine (DESIGN.md 5.9)
    Start,        // start(coroutine call) -> Task
    Free,         // free(x): frees a manual object (DESIGN.md 5.10)
    Try,          // `x?`: unwraps a Result / optional or returns its error / null (DESIGN.md 5.8)
    BoundMethod,  // `obj.method` as a value: a closure over the receiver (LANGUAGE.md 9.19)
};

struct Expr {
    EK kind = EK::Const;
    const Type* type = nullptr;
    Span span;
};

template <class T>
T* as(Expr* e) {
    return e != nullptr && e->kind == T::node_kind ? static_cast<T*>(e) : nullptr;
}
template <class T>
const T* as(const Expr* e) {
    return e != nullptr && e->kind == T::node_kind ? static_cast<const T*>(e) : nullptr;
}

struct Const : Expr {
    static constexpr EK node_kind = EK::Const;
    ConstValue value;
};

struct Local : Expr {
    static constexpr EK node_kind = EK::Local;
    LocalVar* var = nullptr;
};

struct Capture : Expr {
    static constexpr EK node_kind = EK::Capture;
    std::uint32_t index = 0;
    LocalVar* var = nullptr; // the original variable (by_cell if mutable)
};

struct Global : Expr {
    static constexpr EK node_kind = EK::Global;
    GlobalVar* var = nullptr;
};

struct FnRef : Expr {
    static constexpr EK node_kind = EK::FnRef;
    Function* fn = nullptr;
};

struct Unary : Expr {
    static constexpr EK node_kind = EK::Unary;
    UnaryOp op = UnaryOp::Neg;
    Expr* operand = nullptr;
};

// Arithmetic, bitwise and comparison operators on built-in types. The
// operation is chosen by `operand_type` (numeric kind, bool, str, name,
// reference identity, plain enum, or a generic parameter = dynamic dispatch).
struct Binary : Expr {
    static constexpr EK node_kind = EK::Binary;
    BinaryOp op = BinaryOp::Add;
    const Type* operand_type = nullptr;
    // operand_type is a vecN and one side is a float: 1 = rhs (v * s, v / s), 2 = lhs (s * v).
    std::uint8_t vec_scalar = 0;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
};

struct Logical : Expr {
    static constexpr EK node_kind = EK::Logical;
    bool is_and = true;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
};

struct Coalesce : Expr {
    static constexpr EK node_kind = EK::Coalesce;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
};

struct Ternary : Expr {
    static constexpr EK node_kind = EK::Ternary;
    Expr* cond = nullptr;
    Expr* then_expr = nullptr;
    Expr* else_expr = nullptr;
};

struct Cast : Expr {
    static constexpr EK node_kind = EK::Cast;
    enum class Op : std::uint8_t { Numeric, Downcast } op = Op::Numeric;
    Expr* operand = nullptr; // numeric: from operand->type to type
};

struct Call : Expr {
    static constexpr EK node_kind = EK::Call;
    Function* fn = nullptr;
    List<Expr*> args;
};

struct MethodCall : Expr {
    static constexpr EK node_kind = EK::MethodCall;
    Function* fn = nullptr;
    bool is_virtual = false;
    int vslot = -1; // when virtual: slot in the receiver class's vtable
    Expr* receiver = nullptr;
    List<Expr*> args; // excluding the receiver
};

struct DynMethodCall : Expr {
    static constexpr EK node_kind = EK::DynMethodCall;
    std::string_view name;
    Expr* receiver = nullptr;
    List<Expr*> args;
};

struct ClosureCall : Expr {
    static constexpr EK node_kind = EK::ClosureCall;
    Expr* callee = nullptr;
    List<Expr*> args;
};

struct BuiltinCall : Expr {
    static constexpr EK node_kind = EK::BuiltinCall;
    Builtin builtin = Builtin::Print;
    ArenaKind arena = ArenaKind::None; // ListNew: where the list is allocated (see New)
    List<Expr*> args;
};

struct New : Expr {
    static constexpr EK node_kind = EK::New;
    // `C()`: None; `alloc C()` or a `manual class`: Alloc; `new(frame) C()`:
    // Frame; `new(scene) C()`: Scene (DESIGN.md 5.10).
    ArenaKind arena = ArenaKind::None;
    ClassInfo* cls = nullptr;
    Function* init = nullptr; // nearest `init` in the hierarchy, or null
    List<Expr*> args;
};

struct StructNew : Expr {
    static constexpr EK node_kind = EK::StructNew;
    StructInfo* st = nullptr;
    // One per field slot in declaration order; evaluated in `order`.
    List<Expr*> fields;
    List<std::uint32_t> order; // slot indices, in source evaluation order
};

struct EnumNew : Expr {
    static constexpr EK node_kind = EK::EnumNew;
    EnumInfo* en = nullptr;
    std::uint32_t variant = 0;
    List<Expr*> args;
};

struct Field : Expr {
    static constexpr EK node_kind = EK::Field;
    Expr* object = nullptr;
    FieldInfo* field = nullptr;
};

struct Index : Expr {
    static constexpr EK node_kind = EK::Index;
    Expr* object = nullptr;
    Expr* index = nullptr;
};

struct Lambda : Expr {
    static constexpr EK node_kind = EK::Lambda;
    Function* fn = nullptr; // fn->captures describes the captured variables
};

struct ToStr : Expr {
    static constexpr EK node_kind = EK::ToStr;
    Expr* operand = nullptr;
    Function* display = nullptr; // statically known `to_str`, if any
};

struct Concat : Expr {
    static constexpr EK node_kind = EK::Concat;
    List<Expr*> parts; // all str
};

struct Is : Expr {
    static constexpr EK node_kind = EK::Is;
    Expr* operand = nullptr;
    Pattern* pattern = nullptr;
};

struct Copy : Expr {
    static constexpr EK node_kind = EK::Copy;
    Expr* operand = nullptr;
};

struct ListLit : Expr {
    static constexpr EK node_kind = EK::ListLit;
    List<Expr*> elements;
};

struct SafeNav : Expr {
    static constexpr EK node_kind = EK::SafeNav;
    Expr* object = nullptr;
    LocalVar* tmp = nullptr;
    Expr* body = nullptr;
};

struct Default : Expr {
    static constexpr EK node_kind = EK::Default;
};

struct Hole : Expr {
    static constexpr EK node_kind = EK::Hole;
};

// `vec3(p, 1.0)`: floats and vectors whose sizes add up to the result's
// size, or a single float splatted to every component.
struct VecNew : Expr {
    static constexpr EK node_kind = EK::VecNew;
    List<Expr*> parts;
};

// Components 0..3 named by x, y, z, w. One component gives a float.
struct Swizzle : Expr {
    static constexpr EK node_kind = EK::Swizzle;
    Expr* operand = nullptr;
    std::uint8_t count = 0;
    std::uint8_t index[4] = {};
};

// `operand` with components `index[0..count)` replaced by `value` (a float
// when count is 1, else a vec of that size). Lowers `v.x = e`.
struct VecWith : Expr {
    static constexpr EK node_kind = EK::VecWith;
    Expr* operand = nullptr;
    Expr* value = nullptr;
    std::uint8_t count = 0;
    std::uint8_t index[4] = {};
};

enum class AwaitKind : std::uint8_t {
    Ticks,   // wait_ticks(n), yield
    Seconds, // wait(seconds), converted to ticks at the VM's tick rate
    Until,   // until(fn() -> bool)
    Task,    // a started coroutine finishing
    Signal,  // the next emit of a signal; value = its argument (one parameter)
    Call,    // a coroutine call run to completion inside this one; value = its result
};

struct Await : Expr {
    static constexpr EK node_kind = EK::Await;
    AwaitKind await_kind = AwaitKind::Ticks;
    Expr* operand = nullptr;
};

// `free(x)`: x is a class or list reference; panics unless it is a live
// manual object (null is ignored).
struct Free : Expr {
    static constexpr EK node_kind = EK::Free;
    Expr* operand = nullptr;
};

// `start(f(args))`: `call` is a Call or MethodCall of a coroutine. The new
// coroutine is owned by `owner` (the starting method's `self`, or null).
struct Start : Expr {
    static constexpr EK node_kind = EK::Start;
    Expr* call = nullptr;
    Expr* owner = nullptr;
};

// `x?` (DESIGN.md 5.8): `operand` is a Result<T, E> (is_result) or a
// nullable value. Ok / non-null gives the value; Err / null returns it from
// the enclosing function (running its defers), whose return type is a
// Result with a compatible error type, or nullable.
struct Try : Expr {
    static constexpr EK node_kind = EK::Try;
    Expr* operand = nullptr;
    bool is_result = true;
};

// `obj.hurt` used as a value: `thunk` is a synthesized lambda with one
// capture (the receiver, evaluated now) that calls the method.
struct BoundMethod : Expr {
    static constexpr EK node_kind = EK::BoundMethod;
    Function* thunk = nullptr;
    Expr* receiver = nullptr;
};

// ---------------------------------------------------------------- patterns

enum class PK : std::uint8_t { Wildcard, Bind, Literal, Range, Variant, Class, Null, NonNull, Or, Result };

struct Pattern {
    PK kind = PK::Wildcard;
    Span span;
    const Type* type = nullptr; // the type being matched
};

template <class T>
const T* as(const Pattern* p) {
    return p != nullptr && p->kind == T::node_kind ? static_cast<const T*>(p) : nullptr;
}

struct WildcardPat : Pattern {
    static constexpr PK node_kind = PK::Wildcard;
};

struct BindPat : Pattern {
    static constexpr PK node_kind = PK::Bind;
    LocalVar* var = nullptr;
};

struct LiteralPat : Pattern {
    static constexpr PK node_kind = PK::Literal;
    ConstValue value; // compared with `type` semantics
};

struct RangePat : Pattern {
    static constexpr PK node_kind = PK::Range;
    ConstValue lo;
    ConstValue hi;
    bool inclusive = false;
};

struct VariantPat : Pattern {
    static constexpr PK node_kind = PK::Variant;
    EnumInfo* en = nullptr;
    std::uint32_t variant = 0;
    bool has_args = false;
    List<Pattern*> args;
};

// `Boss` / `Boss b` (type test on a class reference, optional binding)
struct ClassPat : Pattern {
    static constexpr PK node_kind = PK::Class;
    ClassInfo* cls = nullptr;
    LocalVar* bind = nullptr;
};

struct NullPat : Pattern {
    static constexpr PK node_kind = PK::Null;
};

// `if x? t`: matches non-null, binds the unwrapped value.
struct NonNullPat : Pattern {
    static constexpr PK node_kind = PK::NonNull;
    LocalVar* bind = nullptr;
};

// `Ok(p)` / `Err(p)` on a Result; `inner` null = any payload.
struct ResultPat : Pattern {
    static constexpr PK node_kind = PK::Result;
    bool ok = true;
    Pattern* inner = nullptr;
};

struct OrPat : Pattern {
    static constexpr PK node_kind = PK::Or;
    List<Pattern*> alternatives;
};

// ---------------------------------------------------------------- statements

enum class SK : std::uint8_t {
    Expr, Var, Assign, Block, If, While, Loop, ForRange, ForList, Match, Return, Break, Continue, Defer,
};

struct Stmt {
    SK kind = SK::Expr;
    Span span;
};

template <class T>
T* as(Stmt* s) {
    return s != nullptr && s->kind == T::node_kind ? static_cast<T*>(s) : nullptr;
}
template <class T>
const T* as(const Stmt* s) {
    return s != nullptr && s->kind == T::node_kind ? static_cast<const T*>(s) : nullptr;
}

struct ExprStmt : Stmt {
    static constexpr SK node_kind = SK::Expr;
    Expr* expr = nullptr;
    bool echo = false; // REPL: print the value
};

struct VarStmt : Stmt {
    static constexpr SK node_kind = SK::Var;
    LocalVar* var = nullptr;
    Expr* init = nullptr; // never null (Default when omitted)
};

// `target = value`. Targets: Local, Capture, Global, Field, Index. For
// compound assignment `value` contains a Hole standing for the target's
// current value; the object/index of the target are evaluated once.
struct Assign : Stmt {
    static constexpr SK node_kind = SK::Assign;
    Expr* target = nullptr;
    Expr* value = nullptr;
    bool uses_hole = false;
};

struct Block : Stmt {
    static constexpr SK node_kind = SK::Block;
    List<Stmt*> stmts;
    // `unsafe { }`: references to manual objects are used without their
    // generation check inside (not inside nested lambdas).
    bool unchecked = false;
};

struct If : Stmt {
    static constexpr SK node_kind = SK::If;
    Expr* cond = nullptr; // may be an Is whose bindings are visible in then_body
    Block* then_body = nullptr;
    Stmt* else_body = nullptr; // Block, If, or null
};

struct While : Stmt {
    static constexpr SK node_kind = SK::While;
    Expr* cond = nullptr;
    Block* body = nullptr;
};

struct Loop : Stmt {
    static constexpr SK node_kind = SK::Loop;
    Block* body = nullptr;
};

// `for i in lo..hi step s` over integers. A negative step counts down.
struct ForRange : Stmt {
    static constexpr SK node_kind = SK::ForRange;
    LocalVar* var = nullptr;
    Expr* lo = nullptr;
    Expr* hi = nullptr;
    Expr* step = nullptr; // null = 1
    bool inclusive = false;
    Block* body = nullptr;
};

// `for e in list` / `for i, e in list`, also over [T; N], Deque and Set
// (`for i, e` on lists, arrays and deques only), and Map: `for k in map`,
// `for k, v in map` (`value` set).
struct ForList : Stmt {
    static constexpr SK node_kind = SK::ForList;
    enum class Over : std::uint8_t { List, Hash, Deque } over = Over::List;
    LocalVar* index = nullptr; // optional
    LocalVar* elem = nullptr;
    LocalVar* value = nullptr; // Map: the entry's value
    Expr* list = nullptr;
    Block* body = nullptr;
};

struct MatchArm {
    Pattern* pattern = nullptr;
    Expr* guard = nullptr;
    Stmt* body = nullptr;
    Span span;
};

struct Match : Stmt {
    static constexpr SK node_kind = SK::Match;
    Expr* subject = nullptr;
    List<MatchArm> arms;
};

struct Return : Stmt {
    static constexpr SK node_kind = SK::Return;
    Expr* value = nullptr;
};

struct Break : Stmt {
    static constexpr SK node_kind = SK::Break;
    Stmt* loop = nullptr; // While / Loop / ForRange / ForList
};

struct Continue : Stmt {
    static constexpr SK node_kind = SK::Continue;
    Stmt* loop = nullptr;
};

struct Defer : Stmt {
    static constexpr SK node_kind = SK::Defer;
    Stmt* body = nullptr;
};

} // namespace pluma::tir

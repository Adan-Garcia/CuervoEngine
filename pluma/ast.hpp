#pragma once

// AST for the whole grammar in docs/LANGUAGE.md (sections 2-5, EBNF in 7).
//
// Every node lives in an Arena (see arena.hpp) and is trivially destructible;
// child lists are List<T>. Names are string_views into the SourceFile (or into
// the arena for decoded string text). Every node carries a Span.
//
// Parentheses are not represented: the parser returns the inner expression
// (with the span widened to include the parens) and the printer re-inserts
// parens from the precedence table (LANGUAGE.md 4.1).

#include <cstdint>
#include <string_view>

#include "pluma/arena.hpp"
#include "pluma/source.hpp"

namespace pluma {

struct Expr;
struct Stmt;
struct Decl;
struct TypeExpr;
struct Pattern;
struct BlockStmt;

// Downcast helpers. Every concrete node declares `static constexpr ... node_kind`.
template <class T, class Base>
T* as(Base* node) {
    return node != nullptr && node->kind == T::node_kind ? static_cast<T*>(node) : nullptr;
}
template <class T, class Base>
const T* as(const Base* node) {
    return node != nullptr && node->kind == T::node_kind ? static_cast<const T*>(node) : nullptr;
}

struct Ident {
    std::string_view name;
    Span span;
    bool empty() const { return name.empty(); }
};

// ---------------------------------------------------------------- types

enum class TypeKind : std::uint8_t { Error, Named, Array, Fn, Nullable };

struct TypeExpr {
    TypeKind kind = TypeKind::Error;
    Span span;
};

struct ErrorType : TypeExpr {
    static constexpr TypeKind node_kind = TypeKind::Error;
};

// `int`, `List<T>`, `Map<str, int>`
struct NamedType : TypeExpr {
    static constexpr TypeKind node_kind = TypeKind::Named;
    Ident name;
    List<TypeExpr*> args;
};

// `[T; N]`
struct ArrayType : TypeExpr {
    static constexpr TypeKind node_kind = TypeKind::Array;
    TypeExpr* element = nullptr;
    Expr* size = nullptr;
};

// `fn(int, str) -> bool`
struct FnType : TypeExpr {
    static constexpr TypeKind node_kind = TypeKind::Fn;
    List<TypeExpr*> params;
    TypeExpr* ret = nullptr; // null = void
};

// `T?`
struct NullableType : TypeExpr {
    static constexpr TypeKind node_kind = TypeKind::Nullable;
    TypeExpr* inner = nullptr;
};

// ---------------------------------------------------------------- shared pieces

// `name: Type = default` (function, lambda, signal and enum-variant parameters)
struct Param {
    Ident name;
    TypeExpr* type = nullptr;  // may be null when a default fixes it, or in lambdas
    Expr* default_value = nullptr;
    Span span;
};

// `T: Add + Default`
struct GenericParam {
    Ident name;
    List<TypeExpr*> bounds;
    Span span;
};

// `name = value` or `value`
struct Arg {
    Ident name; // empty for positional arguments
    Expr* value = nullptr;
    Span span;
};

// `@export(range = 0..1000)`
struct Attribute {
    Ident name;
    bool has_args = false; // `@export()` vs `@export`
    List<Arg> args;
    Span span;
};

// ---------------------------------------------------------------- expressions

enum class ExprKind : std::uint8_t {
    Error,
    Int, Float, Bool, Null, String, Name,
    Identifier, Self, Super,
    ListLit, MapLit, StructLit,
    Unary, Binary, Range, Ternary, Is, As,
    Call, Index, Member, Try,
    Lambda, New, Alloc, Await, Start, Free,
};

enum class UnaryOp : std::uint8_t { Neg, Not, BitNot };

enum class BinaryOp : std::uint8_t {
    Mul, Div, Rem,
    Add, Sub,
    Shl, Shr,
    BitAnd, BitXor, BitOr,
    Lt, Le, Gt, Ge,
    Eq, Ne,
    And, Or,
    Coalesce, // ??
};

// Suffix of a numeric literal (`42u8`, `2.5f64`). None = default type.
// `Deg` (`45deg`) is a float literal in degrees, converted to radians (LANGUAGE.md 1.5).
enum class NumSuffix : std::uint8_t { None, I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, Deg };

struct Expr {
    ExprKind kind = ExprKind::Error;
    Span span;
};

struct ErrorExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Error;
};

struct IntLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::Int;
    std::uint64_t value = 0;
    NumSuffix suffix = NumSuffix::None;
    std::string_view text; // source spelling, kept by the printer (`0xff`, `1_000`)
};

struct FloatLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::Float;
    double value = 0.0;
    NumSuffix suffix = NumSuffix::None;
    std::string_view text;
};

struct BoolLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::Bool;
    bool value = false;
};

struct NullLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::Null;
};

// One piece of a string literal: decoded text, or an interpolated expression.
struct StringPart {
    std::string_view text; // decoded (escapes resolved, `{{` -> `{`), when expr is null
    Expr* expr = nullptr;
    Span span;
};

struct StringLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::String;
    List<StringPart> parts; // adjacent text is merged; empty string = no parts
    bool raw = false;       // r"..."
    bool multiline = false; // """..."""
    std::string_view source_text;
};

// `#shoot`
struct NameLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::Name;
    std::string_view name; // without '#'
};

// `x`, or `List<T>` / `max<int>` when followed by a call or struct literal.
struct IdentExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Identifier;
    std::string_view name;
    List<TypeExpr*> type_args;
};

struct SelfExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Self;
};

struct SuperExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Super;
};

struct ListLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::ListLit;
    List<Expr*> elements;
};

struct MapEntry {
    Expr* key = nullptr;
    Expr* value = nullptr;
};

struct MapLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::MapLit;
    List<MapEntry> entries;
};

struct FieldInit {
    Ident name;
    Expr* value = nullptr;
    Span span;
};

// `Bullet { pos = p, vel = v }`
struct StructLit : Expr {
    static constexpr ExprKind node_kind = ExprKind::StructLit;
    NamedType* type = nullptr;
    List<FieldInit> fields;
};

struct UnaryExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Unary;
    UnaryOp op = UnaryOp::Neg;
    Expr* operand = nullptr;
};

struct BinaryExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Binary;
    BinaryOp op = BinaryOp::Add;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
};

// `lo..hi`, `lo..=hi`, `lo..hi step s`
struct RangeExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Range;
    Expr* lo = nullptr;
    Expr* hi = nullptr;
    Expr* step = nullptr; // optional
    bool inclusive = false;
};

struct TernaryExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Ternary;
    Expr* cond = nullptr;
    Expr* then_expr = nullptr;
    Expr* else_expr = nullptr;
};

// `x is Boss`, `x is Boss b`, `phase is Phase.Spiral(arms)`
struct IsExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Is;
    Expr* operand = nullptr;
    Pattern* pattern = nullptr;
    Ident binding; // optional
};

struct AsExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::As;
    Expr* operand = nullptr;
    TypeExpr* type = nullptr;
};

struct CallExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Call;
    Expr* callee = nullptr;
    List<Arg> args;
};

struct IndexExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Index;
    Expr* object = nullptr;
    Expr* index = nullptr;
};

// `a.b`, `a?.b`, `a.f<T>`
struct MemberExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Member;
    Expr* object = nullptr;
    Ident member;
    bool safe = false; // ?.
    List<TypeExpr*> type_args;
};

// Postfix `?`: propagate a Result error.
struct TryExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Try;
    Expr* operand = nullptr;
};

// `fn(x: int) -> int { ... }` (block body) or `x => expr` / `(a, b) => expr`.
struct LambdaExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Lambda;
    List<Param> params;
    TypeExpr* ret = nullptr;
    BlockStmt* body = nullptr;  // `fn` form
    Expr* expr_body = nullptr;  // `=>` form
    bool arrow = false;         // `=>` form
    bool suspends = false;      // its body uses `await` or `yield` (not counting nested lambdas)
};

// `new(frame) List<vec2>()`
struct NewExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::New;
    Ident arena;
    TypeExpr* type = nullptr;
    List<Arg> args;
};

// `alloc Buffer(4096)`
struct AllocExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Alloc;
    TypeExpr* type = nullptr;
    List<Arg> args;
};

struct AwaitExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Await;
    Expr* operand = nullptr;
};

// `start(coroutine())`
struct StartExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Start;
    Expr* operand = nullptr;
};

// `free(buf)`
struct FreeExpr : Expr {
    static constexpr ExprKind node_kind = ExprKind::Free;
    Expr* operand = nullptr;
};

// ---------------------------------------------------------------- patterns

enum class PatternKind : std::uint8_t { Error, Wildcard, Literal, Range, Path, Or };

struct Pattern {
    PatternKind kind = PatternKind::Error;
    Span span;
};

struct ErrorPattern : Pattern {
    static constexpr PatternKind node_kind = PatternKind::Error;
};

struct WildcardPattern : Pattern {
    static constexpr PatternKind node_kind = PatternKind::Wildcard;
};

// `0`, `-1`, `"a"`, `#name`, `true`, `null`
struct LiteralPattern : Pattern {
    static constexpr PatternKind node_kind = PatternKind::Literal;
    Expr* value = nullptr;
};

// `2..=9`
struct RangePattern : Pattern {
    static constexpr PatternKind node_kind = PatternKind::Range;
    Expr* lo = nullptr;
    Expr* hi = nullptr;
    bool inclusive = false;
};

// `x` (binding, or a type/constant name; resolved later), `Phase.Rage`,
// `Phase.Spiral(arms)`.
struct PathPattern : Pattern {
    static constexpr PatternKind node_kind = PatternKind::Path;
    List<Ident> path;
    bool has_args = false;
    List<Pattern*> args;
};

struct OrPattern : Pattern {
    static constexpr PatternKind node_kind = PatternKind::Or;
    List<Pattern*> alternatives;
};

// ---------------------------------------------------------------- statements

enum class StmtKind : std::uint8_t {
    Error, Expr, Decl, Assign, Block,
    If, Match, For, Foreach, While, Loop,
    Return, Break, Continue, Defer, Unsafe, Yield, Emit,
};

enum class AssignOp : std::uint8_t { Set, Add, Sub, Mul, Div, Rem, BitAnd, BitOr, BitXor, Shl, Shr };

struct Stmt {
    StmtKind kind = StmtKind::Error;
    Span span;
};

struct ErrorStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Error;
};

struct ExprStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Expr;
    Expr* expr = nullptr;
};

// A local `var`/`const`, or (in script mode) any declaration.
struct DeclStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Decl;
    Decl* decl = nullptr;
};

struct AssignStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Assign;
    AssignOp op = AssignOp::Set;
    Expr* target = nullptr;
    Expr* value = nullptr;
};

struct BlockStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Block;
    List<Stmt*> stmts;
};

// One `if cond block` link of an if / else-if chain.
struct IfBranch {
    Expr* cond = nullptr;
    Ident unwrap; // `if target? t` binds t; empty otherwise
    BlockStmt* body = nullptr;
    Span span;
};

struct IfStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::If;
    List<IfBranch> branches;          // at least one
    BlockStmt* else_body = nullptr;   // optional
};

struct MatchArm {
    Pattern* pattern = nullptr;
    Expr* guard = nullptr; // optional
    Stmt* body = nullptr;  // BlockStmt or a single statement
    Span span;
};

struct MatchStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Match;
    Expr* subject = nullptr;
    List<MatchArm> arms;
};

struct ForStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::For;
    Ident label;
    Ident first;  // `for i in` / `for i, e in`
    Ident second; // optional
    Expr* iterable = nullptr;
    BlockStmt* body = nullptr;
};

struct ForeachStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Foreach;
    List<Attribute> attributes; // e.g. @allow_slow
    Ident var;
    Expr* pool = nullptr;
    BlockStmt* body = nullptr;
};

struct WhileStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::While;
    Ident label;
    Expr* cond = nullptr;
    BlockStmt* body = nullptr;
};

struct LoopStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Loop;
    Ident label;
    BlockStmt* body = nullptr;
};

struct ReturnStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Return;
    Expr* value = nullptr;
};

struct BreakStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Break;
    Ident label;
};

struct ContinueStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Continue;
    Ident label;
};

struct DeferStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Defer;
    Stmt* body = nullptr; // BlockStmt or a single statement
};

struct UnsafeStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Unsafe;
    BlockStmt* body = nullptr;
};

struct YieldStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Yield;
};

struct EmitStmt : Stmt {
    static constexpr StmtKind node_kind = StmtKind::Emit;
    Ident signal;
    bool has_args = false;
    List<Arg> args;
};

// ---------------------------------------------------------------- declarations

enum class DeclKind : std::uint8_t { Error, Var, Const, Fn, Class, Struct, Enum, Trait, Signal };

struct Decl {
    DeclKind kind = DeclKind::Error;
    Span span;
    Ident name;
    List<Attribute> attributes;
    bool is_pub = false;
    bool is_static = false;
    bool is_final = false;
};

struct ErrorDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Error;
};

// `[weak] var name [: T] [= init] [{ get {...} set(v) {...} }]`
struct VarDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Var;
    bool weak = false;
    TypeExpr* type = nullptr;
    Expr* init = nullptr;
    bool has_accessors = false;
    BlockStmt* getter = nullptr;
    Ident setter_param;
    BlockStmt* setter = nullptr;
};

struct ConstDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Const;
    TypeExpr* type = nullptr;
    Expr* value = nullptr;
};

struct FnDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Fn;
    List<GenericParam> generics;
    List<Param> params;
    TypeExpr* ret = nullptr;     // null = void
    BlockStmt* body = nullptr;   // null for a trait method signature
    // The body uses `await` or `yield` (outside nested lambdas): the function
    // is a coroutine (DESIGN.md 5.9). Set by the parser.
    bool suspends = false;
};

struct ClassDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Class;
    bool manual = false;
    List<GenericParam> generics;
    TypeExpr* extends = nullptr;
    List<TypeExpr*> traits;
    List<Decl*> members;
};

struct StructDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Struct;
    List<GenericParam> generics;
    List<TypeExpr*> traits;
    List<Decl*> members;
};

struct EnumVariant {
    Ident name;
    bool has_params = false;
    List<Param> params;
    Expr* value = nullptr; // explicit discriminant
    Span span;
};

struct EnumDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Enum;
    TypeExpr* backing = nullptr;
    List<EnumVariant> variants;
};

struct TraitDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Trait;
    List<GenericParam> generics;
    List<TypeExpr*> supertraits;
    List<Decl*> members; // FnDecl only
};

struct SignalDecl : Decl {
    static constexpr DeclKind node_kind = DeclKind::Signal;
    bool has_params = false;
    List<Param> params;
};

// ---------------------------------------------------------------- roots

struct Module {
    List<Decl*> decls;
};

// Script mode (REPL): declarations and statements mixed at top level.
// Declarations appear as DeclStmt.
struct Script {
    List<Stmt*> stmts;
};

// Precedence levels, high number = binds tighter. Mirrors LANGUAGE.md 4.1
// (whose level numbers run the other way).
enum class Prec : std::uint8_t {
    Lowest = 0,
    Lambda,     // =>
    Ternary,    // ? :
    Coalesce,   // ??
    Or,         // ||
    And,        // &&
    Equality,   // == !=          (non-associative)
    Comparison, // < <= > >= is   (non-associative)
    Range,      // .. ..= step    (non-associative)
    BitOr,      // |
    BitXor,     // ^
    BitAnd,     // &
    Shift,      // << >>
    Additive,   // + -
    Multiplicative, // * / %
    As,         // as
    Unary,      // - ! ~ await
    Postfix,    // () [] . ?. ?
    Primary,
};

std::string_view to_string(UnaryOp op);
std::string_view to_string(BinaryOp op);
std::string_view to_string(AssignOp op);
std::string_view to_string(NumSuffix suffix);
Prec precedence(BinaryOp op);
// Precedence of the expression as a whole, for parenthesization.
Prec precedence(const Expr* expr);

} // namespace pluma

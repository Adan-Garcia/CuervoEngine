#include "pluma/checker.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pluma/numeric.hpp"

namespace pluma {

namespace ir = tir;

namespace {

constexpr double deg_to_rad = 3.14159265358979323846 / 180.0;

// Rounds a constant to the precision of its kind, so folded constants match
// what the VM computes.
double round_to(NumKind k, double v) { return k == NumKind::F32 ? static_cast<double>(static_cast<float>(v)) : v; }

std::optional<NumKind> builtin_num(std::string_view n) {
    if (n == "int" || n == "i32") return NumKind::I32;
    if (n == "float" || n == "f32") return NumKind::F32;
    if (n == "i8") return NumKind::I8;
    if (n == "i16") return NumKind::I16;
    if (n == "i64") return NumKind::I64;
    if (n == "u8") return NumKind::U8;
    if (n == "u16") return NumKind::U16;
    if (n == "u32") return NumKind::U32;
    if (n == "u64") return NumKind::U64;
    if (n == "f64") return NumKind::F64;
    return std::nullopt;
}

// `vec1` ... `vec64` (DESIGN.md 4.2). `ivecN` is the math prelude's structs (LANGUAGE.md 9.21).
constexpr std::uint32_t max_vec_dim = 64;
std::optional<std::uint32_t> vec_dim(std::string_view n, std::string_view prefix = "vec") {
    if (!n.starts_with(prefix) || n.size() == prefix.size() || n.size() > prefix.size() + 2) return std::nullopt;
    std::uint32_t d = 0;
    for (std::size_t i = prefix.size(); i < n.size(); ++i) {
        if (n[i] < '0' || n[i] > '9') return std::nullopt;
        d = d * 10 + static_cast<std::uint32_t>(n[i] - '0');
    }
    if (n[prefix.size()] == '0' || d < 1 || d > max_vec_dim) return std::nullopt;
    return d;
}

bool is_builtin_type_name(std::string_view n) {
    return builtin_num(n).has_value() || n == "bool" || n == "str" || n == "name" || n == "List" || n == "void" ||
           n == "Task" || n == "Map" || n == "Set" || n == "Deque" || n == "PriorityQueue" || n == "StrBuilder" ||
           n == "Result" || vec_dim(n).has_value();
}

bool has_attribute(const Decl* d, std::string_view name) {
    for (const Attribute& a : d->attributes) {
        if (a.name.name == name) return true;
    }
    return false;
}

const Attribute* find_attribute(const Decl* d, std::string_view name) {
    for (const Attribute& a : d->attributes) {
        if (a.name.name == name) return &a;
    }
    return nullptr;
}

// A number literal, optionally negated (attribute arguments are literals).
std::optional<double> literal_number(const Expr* e) {
    double sign = 1.0;
    if (e != nullptr && e->kind == ExprKind::Unary && as<UnaryExpr>(e)->op == UnaryOp::Neg) {
        sign = -1.0;
        e = as<UnaryExpr>(e)->operand;
    }
    if (e == nullptr) return std::nullopt;
    if (e->kind == ExprKind::Int) return sign * static_cast<double>(as<IntLit>(e)->value);
    if (e->kind == ExprKind::Float) return sign * as<FloatLit>(e)->value;
    return std::nullopt;
}

std::optional<Builtin> builtin_fn(std::string_view n) {
    if (n == "print") return Builtin::Print;
    if (n == "assert") return Builtin::Assert;
    if (n == "panic") return Builtin::Panic;
    if (n == "min") return Builtin::Min;
    if (n == "max") return Builtin::Max;
    if (n == "abs") return Builtin::Abs;
    if (n == "clamp") return Builtin::Clamp;
    if (n == "sqrt") return Builtin::Sqrt;
    if (n == "floor") return Builtin::Floor;
    if (n == "ceil") return Builtin::Ceil;
    if (n == "round") return Builtin::Round;
    if (n == "sin") return Builtin::Sin;
    if (n == "cos") return Builtin::Cos;
    if (n == "tan") return Builtin::Tan;
    if (n == "asin") return Builtin::Asin;
    if (n == "acos") return Builtin::Acos;
    if (n == "atan") return Builtin::Atan;
    if (n == "atan2") return Builtin::Atan2;
    if (n == "exp") return Builtin::Exp;
    if (n == "log") return Builtin::Log;
    if (n == "log2") return Builtin::Log2;
    if (n == "log10") return Builtin::Log10;
    if (n == "pow") return Builtin::Pow;
    if (n == "sign") return Builtin::Sign;
    if (n == "lerp") return Builtin::Lerp;
    if (n == "inverse_lerp") return Builtin::InverseLerp;
    if (n == "remap") return Builtin::Remap;
    if (n == "smoothstep") return Builtin::Smoothstep;
    if (n == "step") return Builtin::Step;
    if (n == "move_toward") return Builtin::MoveToward;
    if (n == "rad_to_deg") return Builtin::RadToDeg;
    if (n == "deg_to_rad") return Builtin::DegToRad;
    if (n == "wrap") return Builtin::Wrap;
    if (n == "wrap_angle") return Builtin::WrapAngle;
    if (n == "from_angle") return Builtin::FromAngle;
    if (n == "parse_int") return Builtin::ParseInt;
    if (n == "parse_float") return Builtin::ParseFloat;
    if (n == "wrapping_add") return Builtin::WrappingAdd;
    if (n == "wrapping_sub") return Builtin::WrappingSub;
    if (n == "wrapping_mul") return Builtin::WrappingMul;
    if (n == "checked_add") return Builtin::CheckedAdd;
    if (n == "checked_sub") return Builtin::CheckedSub;
    if (n == "checked_mul") return Builtin::CheckedMul;
    if (n == "saturating_add") return Builtin::SaturatingAdd;
    if (n == "saturating_sub") return Builtin::SaturatingSub;
    if (n == "saturating_mul") return Builtin::SaturatingMul;
    return std::nullopt;
}

// Awaitables that only make sense after `await` (DESIGN.md 5.9).
bool is_await_only(std::string_view n) { return n == "wait" || n == "wait_ticks" || n == "until"; }

NumKind suffix_kind(NumSuffix s) {
    switch (s) {
        case NumSuffix::I8: return NumKind::I8;
        case NumSuffix::I16: return NumKind::I16;
        case NumSuffix::I32: return NumKind::I32;
        case NumSuffix::I64: return NumKind::I64;
        case NumSuffix::U8: return NumKind::U8;
        case NumSuffix::U16: return NumKind::U16;
        case NumSuffix::U32: return NumKind::U32;
        case NumSuffix::U64: return NumKind::U64;
        case NumSuffix::F32: return NumKind::F32;
        case NumSuffix::F64: return NumKind::F64;
        default: return NumKind::I32;
    }
}

BinaryOp assign_to_binary(AssignOp op) {
    switch (op) {
        case AssignOp::Add: return BinaryOp::Add;
        case AssignOp::Sub: return BinaryOp::Sub;
        case AssignOp::Mul: return BinaryOp::Mul;
        case AssignOp::Div: return BinaryOp::Div;
        case AssignOp::Rem: return BinaryOp::Rem;
        case AssignOp::BitAnd: return BinaryOp::BitAnd;
        case AssignOp::BitOr: return BinaryOp::BitOr;
        case AssignOp::BitXor: return BinaryOp::BitXor;
        case AssignOp::Shl: return BinaryOp::Shl;
        case AssignOp::Shr: return BinaryOp::Shr;
        case AssignOp::Set: break;
    }
    return BinaryOp::Add;
}

BuiltinTrait op_trait(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return BuiltinTrait::Add;
        case BinaryOp::Sub: return BuiltinTrait::Sub;
        case BinaryOp::Mul: return BuiltinTrait::Mul;
        case BinaryOp::Div: return BuiltinTrait::Div;
        case BinaryOp::Rem: return BuiltinTrait::Rem;
        case BinaryOp::Eq:
        case BinaryOp::Ne: return BuiltinTrait::Eq;
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge: return BuiltinTrait::Ord;
        default: return BuiltinTrait::None;
    }
}

bool is_arith(BinaryOp op) {
    return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul || op == BinaryOp::Div ||
           op == BinaryOp::Rem;
}
bool is_bitwise(BinaryOp op) {
    return op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor || op == BinaryOp::Shl ||
           op == BinaryOp::Shr;
}
bool is_ordering(BinaryOp op) {
    return op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt || op == BinaryOp::Ge;
}

// `1`, `2.5`, `-3` without a suffix: their type adapts to the other operand.
bool is_untyped_literal(const Expr* e) {
    if (auto i = as<IntLit>(e)) return i->suffix == NumSuffix::None;
    if (auto f = as<FloatLit>(e)) return f->suffix == NumSuffix::None;
    if (auto u = as<UnaryExpr>(e)) return u->op == UnaryOp::Neg && is_untyped_literal(u->operand);
    return false;
}

bool is_float_literal(const Expr* e) {
    if (auto u = as<UnaryExpr>(e)) return is_float_literal(u->operand);
    return e->kind == ExprKind::Float || (e->kind == ExprKind::Int && as<IntLit>(e)->suffix == NumSuffix::Deg);
}

template <class T>
void bury(std::vector<std::shared_ptr<void>>& graveyard, std::vector<std::unique_ptr<T>>& v, std::size_t from) {
    for (std::size_t i = from; i < v.size(); ++i) {
        T* p = v[i].release();
        graveyard.emplace_back(p, [](void* q) { delete static_cast<T*>(q); });
    }
    v.resize(from);
}

class Checker {
public:
    Checker(Program& program, const SourceFile& source, DiagnosticSink& diags, CheckOptions options)
        : P(program), T(program.types), A(program.arena), src_(source), diags_(diags), opts_(options) {
        start_classes_ = P.classes.size();
        start_structs_ = P.structs.size();
        start_enums_ = P.enums.size();
        start_traits_ = P.traits.size();
        start_functions_ = P.functions.size();
        start_globals_ = P.global_vars.size();
    }

    CheckedUnit run_script(const Script& script) {
        std::vector<Decl*> decls;
        for (Stmt* s : script.stmts) {
            if (auto d = as<DeclStmt>(s)) decls.push_back(d->decl);
        }
        begin_unit(decls);
        // Top-level statements, with global initializers in source order.
        std::vector<ir::Stmt*> out;
        {
            CtxSwap swap(*this, &chunk_ctx_);
            push_scope();
            for (Stmt* s : script.stmts) {
                if (auto d = as<DeclStmt>(s)) {
                    global_init_stmt(d->decl, out);
                    continue;
                }
                ir::Stmt* st = stmt(s, true);
                if (st != nullptr) out.push_back(st);
            }
            pop_scope();
        }
        return finish_unit(out);
    }

    CheckedUnit run_module(const Module& module) {
        std::vector<Decl*> decls(module.decls.begin(), module.decls.end());
        begin_unit(decls);
        std::vector<ir::Stmt*> out;
        for (Decl* d : decls) global_init_stmt(d, out);
        return finish_unit(out);
    }

private:
    // ------------------------------------------------------------ contexts

    struct Entry {
        std::string_view name;
        LocalVar* var = nullptr;
        ir::Const* constant = nullptr; // local `const`
    };
    struct LoopInfo {
        ir::Stmt* node = nullptr;
        std::string_view label;
    };
    struct Ctx {
        Function* fn = nullptr;
        Ctx* outer = nullptr; // enclosing function (lambdas)
        std::vector<std::vector<Entry>> scopes;
        RecordInfo* record = nullptr; // for implicit member lookup
        TraitInfo* trait = nullptr;   // in a trait's default method: implicit calls on self
        bool is_static = true;        // no `self`
        const Type* ret = nullptr;    // null while inferring a lambda's return type
        bool infer_ret = false;
        std::vector<LoopInfo> loops;
        int defer_depth = 0;
        std::vector<GenericParamInfo*> generics;
    };

    struct CtxSwap {
        Checker& c;
        Ctx* saved;
        const std::vector<GenericParamInfo*>* saved_tparams;
        CtxSwap(Checker& checker, Ctx* next) : c(checker), saved(checker.cx_), saved_tparams(checker.tparams_) {
            c.cx_ = next;
            c.tparams_ = &next->generics;
        }
        ~CtxSwap() {
            c.cx_ = saved;
            c.tparams_ = saved_tparams;
        }
        CtxSwap(const CtxSwap&) = delete;
        CtxSwap& operator=(const CtxSwap&) = delete;
    };

    void push_scope() { cx_->scopes.emplace_back(); }
    void pop_scope() { cx_->scopes.pop_back(); }

    LocalVar* new_local(std::string_view name, const Type* type, Span span, bool is_mutable, bool is_param = false) {
        auto v = std::make_unique<LocalVar>();
        v->name = name;
        v->type = type;
        v->span = span;
        v->is_mutable = is_mutable;
        v->is_param = is_param;
        v->id = static_cast<std::uint32_t>(P.locals.size());
        LocalVar* out = v.get();
        P.locals.push_back(std::move(v));
        return out;
    }

    void declare_local(LocalVar* v) {
        if (v->name.empty() || v->name == "_") return;
        cx_->scopes.back().push_back(Entry{v->name, v, nullptr});
    }

    // ------------------------------------------------------------ nodes

    template <class N>
    N* mk(Span span, const Type* type) {
        N* n = A.make<N>();
        n->kind = N::node_kind;
        n->span = span;
        n->type = type;
        return n;
    }
    template <class N>
    N* mks(Span span) {
        N* n = A.make<N>();
        n->kind = N::node_kind;
        n->span = span;
        return n;
    }
    template <class N>
    N* mkp(Span span, const Type* type) {
        N* n = A.make<N>();
        n->kind = N::node_kind;
        n->span = span;
        n->type = type;
        return n;
    }
    template <class X>
    List<X> list(const std::vector<X>& v) {
        return A.list(v);
    }

    ir::Expr* error_node(Span span) { return mk<ir::Const>(span, T.error()); }

    ir::Const* const_node(Span span, const Type* type, ConstValue v) {
        auto* c = mk<ir::Const>(span, type);
        c->value = v;
        return c;
    }
    ir::Const* int_const(Span span, const Type* type, std::int64_t v) {
        ConstValue c;
        c.kind = ConstValue::Kind::Int;
        c.i = v;
        return const_node(span, type, c);
    }
    ir::Const* str_const(Span span, std::string_view s) {
        ConstValue c;
        c.kind = ConstValue::Kind::Str;
        c.s = A.str(s);
        return const_node(span, T.str(), c);
    }

    // ------------------------------------------------------------ diagnostics

    void error(Span span, std::string message) { diags_.error(span, std::move(message)); }
    void error_fix(Span span, std::string message, Span fix_span, std::string replacement) {
        Diagnostic& d = diags_.error(span, std::move(message));
        d.fix = FixIt{fix_span, std::move(replacement)};
    }
    static std::string q(const Type* t) { return "'" + type_name(t) + "'"; }

    // ------------------------------------------------------------ unit setup

    bool register_name(Ident name, Program::SymKind kind, void* ptr) {
        if (name.empty()) return false;
        // Types and values never share a position, so only type
        // declarations clash with the built-in type names.
        bool is_type = kind == Program::SymKind::Class || kind == Program::SymKind::Struct ||
                       kind == Program::SymKind::Enum || kind == Program::SymKind::Trait;
        if (is_type && is_builtin_type_name(name.name)) {
            error(name.span, std::format("'{}' is a built-in type name", name.name));
            return false;
        }
        if (P.globals.find(name.name) != P.globals.end()) {
            error(name.span, std::format("duplicate name '{}': names are global (DESIGN.md 5.11)", name.name));
            return false;
        }
        P.globals.emplace(std::string(name.name), Program::Symbol{kind, ptr});
        added_names_.emplace_back(name.name);
        return true;
    }

    void declare(Decl* d) {
        switch (d->kind) {
            case DeclKind::Class: {
                auto* cd = as<ClassDecl>(d);
                auto c = std::make_unique<ClassInfo>();
                c->is_class = true;
                c->name = cd->name.name;
                c->decl = cd;
                c->source = &src_;
                c->span = cd->name.span;
                c->manual = cd->manual;
                c->promised_acyclic = has_attribute(cd, "acyclic");
                c->rt_index = static_cast<std::uint32_t>(P.classes.size());
                register_name(cd->name, Program::SymKind::Class, c.get());
                new_records_.push_back(c.get());
                P.classes.push_back(std::move(c));
                break;
            }
            case DeclKind::Struct: {
                auto* sd = as<StructDecl>(d);
                auto s = std::make_unique<StructInfo>();
                s->name = sd->name.name;
                s->decl = sd;
                s->source = &src_;
                s->span = sd->name.span;
                s->rt_index = static_cast<std::uint32_t>(P.structs.size());
                register_name(sd->name, Program::SymKind::Struct, s.get());
                new_records_.push_back(s.get());
                P.structs.push_back(std::move(s));
                break;
            }
            case DeclKind::Enum: {
                auto* ed = as<EnumDecl>(d);
                auto e = std::make_unique<EnumInfo>();
                e->name = ed->name.name;
                e->decl = ed;
                e->source = &src_;
                e->span = ed->name.span;
                e->rt_index = static_cast<std::uint32_t>(P.enums.size());
                e->type = T.enum_(e.get());
                register_name(ed->name, Program::SymKind::Enum, e.get());
                new_enums_.push_back(e.get());
                P.enums.push_back(std::move(e));
                break;
            }
            case DeclKind::Trait: {
                auto* td = as<TraitDecl>(d);
                auto t = std::make_unique<TraitInfo>();
                t->name = td->name.name;
                t->decl = td;
                t->source = &src_;
                t->span = td->name.span;
                register_name(td->name, Program::SymKind::Trait, t.get());
                new_traits_.push_back(t.get());
                P.traits.push_back(std::move(t));
                break;
            }
            case DeclKind::Fn: {
                auto* fd = as<FnDecl>(d);
                Function* f = new_function(std::string(fd->name.name), FnKind::Free, fd->span);
                f->decl = fd;
                register_name(fd->name, Program::SymKind::Function, f);
                new_free_fns_.push_back(f);
                if (fd->name.name == "main") main_ = f;
                break;
            }
            case DeclKind::Var:
            case DeclKind::Const: {
                GlobalVar* g = new_global(d, std::string(d->name.name));
                register_name(d->name, Program::SymKind::Global, g);
                break;
            }
            case DeclKind::Signal: error(d->span, "signals are not supported yet (Phase 3)"); break;
            case DeclKind::Error: break;
        }
    }

    Function* new_function(std::string name, FnKind kind, Span span) {
        auto f = std::make_unique<Function>();
        f->name = std::move(name);
        f->kind = kind;
        f->span = span;
        f->source = &src_;
        f->index = static_cast<std::uint32_t>(P.functions.size());
        Function* out = f.get();
        P.functions.push_back(std::move(f));
        return out;
    }

    GlobalVar* new_global(Decl* d, std::string qualified) {
        auto g = std::make_unique<GlobalVar>();
        g->name = d->name.name;
        g->qualified = std::move(qualified);
        g->decl = d;
        g->span = d->name.span;
        g->source = &src_;
        g->is_const = d->kind == DeclKind::Const;
        g->is_mutable = !g->is_const;
        g->index = static_cast<std::uint32_t>(P.global_vars.size());
        GlobalVar* out = g.get();
        P.global_vars.push_back(std::move(g));
        new_globals_.push_back(out);
        return out;
    }

    // A struct is a value: one that holds itself, directly or through other
    // structs and fixed arrays, would need infinite space (references, lists
    // and optionals break the chain). Without this check `S {}` recursed
    // until the stack overflowed at run time.
    bool holds_by_value(const Type* t, const StructInfo* target, std::vector<const StructInfo*>& seen) {
        if (t->tag == TypeTag::Array) return holds_by_value(t->args[0], target, seen);
        if (t->tag != TypeTag::Struct) return false;
        const StructInfo* st = t->st();
        if (st == target) return true;
        if (std::find(seen.begin(), seen.end(), st) != seen.end()) return false;
        seen.push_back(st);
        Subst subst;
        for (std::size_t i = 0; i < st->generics.size() && i < t->args.size(); ++i) subst[st->generics[i]] = t->args[i];
        for (const FieldInfo* f : st->fields) {
            if (f->type != nullptr && holds_by_value(substitute(T, f->type, subst), target, seen)) return true;
        }
        return false;
    }

    void check_struct_size(RecordInfo* r) {
        if (r->is_class) return;
        auto* s = static_cast<StructInfo*>(r);
        for (const FieldInfo* f : s->fields) {
            if (f->type == nullptr) continue;
            std::vector<const StructInfo*> seen;
            if (holds_by_value(f->type, s, seen)) {
                error(f->span, std::format("struct '{}' contains itself through field '{}' and would have infinite size; "
                                           "use an optional ('{}?'), a class or a List",
                                           s->name, f->name, s->name));
                return;
            }
        }
    }

    void begin_unit(const std::vector<Decl*>& decls) {
        chunk_ = new_function("<top level>", FnKind::Chunk, Span{});
        chunk_->ret = T.void_();
        chunk_->signature_done = true;
        chunk_ctx_.fn = chunk_;
        chunk_ctx_.ret = T.void_();
        for (Decl* d : decls) declare(d);
        for (TraitInfo* t : new_traits_) ensure_trait(t);
        for (EnumInfo* e : new_enums_) ensure_enum(e);
        for (RecordInfo* r : new_records_) ensure_header(r);
        for (RecordInfo* r : new_records_) ensure_members(r);
        for (RecordInfo* r : new_records_) check_struct_size(r);
        for (Function* f : new_free_fns_) ensure_signature(f);
        for (GlobalVar* g : new_globals_) ensure_global(g);
        for (RecordInfo* r : new_records_) build_record_inits(r);
        for (RecordInfo* r : new_records_) check_exports(r);
        for (RecordInfo* r : new_records_) check_memory_decls(r);
        for (Decl* d : decls) {
            bool type = d->kind == DeclKind::Class || d->kind == DeclKind::Struct || d->kind == DeclKind::Enum ||
                        d->kind == DeclKind::Trait;
            reject_export(d, type ? "type declarations" : "top-level declarations");
            if (d->kind != DeclKind::Class) {
                if (const Attribute* a = find_attribute(d, "acyclic")) {
                    error(a->span, "'@acyclic' applies only to classes");
                }
            }
        }
        if (main_ != nullptr && (!main_->params.empty() || !main_->ret->is_void() || !main_->generics.empty())) {
            error(main_->decl->name.span, "'main' must take no parameters and return nothing");
        }
        // Bodies. Lambdas and synthesized functions are appended to
        // P.functions as we go; only declared functions are walked here.
        std::vector<Function*> bodies;
        for (std::size_t i = start_functions_; i < P.functions.size(); ++i) {
            Function* f = P.functions[i].get();
            if (f->decl != nullptr && f->decl->body != nullptr) bodies.push_back(f);
        }
        for (Function* f : bodies) check_body(f);
        for (RecordInfo* r : new_records_) check_accessors(r);
    }

    CheckedUnit finish_unit(std::vector<ir::Stmt*>& out) {
        auto* body = mks<ir::Block>(Span{});
        body->stmts = list(out);
        chunk_->body = body;
        chunk_->body_done = true;
        CheckedUnit result;
        diags_.sort_by_position();
        if (diags_.has_errors() && !opts_.keep_on_error) {
            rollback();
            return result;
        }
        result.ok = !diags_.has_errors();
        result.chunk = chunk_;
        result.main = main_;
        return result;
    }

    void rollback() {
        for (const std::string& n : added_names_) P.globals.erase(n);
        bury(graveyard_, P.classes, start_classes_);
        bury(graveyard_, P.structs, start_structs_);
        bury(graveyard_, P.enums, start_enums_);
        bury(graveyard_, P.traits, start_traits_);
        bury(graveyard_, P.functions, start_functions_);
        bury(graveyard_, P.global_vars, start_globals_);
        P.graveyard.insert(P.graveyard.end(), graveyard_.begin(), graveyard_.end());
    }

    // Emits `global = init` into the chunk for a top-level var.
    void global_init_stmt(Decl* d, std::vector<ir::Stmt*>& out) {
        auto emit = [&](GlobalVar* g) {
            if (g->is_const || g->init == nullptr || g->type->is_error()) return;
            auto* a = mks<ir::Assign>(g->decl->span);
            auto* target = mk<ir::Global>(g->span, g->type);
            target->var = g;
            a->target = target;
            a->value = g->init;
            out.push_back(a);
        };
        if (d->kind == DeclKind::Var) {
            auto it = P.globals.find(d->name.name);
            if (it != P.globals.end() && it->second.kind == Program::SymKind::Global) {
                auto* g = static_cast<GlobalVar*>(it->second.ptr);
                if (g->decl == d) emit(g);
            }
        } else if (d->kind == DeclKind::Class || d->kind == DeclKind::Struct) {
            auto it = P.globals.find(d->name.name);
            if (it == P.globals.end()) return;
            if (it->second.kind != Program::SymKind::Class && it->second.kind != Program::SymKind::Struct) return;
            auto* r = static_cast<RecordInfo*>(it->second.ptr);
            for (auto& [name, g] : r->statics) {
                (void)name;
                emit(g);
            }
        }
    }

    // ------------------------------------------------------------ types

    const Type* resolve_type(TypeExpr* te) {
        if (te == nullptr) return T.error();
        switch (te->kind) {
            case TypeKind::Error: return T.error();
            case TypeKind::Array: {
                // `[T; N]` (LANGUAGE.md 9.16): N is a constant expression.
                auto* at = as<ArrayType>(te);
                const Type* el = resolve_type(at->element);
                ir::Expr* n = expr_in_static(at->size, T.int_());
                auto v = const_eval(n);
                if (!v || v->kind != ConstValue::Kind::Int) {
                    if (!n->type->is_error()) error(at->size->span, "the size of '[T; N]' must be an integer constant");
                    return T.error();
                }
                if (v->i < 1 || v->i > 65535) {
                    error(at->size->span, std::format("the size of '[T; N]' must be between 1 and 65535, got {}", v->i));
                    return T.error();
                }
                if (el->is_error()) return el;
                return T.array(el, static_cast<std::uint32_t>(v->i));
            }
            case TypeKind::Fn: {
                auto* f = as<FnType>(te);
                std::vector<const Type*> params;
                for (TypeExpr* p : f->params) params.push_back(resolve_type(p));
                return T.fn(std::move(params), f->ret != nullptr ? resolve_type(f->ret) : T.void_());
            }
            case TypeKind::Nullable: {
                const Type* inner = resolve_type(as<NullableType>(te)->inner);
                return T.nullable(inner);
            }
            case TypeKind::Named: break;
        }
        auto* n = as<NamedType>(te);
        std::string_view name = n->name.name;
        std::vector<const Type*> args;
        for (TypeExpr* a : n->args) args.push_back(resolve_type(a));
        auto no_args = [&](const Type* t) {
            if (!args.empty()) {
                error(n->span, std::format("type '{}' does not take type arguments", name));
            }
            return t;
        };
        if (tparams_ != nullptr) {
            for (auto it = tparams_->rbegin(); it != tparams_->rend(); ++it) {
                if ((*it)->name == name) return no_args(T.param(*it));
            }
        }
        if (auto k = builtin_num(name)) return no_args(T.num(*k));
        if (name == "bool") return no_args(T.bool_());
        if (name == "str") return no_args(T.str());
        if (name == "name") return no_args(T.name());
        if (name == "void") {
            error(n->span, "'void' is not a value type; omit the return type instead");
            return T.error();
        }
        if (auto d = vec_dim(name)) return no_args(T.vec(*d));
        if (name == "Task") return no_args(T.task());
        if (name == "List") {
            if (args.size() != 1) {
                error(n->span, "'List' takes one type argument: List<T>");
                return T.error();
            }
            return T.list(args[0]);
        }
        if (name == "Map" || name == "Result") {
            if (args.size() != 2) {
                error(n->span, std::format("'{}' takes two type arguments: {}", name,
                                           name == "Map" ? "Map<K, V>" : "Result<T, E>"));
                return T.error();
            }
            if (name == "Result") return T.result(args[0], args[1]);
            if (!hashable(args[0])) require_hashable(args[0], n->args[0]->span, "a map key");
            return T.map(args[0], args[1]);
        }
        if (name == "Set" || name == "Deque" || name == "PriorityQueue") {
            if (args.size() != 1) {
                error(n->span, std::format("'{}' takes one type argument: {}<T>", name, name));
                return T.error();
            }
            if (name == "Set") {
                if (!hashable(args[0])) require_hashable(args[0], n->args[0]->span, "a set element");
                return T.set(args[0]);
            }
            return name == "Deque" ? T.deque(args[0]) : T.pqueue(args[0]);
        }
        if (name == "StrBuilder") return no_args(T.str_builder());
        auto it = P.globals.find(name);
        if (it == P.globals.end()) {
            error(n->name.span, std::format("unknown type '{}'", name));
            return T.error();
        }
        switch (it->second.kind) {
            case Program::SymKind::Class:
            case Program::SymKind::Struct: {
                auto* r = static_cast<RecordInfo*>(it->second.ptr);
                ensure_header(r);
                if (args.size() != r->generics.size()) {
                    if (r->generics.empty()) {
                        error(n->span, std::format("type '{}' does not take type arguments", name));
                    } else {
                        error(n->span, std::format("type '{}' needs {} type argument{}", name, r->generics.size(),
                                                   r->generics.size() == 1 ? "" : "s"));
                    }
                    return T.error();
                }
                for (std::size_t i = 0; i < args.size(); ++i) check_bounds(r->generics[i], args[i], n->args[i]->span);
                if (r->is_class) return T.class_(static_cast<ClassInfo*>(r), std::move(args));
                return T.struct_(static_cast<StructInfo*>(r), std::move(args));
            }
            case Program::SymKind::Enum: return no_args(static_cast<EnumInfo*>(it->second.ptr)->type);
            case Program::SymKind::Trait: {
                auto* t = static_cast<TraitInfo*>(it->second.ptr);
                if (t->builtin != BuiltinTrait::None) {
                    error(n->name.span, std::format("built-in trait '{}' can only be used as a bound", name));
                    return T.error();
                }
                return no_args(T.trait(t));
            }
            default: error(n->name.span, std::format("'{}' is not a type", name)); return T.error();
        }
    }

    // Does `t` satisfy trait `tr` (generic bounds, DESIGN.md 5.5)?
    bool satisfies(const Type* t, TraitInfo* tr) {
        if (t->is_error()) return true;
        BuiltinTrait b = tr->builtin;
        if (b == BuiltinTrait::Display || b == BuiltinTrait::Eq) return true; // every value prints and compares
        if (auto p = t->param()) {
            return std::find(p->bounds.begin(), p->bounds.end(), tr) != p->bounds.end();
        }
        if (t->tag == TypeTag::Trait) return t->tr() == tr;
        if (b != BuiltinTrait::None) {
            switch (t->tag) {
                case TypeTag::Num:
                    if (b == BuiltinTrait::Neg) return is_signed(t->num) || is_float(t->num);
                    return b == BuiltinTrait::Add || b == BuiltinTrait::Sub || b == BuiltinTrait::Mul ||
                           b == BuiltinTrait::Div || b == BuiltinTrait::Rem || b == BuiltinTrait::Ord ||
                           b == BuiltinTrait::Default;
                case TypeTag::Str:
                    return b == BuiltinTrait::Add || b == BuiltinTrait::Ord || b == BuiltinTrait::Default;
                case TypeTag::Bool:
                case TypeTag::Name: return b == BuiltinTrait::Default;
                case TypeTag::Vec:
                    return b == BuiltinTrait::Add || b == BuiltinTrait::Sub || b == BuiltinTrait::Mul ||
                           b == BuiltinTrait::Div || b == BuiltinTrait::Neg || b == BuiltinTrait::Default;
                default: break;
            }
        }
        if (RecordInfo* r = record_of(t)) {
            for (RecordInfo* x = r; x != nullptr; x = x->is_class ? static_cast<ClassInfo*>(x)->base : nullptr) {
                if (std::find(x->traits.begin(), x->traits.end(), tr) != x->traits.end()) return true;
            }
        }
        return false;
    }

    void check_bounds(GenericParamInfo* p, const Type* arg, Span span) {
        for (TraitInfo* tr : p->bounds) {
            if (!satisfies(arg, tr)) {
                error(span, std::format("type {} does not implement '{}' (required by '{}')", q(arg), tr->name, p->name));
            }
        }
    }

    RecordInfo* record_of(const Type* t) {
        if (auto c = t->cls()) return c;
        if (auto s = t->st()) return s;
        return nullptr;
    }

    std::vector<GenericParamInfo*> make_generics(const List<GenericParam>& gps) {
        std::vector<GenericParamInfo*> out;
        for (const GenericParam& g : gps) {
            auto p = std::make_unique<GenericParamInfo>();
            p->name = g.name.name;
            p->span = g.span;
            p->index = static_cast<std::uint32_t>(out.size());
            out.push_back(p.get());
            P.generic_params.push_back(std::move(p));
        }
        // Bounds are resolved with all the parameters in scope.
        std::vector<GenericParamInfo*> scope = tparams_ != nullptr ? *tparams_ : std::vector<GenericParamInfo*>{};
        scope.insert(scope.end(), out.begin(), out.end());
        const auto* saved = tparams_;
        tparams_ = &scope;
        for (std::size_t i = 0; i < gps.size(); ++i) {
            for (TypeExpr* b : gps[i].bounds) {
                if (auto* n = as<NamedType>(b); n != nullptr && n->args.empty()) {
                    auto it = P.globals.find(n->name.name);
                    if (it != P.globals.end() && it->second.kind == Program::SymKind::Trait) {
                        out[i]->bounds.push_back(static_cast<TraitInfo*>(it->second.ptr));
                        continue;
                    }
                }
                error(b->span, "a generic bound must be a trait");
            }
        }
        tparams_ = saved;
        return out;
    }

    // Substitution mapping a record's generics to the arguments in `t`,
    // walking up to `declaring` through `extends` clauses.
    Subst record_subst(const Type* t, const void* declaring) {
        Subst s;
        const Type* cur = t;
        while (cur != nullptr) {
            RecordInfo* r = record_of(cur);
            if (r == nullptr) return {};
            Subst level;
            for (std::size_t i = 0; i < r->generics.size(); ++i) {
                level[r->generics[i]] = i < cur->args.size() ? cur->args[i] : T.param(r->generics[i]);
            }
            if (r == declaring) return level;
            if (!r->is_class) return {};
            auto* c = static_cast<ClassInfo*>(r);
            if (c->base_type == nullptr) return {};
            cur = substitute(T, c->base_type, level);
        }
        return s;
    }

    // ------------------------------------------------------------ declarations

    void ensure_trait(TraitInfo* t) {
        if (t->done) return;
        t->done = true;
        TraitDecl* d = t->decl;
        if (!d->generics.empty()) error(d->name.span, "generic traits are not supported yet");
        if (!d->supertraits.empty()) error(d->name.span, "trait inheritance is not supported yet");
        for (Decl* m : d->members) {
            auto* fd = as<FnDecl>(m);
            if (fd == nullptr) continue;
            if (t->methods.count(fd->name.name) != 0) {
                error(fd->name.span, std::format("duplicate method '{}' in trait '{}'", fd->name.name, t->name));
                continue;
            }
            Function* f = new_function(std::format("{}.{}", t->name, fd->name.name), FnKind::Method, fd->span);
            f->decl = fd;
            f->owner = t;
            f->owner_kind = Function::Owner::Trait;
            f->is_abstract = fd->body == nullptr;
            t->methods[fd->name.name] = f;
            ensure_signature(f);
            if (f->is_coroutine) error(fd->name.span, "trait methods cannot be coroutines yet");
            if (f->is_native) error(fd->name.span, "trait methods cannot be '@native'");
        }
    }

    void ensure_enum(EnumInfo* e) {
        if (e->done) return;
        e->done = true;
        EnumDecl* d = e->decl;
        if (d->backing != nullptr) {
            const Type* b = resolve_type(d->backing);
            if (b->is_int()) {
                e->backing = b->num;
            } else if (!b->is_error()) {
                error(d->backing->span, "an enum's backing type must be an integer type");
            }
        }
        std::int64_t next = 0;
        for (const EnumVariant& v : d->variants) {
            if (e->variant_index(v.name.name) >= 0) {
                error(v.name.span, std::format("duplicate variant '{}' in enum '{}'", v.name.name, e->name));
                continue;
            }
            VariantInfo vi;
            vi.name = v.name.name;
            vi.span = v.span;
            if (v.has_params) {
                e->plain = false;
                for (const Param& p : v.params) {
                    for (std::string_view prev : vi.field_names) {
                        if (prev == p.name.name) error(p.span, std::format("duplicate field '{}'", p.name.name));
                    }
                    vi.field_names.push_back(p.name.name);
                    vi.field_types.push_back(p.type != nullptr ? resolve_type(p.type) : T.error());
                    if (p.default_value != nullptr) error(p.span, "variant fields cannot have default values");
                }
            }
            if (v.value != nullptr) {
                ir::Expr* x = expr_in_static(v.value, T.num(e->backing));
                auto c = const_eval(x);
                if (!c || c->kind != ConstValue::Kind::Int) {
                    error(v.value->span, "an enum discriminant must be an integer constant");
                } else {
                    next = c->i;
                }
            }
            if (v.value == nullptr && bit_width(e->backing) < 64 &&
                !num::fits(e->backing, next < 0, next < 0 ? 0 - static_cast<std::uint64_t>(next) : static_cast<std::uint64_t>(next))) {
                error(v.name.span, std::format("the discriminant {} of variant '{}' does not fit in '{}'; use a wider backing type",
                                               next, v.name.name, to_string(e->backing)));
            }
            vi.discriminant = next;
            next += 1;
            e->variants.push_back(std::move(vi));
        }
        if (!e->plain) {
            for (const EnumVariant& v : d->variants) {
                if (v.value != nullptr) {
                    error(v.value->span, "only enums without payloads can have explicit discriminants");
                }
            }
        }
        if (e->variants.empty()) error(e->span, std::format("enum '{}' has no variants", e->name));
    }

    void ensure_header(RecordInfo* r) {
        if (r->header_done) return;
        if (r->resolving) return; // a cycle; reported where it closes (below)
        r->resolving = true;
        const List<GenericParam>& gps = r->is_class ? static_cast<ClassInfo*>(r)->decl->generics
                                                    : static_cast<StructInfo*>(r)->decl->generics;
        const auto* saved = tparams_;
        std::vector<GenericParamInfo*> none;
        tparams_ = &none;
        r->generics = make_generics(gps);
        std::vector<const Type*> self_args;
        for (GenericParamInfo* g : r->generics) self_args.push_back(T.param(g));
        tparams_ = &r->generics;
        const List<TypeExpr*>* traits = nullptr;
        if (r->is_class) {
            auto* c = static_cast<ClassInfo*>(r);
            c->self_type = T.class_(c, self_args);
            traits = &c->decl->traits;
            if (c->decl->extends != nullptr) {
                const Type* b = resolve_type(c->decl->extends);
                if (ClassInfo* bc = b->cls()) {
                    ensure_header(bc);
                    if (bc->is_subclass_of(c)) {
                        error(c->decl->extends->span, std::format("'{}' inherits from itself", c->name));
                    } else {
                        c->base = bc;
                        c->base_type = b;
                    }
                } else if (!b->is_error()) {
                    error(c->decl->extends->span, std::format("'{}' can only extend a class, not {}", c->name, q(b)));
                }
            }
        } else {
            auto* s = static_cast<StructInfo*>(r);
            s->self_type = T.struct_(s, self_args);
            traits = &s->decl->traits;
        }
        for (TypeExpr* te : *traits) {
            if (auto* n = as<NamedType>(te); n != nullptr) {
                auto it = P.globals.find(n->name.name);
                if (it != P.globals.end() && it->second.kind == Program::SymKind::Trait) {
                    auto* tr = static_cast<TraitInfo*>(it->second.ptr);
                    if (tr->builtin == BuiltinTrait::Iterable) {
                        error(te->span, "the 'Iterable' trait is not supported yet");
                    }
                    r->traits.push_back(tr);
                    continue;
                }
            }
            if (resolve_type(te)->is_error()) continue;
            error(te->span, "only traits can be listed after ':'");
        }
        tparams_ = saved;
        r->resolving = false;
        r->header_done = true;
    }

    // Fields, methods, statics, the vtable, and trait conformance.
    void ensure_members(RecordInfo* r) {
        if (r->members_done) return;
        ensure_header(r);
        if (members_resolving_.count(r) != 0) return;
        members_resolving_.insert(r);
        ClassInfo* c = r->is_class ? static_cast<ClassInfo*>(r) : nullptr;
        if (c != nullptr && c->base != nullptr) {
            ensure_members(c->base);
            r->fields = c->base->fields;
            r->field_map = c->base->field_map;
            c->vtable = c->base->vtable;
        }
        const auto* saved = tparams_;
        tparams_ = &r->generics;
        const List<Decl*>& members = c != nullptr ? c->decl->members : static_cast<StructInfo*>(r)->decl->members;
        std::string_view kind_name = c != nullptr ? "class" : "struct";
        auto taken = [&](Ident n) {
            if (r->methods.count(n.name) != 0 || r->statics.count(n.name) != 0) return true;
            auto f = r->field_map.find(n.name);
            return f != r->field_map.end() && f->second->owner == r;
        };
        for (Decl* m : members) {
            if (m->kind == DeclKind::Error) continue;
            Ident n = m->name;
            if (taken(n)) {
                error(n.span, std::format("duplicate member '{}' in {} '{}'", n.name, kind_name, r->name));
                continue;
            }
            if (m->kind == DeclKind::Signal) {
                // A signal is a field holding its signal object, created by the
                // field initializers (DESIGN.md 5.9).
                auto* sd = as<SignalDecl>(m);
                if (r->field_map.count(n.name) != 0) {
                    error(n.span, std::format("'{}' is already declared in a base class", n.name));
                    continue;
                }
                auto f = std::make_unique<FieldInfo>();
                f->name = n.name;
                f->span = n.span;
                f->owner = r;
                f->index = static_cast<std::uint32_t>(r->fields.size());
                f->is_signal = true;
                f->init_checked = true;
                std::vector<const Type*> params;
                for (const Param& p : sd->params) {
                    for (std::string_view prev : f->signal_params) {
                        if (prev == p.name.name) error(p.span, std::format("duplicate parameter '{}'", p.name.name));
                    }
                    f->signal_params.push_back(p.name.name);
                    params.push_back(p.type != nullptr ? resolve_type(p.type) : T.error());
                    if (p.type == nullptr) error(p.span, std::format("signal parameter '{}' needs a type", p.name.name));
                    if (p.default_value != nullptr) error(p.span, "signal parameters cannot have default values");
                }
                f->type = T.signal(std::move(params));
                reject_export(m, "signals");
                r->fields.push_back(f.get());
                r->field_map[n.name] = f.get();
                P.fields.push_back(std::move(f));
                continue;
            }
            if (m->kind == DeclKind::Var || m->kind == DeclKind::Const) {
                auto* v = as<VarDecl>(m);
                if (v != nullptr && v->has_accessors && (m->is_static || m->kind == DeclKind::Const)) {
                    error(n.span, "property accessors (get/set) are only allowed on instance fields");
                }
                if (m->is_static || m->kind == DeclKind::Const) {
                    reject_export(m, m->kind == DeclKind::Const ? "constants" : "static variables");
                    if (v != nullptr && v->weak) error(n.span, "'weak' applies only to instance fields, not to static variables");
                    GlobalVar* g = new_global(m, std::format("{}.{}", r->name, n.name));
                    g->is_mutable = m->kind == DeclKind::Var;
                    static_owner_[g] = r;
                    r->statics[n.name] = g;
                    continue;
                }
                if (r->field_map.count(n.name) != 0) {
                    error(n.span, std::format("field '{}' is already declared in a base class", n.name));
                    continue;
                }
                auto f = std::make_unique<FieldInfo>();
                f->name = n.name;
                f->decl = v;
                f->span = n.span;
                f->owner = r;
                f->index = static_cast<std::uint32_t>(r->fields.size());
                if (v->type != nullptr) {
                    f->type = resolve_type(v->type);
                } else if (v->init == nullptr) {
                    error(n.span, std::format("field '{}' needs a type or an initializer", n.name));
                    f->type = T.error();
                }
                f->exported = read_export(m, f->export_hints);
                f->weak = v->weak;
                if (v->has_accessors) make_accessors(r, f.get(), v);
                r->fields.push_back(f.get());
                r->field_map[n.name] = f.get();
                P.fields.push_back(std::move(f));
                continue;
            }
            auto* fd = as<FnDecl>(m);
            if (fd == nullptr) continue;
            reject_export(m, "functions");
            bool is_init = n.name == "init";
            Function* f = new_function(std::format("{}.{}", r->name, n.name),
                                       m->is_static ? FnKind::Static : FnKind::Method, fd->span);
            f->decl = fd;
            f->owner = r;
            f->owner_kind = c != nullptr ? Function::Owner::Class : Function::Owner::Struct;
            f->is_final = m->is_final;
            r->methods[n.name] = f;
            ensure_signature(f);
            if (is_init) {
                if (c == nullptr) {
                    error(n.span, "structs have no 'init'; build them with a struct literal");
                } else if (m->is_static) {
                    error(n.span, "'init' cannot be static");
                } else {
                    c->init = f;
                    if (!f->ret->is_void()) error(n.span, "'init' cannot return a value");
                }
                continue;
            }
            if (c != nullptr && !m->is_static) assign_vslot(c, f);
        }
        if (c != nullptr) {
            c->field_init = new_function(std::format("{}.<fields>", c->name), FnKind::FieldInit, c->span);
            c->field_init->owner = c;
            c->field_init->owner_kind = Function::Owner::Class;
            c->field_init->ret = T.void_();
            c->field_init->signature_done = true;
            c->field_init->param_vars.push_back(new_local("self", c->self_type, c->span, false, true));
        } else {
            auto* s = static_cast<StructInfo*>(r);
            s->default_ctor = new_function(std::format("{}.<default>", s->name), FnKind::StructInit, s->span);
            s->default_ctor->owner = s;
            s->default_ctor->owner_kind = Function::Owner::Struct;
            s->default_ctor->ret = s->self_type;
            s->default_ctor->signature_done = true;
            s->default_ctor->generics = s->generics;
        }
        tparams_ = saved;
        r->members_done = true;
        members_resolving_.erase(r);
        check_conformance(r);
    }

    // `get { }` / `set(v) { }` of a field (LANGUAGE.md 2.5, 9.19): methods
    // the checker calls for reads and assignments. Their types are filled in
    // by accessor_sig once the field's type is known.
    void make_accessors(RecordInfo* r, FieldInfo* f, VarDecl* v) {
        auto make = [&](std::string_view what) {
            Function* a = new_function(std::format("{}.<{} {}>", r->name, what, f->name), FnKind::Method, v->span);
            a->owner = r;
            a->owner_kind = r->is_class ? Function::Owner::Class : Function::Owner::Struct;
            a->accessor_of = f;
            a->is_final = true;
            a->signature_done = true;
            a->body_done = true; // checked by check_accessors
            a->param_vars.push_back(new_local("self", r->self_type, v->span, false, true));
            return a;
        };
        if (v->getter != nullptr) f->getter = make("get");
        if (v->setter != nullptr) {
            f->setter = make("set");
            ParamInfo pi;
            pi.name = v->setter_param.name;
            pi.span = v->setter_param.span;
            f->setter->params.push_back(pi);
            f->setter->param_vars.push_back(new_local(v->setter_param.name, nullptr, v->setter_param.span, false, true));
            f->setter->ret = T.void_();
        }
        f->computed = v->getter != nullptr && v->init == nullptr;
        if (f->exported && f->computed) error(v->name.span, "a computed property cannot be '@export'ed (it has no storage)");
        if (f->weak) error(v->name.span, "a 'weak' field cannot have accessors");
    }

    void accessor_sig(FieldInfo* f) {
        const Type* t = field_type(f);
        if (f->getter != nullptr && f->getter->ret == nullptr) f->getter->ret = t;
        if (f->setter != nullptr && f->setter->params[0].type == nullptr) {
            f->setter->params[0].type = t;
            f->setter->param_vars[1]->type = t;
        }
    }

    void check_accessor(Function* a, BlockStmt* body) {
        accessor_sig(a->accessor_of);
        auto* rec = static_cast<RecordInfo*>(a->owner);
        Ctx ctx;
        ctx.fn = a;
        ctx.ret = a->ret;
        ctx.record = rec;
        ctx.generics = rec->generics;
        ctx.is_static = false;
        CtxSwap swap(*this, &ctx);
        push_scope();
        for (LocalVar* v : a->param_vars) declare_local(v);
        a->body = block(body);
        pop_scope();
        if (!a->ret->is_void() && !a->ret->is_error() && !always_returns(a->body)) {
            error(body->span, std::format("the getter of '{}' may reach the end without returning a value",
                                          a->accessor_of->name));
        }
    }

    void check_accessors(RecordInfo* r) {
        for (FieldInfo* f : r->fields) {
            if (f->owner != r || f->decl == nullptr || !f->decl->has_accessors) continue;
            if (f->getter != nullptr) check_accessor(f->getter, f->decl->getter);
            if (f->setter != nullptr) check_accessor(f->setter, f->decl->setter);
        }
    }

    void assign_vslot(ClassInfo* c, Function* f) {
        std::string_view name = f->decl->name.name;
        for (std::size_t i = 0; i < c->vtable.size(); ++i) {
            Function* b = c->vtable[i];
            if (method_name(b) != name) continue;
            if (b->is_final) {
                error(f->decl->name.span, std::format("cannot override final method '{}'", b->name));
            }
            Subst s = c->base_type != nullptr ? record_subst(c->base_type, b->owner) : Subst{};
            if (f->is_coroutine != b->is_coroutine) {
                error(f->decl->name.span,
                      std::format("'{}' {} a coroutine but overrides '{}', which {}", f->name,
                                  f->is_coroutine ? "is" : "is not", b->name, b->is_coroutine ? "is" : "is not"));
            } else if (!same_signature(f, b, s)) {
                error(f->decl->name.span,
                      std::format("'{}' overrides '{}' with a different signature", f->name, b->name));
            }
            f->vslot = static_cast<int>(i);
            f->overrides = b;
            c->vtable[i] = f;
            return;
        }
        f->vslot = static_cast<int>(c->vtable.size());
        c->vtable.push_back(f);
    }

    static std::string_view method_name(const Function* f) {
        if (f->decl != nullptr) return f->decl->name.name;
        std::string_view n = f->name;
        auto dot = n.rfind('.');
        return dot == std::string_view::npos ? n : n.substr(dot + 1);
    }

    bool same_signature(Function* a, Function* b, const Subst& b_subst) {
        ensure_signature(a);
        ensure_signature(b);
        if (a->params.size() != b->params.size()) return false;
        for (std::size_t i = 0; i < a->params.size(); ++i) {
            if (a->params[i].type != substitute(T, b->params[i].type, b_subst)) return false;
        }
        return a->ret == substitute(T, b->ret, b_subst);
    }

    void check_conformance(RecordInfo* r) {
        Span where = r->span;
        for (TraitInfo* tr : r->traits) {
            if (tr->builtin != BuiltinTrait::None) {
                check_builtin_trait(r, tr, where);
                continue;
            }
            ensure_trait(tr);
            for (auto& [name, tm] : tr->methods) {
                Function* impl = find_method(r, name);
                if (impl != nullptr && impl->owner_kind != Function::Owner::Trait) {
                    if (!same_signature(impl, tm, {})) {
                        error(impl->decl != nullptr ? impl->decl->name.span : where,
                              std::format("'{}' does not match the signature required by trait '{}'", impl->name,
                                          tr->name));
                    }
                    continue;
                }
                if (impl != nullptr) continue; // inherited default
                if (tm->is_abstract) {
                    error(where, std::format("'{}' does not implement '{}', required by trait '{}'", r->name, name,
                                             tr->name));
                    continue;
                }
                // The trait's default implementation (compiled once; `self`
                // is trait-typed inside it).
                r->methods[name] = tm;
                if (r->is_class) assign_vslot_default(static_cast<ClassInfo*>(r), tm);
            }
        }
    }

    void assign_vslot_default(ClassInfo* c, Function* f) {
        for (Function* v : c->vtable) {
            if (method_name(v) == method_name(f)) return;
        }
        // Trait defaults dispatch through the class's slot like any method;
        // the slot number is per class.
        c->trait_default_slots[f] = static_cast<int>(c->vtable.size());
        c->vtable.push_back(f);
    }

    void check_builtin_trait(RecordInfo* r, TraitInfo* tr, Span where) {
        if (tr->builtin == BuiltinTrait::Default) return;
        std::string_view m = tr->op_method;
        Function* f = find_method(r, m);
        if (f == nullptr) {
            error(where, std::format("'{}' lists trait '{}' but has no method '{}'", r->name, tr->name, m));
            return;
        }
        ensure_signature(f);
        std::size_t want = tr->builtin == BuiltinTrait::Neg || tr->builtin == BuiltinTrait::Display ? 0
                           : tr->builtin == BuiltinTrait::IndexSet                                ? 2
                                                                                                  : 1;
        Span at = f->decl != nullptr ? f->decl->name.span : where;
        if (f->params.size() != want || f->kind != FnKind::Method) {
            error(at, std::format("'{}' must take {} parameter{} for trait '{}'", f->name, want, want == 1 ? "" : "s",
                                  tr->name));
            return;
        }
        if (tr->builtin == BuiltinTrait::Eq && f->ret != T.bool_()) {
            error(at, std::format("'{}' must return bool for trait 'Eq'", f->name));
        } else if (tr->builtin == BuiltinTrait::Ord && f->ret != T.int_()) {
            error(at, std::format("'{}' must return int for trait 'Ord'", f->name));
        } else if (tr->builtin == BuiltinTrait::Display && f->ret != T.str()) {
            error(at, std::format("'{}' must return str for trait 'Display'", f->name));
        }
    }

    Function* find_method(RecordInfo* r, std::string_view name) {
        for (RecordInfo* x = r; x != nullptr; x = x->is_class ? static_cast<ClassInfo*>(x)->base : nullptr) {
            ensure_members(x);
            auto it = x->methods.find(name);
            if (it != x->methods.end()) return it->second;
        }
        return nullptr;
    }

    GlobalVar* find_static(RecordInfo* r, std::string_view name) {
        for (RecordInfo* x = r; x != nullptr; x = x->is_class ? static_cast<ClassInfo*>(x)->base : nullptr) {
            ensure_members(x);
            auto it = x->statics.find(name);
            if (it != x->statics.end()) return it->second;
        }
        return nullptr;
    }

    FieldInfo* find_field(RecordInfo* r, std::string_view name) {
        ensure_members(r);
        auto it = r->field_map.find(name);
        return it != r->field_map.end() ? it->second : nullptr;
    }

    int vslot_of(ClassInfo* c, Function* f) {
        if (f->owner_kind == Function::Owner::Trait) {
            for (ClassInfo* x = c; x != nullptr; x = x->base) {
                auto it = x->trait_default_slots.find(f);
                if (it != x->trait_default_slots.end()) return it->second;
            }
            return -1;
        }
        return f->vslot;
    }

    // Parameter and return types; default values.
    void ensure_signature(Function* f) {
        if (f->signature_done) return;
        f->signature_done = true;
        FnDecl* d = f->decl;
        f->is_coroutine = d->suspends;
        if (has_attribute(d, "native")) {
            f->is_native = true;
            if (P.native_names.find(f->name) == P.native_names.end()) {
                error(d->name.span, std::format("no native implementation is bound for '{}'", f->name));
            }
            if (!d->generics.empty()) error(d->name.span, "a '@native' function cannot be generic");
        }
        RecordInfo* rec = nullptr;
        if (f->owner_kind == Function::Owner::Class || f->owner_kind == Function::Owner::Struct) {
            rec = static_cast<RecordInfo*>(f->owner);
        }
        std::vector<GenericParamInfo*> scope = rec != nullptr ? rec->generics : std::vector<GenericParamInfo*>{};
        const auto* saved = tparams_;
        tparams_ = &scope;
        f->generics = make_generics(d->generics);
        scope.insert(scope.end(), f->generics.begin(), f->generics.end());
        if (f->has_self()) {
            const Type* self_t = rec != nullptr ? rec->self_type : T.trait(f->owner_trait());
            f->param_vars.push_back(new_local("self", self_t, d->name.span, false, true));
        }
        Ctx sig_ctx;
        sig_ctx.fn = chunk_;
        sig_ctx.record = rec;
        sig_ctx.generics = scope;
        for (const Param& p : d->params) {
            ParamInfo pi;
            pi.name = p.name.name;
            pi.ast = &p;
            pi.span = p.span;
            for (const ParamInfo& prev : f->params) {
                if (prev.name == pi.name) error(p.name.span, std::format("duplicate parameter '{}'", pi.name));
            }
            if (p.type != nullptr) pi.type = resolve_type(p.type);
            if (p.default_value != nullptr) {
                CtxSwap swap(*this, &sig_ctx);
                push_scope();
                pi.default_value = expect_type(p.default_value, pi.type, "default value");
                pop_scope();
                if (pi.type == nullptr) pi.type = literal_default_type(pi.default_value->type, p.default_value->span);
            }
            if (pi.type == nullptr) pi.type = T.error();
            if (pi.type->is_void()) error(p.span, "a parameter cannot be void");
            f->param_vars.push_back(new_local(pi.name, pi.type, p.name.span, false, true));
            f->params.push_back(pi);
        }
        f->ret = d->ret != nullptr ? resolve_type(d->ret) : T.void_();
        tparams_ = saved;
    }

    // Type for a variable inferred from an initializer.
    const Type* literal_default_type(const Type* t, Span span) {
        if (t->tag == TypeTag::Null) {
            error(span, "cannot infer a type from 'null'; add a type annotation");
            return T.error();
        }
        if (t->is_void()) {
            error(span, "expression has no value");
            return T.error();
        }
        return t;
    }

    void ensure_global(GlobalVar* g) {
        if (g->state == GlobalVar::State::Done) return;
        if (g->state == GlobalVar::State::Resolving) {
            error(g->span, std::format("the initializer of '{}' depends on itself", g->qualified));
            g->type = T.error();
            return;
        }
        g->state = GlobalVar::State::Resolving;
        Ctx ctx;
        ctx.fn = chunk_;
        ctx.ret = T.void_();
        auto so = static_owner_.find(g);
        if (so != static_owner_.end()) {
            ctx.record = so->second;
            ctx.generics = so->second->generics;
            if (!so->second->generics.empty()) {
                error(g->span, "generic types cannot have static variables yet");
            }
        }
        CtxSwap swap(*this, &ctx);
        push_scope();
        TypeExpr* te = nullptr;
        Expr* init = nullptr;
        if (auto v = as<VarDecl>(g->decl)) {
            te = v->type;
            init = v->init;
            if (v->weak && so == static_owner_.end()) {
                error(g->span, "'weak' applies only to class instance fields, not to global variables");
            }
            if (v->has_accessors && so == static_owner_.end()) {
                // The parser already reported it.
            }
        } else if (auto c = as<ConstDecl>(g->decl)) {
            te = c->type;
            init = c->value;
        }
        const Type* type = te != nullptr ? resolve_type(te) : nullptr;
        if (init != nullptr) {
            g->init = expect_type(init, type, "initializer");
            if (type == nullptr) type = literal_default_type(g->init->type, init->span);
        } else if (type == nullptr) {
            error(g->span, std::format("'{}' needs a type or an initializer", g->name));
            type = T.error();
        } else {
            g->init = default_value(type, g->span);
        }
        g->type = type;
        if (g->init != nullptr) {
            check_escape(g->init, Life::Global, init != nullptr ? init->span : g->span,
                         std::format("{} '{}'", so != static_owner_.end() ? "static variable" : "global", g->qualified));
        }
        if (g->is_const && init != nullptr && !type->is_error()) {
            std::size_t errors_before = diags_.error_count();
            auto v = const_eval(g->init);
            if (v) {
                g->value = *v;
            } else if (diags_.error_count() == errors_before) {
                error(init->span, std::format("the value of constant '{}' must be a constant expression", g->name));
            }
        }
        pop_scope();
        g->state = GlobalVar::State::Done;
    }

    const Type* field_type(FieldInfo* f) {
        if (f->type != nullptr) return f->type;
        if (f->resolving) {
            error(f->span, std::format("the type of field '{}' depends on itself", f->name));
            f->type = T.error();
            return f->type;
        }
        f->resolving = true;
        check_field_init(f);
        f->resolving = false;
        if (f->type == nullptr) f->type = T.error();
        return f->type;
    }

    // Field initializers run without `self` (they cannot read other fields).
    void check_field_init(FieldInfo* f) {
        if (f->init_checked) return;
        f->init_checked = true;
        if (f->decl == nullptr || f->decl->init == nullptr) return;
        auto* r = static_cast<RecordInfo*>(f->owner);
        Ctx ctx;
        ctx.fn = r->is_class ? static_cast<ClassInfo*>(r)->field_init : static_cast<StructInfo*>(r)->default_ctor;
        if (ctx.fn == nullptr) ctx.fn = chunk_;
        ctx.record = r;
        ctx.generics = r->generics;
        ctx.ret = T.void_();
        CtxSwap swap(*this, &ctx);
        push_scope();
        f->init = expect_type(f->decl->init, f->type, "field initializer");
        if (f->type == nullptr) f->type = literal_default_type(f->init->type, f->decl->init->span);
        pop_scope();
    }

    // Value of a field when a constructor or struct literal doesn't set it.
    // A generic-typed field without an initializer has no default; it is
    // null until assigned (struct literals must set it, see struct_literal).
    ir::Expr* field_default(FieldInfo* f, Span span) {
        field_type(f);
        check_field_init(f);
        if (f->init != nullptr) return f->init;
        if (f->type->tag == TypeTag::Param) return mk<ir::Default>(span, f->type);
        return default_value(f->type, span);
    }

    ir::Expr* default_value(const Type* t, Span span) {
        if (t->tag == TypeTag::Param) {
            error(span, std::format("a value of generic type {} needs an initializer", q(t)));
        } else if (auto e = t->en(); e != nullptr && !e->plain && !e->variants.empty() &&
                                     !e->variants[0].field_types.empty()) {
            error(span, std::format("enum {} has no default value (its first variant has a payload); add an "
                                    "initializer",
                                    q(t)));
        }
        return mk<ir::Default>(span, t);
    }

    // ------------------------------------------------------------ @export
    // LANGUAGE.md 2.10 / 9.13: `@export` marks instance fields that scene
    // files save (DESIGN.md 4.12) and the inspector shows.

    void reject_export(const Decl* d, std::string_view what) {
        if (const Attribute* a = find_attribute(d, "export")) {
            error(a->span, std::format("'@export' applies only to instance fields, not to {}", what));
        }
    }

    bool read_export(const Decl* d, FieldInfo::ExportHints& hints) {
        const Attribute* a = find_attribute(d, "export");
        if (a == nullptr) return false;
        for (const Arg& arg : a->args) {
            std::string_view n = arg.name.name;
            if (n.empty()) {
                error(arg.span, "'@export' arguments are named: range = lo..hi, step = s, hint = \"...\"");
            } else if (n == "range") {
                const auto* range = arg.value != nullptr && arg.value->kind == ExprKind::Range ? as<RangeExpr>(arg.value)
                                                                                                : nullptr;
                auto lo = range != nullptr ? literal_number(range->lo) : std::nullopt;
                auto hi = range != nullptr ? literal_number(range->hi) : std::nullopt;
                if (!lo || !hi || range->step != nullptr) {
                    error(arg.span, "'range' must be a range of number literals, e.g. range = 0..100");
                } else if (*hi < *lo) {
                    error(arg.span, "'range' is empty (hi < lo)");
                } else {
                    hints.has_range = true;
                    hints.range_min = *lo;
                    hints.range_max = *hi;
                }
            } else if (n == "step") {
                auto v = literal_number(arg.value);
                if (!v || *v <= 0.0) {
                    error(arg.span, "'step' must be a positive number literal");
                } else {
                    hints.step = *v;
                }
            } else if (n == "hint") {
                const auto* lit = arg.value != nullptr && arg.value->kind == ExprKind::String ? as<StringLit>(arg.value)
                                                                                                : nullptr;
                if (lit == nullptr || lit->parts.size() > 1 || (lit->parts.size() == 1 && lit->parts[0].expr != nullptr)) {
                    error(arg.span, "'hint' must be a string literal without interpolation");
                } else if (lit->parts.size() == 1) {
                    hints.hint = lit->parts[0].text;
                }
            } else {
                error(arg.name.span, std::format("unknown '@export' argument '{}' (expected range, step or hint)", n));
            }
        }
        return true;
    }

    // Why `t` can't be saved in a scene file, or empty if it can: numbers,
    // bool, str, name, vecN, plain enums, structs of such fields and lists of
    // them. References to other objects (classes, functions, tasks) can't be.
    std::string unexportable_reason(const Type* t, std::set<const StructInfo*>& seen) {
        switch (t->tag) {
            case TypeTag::Error:
            case TypeTag::Bool:
            case TypeTag::Num:
            case TypeTag::Str:
            case TypeTag::Name:
            case TypeTag::Vec: return {};
            case TypeTag::Enum:
                return t->en()->plain ? std::string{} : std::format("enum '{}' has payloads", t->en()->name);
            case TypeTag::List: return unexportable_reason(t->args[0], seen);
            case TypeTag::Struct: {
                StructInfo* st = t->st();
                if (!t->args.empty() || !st->generics.empty()) return "generic structs can't be exported yet";
                if (!seen.insert(st).second) return {};
                ensure_members(st);
                for (FieldInfo* f : st->fields) {
                    std::string why = unexportable_reason(field_type(f), seen);
                    if (!why.empty()) return std::format("field '{}.{}': {}", st->name, f->name, why);
                }
                return {};
            }
            case TypeTag::Class: return "class references are not saved; save child nodes instead";
            default: return std::format("'{}' values can't be saved", type_name(t));
        }
    }

    void check_exports(RecordInfo* r) {
        for (FieldInfo* f : r->fields) {
            if (f->owner != r || !f->exported) continue;
            std::set<const StructInfo*> seen;
            std::string why = unexportable_reason(field_type(f), seen);
            if (!why.empty()) {
                error(f->span, std::format("field '{}' of type '{}' can't be exported: {}", f->name,
                                           type_name(field_type(f)), why));
                f->exported = false;
                continue;
            }
            const Type* ft = field_type(f);
            if ((f->export_hints.has_range || f->export_hints.step > 0.0) && !ft->is_num()) {
                error(f->span, std::format("'range' and 'step' apply only to number fields; '{}' is '{}'", f->name,
                                           type_name(ft)));
            }
        }
    }

    // ------------------------------------------------------------ memory
    // DESIGN.md 5.10 / LANGUAGE.md 2.11: `weak var`, `manual class`,
    // `@acyclic` and field initializers that would leak a frame object.

    void check_memory_decls(RecordInfo* r) {
        for (FieldInfo* f : r->fields) {
            if (f->owner != r) continue;
            if (f->weak) {
                const Type* t = T.non_null(field_type(f));
                if (!r->is_class) {
                    error(f->span, "'weak' fields are only allowed in classes (structs are values)");
                    f->weak = false;
                } else if (t->tag != TypeTag::Class && t->tag != TypeTag::List && !t->is_error()) {
                    error(f->span, std::format("a 'weak' field must have a class or list type, not {}", q(field_type(f))));
                    f->weak = false;
                }
            }
            check_field_init(f);
            if (f->init != nullptr) check_escape(f->init, Life::Long, f->decl->init->span, "a field initializer");
        }
        if (!r->is_class) return;
        auto* c = static_cast<ClassInfo*>(r);
        if (c->base != nullptr && c->base->manual && !c->manual) {
            error(c->span, std::format("'{}' extends manual class '{}', so it must be a 'manual class' too", c->name,
                                       c->base->name));
        }
    }

    void build_record_inits(RecordInfo* r) {
        ensure_members(r);
        if (r->is_class) {
            auto* c = static_cast<ClassInfo*>(r);
            std::vector<ir::Stmt*> stmts;
            LocalVar* self = c->field_init->param_vars[0];
            if (c->base != nullptr) {
                auto* call = mk<ir::Call>(c->span, T.void_());
                call->fn = c->base->field_init;
                auto* sx = mk<ir::Local>(c->span, self->type);
                sx->var = self;
                call->args = list(std::vector<ir::Expr*>{sx});
                auto* es = mks<ir::ExprStmt>(c->span);
                es->expr = call;
                stmts.push_back(es);
            }
            for (FieldInfo* f : r->fields) {
                if (f->owner != r) continue;
                auto* a = mks<ir::Assign>(f->span);
                auto* sx = mk<ir::Local>(f->span, self->type);
                sx->var = self;
                auto* fx = mk<ir::Field>(f->span, field_type(f));
                fx->object = sx;
                fx->field = f;
                a->target = fx;
                a->value = field_default(f, f->span);
                stmts.push_back(a);
            }
            auto* b = mks<ir::Block>(c->span);
            b->stmts = list(stmts);
            c->field_init->body = b;
            c->field_init->body_done = true;
        } else {
            auto* s = static_cast<StructInfo*>(r);
            auto* sn = mk<ir::StructNew>(s->span, s->self_type);
            sn->st = s;
            std::vector<ir::Expr*> fields;
            std::vector<std::uint32_t> order;
            for (FieldInfo* f : r->fields) {
                fields.push_back(field_default(f, f->span));
                order.push_back(f->index);
            }
            sn->fields = list(fields);
            sn->order = list(order);
            auto* ret = mks<ir::Return>(s->span);
            ret->value = sn;
            auto* b = mks<ir::Block>(s->span);
            b->stmts = list(std::vector<ir::Stmt*>{ret});
            s->default_ctor->body = b;
            s->default_ctor->body_done = true;
        }
    }

    // ------------------------------------------------------------ bodies

    void check_body(Function* f) {
        if (f->body_done) return;
        f->body_done = true;
        ensure_signature(f);
        if (f->is_native) {
            // The host implements it (pluma/bindings.hpp); the declaration
            // only gives the signature.
            if (f->decl->body != nullptr && !f->decl->body->stmts.empty()) {
                error(f->decl->body->span, "the body of a '@native' function must be empty");
            }
            if (f->is_coroutine) error(f->decl->name.span, "a '@native' function cannot be a coroutine");
            f->body = mks<ir::Block>(f->span);
            return;
        }
        Ctx ctx;
        ctx.fn = f;
        ctx.ret = f->ret;
        RecordInfo* rec = nullptr;
        if (f->owner_kind == Function::Owner::Class || f->owner_kind == Function::Owner::Struct) {
            rec = static_cast<RecordInfo*>(f->owner);
            ctx.generics = rec->generics;
        }
        ctx.record = rec;
        ctx.trait = f->owner_trait();
        ctx.is_static = !f->has_self();
        ctx.generics.insert(ctx.generics.end(), f->generics.begin(), f->generics.end());
        CtxSwap swap(*this, &ctx);
        push_scope();
        for (LocalVar* v : f->param_vars) declare_local(v);
        f->body = block(f->decl->body);
        pop_scope();
        if (!f->ret->is_void() && !f->ret->is_error() && !always_returns(f->body)) {
            error(f->decl->name.span, std::format("'{}' may reach the end without returning a value", f->name));
        }
    }

    // Does control never fall off the end of `s`?
    bool always_returns(const ir::Stmt* s) {
        if (s == nullptr) return false;
        switch (s->kind) {
            case ir::SK::Return: return true;
            case ir::SK::Block:
                for (const ir::Stmt* x : ir::as<ir::Block>(s)->stmts) {
                    if (always_returns(x)) return true;
                }
                return false;
            case ir::SK::If: {
                auto* i = ir::as<ir::If>(s);
                return always_returns(i->then_body) && always_returns(i->else_body);
            }
            case ir::SK::Match: {
                auto* m = ir::as<ir::Match>(s);
                if (m->arms.empty()) return false;
                for (const ir::MatchArm& a : m->arms) {
                    if (!always_returns(a.body)) return false;
                }
                return true;
            }
            case ir::SK::Loop: return !breaks_out(ir::as<ir::Loop>(s)->body, s);
            case ir::SK::Expr: {
                auto* b = ir::as<ir::BuiltinCall>(ir::as<ir::ExprStmt>(s)->expr);
                return b != nullptr && b->builtin == Builtin::Panic;
            }
            default: return false;
        }
    }

    bool breaks_out(const ir::Stmt* s, const ir::Stmt* loop) {
        if (s == nullptr) return false;
        switch (s->kind) {
            case ir::SK::Break: return ir::as<ir::Break>(s)->loop == loop;
            case ir::SK::Block:
                for (const ir::Stmt* x : ir::as<ir::Block>(s)->stmts) {
                    if (breaks_out(x, loop)) return true;
                }
                return false;
            case ir::SK::If: {
                auto* i = ir::as<ir::If>(s);
                return breaks_out(i->then_body, loop) || breaks_out(i->else_body, loop);
            }
            case ir::SK::Match:
                for (const ir::MatchArm& a : ir::as<ir::Match>(s)->arms) {
                    if (breaks_out(a.body, loop)) return true;
                }
                return false;
            case ir::SK::While: return breaks_out(ir::as<ir::While>(s)->body, loop);
            case ir::SK::Loop: return breaks_out(ir::as<ir::Loop>(s)->body, loop);
            case ir::SK::ForRange: return breaks_out(ir::as<ir::ForRange>(s)->body, loop);
            case ir::SK::ForList: return breaks_out(ir::as<ir::ForList>(s)->body, loop);
            default: return false;
        }
    }

#include "pluma/checker_expr.inc"
#include "pluma/checker_stmt.inc"
#include "pluma/checker_lib.inc"

    // ------------------------------------------------------------ state

    Program& P;
    TypeTable& T;
    Arena& A;
    const SourceFile& src_;
    DiagnosticSink& diags_;
    CheckOptions opts_;

    Ctx* cx_ = nullptr;
    const std::vector<GenericParamInfo*>* tparams_ = nullptr;
    Ctx chunk_ctx_;
    Function* chunk_ = nullptr;
    Function* main_ = nullptr;

    std::vector<RecordInfo*> new_records_;
    std::vector<EnumInfo*> new_enums_;
    std::vector<TraitInfo*> new_traits_;
    std::vector<Function*> new_free_fns_;
    std::vector<GlobalVar*> new_globals_;
    std::vector<std::string> added_names_;
    std::set<RecordInfo*> members_resolving_;
    std::map<const GlobalVar*, RecordInfo*> static_owner_;
    std::vector<LocalVar*>* cond_binds_ = nullptr; // set while checking an `if` condition
    // Coroutine calls are only valid as the operand of `await` / `start`
    // (DESIGN.md 5.9): `coro_call_ok_` is that call while it is checked, and
    // `allow_coro_` is true while its callee is being resolved.
    const CallExpr* coro_call_ok_ = nullptr;
    bool allow_coro_ = false;
    std::map<const Function*, std::uint32_t> lambda_counts_; // for Function::stable_key
    GenericParamInfo* infer_param_ = nullptr;                // see infer_t()
    // Method-value thunks (LANGUAGE.md 9.19), by method and receiver type.
    std::map<std::pair<const Function*, const Type*>, Function*> bound_thunks_;
    // Reading or assigning a field as a place (no getter call): set by lvalue().
    bool raw_field_ = false;

    std::size_t start_classes_ = 0, start_structs_ = 0, start_enums_ = 0, start_traits_ = 0, start_functions_ = 0,
                start_globals_ = 0;
    std::vector<std::shared_ptr<void>> graveyard_;
};

} // namespace

CheckedUnit check_script(Program& program, const SourceFile& source, const Script& script, DiagnosticSink& diags,
                         CheckOptions options) {
    Checker c(program, source, diags, options);
    return c.run_script(script);
}

CheckedUnit check_module(Program& program, const SourceFile& source, const Module& module, DiagnosticSink& diags,
                         CheckOptions options) {
    Checker c(program, source, diags, options);
    return c.run_module(module);
}

} // namespace pluma

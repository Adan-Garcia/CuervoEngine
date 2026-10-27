#include "pluma/printer.hpp"

#include <format>

namespace pluma {

namespace {

Prec next_prec(Prec p) { return static_cast<Prec>(static_cast<int>(p) + 1); }

// Mirrors the parser's restrictions: where it would not accept a construct
// bare, the printer wraps it in parentheses.
struct Ctx {
    bool no_struct = false; // loop / if / match headers (LANGUAGE.md 2.6)
    bool no_lambda = false; // match guards: `=>` would end the guard
};

// Does the printed form of `e` end with a postfix `?`? Another `?` right after
// it would lex as `??`.
bool ends_with_try(const Expr* e) {
    while (e != nullptr) {
        switch (e->kind) {
            case ExprKind::Try: return true;
            case ExprKind::Binary: e = as<BinaryExpr>(e)->rhs; break;
            case ExprKind::Unary: e = as<UnaryExpr>(e)->operand; break;
            case ExprKind::Await: e = as<AwaitExpr>(e)->operand; break;
            case ExprKind::Ternary: e = as<TernaryExpr>(e)->else_expr; break;
            case ExprKind::Range: {
                auto r = as<RangeExpr>(e);
                e = r->step != nullptr ? r->step : r->hi;
                break;
            }
            case ExprKind::Lambda: e = as<LambdaExpr>(e)->expr_body; break;
            default: return false;
        }
    }
    return false;
}

// Does the printed form of `e` start with `{` (a map literal)? A statement
// starting with `{` would be read as a block.
bool starts_with_brace(const Expr* e) {
    while (e != nullptr) {
        switch (e->kind) {
            case ExprKind::MapLit: return true;
            case ExprKind::Binary: e = as<BinaryExpr>(e)->lhs; break;
            case ExprKind::Range: e = as<RangeExpr>(e)->lo; break;
            case ExprKind::Ternary: e = as<TernaryExpr>(e)->cond; break;
            case ExprKind::Is: e = as<IsExpr>(e)->operand; break;
            case ExprKind::As: e = as<AsExpr>(e)->operand; break;
            case ExprKind::Call: e = as<CallExpr>(e)->callee; break;
            case ExprKind::Index: e = as<IndexExpr>(e)->object; break;
            case ExprKind::Member: e = as<MemberExpr>(e)->object; break;
            case ExprKind::Try: e = as<TryExpr>(e)->operand; break;
            default: return false;
        }
    }
    return false;
}

bool is_big_member(const Decl* d) {
    if (auto f = as<FnDecl>(d)) return f->body != nullptr;
    if (auto v = as<VarDecl>(d)) return v->has_accessors;
    return d->kind == DeclKind::Class || d->kind == DeclKind::Struct || d->kind == DeclKind::Enum ||
           d->kind == DeclKind::Trait;
}

class Printer {
public:
    std::string take() { return std::move(out_); }

    void module(const Module& m) {
        bool first = true;
        for (const Decl* d : m.decls) {
            if (!first) out_ += '\n';
            first = false;
            decl(d);
        }
    }

    void script(const Script& s) {
        const Stmt* prev = nullptr;
        for (const Stmt* st : s.stmts) {
            if (prev != nullptr && (is_big(prev) || is_big(st))) out_ += '\n';
            stmt(st);
            prev = st;
        }
    }

    void expr(const Expr* e, Prec required = Prec::Lowest, Ctx ctx = {}) {
        // Trees with syntax errors may have missing children.
        if (e == nullptr) {
            out_ += "<error>";
            return;
        }
        bool parens = precedence(e) < required || (ctx.no_struct && (e->kind == ExprKind::StructLit ||
                                                                       e->kind == ExprKind::MapLit)) ||
                      (ctx.no_lambda && e->kind == ExprKind::Lambda && as<LambdaExpr>(e)->arrow);
        if (parens) {
            out_ += '(';
            ctx = {};
        }
        expr_inner(e, ctx);
        if (parens) out_ += ')';
    }

private:
    static bool is_big(const Stmt* s) {
        auto d = as<DeclStmt>(s);
        return d != nullptr && is_big_member(d->decl);
    }

    void line_start() {
        if (inline_ == 0) out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
    }

    void end_line() {
        if (inline_ == 0) out_ += '\n';
    }

    // ------------------------------------------------------------ declarations

    void attributes(const List<Attribute>& attrs) {
        for (const Attribute& a : attrs) {
            line_start();
            out_ += '@';
            out_ += a.name.name;
            if (a.has_args) args(a.args);
            end_line();
            if (inline_ > 0) out_ += ' ';
        }
    }

    void decl(const Decl* d) {
        if (d != nullptr) attributes(d->attributes);
        line_start();
        decl_body(d);
        end_line();
    }

    void decl_body(const Decl* d) {
        if (d == nullptr) {
            out_ += "<error>";
            return;
        }
        if (d->is_pub) out_ += "pub ";
        if (d->is_static) out_ += "static ";
        if (d->is_final) out_ += "final ";
        switch (d->kind) {
            case DeclKind::Error: out_ += "<error>"; break;
            case DeclKind::Var: {
                auto v = as<VarDecl>(d);
                if (v->weak) out_ += "weak ";
                out_ += "var ";
                out_ += v->name.name;
                if (v->type != nullptr) {
                    out_ += ": ";
                    type(v->type);
                }
                if (v->init != nullptr) {
                    out_ += " = ";
                    expr(v->init);
                }
                if (v->has_accessors) accessors(v);
                break;
            }
            case DeclKind::Const: {
                auto c = as<ConstDecl>(d);
                out_ += "const ";
                out_ += c->name.name;
                if (c->type != nullptr) {
                    out_ += ": ";
                    type(c->type);
                }
                out_ += " = ";
                expr(c->value);
                break;
            }
            case DeclKind::Fn: {
                auto f = as<FnDecl>(d);
                out_ += "fn ";
                out_ += f->name.name;
                generics(f->generics);
                params(f->params);
                if (f->ret != nullptr) {
                    out_ += " -> ";
                    type(f->ret);
                }
                if (f->body != nullptr) {
                    out_ += ' ';
                    block(f->body);
                }
                break;
            }
            case DeclKind::Class: {
                auto c = as<ClassDecl>(d);
                if (c->manual) out_ += "manual ";
                out_ += "class ";
                out_ += c->name.name;
                generics(c->generics);
                if (c->extends != nullptr) {
                    out_ += " extends ";
                    type(c->extends);
                }
                if (!c->traits.empty()) {
                    out_ += ": ";
                    types(c->traits);
                }
                members(c->members);
                break;
            }
            case DeclKind::Struct: {
                auto s = as<StructDecl>(d);
                out_ += "struct ";
                out_ += s->name.name;
                generics(s->generics);
                if (!s->traits.empty()) {
                    out_ += ": ";
                    types(s->traits);
                }
                members(s->members);
                break;
            }
            case DeclKind::Trait: {
                auto t = as<TraitDecl>(d);
                out_ += "trait ";
                out_ += t->name.name;
                generics(t->generics);
                if (!t->supertraits.empty()) {
                    out_ += ": ";
                    types(t->supertraits);
                }
                members(t->members);
                break;
            }
            case DeclKind::Enum: {
                auto e = as<EnumDecl>(d);
                out_ += "enum ";
                out_ += e->name.name;
                if (e->backing != nullptr) {
                    out_ += ": ";
                    type(e->backing);
                }
                if (e->variants.empty()) {
                    out_ += " {}";
                    break;
                }
                // Style guide: trailing commas in multiline enums.
                out_ += " {\n";
                ++indent_;
                for (const EnumVariant& v : e->variants) {
                    line_start();
                    out_ += v.name.name;
                    if (v.has_params) params(v.params);
                    if (v.value != nullptr) {
                        out_ += " = ";
                        expr(v.value);
                    }
                    out_ += ",\n";
                }
                --indent_;
                line_start();
                out_ += '}';
                break;
            }
            case DeclKind::Signal: {
                auto s = as<SignalDecl>(d);
                out_ += "signal ";
                out_ += s->name.name;
                if (s->has_params) params(s->params);
                break;
            }
        }
    }

    void accessors(const VarDecl* v) {
        out_ += " {\n";
        ++indent_;
        if (v->getter != nullptr) {
            line_start();
            out_ += "get ";
            block(v->getter);
            out_ += '\n';
        }
        if (v->setter != nullptr) {
            line_start();
            out_ += "set(";
            out_ += v->setter_param.name;
            out_ += ") ";
            block(v->setter);
            out_ += '\n';
        }
        --indent_;
        line_start();
        out_ += '}';
    }

    void members(const List<Decl*>& ms) {
        if (ms.empty()) {
            out_ += " {}";
            return;
        }
        out_ += " {\n";
        ++indent_;
        const Decl* prev = nullptr;
        for (const Decl* m : ms) {
            if (prev != nullptr && (is_big_member(prev) || is_big_member(m))) out_ += '\n';
            decl(m);
            prev = m;
        }
        --indent_;
        line_start();
        out_ += '}';
    }

    void generics(const List<GenericParam>& gs) {
        if (gs.empty()) return;
        out_ += '<';
        bool first = true;
        for (const GenericParam& g : gs) {
            if (!first) out_ += ", ";
            first = false;
            out_ += g.name.name;
            if (!g.bounds.empty()) {
                out_ += ": ";
                bool first_bound = true;
                for (const TypeExpr* b : g.bounds) {
                    if (!first_bound) out_ += " + ";
                    first_bound = false;
                    type(b);
                }
            }
        }
        out_ += '>';
    }

    void params(const List<Param>& ps) {
        out_ += '(';
        bool first = true;
        for (const Param& p : ps) {
            if (!first) out_ += ", ";
            first = false;
            out_ += p.name.name;
            if (p.type != nullptr) {
                out_ += ": ";
                type(p.type);
            }
            if (p.default_value != nullptr) {
                out_ += " = ";
                expr(p.default_value);
            }
        }
        out_ += ')';
    }

    // ------------------------------------------------------------ types

    void types(const List<TypeExpr*>& ts) {
        bool first = true;
        for (const TypeExpr* t : ts) {
            if (!first) out_ += ", ";
            first = false;
            type(t);
        }
    }

    void type(const TypeExpr* t) {
        if (t == nullptr) {
            out_ += "<error>";
            return;
        }
        switch (t->kind) {
            case TypeKind::Error: out_ += "<error>"; break;
            case TypeKind::Named: {
                auto n = as<NamedType>(t);
                if (!n->qualifier.empty()) {
                    out_ += n->qualifier.name;
                    out_ += '.';
                }
                out_ += n->name.name;
                if (!n->args.empty()) {
                    out_ += '<';
                    types(n->args);
                    out_ += '>';
                }
                break;
            }
            case TypeKind::Array: {
                auto a = as<ArrayType>(t);
                out_ += '[';
                type(a->element);
                out_ += "; ";
                expr(a->size);
                out_ += ']';
                break;
            }
            case TypeKind::Fn: {
                auto f = as<FnType>(t);
                out_ += "fn(";
                types(f->params);
                out_ += ')';
                if (f->ret != nullptr) {
                    out_ += " -> ";
                    type(f->ret);
                }
                break;
            }
            case TypeKind::Nullable:
                type(as<NullableType>(t)->inner);
                out_ += '?';
                break;
        }
    }

    // ------------------------------------------------------------ statements

    void block(const BlockStmt* b) {
        if (b == nullptr) {
            out_ += "{ <error> }";
            return;
        }
        if (b->stmts.empty()) {
            out_ += "{}";
            return;
        }
        if (inline_ > 0) {
            out_ += "{ ";
            bool first = true;
            for (const Stmt* s : b->stmts) {
                if (!first) out_ += "; ";
                first = false;
                stmt_body(s);
            }
            out_ += " }";
            return;
        }
        out_ += "{\n";
        ++indent_;
        for (const Stmt* s : b->stmts) stmt(s);
        --indent_;
        line_start();
        out_ += '}';
    }

    void stmt(const Stmt* s) {
        if (s == nullptr) return;
        if (auto d = as<DeclStmt>(s)) {
            decl(d->decl);
            return;
        }
        if (auto f = as<ForeachStmt>(s)) attributes(f->attributes);
        line_start();
        stmt_body(s);
        end_line();
    }

    void label(const Ident& l) {
        if (l.empty()) return;
        out_ += l.name;
        out_ += ": ";
    }

    void stmt_body(const Stmt* s) {
        if (s == nullptr) {
            out_ += "<error>";
            return;
        }
        switch (s->kind) {
            case StmtKind::Error: out_ += "<error>"; break;
            case StmtKind::Expr: {
                const Expr* e = as<ExprStmt>(s)->expr;
                if (starts_with_brace(e)) {
                    out_ += '(';
                    expr(e);
                    out_ += ')';
                } else {
                    expr(e);
                }
                break;
            }
            case StmtKind::Decl: decl_body(as<DeclStmt>(s)->decl); break;
            case StmtKind::Assign: {
                auto a = as<AssignStmt>(s);
                if (starts_with_brace(a->target)) {
                    out_ += '(';
                    expr(a->target);
                    out_ += ')';
                } else {
                    expr(a->target);
                }
                out_ += ' ';
                out_ += to_string(a->op);
                out_ += ' ';
                expr(a->value);
                break;
            }
            case StmtKind::Block: block(as<BlockStmt>(s)); break;
            case StmtKind::If: {
                auto i = as<IfStmt>(s);
                bool first = true;
                for (const IfBranch& b : i->branches) {
                    out_ += first ? "if " : " else if ";
                    first = false;
                    bool wrap = !b.unwrap.empty() && ends_with_try(b.cond);
                    if (wrap) out_ += '(';
                    expr(b.cond, Prec::Lowest, {.no_struct = !wrap, .no_lambda = false});
                    if (wrap) out_ += ')';
                    if (!b.unwrap.empty()) {
                        out_ += "? ";
                        out_ += b.unwrap.name;
                    }
                    out_ += ' ';
                    block(b.body);
                }
                if (i->else_body != nullptr) {
                    out_ += " else ";
                    block(i->else_body);
                }
                break;
            }
            case StmtKind::Match: {
                auto m = as<MatchStmt>(s);
                out_ += "match ";
                expr(m->subject, Prec::Lowest, {.no_struct = true, .no_lambda = false});
                if (m->arms.empty()) {
                    out_ += " {}";
                    break;
                }
                out_ += inline_ > 0 ? " { " : " {\n";
                ++indent_;
                bool first = true;
                for (const MatchArm& a : m->arms) {
                    if (inline_ > 0 && !first) out_ += "; ";
                    first = false;
                    line_start();
                    pattern(a.pattern);
                    if (a.guard != nullptr) {
                        out_ += " if ";
                        expr(a.guard, Prec::Lowest, {.no_struct = false, .no_lambda = true});
                    }
                    out_ += " => ";
                    stmt_body(a.body);
                    end_line();
                }
                --indent_;
                if (inline_ > 0) {
                    out_ += " }";
                } else {
                    line_start();
                    out_ += '}';
                }
                break;
            }
            case StmtKind::For: {
                auto f = as<ForStmt>(s);
                label(f->label);
                out_ += "for ";
                out_ += f->first.name;
                if (!f->second.empty()) {
                    out_ += ", ";
                    out_ += f->second.name;
                }
                out_ += " in ";
                expr(f->iterable, Prec::Lowest, {.no_struct = true, .no_lambda = false});
                out_ += ' ';
                block(f->body);
                break;
            }
            case StmtKind::Foreach: {
                auto f = as<ForeachStmt>(s);
                out_ += "foreach ";
                out_ += f->var.name;
                out_ += " in ";
                expr(f->pool, Prec::Lowest, {.no_struct = true, .no_lambda = false});
                out_ += ' ';
                block(f->body);
                break;
            }
            case StmtKind::While: {
                auto w = as<WhileStmt>(s);
                label(w->label);
                out_ += "while ";
                expr(w->cond, Prec::Lowest, {.no_struct = true, .no_lambda = false});
                out_ += ' ';
                block(w->body);
                break;
            }
            case StmtKind::Loop: {
                auto l = as<LoopStmt>(s);
                label(l->label);
                out_ += "loop ";
                block(l->body);
                break;
            }
            case StmtKind::Return: {
                auto r = as<ReturnStmt>(s);
                out_ += "return";
                if (r->value != nullptr) {
                    out_ += ' ';
                    expr(r->value);
                }
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue: {
                const Ident& l = s->kind == StmtKind::Break ? as<BreakStmt>(s)->label : as<ContinueStmt>(s)->label;
                out_ += s->kind == StmtKind::Break ? "break" : "continue";
                if (!l.empty()) {
                    out_ += ' ';
                    out_ += l.name;
                }
                break;
            }
            case StmtKind::Defer:
                out_ += "defer ";
                stmt_body(as<DeferStmt>(s)->body);
                break;
            case StmtKind::Unsafe:
                out_ += "unsafe ";
                block(as<UnsafeStmt>(s)->body);
                break;
            case StmtKind::Yield: out_ += "yield"; break;
            case StmtKind::Emit: {
                auto e = as<EmitStmt>(s);
                out_ += "emit ";
                out_ += e->signal.name;
                if (e->has_args) args(e->args);
                break;
            }
        }
    }

    // ------------------------------------------------------------ patterns

    void pattern(const Pattern* p) {
        if (p == nullptr) {
            out_ += "<error>";
            return;
        }
        switch (p->kind) {
            case PatternKind::Error: out_ += "<error>"; break;
            case PatternKind::Wildcard: out_ += '_'; break;
            case PatternKind::Literal: expr(as<LiteralPattern>(p)->value); break;
            case PatternKind::Range: {
                auto r = as<RangePattern>(p);
                expr(r->lo);
                out_ += r->inclusive ? "..=" : "..";
                expr(r->hi);
                break;
            }
            case PatternKind::Path: {
                auto pp = as<PathPattern>(p);
                bool first = true;
                for (const Ident& id : pp->path) {
                    if (!first) out_ += '.';
                    first = false;
                    out_ += id.name;
                }
                if (pp->has_args) {
                    out_ += '(';
                    bool first_arg = true;
                    for (const Pattern* a : pp->args) {
                        if (!first_arg) out_ += ", ";
                        first_arg = false;
                        pattern(a);
                    }
                    out_ += ')';
                }
                break;
            }
            case PatternKind::Or: {
                bool first = true;
                for (const Pattern* a : as<OrPattern>(p)->alternatives) {
                    if (!first) out_ += " | ";
                    first = false;
                    pattern(a);
                }
                break;
            }
        }
    }

    // ------------------------------------------------------------ expressions

    void args(const List<Arg>& as_) {
        out_ += '(';
        bool first = true;
        for (const Arg& a : as_) {
            if (!first) out_ += ", ";
            first = false;
            if (!a.name.empty()) {
                out_ += a.name.name;
                out_ += " = ";
            }
            expr(a.value);
        }
        out_ += ')';
    }

    void escaped(std::string_view text, bool multiline) {
        for (char c : text) {
            switch (c) {
                case '\n': out_ += multiline ? "\n" : "\\n"; break;
                case '\t': out_ += "\\t"; break;
                case '\r': out_ += "\\r"; break;
                case '\0': out_ += "\\0"; break;
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '{': out_ += "{{"; break;
                case '}': out_ += "}}"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        out_ += std::format("\\u{{{:x}}}", static_cast<unsigned>(c));
                    } else {
                        out_ += c;
                    }
            }
        }
    }

    void string_lit(const StringLit* s) {
        // Interpolations print on one line, so they fit any string form.
        auto parts_out = [&](bool multiline) {
            for (const StringPart& p : s->parts) {
                if (p.expr != nullptr) {
                    out_ += '{';
                    ++inline_;
                    expr(p.expr);
                    --inline_;
                    out_ += '}';
                } else if (s->raw) {
                    out_ += p.text;
                } else {
                    escaped(p.text, multiline);
                }
            }
        };
        if (s->multiline && inline_ == 0) {
            // The closing `"""` sets the indentation that the parser strips.
            std::string pad(static_cast<std::size_t>(indent_ + 1) * 4, ' ');
            out_ += s->raw ? "r\"\"\"\n" : "\"\"\"\n";
            std::size_t mark = out_.size();
            parts_out(true);
            std::string body = out_.substr(mark);
            out_.resize(mark);
            std::size_t line_begin = 0;
            for (;;) {
                std::size_t nl = body.find('\n', line_begin);
                std::string_view line = std::string_view(body).substr(
                    line_begin, nl == std::string::npos ? std::string::npos : nl - line_begin);
                if (!line.empty()) out_ += pad;
                out_ += line;
                out_ += '\n';
                if (nl == std::string::npos) break;
                line_begin = nl + 1;
            }
            out_ += pad;
            out_ += "\"\"\"";
            return;
        }
        if (s->raw && !s->multiline) {
            out_ += "r\"";
            parts_out(false);
            out_ += '"';
            return;
        }
        out_ += '"';
        for (const StringPart& p : s->parts) {
            if (p.expr != nullptr) {
                out_ += '{';
                ++inline_;
                expr(p.expr);
                --inline_;
                out_ += '}';
            } else {
                escaped(p.text, false);
            }
        }
        out_ += '"';
    }

    void type_args(const List<TypeExpr*>& ts) {
        if (ts.empty()) return;
        out_ += '<';
        types(ts);
        out_ += '>';
    }

    void expr_inner(const Expr* e, Ctx ctx) {
        switch (e->kind) {
            case ExprKind::Error: out_ += "<error>"; break;
            case ExprKind::Int: {
                auto i = as<IntLit>(e);
                out_ += i->text.empty() ? std::to_string(i->value) + std::string(to_string(i->suffix)) : std::string(i->text);
                break;
            }
            case ExprKind::Float: {
                auto f = as<FloatLit>(e);
                out_ += f->text.empty() ? std::format("{}", f->value) : std::string(f->text);
                break;
            }
            case ExprKind::Bool: out_ += as<BoolLit>(e)->value ? "true" : "false"; break;
            case ExprKind::Null: out_ += "null"; break;
            case ExprKind::String: string_lit(as<StringLit>(e)); break;
            case ExprKind::Name:
                out_ += '#';
                out_ += as<NameLit>(e)->name;
                break;
            case ExprKind::Identifier: {
                auto i = as<IdentExpr>(e);
                out_ += i->name;
                type_args(i->type_args);
                break;
            }
            case ExprKind::Self: out_ += "self"; break;
            case ExprKind::Super: out_ += "super"; break;
            case ExprKind::ListLit: {
                out_ += '[';
                bool first = true;
                for (const Expr* x : as<ListLit>(e)->elements) {
                    if (!first) out_ += ", ";
                    first = false;
                    expr(x);
                }
                out_ += ']';
                break;
            }
            case ExprKind::MapLit: {
                out_ += '{';
                bool first = true;
                for (const MapEntry& m : as<MapLit>(e)->entries) {
                    if (!first) out_ += ", ";
                    first = false;
                    expr(m.key);
                    out_ += ": ";
                    expr(m.value);
                }
                out_ += '}';
                break;
            }
            case ExprKind::StructLit: {
                auto s = as<StructLit>(e);
                type(s->type);
                if (s->fields.empty()) {
                    out_ += " {}";
                    break;
                }
                out_ += " { ";
                bool first = true;
                for (const FieldInit& f : s->fields) {
                    if (!first) out_ += ", ";
                    first = false;
                    out_ += f.name.name;
                    out_ += " = ";
                    expr(f.value);
                }
                out_ += " }";
                break;
            }
            case ExprKind::Unary: {
                auto u = as<UnaryExpr>(e);
                out_ += to_string(u->op);
                expr(u->operand, Prec::Unary, ctx);
                break;
            }
            case ExprKind::Binary: {
                auto b = as<BinaryExpr>(e);
                Prec p = precedence(b->op);
                bool non_assoc = p == Prec::Comparison || p == Prec::Equality;
                bool right = b->op == BinaryOp::Coalesce;
                expr(b->lhs, right || non_assoc ? next_prec(p) : p, ctx);
                out_ += ' ';
                out_ += to_string(b->op);
                out_ += ' ';
                expr(b->rhs, right ? p : next_prec(p), ctx);
                break;
            }
            case ExprKind::Range: {
                auto r = as<RangeExpr>(e);
                expr(r->lo, next_prec(Prec::Range), ctx);
                out_ += r->inclusive ? "..=" : "..";
                expr(r->hi, next_prec(Prec::Range), ctx);
                if (r->step != nullptr) {
                    out_ += " step ";
                    expr(r->step, next_prec(Prec::Range), ctx);
                }
                break;
            }
            case ExprKind::Ternary: {
                auto t = as<TernaryExpr>(e);
                expr(t->cond, next_prec(Prec::Ternary), ctx);
                out_ += " ? ";
                expr(t->then_expr, Prec::Lowest, {.no_struct = ctx.no_struct, .no_lambda = false});
                out_ += " : ";
                expr(t->else_expr, Prec::Ternary, ctx);
                break;
            }
            case ExprKind::Is: {
                auto i = as<IsExpr>(e);
                expr(i->operand, next_prec(Prec::Comparison), ctx);
                out_ += " is ";
                pattern(i->pattern);
                if (!i->binding.empty()) {
                    out_ += ' ';
                    out_ += i->binding.name;
                }
                break;
            }
            case ExprKind::As: {
                auto a = as<AsExpr>(e);
                expr(a->operand, Prec::As, ctx);
                out_ += " as ";
                type(a->type);
                break;
            }
            case ExprKind::Call: {
                auto c = as<CallExpr>(e);
                expr(c->callee, Prec::Postfix, ctx);
                args(c->args);
                break;
            }
            case ExprKind::Index: {
                auto i = as<IndexExpr>(e);
                expr(i->object, Prec::Postfix, ctx);
                out_ += '[';
                expr(i->index);
                out_ += ']';
                break;
            }
            case ExprKind::Member: {
                auto m = as<MemberExpr>(e);
                if (m->safe && m->object->kind == ExprKind::Try) {
                    out_ += '(';
                    expr(m->object);
                    out_ += ')';
                } else {
                    expr(m->object, Prec::Postfix, ctx);
                }
                out_ += m->safe ? "?." : ".";
                out_ += m->member.name;
                type_args(m->type_args);
                break;
            }
            case ExprKind::Try: {
                auto t = as<TryExpr>(e);
                if (t->operand->kind == ExprKind::Try) {
                    out_ += '(';
                    expr(t->operand);
                    out_ += ')';
                } else {
                    expr(t->operand, Prec::Postfix, ctx);
                }
                out_ += '?';
                break;
            }
            case ExprKind::Lambda: {
                auto l = as<LambdaExpr>(e);
                if (l->arrow) {
                    if (l->params.size() == 1) {
                        out_ += l->params[0].name.name;
                    } else {
                        out_ += '(';
                        bool first = true;
                        for (const Param& p : l->params) {
                            if (!first) out_ += ", ";
                            first = false;
                            out_ += p.name.name;
                        }
                        out_ += ')';
                    }
                    out_ += " => ";
                    expr(l->expr_body, Prec::Lowest, ctx);
                    break;
                }
                out_ += "fn";
                params(l->params);
                if (l->ret != nullptr) {
                    out_ += " -> ";
                    type(l->ret);
                }
                out_ += ' ';
                block(l->body);
                break;
            }
            case ExprKind::New: {
                auto n = as<NewExpr>(e);
                out_ += "new(";
                out_ += n->arena.name;
                out_ += ") ";
                type(n->type);
                args(n->args);
                break;
            }
            case ExprKind::Alloc: {
                auto a = as<AllocExpr>(e);
                out_ += "alloc ";
                type(a->type);
                args(a->args);
                break;
            }
            case ExprKind::Await:
                out_ += "await ";
                expr(as<AwaitExpr>(e)->operand, Prec::Unary, ctx);
                break;
            case ExprKind::Start:
                out_ += "start(";
                expr(as<StartExpr>(e)->operand);
                out_ += ')';
                break;
            case ExprKind::Free:
                out_ += "free(";
                expr(as<FreeExpr>(e)->operand);
                out_ += ')';
                break;
        }
    }

    std::string out_;
    int indent_ = 0;
    int inline_ = 0; // > 0 inside a string interpolation: print blocks on one line
};

} // namespace

std::string print(const Module& module) {
    Printer p;
    p.module(module);
    return p.take();
}

std::string print(const Script& script) {
    Printer p;
    p.script(script);
    return p.take();
}

std::string print(const Expr* expr) {
    Printer p;
    p.expr(expr);
    return p.take();
}

} // namespace pluma

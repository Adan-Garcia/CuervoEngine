#include "pluma/ast_dump.hpp"

#include <format>
#include <utility>
#include <vector>

namespace pluma {

namespace {

// A dump node: a bare atom when `kids` is empty and `list` is false.
struct SNode {
    std::string head;
    std::vector<SNode> kids;
    bool list = false;

    SNode() = default;
    explicit SNode(std::string atom) : head(std::move(atom)) {}
    SNode(std::string h, std::vector<SNode> k) : head(std::move(h)), kids(std::move(k)), list(true) {}

    SNode& add(SNode kid) {
        kids.push_back(std::move(kid));
        list = true;
        return *this;
    }
};

SNode tag(std::string head) {
    SNode n(std::move(head));
    n.list = true;
    return n;
}

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\0': out += "\\0"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
        }
    }
    return out + '"';
}

void render_compact(const SNode& n, std::string& out) {
    if (!n.list) {
        out += n.head;
        return;
    }
    out += '(';
    out += n.head;
    for (const SNode& k : n.kids) {
        out += ' ';
        render_compact(k, out);
    }
    out += ')';
}

void render_pretty(const SNode& n, std::string& out, int indent) {
    std::string compact;
    render_compact(n, compact);
    if (!n.list || compact.size() + static_cast<std::size_t>(indent) <= 80) {
        out += compact;
        return;
    }
    out += '(';
    out += n.head;
    for (const SNode& k : n.kids) {
        out += '\n';
        out.append(static_cast<std::size_t>(indent + 2), ' ');
        render_pretty(k, out, indent + 2);
    }
    out += ')';
}

std::string render(const SNode& n, bool pretty) {
    std::string out;
    if (pretty) {
        render_pretty(n, out, 0);
    } else {
        render_compact(n, out);
    }
    return out;
}

SNode of(const Expr* e);
SNode of(const Stmt* s);
SNode of(const Decl* d);
SNode of(const TypeExpr* t);
SNode of(const Pattern* p);

SNode atom(std::string_view s) { return SNode(std::string(s)); }

SNode type_list(std::string head, const List<TypeExpr*>& types) {
    SNode n = tag(std::move(head));
    for (const TypeExpr* t : types) n.add(of(t));
    return n;
}

SNode of(const TypeExpr* t) {
    if (t == nullptr) return atom("<null>");
    switch (t->kind) {
        case TypeKind::Error: return atom("<error>");
        case TypeKind::Named: {
            auto n = as<NamedType>(t);
            if (n->args.empty()) return atom(n->name.name);
            SNode out = tag(std::string(n->name.name));
            for (const TypeExpr* a : n->args) out.add(of(a));
            return out;
        }
        case TypeKind::Array: {
            auto a = as<ArrayType>(t);
            return SNode("array", {of(a->element), of(a->size)});
        }
        case TypeKind::Fn: {
            auto f = as<FnType>(t);
            SNode out("fn", {type_list("params", f->params)});
            if (f->ret != nullptr) out.add(of(f->ret));
            return out;
        }
        case TypeKind::Nullable: return SNode("?", {of(as<NullableType>(t)->inner)});
    }
    return atom("<?>");
}

SNode args_of(SNode n, const List<Arg>& args) {
    for (const Arg& a : args) {
        if (a.name.empty()) {
            n.add(of(a.value));
        } else {
            n.add(SNode("named", {atom(a.name.name), of(a.value)}));
        }
    }
    return n;
}

SNode params_of(const List<Param>& params) {
    SNode n = tag("params");
    for (const Param& p : params) {
        SNode pn = tag(std::string(p.name.name));
        if (p.type != nullptr) pn.add(of(p.type));
        if (p.default_value != nullptr) pn.add(SNode("=", {of(p.default_value)}));
        if (pn.kids.empty()) {
            n.add(atom(p.name.name));
        } else {
            n.add(pn);
        }
    }
    return n;
}

std::string suffix_str(NumSuffix s) { return std::string(to_string(s)); }

SNode of(const Expr* e) {
    if (e == nullptr) return atom("<null>");
    switch (e->kind) {
        case ExprKind::Error: return atom("<error>");
        case ExprKind::Int: {
            auto i = as<IntLit>(e);
            if (i->suffix == NumSuffix::None) return atom(std::to_string(i->value));
            return SNode("int", {atom(std::to_string(i->value)), atom(suffix_str(i->suffix))});
        }
        case ExprKind::Float: {
            auto f = as<FloatLit>(e);
            SNode n("float", {atom(std::format("{}", f->value))});
            if (f->suffix != NumSuffix::None) n.add(atom(suffix_str(f->suffix)));
            return n;
        }
        case ExprKind::Bool: return atom(as<BoolLit>(e)->value ? "true" : "false");
        case ExprKind::Null: return atom("null");
        case ExprKind::String: {
            auto s = as<StringLit>(e);
            SNode n = tag("str");
            if (s->raw) n.add(atom("raw"));
            if (s->multiline) n.add(atom("multiline"));
            for (const StringPart& p : s->parts) {
                if (p.expr != nullptr) {
                    n.add(SNode("{}", {of(p.expr)}));
                } else {
                    n.add(atom(quote(p.text)));
                }
            }
            return n;
        }
        case ExprKind::Name: return atom(std::format("#{}", as<NameLit>(e)->name));
        case ExprKind::Identifier: {
            auto i = as<IdentExpr>(e);
            if (i->type_args.empty()) return atom(i->name);
            return SNode("generic", {atom(i->name), type_list("types", i->type_args)});
        }
        case ExprKind::Self: return atom("self");
        case ExprKind::Super: return atom("super");
        case ExprKind::ListLit: {
            SNode n = tag("list");
            for (const Expr* x : as<ListLit>(e)->elements) n.add(of(x));
            return n;
        }
        case ExprKind::MapLit: {
            SNode n = tag("map");
            for (const MapEntry& m : as<MapLit>(e)->entries) n.add(SNode("entry", {of(m.key), of(m.value)}));
            return n;
        }
        case ExprKind::StructLit: {
            auto s = as<StructLit>(e);
            SNode n("struct", {of(s->type)});
            for (const FieldInit& f : s->fields) n.add(SNode(std::string(f.name.name), {of(f.value)}));
            return n;
        }
        case ExprKind::Unary: {
            auto u = as<UnaryExpr>(e);
            std::string_view name = u->op == UnaryOp::Neg ? "neg" : u->op == UnaryOp::Not ? "not" : "bitnot";
            return SNode(std::string(name), {of(u->operand)});
        }
        case ExprKind::Binary: {
            auto b = as<BinaryExpr>(e);
            return SNode(std::string(to_string(b->op)), {of(b->lhs), of(b->rhs)});
        }
        case ExprKind::Range: {
            auto r = as<RangeExpr>(e);
            SNode n(r->inclusive ? "..=" : "..", {of(r->lo), of(r->hi)});
            if (r->step != nullptr) n.add(SNode("step", {of(r->step)}));
            return n;
        }
        case ExprKind::Ternary: {
            auto t = as<TernaryExpr>(e);
            return SNode("?:", {of(t->cond), of(t->then_expr), of(t->else_expr)});
        }
        case ExprKind::Is: {
            auto i = as<IsExpr>(e);
            SNode n("is", {of(i->operand), of(i->pattern)});
            if (!i->binding.empty()) n.add(SNode("bind", {atom(i->binding.name)}));
            return n;
        }
        case ExprKind::As: {
            auto a = as<AsExpr>(e);
            return SNode("as", {of(a->operand), of(a->type)});
        }
        case ExprKind::Call: {
            auto c = as<CallExpr>(e);
            return args_of(SNode("call", {of(c->callee)}), c->args);
        }
        case ExprKind::Index: {
            auto i = as<IndexExpr>(e);
            return SNode("index", {of(i->object), of(i->index)});
        }
        case ExprKind::Member: {
            auto m = as<MemberExpr>(e);
            SNode n(m->safe ? "?." : ".", {of(m->object), atom(m->member.name)});
            if (!m->type_args.empty()) n.add(type_list("types", m->type_args));
            return n;
        }
        case ExprKind::Try: return SNode("try", {of(as<TryExpr>(e)->operand)});
        case ExprKind::Lambda: {
            auto l = as<LambdaExpr>(e);
            if (l->arrow) {
                SNode ps = tag("params");
                for (const Param& p : l->params) ps.add(atom(p.name.name));
                return SNode("=>", {ps, of(l->expr_body)});
            }
            SNode n("lambda", {params_of(l->params)});
            if (l->ret != nullptr) n.add(SNode("ret", {of(l->ret)}));
            n.add(of(l->body));
            return n;
        }
        case ExprKind::New: {
            auto n = as<NewExpr>(e);
            return args_of(SNode("new", {atom(n->arena.name), of(n->type)}), n->args);
        }
        case ExprKind::Alloc: {
            auto a = as<AllocExpr>(e);
            return args_of(SNode("alloc", {of(a->type)}), a->args);
        }
        case ExprKind::Await: return SNode("await", {of(as<AwaitExpr>(e)->operand)});
        case ExprKind::Start: return SNode("start", {of(as<StartExpr>(e)->operand)});
        case ExprKind::Free: return SNode("free", {of(as<FreeExpr>(e)->operand)});
    }
    return atom("<?>");
}

SNode of(const Pattern* p) {
    if (p == nullptr) return atom("<null>");
    switch (p->kind) {
        case PatternKind::Error: return atom("<error>");
        case PatternKind::Wildcard: return atom("_");
        case PatternKind::Literal: return SNode("lit", {of(as<LiteralPattern>(p)->value)});
        case PatternKind::Range: {
            auto r = as<RangePattern>(p);
            return SNode(r->inclusive ? "range=" : "range", {of(r->lo), of(r->hi)});
        }
        case PatternKind::Path: {
            auto pp = as<PathPattern>(p);
            std::string path;
            for (const Ident& id : pp->path) {
                if (!path.empty()) path += '.';
                path += id.name;
            }
            if (!pp->has_args) return SNode(path);
            SNode n = tag(path);
            for (const Pattern* a : pp->args) n.add(of(a));
            return n;
        }
        case PatternKind::Or: {
            SNode n = tag("|");
            for (const Pattern* a : as<OrPattern>(p)->alternatives) n.add(of(a));
            return n;
        }
    }
    return atom("<?>");
}

SNode label_of(SNode n, const Ident& label) {
    if (!label.empty()) n.add(SNode("label", {atom(label.name)}));
    return n;
}

SNode attrs_of(const List<Attribute>& attrs) {
    SNode n = tag("attrs");
    for (const Attribute& a : attrs) {
        std::string name = "@" + std::string(a.name.name);
        // `()` marks an argument list, so `@export` and `@export()` differ.
        n.add(a.has_args ? args_of(SNode(name, {atom("()")}), a.args) : atom(name));
    }
    return n;
}

SNode of(const Stmt* s) {
    if (s == nullptr) return atom("<null>");
    switch (s->kind) {
        case StmtKind::Error: return atom("<error>");
        case StmtKind::Expr: return of(as<ExprStmt>(s)->expr);
        case StmtKind::Decl: return of(as<DeclStmt>(s)->decl);
        case StmtKind::Assign: {
            auto a = as<AssignStmt>(s);
            return SNode(std::string(to_string(a->op)), {of(a->target), of(a->value)});
        }
        case StmtKind::Block: {
            SNode n = tag("block");
            for (const Stmt* x : as<BlockStmt>(s)->stmts) n.add(of(x));
            return n;
        }
        case StmtKind::If: {
            auto i = as<IfStmt>(s);
            SNode n = tag("if");
            for (const IfBranch& b : i->branches) {
                SNode bn("branch", {of(b.cond)});
                if (!b.unwrap.empty()) bn.add(SNode("unwrap", {atom(b.unwrap.name)}));
                bn.add(of(b.body));
                n.add(bn);
            }
            if (i->else_body != nullptr) n.add(SNode("else", {of(i->else_body)}));
            return n;
        }
        case StmtKind::Match: {
            auto m = as<MatchStmt>(s);
            SNode n("match", {of(m->subject)});
            for (const MatchArm& a : m->arms) {
                SNode an("arm", {of(a.pattern)});
                if (a.guard != nullptr) an.add(SNode("if", {of(a.guard)}));
                an.add(of(a.body));
                n.add(an);
            }
            return n;
        }
        case StmtKind::For: {
            auto f = as<ForStmt>(s);
            SNode n = label_of(tag("for"), f->label);
            n.add(atom(f->first.name));
            if (!f->second.empty()) n.add(atom(f->second.name));
            n.add(of(f->iterable));
            n.add(of(f->body));
            return n;
        }
        case StmtKind::Foreach: {
            auto f = as<ForeachStmt>(s);
            SNode n = tag("foreach");
            if (!f->attributes.empty()) n.add(attrs_of(f->attributes));
            n.add(atom(f->var.name));
            n.add(of(f->pool));
            n.add(of(f->body));
            return n;
        }
        case StmtKind::While: {
            auto w = as<WhileStmt>(s);
            SNode n = label_of(tag("while"), w->label);
            n.add(of(w->cond));
            n.add(of(w->body));
            return n;
        }
        case StmtKind::Loop: {
            auto l = as<LoopStmt>(s);
            SNode n = label_of(tag("loop"), l->label);
            n.add(of(l->body));
            return n;
        }
        case StmtKind::Return: {
            auto r = as<ReturnStmt>(s);
            if (r->value == nullptr) return tag("return");
            return SNode("return", {of(r->value)});
        }
        case StmtKind::Break: {
            auto b = as<BreakStmt>(s);
            return b->label.empty() ? tag("break") : SNode("break", {atom(b->label.name)});
        }
        case StmtKind::Continue: {
            auto c = as<ContinueStmt>(s);
            return c->label.empty() ? tag("continue") : SNode("continue", {atom(c->label.name)});
        }
        case StmtKind::Defer: return SNode("defer", {of(as<DeferStmt>(s)->body)});
        case StmtKind::Unsafe: return SNode("unsafe", {of(as<UnsafeStmt>(s)->body)});
        case StmtKind::Yield: return tag("yield");
        case StmtKind::Emit: {
            auto e = as<EmitStmt>(s);
            if (!e->has_args) return SNode("emit", {atom(e->signal.name)});
            return args_of(SNode("emit", {atom(e->signal.name), atom("()")}), e->args);
        }
    }
    return atom("<?>");
}

SNode generics_of(const List<GenericParam>& generics) {
    SNode n = tag("generics");
    for (const GenericParam& g : generics) {
        if (g.bounds.empty()) {
            n.add(atom(g.name.name));
        } else {
            n.add(type_list(std::string(g.name.name), g.bounds));
        }
    }
    return n;
}

SNode members_of(SNode n, const List<Decl*>& members) {
    for (const Decl* m : members) n.add(of(m));
    return n;
}

SNode of(const Decl* d) {
    if (d == nullptr) return atom("<null>");
    SNode n;
    switch (d->kind) {
        case DeclKind::Error: return atom("<error>");
        case DeclKind::Var: {
            auto v = as<VarDecl>(d);
            n = SNode("var", {atom(v->name.name)});
            if (v->weak) n.add(atom("weak"));
            if (v->type != nullptr) n.add(SNode("type", {of(v->type)}));
            if (v->init != nullptr) n.add(SNode("init", {of(v->init)}));
            if (v->has_accessors) {
                SNode acc = tag("accessors");
                if (v->getter != nullptr) acc.add(SNode("get", {of(v->getter)}));
                if (v->setter != nullptr) acc.add(SNode("set", {atom(v->setter_param.name), of(v->setter)}));
                n.add(acc);
            }
            break;
        }
        case DeclKind::Const: {
            auto c = as<ConstDecl>(d);
            n = SNode("const", {atom(c->name.name)});
            if (c->type != nullptr) n.add(SNode("type", {of(c->type)}));
            n.add(of(c->value));
            break;
        }
        case DeclKind::Fn: {
            auto f = as<FnDecl>(d);
            n = SNode("fn", {atom(f->name.name)});
            if (!f->generics.empty()) n.add(generics_of(f->generics));
            n.add(params_of(f->params));
            if (f->ret != nullptr) n.add(SNode("ret", {of(f->ret)}));
            if (f->body != nullptr) n.add(of(f->body));
            break;
        }
        case DeclKind::Class: {
            auto c = as<ClassDecl>(d);
            n = SNode("class", {atom(c->name.name)});
            if (c->manual) n.add(atom("manual"));
            if (!c->generics.empty()) n.add(generics_of(c->generics));
            if (c->extends != nullptr) n.add(SNode("extends", {of(c->extends)}));
            if (!c->traits.empty()) n.add(type_list("traits", c->traits));
            n = members_of(n, c->members);
            break;
        }
        case DeclKind::Struct: {
            auto s = as<StructDecl>(d);
            n = SNode("struct", {atom(s->name.name)});
            if (!s->generics.empty()) n.add(generics_of(s->generics));
            if (!s->traits.empty()) n.add(type_list("traits", s->traits));
            n = members_of(n, s->members);
            break;
        }
        case DeclKind::Enum: {
            auto e = as<EnumDecl>(d);
            n = SNode("enum", {atom(e->name.name)});
            if (e->backing != nullptr) n.add(SNode("backing", {of(e->backing)}));
            for (const EnumVariant& v : e->variants) {
                SNode vn = tag(std::string(v.name.name));
                if (v.has_params) vn.add(params_of(v.params));
                if (v.value != nullptr) vn.add(SNode("=", {of(v.value)}));
                n.add(vn.kids.empty() ? atom(v.name.name) : vn);
            }
            break;
        }
        case DeclKind::Trait: {
            auto t = as<TraitDecl>(d);
            n = SNode("trait", {atom(t->name.name)});
            if (!t->generics.empty()) n.add(generics_of(t->generics));
            if (!t->supertraits.empty()) n.add(type_list("traits", t->supertraits));
            n = members_of(n, t->members);
            break;
        }
        case DeclKind::Signal: {
            auto s = as<SignalDecl>(d);
            n = SNode("signal", {atom(s->name.name)});
            if (s->has_params) n.add(params_of(s->params));
            break;
        }
    }
    // Modifiers and attributes go right after the name.
    std::vector<SNode> extra;
    if (d->is_pub) extra.push_back(atom("pub"));
    if (d->is_static) extra.push_back(atom("static"));
    if (d->is_final) extra.push_back(atom("final"));
    if (!d->attributes.empty()) extra.push_back(attrs_of(d->attributes));
    n.kids.insert(n.kids.begin() + 1, extra.begin(), extra.end());
    return n;
}

} // namespace

std::string dump(const Module& module, bool pretty) {
    SNode n = tag("module");
    for (const Decl* d : module.decls) n.add(of(d));
    return render(n, pretty);
}

std::string dump(const Script& script, bool pretty) {
    SNode n = tag("script");
    for (const Stmt* s : script.stmts) n.add(of(s));
    return render(n, pretty);
}

std::string dump(const Expr* expr, bool pretty) { return render(of(expr), pretty); }
std::string dump(const Stmt* stmt, bool pretty) { return render(of(stmt), pretty); }
std::string dump(const Decl* decl, bool pretty) { return render(of(decl), pretty); }

} // namespace pluma

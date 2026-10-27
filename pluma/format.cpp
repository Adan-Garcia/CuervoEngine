#include "pluma/format.hpp"

#include <algorithm>
#include <cctype>
#include <format>

#include "pluma/arena.hpp"
#include "pluma/parser.hpp"

namespace pluma {

namespace {

Prec next_prec(Prec p) { return static_cast<Prec>(static_cast<int>(p) + 1); }

struct Ctx {
    bool no_struct = false;
    bool no_lambda = false;
};

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
    if (d == nullptr) return false;
    if (auto f = as<FnDecl>(d)) return f->body != nullptr;
    if (auto v = as<VarDecl>(d)) return v->has_accessors;
    return d->kind == DeclKind::Class || d->kind == DeclKind::Struct || d->kind == DeclKind::Enum ||
           d->kind == DeclKind::Trait;
}

// ------------------------------------------------------------ comment scanner

struct Comment {
    Span span;
    std::string_view text;
    std::uint32_t start_line = 1;
    std::uint32_t end_line = 1;
    bool is_block = false;
    // Preceded on its starting line by non-whitespace code/tokens.
    bool trailing = false;
    // Preceded by at least one completely blank line.
    bool blank_before = false;
};

// Extracts all `//` and `/* ... */` comments in source order, skipping string
// literals (while still scanning inside `{...}` string interpolations).
std::vector<Comment> extract_comments(const SourceFile& source) {
    std::vector<Comment> out;
    std::string_view s = source.text();
    std::size_t n = s.size();
    std::size_t i = 0;
    bool line_has_code = false;
    std::uint32_t consecutive_newlines = 0;

    auto scan_self = [&](auto& self, std::size_t end_pos, bool stop_at_rbrace) -> void {
        int brace_depth = 0;
        while (i < end_pos) {
            char c = s[i];
            if (c == '\r') {
                ++i;
                continue;
            }
            if (c == '\n') {
                ++i;
                line_has_code = false;
                ++consecutive_newlines;
                continue;
            }
            if (c == ' ' || c == '\t') {
                ++i;
                continue;
            }
            if (stop_at_rbrace && c == '}') {
                if (brace_depth == 0) return;
                --brace_depth;
                line_has_code = true;
                consecutive_newlines = 0;
                ++i;
                continue;
            }
            if (stop_at_rbrace && c == '{') {
                ++brace_depth;
                line_has_code = true;
                consecutive_newlines = 0;
                ++i;
                continue;
            }
            // Line comment `// ...`
            if (c == '/' && i + 1 < end_pos && s[i + 1] == '/') {
                std::size_t start = i;
                i += 2;
                while (i < end_pos && s[i] != '\n') ++i;
                std::size_t text_end = i;
                if (text_end > start && s[text_end - 1] == '\r') --text_end;
                std::uint32_t ln = source.line_col(static_cast<std::uint32_t>(start)).line;
                out.push_back(Comment{
                    .span = {static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(text_end)},
                    .text = s.substr(start, text_end - start),
                    .start_line = ln,
                    .end_line = ln,
                    .is_block = false,
                    .trailing = line_has_code,
                    .blank_before = !line_has_code && consecutive_newlines >= 2,
                });
                line_has_code = false;
                consecutive_newlines = 0;
                continue;
            }
            // Block comment `/* ... */` (nestable)
            if (c == '/' && i + 1 < end_pos && s[i + 1] == '*') {
                std::size_t start = i;
                bool was_trailing = line_has_code;
                bool had_blank = !line_has_code && consecutive_newlines >= 2;
                i += 2;
                int depth = 1;
                while (i < end_pos && depth > 0) {
                    if (s[i] == '/' && i + 1 < end_pos && s[i + 1] == '*') {
                        ++depth;
                        i += 2;
                    } else if (s[i] == '*' && i + 1 < end_pos && s[i + 1] == '/') {
                        --depth;
                        i += 2;
                    } else {
                        ++i;
                    }
                }
                std::uint32_t sl = source.line_col(static_cast<std::uint32_t>(start)).line;
                std::uint32_t el = source.line_col(static_cast<std::uint32_t>(i > start ? i - 1 : start)).line;
                out.push_back(Comment{
                    .span = {static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(i)},
                    .text = s.substr(start, i - start),
                    .start_line = sl,
                    .end_line = el,
                    .is_block = true,
                    .trailing = was_trailing,
                    .blank_before = had_blank,
                });
                consecutive_newlines = 0;
                continue;
            }
            // String literal (`"..."`, `r"..."`, `"""..."""`, `r"""..."""`)
            bool raw = (c == 'r' && i + 1 < end_pos && s[i + 1] == '"');
            if (raw || c == '"') {
                line_has_code = true;
                consecutive_newlines = 0;
                if (raw) ++i;
                bool multiline = (i + 2 < end_pos && s[i + 1] == '"' && s[i + 2] == '"');
                i += multiline ? 3 : 1;
                while (i < end_pos) {
                    if (multiline) {
                        if (i + 2 < end_pos && s[i] == '"' && s[i + 1] == '"' && s[i + 2] == '"') {
                            i += 3;
                            break;
                        }
                    } else if (s[i] == '"' || s[i] == '\n') {
                        if (s[i] == '"') ++i;
                        break;
                    }
                    if (!raw && s[i] == '\\' && i + 1 < end_pos) {
                        i += 2;
                        continue;
                    }
                    if (!raw && s[i] == '{') {
                        if (i + 1 < end_pos && s[i + 1] == '{') {
                            i += 2;
                            continue;
                        }
                        ++i;
                        self(self, end_pos, true);
                        if (i < end_pos && s[i] == '}') ++i;
                        continue;
                    }
                    ++i;
                }
                continue;
            }
            line_has_code = true;
            consecutive_newlines = 0;
            ++i;
        }
    };

    scan_self(scan_self, n, false);
    return out;
}

// ------------------------------------------------------------ AST error check

bool has_type_error(const TypeExpr* t) {
    if (t == nullptr) return true;
    switch (t->kind) {
        case TypeKind::Error: return true;
        case TypeKind::Named:
            for (const TypeExpr* a : as<NamedType>(t)->args) {
                if (has_type_error(a)) return true;
            }
            return false;
        case TypeKind::Array: return has_type_error(as<ArrayType>(t)->element);
        case TypeKind::Fn: {
            auto f = as<FnType>(t);
            for (const TypeExpr* p : f->params) {
                if (has_type_error(p)) return true;
            }
            return f->ret != nullptr && has_type_error(f->ret);
        }
        case TypeKind::Nullable: return has_type_error(as<NullableType>(t)->inner);
    }
    return false;
}

bool has_expr_error(const Expr* e);
bool has_stmt_error(const Stmt* s);
bool has_decl_error(const Decl* d);

bool has_pattern_error(const Pattern* p) {
    if (p == nullptr) return true;
    switch (p->kind) {
        case PatternKind::Error: return true;
        case PatternKind::Wildcard: return false;
        case PatternKind::Literal: return has_expr_error(as<LiteralPattern>(p)->value);
        case PatternKind::Range: {
            auto r = as<RangePattern>(p);
            return has_expr_error(r->lo) || has_expr_error(r->hi);
        }
        case PatternKind::Path:
            for (const Pattern* a : as<PathPattern>(p)->args) {
                if (has_pattern_error(a)) return true;
            }
            return false;
        case PatternKind::Or:
            for (const Pattern* a : as<OrPattern>(p)->alternatives) {
                if (has_pattern_error(a)) return true;
            }
            return false;
    }
    return false;
}

bool has_expr_error(const Expr* e) {
    if (e == nullptr) return true;
    switch (e->kind) {
        case ExprKind::Error: return true;
        case ExprKind::Int:
        case ExprKind::Float:
        case ExprKind::Bool:
        case ExprKind::Null:
        case ExprKind::Name:
        case ExprKind::Self:
        case ExprKind::Super: return false;
        case ExprKind::String:
            for (const StringPart& p : as<StringLit>(e)->parts) {
                if (p.expr != nullptr && has_expr_error(p.expr)) return true;
            }
            return false;
        case ExprKind::Identifier:
            for (const TypeExpr* t : as<IdentExpr>(e)->type_args) {
                if (has_type_error(t)) return true;
            }
            return false;
        case ExprKind::ListLit:
            for (const Expr* x : as<ListLit>(e)->elements) {
                if (has_expr_error(x)) return true;
            }
            return false;
        case ExprKind::MapLit:
            for (const MapEntry& m : as<MapLit>(e)->entries) {
                if (has_expr_error(m.key) || has_expr_error(m.value)) return true;
            }
            return false;
        case ExprKind::StructLit: {
            auto s = as<StructLit>(e);
            if (has_type_error(s->type)) return true;
            for (const FieldInit& f : s->fields) {
                if (has_expr_error(f.value)) return true;
            }
            return false;
        }
        case ExprKind::Unary: return has_expr_error(as<UnaryExpr>(e)->operand);
        case ExprKind::Binary: {
            auto b = as<BinaryExpr>(e);
            return has_expr_error(b->lhs) || has_expr_error(b->rhs);
        }
        case ExprKind::Range: {
            auto r = as<RangeExpr>(e);
            return has_expr_error(r->lo) || has_expr_error(r->hi) || (r->step != nullptr && has_expr_error(r->step));
        }
        case ExprKind::Ternary: {
            auto t = as<TernaryExpr>(e);
            return has_expr_error(t->cond) || has_expr_error(t->then_expr) || has_expr_error(t->else_expr);
        }
        case ExprKind::Is: {
            auto i = as<IsExpr>(e);
            return has_expr_error(i->operand) || has_pattern_error(i->pattern);
        }
        case ExprKind::As: {
            auto a = as<AsExpr>(e);
            return has_expr_error(a->operand) || has_type_error(a->type);
        }
        case ExprKind::Call: {
            auto c = as<CallExpr>(e);
            if (has_expr_error(c->callee)) return true;
            for (const Arg& a : c->args) {
                if (has_expr_error(a.value)) return true;
            }
            return false;
        }
        case ExprKind::Index: {
            auto i = as<IndexExpr>(e);
            return has_expr_error(i->object) || has_expr_error(i->index);
        }
        case ExprKind::Member: {
            auto m = as<MemberExpr>(e);
            if (has_expr_error(m->object)) return true;
            for (const TypeExpr* t : m->type_args) {
                if (has_type_error(t)) return true;
            }
            return false;
        }
        case ExprKind::Try: return has_expr_error(as<TryExpr>(e)->operand);
        case ExprKind::Lambda: {
            auto l = as<LambdaExpr>(e);
            for (const Param& p : l->params) {
                if ((p.type != nullptr && has_type_error(p.type)) ||
                    (p.default_value != nullptr && has_expr_error(p.default_value))) {
                    return true;
                }
            }
            if (l->ret != nullptr && has_type_error(l->ret)) return true;
            return l->arrow ? has_expr_error(l->expr_body) : has_stmt_error(l->body);
        }
        case ExprKind::New: {
            auto n = as<NewExpr>(e);
            if (has_type_error(n->type)) return true;
            for (const Arg& a : n->args) {
                if (has_expr_error(a.value)) return true;
            }
            return false;
        }
        case ExprKind::Alloc: {
            auto a = as<AllocExpr>(e);
            if (has_type_error(a->type)) return true;
            for (const Arg& arg : a->args) {
                if (has_expr_error(arg.value)) return true;
            }
            return false;
        }
        case ExprKind::Await: return has_expr_error(as<AwaitExpr>(e)->operand);
        case ExprKind::Start: return has_expr_error(as<StartExpr>(e)->operand);
        case ExprKind::Free: return has_expr_error(as<FreeExpr>(e)->operand);
    }
    return false;
}

bool has_stmt_error(const Stmt* s) {
    if (s == nullptr) return true;
    switch (s->kind) {
        case StmtKind::Error: return true;
        case StmtKind::Expr: return has_expr_error(as<ExprStmt>(s)->expr);
        case StmtKind::Decl: return has_decl_error(as<DeclStmt>(s)->decl);
        case StmtKind::Assign: {
            auto a = as<AssignStmt>(s);
            return has_expr_error(a->target) || has_expr_error(a->value);
        }
        case StmtKind::Block:
            for (const Stmt* child : as<BlockStmt>(s)->stmts) {
                if (has_stmt_error(child)) return true;
            }
            return false;
        case StmtKind::If: {
            auto i = as<IfStmt>(s);
            for (const IfBranch& b : i->branches) {
                if (has_expr_error(b.cond) || has_stmt_error(b.body)) return true;
            }
            return i->else_body != nullptr && has_stmt_error(i->else_body);
        }
        case StmtKind::Match: {
            auto m = as<MatchStmt>(s);
            if (has_expr_error(m->subject)) return true;
            for (const MatchArm& a : m->arms) {
                if (has_pattern_error(a.pattern) || (a.guard != nullptr && has_expr_error(a.guard)) ||
                    has_stmt_error(a.body)) {
                    return true;
                }
            }
            return false;
        }
        case StmtKind::For: {
            auto f = as<ForStmt>(s);
            return has_expr_error(f->iterable) || has_stmt_error(f->body);
        }
        case StmtKind::Foreach: {
            auto f = as<ForeachStmt>(s);
            return has_expr_error(f->pool) || has_stmt_error(f->body);
        }
        case StmtKind::While: {
            auto w = as<WhileStmt>(s);
            return has_expr_error(w->cond) || has_stmt_error(w->body);
        }
        case StmtKind::Loop: return has_stmt_error(as<LoopStmt>(s)->body);
        case StmtKind::Return: {
            auto r = as<ReturnStmt>(s);
            return r->value != nullptr && has_expr_error(r->value);
        }
        case StmtKind::Break:
        case StmtKind::Continue:
        case StmtKind::Yield: return false;
        case StmtKind::Defer: return has_stmt_error(as<DeferStmt>(s)->body);
        case StmtKind::Unsafe: return has_stmt_error(as<UnsafeStmt>(s)->body);
        case StmtKind::Emit:
            for (const Arg& a : as<EmitStmt>(s)->args) {
                if (has_expr_error(a.value)) return true;
            }
            return false;
    }
    return false;
}

bool has_decl_error(const Decl* d) {
    if (d == nullptr) return true;
    for (const Attribute& a : d->attributes) {
        for (const Arg& arg : a.args) {
            if (has_expr_error(arg.value)) return true;
        }
    }
    switch (d->kind) {
        case DeclKind::Error: return true;
        case DeclKind::Var: {
            auto v = as<VarDecl>(d);
            if ((v->type != nullptr && has_type_error(v->type)) || (v->init != nullptr && has_expr_error(v->init))) {
                return true;
            }
            if (v->has_accessors) {
                if ((v->getter != nullptr && has_stmt_error(v->getter)) ||
                    (v->setter != nullptr && has_stmt_error(v->setter))) {
                    return true;
                }
            }
            return false;
        }
        case DeclKind::Const: {
            auto c = as<ConstDecl>(d);
            return (c->type != nullptr && has_type_error(c->type)) || has_expr_error(c->value);
        }
        case DeclKind::Fn: {
            auto f = as<FnDecl>(d);
            for (const Param& p : f->params) {
                if ((p.type != nullptr && has_type_error(p.type)) ||
                    (p.default_value != nullptr && has_expr_error(p.default_value))) {
                    return true;
                }
            }
            if (f->ret != nullptr && has_type_error(f->ret)) return true;
            return f->body != nullptr && has_stmt_error(f->body);
        }
        case DeclKind::Class: {
            auto c = as<ClassDecl>(d);
            if (c->extends != nullptr && has_type_error(c->extends)) return true;
            for (const TypeExpr* t : c->traits) {
                if (has_type_error(t)) return true;
            }
            for (const Decl* m : c->members) {
                if (has_decl_error(m)) return true;
            }
            return false;
        }
        case DeclKind::Struct: {
            auto s = as<StructDecl>(d);
            for (const TypeExpr* t : s->traits) {
                if (has_type_error(t)) return true;
            }
            for (const Decl* m : s->members) {
                if (has_decl_error(m)) return true;
            }
            return false;
        }
        case DeclKind::Trait: {
            auto t = as<TraitDecl>(d);
            for (const TypeExpr* st : t->supertraits) {
                if (has_type_error(st)) return true;
            }
            for (const Decl* m : t->members) {
                if (has_decl_error(m)) return true;
            }
            return false;
        }
        case DeclKind::Enum: {
            auto e = as<EnumDecl>(d);
            if (e->backing != nullptr && has_type_error(e->backing)) return true;
            for (const EnumVariant& v : e->variants) {
                for (const Param& p : v.params) {
                    if (p.type != nullptr && has_type_error(p.type)) return true;
                }
                if (v.value != nullptr && has_expr_error(v.value)) return true;
            }
            return false;
        }
        case DeclKind::Signal: {
            auto s = as<SignalDecl>(d);
            for (const Param& p : s->params) {
                if (p.type != nullptr && has_type_error(p.type)) return true;
            }
            return false;
        }
    }
    return false;
}

// Errors that the parser recovers from into a complete AST and that the
// formatter normalizes directly (LANGUAGE.md 1.6, 2.7).
bool is_normalizable_parse_error(const Diagnostic& d) {
    return d.message.starts_with("ternary '?' needs spaces around it") ||
           d.message.starts_with("enum variants must be separated by ','") ||
           d.message.starts_with("expected ',' between enum variants");
}

// ------------------------------------------------------------ Formatter

class Formatter {
public:
    Formatter(const SourceFile& src, std::vector<Comment> comments)
        : src_(src), comments_(std::move(comments)) {}

    std::string take() { return std::move(out_); }

    void module(const Module& m) {
        const Decl* prev = nullptr;
        std::size_t next_import = 0;
        // Imports print at their place among the declarations, a blank line
        // after the last one of a run (LANGUAGE.md 9.26).
        auto imports_before = [&](std::uint32_t limit) {
            bool any = false;
            while (next_import < m.imports.size() && m.imports[next_import]->span.begin < limit) {
                const ImportDecl* im = m.imports[next_import++];
                if (!any && (prev != nullptr || !out_.empty()) && !ended_with_blank_line()) out_ += '\n';
                emit_comments_before(im->span.begin, prev == nullptr && !any && out_.empty());
                import_decl(*im);
                any = true;
            }
            if (any && limit != UINT32_MAX) out_ += '\n';
            return any;
        };
        for (const Decl* d : m.decls) {
            std::uint32_t anchor = decl_start(d);
            if (imports_before(anchor)) prev = nullptr;
            // The separator goes before the item's leading comments, so a doc
            // comment stays attached to what it documents.
            if (prev != nullptr) {
                bool big = is_big_member(prev) || is_big_member(d);
                bool user_blank = first_line_from(anchor) > line_of(prev->span.end) + 1;
                if ((big || user_blank) && !ended_with_blank_line()) out_ += '\n';
            }
            emit_comments_before(anchor, prev == nullptr && out_.empty());
            decl(d);
            prev = d;
        }
        imports_before(UINT32_MAX);
        emit_remaining_comments();
    }

    void import_decl(const ImportDecl& im) {
        line_start();
        out_ += "import ";
        out_ += im.package.name;
        if (im.has_items) {
            out_ += ".{";
            for (std::size_t i = 0; i < im.items.size(); ++i) {
                if (i > 0) out_ += ", ";
                out_ += im.items[i].name.name;
                if (!im.items[i].alias.empty()) {
                    out_ += " as ";
                    out_ += im.items[i].alias.name;
                }
            }
            out_ += '}';
        } else if (!im.alias.empty()) {
            out_ += " as ";
            out_ += im.alias.name;
        }
        end_line_at(im.span.end);
    }

    void script(const Script& s) {
        for (const ImportDecl* im : s.imports) {
            emit_comments_before(im->span.begin, out_.empty());
            import_decl(*im);
        }
        if (!s.imports.empty() && !s.stmts.empty()) out_ += '\n';
        const auto& stmts = s.stmts;
        for (std::size_t i = 0; i < stmts.size(); ++i) {
            const Stmt* st = stmts[i];
            std::uint32_t anchor = stmt_start(st);
            if (i > 0) { // before the comments: see module()
                const Stmt* prev = stmts[i - 1];
                std::uint32_t prev_end_ln = line_of(prev->span.end);
                bool big_gap = is_big(prev) || is_big(st);
                bool user_blank = first_line_from(anchor) > prev_end_ln + 1;
                if ((big_gap || user_blank) && !ended_with_blank_line()) out_ += '\n';
            }
            emit_comments_before(anchor, i == 0);
            if (auto d = as<DeclStmt>(st)) {
                decl(d->decl);
                continue;
            }
            if (auto f = as<ForeachStmt>(st)) attributes(f->attributes);
            line_start();
            stmt_body(st);
            while (i + 1 < stmts.size() && shares_line_with_semicolon(stmts[i], stmts[i + 1])) {
                out_ += "; ";
                ++i;
                stmt_body(stmts[i]);
            }
            end_line_at(stmts[i]->span.end);
        }
        emit_remaining_comments();
    }

private:
    static bool is_big(const Stmt* s) {
        auto d = as<DeclStmt>(s);
        return d != nullptr && is_big_member(d->decl);
    }

    static std::uint32_t decl_start(const Decl* d) {
        if (d == nullptr) return 0;
        if (!d->attributes.empty()) return d->attributes[0].span.begin;
        return d->span.begin;
    }

    static std::uint32_t stmt_start(const Stmt* s) {
        if (s == nullptr) return 0;
        if (auto d = as<DeclStmt>(s)) return decl_start(d->decl);
        if (auto f = as<ForeachStmt>(s); f != nullptr && !f->attributes.empty()) return f->attributes[0].span.begin;
        return s->span.begin;
    }

    std::uint32_t line_of(std::uint32_t offset) const { return src_.line_col(offset).line; }

    // Line where the next item really starts: its first leading comment if
    // one comes before `anchor`, else the item itself. A user's blank line
    // between two items is one above that.
    std::uint32_t first_line_from(std::uint32_t anchor) const {
        if (comment_idx_ < comments_.size() && comments_[comment_idx_].span.begin < anchor) {
            return comments_[comment_idx_].start_line;
        }
        return line_of(anchor);
    }

    bool ended_with_blank_line() const {
        return out_.size() >= 2 && out_[out_.size() - 1] == '\n' && out_[out_.size() - 2] == '\n';
    }

    bool at_block_top() const {
        return out_.empty() || (out_.size() >= 2 && out_[out_.size() - 1] == '\n' && out_[out_.size() - 2] == '{');
    }

    void line_start() {
        if (inline_ == 0) out_.append(static_cast<std::size_t>(indent_) * 4, ' ');
    }

    void end_line() {
        if (inline_ == 0) out_ += '\n';
    }

    void end_line_at(std::uint32_t end_offset) {
        if (inline_ > 0) return;
        emit_trailing_comment_on_line(line_of(end_offset), end_offset);
        out_ += '\n';
    }

    bool has_comment_in(Span span) const {
        for (std::size_t i = comment_idx_; i < comments_.size(); ++i) {
            if (comments_[i].span.begin >= span.end) break;
            if (comments_[i].span.begin >= span.begin) return true;
        }
        return false;
    }

    bool has_comment_between(std::uint32_t lo, std::uint32_t hi) const {
        for (std::size_t i = comment_idx_; i < comments_.size(); ++i) {
            if (comments_[i].span.begin >= hi) break;
            if (comments_[i].span.begin >= lo) return true;
        }
        return false;
    }

    void emit_comment_text(const Comment& c) {
        if (!c.is_block || c.start_line == c.end_line) {
            out_ += c.text;
            return;
        }
        // Multiline block comment: indent each continuation line relative to its
        // original starting column.
        std::uint32_t orig_col = src_.line_col(c.span.begin).column;
        std::size_t base_indent = orig_col > 1 ? static_cast<std::size_t>(orig_col - 1) : 0;
        std::size_t pos = 0;
        bool first = true;
        while (pos < c.text.size()) {
            std::size_t nl = c.text.find('\n', pos);
            std::string_view line = c.text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!first) {
                out_ += '\n';
                std::size_t spaces = 0;
                while (spaces < line.size() && line[spaces] == ' ') ++spaces;
                if (!line.substr(spaces).empty()) {
                    line_start();
                    if (spaces > base_indent) out_.append(spaces - base_indent, ' ');
                    out_ += line.substr(spaces);
                }
            } else {
                out_ += line;
            }
            first = false;
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
    }

    // Emits any unconsumed comment that is on `line` and starts at or after
    // `min_offset` (or any earlier unconsumed comment on that same line).
    void emit_trailing_comment_on_line(std::uint32_t line, std::uint32_t /*min_offset*/) {
        if (inline_ > 0) return;
        while (comment_idx_ < comments_.size()) {
            const Comment& c = comments_[comment_idx_];
            if (c.start_line > line) break;
            if (c.start_line == line && c.trailing) {
                out_ += ' ';
                emit_comment_text(c);
                ++comment_idx_;
            } else {
                break;
            }
        }
    }

    // Emits all comments whose span starts before `limit_offset`.
    void emit_comments_before(std::uint32_t limit_offset, bool is_first_in_scope = false) {
        if (inline_ > 0) return;
        while (comment_idx_ < comments_.size() && comments_[comment_idx_].span.begin < limit_offset) {
            const Comment& c = comments_[comment_idx_++];
            if (c.blank_before && !is_first_in_scope && !at_block_top() && !ended_with_blank_line()) {
                out_ += '\n';
            }
            line_start();
            emit_comment_text(c);
            out_ += '\n';
            is_first_in_scope = false;
            // Preserve a single blank line after a comment if the next token or
            // comment is separated by an empty line.
            std::uint32_t next_line = comment_idx_ < comments_.size() && comments_[comment_idx_].span.begin < limit_offset
                                          ? comments_[comment_idx_].start_line
                                          : line_of(limit_offset);
            if (next_line > c.end_line + 1 && !ended_with_blank_line()) {
                out_ += '\n';
            }
        }
    }

    void emit_remaining_comments() {
        if (inline_ > 0) return;
        while (comment_idx_ < comments_.size()) {
            const Comment& c = comments_[comment_idx_++];
            if (c.blank_before && !out_.empty() && !ended_with_blank_line()) {
                out_ += '\n';
            }
            line_start();
            emit_comment_text(c);
            out_ += '\n';
        }
    }

    static bool is_compound_stmt(const Stmt* s) {
        if (s == nullptr) return false;
        switch (s->kind) {
            case StmtKind::Block:
            case StmtKind::If:
            case StmtKind::Match:
            case StmtKind::For:
            case StmtKind::Foreach:
            case StmtKind::While:
            case StmtKind::Loop:
            case StmtKind::Unsafe: return true;
            case StmtKind::Decl: return is_big_member(as<DeclStmt>(s)->decl);
            default: return false;
        }
    }

    // True when `a` and `b` were intentionally placed on the same line with `;`
    // (LANGUAGE.md 6: "One statement per line; no `;` except when sharing a line
    // on purpose").
    bool shares_line_with_semicolon(const Stmt* a, const Stmt* b) const {
        if (a == nullptr || b == nullptr) return false;
        if (is_compound_stmt(a) || is_compound_stmt(b)) return false;
        if (line_of(a->span.begin) != line_of(a->span.end)) return false;
        if (line_of(b->span.begin) != line_of(b->span.end)) return false;
        if (line_of(a->span.end) != line_of(b->span.begin)) return false;
        if (b->span.begin < a->span.end) return false;
        std::string_view between = src_.slice({a->span.end, b->span.begin});
        return between.find(';') != std::string_view::npos;
    }

    // Finds the offset of `ch` at or before `before_offset` within `outer`.
    std::uint32_t find_open_delim(Span outer, std::uint32_t before_offset, char ch) const {
        std::string_view text = src_.text();
        std::uint32_t hi = std::min<std::uint32_t>(before_offset, static_cast<std::uint32_t>(text.size()));
        std::uint32_t lo = std::min<std::uint32_t>(outer.begin, hi);
        for (std::uint32_t p = hi; p > lo; --p) {
            if (text[p - 1] == ch) return p - 1;
        }
        return lo;
    }

    // True when a bracketed list of elements was written across multiple lines
    // (newline after opening delimiter, between elements, or before closing
    // delimiter), rather than a single-line call whose argument happens to be a
    // multiline lambda.
    template <class T>
    bool is_multiline_seq(Span outer, const List<T>& items, char open_ch) const {
        if (inline_ > 0 || items.empty()) return false;
        std::uint32_t first_begin = item_span(items[0]).begin;
        std::uint32_t last_end = item_span(items[items.size() - 1]).end;
        std::uint32_t open_pos = find_open_delim(outer, first_begin, open_ch);
        std::uint32_t close_pos = outer.end > 0 ? outer.end - 1 : outer.end;
        if (line_of(open_pos) != line_of(first_begin)) return true;
        if (line_of(last_end) != line_of(close_pos)) return true;
        for (std::size_t i = 0; i + 1 < items.size(); ++i) {
            if (line_of(item_span(items[i]).end) != line_of(item_span(items[i + 1]).begin)) return true;
        }
        return false;
    }

    static Span item_span(const Expr* e) { return e != nullptr ? e->span : Span{}; }
    static Span item_span(const Arg& a) { return a.span; }
    static Span item_span(const Param& p) { return p.span; }
    static Span item_span(const FieldInit& f) { return f.span; }
    static Span item_span(const MapEntry& m) {
        return Span::cover(m.key != nullptr ? m.key->span : Span{}, m.value != nullptr ? m.value->span : Span{});
    }

    // ------------------------------------------------------------ declarations

    void attributes(const List<Attribute>& attrs) {
        for (std::size_t i = 0; i < attrs.size(); ++i) {
            const Attribute& a = attrs[i];
            emit_comments_before(a.span.begin);
            line_start();
            out_ += '@';
            out_ += a.name.name;
            if (a.has_args) args(a.span, a.args);
            end_line_at(a.span.end);
            if (inline_ > 0) out_ += ' ';
        }
    }

    void decl(const Decl* d) {
        if (d == nullptr) return;
        attributes(d->attributes);
        emit_comments_before(d->span.begin);
        line_start();
        decl_body(d);
        end_line_at(d->span.end);
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
                Span sig_span{f->name.span.end, f->ret != nullptr   ? f->ret->span.begin
                                                : f->body != nullptr ? f->body->span.begin
                                                                     : f->span.end};
                params(sig_span, f->params);
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
                members(c->span, c->members);
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
                members(s->span, s->members);
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
                members(t->span, t->members);
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
                if (e->variants.empty() && !has_comment_in(e->span)) {
                    out_ += " {}";
                    break;
                }
                bool single_line = (line_of(e->span.begin) == line_of(e->span.end)) && !has_comment_in(e->span);
                if (single_line) {
                    out_ += " { ";
                    ++inline_;
                    for (std::size_t i = 0; i < e->variants.size(); ++i) {
                        if (i > 0) out_ += ", ";
                        const EnumVariant& v = e->variants[i];
                        out_ += v.name.name;
                        if (v.has_params) params(v.span, v.params);
                        if (v.value != nullptr) {
                            out_ += " = ";
                            expr(v.value);
                        }
                    }
                    --inline_;
                    out_ += " }";
                    break;
                }
                // Style guide (LANGUAGE.md 6): trailing commas in multiline enums.
                out_ += " {";
                emit_trailing_comment_on_line(line_of(e->name.span.end), e->name.span.end);
                out_ += '\n';
                ++indent_;
                const EnumVariant* prev = nullptr;
                for (const EnumVariant& v : e->variants) {
                    emit_comments_before(v.span.begin, prev == nullptr);
                    if (prev != nullptr && line_of(v.span.begin) > line_of(prev->span.end) + 1 &&
                        !has_comment_between(prev->span.end, v.span.begin) && !ended_with_blank_line()) {
                        out_ += '\n';
                    }
                    line_start();
                    out_ += v.name.name;
                    if (v.has_params) params(v.span, v.params);
                    if (v.value != nullptr) {
                        out_ += " = ";
                        expr(v.value);
                    }
                    out_ += ',';
                    end_line_at(v.span.end);
                    prev = &v;
                }
                emit_comments_before(e->span.end, prev == nullptr);
                --indent_;
                line_start();
                out_ += '}';
                break;
            }
            case DeclKind::Signal: {
                auto s = as<SignalDecl>(d);
                out_ += "signal ";
                out_ += s->name.name;
                if (s->has_params) params(s->span, s->params);
                break;
            }
        }
    }

    void accessors(const VarDecl* v) {
        out_ += " {\n";
        ++indent_;
        if (v->getter != nullptr) {
            emit_comments_before(v->getter->span.begin, true);
            line_start();
            out_ += "get ";
            block(v->getter);
            end_line_at(v->getter->span.end);
        }
        if (v->setter != nullptr) {
            emit_comments_before(v->setter->span.begin, v->getter == nullptr);
            line_start();
            out_ += "set(";
            out_ += v->setter_param.name;
            out_ += ") ";
            block(v->setter);
            end_line_at(v->setter->span.end);
        }
        emit_comments_before(v->span.end);
        --indent_;
        line_start();
        out_ += '}';
    }

    void members(Span outer, const List<Decl*>& ms) {
        if (ms.empty() && !has_comment_in(outer)) {
            out_ += " {}";
            return;
        }
        std::uint32_t open_brace = ms.empty() ? outer.begin : find_open_delim(outer, decl_start(ms[0]), '{');
        out_ += " {";
        emit_trailing_comment_on_line(line_of(open_brace), open_brace);
        out_ += '\n';
        ++indent_;
        const Decl* prev = nullptr;
        for (const Decl* m : ms) {
            std::uint32_t anchor = decl_start(m);
            if (prev != nullptr) { // before the comments: see module()
                bool big = is_big_member(prev) || is_big_member(m);
                bool user_blank = first_line_from(anchor) > line_of(prev->span.end) + 1;
                if ((big || user_blank) && !ended_with_blank_line()) out_ += '\n';
            }
            emit_comments_before(anchor, prev == nullptr);
            decl(m);
            prev = m;
        }
        emit_comments_before(outer.end, prev == nullptr);
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

    void params(Span outer, const List<Param>& ps) {
        if (is_multiline_seq(outer, ps, '(')) {
            out_ += "(\n";
            ++indent_;
            for (std::size_t i = 0; i < ps.size(); ++i) {
                const Param& p = ps[i];
                emit_comments_before(p.span.begin, i == 0);
                line_start();
                param_one(p);
                out_ += ',';
                end_line_at(p.span.end);
            }
            emit_comments_before(outer.end);
            --indent_;
            line_start();
            out_ += ')';
            return;
        }
        out_ += '(';
        bool first = true;
        for (const Param& p : ps) {
            if (!first) out_ += ", ";
            first = false;
            param_one(p);
        }
        out_ += ')';
    }

    void param_one(const Param& p) {
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
        if (b->stmts.empty() && !has_comment_in(b->span)) {
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
        out_ += '{';
        emit_trailing_comment_on_line(line_of(b->span.begin), b->span.begin);
        out_ += '\n';
        ++indent_;
        const auto& stmts = b->stmts;
        for (std::size_t i = 0; i < stmts.size(); ++i) {
            const Stmt* s = stmts[i];
            std::uint32_t anchor = stmt_start(s);
            if (i > 0) { // before the comments: see module()
                const Stmt* prev = stmts[i - 1];
                bool user_blank = first_line_from(anchor) > line_of(prev->span.end) + 1;
                if (user_blank && !ended_with_blank_line()) out_ += '\n';
            }
            emit_comments_before(anchor, i == 0);
            if (auto d = as<DeclStmt>(s)) {
                if (!shares_line_with_semicolon(s, i + 1 < stmts.size() ? stmts[i + 1] : nullptr)) {
                    decl(d->decl);
                    continue;
                }
            }
            if (auto f = as<ForeachStmt>(s)) attributes(f->attributes);
            line_start();
            stmt_body(s);
            while (i + 1 < stmts.size() && shares_line_with_semicolon(stmts[i], stmts[i + 1])) {
                out_ += "; ";
                ++i;
                stmt_body(stmts[i]);
            }
            end_line_at(stmts[i]->span.end);
        }
        emit_comments_before(b->span.end, stmts.empty());
        --indent_;
        line_start();
        out_ += '}';
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
                if (m->arms.empty() && !has_comment_in(m->span)) {
                    out_ += " {}";
                    break;
                }
                if (inline_ > 0) {
                    out_ += " { ";
                    bool first = true;
                    for (const MatchArm& a : m->arms) {
                        if (!first) out_ += "; ";
                        first = false;
                        pattern(a.pattern);
                        if (a.guard != nullptr) {
                            out_ += " if ";
                            expr(a.guard, Prec::Lowest, {.no_struct = false, .no_lambda = true});
                        }
                        out_ += " => ";
                        stmt_body(a.body);
                    }
                    out_ += " }";
                    break;
                }
                out_ += " {";
                emit_trailing_comment_on_line(line_of(m->subject->span.end), m->subject->span.end);
                out_ += '\n';
                ++indent_;
                const MatchArm* prev = nullptr;
                for (const MatchArm& a : m->arms) {
                    emit_comments_before(a.span.begin, prev == nullptr);
                    if (prev != nullptr && line_of(a.span.begin) > line_of(prev->span.end) + 1 &&
                        !has_comment_between(prev->span.end, a.span.begin) && !ended_with_blank_line()) {
                        out_ += '\n';
                    }
                    line_start();
                    pattern(a.pattern);
                    if (a.guard != nullptr) {
                        out_ += " if ";
                        expr(a.guard, Prec::Lowest, {.no_struct = false, .no_lambda = true});
                    }
                    out_ += " => ";
                    stmt_body(a.body);
                    end_line_at(a.span.end);
                    prev = &a;
                }
                emit_comments_before(m->span.end, prev == nullptr);
                --indent_;
                line_start();
                out_ += '}';
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
                if (e->has_args) args(e->span, e->args);
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

    void args(Span outer, const List<Arg>& as_) {
        if (is_multiline_seq(outer, as_, '(')) {
            out_ += "(\n";
            ++indent_;
            for (std::size_t i = 0; i < as_.size(); ++i) {
                const Arg& a = as_[i];
                emit_comments_before(a.span.begin, i == 0);
                line_start();
                arg_one(a);
                out_ += ',';
                end_line_at(a.span.end);
            }
            emit_comments_before(outer.end);
            --indent_;
            line_start();
            out_ += ')';
            return;
        }
        out_ += '(';
        bool first = true;
        for (const Arg& a : as_) {
            if (!first) out_ += ", ";
            first = false;
            arg_one(a);
        }
        out_ += ')';
    }

    void arg_one(const Arg& a) {
        if (!a.name.empty()) {
            out_ += a.name.name;
            out_ += " = ";
        }
        expr(a.value);
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

    void expr(const Expr* e, Prec required = Prec::Lowest, Ctx ctx = {}) {
        if (e == nullptr) {
            out_ += "<error>";
            return;
        }
        bool parens = precedence(e) < required ||
                      (ctx.no_struct && (e->kind == ExprKind::StructLit || e->kind == ExprKind::MapLit)) ||
                      (ctx.no_lambda && e->kind == ExprKind::Lambda && as<LambdaExpr>(e)->arrow);
        if (parens) {
            out_ += '(';
            ctx = {};
        }
        expr_inner(e, ctx);
        if (parens) out_ += ')';
    }

    void expr_inner(const Expr* e, Ctx ctx) {
        switch (e->kind) {
            case ExprKind::Error: out_ += "<error>"; break;
            case ExprKind::Int: {
                auto i = as<IntLit>(e);
                out_ += i->text.empty() ? std::to_string(i->value) + std::string(to_string(i->suffix))
                                        : std::string(i->text);
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
                auto l = as<ListLit>(e);
                if (is_multiline_seq(l->span, l->elements, '[')) {
                    out_ += "[\n";
                    ++indent_;
                    for (std::size_t i = 0; i < l->elements.size(); ++i) {
                        const Expr* x = l->elements[i];
                        emit_comments_before(x->span.begin, i == 0);
                        line_start();
                        expr(x);
                        out_ += ',';
                        end_line_at(x->span.end);
                    }
                    emit_comments_before(l->span.end);
                    --indent_;
                    line_start();
                    out_ += ']';
                    break;
                }
                out_ += '[';
                bool first = true;
                for (const Expr* x : l->elements) {
                    if (!first) out_ += ", ";
                    first = false;
                    expr(x);
                }
                out_ += ']';
                break;
            }
            case ExprKind::MapLit: {
                auto m = as<MapLit>(e);
                if (is_multiline_seq(m->span, m->entries, '{')) {
                    out_ += "{\n";
                    ++indent_;
                    for (std::size_t i = 0; i < m->entries.size(); ++i) {
                        const MapEntry& entry = m->entries[i];
                        emit_comments_before(entry.key->span.begin, i == 0);
                        line_start();
                        expr(entry.key);
                        out_ += ": ";
                        expr(entry.value);
                        out_ += ',';
                        end_line_at(entry.value->span.end);
                    }
                    emit_comments_before(m->span.end);
                    --indent_;
                    line_start();
                    out_ += '}';
                    break;
                }
                out_ += '{';
                bool first = true;
                for (const MapEntry& entry : m->entries) {
                    if (!first) out_ += ", ";
                    first = false;
                    expr(entry.key);
                    out_ += ": ";
                    expr(entry.value);
                }
                out_ += '}';
                break;
            }
            case ExprKind::StructLit: {
                auto s = as<StructLit>(e);
                type(s->type);
                if (s->fields.empty() && !has_comment_in(s->span)) {
                    out_ += " {}";
                    break;
                }
                if (is_multiline_seq(s->span, s->fields, '{')) {
                    out_ += " {\n";
                    ++indent_;
                    for (std::size_t i = 0; i < s->fields.size(); ++i) {
                        const FieldInit& f = s->fields[i];
                        emit_comments_before(f.span.begin, i == 0);
                        line_start();
                        out_ += f.name.name;
                        out_ += " = ";
                        expr(f.value);
                        out_ += ',';
                        end_line_at(f.span.end);
                    }
                    emit_comments_before(s->span.end);
                    --indent_;
                    line_start();
                    out_ += '}';
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
                Span args_span{c->callee->span.end, c->span.end};
                args(args_span, c->args);
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
                // Preserve multiline method chains (LANGUAGE.md 1.3):
                //   g.path()
                //       .move_to(a)
                if (inline_ == 0 && line_of(m->member.span.begin) > line_of(m->object->span.end)) {
                    emit_trailing_comment_on_line(line_of(m->object->span.end), m->object->span.end);
                    out_ += '\n';
                    out_.append(static_cast<std::size_t>(indent_ + 1) * 4, ' ');
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
                Span sig_span{l->span.begin, l->ret != nullptr    ? l->ret->span.begin
                                             : l->body != nullptr ? l->body->span.begin
                                                                  : l->span.end};
                params(sig_span, l->params);
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
                Span args_span{n->type != nullptr ? n->type->span.end : n->span.begin, n->span.end};
                args(args_span, n->args);
                break;
            }
            case ExprKind::Alloc: {
                auto a = as<AllocExpr>(e);
                out_ += "alloc ";
                type(a->type);
                Span args_span{a->type != nullptr ? a->type->span.end : a->span.begin, a->span.end};
                args(args_span, a->args);
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

    const SourceFile& src_;
    std::vector<Comment> comments_;
    std::size_t comment_idx_ = 0;
    std::string out_;
    int indent_ = 0;
    int inline_ = 0;
};

// ------------------------------------------------------------ Style Checker

bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// PascalCase: starts with A-Z, only letters and digits (no underscores).
bool is_pascal_case(std::string_view s) {
    if (s.empty() || !is_upper(s[0])) return false;
    for (char c : s) {
        if (!is_upper(c) && !is_lower(c) && !is_digit(c)) return false;
    }
    return true;
}

// snake_case: optional leading `_` (for private members), then starts with a-z
// and contains only a-z, 0-9, and `_` (or bare `_` wildcard).
bool is_snake_case(std::string_view s, bool allow_leading_underscore = true) {
    if (s == "_") return true;
    if (allow_leading_underscore && !s.empty() && s[0] == '_') s.remove_prefix(1);
    if (s.empty() || !is_lower(s[0])) return false;
    for (char c : s) {
        if (!is_lower(c) && !is_digit(c) && c != '_') return false;
    }
    return true;
}

// SCREAMING_SNAKE: starts with A-Z (after optional `_`), only A-Z, 0-9, `_`.
bool is_screaming_snake(std::string_view s) {
    if (!s.empty() && s[0] == '_') s.remove_prefix(1);
    if (s.empty() || !is_upper(s[0])) return false;
    for (char c : s) {
        if (!is_upper(c) && !is_digit(c) && c != '_') return false;
    }
    return true;
}

// Split an identifier into lowercase word tokens.
std::vector<std::string> split_words(std::string_view s) {
    std::vector<std::string> words;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '_') {
            if (!cur.empty()) {
                words.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        if (is_upper(c)) {
            bool prev_lower_or_digit = (i > 0 && (is_lower(s[i - 1]) || is_digit(s[i - 1])));
            bool prev_upper_next_lower = (i > 0 && is_upper(s[i - 1]) && i + 1 < s.size() && is_lower(s[i + 1]));
            if (!cur.empty() && (prev_lower_or_digit || prev_upper_next_lower)) {
                words.push_back(std::move(cur));
                cur.clear();
            }
            cur.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    return words;
}

std::string to_pascal_case(std::string_view s) {
    auto words = split_words(s);
    std::string out;
    for (auto& w : words) {
        if (!w.empty() && is_lower(w[0])) w[0] = static_cast<char>(w[0] - 'a' + 'A');
        out += w;
    }
    return out;
}

std::string to_snake_case(std::string_view s, bool allow_leading_underscore = true) {
    bool leading = allow_leading_underscore && !s.empty() && s[0] == '_';
    auto words = split_words(s);
    std::string out = leading ? "_" : "";
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) out += '_';
        out += words[i];
    }
    return out;
}

std::string to_screaming_snake(std::string_view s) {
    bool leading = !s.empty() && s[0] == '_';
    auto words = split_words(s);
    std::string out = leading ? "_" : "";
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0) out += '_';
        for (char c : words[i]) {
            out.push_back(is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c);
        }
    }
    return out;
}

class StyleChecker {
public:
    explicit StyleChecker(DiagnosticSink& diags) : diags_(diags) {}

    void module(const Module& m) {
        for (const Decl* d : m.decls) decl(d, /*top_level=*/true);
    }

    void script(const Script& s) {
        for (const Stmt* st : s.stmts) stmt(st, /*top_level=*/true);
    }

private:
    void warn(Span span, std::string msg, std::string replacement = {}) {
        Diagnostic& d = diags_.report(Severity::Warning, span, std::move(msg));
        if (!replacement.empty()) {
            d.fix = FixIt{span, std::move(replacement)};
        }
    }

    void check_pascal(const Ident& id, std::string_view kind) {
        if (!id.empty() && !is_pascal_case(id.name)) {
            warn(id.span, std::format("{} '{}' should use PascalCase (style guide, LANGUAGE.md 6)", kind, id.name),
                 to_pascal_case(id.name));
        }
    }

    void check_snake(const Ident& id, std::string_view kind, bool allow_leading_underscore = true) {
        if (!id.empty() && !is_snake_case(id.name, allow_leading_underscore)) {
            warn(id.span, std::format("{} '{}' should use snake_case (style guide, LANGUAGE.md 6)", kind, id.name),
                 to_snake_case(id.name, allow_leading_underscore));
        }
    }

    void check_const_name(const Ident& id) {
        if (!id.empty() && !is_screaming_snake(id.name)) {
            warn(id.span,
                 std::format("constant '{}' should use SCREAMING_SNAKE_CASE (style guide, LANGUAGE.md 6)", id.name),
                 to_screaming_snake(id.name));
        }
    }

    void decl(const Decl* d, bool top_level) {
        if (d == nullptr) return;
        for (const Attribute& a : d->attributes) {
            for (const Arg& arg : a.args) expr(arg.value);
        }
        switch (d->kind) {
            case DeclKind::Error: break;
            case DeclKind::Var: {
                auto v = as<VarDecl>(d);
                if (top_level || v->is_static) {
                    if (!v->name.empty() && !is_snake_case(v->name.name, true) && !is_screaming_snake(v->name.name)) {
                        warn(v->name.span,
                             std::format("variable '{}' should use snake_case or SCREAMING_SNAKE_CASE (style guide, "
                                         "LANGUAGE.md 6)",
                                         v->name.name),
                             to_snake_case(v->name.name, true));
                    }
                } else {
                    check_snake(v->name, "variable", true);
                }
                if (v->init != nullptr) expr(v->init);
                if (v->has_accessors) {
                    if (v->getter != nullptr) stmt(v->getter, false);
                    if (v->setter != nullptr) {
                        check_snake(v->setter_param, "parameter", false);
                        stmt(v->setter, false);
                    }
                }
                break;
            }
            case DeclKind::Const: {
                auto c = as<ConstDecl>(d);
                check_const_name(c->name);
                expr(c->value);
                break;
            }
            case DeclKind::Fn: {
                auto f = as<FnDecl>(d);
                check_snake(f->name, "function", true);
                for (const GenericParam& g : f->generics) check_pascal(g.name, "type parameter");
                for (const Param& p : f->params) {
                    check_snake(p.name, "parameter", true);
                    if (p.default_value != nullptr) expr(p.default_value);
                }
                if (f->body != nullptr) stmt(f->body, false);
                break;
            }
            case DeclKind::Class: {
                auto c = as<ClassDecl>(d);
                check_pascal(c->name, "class");
                for (const GenericParam& g : c->generics) check_pascal(g.name, "type parameter");
                for (const Decl* m : c->members) decl(m, false);
                break;
            }
            case DeclKind::Struct: {
                auto s = as<StructDecl>(d);
                check_pascal(s->name, "struct");
                for (const GenericParam& g : s->generics) check_pascal(g.name, "type parameter");
                for (const Decl* m : s->members) decl(m, false);
                break;
            }
            case DeclKind::Trait: {
                auto t = as<TraitDecl>(d);
                check_pascal(t->name, "trait");
                for (const GenericParam& g : t->generics) check_pascal(g.name, "type parameter");
                for (const Decl* m : t->members) decl(m, false);
                break;
            }
            case DeclKind::Enum: {
                auto e = as<EnumDecl>(d);
                check_pascal(e->name, "enum");
                for (const EnumVariant& v : e->variants) {
                    check_pascal(v.name, "enum variant");
                    for (const Param& p : v.params) check_snake(p.name, "field", false);
                    if (v.value != nullptr) expr(v.value);
                }
                break;
            }
            case DeclKind::Signal: {
                auto s = as<SignalDecl>(d);
                check_snake(s->name, "signal", true);
                for (const Param& p : s->params) check_snake(p.name, "parameter", false);
                break;
            }
        }
    }

    void stmt(const Stmt* s, bool top_level) {
        if (s == nullptr) return;
        switch (s->kind) {
            case StmtKind::Error:
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Yield: break;
            case StmtKind::Expr: expr(as<ExprStmt>(s)->expr); break;
            case StmtKind::Decl: decl(as<DeclStmt>(s)->decl, top_level); break;
            case StmtKind::Assign: {
                auto a = as<AssignStmt>(s);
                expr(a->target);
                expr(a->value);
                break;
            }
            case StmtKind::Block:
                for (const Stmt* child : as<BlockStmt>(s)->stmts) stmt(child, false);
                break;
            case StmtKind::If: {
                auto i = as<IfStmt>(s);
                for (const IfBranch& b : i->branches) {
                    expr(b.cond);
                    if (!b.unwrap.empty()) check_snake(b.unwrap, "variable", false);
                    stmt(b.body, false);
                }
                if (i->else_body != nullptr) stmt(i->else_body, false);
                break;
            }
            case StmtKind::Match: {
                auto m = as<MatchStmt>(s);
                expr(m->subject);
                for (const MatchArm& a : m->arms) {
                    if (a.guard != nullptr) expr(a.guard);
                    stmt(a.body, false);
                }
                break;
            }
            case StmtKind::For: {
                auto f = as<ForStmt>(s);
                if (!f->label.empty()) check_snake(f->label, "label", false);
                check_snake(f->first, "variable", false);
                if (!f->second.empty()) check_snake(f->second, "variable", false);
                expr(f->iterable);
                stmt(f->body, false);
                break;
            }
            case StmtKind::Foreach: {
                auto f = as<ForeachStmt>(s);
                for (const Attribute& a : f->attributes) {
                    for (const Arg& arg : a.args) expr(arg.value);
                }
                check_snake(f->var, "variable", false);
                expr(f->pool);
                stmt(f->body, false);
                break;
            }
            case StmtKind::While: {
                auto w = as<WhileStmt>(s);
                if (!w->label.empty()) check_snake(w->label, "label", false);
                expr(w->cond);
                stmt(w->body, false);
                break;
            }
            case StmtKind::Loop: {
                auto l = as<LoopStmt>(s);
                if (!l->label.empty()) check_snake(l->label, "label", false);
                stmt(l->body, false);
                break;
            }
            case StmtKind::Return: expr(as<ReturnStmt>(s)->value); break;
            case StmtKind::Defer: stmt(as<DeferStmt>(s)->body, false); break;
            case StmtKind::Unsafe: stmt(as<UnsafeStmt>(s)->body, false); break;
            case StmtKind::Emit:
                for (const Arg& a : as<EmitStmt>(s)->args) expr(a.value);
                break;
        }
    }

    void expr(const Expr* e) {
        if (e == nullptr) return;
        switch (e->kind) {
            case ExprKind::Error:
            case ExprKind::Int:
            case ExprKind::Float:
            case ExprKind::Bool:
            case ExprKind::Null:
            case ExprKind::Identifier:
            case ExprKind::Self:
            case ExprKind::Super: break;
            case ExprKind::Name: {
                auto n = as<NameLit>(e);
                if (!n->name.empty() && !is_snake_case(n->name, false)) {
                    warn(n->span, std::format("interned name '#{}' should use #snake_case (style guide, LANGUAGE.md 6)",
                                              n->name),
                         "#" + to_snake_case(n->name, false));
                }
                break;
            }
            case ExprKind::String:
                for (const StringPart& p : as<StringLit>(e)->parts) {
                    if (p.expr != nullptr) expr(p.expr);
                }
                break;
            case ExprKind::ListLit:
                for (const Expr* x : as<ListLit>(e)->elements) expr(x);
                break;
            case ExprKind::MapLit:
                for (const MapEntry& m : as<MapLit>(e)->entries) {
                    expr(m.key);
                    expr(m.value);
                }
                break;
            case ExprKind::StructLit:
                for (const FieldInit& f : as<StructLit>(e)->fields) expr(f.value);
                break;
            case ExprKind::Unary: expr(as<UnaryExpr>(e)->operand); break;
            case ExprKind::Binary: {
                auto b = as<BinaryExpr>(e);
                expr(b->lhs);
                expr(b->rhs);
                break;
            }
            case ExprKind::Range: {
                auto r = as<RangeExpr>(e);
                expr(r->lo);
                expr(r->hi);
                expr(r->step);
                break;
            }
            case ExprKind::Ternary: {
                auto t = as<TernaryExpr>(e);
                expr(t->cond);
                expr(t->then_expr);
                expr(t->else_expr);
                break;
            }
            case ExprKind::Is: {
                auto i = as<IsExpr>(e);
                expr(i->operand);
                if (!i->binding.empty()) check_snake(i->binding, "variable", false);
                break;
            }
            case ExprKind::As: expr(as<AsExpr>(e)->operand); break;
            case ExprKind::Call: {
                auto c = as<CallExpr>(e);
                expr(c->callee);
                for (const Arg& a : c->args) expr(a.value);
                break;
            }
            case ExprKind::Index: {
                auto i = as<IndexExpr>(e);
                expr(i->object);
                expr(i->index);
                break;
            }
            case ExprKind::Member: expr(as<MemberExpr>(e)->object); break;
            case ExprKind::Try: expr(as<TryExpr>(e)->operand); break;
            case ExprKind::Lambda: {
                auto l = as<LambdaExpr>(e);
                for (const Param& p : l->params) {
                    check_snake(p.name, "parameter", false);
                    expr(p.default_value);
                }
                if (l->arrow) {
                    expr(l->expr_body);
                } else {
                    stmt(l->body, false);
                }
                break;
            }
            case ExprKind::New:
                for (const Arg& a : as<NewExpr>(e)->args) expr(a.value);
                break;
            case ExprKind::Alloc:
                for (const Arg& a : as<AllocExpr>(e)->args) expr(a.value);
                break;
            case ExprKind::Await: expr(as<AwaitExpr>(e)->operand); break;
            case ExprKind::Start: expr(as<StartExpr>(e)->operand); break;
            case ExprKind::Free: expr(as<FreeExpr>(e)->operand); break;
        }
    }

    DiagnosticSink& diags_;
};

} // namespace

void check_style(const SourceFile& /*source*/, const Module& module, DiagnosticSink& diags) {
    StyleChecker(diags).module(module);
}

void check_style(const SourceFile& /*source*/, const Script& script, DiagnosticSink& diags) {
    StyleChecker(diags).script(script);
}

FormatResult format_source(const SourceFile& source, FormatOptions options) {
    Arena arena;
    DiagnosticSink parse_diags(source);
    FormatResult res;

    if (options.script_mode) {
        Script s = parse_script(source, arena, parse_diags);
        bool tree_broken = false;
        for (const Stmt* st : s.stmts) {
            if (has_stmt_error(st)) {
                tree_broken = true;
                break;
            }
        }
        bool unrecoverable = tree_broken;
        for (const Diagnostic& d : parse_diags.all()) {
            if (d.severity == Severity::Error && !is_normalizable_parse_error(d)) {
                unrecoverable = true;
                break;
            }
        }
        if (unrecoverable) {
            res.ok = false;
            res.formatted = std::string(source.text());
            res.diagnostics = parse_diags.all();
            res.rendered = render_all(res.diagnostics, source);
            return res;
        }
        DiagnosticSink style_diags(source);
        if (options.check_style) check_style(source, s, style_diags);
        style_diags.sort_by_position();
        Formatter fmt(source, extract_comments(source));
        fmt.script(s);
        res.ok = true;
        res.formatted = fmt.take();
        res.changed = res.formatted != source.text();
        res.diagnostics = style_diags.all();
        res.rendered = render_all(res.diagnostics, source);
        return res;
    }

    Module m = parse_module(source, arena, parse_diags);
    bool tree_broken = false;
    for (const Decl* d : m.decls) {
        if (has_decl_error(d)) {
            tree_broken = true;
            break;
        }
    }
    bool unrecoverable = tree_broken;
    for (const Diagnostic& d : parse_diags.all()) {
        if (d.severity == Severity::Error && !is_normalizable_parse_error(d)) {
            unrecoverable = true;
            break;
        }
    }
    if (unrecoverable) {
        res.ok = false;
        res.formatted = std::string(source.text());
        res.diagnostics = parse_diags.all();
        res.rendered = render_all(res.diagnostics, source);
        return res;
    }

    DiagnosticSink style_diags(source);
    if (options.check_style) check_style(source, m, style_diags);
    style_diags.sort_by_position();
    Formatter fmt(source, extract_comments(source));
    fmt.module(m);
    res.ok = true;
    res.formatted = fmt.take();
    res.changed = res.formatted != source.text();
    res.diagnostics = style_diags.all();
    res.rendered = render_all(res.diagnostics, source);
    return res;
}

FormatResult format_source(std::string_view path, std::string_view text, FormatOptions options) {
    SourceFile src{std::string(path), std::string(text)};
    return format_source(src, options);
}

} // namespace pluma

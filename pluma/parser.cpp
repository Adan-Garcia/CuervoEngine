#include "pluma/parser.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pluma/lexer.hpp"

namespace pluma {

namespace {

using K = TokenKind;

// `45deg` is stored in radians (LANGUAGE.md 1.5). Plain multiplication by a
// constant: no libm, so the conversion is the same on every platform.
constexpr double deg_to_rad = 3.14159265358979323846 / 180.0;

// Where a declaration appears; drives which modifiers and kinds are legal.
enum class DeclContext : std::uint8_t { TopLevel, Class, Struct, Trait, Local };

// Expression-parsing restrictions. Reset inside any bracket pair.
struct Restrictions {
    // No struct or map literals: the `{` belongs to the following block
    // (`if`/`while`/`for`/`foreach`/`match` headers, LANGUAGE.md 2.6).
    bool no_struct = false;
    // No `x => e` lambdas: `=>` ends a match guard.
    bool no_lambda = false;
    // `if x? t {`: a no-space `?` followed by `IDENT {` is an if-binding.
    bool if_binding = false;
};

Prec next_prec(Prec p) { return static_cast<Prec>(static_cast<int>(p) + 1); }

bool is_opener(K k) { return k == K::LeftParen || k == K::LeftBracket || k == K::LeftBrace; }
bool is_closer(K k) { return k == K::RightParen || k == K::RightBracket || k == K::RightBrace; }

std::string describe(const Token& t) {
    switch (t.kind) {
        case K::Newline: return "newline";
        case K::Eof: return "end of file";
        case K::String: return "string literal";
        case K::Error: return "invalid token";
        default: return std::format("'{}'", t.text);
    }
}

bool is_decl_start(const Token& t) {
    if (t.kind == K::At) return true;
    if (t.kind != K::Keyword) return false;
    for (std::string_view w : {"var", "weak", "const", "fn", "class", "struct", "enum", "trait", "signal", "static",
                               "final", "pub", "manual"}) {
        if (t.text == w) return true;
    }
    return false;
}

class Parser {
public:
    Parser(const SourceFile& source, std::vector<Token> tokens, Arena& arena, DiagnosticSink& diags)
        : src_(source), toks_(std::move(tokens)), arena_(arena), diags_(diags), lex_reported_(toks_.size(), 0) {}

    Module parse_module_root() {
        Module m;
        std::vector<Decl*> decls;
        for (;;) {
            skip_terminators();
            if (raw().kind == K::Eof) break;
            std::size_t before = pos_;
            Decl* d = declaration(DeclContext::TopLevel);
            if (!panic_) expect_stmt_end();
            recover_decl(before);
            decls.push_back(d);
        }
        m.decls = arena_.list(decls);
        return m;
    }

    Script parse_script_root() {
        Script s;
        script_ = true;
        std::vector<Stmt*> stmts;
        for (;;) {
            skip_terminators();
            if (raw().kind == K::Eof) break;
            std::size_t before = pos_;
            Stmt* st = statement();
            if (!panic_) expect_stmt_end();
            recover_stmt(before);
            stmts.push_back(st);
        }
        s.stmts = arena_.list(stmts);
        return s;
    }

    Expr* parse_single_expression() {
        skip_terminators();
        Expr* e = parse_expr();
        if (!panic_) {
            skip_terminators();
            if (raw().kind != K::Eof) error_expected("end of expression");
        }
        return e;
    }

    // Parses the tokens of an interpolation `{...}`; they end with Eof.
    Expr* parse_interpolation() {
        NlGuard nl(*this, true);
        if (at(K::Eof)) {
            error(sp(cur()), "empty interpolation in string");
            return make<ErrorExpr>(sp(cur()));
        }
        Expr* e = parse_expr();
        if (!panic_ && !at(K::Eof)) error_expected("'}' to end the interpolation");
        // Lexer errors inside the interpolation are independent of the parse.
        for (std::size_t i = 0; i < toks_.size() && speculating_ == 0; ++i) {
            if (toks_[i].kind == K::Error) report_lex_error(i);
        }
        return e;
    }

private:
    // ------------------------------------------------------------ scopes

    // Newline sensitivity (LANGUAGE.md 1.3): inside ( ) and [ ] (and literal
    // braces) newlines are not terminators; inside blocks they are.
    // `opener` is the bracket token that opened the scope (Eof: none); error
    // recovery uses it to know which closers are still pending.
    struct NlGuard {
        Parser& p;
        NlGuard(Parser& parser, bool insensitive, K opener = K::Eof) : p(parser) {
            p.nl_.push_back({insensitive, opener});
        }
        ~NlGuard() { p.nl_.pop_back(); }
        NlGuard(const NlGuard&) = delete;
        NlGuard& operator=(const NlGuard&) = delete;
    };

    struct RestrictGuard {
        Parser& p;
        Restrictions saved;
        RestrictGuard(Parser& parser, Restrictions r) : p(parser), saved(parser.r_) { p.r_ = r; }
        ~RestrictGuard() { p.r_ = saved; }
        RestrictGuard(const RestrictGuard&) = delete;
        RestrictGuard& operator=(const RestrictGuard&) = delete;
    };

    // ------------------------------------------------------------ tokens

    bool insensitive() const { return !nl_.empty() && nl_.back().insensitive; }

    std::size_t skip_nl_from(std::size_t i) const {
        while (toks_[i].kind == K::Newline) ++i;
        return i;
    }

    std::size_t effective_pos() const { return insensitive() ? skip_nl_from(pos_) : pos_; }

    const Token& raw() const { return toks_[pos_]; }
    const Token& cur() const { return toks_[effective_pos()]; }

    // The n-th token after the current one, skipping newlines when they are
    // insignificant (or always, with `skip_nl`).
    const Token& peek(std::size_t n = 1, bool skip_nl = false) const {
        std::size_t i = effective_pos();
        for (std::size_t k = 0; k < n && toks_[i].kind != K::Eof; ++k) {
            ++i;
            if (skip_nl || insensitive()) i = skip_nl_from(i);
        }
        return toks_[i];
    }

    bool at(K k) const { return cur().kind == k; }
    bool at_kw(std::string_view w) const { return cur().is_keyword(w); }

    const Token& advance() {
        std::size_t i = effective_pos();
        const Token& t = toks_[i];
        if (t.kind != K::Eof) pos_ = i + 1;
        last_end_ = t.offset + t.length;
        return t;
    }

    bool accept(K k) {
        if (!at(k)) return false;
        advance();
        return true;
    }

    bool accept_kw(std::string_view w) {
        if (!at_kw(w)) return false;
        advance();
        return true;
    }

    bool expect(K k, std::string_view what) {
        if (accept(k)) return true;
        error_expected(what);
        return false;
    }

    void skip_nl() { pos_ = skip_nl_from(pos_); }

    void skip_terminators() {
        while (raw().kind == K::Newline || raw().kind == K::Semicolon) ++pos_;
    }

    static Span sp(const Token& t) { return {t.offset, t.offset + t.length}; }
    Span from(const Token& start) const { return {start.offset, std::max(last_end_, start.offset)}; }
    Span from(Span start) const { return {start.begin, std::max(last_end_, start.begin)}; }

    template <class T>
    T* make(Span s) {
        T* n = arena_.make<T>();
        n->kind = T::node_kind;
        n->span = s;
        return n;
    }

    Ident ident_of(const Token& t) const { return {t.text, sp(t)}; }

    // ------------------------------------------------------------ errors

    // A syntax error: reported, then the parser is in panic mode until the
    // next synchronization point, which suppresses cascading errors.
    void error(Span s, std::string message, std::optional<FixIt> fix = std::nullopt) {
        if (speculating_ > 0) {
            spec_failed_ = true;
            panic_ = true;
            return;
        }
        if (panic_) return;
        panic_ = true;
        panic_open_.clear();
        for (const Scope& scope : nl_) panic_open_.push_back(scope.opener);
        diags_.error(s, std::move(message)).fix = std::move(fix);
    }

    // An error that doesn't derail parsing (the structure is still clear).
    void report(Span s, std::string message, std::optional<FixIt> fix = std::nullopt) {
        if (speculating_ > 0) {
            spec_failed_ = true;
            return;
        }
        if (panic_) return;
        diags_.error(s, std::move(message)).fix = std::move(fix);
    }

    void error_expected(std::string_view what) {
        std::size_t i = effective_pos();
        const Token& t = toks_[i];
        if (t.kind == K::Error) {
            if (speculating_ == 0 && !panic_) lex_reported_[i] = 1;
            error(sp(t), std::string(t.text));
            return;
        }
        error(sp(t), std::format("expected {}, found {}", what, describe(t)));
    }

    void report_lex_error(std::size_t i) {
        if (lex_reported_[i] != 0) return;
        lex_reported_[i] = 1;
        diags_.error(sp(toks_[i]), std::string(toks_[i].text));
    }

    // Brackets that were open when the error happened, in scopes that have
    // since been unwound: their closers are still ahead in the token stream.
    std::vector<K> pending_closers() const {
        std::vector<K> stack;
        for (std::size_t i = nl_.size(); i < panic_open_.size(); ++i) {
            if (panic_open_[i] != K::Eof) stack.push_back(panic_open_[i]);
        }
        return stack;
    }

    static K opener_of(K closer) {
        return closer == K::RightParen ? K::LeftParen : closer == K::RightBracket ? K::LeftBracket : K::LeftBrace;
    }

    // Tracks brackets while skipping. Returns true when `t` is a `}` that
    // closes the enclosing body (stop without consuming it).
    static bool track_bracket(std::vector<K>& stack, K k) {
        if (is_opener(k)) {
            stack.push_back(k);
        } else if (is_closer(k)) {
            K open = opener_of(k);
            auto it = std::find(stack.rbegin(), stack.rend(), open);
            if (it == stack.rend()) return k == K::RightBrace; // stray ) or ] is skipped
            stack.erase(std::prev(it.base()), stack.end());
        }
        return false;
    }

    static bool only_parens(const std::vector<K>& stack) {
        return std::none_of(stack.begin(), stack.end(), [](K k) { return k == K::LeftBrace; });
    }

    // Skip to the end of the current statement: a newline or `;` outside any
    // brackets, or the `}` closing the enclosing block. An unclosed `(` or `[`
    // stops at the next line that starts at or left of the broken statement
    // (`column`), since bracketed continuation lines are indented.
    void sync_stmt(std::uint32_t column) {
        std::vector<K> stack = pending_closers();
        for (;;) {
            const Token& t = toks_[pos_];
            if (t.kind == K::Eof) break;
            if (t.kind == K::Error) report_lex_error(pos_);
            if (stack.empty()) {
                if (t.kind == K::Newline || t.kind == K::Semicolon) {
                    ++pos_;
                    break;
                }
                if (t.kind == K::RightBrace) break;
            } else if (t.kind == K::Newline && only_parens(stack) && toks_[pos_ + 1].kind != K::Eof &&
                       toks_[pos_ + 1].column <= column && !is_closer(toks_[pos_ + 1].kind)) {
                ++pos_;
                break;
            }
            if (track_bracket(stack, t.kind)) break;
            ++pos_;
        }
        panic_ = false;
    }

    // Skip to the next line that starts a declaration, or the `}` closing the
    // enclosing body. Whole broken bodies are skipped as a unit.
    // A line starting a declaration at or left of the broken one's `column`
    // also ends the skip, even inside unclosed brackets.
    void sync_decl(std::uint32_t column) {
        std::vector<K> stack = pending_closers();
        for (;;) {
            const Token& t = toks_[pos_];
            if (t.kind == K::Eof) break;
            if (t.kind == K::Error) report_lex_error(pos_);
            if (t.kind == K::Newline && is_decl_start(toks_[pos_ + 1]) && toks_[pos_ + 1].column <= column) {
                ++pos_;
                break;
            }
            if (stack.empty()) {
                if (t.kind == K::RightBrace) break;
                if (t.kind == K::Newline || t.kind == K::Semicolon) {
                    std::size_t n = pos_ + 1;
                    while (toks_[n].kind == K::Newline || toks_[n].kind == K::Semicolon) ++n;
                    if (is_decl_start(toks_[n]) || toks_[n].kind == K::RightBrace || toks_[n].kind == K::Eof) {
                        pos_ = n;
                        break;
                    }
                }
            }
            if (track_bracket(stack, t.kind)) break;
            ++pos_;
        }
        panic_ = false;
    }

    // After a statement / declaration starting at token `before`: on error,
    // resynchronize; always move forward, even on a stray token.
    void recover_stmt(std::size_t before) {
        bool failed = panic_;
        if (failed) sync_stmt(toks_[before].column);
        ensure_progress(before, failed);
    }

    void recover_decl(std::size_t before) {
        bool failed = panic_;
        if (failed) sync_decl(toks_[before].column);
        ensure_progress(before, failed);
    }

    void ensure_progress(std::size_t before, bool reported) {
        if (pos_ == before && raw().kind != K::Eof) {
            if (raw().kind == K::Error) {
                report_lex_error(pos_);
            } else if (!reported) {
                diags_.error(sp(raw()), std::format("unexpected {}", describe(raw())));
            }
            ++pos_;
        }
    }

    void expect_stmt_end() {
        const Token& t = raw();
        if (t.kind == K::Newline || t.kind == K::Semicolon) {
            skip_terminators();
            return;
        }
        if (t.kind == K::RightBrace || t.kind == K::Eof) return;
        error_expected("newline or ';' after statement");
    }

    Ident expect_ident(std::string_view what) {
        if (at(K::Identifier)) return ident_of(advance());
        error_expected(what);
        return {};
    }

    // ------------------------------------------------------------ declarations

    List<Attribute> attributes() {
        std::vector<Attribute> out;
        while (at(K::At)) {
            const Token& at_tok = advance();
            Attribute a;
            if (at(K::Identifier) || at(K::Keyword)) {
                a.name = ident_of(advance());
            } else {
                error_expected("attribute name after '@'");
                break;
            }
            if (raw().kind == K::LeftParen) {
                a.has_args = true;
                a.args = call_args();
            }
            a.span = from(at_tok);
            out.push_back(a);
            if (panic_) break;
            skip_nl();
        }
        return arena_.list(out);
    }

    Decl* declaration(DeclContext ctx) {
        const Token start = cur();
        List<Attribute> attrs = attributes();
        if (panic_) return make<ErrorDecl>(from(start));

        bool is_pub = false, is_static = false, is_final = false;
        for (;;) {
            const Token& t = cur();
            bool* flag = t.is_keyword("pub") ? &is_pub : t.is_keyword("static") ? &is_static
                                                  : t.is_keyword("final")    ? &is_final
                                                                              : nullptr;
            if (flag == nullptr) break;
            if (*flag) report(sp(t), std::format("duplicate '{}' modifier", t.text));
            *flag = true;
            advance();
        }

        const Token& kw = cur();
        Decl* d = nullptr;
        if (kw.is_keyword("var") || kw.is_keyword("weak")) {
            d = var_decl();
        } else if (kw.is_keyword("const")) {
            d = const_decl();
        } else if (kw.is_keyword("fn")) {
            d = fn_decl(ctx);
        } else if (kw.is_keyword("class") || kw.is_keyword("manual")) {
            d = class_decl();
        } else if (kw.is_keyword("struct")) {
            d = struct_decl();
        } else if (kw.is_keyword("enum")) {
            d = enum_decl();
        } else if (kw.is_keyword("trait")) {
            d = trait_decl();
        } else if (kw.is_keyword("signal")) {
            d = signal_decl();
        } else {
            error_expected("declaration");
            return make<ErrorDecl>(from(start));
        }
        d->attributes = attrs;
        d->is_pub = is_pub;
        d->is_static = is_static;
        d->is_final = is_final;
        d->span = from(start);
        validate_placement(d, ctx, kw);
        return d;
    }

    void validate_placement(const Decl* d, DeclContext ctx, const Token& kw) {
        bool member = ctx == DeclContext::Class || ctx == DeclContext::Struct || ctx == DeclContext::Trait;
        bool type_decl = d->kind == DeclKind::Class || d->kind == DeclKind::Struct || d->kind == DeclKind::Enum ||
                         d->kind == DeclKind::Trait;
        if (d->is_pub && ctx != DeclContext::TopLevel) {
            report(d->span, "'pub' is only allowed on top-level declarations");
        }
        if (d->is_static && ctx != DeclContext::Class && ctx != DeclContext::Struct) {
            report(d->span, "'static' is only allowed on class and struct members");
        }
        if (d->is_final && ctx != DeclContext::Class) {
            report(d->span, "'final' is only allowed on class members");
        }
        if (d->is_static && d->is_final) {
            report(d->span, "a member cannot be both 'static' and 'final'");
        }
        if (type_decl && ctx != DeclContext::TopLevel) {
            report(sp(kw), std::format("'{}' declarations are only allowed at the top level",
                                       kw.is_keyword("manual") ? "class" : kw.text));
        }
        if (d->kind == DeclKind::Signal && ctx != DeclContext::Class) {
            report(sp(kw), "signals can only be declared in a class");
        }
        if (ctx == DeclContext::Trait && d->kind != DeclKind::Fn) {
            report(sp(kw), "traits can only contain functions");
        }
        if (d->kind == DeclKind::Fn && ctx == DeclContext::Local) {
            report(sp(kw), "functions cannot be declared inside a block; use a lambda");
        }
        if (auto v = as<VarDecl>(d); v != nullptr && v->has_accessors && !member) {
            report(d->span, "accessors are only allowed on fields");
        }
    }

    VarDecl* var_decl() {
        const Token start = cur();
        auto* v = make<VarDecl>(sp(start));
        v->weak = accept_kw("weak");
        if (!accept_kw("var")) {
            error_expected("'var'");
            return v;
        }
        v->name = expect_ident("variable name");
        if (panic_) return v;
        if (accept(K::Colon)) v->type = parse_type();
        if (!panic_ && accept(K::Equal)) v->init = parse_expr();
        // Accessors: `{` on the same line (LANGUAGE.md 2.5).
        if (!panic_ && raw().kind == K::LeftBrace) accessors(v);
        v->span = from(start);
        return v;
    }

    void accessors(VarDecl* v) {
        advance(); // {
        v->has_accessors = true;
        NlGuard nl(*this, false, K::LeftBrace);
        for (;;) {
            skip_terminators();
            if (at(K::RightBrace) || at(K::Eof)) break;
            const Token& t = cur();
            if (t.is_keyword("get")) {
                advance();
                if (v->getter != nullptr) report(sp(t), "duplicate getter");
                v->getter = block();
            } else if (t.is_keyword("set")) {
                advance();
                if (v->setter != nullptr) report(sp(t), "duplicate setter");
                if (!expect(K::LeftParen, "'(' after 'set'")) return;
                v->setter_param = expect_ident("setter parameter name");
                if (!expect(K::RightParen, "')'")) return;
                v->setter = block();
            } else {
                error_expected("'get' or 'set'");
                return;
            }
            if (panic_) return;
        }
        expect(K::RightBrace, "'}' to close the accessors");
    }

    ConstDecl* const_decl() {
        const Token start = advance(); // const
        auto* c = make<ConstDecl>(sp(start));
        c->name = expect_ident("constant name");
        if (panic_) return c;
        if (accept(K::Colon)) c->type = parse_type();
        if (panic_) return c;
        if (!at(K::Equal)) {
            error_expected(std::format("'=' and a value for constant '{}'", c->name.name));
            return c;
        }
        advance();
        c->value = parse_expr();
        return c;
    }

    enum class ParamRules : std::uint8_t { Function, Lambda, Signal, Variant };

    List<Param> param_list(ParamRules rules) {
        // The caller has checked for '('.
        advance();
        std::vector<Param> params;
        {
            NlGuard nl(*this, true, K::LeftParen);
            RestrictGuard rg(*this, {});
            bool seen_default = false;
            while (!at(K::RightParen) && !at(K::Eof)) {
                Param p;
                const Token& name = cur();
                p.name = expect_ident("parameter name");
                if (panic_) break;
                if (accept(K::Colon)) p.type = parse_type();
                if (!panic_ && accept(K::Equal)) p.default_value = parse_expr();
                if (panic_) break;
                p.span = from(name);
                if (p.type == nullptr && p.default_value == nullptr &&
                    (rules == ParamRules::Function || rules == ParamRules::Signal || rules == ParamRules::Variant)) {
                    report(p.span, std::format("parameter '{}' needs a type{}", p.name.name,
                                               rules == ParamRules::Function ? " or a default value" : ""));
                }
                if (p.default_value != nullptr && rules == ParamRules::Signal) {
                    report(p.span, "signal parameters cannot have default values");
                }
                if (p.default_value != nullptr) {
                    seen_default = true;
                } else if (seen_default) {
                    report(p.span, std::format("required parameter '{}' follows a parameter with a default value",
                                               p.name.name));
                }
                params.push_back(p);
                if (!accept(K::Comma)) break;
            }
            if (!panic_) expect(K::RightParen, "',' or ')' in parameter list");
        }
        return arena_.list(params);
    }

    List<GenericParam> generic_params() {
        std::vector<GenericParam> out;
        if (!at(K::Less)) return {};
        advance();
        while (!at(K::Greater) && !at(K::Eof)) {
            GenericParam g;
            const Token& name = cur();
            g.name = expect_ident("generic parameter name");
            if (panic_) break;
            if (accept(K::Colon)) {
                std::vector<TypeExpr*> bounds;
                do {
                    bounds.push_back(parse_type());
                } while (!panic_ && accept(K::Plus));
                g.bounds = arena_.list(bounds);
            }
            g.span = from(name);
            out.push_back(g);
            if (panic_ || !accept(K::Comma)) break;
        }
        if (!panic_) expect(K::Greater, "',' or '>' in generic parameters");
        return arena_.list(out);
    }

    List<TypeExpr*> type_list() {
        std::vector<TypeExpr*> out;
        do {
            out.push_back(parse_type());
        } while (!panic_ && accept(K::Comma));
        return arena_.list(out);
    }

    FnDecl* fn_decl(DeclContext ctx) {
        const Token start = advance(); // fn
        auto* f = make<FnDecl>(sp(start));
        f->name = expect_ident("function name");
        if (panic_) return f;
        f->generics = generic_params();
        if (panic_) return f;
        if (!at(K::LeftParen)) {
            error_expected("'(' after the function name");
            return f;
        }
        f->params = param_list(ParamRules::Function);
        if (panic_) return f;
        if (accept(K::Arrow)) f->ret = parse_type();
        if (panic_) return f;
        // A body may start after newlines; a trait signature has none.
        bool has_body = raw().kind == K::LeftBrace ||
                        (raw().kind == K::Newline && toks_[skip_nl_from(pos_)].kind == K::LeftBrace);
        if (has_body) {
            suspend_targets_.push_back(&f->suspends);
            f->body = block();
            suspend_targets_.pop_back();
        } else if (ctx != DeclContext::Trait) {
            error_expected(std::format("'{{' to start the body of '{}'", f->name.name));
        }
        return f;
    }

    // Parses `{ members }` for class, struct and trait bodies.
    List<Decl*> member_body(DeclContext ctx) {
        skip_nl();
        if (!expect(K::LeftBrace, "'{'")) return {};
        std::vector<Decl*> members;
        NlGuard nl(*this, false, K::LeftBrace);
        for (;;) {
            skip_terminators();
            if (raw().kind == K::RightBrace || raw().kind == K::Eof) break;
            std::size_t before = pos_;
            Decl* d = declaration(ctx);
            if (!panic_) expect_stmt_end();
            recover_decl(before);
            members.push_back(d);
        }
        expect(K::RightBrace, "'}' to close the body");
        return arena_.list(members);
    }

    ClassDecl* class_decl() {
        const Token start = cur();
        auto* c = make<ClassDecl>(sp(start));
        c->manual = accept_kw("manual");
        if (!accept_kw("class")) {
            error_expected("'class' after 'manual'");
            return c;
        }
        c->name = expect_ident("class name");
        if (panic_) return c;
        c->generics = generic_params();
        if (!panic_ && accept_kw("extends")) c->extends = parse_type();
        if (!panic_ && accept(K::Colon)) c->traits = type_list();
        if (panic_) return c;
        c->members = member_body(DeclContext::Class);
        return c;
    }

    StructDecl* struct_decl() {
        const Token start = advance(); // struct
        auto* s = make<StructDecl>(sp(start));
        s->name = expect_ident("struct name");
        if (panic_) return s;
        s->generics = generic_params();
        if (!panic_ && at_kw("extends")) {
            report(sp(cur()), "structs cannot inherit; use a class, or implement traits with ':'");
            advance();
            parse_type();
        }
        if (!panic_ && accept(K::Colon)) s->traits = type_list();
        if (panic_) return s;
        s->members = member_body(DeclContext::Struct);
        return s;
    }

    TraitDecl* trait_decl() {
        const Token start = advance(); // trait
        auto* t = make<TraitDecl>(sp(start));
        t->name = expect_ident("trait name");
        if (panic_) return t;
        t->generics = generic_params();
        if (!panic_ && accept(K::Colon)) t->supertraits = type_list();
        if (panic_) return t;
        t->members = member_body(DeclContext::Trait);
        return t;
    }

    EnumDecl* enum_decl() {
        const Token start = advance(); // enum
        auto* e = make<EnumDecl>(sp(start));
        e->name = expect_ident("enum name");
        if (panic_) return e;
        if (accept(K::Colon)) e->backing = parse_type();
        if (panic_) return e;
        skip_nl();
        if (!expect(K::LeftBrace, "'{'")) return e;
        std::vector<EnumVariant> variants;
        {
            // Variants are comma-separated; newlines inside the braces don't matter.
            NlGuard nl(*this, true, K::LeftBrace);
            while (!at(K::RightBrace) && !at(K::Eof)) {
                EnumVariant v;
                const Token& name = cur();
                v.name = expect_ident("enum variant name");
                if (panic_) break;
                if (at(K::LeftParen)) {
                    v.has_params = true;
                    v.params = param_list(ParamRules::Variant);
                }
                if (!panic_ && accept(K::Equal)) v.value = parse_expr();
                if (panic_) break;
                v.span = from(name);
                variants.push_back(v);
                if (accept(K::Comma)) continue;
                if (!at(K::RightBrace)) {
                    if (at(K::Identifier)) {
                        // Common slip: one variant per line, comma forgotten.
                        report(sp(cur()), "expected ',' between enum variants",
                               FixIt{{v.span.end, v.span.end}, ","});
                        continue;
                    }
                    error_expected("',' or '}' after enum variant");
                }
                break;
            }
            if (!panic_) expect(K::RightBrace, "'}' to close the enum");
        }
        e->variants = arena_.list(variants);
        return e;
    }

    SignalDecl* signal_decl() {
        const Token start = advance(); // signal
        auto* s = make<SignalDecl>(sp(start));
        s->name = expect_ident("signal name");
        if (!panic_ && raw().kind == K::LeftParen) {
            s->has_params = true;
            s->params = param_list(ParamRules::Signal);
        }
        return s;
    }

    // ------------------------------------------------------------ types

    TypeExpr* parse_type() {
        const Token start = cur();
        TypeExpr* out = nullptr;
        if (start.kind == K::Identifier) {
            advance();
            auto* n = make<NamedType>(sp(start));
            n->name = ident_of(start);
            if (at(K::Less)) n->args = type_args();
            out = n;
        } else if (start.kind == K::LeftBracket) {
            advance();
            auto* a = make<ArrayType>(sp(start));
            NlGuard nl(*this, true, K::LeftBracket);
            RestrictGuard rg(*this, {});
            a->element = parse_type();
            if (!panic_ && expect(K::Semicolon, "';' in array type '[T; N]'")) {
                a->size = parse_expr();
                if (!panic_) expect(K::RightBracket, "']'");
            }
            out = a;
        } else if (start.is_keyword("fn")) {
            advance();
            auto* f = make<FnType>(sp(start));
            if (expect(K::LeftParen, "'(' in function type")) {
                NlGuard nl(*this, true, K::LeftParen);
                std::vector<TypeExpr*> params;
                while (!at(K::RightParen) && !at(K::Eof) && !panic_) {
                    params.push_back(parse_type());
                    if (!accept(K::Comma)) break;
                }
                f->params = arena_.list(params);
                if (!panic_) expect(K::RightParen, "',' or ')' in function type");
            }
            if (!panic_ && accept(K::Arrow)) f->ret = parse_type();
            out = f;
        } else {
            error_expected("type");
            return make<ErrorType>(sp(start));
        }
        out->span = from(start);
        while (raw().kind == K::Question && !raw().space_before) {
            advance();
            auto* n = make<NullableType>(from(start));
            n->inner = out;
            out = n;
        }
        return out;
    }

    List<TypeExpr*> type_args() {
        advance(); // <
        std::vector<TypeExpr*> args;
        while (!panic_) {
            args.push_back(parse_type());
            if (!accept(K::Comma)) break;
        }
        if (!panic_) expect(K::Greater, "',' or '>' in type arguments");
        return arena_.list(args);
    }

    // `name<T, U>` in an expression: only a generic when the `>` is followed by
    // `(` (a call) or a struct literal; otherwise `<` is less-than.
    bool try_expr_type_args(List<TypeExpr*>& out) {
        std::size_t save_pos = pos_;
        std::uint32_t save_end = last_end_;
        bool save_panic = panic_;
        bool save_failed = spec_failed_;
        ++speculating_;
        spec_failed_ = false;
        List<TypeExpr*> args = type_args();
        bool ok = !spec_failed_ && (raw().kind == K::LeftParen ||
                                    (raw().kind == K::LeftBrace && !r_.no_struct && struct_lit_ahead()));
        --speculating_;
        spec_failed_ = save_failed;
        panic_ = save_panic;
        if (!ok) {
            pos_ = save_pos;
            last_end_ = save_end;
            return false;
        }
        out = args;
        return true;
    }

    // ------------------------------------------------------------ statements

    BlockStmt* block() {
        skip_nl();
        const Token start = cur();
        auto* b = make<BlockStmt>(sp(start));
        if (!expect(K::LeftBrace, "'{'")) return b;
        std::vector<Stmt*> stmts;
        {
            NlGuard nl(*this, false, K::LeftBrace);
            RestrictGuard rg(*this, {});
            ++block_depth_;
            for (;;) {
                skip_terminators();
                if (raw().kind == K::RightBrace || raw().kind == K::Eof) break;
                std::size_t before = pos_;
                Stmt* s = statement();
                if (!panic_) expect_stmt_end();
                recover_stmt(before);
                stmts.push_back(s);
            }
            --block_depth_;
            if (raw().kind == K::Eof) {
                error(sp(raw()), "expected '}' to close the block, found end of file");
            } else {
                advance();
            }
        }
        b->stmts = arena_.list(stmts);
        b->span = from(start);
        return b;
    }

    bool script_top() const { return script_ && block_depth_ == 0; }

    Stmt* statement() {
        const Token start = cur();
        Stmt* s = statement_inner();
        if (s->span.end <= s->span.begin) s->span = from(start);
        return s;
    }

    Stmt* decl_stmt(Decl* d) {
        auto* s = make<DeclStmt>(d->span);
        s->decl = d;
        return s;
    }

    Stmt* statement_inner() {
        const Token& t = cur();
        if (t.kind == K::At) {
            // Attributes before `foreach`, or before a declaration in script mode.
            std::size_t save = pos_;
            const Token start = t;
            List<Attribute> attrs = attributes();
            if (panic_) return make<ErrorStmt>(from(start));
            if (at_kw("foreach")) return foreach_stmt(attrs, start);
            if (script_top() && is_decl_start(cur())) {
                pos_ = save;
                return decl_stmt(declaration(DeclContext::TopLevel));
            }
            error_expected("'foreach' or a declaration after attributes");
            return make<ErrorStmt>(from(start));
        }
        if (t.kind == K::Keyword) {
            std::string_view w = t.text;
            if (w == "var" || w == "weak" || w == "const") {
                return decl_stmt(declaration(script_top() ? DeclContext::TopLevel : DeclContext::Local));
            }
            if (w == "fn" && peek().kind == K::Identifier) {
                return decl_stmt(declaration(script_top() ? DeclContext::TopLevel : DeclContext::Local));
            }
            if (w == "class" || w == "struct" || w == "enum" || w == "trait" || w == "manual" || w == "pub" ||
                w == "static" || w == "final" || w == "signal") {
                return decl_stmt(declaration(script_top() ? DeclContext::TopLevel : DeclContext::Local));
            }
            if (w == "if") return if_stmt();
            if (w == "match") return match_stmt();
            if (w == "for") return for_stmt({});
            if (w == "foreach") return foreach_stmt({}, t);
            if (w == "while") return while_stmt({});
            if (w == "loop") return loop_stmt({});
            if (w == "return") return return_stmt();
            if (w == "break" || w == "continue") return jump_stmt();
            if (w == "defer") return defer_stmt();
            if (w == "unsafe") return unsafe_stmt();
            if (w == "yield") {
                mark_suspends();
                return make<YieldStmt>(sp(advance()));
            }
            if (w == "emit") return emit_stmt();
        }
        if (t.kind == K::Identifier && peek().kind == K::Colon) {
            const Token& kw = peek(2);
            if (kw.is_keyword("for") || kw.is_keyword("while") || kw.is_keyword("loop")) {
                Ident label = ident_of(advance());
                advance(); // :
                if (kw.is_keyword("for")) return for_stmt(label);
                if (kw.is_keyword("while")) return while_stmt(label);
                return loop_stmt(label);
            }
            if (kw.kind == K::Keyword && (kw.is_keyword("foreach") || kw.is_keyword("if") || kw.is_keyword("match"))) {
                error(sp(t), "only 'for', 'while' and 'loop' can have a label");
                return make<ErrorStmt>(sp(t));
            }
        }
        if (t.kind == K::LeftBrace) return block();
        return expr_or_assign_stmt();
    }

    static bool is_lvalue(const Expr* e) {
        if (e->kind == ExprKind::Identifier) return as<IdentExpr>(e)->type_args.empty();
        if (e->kind == ExprKind::Index) return true;
        if (e->kind == ExprKind::Member) return !as<MemberExpr>(e)->safe;
        return false;
    }

    std::optional<AssignOp> assign_op_at(int& token_count) const {
        const Token& t = raw();
        token_count = 1;
        switch (t.kind) {
            case K::Equal: return AssignOp::Set;
            case K::PlusEqual: return AssignOp::Add;
            case K::MinusEqual: return AssignOp::Sub;
            case K::StarEqual: return AssignOp::Mul;
            case K::SlashEqual: return AssignOp::Div;
            case K::PercentEqual: return AssignOp::Rem;
            case K::AmpEqual: return AssignOp::BitAnd;
            case K::PipeEqual: return AssignOp::BitOr;
            case K::CaretEqual: return AssignOp::BitXor;
            case K::LessLessEqual: return AssignOp::Shl;
            case K::Greater:
                if (toks_[pos_ + 1].kind == K::GreaterEqual && !toks_[pos_ + 1].space_before) {
                    token_count = 2;
                    return AssignOp::Shr;
                }
                return std::nullopt;
            default: return std::nullopt;
        }
    }

    Stmt* expr_or_assign_stmt() {
        const Token start = cur();
        Expr* e = parse_expr();
        if (panic_) {
            auto* s = make<ExprStmt>(from(start));
            s->expr = e;
            return s;
        }
        int count = 0;
        if (std::optional<AssignOp> op = assign_op_at(count)) {
            if (!is_lvalue(e)) report(e->span, "invalid assignment target");
            for (int i = 0; i < count; ++i) advance();
            auto* a = make<AssignStmt>(sp(start));
            a->op = *op;
            a->target = e;
            a->value = parse_expr();
            a->span = from(start);
            return a;
        }
        auto* s = make<ExprStmt>(from(start));
        s->expr = e;
        return s;
    }

    // A loop / if / match header expression: no struct literals (2.6).
    Expr* header_expr(bool if_binding = false) {
        RestrictGuard rg(*this, {.no_struct = true, .no_lambda = false, .if_binding = if_binding});
        Expr* e = parse_expr();
        if (!panic_ && raw().kind == K::Equal) {
            const Token& eq = raw();
            error(sp(eq), "assignment is not allowed in a condition; did you mean '=='?", FixIt{sp(eq), "=="});
        }
        return e;
    }

    Stmt* if_stmt() {
        const Token start = advance(); // if
        auto* s = make<IfStmt>(sp(start));
        std::vector<IfBranch> branches;
        Token branch_start = start;
        for (;;) {
            IfBranch b;
            b.cond = header_expr(true);
            if (!panic_ && raw().kind == K::Question) {
                // `if target? t { ... }`: unwrap a nullable (LANGUAGE.md 3).
                advance();
                b.unwrap = expect_ident("name to bind after '?'");
            }
            if (!panic_) b.body = block();
            b.span = from(branch_start);
            branches.push_back(b);
            if (panic_) break;
            // `else` may follow on the next line.
            std::size_t n = skip_nl_from(pos_);
            if (!toks_[n].is_keyword("else")) break;
            pos_ = n;
            advance();
            if (at_kw("if")) {
                branch_start = advance();
                continue;
            }
            s->else_body = block();
            break;
        }
        s->branches = arena_.list(branches);
        s->span = from(start);
        return s;
    }

    Stmt* match_stmt() {
        const Token start = advance(); // match
        auto* m = make<MatchStmt>(sp(start));
        m->subject = header_expr();
        if (panic_) return m;
        skip_nl();
        if (!expect(K::LeftBrace, "'{' after the match subject")) return m;
        std::vector<MatchArm> arms;
        {
            NlGuard nl(*this, false, K::LeftBrace);
            RestrictGuard rg(*this, {});
            ++block_depth_;
            for (;;) {
                skip_terminators();
                if (raw().kind == K::RightBrace || raw().kind == K::Eof) break;
                std::size_t before = pos_;
                arms.push_back(match_arm());
                if (!panic_) {
                    accept(K::Comma);
                    expect_stmt_end();
                }
                recover_stmt(before);
            }
            --block_depth_;
            if (raw().kind == K::Eof) {
                error(sp(raw()), "expected '}' to close the match, found end of file");
            } else {
                advance();
            }
        }
        m->arms = arena_.list(arms);
        m->span = from(start);
        return m;
    }

    MatchArm match_arm() {
        const Token start = cur();
        MatchArm arm;
        arm.pattern = parse_pattern(true);
        if (!panic_ && accept_kw("if")) {
            RestrictGuard rg(*this, {.no_struct = false, .no_lambda = true, .if_binding = false});
            arm.guard = parse_expr();
        }
        if (!panic_ && expect(K::FatArrow, "'=>' after the match pattern")) {
            skip_nl();
            arm.body = raw().kind == K::LeftBrace ? block() : statement();
        }
        if (arm.body == nullptr) arm.body = make<ErrorStmt>(from(start));
        arm.span = from(start);
        return arm;
    }

    Stmt* for_stmt(Ident label) {
        const Token start = advance(); // for
        auto* f = make<ForStmt>(sp(start));
        f->label = label;
        f->first = expect_ident("loop variable name");
        if (!panic_ && accept(K::Comma)) f->second = expect_ident("loop variable name");
        if (!panic_ && !accept_kw("in")) error_expected("'in'");
        if (!panic_) f->iterable = header_expr();
        if (!panic_) f->body = block();
        f->span = from(label.empty() ? sp(start) : label.span);
        return f;
    }

    Stmt* foreach_stmt(List<Attribute> attrs, const Token& start) {
        advance(); // foreach
        auto* f = make<ForeachStmt>(sp(start));
        f->attributes = attrs;
        f->var = expect_ident("element name");
        if (!panic_ && !accept_kw("in")) error_expected("'in'");
        if (!panic_) f->pool = header_expr();
        if (!panic_) f->body = block();
        f->span = from(start);
        return f;
    }

    Stmt* while_stmt(Ident label) {
        const Token start = advance(); // while
        auto* w = make<WhileStmt>(sp(start));
        w->label = label;
        w->cond = header_expr();
        if (!panic_) w->body = block();
        w->span = from(label.empty() ? sp(start) : label.span);
        return w;
    }

    Stmt* loop_stmt(Ident label) {
        const Token start = advance(); // loop
        auto* l = make<LoopStmt>(sp(start));
        l->label = label;
        l->body = block();
        l->span = from(label.empty() ? sp(start) : label.span);
        return l;
    }

    bool at_stmt_end() const {
        K k = raw().kind;
        return k == K::Newline || k == K::Semicolon || k == K::RightBrace || k == K::Eof;
    }

    Stmt* return_stmt() {
        const Token start = advance();
        auto* r = make<ReturnStmt>(sp(start));
        if (!at_stmt_end()) r->value = parse_expr();
        r->span = from(start);
        return r;
    }

    Stmt* jump_stmt() {
        const Token start = advance();
        Ident label;
        if (raw().kind == K::Identifier) label = ident_of(advance());
        if (start.is_keyword("break")) {
            auto* b = make<BreakStmt>(from(start));
            b->label = label;
            return b;
        }
        auto* c = make<ContinueStmt>(from(start));
        c->label = label;
        return c;
    }

    Stmt* defer_stmt() {
        const Token start = advance();
        auto* d = make<DeferStmt>(sp(start));
        if (at_stmt_end()) {
            error_expected("statement or block after 'defer'");
            return d;
        }
        d->body = statement();
        d->span = from(start);
        return d;
    }

    Stmt* unsafe_stmt() {
        const Token start = advance();
        auto* u = make<UnsafeStmt>(sp(start));
        u->body = block();
        u->span = from(start);
        return u;
    }

    Stmt* emit_stmt() {
        const Token start = advance();
        auto* e = make<EmitStmt>(sp(start));
        e->signal = expect_ident("signal name after 'emit'");
        if (!panic_ && raw().kind == K::LeftParen) {
            e->has_args = true;
            e->args = call_args();
        }
        e->span = from(start);
        return e;
    }

    // ------------------------------------------------------------ patterns

    // `allow_or`: match arms take `a | b`; `is` patterns don't, because `|`
    // there is the bitwise-or operator.
    Pattern* parse_pattern(bool allow_or) {
        const Token start = cur();
        Pattern* first = pattern_single();
        if (!allow_or || !at(K::Pipe)) return first;
        std::vector<Pattern*> alts{first};
        while (!panic_ && accept(K::Pipe)) {
            skip_nl();
            alts.push_back(pattern_single());
        }
        auto* o = make<OrPattern>(from(start));
        o->alternatives = arena_.list(alts);
        return o;
    }

    bool at_literal_pattern_start() const {
        const Token& t = cur();
        switch (t.kind) {
            case K::Int:
            case K::Float:
            case K::String:
            case K::Name: return true;
            case K::Minus: return peek().kind == K::Int || peek().kind == K::Float;
            case K::Keyword: return t.is_keyword("true") || t.is_keyword("false") || t.is_keyword("null");
            default: return false;
        }
    }

    Expr* literal_pattern_value() {
        const Token start = cur();
        if (at(K::Minus)) {
            advance();
            auto* u = make<UnaryExpr>(sp(start));
            u->op = UnaryOp::Neg;
            u->operand = primary();
            u->span = from(start);
            return u;
        }
        Expr* e = primary();
        if (auto s = as<StringLit>(e); s != nullptr) {
            for (const StringPart& part : s->parts) {
                if (part.expr != nullptr) {
                    report(e->span, "interpolated strings are not allowed in patterns");
                    break;
                }
            }
        }
        return e;
    }

    Pattern* pattern_single() {
        const Token start = cur();
        if (start.kind == K::Identifier && start.text == "_") {
            advance();
            return make<WildcardPattern>(sp(start));
        }
        if (at_literal_pattern_start()) {
            Expr* lo = literal_pattern_value();
            if (!panic_ && (at(K::DotDot) || at(K::DotDotEqual))) {
                auto* r = make<RangePattern>(sp(start));
                r->inclusive = advance().kind == K::DotDotEqual;
                r->lo = lo;
                if (at_literal_pattern_start()) {
                    r->hi = literal_pattern_value();
                } else {
                    error_expected("literal after '..' in range pattern");
                }
                r->span = from(start);
                return r;
            }
            auto* l = make<LiteralPattern>(from(start));
            l->value = lo;
            return l;
        }
        if (start.kind == K::Identifier) {
            std::vector<Ident> path{ident_of(advance())};
            while (at(K::Dot)) {
                advance();
                if (at(K::Identifier) || at(K::Keyword)) {
                    path.push_back(ident_of(advance()));
                } else {
                    error_expected("name after '.' in pattern");
                    break;
                }
            }
            auto* p = make<PathPattern>(sp(start));
            p->path = arena_.list(path);
            if (!panic_ && raw().kind == K::LeftParen) {
                advance();
                p->has_args = true;
                std::vector<Pattern*> args;
                {
                    NlGuard nl(*this, true, K::LeftParen);
                    while (!at(K::RightParen) && !at(K::Eof) && !panic_) {
                        args.push_back(parse_pattern(true));
                        if (!accept(K::Comma)) break;
                    }
                    if (!panic_) expect(K::RightParen, "',' or ')' in pattern");
                }
                p->args = arena_.list(args);
            }
            p->span = from(start);
            return p;
        }
        error_expected("pattern");
        return make<ErrorPattern>(sp(start));
    }

    // ------------------------------------------------------------ expressions

    Expr* parse_expr() { return expr_prec(Prec::Lowest); }

    // Operands come after an operator, `(`, `,`, `=`, ... so a newline before
    // one never ends the statement (LANGUAGE.md 1.3).
    Expr* expr_prec(Prec min) {
        skip_nl();
        if (min <= Prec::Lambda && !r_.no_lambda && arrow_lambda_ahead()) return arrow_lambda();
        Expr* lhs = unary();
        return binary_loop(lhs, min);
    }

    enum class OpClass : std::uint8_t { None, Binary, Range, Is, As, Ternary };
    struct OpInfo {
        OpClass cls = OpClass::None;
        Prec prec = Prec::Lowest;
        BinaryOp op = BinaryOp::Add;
        int tokens = 1;
    };

    // `a?b:c`: a no-space `?` directly followed by something that starts an
    // operand is a mis-spaced ternary, not a postfix `?` (LANGUAGE.md 1.6).
    bool misspaced_ternary_at(std::size_t i) const {
        const Token& q = toks_[i];
        if (q.kind != K::Question || q.space_before) return false;
        const Token& n = toks_[i + 1];
        switch (n.kind) {
            case K::Identifier:
            case K::Int:
            case K::Float:
            case K::String:
            case K::Name:
            case K::Bang:
            case K::Tilde: return true;
            case K::Keyword:
                for (std::string_view w : {"true", "false", "null", "self", "super", "fn", "new", "alloc", "await",
                                           "start", "free"}) {
                    if (n.text == w) return true;
                }
                return false;
            default: return false;
        }
    }

    // `if x? t {`
    bool if_binding_at(std::size_t i) const {
        return r_.if_binding && toks_[i].kind == K::Question && !toks_[i].space_before &&
               toks_[i + 1].kind == K::Identifier && toks_[i + 2].kind == K::LeftBrace;
    }

    OpInfo op_info() const {
        std::size_t i = effective_pos();
        const Token& t = toks_[i];
        auto bin = [](BinaryOp op) { return OpInfo{OpClass::Binary, precedence(op), op, 1}; };
        switch (t.kind) {
            case K::Star: return bin(BinaryOp::Mul);
            case K::Slash: return bin(BinaryOp::Div);
            case K::Percent: return bin(BinaryOp::Rem);
            case K::Plus: return bin(BinaryOp::Add);
            case K::Minus: return bin(BinaryOp::Sub);
            case K::LessLess: return bin(BinaryOp::Shl);
            case K::Amp: return bin(BinaryOp::BitAnd);
            case K::Caret: return bin(BinaryOp::BitXor);
            case K::Pipe: return bin(BinaryOp::BitOr);
            case K::Less: return bin(BinaryOp::Lt);
            case K::LessEqual: return bin(BinaryOp::Le);
            case K::GreaterEqual: return bin(BinaryOp::Ge);
            case K::EqualEqual: return bin(BinaryOp::Eq);
            case K::BangEqual: return bin(BinaryOp::Ne);
            case K::AmpAmp: return bin(BinaryOp::And);
            case K::PipePipe: return bin(BinaryOp::Or);
            case K::QuestionQuestion: return bin(BinaryOp::Coalesce);
            case K::Greater: {
                // `>>` and `>>=` arrive as two adjacent tokens (token.hpp).
                const Token& n = toks_[i + 1];
                if (n.kind == K::Greater && !n.space_before) return {OpClass::Binary, Prec::Shift, BinaryOp::Shr, 2};
                if (n.kind == K::GreaterEqual && !n.space_before) return {};
                return bin(BinaryOp::Gt);
            }
            case K::DotDot:
            case K::DotDotEqual: return {OpClass::Range, Prec::Range, BinaryOp::Add, 1};
            case K::Question:
                if (if_binding_at(i)) return {};
                if (t.space_before || misspaced_ternary_at(i)) return {OpClass::Ternary, Prec::Ternary, BinaryOp::Add, 1};
                return {};
            case K::Keyword:
                if (t.text == "is") return {OpClass::Is, Prec::Comparison, BinaryOp::Add, 1};
                if (t.text == "as") return {OpClass::As, Prec::As, BinaryOp::Add, 1};
                return {};
            default: return {};
        }
    }

    static bool non_associative(Prec p) { return p == Prec::Comparison || p == Prec::Equality || p == Prec::Range; }

    Expr* binary_loop(Expr* lhs, Prec min) {
        Prec chained = Prec::Lowest; // level of the non-associative operator just built
        while (!panic_) {
            OpInfo info = op_info();
            if (info.cls == OpClass::None || info.prec < min) break;
            const Token op_tok = cur();
            Span op_span = sp(op_tok);
            if (info.tokens == 2) op_span.end = toks_[effective_pos() + 1].offset + 1;

            if (non_associative(info.prec) && chained == info.prec) {
                std::optional<FixIt> fix;
                auto* prev = as<BinaryExpr>(lhs);
                if (info.prec == Prec::Comparison && prev != nullptr && info.cls == OpClass::Binary) {
                    // `a < b < c` -> `a < b && b < c`
                    std::size_t save = pos_;
                    std::uint32_t save_end = last_end_;
                    for (int i = 0; i < info.tokens; ++i) advance();
                    ++speculating_;
                    Expr* rhs = expr_prec(next_prec(info.prec));
                    bool ok = !spec_failed_;
                    --speculating_;
                    spec_failed_ = false;
                    panic_ = false;
                    pos_ = save;
                    last_end_ = save_end;
                    if (ok) {
                        fix = FixIt{Span{lhs->span.begin, rhs->span.end},
                                    std::format("{} && {} {} {}", src_.slice(lhs->span), src_.slice(prev->rhs->span),
                                                op_tok.text, src_.slice(rhs->span))};
                    }
                }
                std::string_view what = info.prec == Prec::Range ? "range operators" : "comparison operators";
                report(op_span, std::format("{} cannot be chained", what), fix);
            }

            for (int i = 0; i < info.tokens; ++i) advance();
            Span start{lhs->span.begin, lhs->span.end};
            switch (info.cls) {
                case OpClass::Binary: {
                    // `??` is right-associative; the rest are left (or non-) associative.
                    Prec rhs_min = info.op == BinaryOp::Coalesce ? info.prec : next_prec(info.prec);
                    auto* b = make<BinaryExpr>(start);
                    b->op = info.op;
                    b->lhs = lhs;
                    b->rhs = expr_prec(rhs_min);
                    b->span = from(start);
                    lhs = b;
                    break;
                }
                case OpClass::Range: {
                    auto* r = make<RangeExpr>(start);
                    r->inclusive = op_tok.kind == K::DotDotEqual;
                    r->lo = lhs;
                    r->hi = expr_prec(next_prec(Prec::Range));
                    // `step` is contextual: `f(range = 0..1, step = 0.1)` names an argument.
                    if (!panic_ && at_kw("step") && peek().kind != K::Equal) {
                        advance();
                        r->step = expr_prec(next_prec(Prec::Range));
                    }
                    r->span = from(start);
                    lhs = r;
                    break;
                }
                case OpClass::Is: {
                    auto* is = make<IsExpr>(start);
                    is->operand = lhs;
                    skip_nl();
                    is->pattern = parse_pattern(false);
                    // `x is Boss b`: a bare type path can bind the narrowed value.
                    auto* path = as<PathPattern>(is->pattern);
                    if (!panic_ && path != nullptr && !path->has_args && cur().kind == K::Identifier &&
                        cur().line == toks_[pos_ > 0 ? pos_ - 1 : 0].line) {
                        is->binding = ident_of(advance());
                    }
                    is->span = from(start);
                    lhs = is;
                    break;
                }
                case OpClass::As: {
                    auto* a = make<AsExpr>(start);
                    a->operand = lhs;
                    skip_nl();
                    a->type = parse_type();
                    a->span = from(start);
                    lhs = a;
                    break;
                }
                case OpClass::Ternary: {
                    bool misspaced = !op_tok.space_before;
                    auto* t = make<TernaryExpr>(start);
                    t->cond = lhs;
                    {
                        // The middle operand is delimited by `?` and `:`, like brackets.
                        RestrictGuard rg(*this, {.no_struct = r_.no_struct, .no_lambda = false, .if_binding = false});
                        t->then_expr = parse_expr();
                    }
                    if (!panic_) {
                        skip_nl();
                        if (expect(K::Colon, "':' in ternary expression")) t->else_expr = expr_prec(Prec::Ternary);
                    }
                    t->span = from(start);
                    if (!panic_ && misspaced) {
                        std::string fix = std::format("{} ? {} : {}", src_.slice(t->cond->span),
                                                      src_.slice(t->then_expr->span), src_.slice(t->else_expr->span));
                        report(op_span, "ternary '?' needs spaces around it (a '?' right after an expression "
                                        "propagates an error)",
                               FixIt{t->span, fix});
                    }
                    lhs = t;
                    break;
                }
                case OpClass::None: break;
            }
            chained = non_associative(info.prec) ? info.prec : Prec::Lowest;
        }
        return lhs;
    }

    Expr* unary() {
        skip_nl();
        const Token start = cur();
        std::optional<UnaryOp> op;
        if (start.kind == K::Minus) op = UnaryOp::Neg;
        if (start.kind == K::Bang) op = UnaryOp::Not;
        if (start.kind == K::Tilde) op = UnaryOp::BitNot;
        if (op) {
            advance();
            auto* u = make<UnaryExpr>(sp(start));
            u->op = *op;
            u->operand = unary();
            u->span = from(start);
            return u;
        }
        if (start.is_keyword("await")) {
            advance();
            mark_suspends();
            auto* a = make<AwaitExpr>(sp(start));
            a->operand = unary();
            a->span = from(start);
            return a;
        }
        return postfix(primary());
    }

    Expr* postfix(Expr* e) {
        while (!panic_) {
            // A line starting with `.` or `?.` continues the previous one (1.3).
            if (raw().kind == K::Newline) {
                std::size_t n = skip_nl_from(pos_);
                if (toks_[n].kind == K::Dot || toks_[n].kind == K::QuestionDot) {
                    pos_ = n;
                } else if (!insensitive()) {
                    break;
                }
            }
            const Token& t = cur();
            Span start = e->span;
            if (t.kind == K::LeftParen) {
                auto* c = make<CallExpr>(start);
                c->callee = e;
                c->args = call_args();
                c->span = from(start);
                e = c;
            } else if (t.kind == K::LeftBracket) {
                advance();
                auto* ix = make<IndexExpr>(start);
                ix->object = e;
                {
                    NlGuard nl(*this, true, K::LeftBracket);
                    RestrictGuard rg(*this, {});
                    ix->index = parse_expr();
                    if (!panic_) expect(K::RightBracket, "']'");
                }
                ix->span = from(start);
                e = ix;
            } else if (t.kind == K::Dot || t.kind == K::QuestionDot) {
                advance();
                skip_nl();
                auto* m = make<MemberExpr>(start);
                m->object = e;
                m->safe = t.kind == K::QuestionDot;
                // Keywords are fine as member names: `map.get(k)`, `x.start()`.
                if (at(K::Identifier) || at(K::Keyword)) {
                    m->member = ident_of(advance());
                } else {
                    error_expected(std::format("member name after '{}'", t.text));
                }
                if (!panic_ && at(K::Less)) {
                    List<TypeExpr*> args;
                    if (try_expr_type_args(args)) m->type_args = args;
                }
                m->span = from(start);
                e = m;
            } else if (t.kind == K::Question && !t.space_before && !misspaced_ternary_at(effective_pos()) &&
                       !if_binding_at(effective_pos())) {
                advance();
                auto* tr = make<TryExpr>(from(start));
                tr->operand = e;
                e = tr;
            } else {
                break;
            }
        }
        return e;
    }

    // `name = value` in an argument list. Keywords that can't start an
    // expression are fine as names (`@export(range = r, step = 0.5)`).
    static bool is_arg_name(const Token& t) {
        if (t.kind == K::Identifier) return true;
        if (t.kind != K::Keyword) return false;
        for (std::string_view w :
             {"true", "false", "null", "self", "super", "fn", "new", "alloc", "await", "start", "free"}) {
            if (t.text == w) return false;
        }
        return true;
    }

    List<Arg> call_args() {
        advance(); // (
        std::vector<Arg> args;
        NlGuard nl(*this, true, K::LeftParen);
        RestrictGuard rg(*this, {});
        bool seen_named = false;
        while (!at(K::RightParen) && !at(K::Eof)) {
            Arg a;
            const Token start = cur();
            if (is_arg_name(start) && peek().kind == K::Equal) {
                // Assignment is never an expression, so `name = value` is a named argument (2.3).
                a.name = ident_of(advance());
                advance(); // =
                seen_named = true;
                for (const Arg& prev : args) {
                    if (prev.name.name == a.name.name) {
                        report(a.name.span, std::format("argument '{}' is given more than once", a.name.name));
                        break;
                    }
                }
            } else if (seen_named) {
                report(sp(start), "positional argument after named arguments");
            }
            a.value = parse_expr();
            a.span = from(start);
            args.push_back(a);
            if (panic_ || !accept(K::Comma)) break;
        }
        if (!panic_) expect(K::RightParen, "',' or ')' in argument list");
        return arena_.list(args);
    }

    // After `{`: `}` or `IDENT =` means a struct literal. Accessor blocks
    // start with the keywords `get`/`set`, so `var hp = max {` is unambiguous.
    bool struct_lit_ahead() const {
        std::size_t i = skip_nl_from(pos_ + 1);
        if (toks_[i].kind == K::RightBrace) return true;
        return toks_[i].kind == K::Identifier && toks_[i + 1].kind == K::Equal;
    }

    bool arrow_lambda_ahead() const {
        std::size_t i = effective_pos();
        if (toks_[i].kind == K::Identifier) {
            return toks_[insensitive() ? skip_nl_from(i + 1) : i + 1].kind == K::FatArrow;
        }
        if (toks_[i].kind != K::LeftParen) return false;
        ++i;
        for (;;) {
            i = skip_nl_from(i);
            K k = toks_[i].kind;
            if (k == K::RightParen) return toks_[skip_nl_from(i + 1)].kind == K::FatArrow;
            if (k != K::Identifier && k != K::Comma) return false;
            ++i;
        }
    }

    Expr* arrow_lambda() {
        const Token start = cur();
        auto* l = make<LambdaExpr>(sp(start));
        l->arrow = true;
        std::vector<Param> params;
        if (start.kind == K::Identifier) {
            advance();
            params.push_back(Param{ident_of(start), nullptr, nullptr, sp(start)});
        } else {
            advance(); // (
            NlGuard nl(*this, true, K::LeftParen);
            while (at(K::Identifier)) {
                const Token& p = advance();
                params.push_back(Param{ident_of(p), nullptr, nullptr, sp(p)});
                if (!accept(K::Comma)) break;
            }
            expect(K::RightParen, "')'");
        }
        l->params = arena_.list(params);
        skip_nl();
        expect(K::FatArrow, "'=>'");
        suspend_targets_.push_back(&l->suspends);
        l->expr_body = parse_expr();
        suspend_targets_.pop_back();
        l->span = from(start);
        return l;
    }

    Expr* block_lambda() {
        const Token start = advance(); // fn
        auto* l = make<LambdaExpr>(sp(start));
        if (!at(K::LeftParen)) {
            error_expected("'(' after 'fn' in a lambda");
            return l;
        }
        l->params = param_list(ParamRules::Lambda);
        if (!panic_ && accept(K::Arrow)) l->ret = parse_type();
        if (!panic_) {
            // The body is a block even inside parentheses.
            NlGuard nl(*this, false);
            suspend_targets_.push_back(&l->suspends);
            l->body = block();
            suspend_targets_.pop_back();
        }
        l->span = from(start);
        return l;
    }

    Expr* number_literal(const Token& t) {
        std::string_view text = t.text;
        std::size_t i = 0;
        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) base = 16;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) base = 2;
        std::string digits;
        if (base != 10) {
            i = 2;
            auto ok = [&](char c) {
                return base == 2 ? (c == '0' || c == '1')
                                 : ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
            };
            for (; i < text.size() && (ok(text[i]) || text[i] == '_'); ++i) {
                if (text[i] != '_') digits += text[i];
            }
        } else {
            auto digit = [](char c) { return c >= '0' && c <= '9'; };
            for (; i < text.size(); ++i) {
                char c = text[i];
                if (digit(c) || c == '.') {
                    digits += c;
                } else if (c == '_') {
                    continue;
                } else if ((c == 'e' || c == 'E') && i + 1 < text.size() &&
                           (digit(text[i + 1]) || ((text[i + 1] == '+' || text[i + 1] == '-') && i + 2 < text.size() &&
                                                   digit(text[i + 2])))) {
                    digits += c;
                    if (text[i + 1] == '+' || text[i + 1] == '-') digits += text[++i];
                } else {
                    break;
                }
            }
        }
        std::string_view suffix_text = text.substr(i);
        NumSuffix suffix = NumSuffix::None;
        for (int s = static_cast<int>(NumSuffix::I8); s <= static_cast<int>(NumSuffix::Deg); ++s) {
            if (to_string(static_cast<NumSuffix>(s)) == suffix_text) suffix = static_cast<NumSuffix>(s);
        }
        if (t.kind == K::Float) {
            auto* f = make<FloatLit>(sp(t));
            f->text = text;
            f->suffix = suffix;
            double v = 0.0;
            auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
            if (ec != std::errc() || ptr != digits.data() + digits.size()) {
                report(sp(t), "invalid float literal");
            }
            f->value = suffix == NumSuffix::Deg ? v * deg_to_rad : v;
            return f;
        }
        auto* n = make<IntLit>(sp(t));
        n->text = text;
        n->suffix = suffix;
        std::uint64_t v = 0;
        auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
        if (ec == std::errc::result_out_of_range) {
            report(sp(t), "integer literal is too large");
        } else if (ec != std::errc() || ptr != digits.data() + digits.size()) {
            report(sp(t), "invalid integer literal");
        }
        n->value = v;
        return n;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

    // Decodes a string token into text and interpolation parts (LANGUAGE.md 1.5).
    Expr* string_literal(const Token& t) {
        auto* s = make<StringLit>(sp(t));
        s->source_text = t.text;
        std::string_view text = src_.text();
        std::uint32_t begin = t.offset;
        std::uint32_t end = t.offset + t.length;
        if (text[begin] == 'r') {
            s->raw = true;
            ++begin;
        }
        s->multiline = end - begin >= 6 && text.substr(begin, 3) == "\"\"\"";
        std::uint32_t quote = s->multiline ? 3 : 1;
        std::uint32_t content_begin = begin + quote;
        std::uint32_t content_end = end - quote;

        // Multiline dedent: drop the line break after the opening `"""`; if the
        // closing `"""` is on its own line, drop that line and strip its
        // indentation from every line.
        std::uint32_t indent = 0;
        if (s->multiline) {
            std::uint32_t i = content_begin;
            while (i < content_end && is_blank(text[i])) ++i;
            if (i < content_end && text[i] == '\n') content_begin = i + 1;
            std::uint32_t j = content_end;
            while (j > content_begin && (text[j - 1] == ' ' || text[j - 1] == '\t')) --j;
            if (j > content_begin && text[j - 1] == '\n') {
                indent = content_end - j;
                content_end = j - 1;
                if (content_end > content_begin && text[content_end - 1] == '\r') --content_end;
            } else if (j == content_begin && content_begin > begin + quote) {
                // Only whitespace after the first line break: empty string.
                content_end = content_begin;
            }
        }

        std::vector<StringPart> parts;
        std::string buf;
        std::uint32_t buf_start = content_begin;
        auto flush = [&](std::uint32_t at_offset) {
            if (!buf.empty()) parts.push_back(StringPart{arena_.str(buf), nullptr, Span{buf_start, at_offset}});
            buf.clear();
        };

        bool line_start = true;
        std::uint32_t i = content_begin;
        while (i < content_end) {
            if (line_start && indent > 0) {
                std::uint32_t k = 0;
                while (k < indent && i < content_end && (text[i] == ' ' || text[i] == '\t')) {
                    ++i;
                    ++k;
                }
                line_start = false;
                continue;
            }
            line_start = false;
            char c = text[i];
            if (c == '\n') {
                line_start = true;
                buf += c;
                ++i;
                continue;
            }
            if (c == '\r' && i + 1 < content_end && text[i + 1] == '\n') {
                ++i; // normalize CRLF
                continue;
            }
            if (s->raw) {
                buf += c;
                ++i;
                continue;
            }
            if (c == '\\') {
                i = decode_escape(i, content_end, buf);
                continue;
            }
            if (c == '{' && i + 1 < content_end && text[i + 1] == '{') {
                buf += '{';
                i += 2;
                continue;
            }
            if (c == '}' && i + 1 < content_end && text[i + 1] == '}') {
                buf += '}';
                i += 2;
                continue;
            }
            if (c == '}') {
                report(Span{i, i + 1}, "unmatched '}' in string; write '}}' for a literal brace");
                buf += '}';
                ++i;
                continue;
            }
            if (c == '{') {
                flush(i);
                std::uint32_t close = 0;
                Expr* e = interpolation(i + 1, content_end, close);
                parts.push_back(StringPart{{}, e, Span{i, close + 1}});
                i = close + 1;
                buf_start = i;
                continue;
            }
            buf += c;
            ++i;
        }
        flush(content_end);
        s->parts = arena_.list(parts);
        return s;
    }

    // Decodes the escape at `i` (a backslash). Returns the index after it.
    std::uint32_t decode_escape(std::uint32_t i, std::uint32_t end, std::string& buf) {
        std::string_view text = src_.text();
        if (i + 1 >= end) {
            report(Span{i, i + 1}, "unfinished escape sequence");
            return end;
        }
        char e = text[i + 1];
        switch (e) {
            case 'n': buf += '\n'; return i + 2;
            case 't': buf += '\t'; return i + 2;
            case 'r': buf += '\r'; return i + 2;
            case '\\': buf += '\\'; return i + 2;
            case '"': buf += '"'; return i + 2;
            case '0': buf += '\0'; return i + 2;
            case 'u': {
                std::uint32_t j = i + 2;
                if (j < end && text[j] == '{') {
                    std::uint32_t k = j + 1;
                    std::uint32_t cp = 0;
                    int digits = 0;
                    while (k < end && digits <= 6) {
                        char h = text[k];
                        int v = h >= '0' && h <= '9'   ? h - '0'
                                : h >= 'a' && h <= 'f' ? h - 'a' + 10
                                : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                       : -1;
                        if (v < 0) break;
                        cp = cp * 16 + static_cast<std::uint32_t>(v);
                        ++digits;
                        ++k;
                    }
                    if (k < end && text[k] == '}' && digits >= 1 && digits <= 6 && cp <= 0x10FFFF &&
                        !(cp >= 0xD800 && cp <= 0xDFFF)) {
                        append_utf8(buf, cp);
                        return k + 1;
                    }
                }
                report(Span{i, std::min(i + 2, end)}, "invalid unicode escape; expected '\\u{XXXX}'");
                return i + 2;
            }
            default:
                report(Span{i, i + 2}, std::format("invalid escape sequence '\\{}'", e));
                return i + 2;
        }
    }

    // Parses the interpolated expression starting at `begin` (just after the
    // `{`); sets `close` to the offset of the matching `}`.
    Expr* interpolation(std::uint32_t begin, std::uint32_t end, std::uint32_t& close) {
        LineCol lc = src_.line_col(begin);
        Lexer lexer(src_.text(), begin, end, lc.line, lc.column);
        std::vector<Token> tokens;
        int depth = 0;
        for (;;) {
            Token tok = lexer.next();
            if (tok.kind == K::Eof) {
                close = end;
                tok.offset = end;
                tok.length = 0;
                tokens.push_back(tok);
                break;
            }
            if (tok.kind == K::LeftBrace) ++depth;
            if (tok.kind == K::RightBrace && depth-- == 0) {
                close = tok.offset;
                tok.kind = K::Eof;
                tok.length = 0;
                tokens.push_back(tok);
                break;
            }
            tokens.push_back(tok);
        }
        Parser sub(src_, std::move(tokens), arena_, diags_);
        sub.speculating_ = speculating_;
        return sub.parse_interpolation();
    }

    Expr* primary() {
        const Token t = cur();
        switch (t.kind) {
            case K::Int:
            case K::Float: advance(); return number_literal(t);
            case K::String: advance(); return string_literal(t);
            case K::Name: {
                advance();
                auto* n = make<NameLit>(sp(t));
                n->name = t.text.substr(1);
                return n;
            }
            case K::Identifier: return identifier_expr();
            case K::LeftParen: {
                advance();
                NlGuard nl(*this, true, K::LeftParen);
                RestrictGuard rg(*this, {});
                Expr* inner = parse_expr();
                if (!panic_) expect(K::RightParen, "')'");
                // Parentheses aren't kept; widen the span to cover them.
                inner->span = from(t);
                return inner;
            }
            case K::LeftBracket: return list_literal();
            case K::LeftBrace:
                if (!r_.no_struct) return map_literal();
                break;
            case K::Keyword: {
                std::string_view w = t.text;
                if (w == "true" || w == "false") {
                    advance();
                    auto* b = make<BoolLit>(sp(t));
                    b->value = w == "true";
                    return b;
                }
                if (w == "null") return make<NullLit>(sp(advance()));
                if (w == "self") return make<SelfExpr>(sp(advance()));
                if (w == "super") return make<SuperExpr>(sp(advance()));
                if (w == "fn") return block_lambda();
                if (w == "new") return new_expr();
                if (w == "alloc") return alloc_expr();
                if (w == "start" || w == "free") return builtin_call(w);
                if (w == "await") return unary();
                if (w == "step") return identifier_expr();
                break;
            }
            default: break;
        }
        error_expected("expression");
        return make<ErrorExpr>(sp(t));
    }

    Expr* identifier_expr() {
        const Token t = advance();
        auto* id = make<IdentExpr>(sp(t));
        id->name = t.text;
        if (at(K::Less)) {
            List<TypeExpr*> args;
            if (try_expr_type_args(args)) id->type_args = args;
        }
        id->span = from(t);
        if (!r_.no_struct && raw().kind == K::LeftBrace && struct_lit_ahead()) return struct_literal(id);
        return id;
    }

    Expr* struct_literal(IdentExpr* type_name) {
        auto* type = make<NamedType>(type_name->span);
        type->name = Ident{type_name->name, Span{type_name->span.begin,
                                                 type_name->span.begin + static_cast<std::uint32_t>(type_name->name.size())}};
        type->args = type_name->type_args;
        auto* s = make<StructLit>(type_name->span);
        s->type = type;
        advance(); // {
        std::vector<FieldInit> fields;
        {
            NlGuard nl(*this, true, K::LeftBrace);
            RestrictGuard rg(*this, {});
            while (!at(K::RightBrace) && !at(K::Eof)) {
                FieldInit f;
                const Token& name = cur();
                f.name = expect_ident("field name");
                if (panic_) break;
                for (const FieldInit& prev : fields) {
                    if (prev.name.name == f.name.name) {
                        report(f.name.span, std::format("field '{}' is initialized more than once", f.name.name));
                        break;
                    }
                }
                if (!expect(K::Equal, "'=' after the field name")) break;
                f.value = parse_expr();
                f.span = from(name);
                fields.push_back(f);
                if (panic_ || !accept(K::Comma)) break;
            }
            if (!panic_) expect(K::RightBrace, "',' or '}' in struct literal");
        }
        s->fields = arena_.list(fields);
        s->span = from(type_name->span);
        return s;
    }

    Expr* list_literal() {
        const Token start = advance(); // [
        auto* l = make<ListLit>(sp(start));
        std::vector<Expr*> elems;
        {
            NlGuard nl(*this, true, K::LeftBracket);
            RestrictGuard rg(*this, {});
            while (!at(K::RightBracket) && !at(K::Eof)) {
                elems.push_back(parse_expr());
                if (panic_ || !accept(K::Comma)) break;
            }
            if (!panic_) expect(K::RightBracket, "',' or ']' in list literal");
        }
        l->elements = arena_.list(elems);
        l->span = from(start);
        return l;
    }

    Expr* map_literal() {
        const Token start = advance(); // {
        auto* m = make<MapLit>(sp(start));
        std::vector<MapEntry> entries;
        {
            NlGuard nl(*this, true, K::LeftBrace);
            RestrictGuard rg(*this, {});
            while (!at(K::RightBrace) && !at(K::Eof)) {
                MapEntry e;
                e.key = parse_expr();
                if (!panic_ && expect(K::Colon, "':' after the map key")) e.value = parse_expr();
                if (e.value == nullptr) e.value = make<ErrorExpr>(e.key->span);
                entries.push_back(e);
                if (panic_ || !accept(K::Comma)) break;
            }
            if (!panic_) expect(K::RightBrace, "',' or '}' in map literal");
        }
        m->entries = arena_.list(entries);
        m->span = from(start);
        return m;
    }

    Expr* new_expr() {
        const Token start = advance(); // new
        auto* n = make<NewExpr>(sp(start));
        if (!expect(K::LeftParen, "'(' after 'new' (e.g. 'new(frame) T()')")) return n;
        n->arena = expect_ident("arena name");
        if (panic_ || !expect(K::RightParen, "')'")) return n;
        n->type = parse_type();
        if (!panic_) {
            if (raw().kind == K::LeftParen) {
                n->args = call_args();
            } else {
                error_expected("'(' with constructor arguments");
            }
        }
        n->span = from(start);
        return n;
    }

    Expr* alloc_expr() {
        const Token start = advance(); // alloc
        auto* a = make<AllocExpr>(sp(start));
        a->type = parse_type();
        if (!panic_) {
            if (raw().kind == K::LeftParen) {
                a->args = call_args();
            } else {
                error_expected("'(' with constructor arguments");
            }
        }
        a->span = from(start);
        return a;
    }

    // `start(expr)` / `free(expr)`
    Expr* builtin_call(std::string_view which) {
        const Token start = advance();
        Expr* operand = nullptr;
        if (expect(K::LeftParen, std::format("'(' after '{}'", which))) {
            NlGuard nl(*this, true, K::LeftParen);
            RestrictGuard rg(*this, {});
            operand = parse_expr();
            if (!panic_) expect(K::RightParen, "')'");
        } else {
            operand = make<ErrorExpr>(sp(start));
        }
        if (which == "start") {
            auto* s = make<StartExpr>(from(start));
            s->operand = operand;
            return s;
        }
        auto* f = make<FreeExpr>(from(start));
        f->operand = operand;
        return f;
    }

    // `await` / `yield` make the innermost enclosing function or lambda a
    // coroutine (DESIGN.md 5.9).
    void mark_suspends() {
        if (!suspend_targets_.empty()) *suspend_targets_.back() = true;
    }

    // ------------------------------------------------------------ state

    std::vector<bool*> suspend_targets_;
    const SourceFile& src_;
    std::vector<Token> toks_;
    Arena& arena_;
    DiagnosticSink& diags_;
    std::vector<char> lex_reported_;
    std::size_t pos_ = 0;
    std::uint32_t last_end_ = 0;
    bool panic_ = false;
    std::vector<K> panic_open_;
    int speculating_ = 0;
    bool spec_failed_ = false;
    struct Scope {
        bool insensitive = false;
        K opener = K::Eof;
    };
    std::vector<Scope> nl_;
    Restrictions r_;
    bool script_ = false;
    int block_depth_ = 0;
};

std::vector<Token> lex(const SourceFile& source) { return Lexer::tokenize(source.text()); }

} // namespace

Module parse_module(const SourceFile& source, Arena& arena, DiagnosticSink& diagnostics) {
    Parser p(source, lex(source), arena, diagnostics);
    Module m = p.parse_module_root();
    diagnostics.sort_by_position();
    return m;
}

Script parse_script(const SourceFile& source, Arena& arena, DiagnosticSink& diagnostics) {
    Parser p(source, lex(source), arena, diagnostics);
    Script s = p.parse_script_root();
    diagnostics.sort_by_position();
    return s;
}

Expr* parse_expression(const SourceFile& source, Arena& arena, DiagnosticSink& diagnostics) {
    Parser p(source, lex(source), arena, diagnostics);
    Expr* e = p.parse_single_expression();
    diagnostics.sort_by_position();
    return e;
}

} // namespace pluma

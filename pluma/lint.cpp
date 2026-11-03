#include "pluma/lint.hpp"

namespace pluma {

namespace {

void walk(const Stmt* s, DiagnosticSink& diags);

void walk_block(const BlockStmt* b, DiagnosticSink& diags) {
    if (b == nullptr) return;
    bool dead = false;
    for (const Stmt* s : b->stmts) {
        if (s == nullptr) continue;
        if (dead && s->kind != StmtKind::Error) {
            diags.report(Severity::Warning, s->span, "unreachable code").code = warn_unreachable_code;
            dead = false; // one warning per run of dead statements
        }
        walk(s, diags);
        if (s->kind == StmtKind::Return || s->kind == StmtKind::Break || s->kind == StmtKind::Continue) {
            dead = true;
        }
    }
}

void walk(const Stmt* s, DiagnosticSink& diags) {
    if (s == nullptr) return;
    switch (s->kind) {
        case StmtKind::Block: walk_block(as<BlockStmt>(s), diags); break;
        case StmtKind::If: {
            const auto* i = as<IfStmt>(s);
            for (const IfBranch& br : i->branches) walk_block(br.body, diags);
            walk_block(i->else_body, diags);
            break;
        }
        case StmtKind::Match:
            for (const MatchArm& arm : as<MatchStmt>(s)->arms) walk(arm.body, diags);
            break;
        case StmtKind::For: walk_block(as<ForStmt>(s)->body, diags); break;
        case StmtKind::Foreach: walk_block(as<ForeachStmt>(s)->body, diags); break;
        case StmtKind::While: walk_block(as<WhileStmt>(s)->body, diags); break;
        case StmtKind::Loop: walk_block(as<LoopStmt>(s)->body, diags); break;
        case StmtKind::Defer: walk(as<DeferStmt>(s)->body, diags); break;
        case StmtKind::Unsafe: walk_block(as<UnsafeStmt>(s)->body, diags); break;
        default: break;
    }
}

void walk_decl(const Decl* d, DiagnosticSink& diags) {
    if (d == nullptr) return;
    switch (d->kind) {
        case DeclKind::Fn: walk_block(as<FnDecl>(d)->body, diags); break;
        case DeclKind::Class:
            for (const Decl* m : as<ClassDecl>(d)->members) walk_decl(m, diags);
            break;
        case DeclKind::Struct:
            for (const Decl* m : as<StructDecl>(d)->members) walk_decl(m, diags);
            break;
        case DeclKind::Trait:
            for (const Decl* m : as<TraitDecl>(d)->members) walk_decl(m, diags);
            break;
        default: break;
    }
}

} // namespace

void lint_unreachable(const Module& module, DiagnosticSink& diags) {
    for (const Decl* d : module.decls) walk_decl(d, diags);
}

} // namespace pluma

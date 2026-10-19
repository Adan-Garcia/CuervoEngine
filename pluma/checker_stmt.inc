// Statement, pattern and match checking. Included inside class Checker.
// clang-format off

    ir::Block* block(BlockStmt* b, const std::vector<LocalVar*>& pre = {}) {
        auto* out = mks<ir::Block>(b != nullptr ? b->span : Span{});
        if (b == nullptr) return out;
        push_scope();
        for (LocalVar* v : pre) declare_local(v);
        std::vector<ir::Stmt*> stmts;
        for (Stmt* s : b->stmts) {
            if (ir::Stmt* st = stmt(s, false)) stmts.push_back(st);
        }
        pop_scope();
        out->stmts = list(stmts);
        return out;
    }

    // `top`: a top-level statement of a script (REPL echo applies).
    ir::Stmt* stmt(Stmt* s, bool top) {
        switch (s->kind) {
            case StmtKind::Error: return nullptr;
            case StmtKind::Expr: {
                auto* es = as<ExprStmt>(s);
                ir::Expr* x = expr(es->expr);
                auto* n = mks<ir::ExprStmt>(s->span);
                n->expr = x;
                n->echo = top && opts_.echo && !x->type->is_void() && !x->type->is_error();
                return n;
            }
            case StmtKind::Decl: return local_decl(as<DeclStmt>(s)->decl);
            case StmtKind::Assign: return assign(as<AssignStmt>(s));
            case StmtKind::Block: return block(as<BlockStmt>(s));
            case StmtKind::If: return if_stmt(as<IfStmt>(s), 0);
            case StmtKind::Match: return match_stmt(as<MatchStmt>(s));
            case StmtKind::For: return for_stmt(as<ForStmt>(s));
            case StmtKind::Foreach:
                error(s->span, "'foreach' over pools is not supported yet (Phase 4); use 'for'");
                return nullptr;
            case StmtKind::While: {
                auto* w = as<WhileStmt>(s);
                auto* n = mks<ir::While>(s->span);
                n->cond = expect_bool(w->cond);
                n->body = loop_body(n, w->label, w->body);
                return n;
            }
            case StmtKind::Loop: {
                auto* l = as<LoopStmt>(s);
                auto* n = mks<ir::Loop>(s->span);
                n->body = loop_body(n, l->label, l->body);
                return n;
            }
            case StmtKind::Return: return return_stmt(as<ReturnStmt>(s));
            case StmtKind::Break:
            case StmtKind::Continue: return jump(s);
            case StmtKind::Defer: {
                auto* d = as<DeferStmt>(s);
                auto* n = mks<ir::Defer>(s->span);
                ++cx_->defer_depth;
                std::vector<LoopInfo> loops;
                std::swap(loops, cx_->loops); // no break/continue out of a defer
                if (auto b = as<BlockStmt>(d->body)) {
                    n->body = block(b);
                } else {
                    push_scope();
                    n->body = stmt(d->body, false);
                    pop_scope();
                }
                std::swap(loops, cx_->loops);
                --cx_->defer_depth;
                return n->body != nullptr ? n : nullptr;
            }
            case StmtKind::Unsafe: {
                // Skips the generation checks of manual references inside
                // (DESIGN.md 5.10); otherwise a plain block.
                ir::Block* b = block(as<UnsafeStmt>(s)->body);
                b->unchecked = true;
                return b;
            }
            case StmtKind::Yield: {
                // `yield` waits one tick (DESIGN.md 5.9).
                if (!await_allowed(s->span, "yield")) return nullptr;
                auto* n = mks<ir::ExprStmt>(s->span);
                n->expr = await_node(s->span, ir::AwaitKind::Ticks, int_const(s->span, T.int_(), 1), T.void_());
                return n;
            }
            case StmtKind::Emit: return emit_stmt(as<EmitStmt>(s));
        }
        return nullptr;
    }

    // `emit name(args)`: one of the enclosing class's signals (DESIGN.md 5.9).
    ir::Stmt* emit_stmt(EmitStmt* e) {
        RecordInfo* r = cx_->record;
        FieldInfo* f = r != nullptr ? find_field(r, e->signal.name) : nullptr;
        if (f == nullptr || !f->is_signal) {
            error(e->signal.span, std::format("unknown signal '{}'{}", e->signal.name,
                                              r != nullptr ? std::format(" in '{}'", r->name) : std::string()));
            check_args_loosely(e->args);
            return nullptr;
        }
        if (cx_->is_static) {
            error(e->span, "'emit' needs an object: use it in an instance method");
            check_args_loosely(e->args);
            return nullptr;
        }
        const Type* st = field_type(f);
        std::vector<const Arg*> slot(f->signal_params.size(), nullptr);
        map_args(e->args, f->signal_params, slot, std::format("signal '{}'", f->name));
        std::vector<ir::Expr*> args{field_access(self_expr(e->signal.span), f, e->signal.span)};
        for (std::size_t i = 0; i < slot.size(); ++i) {
            if (slot[i] == nullptr) {
                error(e->span, std::format("missing argument '{}' for signal '{}'", f->signal_params[i], f->name));
                args.push_back(error_node(e->span));
            } else {
                args.push_back(expect_type(slot[i]->value, st->args[i], "signal argument"));
            }
        }
        auto* n = mks<ir::ExprStmt>(e->span);
        n->expr = builtin_node(Builtin::SignalEmit, e->span, T.void_(), args);
        return n;
    }

    ir::Stmt* local_decl(Decl* d) {
        if (d->kind == DeclKind::Var) {
            auto* v = as<VarDecl>(d);
            if (v->weak) error(d->span, "'weak' applies only to class instance fields, not to local variables");
            const Type* type = v->type != nullptr ? resolve_type(v->type) : nullptr;
            ir::Expr* init = nullptr;
            if (v->init != nullptr) {
                init = expect_type(v->init, type, "initializer");
                if (type == nullptr) type = literal_default_type(init->type, v->init->span);
            } else if (type == nullptr) {
                error(v->name.span, std::format("variable '{}' needs a type or an initializer", v->name.name));
                type = T.error();
                init = error_node(d->span);
            } else {
                init = default_value(type, v->name.span);
            }
            LocalVar* var = new_local(v->name.name, type, v->name.span, true);
            var->holds = arena_of_life(lifetime_of(init));
            declare_local(var);
            auto* n = mks<ir::VarStmt>(d->span);
            n->var = var;
            n->init = init;
            return n;
        }
        if (d->kind == DeclKind::Const) {
            auto* c = as<ConstDecl>(d);
            const Type* type = c->type != nullptr ? resolve_type(c->type) : nullptr;
            ir::Expr* x = expect_type(c->value, type, "initializer");
            if (type == nullptr) type = literal_default_type(x->type, c->value->span);
            std::size_t errors_before = diags_.error_count();
            auto v = const_eval(x);
            ir::Const* node = nullptr;
            if (!v) {
                if (!x->type->is_error() && diags_.error_count() == errors_before) {
                    error(c->value->span,
                          std::format("the value of constant '{}' must be a constant expression", c->name.name));
                }
                node = mk<ir::Const>(c->name.span, T.error());
            } else {
                node = const_node(c->name.span, type, *v);
            }
            if (!c->name.empty()) cx_->scopes.back().push_back(Entry{c->name.name, nullptr, node});
            return nullptr;
        }
        // Declarations inside blocks were already rejected by the parser,
        // except at the top level of a script, which run_script handles.
        return nullptr;
    }

    // ------------------------------------------------------------ assignment

    // `place.x = e`, `place.xy += e`: the vector in `place` is replaced by a
    // copy with those components changed (vectors are values, LANGUAGE.md 4.2).
    ir::Stmt* vec_component_assign(AssignStmt* a, MemberExpr* m, ir::Expr* obj) {
        auto* n = mks<ir::Assign>(a->span);
        n->target = obj;
        n->uses_hole = true;
        std::uint8_t idx[4] = {};
        std::uint8_t count = 0;
        bool place_ok = true;
        switch (obj->kind) {
            case ir::EK::Local:
            case ir::EK::Capture: {
                LocalVar* v = obj->kind == ir::EK::Local ? ir::as<ir::Local>(obj)->var : ir::as<ir::Capture>(obj)->var;
                place_ok = v->is_mutable;
                break;
            }
            case ir::EK::Global: place_ok = ir::as<ir::Global>(obj)->var->is_mutable; break;
            case ir::EK::Field: place_ok = struct_place_mutable(obj); break;
            case ir::EK::Index: break;
            default: place_ok = false; break;
        }
        bool dup = false;
        bool named = swizzle_indices(m->member, obj->type->dim, idx, count);
        for (std::uint8_t i = 0; named && i < count; ++i) {
            for (std::uint8_t j = 0; j < i; ++j) dup = dup || idx[i] == idx[j];
        }
        if (!named || dup || !place_ok) {
            if (!named) {
                error(m->member.span, std::format("{} has no component '{}'", q(obj->type), m->member.name));
            } else if (dup) {
                error(m->member.span, std::format("'{}' names a component twice", m->member.name));
            } else {
                error(a->target->span, "cannot assign to a component of an immutable vector (a parameter, constant or "
                                       "temporary); copy it into a 'var'");
            }
            expr(a->value);
            n->value = error_node(a->span);
            return n;
        }
        const Type* part = count == 1 ? T.float_() : T.vec(count);
        auto* hole = mk<ir::Hole>(a->target->span, obj->type);
        ir::Expr* value = nullptr;
        if (a->op == AssignOp::Set) {
            value = expect_type(a->value, part, "assignment");
        } else {
            auto* cur = mk<ir::Swizzle>(a->target->span, part);
            cur->operand = hole;
            cur->count = count;
            std::copy(idx, idx + 4, cur->index);
            ir::Expr* rhs = expr(a->value, part);
            value = coerce(binary_typed(assign_to_binary(a->op), cur, rhs, a->span), part, a->span, "assignment");
        }
        auto* w = mk<ir::VecWith>(a->span, obj->type);
        w->operand = hole;
        w->value = value;
        w->count = count;
        std::copy(idx, idx + 4, w->index);
        n->value = w;
        return n;
    }

    ir::Stmt* assign(AssignStmt* a) {
        if (auto* m = as<MemberExpr>(a->target); m != nullptr && !m->safe && as<SuperExpr>(m->object) == nullptr &&
                                                  type_target(m->object) == nullptr) {
            ir::Expr* obj = expr(m->object);
            if (obj->type->is_vec()) return vec_component_assign(a, m, obj);
            bool saved = raw_field_;
            raw_field_ = true;
            ir::Expr* target = member_lvalue(m, obj);
            raw_field_ = saved;
            return finish_assign(a, target);
        }
        bool saved = raw_field_;
        raw_field_ = as<IdentExpr>(a->target) != nullptr;
        ir::Expr* target = lvalue(a->target);
        raw_field_ = saved;
        return finish_assign(a, target);
    }

    // Assignment to a property with a setter (LANGUAGE.md 9.19): calls it;
    // a compound assignment reads through the getter first and evaluates the
    // object once.
    ir::Stmt* property_assign(AssignStmt* a, ir::Field* target) {
        FieldInfo* f = target->field;
        accessor_sig(f);
        if (f->setter == nullptr) {
            error(a->target->span, std::format("property '{}' is read-only (it has a getter but no setter)", f->name));
            expr(a->value);
            return nullptr;
        }
        Subst s = record_subst(T.non_null(target->object->type), f->owner);
        const Type* ft = substitute(T, field_type(f), s);
        ir::Expr* obj = target->object;
        std::vector<ir::Stmt*> stmts;
        if (a->op != AssignOp::Set && obj->kind != ir::EK::Local && obj->kind != ir::EK::Capture &&
            obj->kind != ir::EK::Global) {
            LocalVar* tmp = new_local("<obj>", obj->type, obj->span, false);
            auto* vs = mks<ir::VarStmt>(a->span);
            vs->var = tmp;
            vs->init = obj;
            stmts.push_back(vs);
            auto* l = mk<ir::Local>(obj->span, obj->type);
            l->var = tmp;
            obj = l;
        }
        ir::Expr* value = nullptr;
        if (a->op == AssignOp::Set) {
            value = expect_type(a->value, ft, "assignment");
        } else {
            ir::Expr* cur = field_read(obj, f, a->target->span);
            ir::Expr* rhs = expr(a->value, T.non_null(ft));
            value = coerce(binary_typed(assign_to_binary(a->op), cur, rhs, a->span), ft, a->span, "assignment");
        }
        auto* es = mks<ir::ExprStmt>(a->span);
        es->expr = method_call_args(obj, f->setter, {value}, a->span, T.void_());
        stmts.push_back(es);
        if (stmts.size() == 1) return es;
        auto* b = mks<ir::Block>(a->span);
        b->stmts = list(stmts);
        return b;
    }

    // `place[i] = e`, `place[i] += e` on a vector (LANGUAGE.md 9.20): the vector in
    // `place` is replaced by a copy with component `i` set (bounds-checked at
    // run time). The index is evaluated once, even for compound assignment.
    ir::Stmt* vec_index_assign(AssignStmt* a, ir::Index* ix) {
        ir::Expr* obj = ix->object;
        auto* n = mks<ir::Assign>(a->span);
        n->target = obj;
        n->uses_hole = true;
        bool place_ok = true;
        switch (obj->kind) {
            case ir::EK::Local:
            case ir::EK::Capture: {
                LocalVar* v = obj->kind == ir::EK::Local ? ir::as<ir::Local>(obj)->var : ir::as<ir::Capture>(obj)->var;
                place_ok = v->is_mutable;
                break;
            }
            case ir::EK::Global: place_ok = ir::as<ir::Global>(obj)->var->is_mutable; break;
            case ir::EK::Field: place_ok = struct_place_mutable(obj); break;
            case ir::EK::Index: break;
            default: place_ok = false; break;
        }
        if (!place_ok) {
            error(a->target->span, "cannot assign to a component of an immutable vector (a parameter, constant or "
                                   "temporary); copy it into a 'var'");
            expr(a->value);
            n->value = error_node(a->span);
            return n;
        }
        std::vector<ir::Stmt*> stmts;
        ir::Expr* idx = ix->index;
        if (a->op != AssignOp::Set && idx->kind != ir::EK::Local && idx->kind != ir::EK::Const) {
            LocalVar* tmp = new_local("<index>", idx->type, idx->span, false);
            auto* vs = mks<ir::VarStmt>(a->span);
            vs->var = tmp;
            vs->init = idx;
            stmts.push_back(vs);
            auto* l = mk<ir::Local>(idx->span, idx->type);
            l->var = tmp;
            idx = l;
        }
        auto* hole = mk<ir::Hole>(a->target->span, obj->type);
        ir::Expr* value = nullptr;
        if (a->op == AssignOp::Set) {
            value = expect_type(a->value, T.float_(), "assignment");
        } else {
            auto* cur = mk<ir::Index>(a->target->span, T.float_());
            cur->object = hole;
            cur->index = idx;
            ir::Expr* rhs = expr(a->value, T.float_());
            value = coerce(binary_typed(assign_to_binary(a->op), cur, rhs, a->span), T.float_(), a->span, "assignment");
        }
        n->value = builtin_node(Builtin::VecSetAt, a->span, obj->type, {hole, idx, value});
        if (stmts.empty()) return n;
        stmts.push_back(n);
        auto* b = mks<ir::Block>(a->span);
        b->stmts = list(stmts);
        return b;
    }

    ir::Stmt* finish_assign(AssignStmt* a, ir::Expr* target) {
        if (auto* vix = ir::as<ir::Index>(target); vix != nullptr && vix->object->type->is_vec()) {
            return vec_index_assign(a, vix);
        }
        if (auto fx = ir::as<ir::Field>(target);
            fx != nullptr && (fx->field->setter != nullptr || fx->field->computed) && !in_accessor_of(fx->field)) {
            return property_assign(a, fx);
        }
        auto* n = mks<ir::Assign>(a->span);
        n->target = target;
        if (auto f = ir::as<ir::Field>(target); f != nullptr && f->field->is_signal) {
            error(a->target->span, std::format("cannot assign to signal '{}'", f->field->name));
            n->target = target = error_node(a->target->span);
        }
        if (target->type->is_error()) {
            expr(a->value);
            n->value = error_node(a->span);
            return n;
        }
        if (a->op == AssignOp::Set) {
            n->value = expect_type(a->value, target->type, "assignment");
            check_store(target, n->value, a->value->span);
            return n;
        }
        auto* hole = mk<ir::Hole>(a->target->span, target->type);
        BinaryOp op = assign_to_binary(a->op);
        ir::Expr* rhs = expr(a->value, T.non_null(target->type));
        ir::Expr* v = binary_typed(op, hole, rhs, a->span);
        n->value = coerce(v, target->type, a->span, "assignment");
        n->uses_hole = true;
        return n;
    }

    // Can a struct value reached through `x` be mutated in place?
    bool struct_place_mutable(const ir::Expr* x) {
        switch (x->kind) {
            case ir::EK::Local: {
                LocalVar* v = ir::as<ir::Local>(x)->var;
                return v->is_mutable || v->name == "self";
            }
            case ir::EK::Capture: {
                LocalVar* v = ir::as<ir::Capture>(x)->var;
                return v->is_mutable || v->name == "self";
            }
            case ir::EK::Global: return ir::as<ir::Global>(x)->var->is_mutable;
            case ir::EK::Index: return true;
            case ir::EK::Field: {
                const ir::Expr* obj = ir::as<ir::Field>(x)->object;
                return T.non_null(obj->type)->tag != TypeTag::Struct || struct_place_mutable(obj);
            }
            default: return false;
        }
    }

    ir::Expr* lvalue(Expr* e) {
        if (auto* id = as<IdentExpr>(e)) {
            ir::Expr* x = nullptr;
            if (ir::Expr* v = lookup_var(cx_, id->name, id->span)) {
                x = v;
                LocalVar* var = v->kind == ir::EK::Local     ? ir::as<ir::Local>(v)->var
                                : v->kind == ir::EK::Capture ? ir::as<ir::Capture>(v)->var
                                                             : nullptr;
                if (var == nullptr) {
                    error(id->span, std::format("cannot assign to constant '{}'", id->name));
                    return error_node(id->span);
                }
                if (!var->is_mutable) {
                    if (var->name == "self") {
                        error(id->span, "cannot assign to 'self'");
                    } else if (var->is_param) {
                        error(id->span, std::format("cannot assign to parameter '{}' (parameters are immutable)",
                                                    id->name));
                    } else {
                        error(id->span, std::format("cannot assign to '{}': it is not a 'var'", id->name));
                    }
                    return error_node(id->span);
                }
                return x;
            }
            x = member_value(id->name, id->span);
            if (x == nullptr) {
                if (const Program::Symbol* s = global_symbol(id->name);
                    s != nullptr && s->kind == Program::SymKind::Global) {
                    x = global_ref(static_cast<GlobalVar*>(s->ptr), id->span);
                } else {
                    x = resolve_value(id->name, id->span);
                    if (!x->type->is_error()) error(id->span, std::format("cannot assign to '{}'", id->name));
                    return error_node(id->span);
                }
            }
            return check_assignable_place(x, id->span, id->name);
        }
        if (auto* m = as<MemberExpr>(e)) {
            if (m->safe) {
                error(e->span, "cannot assign through '?.'");
                return error_node(e->span);
            }
            if (const Program::Symbol* s = type_target(m->object)) {
                return check_assignable_place(static_member(*s, m->member, m->span), m->span, m->member.name);
            }
            return member_lvalue(m, expr(m->object));
        }
        if (auto* ix = as<IndexExpr>(e)) {
            ir::Expr* x = index(ix);
            if (x->kind == ir::EK::MethodCall) {
                error(e->span, "assigning through a custom 'index' (IndexSet) is not supported yet");
                return error_node(e->span);
            }
            return x;
        }
        error(e->span, "invalid assignment target");
        return error_node(e->span);
    }

    ir::Expr* member_lvalue(MemberExpr* m, ir::Expr* obj) {
        Expr* e = m;
        {
            ir::Expr* x = member_access(obj, m->member, m->span);
            if (x->kind == ir::EK::Field && T.non_null(obj->type)->tag == TypeTag::Struct &&
                !struct_place_mutable(obj)) {
                if (auto l = ir::as<ir::Local>(obj); l != nullptr && l->var->is_param) {
                    error(e->span, std::format("cannot assign to a field of parameter '{}' (parameters are "
                                               "immutable; copy it into a 'var')",
                                               l->var->name));
                } else {
                    error(e->span, "cannot assign to a field of a temporary or immutable struct value");
                }
                return error_node(e->span);
            }
            return x;
        }
    }

    ir::Expr* check_assignable_place(ir::Expr* x, Span span, std::string_view name) {
        if (x->type->is_error()) return x;
        if (x->kind == ir::EK::Const) {
            error(span, std::format("cannot assign to constant '{}'", name));
            return error_node(span);
        }
        if (auto g = ir::as<ir::Global>(x); g != nullptr && !g->var->is_mutable) {
            error(span, std::format("cannot assign to constant '{}'", name));
            return error_node(span);
        }
        if (x->kind != ir::EK::Global && x->kind != ir::EK::Field) {
            error(span, std::format("cannot assign to '{}'", name));
            return error_node(span);
        }
        return x;
    }

    // ------------------------------------------------------------ control flow

    ir::Stmt* if_stmt(IfStmt* s, std::uint32_t branch) {
        const IfBranch& b = s->branches[branch];
        auto* n = mks<ir::If>(b.span);
        std::vector<LocalVar*> binds;
        if (!b.unwrap.empty()) {
            // `if target? t { }` (LANGUAGE.md 3)
            ir::Expr* x = expr(b.cond);
            if (!x->type->is_nullable() && !x->type->is_error()) {
                error(b.cond->span, std::format("'?' unwraps a nullable value, but {} is never null", q(x->type)));
            }
            LocalVar* v = new_local(b.unwrap.name, T.non_null(x->type), b.unwrap.span, false);
            binds.push_back(v);
            auto* p = mkp<ir::NonNullPat>(b.unwrap.span, x->type);
            p->bind = v;
            auto* is = mk<ir::Is>(b.cond->span, T.bool_());
            is->operand = x;
            is->pattern = p;
            n->cond = is;
        } else {
            std::vector<LocalVar*>* saved = cond_binds_;
            cond_binds_ = &binds;
            n->cond = expect_bool(b.cond);
            cond_binds_ = saved;
        }
        n->then_body = block(b.body, binds);
        if (branch + 1 < s->branches.size()) {
            n->else_body = if_stmt(s, branch + 1);
        } else if (s->else_body != nullptr) {
            n->else_body = block(s->else_body);
        }
        return n;
    }

    ir::Block* loop_body(ir::Stmt* node, Ident label, BlockStmt* body, const std::vector<LocalVar*>& pre = {}) {
        for (const LoopInfo& l : cx_->loops) {
            if (!label.empty() && l.label == label.name) {
                error(label.span, std::format("label '{}' is already used by an enclosing loop", label.name));
            }
        }
        cx_->loops.push_back(LoopInfo{node, label.name});
        ir::Block* b = block(body, pre);
        cx_->loops.pop_back();
        return b;
    }

    ir::Stmt* jump(Stmt* s) {
        bool is_break = s->kind == StmtKind::Break;
        Ident label = is_break ? as<BreakStmt>(s)->label : as<ContinueStmt>(s)->label;
        std::string_view word = is_break ? "break" : "continue";
        ir::Stmt* target = nullptr;
        if (cx_->loops.empty()) {
            error(s->span, std::format("'{}' outside of a loop", word));
            return nullptr;
        }
        if (label.empty()) {
            target = cx_->loops.back().node;
        } else {
            for (auto it = cx_->loops.rbegin(); it != cx_->loops.rend(); ++it) {
                if (it->label == label.name) {
                    target = it->node;
                    break;
                }
            }
            if (target == nullptr) {
                error(label.span, std::format("no enclosing loop is labeled '{}'", label.name));
                return nullptr;
            }
        }
        if (is_break) {
            auto* n = mks<ir::Break>(s->span);
            n->loop = target;
            return n;
        }
        auto* n = mks<ir::Continue>(s->span);
        n->loop = target;
        return n;
    }

    ir::Stmt* return_stmt(ReturnStmt* r) {
        auto* n = mks<ir::Return>(r->span);
        if (cx_->defer_depth > 0) {
            error(r->span, "'return' is not allowed inside 'defer'");
            return nullptr;
        }
        if (cx_->fn->kind == FnKind::Chunk) {
            error(r->span, "'return' outside of a function");
            return nullptr;
        }
        if (cx_->ret == nullptr) {
            // Lambda without a declared return type: the first return decides.
            if (r->value == nullptr) {
                cx_->ret = T.void_();
                return n;
            }
            ir::Expr* x = expr(r->value);
            cx_->ret = literal_default_type(x->type, r->value->span);
            n->value = value_copy(x);
            return n;
        }
        if (r->value == nullptr) {
            if (!cx_->ret->is_void() && !cx_->ret->is_error()) {
                error(r->span, std::format("'return' needs a value of type {}", q(cx_->ret)));
            }
            return n;
        }
        if (cx_->ret->is_void()) {
            error(r->value->span, "this function returns nothing; remove the value or declare a return type");
            expr(r->value);
            return n;
        }
        n->value = expect_type(r->value, cx_->ret, "return value");
        return n;
    }

    ir::Stmt* for_stmt(ForStmt* f) {
        if (auto* r = as<RangeExpr>(f->iterable)) {
            auto* n = mks<ir::ForRange>(f->span);
            ir::Expr* lo = nullptr;
            ir::Expr* hi = nullptr;
            if (is_untyped_literal(r->lo) && !is_untyped_literal(r->hi)) {
                hi = expr(r->hi);
                lo = expr(r->lo, hi->type);
            } else {
                lo = expr(r->lo);
                hi = expr(r->hi, lo->type);
            }
            const Type* t = lo->type;
            if (!t->is_error() && !t->is_int()) {
                error(r->lo->span, std::format("a 'for' range needs integers, found {}", q(t)));
                t = T.error();
            } else if (!hi->type->is_error() && hi->type != t && !t->is_error()) {
                error(r->hi->span, std::format("mismatched range bounds {} and {}", q(t), q(hi->type)));
            }
            n->lo = lo;
            n->hi = hi;
            n->inclusive = r->inclusive;
            if (r->step != nullptr) n->step = expect_type(r->step, t->is_error() ? nullptr : t, "step");
            if (!f->second.empty()) error(f->second.span, "a range loop has one variable");
            n->var = new_local(f->first.name, t, f->first.span, false);
            n->body = loop_body(n, f->label, f->body, {n->var});
            return n;
        }
        ir::Expr* x = expr(f->iterable);
        const Type* t = x->type;
        auto* n = mks<ir::ForList>(f->span);
        n->list = x;
        std::vector<LocalVar*> vars;
        switch (t->tag) {
            case TypeTag::List:
            case TypeTag::Array: n->over = ir::ForList::Over::List; break;
            case TypeTag::Deque: n->over = ir::ForList::Over::Deque; break;
            case TypeTag::Set:
            case TypeTag::Map: n->over = ir::ForList::Over::Hash; break;
            default:
                if (!t->is_error()) {
                    error(f->iterable->span, std::format("cannot iterate over {}; 'for' supports ranges, lists, "
                                                         "arrays, maps, sets and deques",
                                                         q(t)));
                }
                t = T.list(T.error());
                break;
        }
        if (t->tag == TypeTag::Map) {
            // `for k in map` / `for k, v in map`, in insertion order.
            n->elem = new_local(f->first.name, t->args[0], f->first.span, false);
            vars = {n->elem};
            if (!f->second.empty()) {
                n->value = new_local(f->second.name, t->args[1], f->second.span, false);
                vars.push_back(n->value);
            }
        } else if (!f->second.empty()) {
            if (t->tag == TypeTag::Set) {
                error(f->second.span, "a set has no indices; iterate with 'for x in set'");
            }
            n->index = new_local(f->first.name, T.int_(), f->first.span, false);
            n->elem = new_local(f->second.name, t->args[0], f->second.span, false);
            vars = {n->index, n->elem};
        } else {
            n->elem = new_local(f->first.name, t->args[0], f->first.span, false);
            vars = {n->elem};
        }
        n->body = loop_body(n, f->label, f->body, vars);
        return n;
    }

    // ------------------------------------------------------------ patterns

    ir::Pattern* class_pattern(Pattern* p, const Type* subject, Ident binding, std::vector<LocalVar*>& binds) {
        auto* path = as<PathPattern>(p);
        if (path == nullptr || path->has_args || path->path.size() != 1) {
            error(p->span, "a binding after 'is' needs a class name: 'x is Boss b'");
            return mkp<ir::WildcardPat>(p->span, subject);
        }
        const Program::Symbol* s = global_symbol(path->path[0].name);
        if (s == nullptr || s->kind != Program::SymKind::Class) {
            error(p->span, std::format("'{}' is not a class", path->path[0].name));
            return mkp<ir::WildcardPat>(p->span, subject);
        }
        auto* cls = static_cast<ClassInfo*>(s->ptr);
        return make_class_pat(cls, subject, p->span, &binding, binds);
    }

    ir::Pattern* make_class_pat(ClassInfo* cls, const Type* subject, Span span, const Ident* binding,
                                std::vector<LocalVar*>& binds) {
        const Type* st = T.non_null(subject);
        if (!st->is_error() && st->tag != TypeTag::Class && st->tag != TypeTag::Trait && st->tag != TypeTag::Param) {
            error(span, std::format("a value of type {} is never a '{}'", q(subject), cls->name));
        } else if (ClassInfo* sc = st->cls(); sc != nullptr && !cls->is_subclass_of(sc) && !sc->is_subclass_of(cls)) {
            error(span, std::format("a '{}' is never a '{}'", sc->name, cls->name));
        }
        auto* n = mkp<ir::ClassPat>(span, subject);
        n->cls = cls;
        if (binding != nullptr && !binding->empty()) {
            const Type* bt = cls->generics.empty() ? T.class_(cls) : nullptr;
            if (bt == nullptr) {
                // Narrowing a generic class keeps the subject's arguments.
                bt = st->cls() == cls ? st : T.class_(cls, std::vector<const Type*>(cls->generics.size(), T.error()));
            }
            n->bind = new_local(binding->name, bt, binding->span, false);
            binds.push_back(n->bind);
        }
        return n;
    }

    ConstValue pattern_const(Expr* e, const Type* subject) {
        ir::Expr* x = expect_type(e, subject, "pattern");
        auto v = const_eval(x);
        if (!v) {
            if (!x->type->is_error()) error(e->span, "a pattern must be a constant");
            return ConstValue{};
        }
        return *v;
    }

    ir::Pattern* pattern(Pattern* p, const Type* subject, std::vector<LocalVar*>& binds, bool in_or) {
        const Type* st = T.non_null(subject);
        switch (p->kind) {
            case PatternKind::Error: return mkp<ir::WildcardPat>(p->span, subject);
            case PatternKind::Wildcard: return mkp<ir::WildcardPat>(p->span, subject);
            case PatternKind::Literal: {
                Expr* v = as<LiteralPattern>(p)->value;
                if (v->kind == ExprKind::Null) {
                    if (!subject->is_nullable() && !subject->is_error()) {
                        error(p->span, std::format("a value of type {} is never null", q(subject)));
                    }
                    return mkp<ir::NullPat>(p->span, subject);
                }
                if (subject->is_error()) return mkp<ir::WildcardPat>(p->span, subject);
                if (subject->tag == TypeTag::Nullable || subject->is_reference()) {
                    // Literals only match non-null values of the inner type.
                }
                auto* n = mkp<ir::LiteralPat>(p->span, st);
                n->value = pattern_const(v, st);
                if (st->tag == TypeTag::Param || st->tag == TypeTag::Struct || st->tag == TypeTag::Class) {
                    error(p->span, std::format("literal patterns cannot match {}", q(st)));
                }
                return n;
            }
            case PatternKind::Range: {
                auto* r = as<RangePattern>(p);
                if (!st->is_num() && !st->is_error()) {
                    error(p->span, std::format("range patterns need a number, but the value is {}", q(subject)));
                    return mkp<ir::WildcardPat>(p->span, subject);
                }
                auto* n = mkp<ir::RangePat>(p->span, st);
                n->lo = pattern_const(r->lo, st);
                n->hi = pattern_const(r->hi, st);
                n->inclusive = r->inclusive;
                return n;
            }
            case PatternKind::Or: {
                auto* o = as<OrPattern>(p);
                std::vector<ir::Pattern*> alts;
                for (Pattern* a : o->alternatives) alts.push_back(pattern(a, subject, binds, true));
                auto* n = mkp<ir::OrPat>(p->span, subject);
                n->alternatives = list(alts);
                return n;
            }
            case PatternKind::Path: break;
        }
        auto* path = as<PathPattern>(p);
        // `Ok(v)` / `Err(e)` on a Result (LANGUAGE.md 9.18).
        if (path->path.size() == 1 && st->tag == TypeTag::Result &&
            (path->path[0].name == "Ok" || path->path[0].name == "Err")) {
            auto* n = mkp<ir::ResultPat>(p->span, subject);
            n->ok = path->path[0].name == "Ok";
            if (path->has_args) {
                if (path->args.size() != 1) {
                    error(p->span, std::format("'{}' has one payload, but the pattern has {}", path->path[0].name,
                                               path->args.size()));
                }
                if (!path->args.empty()) n->inner = pattern(path->args[0], st->args[n->ok ? 0 : 1], binds, in_or);
            }
            return n;
        }
        if (path->path.size() == 1 && !path->has_args) {
            std::string_view name = path->path[0].name;
            // A constant or a class name; otherwise a new binding.
            if (!has_local(name)) {
                if (const Program::Symbol* s = global_symbol(name)) {
                    if (s->kind == Program::SymKind::Class) {
                        return make_class_pat(static_cast<ClassInfo*>(s->ptr), subject, p->span, nullptr, binds);
                    }
                    if (s->kind == Program::SymKind::Global && static_cast<GlobalVar*>(s->ptr)->is_const) {
                        auto* g = static_cast<GlobalVar*>(s->ptr);
                        ensure_global(g);
                        if (!assignable(g->type, st)) {
                            error(p->span, std::format("constant '{}' has type {}, but the value is {}", name,
                                                       q(g->type), q(subject)));
                        }
                        auto* n = mkp<ir::LiteralPat>(p->span, st);
                        n->value = g->value;
                        return n;
                    }
                }
            }
            if (in_or) {
                error(p->span, "bindings are not allowed in '|' patterns");
                return mkp<ir::WildcardPat>(p->span, subject);
            }
            auto* n = mkp<ir::BindPat>(p->span, subject);
            n->var = new_local(name, subject, path->path[0].span, false);
            binds.push_back(n->var);
            return n;
        }
        // `Enum.Variant` / `Enum.Variant(p, ...)`
        if (path->path.size() == 2) {
            const Program::Symbol* s = global_symbol(path->path[0].name);
            if (s != nullptr && s->kind == Program::SymKind::Enum) {
                auto* e = static_cast<EnumInfo*>(s->ptr);
                if (!st->is_error() && st->en() != e) {
                    error(p->span, std::format("pattern of enum '{}' cannot match a value of type {}", e->name,
                                               q(subject)));
                    return mkp<ir::WildcardPat>(p->span, subject);
                }
                int vi = e->variant_index(path->path[1].name);
                if (vi < 0) {
                    error(path->path[1].span, std::format("enum '{}' has no variant '{}'", e->name,
                                                          path->path[1].name));
                    return mkp<ir::WildcardPat>(p->span, subject);
                }
                const VariantInfo& v = e->variants[vi];
                auto* n = mkp<ir::VariantPat>(p->span, subject);
                n->en = e;
                n->variant = static_cast<std::uint32_t>(vi);
                n->has_args = path->has_args;
                if (path->has_args) {
                    if (path->args.size() != v.field_types.size()) {
                        error(p->span, std::format("'{}.{}' has {} payload field{}, but the pattern has {}", e->name,
                                                   v.name, v.field_types.size(), v.field_types.size() == 1 ? "" : "s",
                                                   path->args.size()));
                    }
                    std::vector<ir::Pattern*> args;
                    for (std::size_t i = 0; i < path->args.size(); ++i) {
                        const Type* ft = i < v.field_types.size() ? v.field_types[i] : T.error();
                        args.push_back(pattern(path->args[i], ft, binds, in_or));
                    }
                    n->args = list(args);
                }
                return n;
            }
        }
        error(p->span, "unknown pattern; expected 'Enum.Variant', a constant, a class name or a binding");
        return mkp<ir::WildcardPat>(p->span, subject);
    }

    // ------------------------------------------------------------ match

    static bool irrefutable(const ir::Pattern* p) {
        switch (p->kind) {
            case ir::PK::Wildcard:
            case ir::PK::Bind: return true;
            case ir::PK::Or:
                for (const ir::Pattern* a : ir::as<ir::OrPat>(p)->alternatives) {
                    if (irrefutable(a)) return true;
                }
                return false;
            default: return false;
        }
    }

    // Variants fully covered by `p` (payload sub-patterns all irrefutable).
    void covered_variants(const ir::Pattern* p, std::set<std::uint32_t>& out) {
        if (auto v = ir::as<ir::VariantPat>(p)) {
            bool full = true;
            for (const ir::Pattern* a : v->args) full = full && irrefutable(a);
            if (full) out.insert(v->variant);
        } else if (auto o = ir::as<ir::OrPat>(p)) {
            for (const ir::Pattern* a : o->alternatives) covered_variants(a, out);
        }
    }

    void covered_results(const ir::Pattern* p, bool& ok, bool& err) {
        if (auto r = ir::as<ir::ResultPat>(p)) {
            if (r->inner == nullptr || irrefutable(r->inner)) (r->ok ? ok : err) = true;
        } else if (auto o = ir::as<ir::OrPat>(p)) {
            for (const ir::Pattern* a : o->alternatives) covered_results(a, ok, err);
        }
    }

    void covered_bools(const ir::Pattern* p, bool& t, bool& f) {
        if (auto l = ir::as<ir::LiteralPat>(p); l != nullptr && l->value.kind == ConstValue::Kind::Bool) {
            (l->value.b ? t : f) = true;
        } else if (auto o = ir::as<ir::OrPat>(p)) {
            for (const ir::Pattern* a : o->alternatives) covered_bools(a, t, f);
        }
    }

    bool covers_null(const ir::Pattern* p) {
        if (p->kind == ir::PK::Null) return true;
        if (auto o = ir::as<ir::OrPat>(p)) {
            for (const ir::Pattern* a : o->alternatives) {
                if (covers_null(a)) return true;
            }
        }
        return false;
    }

    ir::Stmt* match_stmt(MatchStmt* m) {
        ir::Expr* subject = expr(m->subject);
        const Type* t = subject->type;
        auto* n = mks<ir::Match>(m->span);
        n->subject = subject;
        std::vector<ir::MatchArm> arms;
        for (const MatchArm& a : m->arms) {
            ir::MatchArm arm;
            arm.span = a.span;
            std::vector<LocalVar*> binds;
            arm.pattern = pattern(a.pattern, t, binds, false);
            push_scope();
            for (LocalVar* v : binds) declare_local(v);
            if (a.guard != nullptr) arm.guard = expect_bool(a.guard);
            if (auto b = as<BlockStmt>(a.body)) {
                arm.body = block(b);
            } else {
                arm.body = stmt(a.body, false);
                if (arm.body == nullptr) arm.body = mks<ir::Block>(a.body->span);
            }
            pop_scope();
            arms.push_back(arm);
        }
        n->arms = list(arms);
        if (!t->is_error()) check_exhaustive(n, t, m->span);
        return n;
    }

    void check_exhaustive(ir::Match* m, const Type* t, Span span) {
        bool nullable = t->tag == TypeTag::Nullable;
        bool null_covered = !nullable;
        std::set<std::uint32_t> variants;
        bool has_true = false;
        bool has_false = false;
        bool has_ok = false;
        bool has_err = false;
        for (const ir::MatchArm& a : m->arms) {
            if (a.guard != nullptr) continue;
            if (irrefutable(a.pattern)) return;
            if (covers_null(a.pattern)) null_covered = true;
            covered_variants(a.pattern, variants);
            covered_bools(a.pattern, has_true, has_false);
            covered_results(a.pattern, has_ok, has_err);
        }
        const Type* st = T.non_null(t);
        std::vector<std::string> missing;
        if (EnumInfo* e = st->en()) {
            for (std::uint32_t i = 0; i < e->variants.size(); ++i) {
                if (variants.count(i) == 0) missing.push_back(std::format("'{}.{}'", e->name, e->variants[i].name));
            }
        } else if (st == T.bool_()) {
            if (!has_true) missing.emplace_back("'true'");
            if (!has_false) missing.emplace_back("'false'");
        } else if (st->tag == TypeTag::Result) {
            if (!has_ok) missing.emplace_back("'Ok(_)'");
            if (!has_err) missing.emplace_back("'Err(_)'");
        } else {
            error(span, std::format("'match' on {} must be exhaustive: add a '_' arm", q(t)));
            return;
        }
        if (!null_covered) missing.emplace_back("'null'");
        if (missing.empty()) return;
        std::string list_text;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i > 0) list_text += ", ";
            list_text += missing[i];
        }
        error(span, std::format("'match' is not exhaustive: missing {}", list_text));
    }

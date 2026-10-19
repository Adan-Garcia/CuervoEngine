// Expression checking. Included inside class Checker (checker.cpp).
// clang-format off

    // ------------------------------------------------------------ coercion

    bool assignable(const Type* from, const Type* to) {
        if (from == to || from->is_error() || to->is_error()) return true;
        if (to->tag == TypeTag::Nullable) {
            const Type* inner = to->args[0];
            return from->tag == TypeTag::Null || assignable(from, inner);
        }
        if (from->tag == TypeTag::Null) return to->is_nullable();
        if (from->tag == TypeTag::Class && to->tag == TypeTag::Class) {
            const Type* up = upcast(from, to->cls());
            return up == to;
        }
        if (to->tag == TypeTag::Trait) {
            if (from->tag == TypeTag::Class || from->tag == TypeTag::Struct || from->tag == TypeTag::Param) {
                return satisfies(from, to->tr());
            }
        }
        return false;
    }

    // `from` (a class type) viewed as its ancestor `target`, with generic
    // arguments carried along; null if not an ancestor.
    const Type* upcast(const Type* from, ClassInfo* target) {
        const Type* cur = from;
        while (cur != nullptr) {
            ClassInfo* c = cur->cls();
            if (c == nullptr) return nullptr;
            if (c == target) return cur;
            if (c->base_type == nullptr) return nullptr;
            Subst level;
            for (std::size_t i = 0; i < c->generics.size(); ++i) {
                level[c->generics[i]] = i < cur->args.size() ? cur->args[i] : T.param(c->generics[i]);
            }
            cur = substitute(T, c->base_type, level);
        }
        return nullptr;
    }

    static bool is_place(const ir::Expr* x) {
        switch (x->kind) {
            case ir::EK::Local:
            case ir::EK::Capture:
            case ir::EK::Global:
            case ir::EK::Field:
            case ir::EK::Index: return true;
            default: return false;
        }
    }

    // Structs are values (DESIGN.md 5.4): reading one out of a place to store
    // or pass it elsewhere copies it. Generic and trait-typed values may hold
    // structs, so they get a (dynamic) copy too.
    ir::Expr* value_copy(ir::Expr* x) {
        TypeTag tag = T.non_null(x->type)->tag;
        if (!is_place(x) || (tag != TypeTag::Struct && tag != TypeTag::Param && tag != TypeTag::Trait &&
                             tag != TypeTag::Array)) {
            return x;
        }
        auto* c = mk<ir::Copy>(x->span, x->type);
        c->operand = x;
        return c;
    }

    ir::Expr* coerce(ir::Expr* x, const Type* to, Span span, std::string_view what) {
        if (to == nullptr) return value_copy(x);
        if (!assignable(x->type, to)) {
            if (x->type->is_void()) {
                error(span, std::format("{}: expression has no value (expected {})", what, q(to)));
            } else if (x->type->tag == TypeTag::Null) {
                error(span, std::format("{} cannot be null; use '{}?' for an optional value", q(to), type_name(to)));
            } else if (x->type->is_num() && to->is_num()) {
                error(span, std::format("mismatched types: expected {}, found {}; convert with 'as {}'", q(to),
                                        q(x->type), type_name(to)));
            } else {
                error(span, std::format("mismatched types: expected {}, found {}", q(to), q(x->type)));
            }
            return error_node(span);
        }
        return value_copy(x);
    }

    ir::Expr* expect_type(Expr* e, const Type* to, std::string_view what) {
        ir::Expr* x = expr(e, to);
        return coerce(x, to, e->span, what);
    }

    ir::Expr* expect_bool(Expr* e) { return expect_type(e, T.bool_(), "condition"); }

    ir::Expr* expr_in_static(Expr* e, const Type* expected) {
        Ctx ctx;
        ctx.fn = chunk_;
        ctx.ret = T.void_();
        CtxSwap swap(*this, &ctx);
        push_scope();
        ir::Expr* x = expect_type(e, expected, "value");
        pop_scope();
        return x;
    }

    // ------------------------------------------------------------ dispatch

    ir::Expr* expr(Expr* e, const Type* expected = nullptr) {
        switch (e->kind) {
            case ExprKind::Error: return error_node(e->span);
            case ExprKind::Int: return int_literal(as<IntLit>(e), expected, false, e->span);
            case ExprKind::Float: return float_literal(as<FloatLit>(e), expected, false, e->span);
            case ExprKind::Bool: {
                ConstValue c;
                c.kind = ConstValue::Kind::Bool;
                c.b = as<BoolLit>(e)->value;
                return const_node(e->span, T.bool_(), c);
            }
            case ExprKind::Null: {
                ConstValue c;
                c.kind = ConstValue::Kind::Null;
                return const_node(e->span, T.null(), c);
            }
            case ExprKind::String: return string_literal(as<StringLit>(e));
            case ExprKind::Name: {
                ConstValue c;
                c.kind = ConstValue::Kind::Name;
                c.s = A.str(as<NameLit>(e)->name);
                return const_node(e->span, T.name(), c);
            }
            case ExprKind::Identifier: {
                auto* id = as<IdentExpr>(e);
                if (!id->type_args.empty()) {
                    error(e->span, "type arguments are only allowed before a call or a struct literal");
                }
                return resolve_value(id->name, id->span);
            }
            case ExprKind::Self: return self_expr(e->span);
            case ExprKind::Super:
                error(e->span, "'super' can only be used to call a parent method: 'super.name(...)'");
                return error_node(e->span);
            case ExprKind::ListLit: return list_literal(as<ListLit>(e), expected);
            case ExprKind::MapLit: return map_literal(as<MapLit>(e), expected);
            case ExprKind::StructLit: return struct_literal(as<StructLit>(e), expected);
            case ExprKind::Unary: return unary(as<UnaryExpr>(e), expected);
            case ExprKind::Binary: return binary(as<BinaryExpr>(e), expected);
            case ExprKind::Range:
                error(e->span, "ranges can only be used in 'for' loops and patterns");
                return error_node(e->span);
            case ExprKind::Ternary: return ternary(as<TernaryExpr>(e), expected);
            case ExprKind::Is: return is_expr(as<IsExpr>(e));
            case ExprKind::As: return cast(as<AsExpr>(e));
            case ExprKind::Call: return call(as<CallExpr>(e), expected);
            case ExprKind::Index: return index(as<IndexExpr>(e));
            case ExprKind::Member: return member_expr(as<MemberExpr>(e));
            case ExprKind::Try: return try_expr(as<TryExpr>(e));
            case ExprKind::Lambda: return lambda(as<LambdaExpr>(e), expected);
            case ExprKind::New: {
                auto* n = as<NewExpr>(e);
                ArenaKind a = ArenaKind::Frame;
                if (n->arena.name == "scene") {
                    a = ArenaKind::Scene;
                } else if (n->arena.name != "frame") {
                    a = ArenaKind::Alloc; // no follow-up escape errors
                    error(n->arena.span, std::format("unknown arena '{}': use 'new(frame)' (freed at the end of the "
                                                     "tick) or 'new(scene)' (freed when the scene unloads)",
                                                     n->arena.name));
                }
                return manual_new(n->type, n->args, a, e->span, expected);
            }
            case ExprKind::Alloc: {
                auto* al = as<AllocExpr>(e);
                return manual_new(al->type, al->args, ArenaKind::Alloc, e->span, expected);
            }
            case ExprKind::Free: return free_expr(as<FreeExpr>(e));
            case ExprKind::Await: return await_expr(as<AwaitExpr>(e));
            case ExprKind::Start: return start_expr(as<StartExpr>(e));
        }
        return error_node(e->span);
    }

    // ------------------------------------------------------------ literals

    ir::Expr* int_literal(IntLit* n, const Type* expected, bool negate, Span span) {
        const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
        if (ex != nullptr && ex->is_vec()) ex = T.float_(); // `v * 2`: the scalar of a vector operation
        if (n->suffix == NumSuffix::Deg) {
            NumKind k = ex != nullptr && ex->is_num() && ex->num == NumKind::F64 ? NumKind::F64 : NumKind::F32;
            double v = static_cast<double>(n->value) * deg_to_rad;
            return float_const(span, k, negate ? -v : v);
        }
        NumKind k = NumKind::I32;
        if (n->suffix != NumSuffix::None) {
            k = suffix_kind(n->suffix);
        } else if (ex != nullptr && ex->is_num()) {
            k = ex->num; // an int literal adapts, including to float (DESIGN.md 5.4)
        }
        if (is_float(k)) {
            double v = static_cast<double>(n->value);
            return float_const(span, k, negate ? -v : v);
        }
        if (!num::fits(k, negate, n->value)) {
            error(span, std::format("integer literal {}{} does not fit in '{}'", negate ? "-" : "", n->text,
                                    to_string(k)));
            return int_const(span, T.num(k), 0);
        }
        std::uint64_t bits = negate ? 0 - n->value : n->value;
        return int_const(span, T.num(k), num::wrap(k, bits));
    }

    ir::Expr* float_literal(FloatLit* n, const Type* expected, bool negate, Span span) {
        const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
        if (ex != nullptr && ex->is_vec()) ex = T.float_();
        bool want_f64 = ex != nullptr && ex->is_num() && ex->num == NumKind::F64;
        NumKind k = NumKind::F32;
        if (n->suffix == NumSuffix::F64) {
            k = NumKind::F64;
        } else if (n->suffix == NumSuffix::None || n->suffix == NumSuffix::Deg) {
            k = want_f64 ? NumKind::F64 : NumKind::F32;
        } else if (n->suffix != NumSuffix::F32) {
            error(span, "a float literal cannot have an integer suffix");
        }
        return float_const(span, k, negate ? -n->value : n->value);
    }

    ir::Expr* float_const(Span span, NumKind k, double v) {
        ConstValue c;
        c.kind = ConstValue::Kind::Float;
        c.f = round_to(k, v);
        return const_node(span, T.num(k), c);
    }

    ir::Expr* string_literal(StringLit* s) {
        if (s->parts.empty()) return str_const(s->span, "");
        std::vector<ir::Expr*> parts;
        for (const StringPart& p : s->parts) {
            if (p.expr == nullptr) {
                parts.push_back(str_const(p.span, p.text));
            } else {
                parts.push_back(to_str(expr(p.expr), p.expr->span));
            }
        }
        if (parts.size() == 1 && (parts[0]->type == T.str() || parts[0]->type->is_error())) return parts[0];
        auto* c = mk<ir::Concat>(s->span, T.str());
        c->parts = list(parts);
        return c;
    }

    // Converts a value to str for interpolation and print: `to_str` from
    // Display if the type declares it, else the VM's default formatting.
    ir::Expr* to_str(ir::Expr* x, Span span) {
        if (x->type == T.str() || x->type->is_error()) return x;
        if (x->type->is_void()) {
            error(span, "cannot convert a void expression to a string");
            return error_node(span);
        }
        if (RecordInfo* r = record_of(x->type)) {
            if (satisfies(x->type, P.builtin_trait(BuiltinTrait::Display))) {
                if (Function* f = find_method(r, "to_str"); f != nullptr && f->kind == FnKind::Method) {
                    bool explicit_display = false;
                    for (RecordInfo* y = r; y != nullptr;
                         y = y->is_class ? static_cast<ClassInfo*>(y)->base : nullptr) {
                        for (TraitInfo* tr : y->traits) {
                            if (tr->builtin == BuiltinTrait::Display) explicit_display = true;
                        }
                    }
                    if (explicit_display) return method_call(x, f, {}, {}, span, nullptr);
                }
            }
        }
        auto* t = mk<ir::ToStr>(span, T.str());
        t->operand = x;
        return t;
    }

    ir::Expr* list_literal(ListLit* l, const Type* expected) {
        const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
        if (ex != nullptr && (ex->tag == TypeTag::Set || ex->tag == TypeTag::Deque || ex->tag == TypeTag::Array)) {
            return collection_literal(l, ex);
        }
        const Type* elem = ex != nullptr && ex->tag == TypeTag::List ? ex->args[0] : nullptr;
        std::vector<ir::Expr*> elems;
        for (Expr* e : l->elements) {
            if (elem == nullptr) {
                ir::Expr* x = expr(e);
                elem = literal_default_type(x->type, e->span);
                elems.push_back(value_copy(x));
            } else {
                elems.push_back(expect_type(e, elem, "list element"));
            }
        }
        if (elem == nullptr) {
            error(l->span, "cannot infer the element type of an empty list; write 'List<T>()' or annotate the "
                           "variable");
            elem = T.error();
        }
        auto* x = mk<ir::ListLit>(l->span, T.list(elem));
        x->elements = list(elems);
        return x;
    }

    // ------------------------------------------------------------ names

    // Side-effect-free: is `name` a local visible here (through lambdas)?
    bool has_local(std::string_view name) {
        for (Ctx* c = cx_; c != nullptr; c = c->outer) {
            for (auto& scope : c->scopes) {
                for (Entry& e : scope) {
                    if (e.name == name) return true;
                }
            }
        }
        return false;
    }

    ir::Expr* lookup_var(Ctx* c, std::string_view name, Span span) {
        for (auto s = c->scopes.rbegin(); s != c->scopes.rend(); ++s) {
            for (auto e = s->rbegin(); e != s->rend(); ++e) {
                if (e->name != name) continue;
                if (e->constant != nullptr) return e->constant;
                auto* l = mk<ir::Local>(span, e->var->type);
                l->var = e->var;
                return l;
            }
        }
        if (c->outer == nullptr) return nullptr;
        ir::Expr* outer = lookup_var(c->outer, name, span);
        if (outer == nullptr || outer->kind == ir::EK::Const) return outer;
        LocalVar* v = outer->kind == ir::EK::Local ? ir::as<ir::Local>(outer)->var : ir::as<ir::Capture>(outer)->var;
        auto& caps = c->fn->captures;
        std::uint32_t idx = 0;
        for (; idx < caps.size(); ++idx) {
            if (caps[idx].var == v) break;
        }
        if (idx == caps.size()) {
            CaptureInfo ci;
            ci.var = v;
            if (auto oc = ir::as<ir::Capture>(outer)) {
                ci.from_outer_capture = true;
                ci.outer_index = oc->index;
            }
            caps.push_back(ci);
            if (v->is_mutable) v->captured = true; // lives in a cell (capture by reference)
        }
        auto* cap = mk<ir::Capture>(span, v->type);
        cap->index = idx;
        cap->var = v;
        return cap;
    }

    ir::Expr* self_expr(Span span) {
        if (ir::Expr* s = lookup_var(cx_, "self", span)) return s;
        error(span, "'self' is only available inside methods");
        return error_node(span);
    }

    void check_private(std::string_view member, const RecordInfo* owner, Span span) {
        if (member.empty() || member[0] != '_') return;
        if (cx_ != nullptr && cx_->record == owner) return;
        error(span, std::format("'{}' is private to '{}'", member, owner->name));
    }

    ir::Expr* global_ref(GlobalVar* g, Span span) {
        ensure_global(g);
        if (g->is_const) {
            if (g->value.kind == ConstValue::Kind::None) return error_node(span);
            return const_node(span, g->type, g->value);
        }
        auto* x = mk<ir::Global>(span, g->type);
        x->var = g;
        return x;
    }

    ir::Expr* field_access(ir::Expr* obj, FieldInfo* f, Span span) {
        const Type* ft = field_type(f);
        Subst s = record_subst(T.non_null(obj->type), f->owner);
        auto* x = mk<ir::Field>(span, substitute(T, ft, s));
        x->object = obj;
        x->field = f;
        return x;
    }

    // Inside `f`'s own get/set (or a lambda in it), the field's name means
    // its storage.
    bool in_accessor_of(const FieldInfo* f) const {
        for (const Ctx* c = cx_; c != nullptr; c = c->outer) {
            if (c->fn != nullptr && c->fn->accessor_of == f) return true;
        }
        return false;
    }

    // A field read: the getter when the field has one (LANGUAGE.md 9.19).
    ir::Expr* field_read(ir::Expr* obj, FieldInfo* f, Span span) {
        if (f->getter == nullptr || raw_field_ || in_accessor_of(f)) return field_access(obj, f, span);
        accessor_sig(f);
        Subst s = record_subst(T.non_null(obj->type), f->owner);
        return method_call_args(obj, f->getter, {}, span, substitute(T, f->getter->ret, s));
    }

    // `obj.method` as a value (LANGUAGE.md 9.19): a closure that captures
    // the receiver now and calls the method (virtually) later.
    ir::Expr* bound_method(ir::Expr* obj, Function* m, Span span) {
        ensure_signature(m);
        if (m->kind == FnKind::Static) return fn_ref(m, span);
        std::string_view mname = m->decl != nullptr ? std::string_view(m->decl->name.name) : std::string_view(m->name);
        if (mname == "init") {
            error(span, "'init' runs when the object is constructed and cannot be used as a value");
            return error_node(span);
        }
        if (!m->generics.empty()) {
            error(span, std::format("generic method '{}' can only be called, not used as a value", mname));
            return error_node(span);
        }
        if (m->is_coroutine) {
            error(span, std::format("'{}' is a coroutine and cannot be used as a value; 'start' it instead", mname));
            return error_node(span);
        }
        if (obj->type->is_error()) return error_node(span);
        const Type* rt = T.non_null(obj->type);
        Subst pre = m->owner_kind == Function::Owner::Trait ? Subst{} : record_subst(rt, m->owner);
        std::vector<const Type*> ptypes;
        for (const ParamInfo& p : m->params) ptypes.push_back(substitute(T, p.type, pre));
        const Type* ret = substitute(T, m->ret, pre);
        Function*& th = bound_thunks_[{m, rt}];
        if (th == nullptr) {
            th = new_function(std::format("<bound {}>", m->name), FnKind::Lambda, span);
            th->stable_key = std::format("<bound {} on {}>", m->name, type_name(rt));
            th->signature_done = true;
            th->body_done = true;
            for (std::size_t i = 0; i < m->params.size(); ++i) {
                ParamInfo pi;
                pi.name = m->params[i].name;
                pi.type = ptypes[i];
                pi.span = m->params[i].span;
                th->params.push_back(pi);
                th->param_vars.push_back(new_local(pi.name, ptypes[i], span, false, true));
            }
            th->ret = ret;
            CaptureInfo ci;
            ci.var = new_local("<receiver>", rt, span, false);
            th->captures.push_back(ci);
            auto* cap = mk<ir::Capture>(span, rt);
            cap->index = 0;
            cap->var = ci.var;
            std::vector<ir::Expr*> args;
            for (LocalVar* v : th->param_vars) {
                auto* l = mk<ir::Local>(span, v->type);
                l->var = v;
                args.push_back(value_copy(l));
            }
            ir::Expr* call = method_call_args(cap, m, args, span, ret);
            ir::Stmt* st = nullptr;
            if (ret->is_void()) {
                auto* es = mks<ir::ExprStmt>(span);
                es->expr = call;
                st = es;
            } else {
                auto* r = mks<ir::Return>(span);
                r->value = call;
                st = r;
            }
            auto* b = mks<ir::Block>(span);
            b->stmts = list(std::vector<ir::Stmt*>{st});
            th->body = b;
        }
        auto* n = mk<ir::BoundMethod>(span, T.fn(std::move(ptypes), ret));
        n->thunk = th;
        n->receiver = value_copy(obj);
        check_escape(obj, Life::Long, span, "a method value (it can outlive the tick)");
        return n;
    }

    ir::Expr* fn_ref(Function* f, Span span) {
        ensure_signature(f);
        if (!f->generics.empty()) {
            error(span, std::format("generic function '{}' can only be called, not used as a value", f->name));
            return error_node(span);
        }
        std::vector<const Type*> params;
        for (const ParamInfo& p : f->params) params.push_back(p.type);
        auto* x = mk<ir::FnRef>(span, T.fn(std::move(params), f->ret));
        x->fn = f;
        return x;
    }

    // Implicit `self.` members and the enclosing type's statics.
    ir::Expr* member_value(std::string_view name, Span span) {
        RecordInfo* r = cx_->record;
        if (r == nullptr) return nullptr;
        if (FieldInfo* f = find_field(r, name)) {
            if (cx_->is_static) {
                error(span, std::format("instance field '{}' cannot be used here (no 'self')", name));
                return error_node(span);
            }
            return field_read(self_expr(span), f, span);
        }
        if (GlobalVar* g = find_static(r, name)) return global_ref(g, span);
        if (Function* m = find_method(r, name)) {
            if (m->kind == FnKind::Static) return fn_ref(m, span);
            if (cx_->is_static) {
                error(span, std::format("instance method '{}' cannot be used here (no 'self')", name));
                return error_node(span);
            }
            return bound_method(self_expr(span), m, span);
        }
        return nullptr;
    }

    const Program::Symbol* global_symbol(std::string_view name) {
        auto it = P.globals.find(name);
        return it != P.globals.end() ? &it->second : nullptr;
    }

    ir::Expr* resolve_value(std::string_view name, Span span) {
        if (ir::Expr* x = lookup_var(cx_, name, span)) return x;
        if (ir::Expr* x = member_value(name, span)) return x;
        if (const Program::Symbol* s = global_symbol(name)) {
            switch (s->kind) {
                case Program::SymKind::Global: return global_ref(static_cast<GlobalVar*>(s->ptr), span);
                case Program::SymKind::Function: return fn_ref(static_cast<Function*>(s->ptr), span);
                default:
                    error(span, std::format("'{}' is a type, not a value", name));
                    return error_node(span);
            }
        }
        if (builtin_fn(name)) {
            error(span, std::format("built-in function '{}' can only be called", name));
            return error_node(span);
        }
        if (is_builtin_type_name(name)) {
            error(span, std::format("'{}' is a type, not a value", name));
            return error_node(span);
        }
        error(span, std::format("unknown name '{}'", name));
        return error_node(span);
    }

    // `Phase`, `Boss` in `Phase.Rage` / `Boss.spawn()`: a type used as a
    // namespace (only when no variable of that name is visible).
    const Program::Symbol* type_target(Expr* e) {
        auto* id = as<IdentExpr>(e);
        if (id == nullptr || has_local(id->name)) return nullptr;
        if (cx_->record != nullptr && (find_field(cx_->record, id->name) != nullptr ||
                                       find_static(cx_->record, id->name) != nullptr)) {
            return nullptr;
        }
        const Program::Symbol* s = global_symbol(id->name);
        if (s == nullptr) return nullptr;
        if (s->kind == Program::SymKind::Class || s->kind == Program::SymKind::Struct ||
            s->kind == Program::SymKind::Enum) {
            return s;
        }
        return nullptr;
    }

    // ------------------------------------------------------------ operators

    const Type* num_hint(const Type* expected) {
        if (expected == nullptr) return nullptr;
        const Type* t = T.non_null(expected);
        return t->is_num() ? t : nullptr;
    }

    ir::Expr* unary(UnaryExpr* u, const Type* expected) {
        switch (u->op) {
            case UnaryOp::Neg: {
                if (auto i = as<IntLit>(u->operand)) return int_literal(i, num_hint(expected), true, u->span);
                if (auto f = as<FloatLit>(u->operand)) return float_literal(f, num_hint(expected), true, u->span);
                ir::Expr* x = expr(u->operand, num_hint(expected));
                const Type* t = x->type;
                if (t->is_error()) return error_node(u->span);
                if (t->is_num()) {
                    if (!is_signed(t->num) && !is_float(t->num)) {
                        error(u->span, std::format("cannot negate a value of unsigned type {}", q(t)));
                        return error_node(u->span);
                    }
                } else if (t->is_vec()) {
                    // componentwise
                } else if (RecordInfo* r = record_of(t);
                           r != nullptr && satisfies(t, P.builtin_trait(BuiltinTrait::Neg))) {
                    if (Function* f = find_method(r, "neg")) return method_call(x, f, {}, {}, u->span, nullptr);
                } else if (t->tag != TypeTag::Param || !satisfies(t, P.builtin_trait(BuiltinTrait::Neg))) {
                    error(u->span, std::format("operator '-' is not defined for {}", q(t)));
                    return error_node(u->span);
                }
                auto* n = mk<ir::Unary>(u->span, t);
                n->op = UnaryOp::Neg;
                n->operand = x;
                return n;
            }
            case UnaryOp::Not: {
                auto* n = mk<ir::Unary>(u->span, T.bool_());
                n->op = UnaryOp::Not;
                n->operand = expect_bool(u->operand);
                return n;
            }
            case UnaryOp::BitNot: {
                ir::Expr* x = expr(u->operand, num_hint(expected));
                if (x->type->is_error()) return error_node(u->span);
                if (!x->type->is_int()) {
                    error(u->span, std::format("operator '~' needs an integer, found {}", q(x->type)));
                    return error_node(u->span);
                }
                auto* n = mk<ir::Unary>(u->span, x->type);
                n->op = UnaryOp::BitNot;
                n->operand = x;
                return n;
            }
        }
        return error_node(u->span);
    }

    ir::Expr* binary(BinaryExpr* b, const Type* expected) {
        if (b->op == BinaryOp::And || b->op == BinaryOp::Or) {
            auto* n = mk<ir::Logical>(b->span, T.bool_());
            n->is_and = b->op == BinaryOp::And;
            n->lhs = expect_bool(b->lhs);
            // `if a is X(v) && v > 0`: bindings of the left side are visible
            // on the right of `&&`.
            bool scoped = n->is_and && cond_binds_ != nullptr && !cond_binds_->empty();
            if (scoped) {
                push_scope();
                for (LocalVar* v : *cond_binds_) declare_local(v);
            }
            n->rhs = expect_bool(b->rhs);
            if (scoped) pop_scope();
            return n;
        }
        if (b->op == BinaryOp::Coalesce) return coalesce(b, expected);
        const Type* hint = is_arith(b->op) || is_bitwise(b->op) ? num_hint(expected) : nullptr;
        ir::Expr* l = nullptr;
        ir::Expr* r = nullptr;
        // An untyped literal takes the other operand's type, so check the
        // other side first (`1 + x`, and `1 + 2.0` is float).
        bool rhs_first = is_untyped_literal(b->lhs) &&
                         (!is_untyped_literal(b->rhs) || (is_float_literal(b->rhs) && !is_float_literal(b->lhs)));
        if (rhs_first) {
            r = expr(b->rhs, hint);
            l = expr(b->lhs, T.non_null(r->type));
        } else {
            l = expr(b->lhs, hint);
            r = expr(b->rhs, is_bitwise(b->op) && (b->op == BinaryOp::Shl || b->op == BinaryOp::Shr)
                                 ? T.non_null(l->type)
                                 : T.non_null(l->type));
        }
        return binary_typed(b->op, l, r, b->span);
    }

    ir::Expr* make_binary(BinaryOp op, ir::Expr* l, ir::Expr* r, const Type* operand, const Type* result, Span span) {
        auto* n = mk<ir::Binary>(span, result);
        n->op = op;
        n->lhs = l;
        n->rhs = r;
        n->operand_type = operand;
        return n;
    }

    ir::Expr* not_expr(ir::Expr* x, Span span) {
        auto* n = mk<ir::Unary>(span, T.bool_());
        n->op = UnaryOp::Not;
        n->operand = x;
        return n;
    }

    ir::Expr* binary_typed(BinaryOp op, ir::Expr* l, ir::Expr* r, Span span) {
        const Type* lt = l->type;
        const Type* rt = r->type;
        if (lt->is_error() || rt->is_error()) return error_node(span);
        std::string_view sym = to_string(op);
        auto undefined = [&]() {
            if (lt == rt) {
                error(span, std::format("operator '{}' is not defined for {}", sym, q(lt)));
            } else if (lt->is_num() && rt->is_num()) {
                error(span, std::format("mismatched operand types {} and {} for '{}'; convert one with 'as'", q(lt),
                                        q(rt), sym));
            } else {
                error(span, std::format("operator '{}' is not defined for {} and {}", sym, q(lt), q(rt)));
            }
            return error_node(span);
        };
        // Operator overloading through the built-in traits (LANGUAGE.md 2.8).
        if (RecordInfo* rec = record_of(lt); rec != nullptr && op_trait(op) != BuiltinTrait::None &&
                                             satisfies(lt, P.builtin_trait(op_trait(op))) &&
                                             explicitly_implements(rec, op_trait(op))) {
            TraitInfo* tr = P.builtin_trait(op_trait(op));
            Function* f = find_method(rec, tr->op_method);
            if (f != nullptr) {
                ensure_signature(f);
                std::vector<ir::Expr*> args{coerce(r, f->params.empty() ? nullptr
                                                                         : substitute(T, f->params[0].type,
                                                                                      record_subst(lt, f->owner)),
                                                   span, "operand")};
                ir::Expr* call = method_call_args(l, f, args, span);
                if (op == BinaryOp::Ne) return not_expr(call, span);
                if (is_ordering(op)) {
                    return make_binary(op, call, int_const(span, T.int_(), 0), T.int_(), T.bool_(), span);
                }
                return call;
            }
        }
        if (lt->is_vec() || rt->is_vec()) return vec_binary(op, l, r, span, undefined);
        switch (op) {
            case BinaryOp::Eq:
            case BinaryOp::Ne: {
                if (lt->tag == TypeTag::Null && rt->tag == TypeTag::Null) {
                    return make_binary(op, l, r, lt, T.bool_(), span);
                }
                if (lt->tag == TypeTag::Null || rt->tag == TypeTag::Null) {
                    const Type* other = lt->tag == TypeTag::Null ? rt : lt;
                    if (!other->is_nullable()) {
                        error(span, std::format("a value of type {} is never null", q(other)));
                        return error_node(span);
                    }
                    return make_binary(op, l, r, other, T.bool_(), span);
                }
                if (!assignable(lt, rt) && !assignable(rt, lt)) {
                    error(span, std::format("cannot compare {} with {}", q(lt), q(rt)));
                    return error_node(span);
                }
                if (lt->tag == TypeTag::Fn) {
                    error(span, "functions cannot be compared");
                    return error_node(span);
                }
                return make_binary(op, l, r, lt->tag == TypeTag::Nullable || lt->is_reference() ? lt : rt,
                                   T.bool_(), span);
            }
            case BinaryOp::Lt:
            case BinaryOp::Le:
            case BinaryOp::Gt:
            case BinaryOp::Ge:
                if (lt != rt) return undefined();
                if (lt->is_num() || lt->tag == TypeTag::Str ||
                    (lt->tag == TypeTag::Param && satisfies(lt, P.builtin_trait(BuiltinTrait::Ord)))) {
                    return make_binary(op, l, r, lt, T.bool_(), span);
                }
                return undefined();
            case BinaryOp::Add:
            case BinaryOp::Sub:
            case BinaryOp::Mul:
            case BinaryOp::Div:
            case BinaryOp::Rem:
                if (lt != rt) return undefined();
                if (lt->is_num()) return make_binary(op, l, r, lt, lt, span);
                if (lt->tag == TypeTag::Str && op == BinaryOp::Add) {
                    auto* c = mk<ir::Concat>(span, T.str());
                    c->parts = list(std::vector<ir::Expr*>{l, r});
                    return c;
                }
                if (lt->tag == TypeTag::Param && satisfies(lt, P.builtin_trait(op_trait(op)))) {
                    return make_binary(op, l, r, lt, lt, span);
                }
                return undefined();
            case BinaryOp::BitAnd:
            case BinaryOp::BitOr:
            case BinaryOp::BitXor:
            case BinaryOp::Shl:
            case BinaryOp::Shr:
                if (lt != rt || !lt->is_int()) {
                    if (lt == rt && lt == T.bool_() && op != BinaryOp::Shl && op != BinaryOp::Shr) {
                        return make_binary(op, l, r, lt, lt, span);
                    }
                    return undefined();
                }
                return make_binary(op, l, r, lt, lt, span);
            default: return undefined();
        }
    }

    // Vector arithmetic (LANGUAGE.md 4.2): componentwise between vectors of
    // the same size; `v * s`, `s * v` and `v / s` with a float scalar.
    template <class Undefined>
    ir::Expr* vec_binary(BinaryOp op, ir::Expr* l, ir::Expr* r, Span span, Undefined& undefined) {
        const Type* lt = l->type;
        const Type* rt = r->type;
        switch (op) {
            case BinaryOp::Eq:
            case BinaryOp::Ne:
                if (lt != rt) return undefined();
                return make_binary(op, l, r, lt, T.bool_(), span);
            case BinaryOp::Add:
            case BinaryOp::Sub:
                if (lt != rt) return undefined();
                return make_binary(op, l, r, lt, lt, span);
            case BinaryOp::Mul:
            case BinaryOp::Div: {
                if (lt == rt) return make_binary(op, l, r, lt, lt, span);
                if (lt->is_vec() && rt == T.float_()) {
                    auto* b = ir::as<ir::Binary>(make_binary(op, l, r, lt, lt, span));
                    b->vec_scalar = 1;
                    return b;
                }
                if (op == BinaryOp::Mul && lt == T.float_() && rt->is_vec()) {
                    auto* b = ir::as<ir::Binary>(make_binary(op, l, r, rt, rt, span));
                    b->vec_scalar = 2;
                    return b;
                }
                return undefined();
            }
            default: return undefined();
        }
    }

    bool explicitly_implements(RecordInfo* r, BuiltinTrait b) {
        for (RecordInfo* x = r; x != nullptr; x = x->is_class ? static_cast<ClassInfo*>(x)->base : nullptr) {
            for (TraitInfo* tr : x->traits) {
                if (tr->builtin == b) return true;
            }
        }
        return false;
    }

    ir::Expr* coalesce(BinaryExpr* b, const Type* expected) {
        ir::Expr* l = expr(b->lhs, expected != nullptr ? T.nullable(expected) : nullptr);
        const Type* lt = l->type;
        if (lt->is_error()) {
            expr(b->rhs, expected);
            return error_node(b->span);
        }
        if (!lt->is_nullable()) {
            error(b->lhs->span, std::format("the left side of '?\?' has type {}, which is never null", q(lt)));
        }
        const Type* target = T.non_null(lt);
        ir::Expr* r = expr(b->rhs, target);
        const Type* result = target;
        if (!assignable(r->type, target)) {
            if (assignable(r->type, lt)) {
                result = lt;
            } else {
                error(b->rhs->span, std::format("mismatched types: expected {}, found {}", q(target), q(r->type)));
                return error_node(b->span);
            }
        }
        auto* n = mk<ir::Coalesce>(b->span, result);
        n->lhs = l;
        n->rhs = value_copy(r);
        return n;
    }

    ir::Expr* ternary(TernaryExpr* t, const Type* expected) {
        ir::Expr* c = expect_bool(t->cond);
        ir::Expr* a = expr(t->then_expr, expected);
        ir::Expr* b = expr(t->else_expr, expected != nullptr ? expected : T.non_null(a->type));
        const Type* type = nullptr;
        if (assignable(b->type, a->type)) {
            type = a->type;
        } else if (assignable(a->type, b->type)) {
            type = b->type;
        } else if (a->type->tag == TypeTag::Null) {
            type = T.nullable(b->type);
        } else if (b->type->tag == TypeTag::Null) {
            type = T.nullable(a->type);
        } else {
            error(t->span, std::format("the branches of '? :' have different types {} and {}", q(a->type),
                                       q(b->type)));
            return error_node(t->span);
        }
        if (type->tag == TypeTag::Null && expected == nullptr) type = T.null();
        auto* n = mk<ir::Ternary>(t->span, type);
        n->cond = c;
        n->then_expr = value_copy(a);
        n->else_expr = value_copy(b);
        return n;
    }

    ir::Expr* cast(AsExpr* a) {
        ir::Expr* x = expr(a->operand);
        const Type* to = resolve_type(a->type);
        const Type* from = x->type;
        if (from->is_error() || to->is_error()) return error_node(a->span);
        if (from == to) return x;
        auto numeric = [&]() {
            auto* c = mk<ir::Cast>(a->span, to);
            c->op = ir::Cast::Op::Numeric;
            c->operand = x;
            return c;
        };
        if (to->is_num() && (from->is_num() || from == T.bool_())) {
            if (from == T.bool_() && !to->is_int()) {
                error(a->span, std::format("cannot cast {} to {}", q(from), q(to)));
                return error_node(a->span);
            }
            return numeric();
        }
        if (to->is_int() && from->tag == TypeTag::Enum && from->en()->plain) return numeric();
        if (to->tag == TypeTag::Class) {
            if (assignable(from, to)) return x; // upcast
            bool down = (from->tag == TypeTag::Class && to->cls()->is_subclass_of(from->cls())) ||
                        from->tag == TypeTag::Trait || from->tag == TypeTag::Param;
            if (down) {
                auto* c = mk<ir::Cast>(a->span, to);
                c->op = ir::Cast::Op::Downcast;
                c->operand = x;
                return c;
            }
        }
        if (assignable(from, to)) return x;
        error(a->span, std::format("cannot cast {} to {}", q(from), q(to)));
        return error_node(a->span);
    }

    ir::Expr* is_expr(IsExpr* e) {
        ir::Expr* x = expr(e->operand);
        std::vector<LocalVar*> binds;
        ir::Pattern* p = nullptr;
        if (!e->binding.empty()) {
            p = class_pattern(e->pattern, x->type, e->binding, binds);
        } else {
            p = pattern(e->pattern, x->type, binds, false);
        }
        if (!binds.empty()) {
            if (cond_binds_ != nullptr) {
                cond_binds_->insert(cond_binds_->end(), binds.begin(), binds.end());
            } else {
                error(e->span, "pattern bindings are only allowed in an 'if' condition");
            }
        }
        auto* n = mk<ir::Is>(e->span, T.bool_());
        n->operand = x;
        n->pattern = p;
        return n;
    }

    // ------------------------------------------------------------ lambdas

    ir::Expr* lambda(LambdaExpr* l, const Type* expected) {
        const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
        if (ex != nullptr && ex->tag != TypeTag::Fn) ex = nullptr;
        Location lc = src_.location(l->span.begin);
        Function* f = new_function(std::format("<lambda@{}:{}>", lc.line, lc.column), FnKind::Lambda, l->span);
        f->lambda = l;
        f->stable_key = std::format("{}/lambda{}", cx_->fn->stable_key.empty() ? cx_->fn->name : cx_->fn->stable_key,
                                    lambda_counts_[cx_->fn]++);
        if (l->suspends) {
            error(l->span, "'await' and 'yield' cannot be used inside a lambda yet; move the code into a method "
                           "and 'start' it");
        }
        f->signature_done = true;
        f->body_done = true;
        bool arity_error = false;
        if (ex != nullptr && ex->args.size() != l->params.size()) {
            error(l->span, std::format("expected a function with {} parameter{}, but this lambda takes {}",
                                       ex->args.size(), ex->args.size() == 1 ? "" : "s", l->params.size()));
            ex = nullptr;
            arity_error = true;
        }
        Ctx ctx;
        ctx.fn = f;
        ctx.outer = cx_;
        ctx.record = cx_->record;
        ctx.trait = cx_->trait;
        ctx.is_static = cx_->is_static;
        ctx.generics = cx_->generics;
        std::vector<const Type*> ptypes;
        for (std::size_t i = 0; i < l->params.size(); ++i) {
            const Param& p = l->params[i];
            const Type* t = nullptr;
            if (p.type != nullptr) {
                t = resolve_type(p.type);
            } else if (ex != nullptr) {
                t = ex->args[i];
            } else {
                if (!arity_error) {
                    error(p.span, std::format("cannot infer the type of parameter '{}'; annotate it", p.name.name));
                }
                t = T.error();
            }
            if (p.default_value != nullptr) error(p.span, "lambda parameters cannot have default values");
            ParamInfo pi;
            pi.name = p.name.name;
            pi.type = t;
            pi.span = p.span;
            f->params.push_back(pi);
            f->param_vars.push_back(new_local(p.name.name, t, p.name.span, false, true));
            ptypes.push_back(t);
        }
        const Type* ret = nullptr;
        if (l->ret != nullptr) {
            ret = resolve_type(l->ret);
        } else if (ex != nullptr && !contains_param(ex->ret)) {
            ret = ex->ret;
        }
        ctx.ret = ret;
        ctx.infer_ret = ret == nullptr;
        {
            CtxSwap swap(*this, &ctx);
            push_scope();
            for (LocalVar* v : f->param_vars) declare_local(v);
            std::vector<ir::Stmt*> stmts;
            if (l->arrow) {
                ir::Expr* x = expr(l->expr_body, ret != nullptr && !ret->is_void() ? ret : nullptr);
                if (ctx.ret == nullptr) {
                    ctx.ret = x->type->tag == TypeTag::Null ? T.error() : x->type;
                    if (x->type->tag == TypeTag::Null) {
                        error(l->expr_body->span, "cannot infer a type from 'null'; annotate the lambda");
                    }
                }
                if (ctx.ret->is_void()) {
                    auto* es = mks<ir::ExprStmt>(l->expr_body->span);
                    es->expr = x;
                    stmts.push_back(es);
                } else {
                    auto* r = mks<ir::Return>(l->expr_body->span);
                    r->value = coerce(x, ctx.ret, l->expr_body->span, "lambda result");
                    stmts.push_back(r);
                }
            } else if (l->body != nullptr) {
                const List<Stmt*>& body = l->body->stmts;
                for (std::uint32_t i = 0; i < body.size(); ++i) {
                    Stmt* s = body[i];
                    // Final expression as the result (LANGUAGE.md 2.4).
                    bool last = i + 1 == body.size();
                    auto* es = as<ExprStmt>(s);
                    if (last && es != nullptr && l->ret == nullptr && ctx.ret != nullptr && !ctx.ret->is_void() &&
                        !ctx.ret->is_error()) {
                        auto* r = mks<ir::Return>(s->span);
                        r->value = expect_type(es->expr, ctx.ret, "lambda result");
                        stmts.push_back(r);
                        continue;
                    }
                    if (ir::Stmt* st = stmt(s, false)) stmts.push_back(st);
                }
            }
            pop_scope();
            if (ctx.ret == nullptr) ctx.ret = T.void_();
            auto* b = mks<ir::Block>(l->span);
            b->stmts = list(stmts);
            f->body = b;
        }
        f->ret = ctx.ret;
        if (!f->ret->is_void() && !f->ret->is_error() && !always_returns(f->body)) {
            error(l->span, "this lambda may finish without returning a value");
        }
        auto* x = mk<ir::Lambda>(l->span, arity_error ? T.error() : T.fn(std::move(ptypes), f->ret));
        x->fn = f;
        return x;
    }

    // ------------------------------------------------------------ members

    ir::Expr* member_expr(MemberExpr* m) {
        if (as<SuperExpr>(m->object) != nullptr) {
            error(m->span, "'super' can only be used to call a parent method: 'super.name(...)'");
            return error_node(m->span);
        }
        if (!m->type_args.empty()) error(m->span, "type arguments are only allowed before a call");
        if (const Program::Symbol* s = type_target(m->object)) return static_member(*s, m->member, m->span);
        if (m->safe) {
            ir::Expr* obj = expr(m->object);
            return safe_nav(obj, m->span, [&](ir::Expr* tmp) { return member_access(tmp, m->member, m->span); });
        }
        ir::Expr* obj = expr(m->object);
        return member_access(obj, m->member, m->span);
    }

    ir::Expr* safe_nav(ir::Expr* obj, Span span, const std::function<ir::Expr*(ir::Expr*)>& body) {
        if (obj->type->is_error()) return error_node(span);
        if (!obj->type->is_nullable()) {
            error(span, std::format("'?.' on a value of type {}, which is never null; use '.'", q(obj->type)));
        }
        LocalVar* tmp = new_local("<safe>", T.non_null(obj->type), span, false);
        auto* ref = mk<ir::Local>(obj->span, tmp->type);
        ref->var = tmp;
        ir::Expr* b = body(ref);
        const Type* type = b->type->is_void() || b->type->is_nullable() || b->type->is_error() ? b->type
                                                                                                 : T.nullable(b->type);
        auto* n = mk<ir::SafeNav>(span, type);
        n->object = obj;
        n->tmp = tmp;
        n->body = b;
        return n;
    }

    ir::Expr* static_member(const Program::Symbol& s, Ident member, Span span) {
        if (s.kind == Program::SymKind::Enum) {
            auto* e = static_cast<EnumInfo*>(s.ptr);
            int vi = e->variant_index(member.name);
            if (vi < 0) {
                error(member.span, std::format("enum '{}' has no variant '{}'", e->name, member.name));
                return error_node(span);
            }
            if (!e->variants[vi].field_types.empty()) {
                error(span, std::format("variant '{}.{}' needs its payload: '{}.{}(...)'", e->name, member.name,
                                        e->name, member.name));
                return error_node(span);
            }
            auto* n = mk<ir::EnumNew>(span, e->type);
            n->en = e;
            n->variant = static_cast<std::uint32_t>(vi);
            return n;
        }
        auto* r = static_cast<RecordInfo*>(s.ptr);
        if (GlobalVar* g = find_static(r, member.name)) {
            check_private(member.name, r, member.span);
            return global_ref(g, span);
        }
        if (Function* f = find_method(r, member.name); f != nullptr && f->kind == FnKind::Static) {
            check_private(member.name, r, member.span);
            return fn_ref(f, span);
        }
        error(member.span, std::format("'{}' has no static member '{}'", r->name, member.name));
        return error_node(span);
    }

    ir::Expr* member_access(ir::Expr* obj, Ident member, Span span) {
        const Type* t = obj->type;
        if (t->is_error()) return error_node(span);
        if (t->tag == TypeTag::Nullable) {
            error(span, std::format("value of type {} may be null; unwrap it with 'if x? v {{ }}' or '?\?'", q(t)));
            return error_node(span);
        }
        if (RecordInfo* r = record_of(t)) {
            if (FieldInfo* f = find_field(r, member.name)) {
                check_private(member.name, static_cast<RecordInfo*>(f->owner), member.span);
                return field_read(obj, f, span);
            }
            if (find_static(r, member.name) != nullptr) {
                error(member.span, std::format("'{}' is static; use '{}.{}'", member.name, r->name, member.name));
                return error_node(span);
            }
            if (Function* m = find_method(r, member.name)) {
                if (m->owner_kind != Function::Owner::Trait) {
                    check_private(member.name, static_cast<RecordInfo*>(m->owner), member.span);
                }
                return bound_method(obj, m, span);
            }
            error(member.span, std::format("{} has no field '{}'", q(t), member.name));
            return error_node(span);
        }
        if (t->is_vec()) return swizzle(obj, member, span);
        if (t->tag == TypeTag::List || t->tag == TypeTag::Str || t->tag == TypeTag::Map || t->tag == TypeTag::Set ||
            t->tag == TypeTag::Array || t->tag == TypeTag::Deque || t->tag == TypeTag::PQueue ||
            t->tag == TypeTag::StrBuilder || t->tag == TypeTag::Result) {
            error(member.span, std::format("{} has no field '{}' (methods need a call, e.g. 'x.len()')", q(t),
                                           member.name));
            return error_node(span);
        }
        error(member.span, std::format("{} has no field '{}'", q(t), member.name));
        return error_node(span);
    }

    // `v.x`, `v.yx`, `v.xyz`: components 0..3 (LANGUAGE.md 4.2).
    bool swizzle_indices(Ident member, std::uint32_t dim, std::uint8_t (&idx)[4], std::uint8_t& count) {
        std::string_view s = member.name;
        if (s.empty() || s.size() > 4) return false;
        count = 0;
        for (char ch : s) {
            int i = ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : ch == 'w' ? 3 : -1;
            if (i < 0 || static_cast<std::uint32_t>(i) >= dim) return false;
            idx[count++] = static_cast<std::uint8_t>(i);
        }
        return true;
    }

    ir::Expr* swizzle(ir::Expr* obj, Ident member, Span span) {
        const Type* t = obj->type;
        auto* n = mk<ir::Swizzle>(span, T.error());
        if (!swizzle_indices(member, t->dim, n->index, n->count)) {
            error(member.span, std::format("{} has no component '{}' (components are x, y, z, w; methods need a "
                                           "call)",
                                           q(t), member.name));
            return error_node(span);
        }
        n->type = n->count == 1 ? T.float_() : T.vec(n->count);
        n->operand = obj;
        return n;
    }

    ir::Expr* index(IndexExpr* ix) {
        ir::Expr* obj = expr(ix->object);
        const Type* t = obj->type;
        if (t->is_error()) {
            expr(ix->index);
            return error_node(ix->span);
        }
        if (t->is_vec()) {
            ir::Expr* i = expr(ix->index, T.int_());
            if (!i->type->is_int() && !i->type->is_error()) {
                error(ix->index->span, std::format("a vector index must be an integer, found {}", q(i->type)));
            }
            auto* n = mk<ir::Index>(ix->span, T.float_());
            n->object = obj;
            n->index = i;
            return n;
        }
        if (t->tag == TypeTag::Map) {
            auto* n = mk<ir::Index>(ix->span, t->args[1]);
            n->object = obj;
            n->index = expect_type(ix->index, t->args[0], "map key");
            return n;
        }
        if (t->tag == TypeTag::List || t->tag == TypeTag::Array || t->tag == TypeTag::Deque) {
            ir::Expr* i = expr(ix->index, T.int_());
            if (!i->type->is_int() && !i->type->is_error()) {
                error(ix->index->span, std::format("a list index must be an integer, found {}", q(i->type)));
            }
            auto* n = mk<ir::Index>(ix->span, t->args[0]);
            n->object = obj;
            n->index = i;
            return n;
        }
        if (RecordInfo* r = record_of(t); r != nullptr && explicitly_implements(r, BuiltinTrait::Index)) {
            if (Function* f = find_method(r, "index")) {
                ensure_signature(f);
                Subst s = record_subst(t, f->owner);
                std::vector<ir::Expr*> args{
                    expect_type(ix->index, f->params.empty() ? nullptr : substitute(T, f->params[0].type, s),
                                "index")};
                return method_call_args(obj, f, args, ix->span);
            }
        }
        error(ix->span, std::format("a value of type {} cannot be indexed", q(t)));
        return error_node(ix->span);
    }

    ir::Expr* struct_literal(StructLit* sl, const Type* expected) {
        NamedType* nt = sl->type;
        const Program::Symbol* sym = has_local(nt->name.name) ? nullptr : global_symbol(nt->name.name);
        if (sym == nullptr || sym->kind != Program::SymKind::Struct) {
            if (sym != nullptr && sym->kind == Program::SymKind::Class) {
                error(nt->span, std::format("'{}' is a class; construct it with '{}(...)'", nt->name.name,
                                            nt->name.name));
            } else {
                error(nt->span, std::format("unknown struct '{}'", nt->name.name));
            }
            for (const FieldInit& fi : sl->fields) expr(fi.value);
            return error_node(sl->span);
        }
        auto* st = static_cast<StructInfo*>(sym->ptr);
        ensure_members(st);
        Subst s;
        bool infer = false;
        if (!nt->args.empty()) {
            const Type* t = resolve_type(nt);
            if (t->is_error()) return error_node(sl->span);
            for (std::size_t i = 0; i < st->generics.size(); ++i) s[st->generics[i]] = t->args[i];
        } else if (!st->generics.empty()) {
            const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
            if (ex != nullptr && ex->st() == st) {
                for (std::size_t i = 0; i < st->generics.size(); ++i) s[st->generics[i]] = ex->args[i];
            } else {
                infer = true;
            }
        }
        std::vector<ir::Expr*> values(st->fields.size(), nullptr);
        std::vector<std::uint32_t> order;
        std::vector<std::pair<FieldInfo*, const FieldInit*>> given;
        for (const FieldInit& fi : sl->fields) {
            FieldInfo* f = find_field(st, fi.name.name);
            if (f == nullptr) {
                error(fi.name.span, std::format("struct '{}' has no field '{}'", st->name, fi.name.name));
                expr(fi.value);
                continue;
            }
            check_private(fi.name.name, st, fi.name.span);
            given.emplace_back(f, &fi);
        }
        std::set<const GenericParamInfo*> inferable(st->generics.begin(), st->generics.end());
        if (infer) {
            for (auto& [f, fi] : given) {
                values[f->index] = expr(fi->value);
                unify(field_type(f), values[f->index]->type, inferable, s);
            }
            for (GenericParamInfo* g : st->generics) {
                if (s.count(g) == 0) {
                    error(sl->span, std::format("cannot infer type argument '{}' of '{}'; write '{}<...> {{ }}'",
                                                g->name, st->name, st->name));
                    s[g] = T.error();
                }
            }
        }
        for (auto& [f, fi] : given) {
            const Type* ft = substitute(T, field_type(f), s);
            if (values[f->index] != nullptr) {
                values[f->index] = coerce(values[f->index], ft, fi->value->span, "field value");
            } else {
                values[f->index] = expect_type(fi->value, ft, "field value");
            }
            order.push_back(f->index);
        }
        for (FieldInfo* f : st->fields) {
            if (values[f->index] != nullptr) continue;
            if (field_type(f)->tag == TypeTag::Param && f->decl->init == nullptr) {
                error(sl->span, std::format("field '{}' of generic type {} must be given a value", f->name,
                                            q(f->type)));
            }
            values[f->index] = field_default(f, sl->span);
            order.push_back(f->index);
        }
        std::vector<const Type*> args;
        for (GenericParamInfo* g : st->generics) args.push_back(s[g]);
        auto* n = mk<ir::StructNew>(sl->span, T.struct_(st, std::move(args)));
        n->st = st;
        n->fields = list(values);
        n->order = list(order);
        return n;
    }

    // ------------------------------------------------------------ calls

    struct BoundCall {
        std::vector<ir::Expr*> args;
        const Type* ret = nullptr;
        Subst subst;
    };

    void unify(const Type* pattern, const Type* actual, const std::set<const GenericParamInfo*>& inferable,
               Subst& s) {
        if (actual->is_error() || actual->tag == TypeTag::Null) return;
        if (auto p = pattern->param(); p != nullptr && inferable.count(p) != 0) {
            if (s.count(p) == 0) s[p] = actual;
            return;
        }
        if (pattern->tag == TypeTag::Nullable) {
            unify(pattern->args[0], T.non_null(actual), inferable, s);
            return;
        }
        if (pattern->tag != actual->tag || pattern->decl != actual->decl || pattern->args.size() != actual->args.size()) {
            return;
        }
        for (std::size_t i = 0; i < pattern->args.size(); ++i) unify(pattern->args[i], actual->args[i], inferable, s);
        if (pattern->ret != nullptr && actual->ret != nullptr) unify(pattern->ret, actual->ret, inferable, s);
    }

    bool mentions(const Type* t, const std::set<const GenericParamInfo*>& ps) {
        if (auto p = t->param()) return ps.count(p) != 0;
        for (const Type* a : t->args) {
            if (mentions(a, ps)) return true;
        }
        return t->ret != nullptr && mentions(t->ret, ps);
    }

    bool fully_bound(const Type* t, const std::set<const GenericParamInfo*>& ps, const Subst& s) {
        if (auto p = t->param()) return ps.count(p) == 0 || s.count(p) != 0;
        for (const Type* a : t->args) {
            if (!fully_bound(a, ps, s)) return false;
        }
        return t->ret == nullptr || fully_bound(t->ret, ps, s);
    }

    // Maps arguments to parameter slots (positional first, then by name).
    void map_args(const List<Arg>& args, const std::vector<std::string_view>& names, std::vector<const Arg*>& slot,
                  std::string_view what) {
        std::size_t pos = 0;
        for (const Arg& a : args) {
            if (a.name.empty()) {
                if (pos >= names.size()) {
                    error(a.span, std::format("too many arguments to {}: expected {}", what, names.size()));
                    expr(a.value);
                    continue;
                }
                slot[pos++] = &a;
                continue;
            }
            auto it = std::find(names.begin(), names.end(), a.name.name);
            if (it == names.end()) {
                error(a.name.span, std::format("{} has no parameter named '{}'", what, a.name.name));
                expr(a.value);
                continue;
            }
            auto idx = static_cast<std::size_t>(it - names.begin());
            if (slot[idx] != nullptr) {
                error(a.name.span, std::format("parameter '{}' is given more than once", a.name.name));
                expr(a.value);
                continue;
            }
            slot[idx] = &a;
        }
    }

    // Binds and checks call arguments against `f`'s parameters. `pre` maps
    // the declaring type's generics (receiver arguments); `infer` lists the
    // generic parameters to infer from the arguments.
    BoundCall bind_call(Function* f, const Subst& pre, std::vector<GenericParamInfo*> infer, const List<Arg>& args,
                        const List<TypeExpr*>& type_args, Span span, const Type* expected) {
        ensure_signature(f);
        // Arguments are never the awaited/started call themselves.
        bool saved_allow = allow_coro_;
        allow_coro_ = false;
        BoundCall out;
        Subst& s = out.subst;
        std::string what = std::format("'{}'", f->kind == FnKind::Method && f->decl != nullptr
                                                   ? std::string(f->decl->name.name)
                                                   : f->name);
        if (!type_args.empty()) {
            if (infer.empty()) {
                error(span, std::format("{} is not generic", what));
            } else if (type_args.size() != infer.size()) {
                error(span, std::format("{} takes {} type argument{}", what, infer.size(), infer.size() == 1 ? "" : "s"));
            } else {
                for (std::size_t i = 0; i < infer.size(); ++i) {
                    s[infer[i]] = resolve_type(type_args[i]);
                    check_bounds(infer[i], s[infer[i]], type_args[i]->span);
                }
            }
            infer.clear();
        }
        std::size_t n = f->params.size();
        std::vector<const Type*> ptypes;
        std::vector<std::string_view> names;
        for (const ParamInfo& p : f->params) {
            ptypes.push_back(substitute(T, p.type, pre));
            names.push_back(p.name);
        }
        std::vector<const Arg*> slot(n, nullptr);
        map_args(args, names, slot, what);
        std::vector<ir::Expr*> vals(n, nullptr);
        std::set<const GenericParamInfo*> inferable(infer.begin(), infer.end());
        if (!inferable.empty()) {
            // Plain arguments first, then untyped literals (so `max(x, 1)`
            // takes x's type), then lambdas (whose parameter types need it).
            for (int pass = 0; pass < 3; ++pass) {
                for (std::size_t i = 0; i < n; ++i) {
                    if (slot[i] == nullptr || vals[i] != nullptr || !mentions(ptypes[i], inferable)) continue;
                    Expr* ae = slot[i]->value;
                    int kind = ae->kind == ExprKind::Lambda ? 2 : is_untyped_literal(ae) ? 1 : 0;
                    if (kind != pass) continue;
                    const Type* hint = fully_bound(ptypes[i], inferable, s) ? substitute(T, ptypes[i], s) : nullptr;
                    vals[i] = expr(ae, hint);
                    unify(ptypes[i], vals[i]->type, inferable, s);
                }
            }
            if (expected != nullptr) unify(substitute(T, f->ret, pre), expected, inferable, s);
            for (GenericParamInfo* g : infer) {
                if (s.count(g) == 0) {
                    error(span, std::format("cannot infer type argument '{}' of {}; write it explicitly", g->name,
                                            what));
                    s[g] = T.error();
                } else {
                    check_bounds(g, s[g], span);
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            const Type* pt = substitute(T, ptypes[i], s);
            if (vals[i] != nullptr) {
                vals[i] = coerce(vals[i], pt, slot[i]->value->span, "argument");
            } else if (slot[i] != nullptr) {
                vals[i] = expect_type(slot[i]->value, pt, "argument");
            } else if (f->params[i].default_value != nullptr) {
                vals[i] = f->params[i].default_value;
            } else {
                error(span, std::format("missing argument '{}' in call to {}", f->params[i].name, what));
                vals[i] = error_node(span);
            }
        }
        out.args = std::move(vals);
        out.ret = substitute(T, substitute(T, f->ret, pre), s);
        allow_coro_ = saved_allow;
        return out;
    }

    void check_coroutine_use(Function* f, Span span) {
        if (!f->is_coroutine || allow_coro_) return;
        std::string_view n = f->decl != nullptr ? f->decl->name.name : std::string_view(f->name);
        error(span, std::format("'{}' is a coroutine: run it with 'start({}(...))', or 'await {}(...)' inside "
                                "another coroutine",
                                n, n, n));
    }

    void check_args_loosely(const List<Arg>& args) {
        for (const Arg& a : args) expr(a.value);
    }

    ir::Expr* call(CallExpr* c, const Type* expected) {
        bool saved = allow_coro_;
        allow_coro_ = c == coro_call_ok_;
        if (allow_coro_) coro_call_ok_ = nullptr;
        ir::Expr* x = call_inner(c, expected);
        allow_coro_ = saved;
        return x;
    }

    ir::Expr* call_inner(CallExpr* c, const Type* expected) {
        Expr* callee = c->callee;
        if (auto* id = as<IdentExpr>(callee)) {
            std::string_view name = id->name;
            if (ir::Expr* v = lookup_var(cx_, name, id->span)) return closure_call(v, c);
            if (RecordInfo* r = cx_->record) {
                if (Function* m = find_method(r, name)) {
                    if (m->kind == FnKind::Static) return direct_call(m, c, id->type_args, expected);
                    if (cx_->is_static) {
                        error(id->span, std::format("instance method '{}' cannot be called here (no 'self')", name));
                        check_args_loosely(c->args);
                        return error_node(c->span);
                    }
                    return method_call(self_expr(id->span), m, c->args, id->type_args, c->span, expected);
                }
                if (find_field(r, name) != nullptr || find_static(r, name) != nullptr) {
                    return closure_call(member_value(name, id->span), c);
                }
            }
            if (TraitInfo* tr = cx_->trait; tr != nullptr && tr->methods.count(name) != 0 && !cx_->is_static) {
                Ident n{name, id->span};
                return method_call_on(self_expr(id->span), n, c, id->type_args, expected);
            }
            if (const Program::Symbol* s = global_symbol(name)) {
                switch (s->kind) {
                    case Program::SymKind::Function:
                        return direct_call(static_cast<Function*>(s->ptr), c, id->type_args, expected);
                    case Program::SymKind::Class:
                        return construct(static_cast<ClassInfo*>(s->ptr), id->type_args, c, expected);
                    case Program::SymKind::Struct:
                        error(id->span, std::format("structs are built with a literal: '{} {{ field = value }}'", name));
                        check_args_loosely(c->args);
                        return error_node(c->span);
                    case Program::SymKind::Global:
                        return closure_call(global_ref(static_cast<GlobalVar*>(s->ptr), id->span), c);
                    default:
                        error(id->span, std::format("'{}' cannot be called", name));
                        check_args_loosely(c->args);
                        return error_node(c->span);
                }
            }
            if (name == "List") return list_new(id->type_args, c, expected);
            if (name == "Map" || name == "Set" || name == "Deque" || name == "PriorityQueue" || name == "StrBuilder") {
                return collection_new(name, id->type_args, c, expected);
            }
            if (name == "Ok" || name == "Err") return result_ctor(name == "Ok", c, expected);
            if (auto d = vec_dim(name)) return vec_new(*d, c);
            if (is_await_only(name)) {
                error(id->span, std::format("'{}' can only be awaited, inside a coroutine: 'await {}(...)'", name,
                                            name));
                check_args_loosely(c->args);
                return error_node(c->span);
            }
            if (auto b = builtin_fn(name)) return builtin_call(*b, c, expected);
            error(id->span, std::format("unknown function '{}'", name));
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        if (auto* m = as<MemberExpr>(callee)) return member_call(m, c, expected);
        return closure_call(expr(callee), c);
    }

    ir::Expr* direct_call(Function* f, CallExpr* c, const List<TypeExpr*>& type_args, const Type* expected) {
        BoundCall b = bind_call(f, {}, f->generics, c->args, type_args, c->span, expected);
        check_coroutine_use(f, c->span);
        auto* n = mk<ir::Call>(c->span, b.ret);
        n->fn = f;
        n->args = list(b.args);
        return n;
    }

    ir::Expr* method_call(ir::Expr* recv, Function* f, const List<Arg>& args, const List<TypeExpr*>& type_args,
                          Span span, const Type* expected, bool force_direct = false) {
        Subst pre = f->owner_kind == Function::Owner::Trait ? Subst{} : record_subst(T.non_null(recv->type), f->owner);
        BoundCall b = bind_call(f, pre, f->generics, args, type_args, span, expected);
        return method_call_args(recv, f, b.args, span, b.ret, force_direct);
    }

    ir::Expr* method_call_args(ir::Expr* recv, Function* f, const std::vector<ir::Expr*>& args, Span span,
                               const Type* ret = nullptr, bool force_direct = false) {
        if (ret == nullptr) {
            ensure_signature(f);
            Subst pre = f->owner_kind == Function::Owner::Trait ? Subst{}
                                                                 : record_subst(T.non_null(recv->type), f->owner);
            ret = substitute(T, f->ret, pre);
        }
        check_coroutine_use(f, span);
        auto* n = mk<ir::MethodCall>(span, ret);
        n->fn = f;
        n->receiver = recv;
        n->args = list(args);
        if (!force_direct) {
            if (ClassInfo* c = T.non_null(recv->type)->cls()) {
                int slot = vslot_of(c, f);
                if (slot >= 0 && !f->is_final) {
                    n->is_virtual = true;
                    n->vslot = slot;
                }
            }
        }
        return n;
    }

    ir::Expr* closure_call(ir::Expr* callee, CallExpr* c) {
        const Type* t = callee->type;
        if (t->is_error()) {
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        if (t->tag != TypeTag::Fn) {
            error(c->callee->span, std::format("a value of type {} cannot be called", q(t)));
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        std::vector<ir::Expr*> args;
        std::size_t i = 0;
        for (const Arg& a : c->args) {
            if (!a.name.empty()) {
                error(a.name.span, "named arguments cannot be used when calling a function value");
            }
            if (i >= t->args.size()) {
                error(a.span, std::format("too many arguments: the function takes {}", t->args.size()));
                expr(a.value);
                continue;
            }
            args.push_back(expect_type(a.value, t->args[i], "argument"));
            ++i;
        }
        if (args.size() < t->args.size()) {
            error(c->span, std::format("missing arguments: the function takes {}, got {}", t->args.size(),
                                       args.size()));
            while (args.size() < t->args.size()) args.push_back(error_node(c->span));
        }
        auto* n = mk<ir::ClosureCall>(c->span, t->ret);
        n->callee = callee;
        n->args = list(args);
        return n;
    }

    ir::Expr* construct(ClassInfo* cls, const List<TypeExpr*>& type_args, CallExpr* c, const Type* expected) {
        ensure_members(cls);
        Function* init = nullptr;
        for (ClassInfo* x = cls; x != nullptr && init == nullptr; x = x->base) init = x->init;
        Subst s;
        std::vector<GenericParamInfo*> infer = cls->generics;
        if (!type_args.empty()) {
            if (type_args.size() != cls->generics.size()) {
                error(c->span, std::format("'{}' takes {} type argument{}", cls->name, cls->generics.size(),
                                           cls->generics.size() == 1 ? "" : "s"));
            } else {
                for (std::size_t i = 0; i < type_args.size(); ++i) {
                    s[cls->generics[i]] = resolve_type(type_args[i]);
                    check_bounds(cls->generics[i], s[cls->generics[i]], type_args[i]->span);
                }
            }
            infer.clear();
        } else if (const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
                   ex != nullptr && ex->cls() == cls && !cls->generics.empty()) {
            for (std::size_t i = 0; i < cls->generics.size(); ++i) s[cls->generics[i]] = ex->args[i];
            infer.clear();
        }
        std::vector<ir::Expr*> args;
        if (init != nullptr) {
            Subst pre = record_subst(cls->self_type, init->owner);
            for (auto& [k, v] : pre) pre[k] = substitute(T, v, s);
            BoundCall b = bind_call(init, pre, infer, c->args, {}, c->span, nullptr);
            for (auto& [k, v] : b.subst) s[k] = v;
            args = std::move(b.args);
        } else {
            if (!c->args.empty()) {
                error(c->span, std::format("'{}' takes no arguments (it declares no 'init')", cls->name));
                check_args_loosely(c->args);
            }
            for (GenericParamInfo* g : infer) {
                if (s.count(g) == 0) {
                    error(c->span, std::format("cannot infer type argument '{}' of '{}'; write '{}<...>()'", g->name,
                                               cls->name, cls->name));
                    s[g] = T.error();
                }
            }
        }
        std::vector<const Type*> targs;
        for (GenericParamInfo* g : cls->generics) targs.push_back(s.count(g) != 0 ? s[g] : T.error());
        auto* n = mk<ir::New>(c->span, T.class_(cls, std::move(targs)));
        n->arena = cls->manual ? ArenaKind::Alloc : ArenaKind::None; // DESIGN.md 5.10: every instance is manual
        n->cls = cls;
        n->init = init;
        n->args = list(args);
        return n;
    }

    ir::Expr* list_new(const List<TypeExpr*>& type_args, CallExpr* c, const Type* expected) {
        const Type* elem = nullptr;
        if (type_args.size() == 1) {
            elem = resolve_type(type_args[0]);
        } else if (!type_args.empty()) {
            error(c->span, "'List' takes one type argument: List<T>()");
            elem = T.error();
        } else if (const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
                   ex != nullptr && ex->tag == TypeTag::List) {
            elem = ex->args[0];
        } else {
            error(c->span, "cannot infer the element type; write 'List<T>()'");
            elem = T.error();
        }
        if (!c->args.empty()) {
            error(c->span, "'List<T>()' takes no arguments");
            check_args_loosely(c->args);
        }
        auto* n = mk<ir::BuiltinCall>(c->span, T.list(elem));
        n->builtin = Builtin::ListNew;
        return n;
    }

    ir::Expr* member_call(MemberExpr* m, CallExpr* c, const Type* expected) {
        if (as<SuperExpr>(m->object) != nullptr) return super_call(m, c, expected);
        if (const Program::Symbol* s = type_target(m->object)) return static_call(*s, m, c, expected);
        ir::Expr* obj = expr(m->object);
        if (m->safe) {
            return safe_nav(obj, c->span, [&](ir::Expr* tmp) {
                return method_call_on(tmp, m->member, c, m->type_args, expected);
            });
        }
        return method_call_on(obj, m->member, c, m->type_args, expected);
    }

    ir::Expr* super_call(MemberExpr* m, CallExpr* c, const Type* expected) {
        ClassInfo* cls = cx_->record != nullptr && cx_->record->is_class ? static_cast<ClassInfo*>(cx_->record)
                                                                         : nullptr;
        if (cls == nullptr || cls->base == nullptr || cx_->is_static) {
            error(m->object->span, "'super' is only available in methods of a class that extends another");
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        Function* f = m->member.name == "init" ? nullptr : find_method(cls->base, m->member.name);
        if (m->member.name == "init") {
            for (ClassInfo* x = cls->base; x != nullptr && f == nullptr; x = x->base) f = x->init;
        }
        if (f == nullptr || f->kind == FnKind::Static) {
            error(m->member.span, std::format("'{}' has no method '{}'", cls->base->name, m->member.name));
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        ir::Expr* self = self_expr(m->object->span);
        // `self` viewed as the base type, for generic substitution.
        const Type* base_view = upcast(self->type, cls->base);
        auto* recv = mk<ir::Local>(self->span, base_view != nullptr ? base_view : self->type);
        if (auto l = ir::as<ir::Local>(self)) {
            recv->var = l->var;
            return method_call(recv, f, c->args, m->type_args, c->span, expected, true);
        }
        return method_call(self, f, c->args, m->type_args, c->span, expected, true);
    }

    ir::Expr* static_call(const Program::Symbol& s, MemberExpr* m, CallExpr* c, const Type* expected) {
        if (s.kind == Program::SymKind::Enum) {
            auto* e = static_cast<EnumInfo*>(s.ptr);
            int vi = e->variant_index(m->member.name);
            if (vi < 0) {
                error(m->member.span, std::format("enum '{}' has no variant '{}'", e->name, m->member.name));
                check_args_loosely(c->args);
                return error_node(c->span);
            }
            const VariantInfo& v = e->variants[vi];
            if (v.field_types.empty()) {
                error(c->span, std::format("variant '{}.{}' has no payload; write '{}.{}' without parentheses",
                                           e->name, v.name, e->name, v.name));
                check_args_loosely(c->args);
                return error_node(c->span);
            }
            std::vector<const Arg*> slot(v.field_types.size(), nullptr);
            map_args(c->args, v.field_names, slot, std::format("'{}.{}'", e->name, v.name));
            std::vector<ir::Expr*> args;
            for (std::size_t i = 0; i < slot.size(); ++i) {
                if (slot[i] == nullptr) {
                    error(c->span, std::format("missing payload field '{}' for '{}.{}'", v.field_names[i], e->name,
                                               v.name));
                    args.push_back(error_node(c->span));
                } else {
                    args.push_back(expect_type(slot[i]->value, v.field_types[i], "payload"));
                }
            }
            auto* n = mk<ir::EnumNew>(c->span, e->type);
            n->en = e;
            n->variant = static_cast<std::uint32_t>(vi);
            n->args = list(args);
            return n;
        }
        auto* r = static_cast<RecordInfo*>(s.ptr);
        if (Function* f = find_method(r, m->member.name)) {
            check_private(m->member.name, r, m->member.span);
            if (f->kind != FnKind::Static) {
                error(m->member.span, std::format("'{}' is an instance method; call it on a value", m->member.name));
                check_args_loosely(c->args);
                return error_node(c->span);
            }
            return direct_call(f, c, m->type_args, expected);
        }
        if (GlobalVar* g = find_static(r, m->member.name)) return closure_call(global_ref(g, m->span), c);
        error(m->member.span, std::format("'{}' has no static method '{}'", r->name, m->member.name));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    ir::Expr* method_call_on(ir::Expr* obj, Ident name, CallExpr* c, const List<TypeExpr*>& type_args,
                             const Type* expected) {
        const Type* t = obj->type;
        switch (t->tag) {
            case TypeTag::Error: check_args_loosely(c->args); return error_node(c->span);
            case TypeTag::Class:
            case TypeTag::Struct: {
                RecordInfo* r = record_of(t);
                if (Function* f = find_method(r, name.name)) {
                    if (f->owner_kind != Function::Owner::Trait) {
                        check_private(name.name, static_cast<RecordInfo*>(f->owner), name.span);
                    }
                    if (f->kind == FnKind::Static) {
                        error(name.span, std::format("'{}' is static; call it as '{}.{}(...)'", name.name, r->name,
                                                     name.name));
                        check_args_loosely(c->args);
                        return error_node(c->span);
                    }
                    if (name.name == "init") {
                        error(name.span, "'init' runs when the object is constructed and cannot be called directly");
                        check_args_loosely(c->args);
                        return error_node(c->span);
                    }
                    return method_call(obj, f, c->args, type_args, c->span, expected);
                }
                if (FieldInfo* f = find_field(r, name.name)) {
                    check_private(name.name, static_cast<RecordInfo*>(f->owner), name.span);
                    return closure_call(field_access(obj, f, name.span), c);
                }
                error(name.span, std::format("{} has no method '{}'", q(t), name.name));
                check_args_loosely(c->args);
                return error_node(c->span);
            }
            case TypeTag::List:
            case TypeTag::Array: return list_method(obj, name, c);
            case TypeTag::Str: return str_method(obj, name, c);
            case TypeTag::Map: return map_method(obj, name, c);
            case TypeTag::Set: return set_method(obj, name, c);
            case TypeTag::Deque: return deque_method(obj, name, c);
            case TypeTag::PQueue: return pqueue_method(obj, name, c);
            case TypeTag::StrBuilder: return str_builder_method(obj, name, c);
            case TypeTag::Result: return result_method(obj, name, c);
            case TypeTag::Num: return num_method(obj, name, c);
            case TypeTag::Vec: return vec_method(obj, name, c);
            case TypeTag::Task: return task_method(obj, name, c);
            case TypeTag::Signal: return signal_method(obj, name, c);
            case TypeTag::Param:
            case TypeTag::Trait: {
                std::vector<TraitInfo*> traits;
                if (auto p = t->param()) {
                    traits = p->bounds;
                } else {
                    traits.push_back(t->tr());
                }
                for (TraitInfo* tr : traits) {
                    if (tr->builtin != BuiltinTrait::None) continue;
                    ensure_trait(tr);
                    auto it = tr->methods.find(name.name);
                    if (it == tr->methods.end()) continue;
                    BoundCall b = bind_call(it->second, {}, it->second->generics, c->args, type_args, c->span,
                                            expected);
                    auto* n = mk<ir::DynMethodCall>(c->span, b.ret);
                    n->name = A.str(name.name);
                    n->receiver = obj;
                    n->args = list(b.args);
                    return n;
                }
                error(name.span, std::format("{} has no method '{}'{}", q(t), name.name,
                                             t->tag == TypeTag::Param ? " (add a trait bound that declares it)" : ""));
                check_args_loosely(c->args);
                return error_node(c->span);
            }
            case TypeTag::Nullable:
                if (ir::Expr* x = optional_method(obj, name, c)) return x;
                error(name.span, std::format("value of type {} may be null; unwrap it with 'if x? v {{ }}', '?\?' "
                                             "or 'unwrap_or(default)'",
                                             q(t)));
                check_args_loosely(c->args);
                return error_node(c->span);
            default:
                error(name.span, std::format("{} has no method '{}'", q(t), name.name));
                check_args_loosely(c->args);
                return error_node(c->span);
        }
    }

    // Positional-only argument checking for built-ins.
    std::vector<ir::Expr*> builtin_args(CallExpr* c, const std::vector<const Type*>& types, std::size_t required,
                                        std::string_view what) {
        std::vector<ir::Expr*> out;
        for (const Arg& a : c->args) {
            if (!a.name.empty()) error(a.name.span, std::format("{} takes positional arguments only", what));
            if (out.size() >= types.size()) {
                error(a.span, std::format("too many arguments to {}", what));
                expr(a.value);
                continue;
            }
            out.push_back(expect_type(a.value, types[out.size()], "argument"));
        }
        if (out.size() < required) {
            error(c->span, std::format("{} needs {} argument{}", what, required, required == 1 ? "" : "s"));
        }
        return out;
    }

    ir::Expr* builtin_node(Builtin b, Span span, const Type* type, std::vector<ir::Expr*> args) {
        auto* n = mk<ir::BuiltinCall>(span, type);
        n->builtin = b;
        n->args = list(args);
        return n;
    }

    ir::Expr* builtin_call(Builtin b, CallExpr* c, const Type* expected) {
        std::string what = std::format("'{}'", builtin_name(b));
        switch (b) {
            case Builtin::Print: {
                if (c->args.size() != 1 || !c->args[0].name.empty()) {
                    error(c->span, "'print' takes one argument");
                    check_args_loosely(c->args);
                    return error_node(c->span);
                }
                ir::Expr* x = expr(c->args[0].value);
                return builtin_node(b, c->span, T.void_(), {to_str(x, c->args[0].value->span)});
            }
            case Builtin::Assert: {
                auto args = builtin_args(c, {T.bool_(), T.str()}, 1, what);
                if (args.size() == 1) args.push_back(str_const(c->span, ""));
                if (args.size() < 2) return error_node(c->span);
                return builtin_node(b, c->span, T.void_(), args);
            }
            case Builtin::Panic: {
                auto args = builtin_args(c, {T.str()}, 1, what);
                if (args.size() != 1) return error_node(c->span);
                return builtin_node(b, c->span, T.void_(), args);
            }
            case Builtin::Sin:
            case Builtin::Cos:
            case Builtin::Tan:
            case Builtin::Asin:
            case Builtin::Acos:
            case Builtin::Atan:
            case Builtin::Exp:
            case Builtin::Log:
            case Builtin::Log2:
            case Builtin::Log10:
            case Builtin::RadToDeg:
            case Builtin::DegToRad:
            case Builtin::WrapAngle: {
                auto args = builtin_args(c, {T.float_()}, 1, what);
                if (args.size() != 1) return error_node(c->span);
                return builtin_node(b, c->span, T.float_(), args);
            }
            case Builtin::Atan2:
            case Builtin::Pow:
            case Builtin::Step: {
                auto args = builtin_args(c, {T.float_(), T.float_()}, 2, what);
                if (args.size() != 2) return error_node(c->span);
                return builtin_node(b, c->span, T.float_(), args);
            }
            case Builtin::Lerp:
            case Builtin::InverseLerp:
            case Builtin::Smoothstep:
            case Builtin::MoveToward:
            case Builtin::Wrap: {
                auto args = builtin_args(c, {T.float_(), T.float_(), T.float_()}, 3, what);
                if (args.size() != 3) return error_node(c->span);
                return builtin_node(b, c->span, T.float_(), args);
            }
            case Builtin::Remap: {
                auto args = builtin_args(c, {T.float_(), T.float_(), T.float_(), T.float_(), T.float_()}, 5, what);
                if (args.size() != 5) return error_node(c->span);
                return builtin_node(b, c->span, T.float_(), args);
            }
            case Builtin::ParseInt: {
                auto args = builtin_args(c, {T.str(), T.int_()}, 1, what);
                if (args.empty()) return error_node(c->span);
                if (args.size() == 1) args.push_back(int_const(c->span, T.int_(), 10));
                return builtin_node(b, c->span, T.nullable(T.int_()), args);
            }
            case Builtin::ParseFloat: {
                auto args = builtin_args(c, {T.str()}, 1, what);
                if (args.size() != 1) return error_node(c->span);
                return builtin_node(b, c->span, T.nullable(T.float_()), args);
            }
            case Builtin::FromAngle: {
                auto args = builtin_args(c, {T.float_(), T.float_()}, 1, what);
                if (args.empty()) return error_node(c->span);
                if (args.size() == 1) args.push_back(float_const(c->span, NumKind::F32, 1.0));
                return builtin_node(b, c->span, T.vec(2), args);
            }
            case Builtin::Min:
            case Builtin::Max:
            case Builtin::Abs:
            case Builtin::Sign:
            case Builtin::Clamp:
            case Builtin::Floor:
            case Builtin::Ceil:
            case Builtin::Round:
            case Builtin::Sqrt:
            case Builtin::WrappingAdd:
            case Builtin::WrappingSub:
            case Builtin::WrappingMul:
            case Builtin::CheckedAdd:
            case Builtin::CheckedSub:
            case Builtin::CheckedMul:
            case Builtin::SaturatingAdd:
            case Builtin::SaturatingSub:
            case Builtin::SaturatingMul: {
                bool int_only = b >= Builtin::WrappingAdd && b <= Builtin::SaturatingMul;
                std::size_t n = b == Builtin::Clamp ? 3 : (b == Builtin::Min || b == Builtin::Max || int_only) ? 2 : 1;
                if (c->args.size() != n) {
                    error(c->span, std::format("{} takes {} argument{}", what, n, n == 1 ? "" : "s"));
                    check_args_loosely(c->args);
                    return error_node(c->span);
                }
                // The first non-literal argument fixes the numeric type.
                const Type* t = nullptr;
                std::vector<ir::Expr*> args(n, nullptr);
                for (std::size_t i = 0; i < n; ++i) {
                    if (!c->args[i].name.empty()) error(c->args[i].name.span, std::format("{} takes positional arguments only", what));
                    if (is_untyped_literal(c->args[i].value)) continue;
                    args[i] = expr(c->args[i].value, num_hint(expected));
                    if (t == nullptr) t = args[i]->type;
                }
                if (t == nullptr) t = num_hint(expected) != nullptr ? num_hint(expected) : nullptr;
                for (std::size_t i = 0; i < n; ++i) {
                    if (args[i] == nullptr) {
                        args[i] = expr(c->args[i].value, t);
                        if (t == nullptr) t = args[i]->type;
                    }
                }
                if (t->is_error()) return error_node(c->span);
                bool needs_float = b == Builtin::Sqrt || b == Builtin::Floor || b == Builtin::Ceil || b == Builtin::Round;
                bool ok = t->is_num() && (!needs_float || t->is_floating()) &&
                          ((b != Builtin::Abs && b != Builtin::Sign) || is_signed(t->num) || is_float(t->num));
                if (int_only) ok = t->is_num() && !t->is_floating();
                if (!ok) {
                    error(c->span, std::format("{} is not defined for {}", what, q(t)));
                    return error_node(c->span);
                }
                for (std::size_t i = 0; i < n; ++i) args[i] = coerce(args[i], t, c->args[i].value->span, "argument");
                bool checked = b >= Builtin::CheckedAdd && b <= Builtin::CheckedMul;
                return builtin_node(b, c->span, checked ? T.nullable(t) : t, args);
            }
            default: break;
        }
        return error_node(c->span);
    }

    ir::Expr* list_method(ir::Expr* obj, Ident name, CallExpr* c) {
        if (ir::Expr* x = list_lib_method(obj, name, c)) return x;
        if (obj->type->tag == TypeTag::Array) {
            error(name.span, std::format("a fixed-size array has no method '{}'{}", name.name,
                                         name.name == "push" || name.name == "pop" || name.name == "insert" ||
                                                 name.name == "remove" || name.name == "clear"
                                             ? " (its length is fixed; use a List)"
                                             : ""));
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        const Type* elem = obj->type->args[0];
        std::string what = std::format("'List.{}'", name.name);
        struct Sig {
            std::string_view name;
            Builtin b;
            std::vector<const Type*> params;
            const Type* ret;
        };
        const Sig sigs[] = {
            {"len", Builtin::ListLen, {}, T.int_()},
            {"push", Builtin::ListPush, {elem}, T.void_()},
            {"pop", Builtin::ListPop, {}, elem},
            {"clear", Builtin::ListClear, {}, T.void_()},
            {"insert", Builtin::ListInsert, {T.int_(), elem}, T.void_()},
            {"remove", Builtin::ListRemove, {T.int_()}, elem},
            {"is_empty", Builtin::ListIsEmpty, {}, T.bool_()},
            {"contains", Builtin::ListContains, {elem}, T.bool_()},
        };
        for (const Sig& s : sigs) {
            if (s.name != name.name) continue;
            std::vector<ir::Expr*> args{obj};
            auto rest = builtin_args(c, s.params, s.params.size(), what);
            if (rest.size() != s.params.size()) return error_node(c->span);
            if (s.b == Builtin::ListPush || s.b == Builtin::ListInsert) {
                check_escape(rest.back(), holder_life(obj), c->span, "a longer-lived list");
            }
            args.insert(args.end(), rest.begin(), rest.end());
            return builtin_node(s.b, c->span, s.ret, args);
        }
        error(name.span, std::format("'List' has no method '{}'", name.name));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    // Built-in methods of the value types (receiver first).
    struct MethodSig {
        std::string_view name;
        Builtin b;
        std::vector<const Type*> params;
        const Type* ret;
    };

    ir::Expr* builtin_method(ir::Expr* obj, Ident name, CallExpr* c, const std::vector<MethodSig>& sigs,
                             std::string_view type_label) {
        std::string what = std::format("'{}.{}'", type_label, name.name);
        for (const MethodSig& s : sigs) {
            if (s.name != name.name) continue;
            std::vector<ir::Expr*> args{obj};
            auto rest = builtin_args(c, s.params, s.params.size(), what);
            if (rest.size() != s.params.size()) return error_node(c->span);
            if (s.b == Builtin::SignalConnect) check_escape(rest.back(), Life::Long, c->span, "a signal's handlers");
            args.insert(args.end(), rest.begin(), rest.end());
            return builtin_node(s.b, c->span, s.ret, args);
        }
        error(name.span, std::format("'{}' has no method '{}'", type_label, name.name));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    ir::Expr* vec_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* v = obj->type;
        const Type* f = T.float_();
        std::vector<MethodSig> sigs = {
            {"length", Builtin::VecLength, {}, f},
            {"length_squared", Builtin::VecLengthSq, {}, f},
            {"dot", Builtin::VecDot, {v}, f},
            {"normalized", Builtin::VecNormalized, {}, v},
            {"distance", Builtin::VecDistance, {v}, f},
            {"distance_squared", Builtin::VecDistanceSq, {v}, f},
            {"lerp", Builtin::VecLerp, {v, f}, v},
            {"abs", Builtin::VecAbs, {}, v},
            {"min", Builtin::VecMin, {v}, v},
            {"max", Builtin::VecMax, {v}, v},
            {"clamp", Builtin::VecClamp, {v, v}, v},
            {"reflect", Builtin::VecReflect, {v}, v},
            {"project", Builtin::VecProject, {v}, v},
            {"slide", Builtin::VecSlide, {v}, v},
            {"clamp_length", Builtin::VecClampLength, {f, f}, v},
            {"floor", Builtin::VecFloor, {}, v},
            {"ceil", Builtin::VecCeil, {}, v},
            {"round", Builtin::VecRound, {}, v},
            {"fract", Builtin::VecFract, {}, v},
            {"sign", Builtin::VecSign, {}, v},
            {"snapped", Builtin::VecSnapped, {f}, v},
            {"bounce", Builtin::VecBounce, {v}, v},
            {"move_toward", Builtin::VecMoveToward, {v, f}, v},
            {"direction_to", Builtin::VecDirectionTo, {v}, v},
            {"limit_length", Builtin::VecLimitLength, {f}, v},
        };
        if (v->dim == 2) {
            sigs.push_back({"rotated", Builtin::VecRotated, {f}, v});
            sigs.push_back({"angle", Builtin::VecAngle, {}, f});
            sigs.push_back({"perpendicular", Builtin::VecPerpendicular, {}, v});
            sigs.push_back({"angle_to", Builtin::VecAngleTo, {v}, f});
            sigs.push_back({"slerp", Builtin::VecSlerp, {v, f}, v});
            sigs.push_back({"cross", Builtin::VecCross2, {v}, f});
        } else if (v->dim == 3) {
            sigs.push_back({"cross", Builtin::VecCross3, {v}, v});
        }
        return builtin_method(obj, name, c, sigs, type_name(v));
    }

    ir::Expr* task_method(ir::Expr* obj, Ident name, CallExpr* c) {
        return builtin_method(obj, name, c,
                              {{"cancel", Builtin::TaskCancel, {}, T.void_()},
                               {"is_done", Builtin::TaskIsDone, {}, T.bool_()}},
                              "Task");
    }

    ir::Expr* signal_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* handler = T.fn(obj->type->args, T.void_());
        return builtin_method(obj, name, c,
                              {{"connect", Builtin::SignalConnect, {handler}, T.void_()},
                               {"disconnect", Builtin::SignalDisconnect, {handler}, T.void_()}},
                              "Signal");
    }

    // `vec3(x, y, z)`, `vec3(v2, z)`, `vec4(0.0)` (LANGUAGE.md 4.2).
    ir::Expr* vec_new(std::uint32_t dim, CallExpr* c) {
        std::vector<ir::Expr*> parts;
        std::uint32_t total = 0;
        bool bad = false;
        for (const Arg& a : c->args) {
            if (!a.name.empty()) error(a.name.span, std::format("'vec{}' takes positional arguments only", dim));
            ir::Expr* x = expr(a.value, T.float_());
            const Type* t = x->type;
            if (t == T.float_()) {
                total += 1;
            } else if (t->is_vec()) {
                total += t->dim;
            } else if (!t->is_error()) {
                error(a.value->span, std::format("vector components must be 'float' or vectors, found {}{}", q(t),
                                                 t->is_num() ? "; convert with 'as float'" : ""));
                bad = true;
            } else {
                bad = true;
            }
            parts.push_back(value_copy(x));
        }
        bool splat = parts.size() == 1 && parts[0]->type == T.float_();
        if (!bad && !splat && total != dim) {
            error(c->span, std::format("'vec{}' needs {} component{}, got {}", dim, dim, dim == 1 ? "" : "s", total));
            bad = true;
        }
        if (bad) return error_node(c->span);
        auto* n = mk<ir::VecNew>(c->span, T.vec(dim));
        n->parts = list(parts);
        return n;
    }

    // ------------------------------------------------------------ coroutines

    // The coroutine function called by `x` (a Call / MethodCall, possibly
    // under `?.`), or null.
    Function* coroutine_called(ir::Expr* x) {
        if (auto s = ir::as<ir::SafeNav>(x)) x = s->body;
        if (auto c = ir::as<ir::Call>(x)) return c->fn->is_coroutine ? c->fn : nullptr;
        if (auto m = ir::as<ir::MethodCall>(x)) return m->fn->is_coroutine ? m->fn : nullptr;
        return nullptr;
    }

    // Can `await` be used here? Reports why not.
    bool await_allowed(Span span, std::string_view what) {
        if (cx_->fn->kind == FnKind::Lambda) return false; // reported once, at the lambda
        if (!cx_->fn->is_coroutine) {
            error(span, std::format("'{}' can only be used inside a function (which makes it a coroutine)", what));
            return false;
        }
        if (cx_->defer_depth > 0) {
            error(span, std::format("'{}' cannot be used inside 'defer'", what));
            return false;
        }
        return true;
    }

    ir::Expr* await_node(Span span, ir::AwaitKind kind, ir::Expr* operand, const Type* type) {
        auto* n = mk<ir::Await>(span, type);
        n->await_kind = kind;
        n->operand = operand;
        return n;
    }

    // `await x` (DESIGN.md 5.9): built-in awaitables, coroutine calls, tasks
    // and signals.
    ir::Expr* await_expr(AwaitExpr* a) {
        bool ok = await_allowed(a->span, "await");
        Expr* op = a->operand;
        auto* c = as<CallExpr>(op);
        if (auto* id = c != nullptr ? as<IdentExpr>(c->callee) : nullptr;
            id != nullptr && is_await_only(id->name) && !has_local(id->name) &&
            (cx_->record == nullptr || (find_method(cx_->record, id->name) == nullptr &&
                                        find_field(cx_->record, id->name) == nullptr)) &&
            global_symbol(id->name) == nullptr) {
            std::string what = std::format("'{}'", id->name);
            ir::AwaitKind kind = ir::AwaitKind::Seconds;
            const Type* param = T.float_();
            if (id->name == "wait_ticks") {
                kind = ir::AwaitKind::Ticks;
                param = T.int_();
            } else if (id->name == "until") {
                kind = ir::AwaitKind::Until;
                param = T.fn({}, T.bool_());
            }
            auto args = builtin_args(c, {param}, 1, what);
            if (args.size() != 1 || !ok) return error_node(a->span);
            return await_node(a->span, kind, args[0], T.void_());
        }
        ir::Expr* x = nullptr;
        if (c != nullptr) {
            const CallExpr* saved = coro_call_ok_;
            coro_call_ok_ = c;
            x = expr(op);
            coro_call_ok_ = saved;
            if (coroutine_called(x) != nullptr) {
                return ok ? await_node(a->span, ir::AwaitKind::Call, x, x->type) : error_node(a->span);
            }
        } else {
            x = expr(op);
        }
        const Type* t = x->type;
        if (t->is_error() || !ok) return error_node(a->span);
        if (t->tag == TypeTag::Task) return await_node(a->span, ir::AwaitKind::Task, x, T.void_());
        if (t->tag == TypeTag::Signal) {
            if (t->args.size() > 1) {
                error(a->span, "awaiting a signal with more than one parameter is not supported yet; 'connect' a "
                               "handler instead");
                return error_node(a->span);
            }
            return await_node(a->span, ir::AwaitKind::Signal, x, t->args.empty() ? T.void_() : t->args[0]);
        }
        error(op->span, std::format("cannot await a value of type {}; await 'wait(seconds)', 'wait_ticks(n)', "
                                    "'until(condition)', a coroutine call, a Task or a signal",
                                    q(t)));
        return error_node(a->span);
    }

    // `start(f(args))`: runs a coroutine until its first suspension and
    // schedules the rest; it is owned by the starting method's object.
    ir::Expr* start_expr(StartExpr* s) {
        auto* c = as<CallExpr>(s->operand);
        ir::Expr* x = nullptr;
        if (c != nullptr) {
            const CallExpr* saved = coro_call_ok_;
            coro_call_ok_ = c;
            x = expr(c);
            coro_call_ok_ = saved;
        } else {
            x = expr(s->operand);
        }
        if (x->type->is_error()) return error_node(s->span);
        if (coroutine_called(x) == nullptr || (x->kind != ir::EK::Call && x->kind != ir::EK::MethodCall)) {
            error(s->operand->span, "'start' needs a call to a coroutine (a function that uses 'await' or 'yield')");
            return error_node(s->span);
        }
        // A coroutine can outlive the tick: frame objects may not go in.
        if (auto* mc = ir::as<ir::MethodCall>(x)) {
            check_escape(mc->receiver, Life::Long, mc->receiver->span, "a coroutine (it can outlive the tick)");
            for (ir::Expr* a : mc->args) check_escape(a, Life::Long, a->span, "a coroutine (it can outlive the tick)");
        } else if (auto* cc = ir::as<ir::Call>(x)) {
            for (ir::Expr* a : cc->args) check_escape(a, Life::Long, a->span, "a coroutine (it can outlive the tick)");
        }
        auto* n = mk<ir::Start>(s->span, T.task());
        n->call = x;
        n->owner = cx_->is_static ? nullptr : self_expr(s->span);
        return n;
    }

    ir::Expr* str_method(ir::Expr* obj, Ident name, CallExpr* c) {
        std::string what = std::format("'str.{}'", name.name);
        struct Sig {
            std::string_view name;
            Builtin b;
            std::vector<const Type*> params;
            const Type* ret;
        };
        const Sig sigs[] = {
            {"len", Builtin::StrLen, {}, T.int_()},
            {"contains", Builtin::StrContains, {T.str()}, T.bool_()},
            {"starts_with", Builtin::StrStartsWith, {T.str()}, T.bool_()},
            {"ends_with", Builtin::StrEndsWith, {T.str()}, T.bool_()},
        };
        for (const Sig& s : sigs) {
            if (s.name != name.name) continue;
            std::vector<ir::Expr*> args{obj};
            auto rest = builtin_args(c, s.params, s.params.size(), what);
            if (rest.size() != s.params.size()) return error_node(c->span);
            args.insert(args.end(), rest.begin(), rest.end());
            return builtin_node(s.b, c->span, s.ret, args);
        }
        if (ir::Expr* x = str_lib_method(obj, name, c)) return x;
        error(name.span, std::format("'str' has no method '{}'", name.name));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    // ------------------------------------------------------------ constants

    std::optional<ConstValue> const_eval(const ir::Expr* x) {
        switch (x->kind) {
            case ir::EK::Const: {
                const ConstValue& v = ir::as<ir::Const>(x)->value;
                if (v.kind == ConstValue::Kind::None) return std::nullopt;
                return v;
            }
            case ir::EK::Unary: {
                auto* u = ir::as<ir::Unary>(x);
                auto v = const_eval(u->operand);
                if (!v) return std::nullopt;
                if (u->op == UnaryOp::Not && v->kind == ConstValue::Kind::Bool) {
                    v->b = !v->b;
                    return v;
                }
                if (v->kind == ConstValue::Kind::Float && u->op == UnaryOp::Neg) {
                    v->f = -v->f;
                    return v;
                }
                if (v->kind == ConstValue::Kind::Int && x->type->is_int()) {
                    NumKind k = x->type->num;
                    if (u->op == UnaryOp::Neg) {
                        if (num::int_neg(k, v->i, v->i) != num::Status::Ok) {
                            error(x->span, std::format("constant expression overflows '{}'", to_string(k)));
                            return std::nullopt;
                        }
                    } else {
                        v->i = num::wrap(k, ~static_cast<std::uint64_t>(v->i));
                    }
                    return v;
                }
                return std::nullopt;
            }
            case ir::EK::Binary: {
                auto* b = ir::as<ir::Binary>(x);
                auto l = const_eval(b->lhs);
                auto r = const_eval(b->rhs);
                if (!l || !r) return std::nullopt;
                const Type* ot = b->operand_type;
                ConstValue out;
                if (ot->is_int() && l->kind == ConstValue::Kind::Int && r->kind == ConstValue::Kind::Int) {
                    NumKind k = ot->num;
                    if (is_arith(b->op) || is_bitwise(b->op)) {
                        out.kind = ConstValue::Kind::Int;
                        num::Status st = is_arith(b->op) ? num::int_arith(b->op, k, l->i, r->i, out.i)
                                                         : num::int_bits(b->op, k, l->i, r->i, out.i);
                        if (st != num::Status::Ok) {
                            error(x->span, st == num::Status::DivByZero
                                               ? std::string("division by zero in a constant expression")
                                               : std::format("constant expression overflows '{}'", to_string(k)));
                            return std::nullopt;
                        }
                        return out;
                    }
                    out.kind = ConstValue::Kind::Bool;
                    switch (b->op) {
                        case BinaryOp::Eq: out.b = l->i == r->i; break;
                        case BinaryOp::Ne: out.b = l->i != r->i; break;
                        case BinaryOp::Lt: out.b = num::int_less(k, l->i, r->i); break;
                        case BinaryOp::Le: out.b = !num::int_less(k, r->i, l->i); break;
                        case BinaryOp::Gt: out.b = num::int_less(k, r->i, l->i); break;
                        case BinaryOp::Ge: out.b = !num::int_less(k, l->i, r->i); break;
                        default: return std::nullopt;
                    }
                    return out;
                }
                if (ot->is_floating() && l->kind == ConstValue::Kind::Float && r->kind == ConstValue::Kind::Float) {
                    NumKind k = ot->num;
                    double a = l->f;
                    double c = r->f;
                    out.kind = ConstValue::Kind::Float;
                    switch (b->op) {
                        case BinaryOp::Add: out.f = round_to(k, a + c); return out;
                        case BinaryOp::Sub: out.f = round_to(k, a - c); return out;
                        case BinaryOp::Mul: out.f = round_to(k, a * c); return out;
                        case BinaryOp::Div: out.f = round_to(k, a / c); return out;
                        default: break;
                    }
                    if (k == NumKind::F32) {
                        // f32 arithmetic is done in float in the VM.
                        return std::nullopt;
                    }
                    return std::nullopt;
                }
                if (l->kind == ConstValue::Kind::Bool && r->kind == ConstValue::Kind::Bool) {
                    out.kind = ConstValue::Kind::Bool;
                    if (b->op == BinaryOp::Eq) out.b = l->b == r->b;
                    else if (b->op == BinaryOp::Ne) out.b = l->b != r->b;
                    else return std::nullopt;
                    return out;
                }
                return std::nullopt;
            }
            case ir::EK::Logical: {
                auto* lg = ir::as<ir::Logical>(x);
                auto l = const_eval(lg->lhs);
                auto r = const_eval(lg->rhs);
                if (!l || !r || l->kind != ConstValue::Kind::Bool || r->kind != ConstValue::Kind::Bool) {
                    return std::nullopt;
                }
                ConstValue out;
                out.kind = ConstValue::Kind::Bool;
                out.b = lg->is_and ? (l->b && r->b) : (l->b || r->b);
                return out;
            }
            case ir::EK::Concat: {
                std::string s;
                for (const ir::Expr* p : ir::as<ir::Concat>(x)->parts) {
                    auto v = const_eval(p);
                    if (!v || v->kind != ConstValue::Kind::Str) return std::nullopt;
                    s += v->s;
                }
                ConstValue out;
                out.kind = ConstValue::Kind::Str;
                out.s = A.str(s);
                return out;
            }
            case ir::EK::Cast: {
                auto* c = ir::as<ir::Cast>(x);
                if (c->op != ir::Cast::Op::Numeric || !x->type->is_num()) return std::nullopt;
                auto v = const_eval(c->operand);
                if (!v) return std::nullopt;
                NumKind to = x->type->num;
                const Type* from = c->operand->type;
                ConstValue out;
                if (is_float(to)) {
                    out.kind = ConstValue::Kind::Float;
                    if (v->kind == ConstValue::Kind::Float) out.f = round_to(to, v->f);
                    else if (v->kind == ConstValue::Kind::Int) out.f = round_to(to, num::int_to_double(from->num, v->i));
                    else return std::nullopt;
                    return out;
                }
                out.kind = ConstValue::Kind::Int;
                if (v->kind == ConstValue::Kind::Float) out.i = num::float_to_int(to, v->f);
                else if (v->kind == ConstValue::Kind::Int) out.i = num::wrap(to, static_cast<std::uint64_t>(v->i));
                else if (v->kind == ConstValue::Kind::Bool) out.i = v->b ? 1 : 0;
                else return std::nullopt;
                return out;
            }
            default: return std::nullopt;
        }
    }

    // ------------------------------------------------------------ manual memory
    // DESIGN.md 5.10, LANGUAGE.md 2.11.

    // `new(frame) T(args)`, `new(scene) T(args)`, `alloc T(args)`: T is a
    // class (constructed as usual) or `List<E>`.
    ir::Expr* manual_new(TypeExpr* te, const List<Arg>& args, ArenaKind arena, Span span, const Type* expected) {
        if (te == nullptr) {
            check_args_loosely(args);
            return error_node(span);
        }
        CallExpr call;
        call.kind = ExprKind::Call;
        call.span = span;
        call.args = args;
        if (auto* nt = as<NamedType>(te)) {
            if (nt->name.name == "List") {
                ir::Expr* x = list_new(nt->args, &call, expected);
                if (auto* b = ir::as<ir::BuiltinCall>(x)) b->arena = arena;
                return x;
            }
            if (const Program::Symbol* sym = global_symbol(nt->name.name);
                sym != nullptr && sym->kind == Program::SymKind::Class) {
                ir::Expr* x = construct(static_cast<ClassInfo*>(sym->ptr), nt->args, &call, expected);
                if (auto* n = ir::as<ir::New>(x)) n->arena = arena;
                return x;
            }
        }
        const Type* t = resolve_type(te);
        if (!t->is_error()) {
            error(te->span, std::format("{} cannot be allocated with '{}': only classes and lists can", q(t),
                                        arena == ArenaKind::Alloc ? "alloc" : "new"));
        }
        check_args_loosely(args);
        return error_node(span);
    }

    ir::Expr* free_expr(FreeExpr* f) {
        ir::Expr* x = expr(f->operand);
        const Type* t = T.non_null(x->type);
        if (!x->type->is_error() && t->tag != TypeTag::Class && t->tag != TypeTag::List) {
            error(f->operand->span,
                  std::format("'free' takes a manual object (a class instance or a list), not {}", q(x->type)));
        } else if (auto* n = ir::as<ir::New>(x); n != nullptr && n->arena == ArenaKind::None) {
            error(f->operand->span, "'free' needs a manual object; this one is automatic (reference-counted): "
                                    "allocate it with 'alloc' to free it yourself");
        } else if (auto* b = ir::as<ir::BuiltinCall>(x);
                   b != nullptr && b->builtin == Builtin::ListNew && b->arena == ArenaKind::None) {
            error(f->operand->span, "'free' needs a manual object; this list is automatic (reference-counted)");
        }
        auto* n = mk<ir::Free>(f->span, T.void_());
        n->operand = x;
        return n;
    }

    // Escape analysis: how long a value is known to live. Only the obvious
    // cases are tracked (DESIGN.md 5.10 "the compiler rejects obvious
    // escapes"); anything else is Long and the generation check catches the
    // rest at run time.
    enum class Life : std::uint8_t { Frame, Scene, Long, Global };

    static Life life_of_arena(ArenaKind a) {
        return a == ArenaKind::Frame ? Life::Frame : a == ArenaKind::Scene ? Life::Scene : Life::Long;
    }
    static ArenaKind arena_of_life(Life l) {
        return l == Life::Frame ? ArenaKind::Frame : l == Life::Scene ? ArenaKind::Scene : ArenaKind::None;
    }

    Life lifetime_of(const ir::Expr* x) {
        if (x == nullptr) return Life::Long;
        auto lower = [&](const List<ir::Expr*>& xs) {
            Life l = Life::Long;
            for (const ir::Expr* e : xs) l = std::min(l, lifetime_of(e));
            return l;
        };
        switch (x->kind) {
            case ir::EK::New: return life_of_arena(ir::as<ir::New>(x)->arena);
            case ir::EK::BuiltinCall: {
                auto* b = ir::as<ir::BuiltinCall>(x);
                return b->builtin == Builtin::ListNew ? life_of_arena(b->arena) : Life::Long;
            }
            case ir::EK::Local: return life_of_arena(ir::as<ir::Local>(x)->var->holds);
            case ir::EK::Capture: return life_of_arena(ir::as<ir::Capture>(x)->var->holds);
            case ir::EK::Copy: return lifetime_of(ir::as<ir::Copy>(x)->operand);
            case ir::EK::Cast: return lifetime_of(ir::as<ir::Cast>(x)->operand);
            case ir::EK::Ternary: {
                auto* t = ir::as<ir::Ternary>(x);
                return std::min(lifetime_of(t->then_expr), lifetime_of(t->else_expr));
            }
            case ir::EK::Coalesce: {
                auto* c = ir::as<ir::Coalesce>(x);
                return std::min(lifetime_of(c->lhs), lifetime_of(c->rhs));
            }
            case ir::EK::StructNew: return lower(ir::as<ir::StructNew>(x)->fields);
            case ir::EK::ListLit: return lower(ir::as<ir::ListLit>(x)->elements);
            case ir::EK::EnumNew: return lower(ir::as<ir::EnumNew>(x)->args);
            case ir::EK::Lambda: {
                Life l = Life::Long;
                for (const CaptureInfo& c : ir::as<ir::Lambda>(x)->fn->captures) {
                    l = std::min(l, life_of_arena(c.var->holds));
                }
                return l;
            }
            default: return Life::Long;
        }
    }

    // What a store into a field or element of `holder` must outlive.
    Life holder_life(const ir::Expr* holder) {
        Life l = lifetime_of(holder);
        return l == Life::Frame || l == Life::Scene ? l : Life::Long;
    }

    void check_escape(const ir::Expr* value, Life target, Span span, std::string_view where) {
        Life v = lifetime_of(value);
        if (v == Life::Frame && target != Life::Frame) {
            error(span, std::format("a 'new(frame)' object cannot be stored in {}: the frame arena frees it at the "
                                    "end of the tick (allocate the holder with 'new(frame)' too, or use 'new(scene)' "
                                    "or 'alloc')",
                                    where));
        } else if (v == Life::Scene && target == Life::Global) {
            error(span, std::format("a 'new(scene)' object cannot be stored in {}: the scene arena frees it when the "
                                    "scene unloads (use 'alloc')",
                                    where));
        }
    }

    // `target = value` (LANGUAGE.md 3).
    void check_store(const ir::Expr* target, const ir::Expr* value, Span span) {
        switch (target->kind) {
            case ir::EK::Local: {
                // A local only lives as long as the function: it can hold
                // anything, and remembers the shortest-lived thing it held.
                LocalVar* v = ir::as<ir::Local>(target)->var;
                Life l = std::min(lifetime_of(value), life_of_arena(v->holds));
                v->holds = arena_of_life(l);
                return;
            }
            case ir::EK::Capture:
                check_escape(value, Life::Long, span, "a captured variable (the closure can outlive the tick)");
                return;
            case ir::EK::Global: {
                const GlobalVar* g = ir::as<ir::Global>(target)->var;
                check_escape(value, Life::Global, span, std::format("'{}'", g->qualified));
                return;
            }
            case ir::EK::Field: {
                auto* f = ir::as<ir::Field>(target);
                check_escape(value, holder_life(f->object), span,
                             std::format("field '{}' of a longer-lived object", f->field->name));
                return;
            }
            case ir::EK::Index:
                check_escape(value, holder_life(ir::as<ir::Index>(target)->object), span, "a longer-lived list");
                return;
            default: return;
        }
    }

// Milestone 9.2a library checking (LANGUAGE.md 9.16 - 9.19): collections,
// List / str / number methods, conversions, Result and optional helpers.
// Included inside class Checker (checker.cpp).
// clang-format off

    // ------------------------------------------------------------ helpers

    // A placeholder type parameter: a lambda checked against `fn(T) -> ?`
    // infers its own return type (see lambda()).
    const Type* infer_t() {
        if (infer_param_ == nullptr) {
            auto p = std::make_unique<GenericParamInfo>();
            p->name = "?";
            infer_param_ = p.get();
            P.generic_params.push_back(std::move(p));
        }
        return T.param(infer_param_);
    }

    // Map keys and Set elements: hashed by value (numbers except floats,
    // bool, str, name, plain enums) or by identity (class references).
    static bool hashable(const Type* t) {
        switch (t->tag) {
            case TypeTag::Error:
            case TypeTag::Bool:
            case TypeTag::Str:
            case TypeTag::Name:
            case TypeTag::Class: return true;
            case TypeTag::Num: return !is_float(t->num);
            case TypeTag::Enum: return t->en()->plain;
            default: return false;
        }
    }

    void require_hashable(const Type* t, Span span, std::string_view what) {
        if (hashable(t)) return;
        error(span, std::format("{} of type {} cannot be hashed; use an integer, bool, str, name, plain enum or "
                                "class type{}",
                                what, q(t), t->is_floating() ? " (round floats to an integer key)" : ""));
    }

    // Has a `<` that sort(), min(), max() and PriorityQueue can use.
    bool orderable(const Type* t) {
        if (t->is_error() || t->is_num() || t->tag == TypeTag::Str) return true;
        if (t->tag == TypeTag::Param) return satisfies(t, P.builtin_trait(BuiltinTrait::Ord));
        if (RecordInfo* r = record_of(t)) return explicitly_implements(r, BuiltinTrait::Ord);
        return false;
    }

    ir::Expr* null_of(const Type* t, Span span) {
        ConstValue c;
        c.kind = ConstValue::Kind::Null;
        return const_node(span, t, c);
    }

    ir::Expr* bool_const(Span span, bool b) {
        ConstValue c;
        c.kind = ConstValue::Kind::Bool;
        c.b = b;
        return const_node(span, T.bool_(), c);
    }

    // Built-in method call `obj.name(args)` with positional parameters;
    // trailing parameters past `required` take `defaults` (in order).
    ir::Expr* lib_call(Builtin b, ir::Expr* obj, CallExpr* c, std::string_view what,
                       const std::vector<const Type*>& params, const Type* ret, std::vector<ir::Expr*> defaults = {}) {
        std::size_t required = params.size() - defaults.size();
        auto rest = builtin_args(c, params, required, std::format("'{}'", what));
        if (rest.size() < required) return error_node(c->span);
        for (std::size_t i = rest.size(); i < params.size(); ++i) rest.push_back(defaults[i - required]);
        std::vector<ir::Expr*> args;
        if (obj != nullptr) args.push_back(obj);
        args.insert(args.end(), rest.begin(), rest.end());
        return builtin_node(b, c->span, ret, args);
    }

    // A function argument checked against `fn(params) -> ret` (`ret` null:
    // inferred, returned in `out_ret`). Lambdas get their parameter types.
    ir::Expr* fn_arg(Expr* e, const std::vector<const Type*>& params, const Type* ret, const Type*& out_ret,
                     std::string_view what) {
        ir::Expr* x = expr(e, T.fn(params, ret != nullptr ? ret : infer_t()));
        out_ret = T.error();
        if (x->type->is_error()) return x;
        const Type* t = x->type;
        bool ok = t->tag == TypeTag::Fn && t->args.size() == params.size();
        for (std::size_t i = 0; ok && i < params.size(); ++i) ok = assignable(params[i], t->args[i]);
        if (ok && ret != nullptr) ok = assignable(t->ret, ret);
        if (!ok) {
            std::string want = type_name(T.fn(params, ret != nullptr ? ret : T.void_()));
            if (ret == nullptr) want += " -> T";
            error(e->span, std::format("{} needs a function '{}', found {}", what, want, q(t)));
            return error_node(e->span);
        }
        out_ret = t->ret;
        return x;
    }

    // Exactly `n` positional arguments, or an error (reported once).
    bool arity(CallExpr* c, std::size_t n, std::string_view what) {
        bool named = false;
        for (const Arg& a : c->args) named = named || !a.name.empty();
        if (c->args.size() == n && !named) return true;
        if (named) {
            error(c->span, std::format("'{}' takes positional arguments only", what));
        } else {
            error(c->span, std::format("'{}' takes {} argument{}, got {}", what, n, n == 1 ? "" : "s", c->args.size()));
        }
        check_args_loosely(c->args);
        return false;
    }

    // ------------------------------------------------------------ constructors

    // `Map<K, V>()`, `Set<T>()`, `Deque<T>()`, `PriorityQueue<T>(less)`,
    // `StrBuilder()`: type arguments written or taken from the expected type.
    ir::Expr* collection_new(std::string_view name, const List<TypeExpr*>& type_args, CallExpr* c,
                             const Type* expected) {
        const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
        if (name == "StrBuilder") {
            if (!type_args.empty()) error(c->span, "'StrBuilder' takes no type arguments");
            if (!arity(c, 0, "StrBuilder")) return error_node(c->span);
            return builtin_node(Builtin::StrBuilderNew, c->span, T.str_builder(), {});
        }
        TypeTag tag = name == "Map" ? TypeTag::Map : name == "Set" ? TypeTag::Set
                      : name == "Deque" ? TypeTag::Deque : TypeTag::PQueue;
        std::size_t nargs = tag == TypeTag::Map ? 2 : 1;
        std::vector<const Type*> args;
        if (!type_args.empty()) {
            if (type_args.size() != nargs) {
                error(c->span, std::format("'{}' takes {} type argument{}", name, nargs, nargs == 1 ? "" : "s"));
                check_args_loosely(c->args);
                return error_node(c->span);
            }
            for (TypeExpr* te : type_args) args.push_back(resolve_type(te));
        } else if (ex != nullptr && ex->tag == tag) {
            args = ex->args;
        } else {
            error(c->span, std::format("cannot infer the type arguments; write '{}<{}>()'", name,
                                       nargs == 2 ? "K, V" : "T"));
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        const Type* t = tag == TypeTag::Map ? T.map(args[0], args[1])
                        : tag == TypeTag::Set ? T.set(args[0])
                        : tag == TypeTag::Deque ? T.deque(args[0]) : T.pqueue(args[0]);
        if (tag == TypeTag::Map) require_hashable(args[0], c->span, "a map key");
        if (tag == TypeTag::Set) require_hashable(args[0], c->span, "a set element");
        if (tag == TypeTag::PQueue) {
            const Type* el = args[0];
            if (c->args.empty()) {
                if (!orderable(el)) {
                    error(c->span, std::format("{} has no '<'; pass an ordering: 'PriorityQueue<{}>((a, b) => ...)'",
                                               q(el), type_name(el)));
                }
                return builtin_node(Builtin::PQueueNew, c->span, t, {null_of(T.fn({el, el}, T.bool_()), c->span)});
            }
            if (!arity(c, 1, "PriorityQueue")) return error_node(c->span);
            const Type* r = nullptr;
            ir::Expr* less = fn_arg(c->args[0].value, {el, el}, T.bool_(), r, "'PriorityQueue'");
            check_escape(less, Life::Long, c->span, "a priority queue's ordering");
            return builtin_node(Builtin::PQueueNew, c->span, t, {less});
        }
        if (!arity(c, 0, name)) return error_node(c->span);
        Builtin b = tag == TypeTag::Map ? Builtin::MapNew : tag == TypeTag::Set ? Builtin::SetNew : Builtin::DequeNew;
        return builtin_node(b, c->span, t, {});
    }

    // `{k: v, ...}`: a Map literal. Key and value types come from the expected
    // type, else from the first entry.
    ir::Expr* map_literal(MapLit* m, const Type* expected) {
        const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
        const Type* kt = nullptr;
        const Type* vt = nullptr;
        if (ex != nullptr && ex->tag == TypeTag::Map) {
            kt = ex->args[0];
            vt = ex->args[1];
        }
        std::vector<ir::Expr*> args;
        for (const MapEntry& e : m->entries) {
            if (kt == nullptr) {
                ir::Expr* k = expr(e.key);
                kt = literal_default_type(k->type, e.key->span);
                args.push_back(value_copy(k));
            } else {
                args.push_back(expect_type(e.key, kt, "map key"));
            }
            if (vt == nullptr) {
                ir::Expr* v = expr(e.value);
                vt = literal_default_type(v->type, e.value->span);
                args.push_back(value_copy(v));
            } else {
                args.push_back(expect_type(e.value, vt, "map value"));
            }
        }
        if (kt == nullptr) {
            error(m->span, "cannot infer the types of an empty map; write 'Map<K, V>()' or annotate the variable");
            return error_node(m->span);
        }
        require_hashable(kt, m->span, "a map key");
        return builtin_node(Builtin::MapLit, m->span, T.map(kt, vt), args);
    }

    // `[a, b, c]` where a Set, Deque or [T; N] is expected.
    ir::Expr* collection_literal(ListLit* l, const Type* ex) {
        const Type* el = ex->args[0];
        std::vector<ir::Expr*> elems;
        for (Expr* e : l->elements) elems.push_back(expect_type(e, el, "element"));
        if (ex->tag == TypeTag::Array) {
            if (elems.size() != ex->dim) {
                error(l->span, std::format("{} needs {} element{}, got {}", q(ex), ex->dim, ex->dim == 1 ? "" : "s",
                                           elems.size()));
                return error_node(l->span);
            }
            auto* x = mk<ir::ListLit>(l->span, ex);
            x->elements = list(elems);
            return x;
        }
        return builtin_node(ex->tag == TypeTag::Set ? Builtin::SetLit : Builtin::DequeLit, l->span, ex, elems);
    }

    // `Ok(v)` / `Err(e)`: the Result type comes from the expected type.
    ir::Expr* result_ctor(bool ok, CallExpr* c, const Type* expected) {
        std::string_view what = ok ? "Ok" : "Err";
        const Type* ex = expected != nullptr ? T.non_null(expected) : nullptr;
        if (ex == nullptr || ex->tag != TypeTag::Result) {
            error(c->span, std::format("cannot infer the Result type of '{}(...)'; use it where a 'Result<T, E>' is "
                                       "expected (a return value, an annotated variable)",
                                       what));
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        if (!arity(c, 1, what)) return error_node(c->span);
        ir::Expr* v = expect_type(c->args[0].value, ex->args[ok ? 0 : 1], ok ? "Ok value" : "Err value");
        return builtin_node(ok ? Builtin::ResultOk : Builtin::ResultErr, c->span, ex, {v});
    }

    // `x?` (DESIGN.md 5.8, LANGUAGE.md 9.18).
    ir::Expr* try_expr(TryExpr* t) {
        ir::Expr* x = expr(t->operand);
        if (x->type->is_error()) return x;
        const Type* ret = cx_->ret;
        auto fail = [&](std::string msg) {
            error(t->span, std::move(msg));
            return error_node(t->span);
        };
        if (cx_->fn->kind == FnKind::Chunk) return fail("'?' can only be used inside a function");
        if (cx_->defer_depth > 0) return fail("'?' cannot be used inside 'defer' (it may return)");
        if (ret == nullptr) {
            return fail("'?' in a lambda needs its return type: annotate it ('fn(...) -> Result<T, E> { ... }')");
        }
        auto* n = mk<ir::Try>(t->span, T.error());
        n->operand = x;
        if (x->type->tag == TypeTag::Result) {
            const Type* rt = T.non_null(ret);
            if (rt->tag != TypeTag::Result) {
                return fail(std::format("'?' on {} returns its error, but this function returns {}; declare a "
                                        "'Result<..., {}>' return type",
                                        q(x->type), q(ret), type_name(x->type->args[1])));
            }
            if (!assignable(x->type->args[1], rt->args[1])) {
                return fail(std::format("'?' would return error type {}, but this function's error type is {}",
                                        q(x->type->args[1]), q(rt->args[1])));
            }
            n->type = x->type->args[0];
            n->is_result = true;
            return n;
        }
        if (x->type->is_nullable()) {
            if (!ret->is_nullable()) {
                return fail(std::format("'?' on {} returns null when it is null, but this function returns {}",
                                        q(x->type), q(ret)));
            }
            n->type = T.non_null(x->type);
            n->is_result = false;
            return n;
        }
        return fail(std::format("'?' needs a Result or a nullable value, found {}", q(x->type)));
    }

    // ------------------------------------------------------------ List / [T; N]

    ir::Expr* list_lib_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* lt = obj->type;
        const bool array = lt->tag == TypeTag::Array;
        const Type* el = lt->args[0];
        std::string what = std::format("{}.{}", array ? "array" : "List", name.name);
        const std::string_view n = name.name;
        const Type* el_opt = T.nullable(el);
        const Type* int_t = T.int_();
        const Type* bool_t = T.bool_();
        auto needs_order = [&]() {
            if (orderable(el)) return true;
            error(c->span, std::format("'{}' needs elements with '<' (numbers, str, or a type implementing Ord); {} "
                                       "has none{}",
                                       what, q(el), n == "sort" ? "; pass an ordering: 'sort((a, b) => ...)'" : ""));
            return false;
        };
        if (n == "len" && array) return lib_call(Builtin::ListLen, obj, c, what, {}, int_t);
        if (n == "is_empty" && array) return lib_call(Builtin::ListIsEmpty, obj, c, what, {}, bool_t);
        if (n == "contains" && array) return lib_call(Builtin::ListContains, obj, c, what, {el}, bool_t);
        if (n == "sort") {
            if (c->args.empty()) {
                if (!needs_order()) return error_node(c->span);
                return builtin_node(Builtin::ListSort, c->span, T.void_(),
                                    {obj, null_of(T.fn({el, el}, bool_t), c->span)});
            }
            if (!arity(c, 1, what)) return error_node(c->span);
            const Type* r = nullptr;
            ir::Expr* f = fn_arg(c->args[0].value, {el, el}, bool_t, r, std::format("'{}'", what));
            return builtin_node(Builtin::ListSort, c->span, T.void_(), {obj, f});
        }
        if (n == "sort_by") {
            if (!arity(c, 1, what)) return error_node(c->span);
            const Type* k = nullptr;
            ir::Expr* f = fn_arg(c->args[0].value, {el}, nullptr, k, std::format("'{}'", what));
            if (!k->is_error() && !orderable(k)) {
                error(c->args[0].value->span, std::format("the sort key {} has no '<'", q(k)));
            }
            return builtin_node(Builtin::ListSortBy, c->span, T.void_(), {obj, f});
        }
        if (n == "reverse") return lib_call(Builtin::ListReverse, obj, c, what, {}, T.void_());
        if (n == "index_of") return lib_call(Builtin::ListIndexOf, obj, c, what, {el}, int_t);
        if (n == "slice") {
            return lib_call(Builtin::ListSlice, obj, c, what, {int_t, int_t}, T.list(el),
                            {int_const(c->span, int_t, 0x7fffffff)});
        }
        if (n == "extend" && !array) {
            auto x = lib_call(Builtin::ListExtend, obj, c, what, {T.list(el)}, T.void_());
            return x;
        }
        if (n == "swap") return lib_call(Builtin::ListSwap, obj, c, what, {int_t, int_t}, T.void_());
        if (n == "first") return lib_call(Builtin::ListFirst, obj, c, what, {}, el_opt);
        if (n == "last") return lib_call(Builtin::ListLast, obj, c, what, {}, el_opt);
        if (n == "fill") return lib_call(Builtin::ListFill, obj, c, what, {el}, T.void_());
        if (n == "to_list") return lib_call(Builtin::ListToList, obj, c, what, {}, T.list(el));
        if (n == "unique") return lib_call(Builtin::ListUnique, obj, c, what, {}, T.list(el));
        if (n == "shuffle") return lib_call(Builtin::ListShuffle, obj, c, what, {}, T.void_());
        if (n == "min" || n == "max") {
            if (!arity(c, 0, what) || !needs_order()) return error_node(c->span);
            return builtin_node(n == "min" ? Builtin::ListMin : Builtin::ListMax, c->span, el_opt, {obj});
        }
        if (n == "sum") {
            if (!arity(c, 0, what)) return error_node(c->span);
            if (!el->is_num() && !el->is_vec() && !el->is_error()) {
                error(c->span, std::format("'{}' needs numbers or vectors, found {}", what, q(el)));
                return error_node(c->span);
            }
            return builtin_node(Builtin::ListSum, c->span, el, {obj, mk<ir::Default>(c->span, el)});
        }
        if (n == "map" || n == "filter" || n == "any" || n == "all" || n == "count") {
            if (!arity(c, 1, what)) return error_node(c->span);
            const Type* r = nullptr;
            const bool pred = n != "map";
            ir::Expr* f = fn_arg(c->args[0].value, {el}, pred ? bool_t : nullptr, r, std::format("'{}'", what));
            if (n == "map") {
                if (r->is_void()) {
                    error(c->args[0].value->span, std::format("the function given to '{}' must return a value", what));
                    return error_node(c->span);
                }
                return builtin_node(Builtin::ListMap, c->span, r->is_error() ? T.error() : T.list(r), {obj, f});
            }
            Builtin b = n == "filter" ? Builtin::ListFilter : n == "any" ? Builtin::ListAny
                        : n == "all" ? Builtin::ListAll : Builtin::ListCount;
            const Type* ret = n == "filter" ? T.list(el) : n == "count" ? int_t : bool_t;
            return builtin_node(b, c->span, ret, {obj, f});
        }
        if (n == "reduce") {
            if (!arity(c, 2, what)) return error_node(c->span);
            ir::Expr* init = expr(c->args[0].value);
            const Type* acc = literal_default_type(init->type, c->args[0].value->span);
            init = value_copy(init);
            const Type* r = nullptr;
            ir::Expr* f = fn_arg(c->args[1].value, {acc, el}, acc, r, std::format("'{}'", what));
            return builtin_node(Builtin::ListReduce, c->span, acc, {obj, init, f});
        }
        if (n == "zip") {
            if (!arity(c, 2, what)) return error_node(c->span);
            ir::Expr* other = expr(c->args[0].value);
            const Type* ot = other->type;
            if (ot->is_error()) return error_node(c->span);
            if (ot->tag != TypeTag::List && ot->tag != TypeTag::Array) {
                error(c->args[0].value->span, std::format("'{}' needs a list, found {}", what, q(ot)));
                return error_node(c->span);
            }
            const Type* r = nullptr;
            ir::Expr* f = fn_arg(c->args[1].value, {el, ot->args[0]}, nullptr, r, std::format("'{}'", what));
            if (r->is_void()) {
                error(c->args[1].value->span, std::format("the function given to '{}' must return a value", what));
                return error_node(c->span);
            }
            return builtin_node(Builtin::ListZip, c->span, r->is_error() ? T.error() : T.list(r), {obj, other, f});
        }
        if (n == "join") {
            error(name.span, "join is a method of the separator: 'sep.join(list)'");
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        return nullptr;
    }

    // ------------------------------------------------------------ Map / Set / Deque / PriorityQueue / StrBuilder

    ir::Expr* map_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* kt = obj->type->args[0];
        const Type* vt = obj->type->args[1];
        std::string what = std::format("Map.{}", name.name);
        const std::string_view n = name.name;
        if (n == "len") return lib_call(Builtin::MapLen, obj, c, what, {}, T.int_());
        if (n == "is_empty") return lib_call(Builtin::MapIsEmpty, obj, c, what, {}, T.bool_());
        if (n == "has") return lib_call(Builtin::MapHas, obj, c, what, {kt}, T.bool_());
        if (n == "get") return lib_call(Builtin::MapGet, obj, c, what, {kt}, T.nullable(vt));
        if (n == "get_or") return lib_call(Builtin::MapGetOr, obj, c, what, {kt, vt}, vt);
        if (n == "remove") return lib_call(Builtin::MapRemove, obj, c, what, {kt}, T.bool_());
        if (n == "clear") return lib_call(Builtin::MapClear, obj, c, what, {}, T.void_());
        if (n == "keys") return lib_call(Builtin::MapKeys, obj, c, what, {}, T.list(kt));
        if (n == "values") return lib_call(Builtin::MapValues, obj, c, what, {}, T.list(vt));
        if (n == "set" || n == "insert" || n == "contains") {
            error(name.span, std::format("'Map' has no method '{}'; use {}", n,
                                         n == "contains" ? "'has(key)'" : "'map[key] = value'"));
            check_args_loosely(c->args);
            return error_node(c->span);
        }
        error(name.span, std::format("'Map' has no method '{}'", n));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    ir::Expr* set_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* el = obj->type->args[0];
        std::string what = std::format("Set.{}", name.name);
        const std::string_view n = name.name;
        if (n == "len") return lib_call(Builtin::SetLen, obj, c, what, {}, T.int_());
        if (n == "is_empty") return lib_call(Builtin::SetIsEmpty, obj, c, what, {}, T.bool_());
        if (n == "add") return lib_call(Builtin::SetAdd, obj, c, what, {el}, T.bool_());
        if (n == "remove") return lib_call(Builtin::SetRemove, obj, c, what, {el}, T.bool_());
        if (n == "contains") return lib_call(Builtin::SetContains, obj, c, what, {el}, T.bool_());
        if (n == "clear") return lib_call(Builtin::SetClear, obj, c, what, {}, T.void_());
        if (n == "to_list") return lib_call(Builtin::SetToList, obj, c, what, {}, T.list(el));
        if (n == "union") return lib_call(Builtin::SetUnion, obj, c, what, {obj->type}, obj->type);
        if (n == "intersection") return lib_call(Builtin::SetIntersection, obj, c, what, {obj->type}, obj->type);
        if (n == "difference") return lib_call(Builtin::SetDifference, obj, c, what, {obj->type}, obj->type);
        error(name.span, std::format("'Set' has no method '{}'", n));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    ir::Expr* deque_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* el = obj->type->args[0];
        std::string what = std::format("Deque.{}", name.name);
        const std::string_view n = name.name;
        if (n == "len") return lib_call(Builtin::DequeLen, obj, c, what, {}, T.int_());
        if (n == "is_empty") return lib_call(Builtin::DequeIsEmpty, obj, c, what, {}, T.bool_());
        if (n == "push_back" || n == "push_front") {
            ir::Expr* x = lib_call(n == "push_back" ? Builtin::DequePushBack : Builtin::DequePushFront, obj, c, what,
                                   {el}, T.void_());
            if (auto b = ir::as<ir::BuiltinCall>(x)) check_escape(b->args[1], holder_life(obj), c->span, "a longer-lived deque");
            return x;
        }
        if (n == "pop_back") return lib_call(Builtin::DequePopBack, obj, c, what, {}, el);
        if (n == "pop_front") return lib_call(Builtin::DequePopFront, obj, c, what, {}, el);
        if (n == "front") return lib_call(Builtin::DequeFront, obj, c, what, {}, el);
        if (n == "back") return lib_call(Builtin::DequeBack, obj, c, what, {}, el);
        if (n == "clear") return lib_call(Builtin::DequeClear, obj, c, what, {}, T.void_());
        if (n == "contains") return lib_call(Builtin::DequeContains, obj, c, what, {el}, T.bool_());
        if (n == "to_list") return lib_call(Builtin::DequeToList, obj, c, what, {}, T.list(el));
        error(name.span, std::format("'Deque' has no method '{}'", n));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    ir::Expr* pqueue_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* el = obj->type->args[0];
        std::string what = std::format("PriorityQueue.{}", name.name);
        const std::string_view n = name.name;
        if (n == "push") {
            ir::Expr* x = lib_call(Builtin::PQPush, obj, c, what, {el}, T.void_());
            if (auto b = ir::as<ir::BuiltinCall>(x)) check_escape(b->args[1], holder_life(obj), c->span, "a longer-lived queue");
            return x;
        }
        if (n == "pop") return lib_call(Builtin::PQPop, obj, c, what, {}, el);
        if (n == "peek") return lib_call(Builtin::PQPeek, obj, c, what, {}, el);
        if (n == "len") return lib_call(Builtin::PQLen, obj, c, what, {}, T.int_());
        if (n == "is_empty") return lib_call(Builtin::PQIsEmpty, obj, c, what, {}, T.bool_());
        if (n == "clear") return lib_call(Builtin::PQClear, obj, c, what, {}, T.void_());
        if (n == "to_list") return lib_call(Builtin::PQToList, obj, c, what, {}, T.list(el));
        error(name.span, std::format("'PriorityQueue' has no method '{}'", n));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    ir::Expr* str_builder_method(ir::Expr* obj, Ident name, CallExpr* c) {
        std::string what = std::format("StrBuilder.{}", name.name);
        const std::string_view n = name.name;
        if (n == "append") {
            if (!arity(c, 1, what)) return error_node(c->span);
            ir::Expr* x = expr(c->args[0].value);
            return builtin_node(Builtin::SbAppend, c->span, T.str_builder(), {obj, to_str(x, c->args[0].value->span)});
        }
        if (n == "len") return lib_call(Builtin::SbLen, obj, c, what, {}, T.int_());
        if (n == "clear") return lib_call(Builtin::SbClear, obj, c, what, {}, T.void_());
        if (n == "to_str") return lib_call(Builtin::SbToStr, obj, c, what, {}, T.str());
        if (n == "is_empty") return lib_call(Builtin::SbIsEmpty, obj, c, what, {}, T.bool_());
        error(name.span, std::format("'StrBuilder' has no method '{}'", n));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    // ------------------------------------------------------------ str / numbers

    ir::Expr* str_lib_method(ir::Expr* obj, Ident name, CallExpr* c) {
        std::string what = std::format("str.{}", name.name);
        const std::string_view n = name.name;
        const Type* s = T.str();
        const Type* i = T.int_();
        if (n == "split") return lib_call(Builtin::StrSplit, obj, c, what, {s}, T.list(s));
        if (n == "join") return lib_call(Builtin::StrJoin, obj, c, what, {T.list(s)}, s);
        if (n == "replace") return lib_call(Builtin::StrReplace, obj, c, what, {s, s}, s);
        if (n == "trim") return lib_call(Builtin::StrTrim, obj, c, what, {}, s);
        if (n == "trim_start") return lib_call(Builtin::StrTrimStart, obj, c, what, {}, s);
        if (n == "trim_end") return lib_call(Builtin::StrTrimEnd, obj, c, what, {}, s);
        if (n == "to_upper") return lib_call(Builtin::StrToUpper, obj, c, what, {}, s);
        if (n == "to_lower") return lib_call(Builtin::StrToLower, obj, c, what, {}, s);
        if (n == "find") return lib_call(Builtin::StrFind, obj, c, what, {s, i}, i, {int_const(c->span, i, 0)});
        if (n == "substr") {
            return lib_call(Builtin::StrSubstr, obj, c, what, {i, i}, s, {int_const(c->span, i, 0x7fffffff)});
        }
        if (n == "byte_at") return lib_call(Builtin::StrByteAt, obj, c, what, {i}, i);
        if (n == "chars") return lib_call(Builtin::StrChars, obj, c, what, {}, T.list(s));
        if (n == "char_count") return lib_call(Builtin::StrCharCount, obj, c, what, {}, i);
        if (n == "repeat") return lib_call(Builtin::StrRepeat, obj, c, what, {i}, s);
        if (n == "pad_left" || n == "pad_right") {
            return lib_call(n == "pad_left" ? Builtin::StrPadLeft : Builtin::StrPadRight, obj, c, what, {i, s}, s,
                            {str_const(c->span, " ")});
        }
        if (n == "lines") return lib_call(Builtin::StrLines, obj, c, what, {}, T.list(s));
        if (n == "is_empty") return lib_call(Builtin::StrIsEmpty, obj, c, what, {}, T.bool_());
        if (n == "parse_int") {
            return lib_call(Builtin::StrParseInt, obj, c, what, {i}, T.nullable(i), {int_const(c->span, i, 10)});
        }
        if (n == "parse_float") return lib_call(Builtin::StrParseFloat, obj, c, what, {}, T.nullable(T.float_()));
        return nullptr;
    }

    ir::Expr* num_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* t = obj->type;
        std::string what = std::format("{}.{}", type_name(t), name.name);
        const std::string_view n = name.name;
        if ((n == "to_hex" || n == "to_radix") && t->is_int()) {
            if (n == "to_hex") return lib_call(Builtin::NumToHex, obj, c, what, {}, T.str());
            return lib_call(Builtin::NumToRadix, obj, c, what, {T.int_()}, T.str());
        }
        if (n == "to_fixed" && t->is_floating()) return lib_call(Builtin::NumToFixed, obj, c, what, {T.int_()}, T.str());
        error(name.span, std::format("{} has no method '{}'{}", q(t), n,
                                     t->is_int() ? " (integers have to_hex() and to_radix(base))"
                                                 : " (floats have to_fixed(digits))"));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    // ------------------------------------------------------------ Result / optionals

    ir::Expr* result_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* vt = obj->type->args[0];
        const Type* et = obj->type->args[1];
        std::string what = std::format("Result.{}", name.name);
        const std::string_view n = name.name;
        if (n == "is_ok") return lib_call(Builtin::ResIsOk, obj, c, what, {}, T.bool_());
        if (n == "is_err") return lib_call(Builtin::ResIsErr, obj, c, what, {}, T.bool_());
        if (n == "unwrap") return lib_call(Builtin::ResUnwrap, obj, c, what, {}, vt);
        if (n == "unwrap_err") return lib_call(Builtin::ResUnwrapErr, obj, c, what, {}, et);
        if (n == "unwrap_or") return lib_call(Builtin::ResUnwrapOr, obj, c, what, {vt}, vt);
        if (n == "expect") return lib_call(Builtin::ResExpect, obj, c, what, {T.str()}, vt);
        if (n == "ok") return lib_call(Builtin::ResOk, obj, c, what, {}, T.nullable(vt));
        if (n == "err") return lib_call(Builtin::ResErr, obj, c, what, {}, T.nullable(et));
        error(name.span, std::format("'Result' has no method '{}'", n));
        check_args_loosely(c->args);
        return error_node(c->span);
    }

    // Methods of an optional value `T?` (value types only: a reference is
    // always nullable and uses '??' / 'if x? v').
    ir::Expr* optional_method(ir::Expr* obj, Ident name, CallExpr* c) {
        const Type* inner = obj->type->args[0];
        std::string what = std::format("{}.{}", type_name(obj->type), name.name);
        const std::string_view n = name.name;
        if (n == "unwrap_or") {
            if (!arity(c, 1, what)) return error_node(c->span);
            auto* co = mk<ir::Coalesce>(c->span, inner);
            co->lhs = obj;
            co->rhs = expect_type(c->args[0].value, inner, "default value");
            return co;
        }
        if (n == "unwrap") return lib_call(Builtin::OptUnwrap, obj, c, what, {}, inner);
        if (n == "expect") return lib_call(Builtin::OptExpect, obj, c, what, {T.str()}, inner);
        if (n == "is_some") return lib_call(Builtin::OptIsSome, obj, c, what, {}, T.bool_());
        if (n == "is_none") return lib_call(Builtin::OptIsNone, obj, c, what, {}, T.bool_());
        return nullptr;
    }

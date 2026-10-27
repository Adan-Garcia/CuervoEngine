#include "pluma/compiler.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "pluma/acyclic.hpp"
#include "pluma/heap.hpp"
#include "pluma/tir.hpp"
#include "pluma/vm.hpp"

namespace pluma {

namespace {

namespace ir = tir;

// Operand kind of a typed instruction for values of type `t`.
std::uint8_t kind_of(const Type* t) {
    switch (t->tag) {
        case TypeTag::Num: return static_cast<std::uint8_t>(t->num);
        case TypeTag::Bool: return opk::Bool;
        case TypeTag::Str: return opk::Str;
        case TypeTag::Name: return opk::Name;
        case TypeTag::Class:
        case TypeTag::List:
        case TypeTag::Fn:
        case TypeTag::Null: return opk::Ref;
        case TypeTag::Enum: return t->en()->plain ? opk::Enum : opk::Dyn;
        case TypeTag::Vec: return opk::Vec;
        case TypeTag::Task:
        case TypeTag::Signal:
        case TypeTag::Map:
        case TypeTag::Set:
        case TypeTag::Deque:
        case TypeTag::PQueue:
        case TypeTag::StrBuilder: return opk::Ref;
        default: return opk::Dyn; // struct, trait, param, nullable, [T; N], Result
    }
}

std::uint8_t pack_indices(const std::uint8_t (&idx)[4], std::uint8_t count) {
    std::uint8_t k = 0;
    for (std::uint8_t i = 0; i < count; ++i) k = static_cast<std::uint8_t>(k | (idx[i] << (2 * i)));
    return k;
}

// Could a register of this type hold a heap object (needs clearing)?
bool may_hold_object(const Type* t) {
    switch (t->tag) {
        case TypeTag::Num:
        case TypeTag::Bool:
        case TypeTag::Name:
        case TypeTag::Void:
        case TypeTag::Null: return false;
        case TypeTag::Enum: return !t->en()->plain;
        case TypeTag::Vec: return t->dim > 2;
        case TypeTag::Nullable: return may_hold_object(t->args[0]);
        default: return true;
    }
}

class FnCompiler {
public:
    FnCompiler(VM& vm, const Function* fn, Proto* proto, const AcyclicAnalysis& acyclic)
        : vm_(vm), fn_(fn), proto_(proto), acyclic_(acyclic) {}

    void compile() {
        proto_->name = fn_->name;
        proto_->key = fn_->stable_key.empty() ? fn_->name : fn_->stable_key;
        proto_->source = fn_->source;
        proto_->param_count = static_cast<std::uint16_t>(fn_->param_vars.size());
        proto_->capture_count = static_cast<std::uint16_t>(fn_->captures.size());
        for (LocalVar* v : fn_->param_vars) bind(v, alloc(may_hold_object(v->type)));
        span_ = fn_->span;
        if (fn_->is_native) {
            // A thunk: the host function gets the parameters (self first).
            std::uint16_t r = alloc(true);
            const std::string& key = fn_->native_name.empty() ? fn_->name : fn_->native_name;
            std::string why;
            std::uint32_t id = vm_.resolve_native(key, &why);
            if (id == VM::no_native) {
                // An `@extern` whose library or symbol is missing fails when called.
                const std::string message = why.empty() ? "native '" + fn_->name + "' is not bound"
                                                        : "'" + fn_->name + "': " + why;
                emit(Op::LoadK, r, constant(str_value(message)));
                emit(Op::CallN, r, static_cast<std::uint16_t>(Builtin::Panic), r, 1);
            } else {
                emit(Op::CallX, r, static_cast<std::uint16_t>(id), 0, static_cast<std::uint8_t>(proto_->param_count));
                emit(Op::Ret, r);
            }
        } else if (fn_->body == nullptr) {
            // Abstract trait method: unreachable through the checker.
            std::uint16_t r = alloc(true);
            emit(Op::LoadK, r, constant(str_value("abstract method '" + fn_->name + "' called")));
            emit(Op::CallN, r, static_cast<std::uint16_t>(Builtin::Panic), r, 1);
        } else {
            scopes_.push_back(Scope{});
            block(fn_->body);
            run_defers(scopes_.back());
            scopes_.pop_back();
            span_ = Span{}; // the implicit return: no line of its own (see stmt())
        }
        emit(Op::Ret0);
        close_locals(0);
        proto_->reg_count = std::max<std::uint16_t>(max_reg_, proto_->param_count);
        proto_->compiled = true;
    }

private:
    // ------------------------------------------------------------ emission

    std::uint32_t emit(Op op, std::uint16_t a = 0, std::uint16_t b = 0, std::uint16_t c = 0, std::uint8_t k = 0) {
        proto_->code.push_back(Instr{op, k, a, b, c});
        proto_->spans.push_back(span_);
        return static_cast<std::uint32_t>(proto_->code.size() - 1);
    }
    std::uint32_t here() const { return static_cast<std::uint32_t>(proto_->code.size()); }
    std::uint32_t emit_jump(Op op, std::uint16_t a = 0) { return emit(op, a); }
    void patch(std::uint32_t at, std::uint32_t target) { proto_->code[at].set_target(target); }
    void patch_here(std::uint32_t at) { patch(at, here()); }
    void patch_all(const std::vector<std::uint32_t>& jumps, std::uint32_t target) {
        for (std::uint32_t j : jumps) patch(j, target);
    }
    void jump_to(Op op, std::uint16_t a, std::uint32_t target) {
        std::uint32_t j = emit(op, a);
        patch(j, target);
    }

    // A local lives in register `r` from here until the statement that
    // allocated the register frees it (close_locals): the pc range a panic
    // uses to list the frame's locals (pluma::LocalDebug).
    void bind(const LocalVar* v, std::uint16_t r) {
        regs_[v] = r;
        if (v->name.empty() || v->name.front() == '<') return; // compiler temporaries
        open_locals_.push_back(proto_->locals.size());
        proto_->locals.push_back(LocalDebug{std::string(v->name), v->type != nullptr ? type_name(v->type) : std::string(),
                                            r, here(), here()});
    }
    void close_locals(std::uint16_t first_free) {
        std::erase_if(open_locals_, [&](std::size_t i) {
            if (proto_->locals[i].reg < first_free) return false;
            proto_->locals[i].end = here();
            return true;
        });
    }

    std::uint16_t alloc(bool obj) {
        std::uint16_t r = next_++;
        if (next_ == 0) throw std::runtime_error("function '" + fn_->name + "' needs too many registers");
        max_reg_ = std::max(max_reg_, next_);
        high_ = std::max(high_, next_);
        if (dirty_.size() <= r) dirty_.resize(r + 1u, false);
        if (obj) dirty_[r] = true;
        return r;
    }
    std::uint16_t alloc_t(const Type* t) { return alloc(may_hold_object(t)); }
    std::uint16_t alloc_block(std::size_t n) {
        std::uint16_t base = next_;
        for (std::size_t i = 0; i < n; ++i) alloc(true);
        return base;
    }
    std::uint16_t dst(int want, const Type* t) {
        if (want >= 0) {
            auto r = static_cast<std::uint16_t>(want);
            if (may_hold_object(t)) mark_dirty(r);
            return r;
        }
        return alloc_t(t);
    }
    void mark_dirty(std::uint16_t r) {
        if (dirty_.size() <= r) dirty_.resize(r + 1u, false);
        dirty_[r] = true;
    }

    std::uint16_t constant(Value v) {
        // Deduplicate strings and names; everything else is cheap to repeat.
        if (v.tag == Tag::Obj && v.o->kind == ObjKind::Str) {
            const std::string& s = static_cast<StrObj*>(v.o)->s;
            auto it = str_consts_.find(s);
            if (it != str_consts_.end()) {
                retain(v);
                release(v);
                return it->second;
            }
            auto idx = static_cast<std::uint16_t>(proto_->consts.size());
            str_consts_[s] = idx;
            retain(v);
            proto_->consts.push_back(v);
            return idx;
        }
        if (proto_->consts.size() >= 0xffff) throw std::runtime_error("too many constants in '" + fn_->name + "'");
        retain(v);
        proto_->consts.push_back(v);
        return static_cast<std::uint16_t>(proto_->consts.size() - 1);
    }

    Value const_value(const ConstValue& c, const Type* t) {
        switch (c.kind) {
            case ConstValue::Kind::Int:
                if (t->en() != nullptr) {
                    Value v = Value::integer(NumKind::I64, c.i);
                    v.tag = Tag::Enum;
                    v.aux = t->en()->rt_index;
                    return v;
                }
                return Value::integer(t->is_num() ? t->num : NumKind::I32, c.i);
            case ConstValue::Kind::Float:
                return t->is_num() && t->num == NumKind::F64 ? Value::f64(c.f) : Value::f32(static_cast<float>(c.f));
            case ConstValue::Kind::Bool: return Value::boolean(c.b);
            case ConstValue::Kind::Str: return str_value(std::string(c.s));
            case ConstValue::Kind::Name: {
                Value v;
                v.tag = Tag::Name;
                v.i = vm_.intern(c.s);
                return v;
            }
            case ConstValue::Kind::Null:
            case ConstValue::Kind::None: break;
        }
        return Value::null();
    }

    void load_const(std::uint16_t r, const ConstValue& c, const Type* t) {
        if (c.kind == ConstValue::Kind::Null) {
            emit(Op::LoadNull, r);
            return;
        }
        if (c.kind == ConstValue::Kind::Bool) {
            emit(Op::LoadBool, r, c.b ? 1 : 0);
            return;
        }
        if (c.kind == ConstValue::Kind::Int && t->is_int() && c.i >= -32768 && c.i <= 32767) {
            emit(Op::LoadI, r, static_cast<std::uint16_t>(static_cast<std::int16_t>(c.i)), 0,
                 static_cast<std::uint8_t>(t->num));
            return;
        }
        emit(Op::LoadK, r, constant(const_value(c, t)));
    }

    // ------------------------------------------------------------ statements

    void block(const ir::Block* b) {
        if (b->unchecked) ++unsafe_;
        for (const ir::Stmt* s : b->stmts) stmt(s);
        if (b->unchecked) --unsafe_;
    }

    // `k` flags of instructions that dereference a reference: inside
    // `unsafe { }` manual references skip their generation check.
    std::uint8_t deref_flags() const { return unsafe_ > 0 ? opk::Unchecked : 0; }
    std::uint8_t field_flags(const FieldInfo* f) const {
        return static_cast<std::uint8_t>(deref_flags() | (f->weak ? opk::WeakField : 0));
    }

    // Each statement frees its temporaries; registers that may still hold
    // objects are cleared so objects die as soon as they are unreachable.
    void stmt(const ir::Stmt* s) {
        std::uint16_t mark = next_;
        std::uint16_t saved_high = high_;
        high_ = next_;
        Span saved_span = span_;
        span_ = s->span;
        std::uint16_t keep = mark;
        if (s->kind == ir::SK::Block) scopes_.push_back(Scope{});
        stmt_inner(s, keep);
        if (s->kind == ir::SK::Block) {
            run_defers(scopes_.back());
            scopes_.pop_back();
        }
        // Cleanup glue (it can't panic) gets no line of its own, so a debugger
        // doesn't step back to the enclosing line after each statement
        // (VM::line_of, pluma/debug.hpp).
        span_ = Span{};
        next_ = keep;
        close_locals(keep);
        for (std::uint16_t r = keep; r < high_; ++r) {
            if (r < dirty_.size() && dirty_[r]) {
                emit(Op::Clear, r);
                dirty_[r] = false;
            }
        }
        high_ = std::max(saved_high, high_);
        span_ = saved_span;
    }

    void stmt_inner(const ir::Stmt* s, std::uint16_t& keep) {
        switch (s->kind) {
            case ir::SK::Expr: {
                auto* es = ir::as<ir::ExprStmt>(s);
                std::uint16_t r = expr(es->expr);
                if (es->echo) emit(Op::CallN, r, static_cast<std::uint16_t>(Builtin::Echo), r, 1);
                return;
            }
            case ir::SK::Var: {
                auto* v = ir::as<ir::VarStmt>(s);
                std::uint16_t r = alloc_t(v->var->type);
                expr(v->init, r);
                if (v->var->captured) {
                    emit(Op::NewCell, r, r, 0, acyclic_.holder_acyclic(v->var->type) ? 1 : 0);
                    mark_dirty(r);
                }
                bind(v->var, r);
                keep = static_cast<std::uint16_t>(r + 1);
                return;
            }
            case ir::SK::Assign: assign(ir::as<ir::Assign>(s)); return;
            case ir::SK::Block: block(ir::as<ir::Block>(s)); return;
            case ir::SK::If: if_stmt(ir::as<ir::If>(s)); return;
            case ir::SK::While: {
                auto* w = ir::as<ir::While>(s);
                std::uint32_t top = here();
                loops_.push_back(Loop{s, scopes_.size(), {}, {}});
                std::uint16_t c = expr(w->cond);
                std::uint32_t exit = emit_jump(Op::JmpF, c);
                body(w->body);
                Loop l = std::move(loops_.back());
                loops_.pop_back();
                patch_all(l.continues, top);
                jump_to(Op::Jmp, 0, top);
                patch_here(exit);
                patch_all(l.breaks, here());
                return;
            }
            case ir::SK::Loop: {
                auto* lp = ir::as<ir::Loop>(s);
                std::uint32_t top = here();
                loops_.push_back(Loop{s, scopes_.size(), {}, {}});
                body(lp->body);
                Loop l = std::move(loops_.back());
                loops_.pop_back();
                patch_all(l.continues, top);
                jump_to(Op::Jmp, 0, top);
                patch_all(l.breaks, here());
                return;
            }
            case ir::SK::ForRange: for_range(ir::as<ir::ForRange>(s)); return;
            case ir::SK::ForList: for_list(ir::as<ir::ForList>(s)); return;
            case ir::SK::Match: match(ir::as<ir::Match>(s)); return;
            case ir::SK::Return: {
                auto* r = ir::as<ir::Return>(s);
                if (r->value != nullptr) {
                    std::uint16_t v = expr(r->value);
                    run_all_defers();
                    emit(Op::Ret, v);
                } else {
                    run_all_defers();
                    emit(Op::Ret0);
                }
                return;
            }
            case ir::SK::Break:
            case ir::SK::Continue: {
                bool is_break = s->kind == ir::SK::Break;
                const ir::Stmt* target = is_break ? ir::as<ir::Break>(s)->loop : ir::as<ir::Continue>(s)->loop;
                for (auto it = loops_.rbegin(); it != loops_.rend(); ++it) {
                    if (it->node != target) continue;
                    // Run the defers of every scope being left.
                    for (std::size_t i = scopes_.size(); i > it->scope_depth; --i) run_defers(scopes_[i - 1]);
                    (is_break ? it->breaks : it->continues).push_back(emit_jump(Op::Jmp));
                    break;
                }
                return;
            }
            case ir::SK::Defer: {
                if (!scopes_.empty()) scopes_.back().defers.push_back(ir::as<ir::Defer>(s)->body);
                return;
            }
        }
    }

    // A loop body: its own scope (for defers), compiled as a block.
    void body(const ir::Block* b) {
        scopes_.push_back(Scope{});
        block(b);
        run_defers(scopes_.back());
        scopes_.pop_back();
    }

    struct Scope {
        std::vector<const ir::Stmt*> defers;
    };
    struct Loop {
        const ir::Stmt* node = nullptr;
        std::size_t scope_depth = 0;
        std::vector<std::uint32_t> breaks;
        std::vector<std::uint32_t> continues;
    };

    void run_defers(const Scope& sc) {
        for (auto it = sc.defers.rbegin(); it != sc.defers.rend(); ++it) {
            // A deferred body is compiled at every exit; it may not jump out.
            std::vector<Loop> saved;
            std::swap(saved, loops_);
            std::vector<Scope> saved_scopes;
            std::swap(saved_scopes, scopes_);
            scopes_.push_back(Scope{});
            stmt(*it);
            run_defers(scopes_.back());
            std::swap(saved_scopes, scopes_);
            std::swap(saved, loops_);
        }
    }

    void run_all_defers() {
        for (std::size_t i = scopes_.size(); i > 0; --i) run_defers(scopes_[i - 1]);
    }

    void assign(const ir::Assign* a) {
        const ir::Expr* t = a->target;
        switch (t->kind) {
            case ir::EK::Local: {
                LocalVar* v = ir::as<ir::Local>(t)->var;
                std::uint16_t r = regs_.at(v);
                if (a->uses_hole) {
                    if (v->captured) {
                        hole_ = alloc_t(v->type);
                        emit(Op::GetCell, hole_, r);
                    } else {
                        hole_ = r;
                    }
                }
                std::uint16_t val = expr(a->value);
                if (v->captured) {
                    emit(Op::SetCell, r, val);
                } else if (val != r) {
                    emit(Op::Mov, r, val);
                }
                return;
            }
            case ir::EK::Capture: {
                auto* c = ir::as<ir::Capture>(t);
                if (a->uses_hole) {
                    hole_ = alloc_t(c->type);
                    emit(Op::GetUpC, hole_, static_cast<std::uint16_t>(c->index));
                }
                std::uint16_t val = expr(a->value);
                emit(Op::SetUpC, static_cast<std::uint16_t>(c->index), val);
                return;
            }
            case ir::EK::Global: {
                auto idx = static_cast<std::uint16_t>(ir::as<ir::Global>(t)->var->index);
                if (a->uses_hole) {
                    hole_ = alloc_t(t->type);
                    emit(Op::GetG, hole_, idx);
                }
                std::uint16_t val = expr(a->value);
                emit(Op::SetG, idx, val);
                return;
            }
            case ir::EK::Field: {
                auto* f = ir::as<ir::Field>(t);
                std::uint16_t obj = expr(f->object);
                auto idx = static_cast<std::uint16_t>(f->field->index);
                if (a->uses_hole) {
                    hole_ = alloc_t(t->type);
                    span_ = f->object->span;
                    emit(Op::GetF, hole_, obj, idx, field_flags(f->field));
                }
                std::uint16_t val = expr(a->value);
                span_ = f->object->span;
                emit(Op::SetF, obj, idx, val, field_flags(f->field));
                return;
            }
            case ir::EK::Index: {
                auto* ix = ir::as<ir::Index>(t);
                if (TypeTag ot = ix->object->type->tag; ot == TypeTag::Map || ot == TypeTag::Deque) {
                    // m[k] = v / d[i] = v: natives (receiver, key, value).
                    std::uint16_t base = alloc_block(3);
                    expr(ix->object, base);
                    expr(ix->index, base + 1);
                    if (a->uses_hole) {
                        hole_ = alloc_t(t->type);
                        span_ = ix->span;
                        emit(Op::CallN, hole_,
                             static_cast<std::uint16_t>(ot == TypeTag::Map ? Builtin::MapIndex : Builtin::DequeGet), base,
                             2);
                    }
                    expr(a->value, base + 2);
                    span_ = ix->span;
                    std::uint16_t scratch = alloc(false);
                    emit(Op::CallN, scratch,
                         static_cast<std::uint16_t>(ot == TypeTag::Map ? Builtin::MapSet : Builtin::DequeSet), base, 3);
                    return;
                }
                std::uint16_t obj = expr(ix->object);
                std::uint16_t i = expr(ix->index);
                if (a->uses_hole) {
                    hole_ = alloc_t(t->type);
                    span_ = ix->span;
                    emit(Op::GetI, hole_, obj, i, deref_flags());
                }
                std::uint16_t val = expr(a->value);
                span_ = ix->span;
                emit(Op::SetI, obj, i, val, deref_flags());
                return;
            }
            default: return;
        }
    }

    void if_stmt(const ir::If* s) {
        std::uint16_t c = expr(s->cond);
        std::uint32_t to_else = emit_jump(Op::JmpF, c);
        stmt(s->then_body);
        if (s->else_body == nullptr) {
            patch_here(to_else);
            return;
        }
        std::uint32_t to_end = emit_jump(Op::Jmp);
        patch_here(to_else);
        stmt(s->else_body);
        patch_here(to_end);
    }

    void for_range(const ir::ForRange* f) {
        std::uint16_t base = alloc_block(3);
        auto k = static_cast<std::uint8_t>(f->lo->type->is_num() ? static_cast<std::uint8_t>(f->lo->type->num)
                                                                 : static_cast<std::uint8_t>(NumKind::I32));
        expr(f->lo, base);
        expr(f->hi, base + 1);
        if (f->step != nullptr) {
            expr(f->step, base + 2);
        } else {
            emit(Op::LoadI, base + 2, 1, 0, k);
        }
        std::uint8_t flags = k | (f->inclusive ? opk::Inclusive : 0);
        std::uint32_t prep = emit(Op::ForPrep, base, 0, 0, flags);
        std::uint32_t top = here();
        bind(f->var, base);
        loops_.push_back(Loop{f, scopes_.size(), {}, {}});
        body(f->body);
        Loop l = std::move(loops_.back());
        loops_.pop_back();
        patch_all(l.continues, here());
        std::uint32_t loop = emit(Op::ForLoop, base, 0, 0, flags);
        patch(loop, top);
        patch_here(prep);
        patch_all(l.breaks, here());
    }

    // `for` over a Map, Set or Deque (LANGUAGE.md 9.16): IterNext walks the
    // dense entries with a cursor and panics if the collection changed.
    void for_iter(const ir::ForList* f) {
        std::uint16_t coll = alloc(true);
        expr(f->list, coll);
        std::uint16_t cursor = alloc(false);
        emit(Op::LoadI, cursor, 0, 0, static_cast<std::uint8_t>(NumKind::I64));
        std::uint16_t idx = 0;
        if (f->index != nullptr) {
            idx = alloc(false);
            emit(Op::LoadI, idx, 0, 0, static_cast<std::uint8_t>(NumKind::I32));
        }
        std::uint16_t elem = alloc_t(f->elem->type);
        std::uint16_t val = 0;
        if (f->value != nullptr) {
            val = alloc(true);
            (void)val;
        }
        bind(f->elem, elem);
        if (f->value != nullptr) bind(f->value, static_cast<std::uint16_t>(elem + 1));
        if (f->index != nullptr) bind(f->index, idx);
        std::uint32_t top = here();
        emit(Op::IterNext, elem, coll, cursor, f->value != nullptr ? opk::IterPair : 0);
        if (f->value != nullptr) mark_dirty(static_cast<std::uint16_t>(elem + 1));
        mark_dirty(elem);
        std::uint32_t exit = emit_jump(Op::Jmp);
        loops_.push_back(Loop{f, scopes_.size(), {}, {}});
        body(f->body);
        Loop l = std::move(loops_.back());
        loops_.pop_back();
        patch_all(l.continues, here());
        if (f->index != nullptr) emit(Op::Inc, idx, 0, 0, static_cast<std::uint8_t>(NumKind::I32));
        jump_to(Op::Jmp, 0, top);
        patch_here(exit);
        patch_all(l.breaks, here());
    }

    void for_list(const ir::ForList* f) {
        if (f->over != ir::ForList::Over::List) {
            for_iter(f);
            return;
        }
        std::uint16_t list = alloc(true);
        expr(f->list, list);
        std::uint16_t idx = alloc(false);
        emit(Op::LoadI, idx, 0, 0, static_cast<std::uint8_t>(NumKind::I32));
        std::uint16_t elem = alloc_t(f->elem->type);
        bind(f->elem, elem);
        if (f->index != nullptr) bind(f->index, idx);
        std::uint32_t top = here();
        emit(Op::ListNext, elem, list, idx, deref_flags());
        std::uint32_t exit = emit_jump(Op::Jmp);
        loops_.push_back(Loop{f, scopes_.size(), {}, {}});
        body(f->body);
        Loop l = std::move(loops_.back());
        loops_.pop_back();
        patch_all(l.continues, here());
        emit(Op::Inc, idx, 0, 0, static_cast<std::uint8_t>(NumKind::I32));
        jump_to(Op::Jmp, 0, top);
        patch_here(exit);
        patch_all(l.breaks, here());
    }

    void match(const ir::Match* m) {
        std::uint16_t subj = expr(m->subject);
        std::vector<std::uint32_t> ends;
        for (const ir::MatchArm& arm : m->arms) {
            std::vector<std::uint32_t> fails;
            span_ = arm.span;
            test_pattern(arm.pattern, subj, m->subject->type, fails);
            if (arm.guard != nullptr) {
                std::uint16_t g = expr(arm.guard);
                fails.push_back(emit_jump(Op::JmpF, g));
            }
            stmt(arm.body);
            ends.push_back(emit_jump(Op::Jmp));
            patch_all(fails, here());
        }
        // Unreachable for a checked (exhaustive) match.
        std::uint16_t r = alloc(true);
        span_ = m->span;
        emit(Op::LoadK, r, constant(str_value("no match arm matched")));
        emit(Op::CallN, r, static_cast<std::uint16_t>(Builtin::Panic), r, 1);
        patch_all(ends, here());
    }

    // Falls through when `p` matches the value in `subj`; otherwise jumps to
    // a location added to `fails`. Binds pattern variables as it goes.
    void test_pattern(const ir::Pattern* p, std::uint16_t subj, const Type* subj_type,
                      std::vector<std::uint32_t>& fails) {
        bool nullable = subj_type->is_nullable() && subj_type->tag != TypeTag::Null;
        switch (p->kind) {
            case ir::PK::Wildcard: return;
            case ir::PK::Bind: {
                auto* b = ir::as<ir::BindPat>(p);
                std::uint16_t r = alloc_t(b->var->type);
                emit(Op::Mov, r, subj);
                bind(b->var, r);
                return;
            }
            case ir::PK::Literal: {
                auto* l = ir::as<ir::LiteralPat>(p);
                if (nullable) fails.push_back(emit_jump(Op::JmpNull, subj));
                std::uint16_t k = alloc_t(l->type);
                load_const(k, l->value, l->type);
                std::uint16_t t = alloc(false);
                emit(Op::Eq, t, subj, k, kind_of(l->type));
                fails.push_back(emit_jump(Op::JmpF, t));
                return;
            }
            case ir::PK::Range: {
                auto* r = ir::as<ir::RangePat>(p);
                if (nullable) fails.push_back(emit_jump(Op::JmpNull, subj));
                std::uint8_t kind = kind_of(r->type);
                std::uint16_t lo = alloc(false);
                load_const(lo, r->lo, r->type);
                std::uint16_t t = alloc(false);
                emit(Op::Le, t, lo, subj, kind);
                fails.push_back(emit_jump(Op::JmpF, t));
                std::uint16_t hi = alloc(false);
                load_const(hi, r->hi, r->type);
                emit(r->inclusive ? Op::Le : Op::Lt, t, subj, hi, kind);
                fails.push_back(emit_jump(Op::JmpF, t));
                return;
            }
            case ir::PK::Variant: {
                auto* v = ir::as<ir::VariantPat>(p);
                if (nullable) fails.push_back(emit_jump(Op::JmpNull, subj));
                std::uint16_t t = alloc(false);
                if (v->en->plain) {
                    std::uint16_t k = alloc(false);
                    ConstValue c;
                    c.kind = ConstValue::Kind::Int;
                    c.i = v->en->variants[v->variant].discriminant;
                    load_const(k, c, v->en->type);
                    emit(Op::Eq, t, subj, k, opk::Enum);
                    fails.push_back(emit_jump(Op::JmpF, t));
                    return;
                }
                std::uint16_t k = alloc(false);
                emit(Op::Variant, t, subj);
                emit(Op::LoadI, k, static_cast<std::uint16_t>(v->variant), 0, static_cast<std::uint8_t>(NumKind::I32));
                emit(Op::Eq, t, t, k, static_cast<std::uint8_t>(NumKind::I32));
                fails.push_back(emit_jump(Op::JmpF, t));
                const VariantInfo& vi = v->en->variants[v->variant];
                for (std::uint32_t i = 0; i < v->args.size(); ++i) {
                    const ir::Pattern* sub = v->args[i];
                    if (sub->kind == ir::PK::Wildcard) continue;
                    const Type* ft = i < vi.field_types.size() ? vi.field_types[i] : sub->type;
                    std::uint16_t f = alloc_t(ft);
                    emit(Op::GetPay, f, subj, static_cast<std::uint16_t>(i));
                    test_pattern(sub, f, ft, fails);
                }
                return;
            }
            case ir::PK::Class: {
                auto* c = ir::as<ir::ClassPat>(p);
                std::uint16_t t = alloc(false);
                emit(Op::InstOf, t, subj, static_cast<std::uint16_t>(c->cls->rt_index));
                fails.push_back(emit_jump(Op::JmpF, t));
                if (c->bind != nullptr) {
                    std::uint16_t r = alloc(true);
                    emit(Op::Mov, r, subj);
                    bind(c->bind, r);
                }
                return;
            }
            case ir::PK::Null: fails.push_back(emit_jump(Op::JmpNN, subj)); return;
            case ir::PK::Result: {
                auto* rp = ir::as<ir::ResultPat>(p);
                if (nullable) fails.push_back(emit_jump(Op::JmpNull, subj));
                std::uint16_t t = alloc(false);
                std::uint16_t k = alloc(false);
                emit(Op::Variant, t, subj);
                emit(Op::LoadI, k, rp->ok ? 0 : 1, 0, static_cast<std::uint8_t>(NumKind::I32));
                emit(Op::Eq, t, t, k, static_cast<std::uint8_t>(NumKind::I32));
                fails.push_back(emit_jump(Op::JmpF, t));
                if (rp->inner != nullptr && rp->inner->kind != ir::PK::Wildcard) {
                    const Type* rt = subj_type->tag == TypeTag::Nullable ? subj_type->args[0] : subj_type;
                    const Type* pt = rt->args[rp->ok ? 0 : 1];
                    std::uint16_t f = alloc_t(pt);
                    emit(Op::GetPay, f, subj, 0);
                    test_pattern(rp->inner, f, pt, fails);
                }
                return;
            }
            case ir::PK::NonNull: {
                auto* n = ir::as<ir::NonNullPat>(p);
                fails.push_back(emit_jump(Op::JmpNull, subj));
                if (n->bind != nullptr) {
                    std::uint16_t r = alloc_t(n->bind->type);
                    emit(Op::Mov, r, subj);
                    bind(n->bind, r);
                }
                return;
            }
            case ir::PK::Or: {
                auto* o = ir::as<ir::OrPat>(p);
                std::vector<std::uint32_t> oks;
                for (std::uint32_t i = 0; i < o->alternatives.size(); ++i) {
                    bool last = i + 1 == o->alternatives.size();
                    if (last) {
                        test_pattern(o->alternatives[i], subj, subj_type, fails);
                    } else {
                        std::vector<std::uint32_t> alt_fails;
                        test_pattern(o->alternatives[i], subj, subj_type, alt_fails);
                        oks.push_back(emit_jump(Op::Jmp));
                        patch_all(alt_fails, here());
                    }
                }
                patch_all(oks, here());
                return;
            }
        }
    }

    // ------------------------------------------------------------ expressions

    // Compiles `e`; returns the register holding the result (`want` if given).
    std::uint16_t expr(const ir::Expr* e, int want = -1) {
        Span saved = span_;
        span_ = e->span;
        std::uint16_t r = expr_inner(e, want);
        span_ = saved;
        return r;
    }

    std::uint16_t move_to(std::uint16_t r, int want) {
        if (want < 0 || r == want) return r;
        emit(Op::Mov, static_cast<std::uint16_t>(want), r);
        return static_cast<std::uint16_t>(want);
    }

    // Evaluates expressions into consecutive fresh registers.
    std::uint16_t args_block(const List<ir::Expr*>& args, std::size_t extra_front = 0) {
        std::uint16_t base = alloc_block(extra_front + args.size());
        for (std::uint32_t i = 0; i < args.size(); ++i) expr(args[i], base + static_cast<int>(extra_front + i));
        return base;
    }

    std::uint16_t expr_inner(const ir::Expr* e, int want) {
        switch (e->kind) {
            case ir::EK::Const: {
                std::uint16_t r = dst(want, e->type);
                load_const(r, ir::as<ir::Const>(e)->value, e->type);
                return r;
            }
            case ir::EK::Local: {
                LocalVar* v = ir::as<ir::Local>(e)->var;
                std::uint16_t reg = regs_.at(v);
                if (v->captured) {
                    std::uint16_t r = dst(want, e->type);
                    emit(Op::GetCell, r, reg);
                    return r;
                }
                if (want >= 0 && may_hold_object(e->type)) mark_dirty(static_cast<std::uint16_t>(want));
                return move_to(reg, want);
            }
            case ir::EK::Capture: {
                auto* c = ir::as<ir::Capture>(e);
                std::uint16_t r = dst(want, e->type);
                emit(c->var->is_mutable ? Op::GetUpC : Op::GetUp, r, static_cast<std::uint16_t>(c->index));
                return r;
            }
            case ir::EK::Global: {
                std::uint16_t r = dst(want, e->type);
                emit(Op::GetG, r, static_cast<std::uint16_t>(ir::as<ir::Global>(e)->var->index));
                return r;
            }
            case ir::EK::FnRef: {
                std::uint16_t r = dst(want, e->type);
                emit(Op::Closure, r, static_cast<std::uint16_t>(ir::as<ir::FnRef>(e)->fn->index), r);
                return r;
            }
            case ir::EK::Unary: {
                auto* u = ir::as<ir::Unary>(e);
                std::uint16_t x = expr(u->operand);
                std::uint16_t r = dst(want, e->type);
                switch (u->op) {
                    case UnaryOp::Neg: emit(Op::Neg, r, x, 0, kind_of(e->type)); break;
                    case UnaryOp::Not: emit(Op::Not, r, x); break;
                    case UnaryOp::BitNot: emit(Op::BNot, r, x, 0, kind_of(e->type)); break;
                }
                return r;
            }
            case ir::EK::Binary: return binary(ir::as<ir::Binary>(e), want);
            case ir::EK::Logical: {
                auto* l = ir::as<ir::Logical>(e);
                std::uint16_t r = dst(want, e->type);
                expr(l->lhs, r);
                std::uint32_t j = emit_jump(l->is_and ? Op::JmpF : Op::JmpT, r);
                expr(l->rhs, r);
                patch_here(j);
                return r;
            }
            case ir::EK::Coalesce: {
                auto* c = ir::as<ir::Coalesce>(e);
                std::uint16_t r = dst(want, e->type);
                expr(c->lhs, r);
                std::uint32_t j = emit_jump(Op::JmpNN, r);
                expr(c->rhs, r);
                patch_here(j);
                return r;
            }
            case ir::EK::Ternary: {
                auto* t = ir::as<ir::Ternary>(e);
                std::uint16_t c = expr(t->cond);
                std::uint16_t r = dst(want, e->type);
                std::uint32_t to_else = emit_jump(Op::JmpF, c);
                expr(t->then_expr, r);
                std::uint32_t to_end = emit_jump(Op::Jmp);
                patch_here(to_else);
                expr(t->else_expr, r);
                patch_here(to_end);
                return r;
            }
            case ir::EK::Cast: {
                auto* c = ir::as<ir::Cast>(e);
                std::uint16_t x = expr(c->operand);
                std::uint16_t r = dst(want, e->type);
                if (c->op == ir::Cast::Op::Downcast) {
                    span_ = c->operand->span;
                    emit(Op::Downcast, r, x, static_cast<std::uint16_t>(e->type->cls()->rt_index));
                    return r;
                }
                const Type* from = c->operand->type;
                std::uint8_t fk = from->tag == TypeTag::Enum ? opk::Enum : kind_of(from);
                emit(Op::Cast, r, x, fk, kind_of(e->type));
                return r;
            }
            case ir::EK::Call: return call(ir::as<ir::Call>(e), want, 0);
            case ir::EK::MethodCall: return method_call(ir::as<ir::MethodCall>(e), want, 0);
            case ir::EK::Start: {
                // The owner goes in the register just below the arguments;
                // Call/CallV with k = 1 (no owner) or 2 start a coroutine.
                auto* s = ir::as<ir::Start>(e);
                std::uint8_t k = 1;
                if (s->owner != nullptr) {
                    std::uint16_t owner = alloc(true);
                    expr(s->owner, owner);
                    k = 2;
                }
                if (auto c = ir::as<ir::Call>(s->call)) return call(c, want, k);
                return method_call(ir::as<ir::MethodCall>(s->call), want, k);
            }
            case ir::EK::Await: {
                auto* a = ir::as<ir::Await>(e);
                if (a->await_kind == ir::AwaitKind::Call) return expr(a->operand, want);
                std::uint16_t x = expr(a->operand);
                std::uint16_t r = dst(want, e->type->is_void() ? e->type : e->type);
                mark_dirty(r);
                emit(Op::Await, r, x, 0, static_cast<std::uint8_t>(a->await_kind));
                return r;
            }
            case ir::EK::VecNew: {
                auto* v = ir::as<ir::VecNew>(e);
                std::uint16_t base = args_block(v->parts);
                std::uint16_t r = dst(want, e->type);
                emit(Op::VecNew, r, base, static_cast<std::uint16_t>(v->parts.size()),
                     static_cast<std::uint8_t>(e->type->dim));
                return r;
            }
            case ir::EK::Swizzle: {
                auto* s = ir::as<ir::Swizzle>(e);
                std::uint16_t x = expr(s->operand);
                std::uint16_t r = dst(want, e->type);
                emit(Op::Swz, r, x, s->count, pack_indices(s->index, s->count));
                return r;
            }
            case ir::EK::VecWith: {
                auto* w = ir::as<ir::VecWith>(e);
                std::uint16_t x = expr(w->operand);
                std::uint16_t v = expr(w->value);
                std::uint16_t r = dst(want, e->type);
                emit(Op::VecWith, r, x, v, pack_indices(w->index, w->count));
                return r;
            }
            case ir::EK::DynMethodCall: {
                auto* m = ir::as<ir::DynMethodCall>(e);
                std::uint16_t base = alloc_block(1 + m->args.size());
                expr(m->receiver, base);
                for (std::uint32_t i = 0; i < m->args.size(); ++i) expr(m->args[i], base + 1 + static_cast<int>(i));
                std::uint16_t r = dst(want, e->type);
                std::uint16_t name = constant(str_value(std::string(m->name)));
                span_ = m->receiver->span;
                emit(Op::CallD, r, name, base, static_cast<std::uint8_t>(1 + m->args.size()));
                return r;
            }
            case ir::EK::ClosureCall: {
                auto* c = ir::as<ir::ClosureCall>(e);
                std::uint16_t f = alloc(true);
                expr(c->callee, f);
                std::uint16_t base = args_block(c->args);
                std::uint16_t r = dst(want, e->type);
                span_ = c->callee->span;
                emit(Op::CallC, r, f, base);
                return r;
            }
            case ir::EK::BuiltinCall: {
                auto* b = ir::as<ir::BuiltinCall>(e);
                if (b->builtin == Builtin::ListNew) {
                    std::uint16_t r = dst(want, e->type);
                    emit(Op::NewList, r, r, 0, list_flags(e->type, b->arena));
                    return r;
                }
                if (b->builtin == Builtin::MapNew || b->builtin == Builtin::SetNew || b->builtin == Builtin::DequeNew) {
                    return collection_new(b->builtin, e->type, want);
                }
                if (b->builtin == Builtin::MapLit || b->builtin == Builtin::SetLit || b->builtin == Builtin::DequeLit) {
                    // A new collection, then one insertion per element in order.
                    Builtin make = b->builtin == Builtin::MapLit   ? Builtin::MapNew
                                   : b->builtin == Builtin::SetLit ? Builtin::SetNew
                                                                   : Builtin::DequeNew;
                    Builtin put = b->builtin == Builtin::MapLit   ? Builtin::MapSet
                                  : b->builtin == Builtin::SetLit ? Builtin::SetAdd
                                                                  : Builtin::DequePushBack;
                    std::uint32_t per = b->builtin == Builtin::MapLit ? 2 : 1;
                    std::uint16_t base = alloc_block(1 + per);
                    collection_new(make, e->type, base);
                    std::uint16_t scratch = alloc(false);
                    for (std::uint32_t i = 0; i < b->args.size(); i += per) {
                        for (std::uint32_t j = 0; j < per; ++j) expr(b->args[i + j], base + 1 + static_cast<int>(j));
                        emit(Op::CallN, scratch, static_cast<std::uint16_t>(put), base,
                             static_cast<std::uint8_t>(1 + per));
                    }
                    if (want >= 0) mark_dirty(static_cast<std::uint16_t>(want));
                    return move_to(base, want);
                }
                std::uint16_t base = args_block(b->args);
                std::uint16_t r = dst(want, e->type);
                emit(Op::CallN, r, static_cast<std::uint16_t>(b->builtin), base,
                     static_cast<std::uint8_t>(b->args.size()));
                return r;
            }
            case ir::EK::New: {
                auto* n = ir::as<ir::New>(e);
                std::uint16_t base = alloc_block(1 + n->args.size());
                emit(Op::NewObj, base, static_cast<std::uint16_t>(n->cls->rt_index), 0,
                     static_cast<std::uint8_t>(n->arena));
                std::uint16_t scratch = alloc(true);
                emit(Op::Call, scratch, static_cast<std::uint16_t>(n->cls->field_init->index), base);
                if (n->init != nullptr) {
                    for (std::uint32_t i = 0; i < n->args.size(); ++i) {
                        expr(n->args[i], base + 1 + static_cast<int>(i));
                    }
                    emit(Op::Call, scratch, static_cast<std::uint16_t>(n->init->index), base);
                }
                if (want >= 0) mark_dirty(static_cast<std::uint16_t>(want));
                return move_to(base, want);
            }
            case ir::EK::StructNew: {
                auto* s = ir::as<ir::StructNew>(e);
                std::uint16_t base = alloc_block(s->fields.size());
                for (std::uint32_t slot : s->order) expr(s->fields[slot], base + static_cast<int>(slot));
                std::uint16_t r = dst(want, e->type);
                emit(Op::NewStruct, r, static_cast<std::uint16_t>(s->st->rt_index), base);
                return r;
            }
            case ir::EK::EnumNew: {
                auto* n = ir::as<ir::EnumNew>(e);
                std::uint16_t r = dst(want, e->type);
                if (n->en->plain) {
                    ConstValue c;
                    c.kind = ConstValue::Kind::Int;
                    c.i = n->en->variants[n->variant].discriminant;
                    emit(Op::LoadK, r, constant(const_value(c, e->type)));
                    return r;
                }
                std::uint16_t base = args_block(n->args);
                emit(Op::NewEnum, r, static_cast<std::uint16_t>(n->en->rt_index), base,
                     static_cast<std::uint8_t>(n->variant));
                return r;
            }
            case ir::EK::Field: {
                auto* f = ir::as<ir::Field>(e);
                std::uint16_t obj = expr(f->object);
                std::uint16_t r = dst(want, e->type);
                span_ = f->object->span;
                emit(Op::GetF, r, obj, static_cast<std::uint16_t>(f->field->index), field_flags(f->field));
                return r;
            }
            case ir::EK::Index: {
                auto* ix = ir::as<ir::Index>(e);
                std::uint16_t obj = expr(ix->object);
                std::uint16_t i = expr(ix->index);
                std::uint16_t r = dst(want, e->type);
                if (ix->object->type->is_vec()) {
                    emit(Op::VecIdx, r, obj, i);
                } else if (TypeTag ot = ix->object->type->tag; ot == TypeTag::Map || ot == TypeTag::Deque) {
                    std::uint16_t base = alloc_block(2);
                    emit(Op::Mov, base, obj);
                    emit(Op::Mov, base + 1, i);
                    emit(Op::CallN, r,
                         static_cast<std::uint16_t>(ot == TypeTag::Map ? Builtin::MapIndex : Builtin::DequeGet), base, 2);
                } else {
                    emit(Op::GetI, r, obj, i, deref_flags());
                }
                return r;
            }
            case ir::EK::Lambda: {
                const Function* f = ir::as<ir::Lambda>(e)->fn;
                std::uint16_t base = alloc_block(f->captures.size());
                for (std::size_t i = 0; i < f->captures.size(); ++i) {
                    const CaptureInfo& c = f->captures[i];
                    auto r = static_cast<std::uint16_t>(base + i);
                    if (c.from_outer_capture) {
                        emit(Op::GetUp, r, static_cast<std::uint16_t>(c.outer_index));
                    } else {
                        // The raw register: the cell itself for captured vars.
                        emit(Op::Mov, r, regs_.at(c.var));
                    }
                }
                std::uint16_t r = dst(want, e->type);
                emit(Op::Closure, r, static_cast<std::uint16_t>(f->index), base);
                return r;
            }
            case ir::EK::ToStr: {
                std::uint16_t x = expr(ir::as<ir::ToStr>(e)->operand);
                std::uint16_t r = dst(want, e->type);
                emit(Op::ToStr, r, x);
                return r;
            }
            case ir::EK::Concat: {
                auto* c = ir::as<ir::Concat>(e);
                std::uint16_t base = args_block(c->parts);
                std::uint16_t r = dst(want, e->type);
                emit(Op::Concat, r, base, static_cast<std::uint16_t>(c->parts.size()));
                return r;
            }
            case ir::EK::Is: {
                auto* is = ir::as<ir::Is>(e);
                std::uint16_t subj = expr(is->operand);
                std::uint16_t r = dst(want, e->type);
                std::vector<std::uint32_t> fails;
                test_pattern(is->pattern, subj, is->operand->type, fails);
                emit(Op::LoadBool, r, 1);
                std::uint32_t end = emit_jump(Op::Jmp);
                patch_all(fails, here());
                emit(Op::LoadBool, r, 0);
                patch_here(end);
                return r;
            }
            case ir::EK::Copy: {
                std::uint16_t x = expr(ir::as<ir::Copy>(e)->operand);
                std::uint16_t r = dst(want, e->type);
                emit(Op::Copy, r, x);
                return r;
            }
            case ir::EK::ListLit: {
                auto* l = ir::as<ir::ListLit>(e);
                std::uint16_t base = args_block(l->elements);
                std::uint16_t r = dst(want, e->type);
                emit(Op::NewList, r, base, static_cast<std::uint16_t>(l->elements.size()), list_flags(e->type, ArenaKind::None));
                return r;
            }
            case ir::EK::Try: return try_expr(ir::as<ir::Try>(e), want);
            case ir::EK::BoundMethod: {
                auto* bm = ir::as<ir::BoundMethod>(e);
                std::uint16_t recv = alloc(true);
                expr(bm->receiver, recv);
                std::uint16_t r = dst(want, e->type);
                emit(Op::Closure, r, static_cast<std::uint16_t>(bm->thunk->index), recv);
                return r;
            }
            case ir::EK::SafeNav: {
                auto* s = ir::as<ir::SafeNav>(e);
                std::uint16_t obj = alloc(true);
                expr(s->object, obj);
                std::uint16_t r = dst(want, e->type);
                std::uint32_t to_null = emit_jump(Op::JmpNull, obj);
                bind(s->tmp, obj);
                expr(s->body, r);
                if (e->type->is_void()) {
                    patch_here(to_null);
                    return r;
                }
                std::uint32_t end = emit_jump(Op::Jmp);
                patch_here(to_null);
                emit(Op::LoadNull, r);
                patch_here(end);
                return r;
            }
            case ir::EK::Default: return default_value(e->type, want);
            case ir::EK::Free: {
                const ir::Expr* operand = ir::as<ir::Free>(e)->operand;
                std::uint16_t x = expr(operand);
                span_ = operand->span; // panics name the operand
                emit(Op::Free, x);
                return dst(want, e->type);
            }
            case ir::EK::Hole: return move_to(hole_, want);
        }
        return dst(want, e->type);
    }

    // `x?` (DESIGN.md 5.8): Err / null leaves the function with that value
    // (Result values don't depend on T, so the same object is returned).
    std::uint16_t try_expr(const ir::Try* t, int want) {
        std::uint16_t x = alloc(true);
        expr(t->operand, x);
        std::uint32_t ok = 0;
        if (t->is_result) {
            std::uint16_t v = alloc(false);
            std::uint16_t one = alloc(false);
            emit(Op::Variant, v, x);
            emit(Op::LoadI, one, 1, 0, static_cast<std::uint8_t>(NumKind::I32));
            emit(Op::Eq, v, v, one, static_cast<std::uint8_t>(NumKind::I32));
            ok = emit_jump(Op::JmpF, v);
        } else {
            ok = emit_jump(Op::JmpNN, x);
        }
        run_all_defers();
        emit(Op::Ret, x);
        patch_here(ok);
        std::uint16_t r = dst(want, t->type);
        if (t->is_result) {
            emit(Op::GetPay, r, x, 0);
        } else {
            emit(Op::Mov, r, x);
        }
        return r;
    }

    // Map<K, V>() / Set<T>() / Deque<T>(): the native gets whether the
    // element types are acyclic (pluma/acyclic.hpp), like NewList.
    std::uint16_t collection_new(Builtin b, const Type* t, int want) {
        bool untraced = true;
        for (const Type* a : t->args) untraced = untraced && acyclic_.holder_acyclic(a);
        std::uint16_t flag = alloc(false);
        emit(Op::LoadBool, flag, untraced ? 1 : 0);
        std::uint16_t r = dst(want, t);
        emit(Op::CallN, r, static_cast<std::uint16_t>(b), flag, 1);
        return r;
    }

    std::uint16_t call(const ir::Call* c, int want, std::uint8_t start) {
        std::uint16_t base = args_block(c->args);
        std::uint16_t r = dst(want, start != 0 ? vm_task_type() : c->type);
        if (start != 0) mark_dirty(r);
        emit(Op::Call, r, static_cast<std::uint16_t>(c->fn->index), base, start);
        return r;
    }

    std::uint16_t method_call(const ir::MethodCall* m, int want, std::uint8_t start) {
        std::uint16_t base = alloc_block(1 + m->args.size());
        expr(m->receiver, base);
        for (std::uint32_t i = 0; i < m->args.size(); ++i) expr(m->args[i], base + 1 + static_cast<int>(i));
        std::uint16_t r = dst(want, start != 0 ? vm_task_type() : m->type);
        if (start != 0) mark_dirty(r);
        span_ = m->receiver->span;
        if (m->is_virtual) {
            emit(Op::CallV, r, static_cast<std::uint16_t>(m->vslot), base,
                 static_cast<std::uint8_t>(start | deref_flags()));
        } else {
            emit(Op::Call, r, static_cast<std::uint16_t>(m->fn->index), base, start);
        }
        return r;
    }

    // Any reference type: only used to mark registers that hold objects.
    static const Type* vm_task_type() {
        static const Type task = [] {
            Type t;
            t.tag = TypeTag::Task;
            return t;
        }();
        return &task;
    }

    std::uint16_t default_value(const Type* t, int want) {
        std::uint16_t r = dst(want, t);
        switch (t->tag) {
            case TypeTag::Vec: {
                std::vector<float> zero(t->dim, 0.0f);
                emit(Op::LoadK, r, constant(vec_value(zero.data(), t->dim)));
                return r;
            }
            case TypeTag::Signal: emit(Op::NewSig, r); return r;
            case TypeTag::Num: emit(Op::LoadI, r, 0, 0, static_cast<std::uint8_t>(t->num)); return r;
            case TypeTag::Bool: emit(Op::LoadBool, r, 0); return r;
            case TypeTag::Str: emit(Op::LoadK, r, constant(str_value(""))); return r;
            case TypeTag::Name: {
                ConstValue c;
                c.kind = ConstValue::Kind::Name;
                emit(Op::LoadK, r, constant(const_value(c, t)));
                return r;
            }
            case TypeTag::Enum: {
                EnumInfo* en = t->en();
                if (en->plain) {
                    ConstValue c;
                    c.kind = ConstValue::Kind::Int;
                    c.i = en->variants.empty() ? 0 : en->variants[0].discriminant;
                    emit(Op::LoadK, r, constant(const_value(c, t)));
                } else {
                    emit(Op::NewEnum, r, static_cast<std::uint16_t>(en->rt_index), r, 0);
                }
                return r;
            }
            case TypeTag::Struct:
                emit(Op::Call, r, static_cast<std::uint16_t>(t->st()->default_ctor->index), r);
                return r;
            case TypeTag::Array: {
                // N copies of the element's zero value (LANGUAGE.md 9.16).
                std::uint16_t base = alloc_block(3);
                ConstValue n;
                n.kind = ConstValue::Kind::Int;
                n.i = t->dim;
                load_const(base, n, T_int());
                default_value(t->args[0], base + 1);
                emit(Op::LoadBool, base + 2, acyclic_.holder_acyclic(t->args[0]) ? 1 : 0);
                emit(Op::CallN, r, static_cast<std::uint16_t>(Builtin::ArrayNew), base, 3);
                return r;
            }
            default: emit(Op::LoadNull, r); return r;
        }
    }

    std::uint16_t binary(const ir::Binary* b, int want) {
        std::uint16_t l = expr(b->lhs);
        std::uint16_t r = expr(b->rhs);
        std::uint16_t d = dst(want, b->type);
        std::uint8_t k = kind_of(b->operand_type);
        if (b->vec_scalar == 1) k = opk::VecS;
        if (b->vec_scalar == 2) k = opk::SVec;
        Op op = Op::Nop;
        bool swap = false;
        switch (b->op) {
            case BinaryOp::Add: op = Op::Add; break;
            case BinaryOp::Sub: op = Op::Sub; break;
            case BinaryOp::Mul: op = Op::Mul; break;
            case BinaryOp::Div: op = Op::Div; break;
            case BinaryOp::Rem: op = Op::Rem; break;
            case BinaryOp::BitAnd: op = Op::BAnd; break;
            case BinaryOp::BitOr: op = Op::BOr; break;
            case BinaryOp::BitXor: op = Op::BXor; break;
            case BinaryOp::Shl: op = Op::Shl; break;
            case BinaryOp::Shr: op = Op::Shr; break;
            case BinaryOp::Eq: op = Op::Eq; break;
            case BinaryOp::Ne: op = Op::Ne; break;
            case BinaryOp::Lt: op = Op::Lt; break;
            case BinaryOp::Le: op = Op::Le; break;
            case BinaryOp::Gt: op = Op::Lt; swap = true; break;
            case BinaryOp::Ge: op = Op::Le; swap = true; break;
            default: break;
        }
        if (swap) std::swap(l, r);
        emit(op, d, l, r, k);
        return d;
    }

    // NewList `k`: the arena, plus opk::UntracedList when the element type
    // is acyclic (pluma/acyclic.hpp).
    std::uint8_t list_flags(const Type* list_type, ArenaKind arena) const {
        const bool array = list_type->tag == TypeTag::Array;
        const Type* t = list_type->tag == TypeTag::List || array ? list_type->args[0] : nullptr;
        bool untraced = t != nullptr && acyclic_.holder_acyclic(t);
        return static_cast<std::uint8_t>(static_cast<std::uint8_t>(arena) | (untraced ? opk::UntracedList : 0) |
                                         (array ? opk::ArrayList : 0));
    }

    // `int` without a TypeTable (only its tag and kind matter to load_const).
    static const Type* T_int() {
        static const Type t = [] {
            Type x;
            x.tag = TypeTag::Num;
            x.num = NumKind::I32;
            return x;
        }();
        return &t;
    }

    VM& vm_;
    const Function* fn_;
    Proto* proto_;
    const AcyclicAnalysis& acyclic_;
    int unsafe_ = 0;
    std::map<const LocalVar*, std::uint16_t> regs_;
    std::vector<std::size_t> open_locals_; // indices into proto_->locals still live
    std::map<std::string, std::uint16_t> str_consts_;
    std::vector<bool> dirty_;
    std::vector<Scope> scopes_;
    std::vector<Loop> loops_;
    std::uint16_t next_ = 0;
    std::uint16_t max_reg_ = 0;
    std::uint16_t high_ = 0;
    std::uint16_t hole_ = 0;
    Span span_;
};

// Collector flags of a type's objects (pluma/acyclic.hpp).
std::uint8_t record_gc_flags(const RecordInfo& r, const AcyclicAnalysis& acyclic) {
    if (r.acyclic) return objflag::Untraced;
    if (r.is_class && acyclic.unproven_promise(static_cast<const ClassInfo*>(&r))) return objflag::Promised;
    return 0;
}

void build_record(VM& vm, RecordRT& rt, const RecordInfo& r) {
    rt.name = std::string(r.name);
    rt.is_struct = !r.is_class;
    for (const FieldInfo* f : r.fields) {
        rt.field_names.emplace_back(f->name);
        rt.field_types.push_back(type_name(f->type));
        rt.field_exported.push_back(f->exported);
        rt.field_weak.push_back(f->weak);
    }
    rt.finalizer = vm.native_finalizer(rt.name);
    if (r.is_class) {
        const auto& c = static_cast<const ClassInfo&>(r);
        if (c.field_init != nullptr) rt.field_init = c.field_init->index;
        if (c.base != nullptr) {
            rt.base = vm.classes[c.base->rt_index].get();
            rt.methods = rt.base->methods;
            if (rt.finalizer == nullptr) rt.finalizer = rt.base->finalizer;
        }
        for (const Function* f : c.vtable) rt.vtable.push_back(f->index);
    }
    for (const auto& [name, f] : r.methods) {
        if (f->kind == FnKind::Method && name != "init") rt.methods[std::string(name)] = f->index;
    }
}

} // namespace

void compile_program(Program& program, VM& vm) {
    // Types first: every new RecordRT exists before any is filled in, since a
    // class may be declared before its base.
    std::size_t first_class = vm.classes.size();
    while (vm.classes.size() < program.classes.size()) vm.classes.push_back(std::make_unique<RecordRT>());
    std::size_t first_struct = vm.structs.size();
    while (vm.structs.size() < program.structs.size()) vm.structs.push_back(std::make_unique<RecordRT>());
    // Bases before subclasses.
    std::vector<bool> built(program.classes.size(), false);
    std::function<void(std::size_t)> build_class = [&](std::size_t i) {
        if (built[i]) return;
        built[i] = true;
        const ClassInfo& c = *program.classes[i];
        if (c.base != nullptr && c.base->rt_index >= first_class) build_class(c.base->rt_index);
        build_record(vm, *vm.classes[i], c);
    };
    for (std::size_t i = first_class; i < program.classes.size(); ++i) build_class(i);
    for (std::size_t i = first_struct; i < program.structs.size(); ++i) {
        build_record(vm, *vm.structs[i], *program.structs[i]);
    }
    for (std::size_t i = vm.enums.size(); i < program.enums.size(); ++i) {
        const EnumInfo& e = *program.enums[i];
        auto rt = std::make_unique<EnumRT>();
        rt->name = std::string(e.name);
        rt->plain = e.plain;
        for (const VariantInfo& v : e.variants) {
            VariantRT vr;
            vr.name = std::string(v.name);
            for (std::string_view f : v.field_names) vr.field_names.emplace_back(f);
            vr.discriminant = v.discriminant;
            rt->variants.push_back(std::move(vr));
        }
        vm.enums.push_back(std::move(rt));
    }
    vm.globals.resize(program.global_vars.size());
    for (std::size_t i = vm.global_names.size(); i < program.global_vars.size(); ++i) {
        const GlobalVar& g = *program.global_vars[i];
        vm.global_names.push_back(g.qualified);
        vm.global_types.push_back(g.type != nullptr ? type_name(g.type) : std::string());
    }
    std::size_t first_fn = vm.protos.size();
    while (vm.protos.size() < program.functions.size()) {
        auto p = std::make_unique<Proto>();
        p->index = static_cast<std::uint32_t>(vm.protos.size());
        vm.protos.push_back(std::move(p));
    }
    // Acyclicity of every type (earlier units' too: a new subclass can make
    // an old type cyclic). Development builds check `@acyclic` promises.
    AcyclicAnalysis acyclic;
    acyclic.run(program, !vm.options().check_acyclic);
    for (std::size_t i = 0; i < program.classes.size(); ++i) {
        vm.classes[i]->gc_flags = record_gc_flags(*program.classes[i], acyclic);
    }
    for (std::size_t i = 0; i < program.structs.size(); ++i) {
        vm.structs[i]->gc_flags = record_gc_flags(*program.structs[i], acyclic);
    }
    for (std::size_t i = 0; i < program.enums.size(); ++i) {
        vm.enums[i]->gc_flags = program.enums[i]->acyclic ? objflag::Untraced : 0;
    }
    for (std::size_t i = first_fn; i < program.functions.size(); ++i) {
        FnCompiler fc(vm, program.functions[i].get(), vm.protos[i].get(), acyclic);
        fc.compile();
    }
}

} // namespace pluma

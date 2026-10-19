#include "pluma/reload.hpp"

#include "pluma/collections.hpp"

#include <format>

namespace pluma {

namespace {

const EnumRT* find_enum(const VM& vm, std::string_view name, std::uint32_t* index) {
    for (std::size_t i = 0; i < vm.enums.size(); ++i) {
        if (vm.enums[i]->name == name) {
            if (index != nullptr) *index = static_cast<std::uint32_t>(i);
            return vm.enums[i].get();
        }
    }
    return nullptr;
}

int field_index(const RecordRT* rt, std::string_view name) {
    for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
        if (rt->field_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

void Migrator::warn(std::string message) { warnings_.push_back(std::move(message)); }

void Migrator::begin() {
    for (CoroObj* c : from_.coroutines()) {
        const std::string& key = from_.protos[c->start_proto]->key;
        auto fn = to_.proto_by_key(key);
        if (!fn) {
            warn(std::format("coroutine '{}' no longer exists; it was stopped", key));
            continue;
        }
        coros_.emplace_back(c, to_.create_coroutine(*fn, {}, nullptr));
    }
}

Value Migrator::migrate(const Value& v) { return convert(v); }

void Migrator::migrate_globals() {
    for (std::size_t i = 0; i < from_.global_names.size(); ++i) {
        auto j = to_.global_index(from_.global_names[i]);
        if (!j) continue; // removed
        if (to_.global_types[*j] != from_.global_types[i]) {
            warn(std::format("'{}' changed type from '{}' to '{}'; it was reset", from_.global_names[i],
                             from_.global_types[i], to_.global_types[*j]));
            continue;
        }
        assign_owned(to_.globals[*j], convert(from_.globals[i]));
    }
}

RunResult Migrator::finish() {
    for (auto& [old_c, new_c] : coros_) {
        std::vector<Value> args;
        for (const Value& a : old_c->start_args) args.push_back(convert(a));
        new_c->start_args = std::move(args);
        if (old_c->owner != nullptr) {
            auto it = memo_.find(old_c->owner);
            if (it != memo_.end() && it->second.is_obj()) {
                new_c->owner = it->second.o;
                ++new_c->owner->rc; // counted, like VM::start_coroutine
            }
        }
    }
    RunResult r = to_.run_fresh();
    migrated_ = memo_.size();
    for (auto& [old, v] : memo_) release(v);
    memo_.clear();
    return r;
}

Value Migrator::migrate_instance(InstanceObj* o) {
    const RecordRT* old_rt = o->type;
    const RecordRT* rt = old_rt->is_struct ? to_.find_struct(old_rt->name) : to_.find_class(old_rt->name);
    if (rt == nullptr) {
        warn(std::format("type '{}' no longer exists; its values became null", old_rt->name));
        memo_[o] = Value::null();
        return Value::null();
    }
    Value v;
    if (rt->is_struct) {
        // Defaults from the struct's own initializers, then the kept fields.
        auto ctor = to_.proto_by_key(rt->name + ".<default>");
        if (!ctor || !to_.call(*ctor, {}, &v).ok || !v.is_obj()) {
            v = Value::object(new_instance(rt, rt->field_names.size()));
            retain(v);
        }
    } else {
        // Manual objects stay manual, in the same kind of arena.
        ArenaKind arena = is_manual(o) ? arena_of(o) : ArenaKind::None;
        RunResult r = to_.instantiate(rt, &v, arena);
        if (!r.ok) {
            warn(std::format("the field initializers of '{}' panicked: {}", rt->name, r.panic->message));
            v = Value::object(new_instance(rt, rt->field_names.size()));
            retain(v);
        }
    }
    memo_[o] = v; // before the fields: cycles map back to it
    auto* n = static_cast<InstanceObj*>(v.o);
    if (o->native != nullptr) {
        n->native = o->native; // host data moves with the object
        o->native = nullptr;
    }
    for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
        int j = field_index(old_rt, rt->field_names[i]);
        if (j < 0) continue; // new field: keeps its initializer
        if (old_rt->field_types[static_cast<std::size_t>(j)] != rt->field_types[i]) {
            warn(std::format("field '{}.{}' changed type from '{}' to '{}'; it was reset", rt->name,
                             rt->field_names[i], old_rt->field_types[static_cast<std::size_t>(j)], rt->field_types[i]));
            continue;
        }
        assign_owned(n->fields[i], convert(o->fields[static_cast<std::size_t>(j)]));
    }
    retain(v);
    return v;
}

Value Migrator::migrate_enum(EnumObj* e) {
    // Result values share one run-time enum in every VM (pluma/value.hpp).
    const EnumRT* rt = e->type == result_rt() ? result_rt() : find_enum(to_, e->type->name, nullptr);
    const VariantRT& old_var = e->type->variants[e->variant];
    std::uint32_t variant = 0;
    bool found = false;
    if (rt != nullptr) {
        for (std::size_t i = 0; i < rt->variants.size(); ++i) {
            if (rt->variants[i].name == old_var.name) {
                variant = static_cast<std::uint32_t>(i);
                found = true;
            }
        }
    }
    if (!found) {
        warn(std::format("enum variant '{}.{}' no longer exists; the value became null", e->type->name, old_var.name));
        memo_[e] = Value::null();
        return Value::null();
    }
    const VariantRT& var = rt->variants[variant];
    EnumObj* n = new_enum(rt, variant, var.field_names.size());
    Value v = Value::object(n);
    retain(v);
    memo_[e] = v;
    for (std::size_t i = 0; i < var.field_names.size(); ++i) {
        for (std::size_t j = 0; j < old_var.field_names.size(); ++j) {
            if (old_var.field_names[j] == var.field_names[i]) assign_owned(n->fields[i], convert(e->fields[j]));
        }
    }
    retain(v);
    return v;
}

Value Migrator::convert(const Value& v) {
    switch (v.tag) {
        case Tag::Name: {
            Value out = v;
            out.i = to_.intern(from_.name_text(static_cast<std::uint32_t>(v.i)));
            return out;
        }
        case Tag::Enum: {
            const EnumRT* old_rt = from_.enums[v.aux].get();
            std::uint32_t index = 0;
            const EnumRT* rt = find_enum(to_, old_rt->name, &index);
            std::string_view vname;
            for (const VariantRT& var : old_rt->variants) {
                if (var.discriminant == v.i) vname = var.name;
            }
            Value out = v;
            if (rt == nullptr || rt->variants.empty()) return out;
            out.aux = index;
            out.i = rt->variants[0].discriminant;
            bool found = false;
            for (const VariantRT& var : rt->variants) {
                if (var.name == vname) {
                    out.i = var.discriminant;
                    found = true;
                }
            }
            if (!found) warn(std::format("enum variant '{}.{}' no longer exists; reset to '{}'", old_rt->name, vname,
                                         rt->variants[0].name));
            return out;
        }
        case Tag::Obj: break;
        default: return v; // numbers, bools, null, inline vectors
    }
    if (is_stale(v)) return Value::null(); // a freed manual object
    auto it = memo_.find(v.o);
    if (it != memo_.end()) {
        retain(it->second);
        return it->second;
    }
    switch (v.o->kind) {
        case ObjKind::Str:
        case ObjKind::Vec: retain(v); return v; // immutable, VM-independent
        case ObjKind::List: {
            const bool untraced = (v.o->flags & objflag::Untraced) != 0;
            Value out = is_manual(v.o) ? to_.new_manual_list(arena_of(v.o), untraced) : Value::object(new_list(untraced));
            auto* l = static_cast<ListObj*>(out.o);
            retain(out);
            memo_[v.o] = out;
            for (const Value& item : static_cast<ListObj*>(v.o)->items) {
                l->items.push_back(convert(item));
            }
            retain(out);
            return out;
        }
        case ObjKind::Instance: return migrate_instance(static_cast<InstanceObj*>(v.o));
        case ObjKind::Enum: return migrate_enum(static_cast<EnumObj*>(v.o));
        case ObjKind::Array: {
            const bool untraced = (v.o->flags & objflag::Untraced) != 0;
            Value out = Value::object(new_array(untraced));
            retain(out);
            memo_[v.o] = out;
            auto* l = static_cast<ListObj*>(out.o);
            for (const Value& item : static_cast<ListObj*>(v.o)->items) l->items.push_back(convert(item));
            retain(out);
            return out;
        }
        case ObjKind::Map:
        case ObjKind::Set: {
            // Entries keep their insertion order; class keys are the new
            // VM's objects, so the table is rebuilt by re-inserting.
            auto* src = static_cast<HashObj*>(v.o);
            HashObj* h = new_hash(v.o->kind, (v.o->flags & objflag::Untraced) != 0);
            Value out = Value::object(h);
            retain(out);
            memo_[v.o] = out;
            for (std::size_t i = 0; i < src->keys.size(); ++i) {
                if (src->keys[i].tag == Tag::Nil) continue;
                Value k = convert(src->keys[i]);
                Value val = v.o->kind == ObjKind::Map ? convert(src->vals[i]) : Value{};
                hash_put(h, k, v.o->kind == ObjKind::Map ? &val : nullptr);
                release(k);
                release(val);
            }
            retain(out);
            return out;
        }
        case ObjKind::Deque: {
            auto* src = static_cast<DequeObj*>(v.o);
            DequeObj* d = new_deque((v.o->flags & objflag::Untraced) != 0);
            Value out = Value::object(d);
            retain(out);
            memo_[v.o] = out;
            for (std::size_t i = 0; i < src->count; ++i) {
                Value x = convert(src->at(i));
                deque_push_back(d, x);
                release(x);
            }
            retain(out);
            return out;
        }
        case ObjKind::PQueue: {
            // The heap layout and tie-break counters carry over unchanged.
            auto* src = static_cast<PQueueObj*>(v.o);
            PQueueObj* q = new_pqueue();
            Value out = Value::object(q);
            retain(out);
            memo_[v.o] = out;
            for (const Value& x : src->items) q->items.push_back(convert(x));
            q->seq = src->seq;
            q->next_seq = src->next_seq;
            assign_owned(q->less, convert(src->less));
            retain(out);
            return out;
        }
        case ObjKind::StrBuilder: {
            StrBuilderObj* b = new_str_builder();
            b->s = static_cast<StrBuilderObj*>(v.o)->s;
            Value out = Value::object(b);
            retain(out);
            memo_[v.o] = out;
            retain(out);
            return out;
        }
        case ObjKind::Closure: {
            auto* c = static_cast<ClosureObj*>(v.o);
            auto fn = to_.proto_by_key(c->fn->key);
            if (!fn || to_.protos[*fn]->capture_count != c->captures.size()) {
                warn(std::format("function '{}' changed; a reference to it became null", c->fn->name));
                memo_[v.o] = Value::null();
                return Value::null();
            }
            ClosureObj* n = new_closure(to_.protos[*fn].get(), c->captures.size());
            Value out = Value::object(n);
            retain(out);
            memo_[v.o] = out;
            for (std::size_t i = 0; i < c->captures.size(); ++i) assign_owned(n->captures[i], convert(c->captures[i]));
            retain(out);
            return out;
        }
        case ObjKind::Cell: {
            CellObj* n = new_cell();
            Value out = Value::object(n);
            retain(out);
            memo_[v.o] = out;
            assign_owned(n->value, convert(static_cast<CellObj*>(v.o)->value));
            retain(out);
            return out;
        }
        case ObjKind::Signal: {
            SignalObj* n = new_signal();
            Value out = Value::object(n);
            retain(out);
            memo_[v.o] = out;
            for (const Value& h : static_cast<SignalObj*>(v.o)->handlers) {
                Value nh = convert(h);
                if (nh.is_obj()) {
                    n->handlers.push_back(nh);
                } else {
                    release(nh);
                }
            }
            retain(out);
            return out;
        }
        case ObjKind::Weak: {
            // A weak cell: the new VM's version of its target, held weakly.
            Object* t = static_cast<WeakObj*>(v.o)->target;
            if (t == nullptr) return Value::null();
            Value nt = convert(Value::object(t));
            Value out = weak_store(nt);
            release(nt);
            return out;
        }
        case ObjKind::Coro: {
            for (auto& [old_c, new_c] : coros_) {
                if (old_c == v.o) {
                    Value out = Value::object(new_c);
                    retain(out); // memo
                    memo_[v.o] = out;
                    retain(out);
                    return out;
                }
            }
            return Value::null(); // finished, or its function is gone
        }
    }
    return Value::null();
}

} // namespace pluma

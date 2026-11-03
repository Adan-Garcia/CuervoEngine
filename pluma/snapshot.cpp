#include "pluma/snapshot.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <unordered_set>

#include "pluma/bytecode.hpp"
#include "pluma/collections.hpp"
#include "pluma/heap_internal.hpp"
#include "pluma/vm.hpp"

namespace pluma {

// The VM's private state, for the snapshot code.
struct SnapshotAccess {
    static void write_vm(SnapshotWriter& w, const VM& vm);
    static bool read_vm(SnapshotReader& r, VM& vm);
    static void reset(VM& vm);
};

namespace {

constexpr std::uint32_t magic = 0x504e5350u; // "PSNP"
constexpr std::uint32_t version = 1;

// Value codes beyond the Tag values.
constexpr std::uint8_t code_obj = 0xF0;   // ref, aux, object id
constexpr std::uint8_t code_const = 0xF1; // ref, aux, proto, constant
constexpr std::uint8_t code_stale = 0xF2; // ref, aux, kind: a freed manual object
constexpr std::uint8_t code_null_obj = 0xF3;

constexpr std::uint32_t no_index = 0xffffffffu;
constexpr std::uint32_t struct_bit = 0x80000000u;
constexpr std::uint32_t result_index = 0xfffffffeu;

std::uint64_t fnv(std::uint64_t h, std::string_view s) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return (h ^ 0xff) * 1099511628211ull;
}

std::uint64_t code_fingerprint(const VM& vm) {
    std::uint64_t h = 1469598103934665603ull;
    for (const std::string& n : vm.global_names) h = fnv(h, n);
    for (const auto& c : vm.classes) h = fnv(h, c->name);
    for (const auto& c : vm.structs) h = fnv(h, c->name);
    for (const auto& c : vm.enums) h = fnv(h, c->name);
    h ^= (static_cast<std::uint64_t>(vm.protos.size()) << 40) ^ (static_cast<std::uint64_t>(vm.globals.size()) << 20);
    return h;
}


} // namespace

// ------------------------------------------------------------------ writer

SnapshotWriter::SnapshotWriter(const VM& vm) : vm_(vm), cur_(&stream_) {
    for (const auto& p : vm.protos) {
        for (std::uint32_t k = 0; k < p->consts.size(); ++k) {
            const Value& c = p->consts[k];
            if (c.tag == Tag::Obj && c.o != nullptr) consts_.emplace(c.o, std::make_pair(p->index, k));
        }
    }
    for (std::uint32_t i = 0; i < vm.classes.size(); ++i) record_ids_.emplace(vm.classes[i].get(), i);
    for (std::uint32_t i = 0; i < vm.structs.size(); ++i) record_ids_.emplace(vm.structs[i].get(), i | struct_bit);
    for (std::uint32_t i = 0; i < vm.enums.size(); ++i) enum_ids_.emplace(vm.enums[i].get(), i);
}

void SnapshotWriter::raw(const void* data, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(data);
    cur_->insert(cur_->end(), b, b + n);
}

std::uint32_t SnapshotWriter::id_of(const Object* o) {
    auto [it, fresh] = ids_.emplace(o, static_cast<std::uint32_t>(order_.size()));
    if (fresh) {
        order_.push_back(o);
        counted_.push_back(0);
    }
    return it->second;
}

void SnapshotWriter::object(const Object* o, bool owned) {
    Value v;
    if (o != nullptr) v = Value::object(const_cast<Object*>(o));
    value(v, owned);
}

void SnapshotWriter::value(const Value& v, bool owned) {
    switch (v.tag) {
        case Tag::Nil:
        case Tag::Null: u8(static_cast<std::uint8_t>(v.tag)); u8(v.ref); u32(v.aux); return;
        case Tag::Bool: u8(static_cast<std::uint8_t>(v.tag)); u8(v.ref); u32(v.aux); u8(v.b ? 1 : 0); return;
        case Tag::F32: u8(static_cast<std::uint8_t>(v.tag)); u8(v.ref); u32(v.aux); f32(v.f); return;
        case Tag::Vec:
            u8(static_cast<std::uint8_t>(v.tag));
            u8(v.ref);
            u32(v.aux);
            f32(v.v2[0]);
            f32(v.v2[1]);
            return;
        case Tag::Obj: {
            if (v.o == nullptr) {
                u8(code_null_obj);
                return;
            }
            if (v.ref != 0 && v.o->gen != v.aux) {
                u8(code_stale);
                u8(v.ref);
                u32(0);
                u8(static_cast<std::uint8_t>(v.o->kind));
                return;
            }
            if (auto it = consts_.find(v.o); it != consts_.end()) {
                u8(code_const);
                u8(v.ref);
                u32(v.aux);
                u32(it->second.first);
                u32(it->second.second);
                return;
            }
            const std::uint32_t id = id_of(v.o);
            if (owned) ++counted_[id];
            u8(code_obj);
            u8(v.ref);
            // A checked reference's generation depends on which freed slot the allocator picked,
            // which is process history, not game state: the reader gives every live manual object
            // generation 1 and the reference the same.
            u32(v.ref != 0 ? 0u : v.aux);
            u32(id);
            return;
        }
        default: u8(static_cast<std::uint8_t>(v.tag)); u8(v.ref); u32(v.aux); i64(v.i); return;
    }
}

void SnapshotWriter::record_ref(const RecordRT* rt) {
    if (rt == nullptr) {
        u32(no_index);
        return;
    }
    auto it = record_ids_.find(rt);
    if (it == record_ids_.end()) {
        fail(std::format("an object of type '{}' does not belong to this VM", rt->name));
        u32(no_index);
        return;
    }
    u32(it->second);
}

void SnapshotWriter::enum_ref(const EnumRT* rt) {
    if (rt == nullptr) {
        u32(no_index);
    } else if (rt == result_rt()) {
        u32(result_index);
    } else if (auto it = enum_ids_.find(rt); it != enum_ids_.end()) {
        u32(it->second);
    } else {
        fail(std::format("an enum '{}' does not belong to this VM", rt->name));
        u32(no_index);
    }
}

void SnapshotWriter::proto_ref(const Proto* p) {
    if (p == nullptr) {
        u32(no_index);
    } else if (p->index < vm_.protos.size() && vm_.protos[p->index].get() == p) {
        u32(p->index);
    } else {
        fail("a function does not belong to this VM");
        u32(no_index);
    }
}

// An unreachable suspect that belongs to another VM (the editor keeps several hosts in one
// process; the suspect buffer is shared): it mentions a type this VM does not have.
// Suspects held from outside this VM's roots. The shared suspect buffer also lists another
// VM's live objects (a plain list its node holds, say) that mention no type of its own, so
// foreign() cannot tell. Real garbage here is closed: every counted reference to each of its
// objects is an edge inside the garbage. An object with more references than that has a holder
// that is not ours, and so does everything it reaches.
void SnapshotWriter::find_alien() {
    std::unordered_set<const Object*> reach;
    std::vector<const Object*> stack;
    auto push = [&](std::unordered_set<const Object*>& into, const Object* o) {
        if (into.insert(o).second) stack.push_back(o);
    };
    for (const Object* o : order_) push(reach, o);
    while (!stack.empty()) {
        const Object* o = stack.back();
        stack.pop_back();
        if ((o->flags & objflag::Dead) != 0) continue;
        for_each_child(const_cast<Object*>(o), [&](const Value& c) {
            if (c.tag == Tag::Obj && c.o != nullptr) push(reach, c.o);
        });
    }
    std::unordered_set<const Object*> garbage;
    for (Object* s : g_heap.suspects) {
        if (!reach.contains(s)) push(garbage, s);
    }
    std::unordered_map<const Object*, std::uint32_t> internal;
    std::vector<const Object*> all;
    while (!stack.empty()) {
        const Object* o = stack.back();
        stack.pop_back();
        all.push_back(o);
        if ((o->flags & objflag::Dead) != 0) continue;
        for_each_child(const_cast<Object*>(o), [&](const Value& c) {
            if (c.tag != Tag::Obj || c.o == nullptr || reach.contains(c.o)) return;
            ++internal[c.o];
            push(garbage, c.o);
        });
    }
    std::vector<const Object*> held;
    for (const Object* o : all) {
        if ((o->flags & objflag::Dead) == 0 && !is_manual(o) && o->rc > internal[o]) held.push_back(o);
    }
    stack = held;
    for (const Object* o : held) alien_[o] = 1;
    while (!stack.empty()) {
        const Object* o = stack.back();
        stack.pop_back();
        if ((o->flags & objflag::Dead) != 0) continue;
        for_each_child(const_cast<Object*>(o), [&](const Value& c) {
            if (c.tag == Tag::Obj && c.o != nullptr && !reach.contains(c.o) && alien_.emplace(c.o, 1).second)
                stack.push_back(c.o);
        });
    }
}

bool SnapshotWriter::foreign(const Object* root) {
    if (alien_.contains(root)) return true;
    std::vector<Object*> stack{const_cast<Object*>(root)};
    std::unordered_set<const Object*> seen{root};
    while (!stack.empty()) {
        Object* o = stack.back();
        stack.pop_back();
        if ((o->flags & objflag::Dead) != 0) continue;
        if (o->kind == ObjKind::Instance && !record_ids_.contains(static_cast<InstanceObj*>(o)->type)) return true;
        if (o->kind == ObjKind::Enum) {
            const EnumRT* t = static_cast<EnumObj*>(o)->type;
            if (t != result_rt() && !enum_ids_.contains(t)) return true;
        }
        if (o->kind == ObjKind::Closure) {
            const Proto* p = static_cast<ClosureObj*>(o)->fn;
            if (p == nullptr || p->index >= vm_.protos.size() || vm_.protos[p->index].get() != p) return true;
        }
        for_each_child(o, [&](const Value& c) {
            if (c.tag == Tag::Obj && c.o != nullptr && seen.insert(c.o).second) stack.push_back(c.o);
        });
    }
    return false;
}

void SnapshotWriter::vm_state() { SnapshotAccess::write_vm(*this, vm_); }

void SnapshotWriter::write_object(std::uint32_t id) {
    const Object* o = order_[id];
    const bool dead = (o->flags & objflag::Dead) != 0;
    cur_ = &header_;
    u8(static_cast<std::uint8_t>(o->kind));
    u8(o->flags);
    const std::uint16_t canonical_gen = is_manual(o) ? (dead ? 2 : 1) : 0;
    u8(static_cast<std::uint8_t>(canonical_gen & 0xff));
    u8(static_cast<std::uint8_t>(canonical_gen >> 8));
    u32(o->rc);
    switch (o->kind) {
        case ObjKind::Instance: {
            const auto* i = static_cast<const InstanceObj*>(o);
            record_ref(i->type);
            u32(static_cast<std::uint32_t>(i->fields.size()));
            break;
        }
        case ObjKind::Enum: {
            const auto* e = static_cast<const EnumObj*>(o);
            enum_ref(e->type);
            u32(e->variant);
            u32(static_cast<std::uint32_t>(e->fields.size()));
            break;
        }
        case ObjKind::Closure: {
            const auto* c = static_cast<const ClosureObj*>(o);
            proto_ref(c->fn);
            u32(static_cast<std::uint32_t>(c->captures.size()));
            break;
        }
        default: break;
    }
    cur_ = &body_;
    const std::size_t start = body_.size();
    if (!dead) {
        auto vals = [&](const std::vector<Value>& v) {
            u32(static_cast<std::uint32_t>(v.size()));
            for (const Value& x : v) value(x);
        };
        switch (o->kind) {
            case ObjKind::Str: str(static_cast<const StrObj*>(o)->s); break;
            case ObjKind::StrBuilder: str(static_cast<const StrBuilderObj*>(o)->s); break;
            case ObjKind::List:
            case ObjKind::Array: vals(static_cast<const ListObj*>(o)->items); break;
            case ObjKind::Instance: {
                const auto* i = static_cast<const InstanceObj*>(o);
                for (const Value& f : i->fields) value(f);
                u8(i->native != nullptr ? 1 : 0);
                if (i->native != nullptr) {
                    if (!native_writer || !native_writer(*this, *i)) {
                        fail(std::format("an instance of '{}' holds native data that cannot be saved",
                                         i->type != nullptr ? i->type->name : std::string("?")));
                    }
                }
                break;
            }
            case ObjKind::Enum: {
                for (const Value& f : static_cast<const EnumObj*>(o)->fields) value(f);
                break;
            }
            case ObjKind::Closure: {
                for (const Value& f : static_cast<const ClosureObj*>(o)->captures) value(f);
                break;
            }
            case ObjKind::Cell: value(static_cast<const CellObj*>(o)->value); break;
            case ObjKind::Vec: {
                const auto& c = static_cast<const VecObj*>(o)->c;
                u32(static_cast<std::uint32_t>(c.size()));
                raw(c.data(), c.size() * sizeof(float));
                break;
            }
            case ObjKind::Signal: vals(static_cast<const SignalObj*>(o)->handlers); break;
            case ObjKind::Weak: object(static_cast<const WeakObj*>(o)->target, false); break;
            case ObjKind::Map:
            case ObjKind::Set: {
                const auto* h = static_cast<const HashObj*>(o);
                u32(static_cast<std::uint32_t>(h->table.size()));
                u32(h->live);
                u32(h->version);
                vals(h->keys);
                vals(h->vals);
                break;
            }
            case ObjKind::Deque: {
                const auto* d = static_cast<const DequeObj*>(o);
                u64(d->head);
                u64(d->count);
                u32(d->version);
                vals(d->buf);
                break;
            }
            case ObjKind::PQueue: {
                const auto* q = static_cast<const PQueueObj*>(o);
                vals(q->items);
                u32(static_cast<std::uint32_t>(q->seq.size()));
                for (std::uint64_t s : q->seq) u64(s);
                value(q->less);
                u64(q->next_seq);
                break;
            }
            case ObjKind::Coro: {
                const auto* c = static_cast<const CoroObj*>(o);
                u8(static_cast<std::uint8_t>(c->state));
                u8(static_cast<std::uint8_t>(c->wait));
                boolean(c->cancel_requested);
                boolean(c->signaled);
                u32(c->await_dst);
                u64(c->wake_tick);
                value(c->wait_value);
                value(c->resume_value);
                u32(static_cast<std::uint32_t>(c->frames.size()));
                for (const CoroObj::SavedFrame& f : c->frames) {
                    proto_ref(f.fn);
                    object(f.closure);
                    u32(f.base);
                    u32(f.pc);
                    u32(f.ret);
                }
                vals(c->regs);
                u32(c->start_proto);
                vals(c->start_args);
                object(c->owner);
                value(c->result);
                break;
            }
        }
    }
    (void)start;
    cur_ = &stream_;
}

bool SnapshotWriter::finish(std::vector<std::uint8_t>& image) {
    cur_ = &stream_;
    // The suspect buffer, in order. Those that no root reaches are the garbage the collector has
    // yet to free: they belong to the image too, unless another VM owns them.
    find_alien();
    std::vector<std::uint32_t> mine;
    for (Object* s : g_heap.suspects) {
        if (!ids_.contains(s) && foreign(s)) continue;
        mine.push_back(id_of(s));
    }
    u32(static_cast<std::uint32_t>(mine.size()));
    for (std::uint32_t id : mine) u32(id);

    for (std::uint32_t i = 0; i < order_.size(); ++i) write_object(i);
    cur_ = &stream_;

    // Every counted reference must have been reported by someone.
    int bad = 0;
    std::string detail;
    for (std::uint32_t i = 0; i < order_.size() && bad < 6; ++i) {
        const Object* o = order_[i];
        if ((o->flags & objflag::Dead) != 0) continue;
        const std::uint32_t expect = counted_[i] + (is_manual(o) ? manual_rc_bias : 0u);
        if (o->rc != expect) {
            ++bad;
            std::string what;
            if (o->kind == ObjKind::Instance && static_cast<const InstanceObj*>(o)->type != nullptr) {
                what = static_cast<const InstanceObj*>(o)->type->name;
            } else if (o->kind == ObjKind::Str) {
                what = "\"" + static_cast<const StrObj*>(o)->s.substr(0, 20) + "\"";
            }
            detail += std::format(" [{} #{} {}: rc {} but {} references accounted]", static_cast<int>(o->kind), i,
                                  what, o->rc, expect);
        }
    }
    if (bad > 0) fail("reference counts do not match what the snapshot reached (a holder is missing):" + detail);
    if (!ok()) return false;

    image.clear();
    auto put = [&](const void* d, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(d);
        image.insert(image.end(), b, b + n);
    };
    const std::uint32_t head[2] = {magic, version};
    const std::uint64_t sizes[5] = {stream_.size(), header_.size(), body_.size(), order_.size(),
                                    code_fingerprint(vm_)};
    put(head, sizeof head);
    put(sizes, sizeof sizes);
    put(stream_.data(), stream_.size());
    put(header_.data(), header_.size());
    put(body_.data(), body_.size());
    return true;
}

// ------------------------------------------------------------------ reader

void SnapshotReader::raw(void* out, std::size_t n) {
    if (cur_ == nullptr || static_cast<std::size_t>(cur_->end - cur_->p) < n) {
        fail("the snapshot is truncated");
        std::memset(out, 0, n);
        if (cur_ != nullptr) cur_->p = cur_->end;
        return;
    }
    std::memcpy(out, cur_->p, n);
    cur_->p += n;
}

std::string SnapshotReader::str() {
    const std::uint32_t n = u32();
    if (!ok() || static_cast<std::size_t>(cur_->end - cur_->p) < n) {
        fail("the snapshot is truncated");
        return {};
    }
    std::string s(reinterpret_cast<const char*>(cur_->p), n);
    cur_->p += n;
    return s;
}

Object* SnapshotReader::resolve(std::uint32_t id, bool owned) {
    if (id >= objs_.size()) {
        fail("the snapshot names an unknown object");
        return nullptr;
    }
    Object* o = objs_[id];
    if (owned) ++o->rc;
    return o;
}

const RecordRT* SnapshotReader::record_ref() {
    const std::uint32_t c = u32();
    if (c == no_index) return nullptr;
    const bool is_struct = (c & struct_bit) != 0;
    const std::uint32_t idx = c & ~struct_bit;
    const auto& list = is_struct ? vm_.structs : vm_.classes;
    if (idx >= list.size()) {
        fail("the snapshot names a class this program does not have");
        return nullptr;
    }
    return list[idx].get();
}

const EnumRT* SnapshotReader::enum_ref() {
    const std::uint32_t c = u32();
    if (c == no_index) return nullptr;
    if (c == result_index) return result_rt();
    if (c >= vm_.enums.size()) {
        fail("the snapshot names an enum this program does not have");
        return nullptr;
    }
    return vm_.enums[c].get();
}

const Proto* SnapshotReader::proto_ref() {
    const std::uint32_t c = u32();
    if (c == no_index) return nullptr;
    if (c >= vm_.protos.size()) {
        fail("the snapshot names a function this program does not have");
        return nullptr;
    }
    return vm_.protos[c].get();
}

Value SnapshotReader::value(bool owned) {
    const std::uint8_t code = u8();
    Value v;
    switch (code) {
        case static_cast<std::uint8_t>(Tag::Nil):
        case static_cast<std::uint8_t>(Tag::Null):
            v.tag = static_cast<Tag>(code);
            v.ref = u8();
            v.aux = u32();
            return v;
        case static_cast<std::uint8_t>(Tag::Bool):
            v.tag = Tag::Bool;
            v.ref = u8();
            v.aux = u32();
            v.b = u8() != 0;
            return v;
        case static_cast<std::uint8_t>(Tag::F32):
            v.tag = Tag::F32;
            v.ref = u8();
            v.aux = u32();
            v.f = f32();
            return v;
        case static_cast<std::uint8_t>(Tag::Vec):
            v.tag = Tag::Vec;
            v.ref = u8();
            v.aux = u32();
            v.v2[0] = f32();
            v.v2[1] = f32();
            return v;
        case code_null_obj: return Value::object(nullptr);
        case code_stale: {
            v.tag = Tag::Obj;
            v.ref = u8();
            v.aux = u32();
            const auto kind = static_cast<ObjKind>(u8());
            // A freed manual object: a dead slot nobody reuses, so the reference stays stale.
            Object* o = nullptr;
            if (kind == ObjKind::Instance) {
                o = new InstanceObj;
            } else {
                o = new ListObj;
            }
            o->kind = kind == ObjKind::Instance ? ObjKind::Instance : ObjKind::List;
            o->rc = manual_rc_bias;
            o->gen = 1; // the reference's generation is 0: stale
            o->flags = objflag::Manual | objflag::Untraced | objflag::Dead;
            g_heap.retired.push_back(o);
            v.o = o;
            return v;
        }
        case code_const: {
            v.tag = Tag::Obj;
            v.ref = u8();
            v.aux = u32();
            const std::uint32_t pi = u32();
            const std::uint32_t k = u32();
            if (pi >= vm_.protos.size() || k >= vm_.protos[pi]->consts.size()) {
                fail("the snapshot names a constant this program does not have");
                return Value{};
            }
            v.o = vm_.protos[pi]->consts[k].o;
            if (owned) ++v.o->rc;
            return v;
        }
        case code_obj: {
            v.tag = Tag::Obj;
            v.ref = u8();
            v.aux = u32();
            Object* o = resolve(u32(), owned);
            if (o == nullptr) return Value{};
            v.o = o;
            if (v.ref != 0) v.aux = o->gen;
            return v;
        }
        default:
            if (code >= static_cast<std::uint8_t>(Tag::I8) && code <= static_cast<std::uint8_t>(Tag::Enum)) {
                v.tag = static_cast<Tag>(code);
                v.ref = u8();
                v.aux = u32();
                v.i = i64();
                return v;
            }
            fail("the snapshot holds an unknown value");
            return Value{};
    }
}

Object* SnapshotReader::object(bool owned) {
    const Value v = value(owned);
    return v.tag == Tag::Obj ? v.o : nullptr;
}

SnapshotReader::SnapshotReader(VM& vm, std::span<const std::uint8_t> image,
                               std::function<void*(SnapshotReader&, InstanceObj&)> native_reader)
    : vm_(vm), native_reader_(std::move(native_reader)) {
    constexpr std::size_t head = 8 + 40;
    std::uint32_t hdr[2] = {};
    std::uint64_t sizes[5] = {};
    if (image.size() < head) {
        fail("the snapshot is truncated");
        return;
    }
    std::memcpy(hdr, image.data(), 8);
    std::memcpy(sizes, image.data() + 8, 40);
    if (hdr[0] != magic || hdr[1] != version) {
        fail("not a snapshot of this version");
        return;
    }
    if (head + sizes[0] + sizes[1] + sizes[2] != image.size()) {
        fail("the snapshot is truncated");
        return;
    }
    if (sizes[4] != code_fingerprint(vm)) {
        fail("the snapshot is from different code (the scripts changed since it was taken)");
        return;
    }
    const std::uint8_t* p = image.data() + head;
    stream_ = {p, p + sizes[0]};
    p += sizes[0];
    header_ = {p, p + sizes[1]};
    p += sizes[1];
    body_ = {p, p + sizes[2]};
    const auto count = static_cast<std::size_t>(sizes[3]);

    // Pass 1: the shells, so references can name any object.
    objs_.assign(count, nullptr);
    saved_rc_.assign(count, 0);
    cur_ = &header_;
    for (std::size_t id = 0; id < count && ok(); ++id) build_object(static_cast<std::uint32_t>(id));
    // Pass 2: their contents.
    cur_ = &body_;
    for (std::size_t id = 0; id < count && ok(); ++id) read_object(static_cast<std::uint32_t>(id));
    cur_ = &stream_;
}

void SnapshotReader::build_object(std::uint32_t id) {
    const auto kind = static_cast<ObjKind>(u8());
    const std::uint8_t flags = u8();
    const std::uint8_t g0 = u8();
    const std::uint8_t g1 = u8();
    const std::uint32_t rc = u32();
    const auto gen = static_cast<std::uint16_t>(g0 | (g1 << 8));
    saved_rc_[id] = rc;
    const bool dead = (flags & objflag::Dead) != 0;
    const bool manual = (flags & objflag::Manual) != 0;
    Object* o = nullptr;
    switch (kind) {
        case ObjKind::Str: o = new_str({}); break;
        case ObjKind::StrBuilder: o = new_str_builder(); break;
        case ObjKind::List: o = new_list(); break;
        case ObjKind::Array: o = new_array(); break;
        case ObjKind::Map:
        case ObjKind::Set: o = new_hash(kind); break;
        case ObjKind::Deque: o = new_deque(); break;
        case ObjKind::PQueue: o = new_pqueue(); break;
        case ObjKind::Cell: o = new_cell(); break;
        case ObjKind::Vec: {
            auto* v = new VecObj;
            v->kind = ObjKind::Vec;
            ++g_heap.live;
            o = v;
            break;
        }
        case ObjKind::Signal: o = new_signal(); break;
        case ObjKind::Coro: o = new_coro(); break;
        case ObjKind::Weak: {
            auto* w = new WeakObj;
            w->kind = ObjKind::Weak;
            ++g_heap.live;
            o = w;
            break;
        }
        case ObjKind::Instance: {
            const RecordRT* t = record_ref();
            const std::uint32_t n = u32();
            if (!ok()) return;
            if (manual) {
                auto* i = new InstanceObj;
                i->kind = ObjKind::Instance;
                i->type = t;
                i->fields.assign(n, Value{});
                if (!dead) {
                    ++g_heap.live;
                    ++g_heap.manual;
                }
                o = i;
            } else {
                o = new_instance(t, n);
            }
            break;
        }
        case ObjKind::Enum: {
            const EnumRT* t = enum_ref();
            const std::uint32_t variant = u32();
            const std::uint32_t n = u32();
            if (!ok()) return;
            auto* e = new_enum(t, variant, n);
            o = e;
            break;
        }
        case ObjKind::Closure: {
            const Proto* p = proto_ref();
            const std::uint32_t n = u32();
            if (!ok()) return;
            o = new_closure(p, n);
            break;
        }
        default: fail("the snapshot holds an unknown object kind"); return;
    }
    if (manual && kind == ObjKind::List) {
        if (dead) {
            --g_heap.live;
        } else {
            ++g_heap.manual;
        }
    }
    if (dead && !manual) --g_heap.live; // a shell awaiting deletion by the collector
    o->flags = flags;
    o->gen = gen;
    o->rc = manual ? manual_rc_bias : 0u;
    objs_[id] = o;
    if (dead && manual) {
        // A freed slot: reusable by the next manual allocation, like the pool it came from.
        if (gen == 0xffff) {
            g_heap.retired.push_back(o);
        } else if (kind == ObjKind::Instance) {
            g_heap.free_instances.push_back(static_cast<InstanceObj*>(o));
        } else {
            g_heap.free_lists.push_back(static_cast<ListObj*>(o));
        }
    }
}

void SnapshotReader::read_object(std::uint32_t id) {
    Object* o = objs_[id];
    if ((o->flags & objflag::Dead) != 0) return;
    auto vals = [&](std::vector<Value>& v) {
        const std::uint32_t n = u32();
        if (!ok() || n > static_cast<std::size_t>(body_.end - body_.p)) {
            fail("the snapshot is truncated");
            return;
        }
        v.resize(n);
        for (Value& x : v) x = value();
    };
    switch (o->kind) {
        case ObjKind::Str: static_cast<StrObj*>(o)->s = str(); break;
        case ObjKind::StrBuilder: static_cast<StrBuilderObj*>(o)->s = str(); break;
        case ObjKind::List:
        case ObjKind::Array: vals(static_cast<ListObj*>(o)->items); break;
        case ObjKind::Instance: {
            auto* i = static_cast<InstanceObj*>(o);
            for (Value& f : i->fields) f = value();
            if (u8() != 0) {
                if (!native_reader_) {
                    fail("the snapshot holds native data and no reader was given");
                    return;
                }
                i->native = native_reader_(*this, *i);
            }
            break;
        }
        case ObjKind::Enum:
            for (Value& f : static_cast<EnumObj*>(o)->fields) f = value();
            break;
        case ObjKind::Closure:
            for (Value& f : static_cast<ClosureObj*>(o)->captures) f = value();
            break;
        case ObjKind::Cell: static_cast<CellObj*>(o)->value = value(); break;
        case ObjKind::Vec: {
            auto& c = static_cast<VecObj*>(o)->c;
            const std::uint32_t n = u32();
            if (!ok() || static_cast<std::size_t>(n) * sizeof(float) > static_cast<std::size_t>(body_.end - body_.p)) {
                fail("the snapshot is truncated");
                return;
            }
            c.resize(n);
            raw(c.data(), n * sizeof(float));
            break;
        }
        case ObjKind::Signal: vals(static_cast<SignalObj*>(o)->handlers); break;
        case ObjKind::Weak: static_cast<WeakObj*>(o)->target = object(false); break;
        case ObjKind::Map:
        case ObjKind::Set: {
            auto* h = static_cast<HashObj*>(o);
            const std::uint32_t table = u32();
            h->live = u32();
            h->version = u32();
            vals(h->keys);
            vals(h->vals);
            h->table.assign(table, 0);
            break;
        }
        case ObjKind::Deque: {
            auto* d = static_cast<DequeObj*>(o);
            d->head = static_cast<std::size_t>(u64());
            d->count = static_cast<std::size_t>(u64());
            d->version = u32();
            vals(d->buf);
            break;
        }
        case ObjKind::PQueue: {
            auto* q = static_cast<PQueueObj*>(o);
            vals(q->items);
            const std::uint32_t n = u32();
            if (!ok() || n > static_cast<std::size_t>(body_.end - body_.p)) {
                fail("the snapshot is truncated");
                return;
            }
            q->seq.resize(n);
            for (std::uint64_t& s : q->seq) s = u64();
            q->less = value();
            q->next_seq = u64();
            break;
        }
        case ObjKind::Coro: {
            auto* c = static_cast<CoroObj*>(o);
            c->state = static_cast<CoroObj::State>(u8());
            c->wait = static_cast<CoroObj::Wait>(u8());
            c->cancel_requested = boolean();
            c->signaled = boolean();
            c->await_dst = static_cast<std::uint16_t>(u32());
            c->wake_tick = u64();
            c->wait_value = value();
            c->resume_value = value();
            const std::uint32_t nf = u32();
            if (!ok() || static_cast<std::ptrdiff_t>(nf) > body_.end - body_.p) {
                fail("the snapshot is truncated");
                return;
            }
            c->frames.resize(nf);
            for (CoroObj::SavedFrame& f : c->frames) {
                f.fn = proto_ref();
                f.closure = static_cast<const ClosureObj*>(object());
                f.base = u32();
                f.pc = u32();
                f.ret = u32();
            }
            vals(c->regs);
            c->start_proto = u32();
            vals(c->start_args);
            c->owner = object();
            c->result = value();
            break;
        }
    }
}

bool SnapshotReader::finish() {
    cur_ = &stream_;
    const std::uint32_t n = u32();
    suspects_.clear();
    for (std::uint32_t i = 0; i < n && ok(); ++i) suspects_.push_back(u32());
    if (!ok()) return false;

    // Hash tables: the probe layout depends on addresses, so it is built here, over the new objects.
    for (Object* o : objs_) {
        if ((o->kind != ObjKind::Map && o->kind != ObjKind::Set) || (o->flags & objflag::Dead) != 0) continue;
        auto* h = static_cast<HashObj*>(o);
        if (h->table.empty()) continue;
        const std::size_t mask = h->table.size() - 1;
        for (std::size_t i = 0; i < h->keys.size(); ++i) {
            if (h->keys[i].tag == Tag::Nil) continue;
            std::size_t s = key_hash(h->keys[i]) & mask;
            while (h->table[s] != 0) s = (s + 1) & mask;
            h->table[s] = static_cast<std::uint32_t>(i + 1);
        }
    }
    int bad = 0;
    for (std::size_t i = 0; i < objs_.size() && bad < 4; ++i) {
        if ((objs_[i]->flags & objflag::Dead) != 0) continue;
        if (objs_[i]->rc != saved_rc_[i]) {
            ++bad;
            fail(std::format("restore: object #{} (kind {}) has rc {} but the snapshot had {}", i,
                             static_cast<int>(objs_[i]->kind), objs_[i]->rc, saved_rc_[i]));
        }
    }
    if (!ok()) return false;
    for (std::uint32_t id : suspects_) {
        if (id < objs_.size()) g_heap.suspects.push_back(objs_[id]);
    }
    for (Object* o : objs_) {
        if (o->kind != ObjKind::Weak || (o->flags & objflag::Dead) != 0) continue;
        auto* w = static_cast<WeakObj*>(o);
        if (w->target != nullptr) g_heap.weak.emplace(w->target, w);
    }
    return true;
}

// ------------------------------------------------------------------ VM state

void SnapshotAccess::write_vm(SnapshotWriter& w, const VM& vm) {
    w.u64(vm.tick_);
    w.u64(vm.rng_state_);
    // The cycle collector's credit is reset to the budget at the start of every collect_cycles whenever it
    // is not in debt (min(credit + b, b)), so any non-negative value restores identically. What it holds
    // after a tick is the work last done, which depends on garbage made outside the tick (draw frames:
    // wall-clock paced), so only a debt is state (DESIGN.md 4.8, 4.1).
    w.i64(std::min<std::int64_t>(vm.gc_credit_, 0));
    w.boolean(vm.suspended_);
    if (!vm.frames_.empty() || !vm.running_.empty()) w.fail("a snapshot cannot be taken while a script is running");
    w.u32(static_cast<std::uint32_t>(vm.names_.size()));
    for (const std::string& n : vm.names_) w.str(n);
    std::size_t used = vm.stack_.size();
    while (used > 0 && vm.stack_[used - 1].tag == Tag::Nil) --used;
    w.u32(static_cast<std::uint32_t>(vm.stack_.size()));
    w.u32(static_cast<std::uint32_t>(used));
    for (std::size_t i = 0; i < used; ++i) w.value(vm.stack_[i]);
    w.u32(static_cast<std::uint32_t>(vm.globals.size()));
    for (const Value& g : vm.globals) w.value(g);
    w.u32(static_cast<std::uint32_t>(vm.coros_.size()));
    for (const CoroObj* c : vm.coros_) w.object(c);
    for (const VM::ManualList& l : vm.manual_) {
        // Entries of freed objects are skipped (the list compacts them away on its own schedule).
        std::uint32_t live = 0;
        for (const auto& e : l.items) live += e.first->gen == e.second ? 1u : 0u;
        w.u32(live);
        for (const auto& e : l.items) {
            if (e.first->gen == e.second) w.object(e.first, false);
        }
        w.u64(l.compact_at);
    }
}

bool SnapshotAccess::read_vm(SnapshotReader& r, VM& vm) {
    vm.tick_ = r.u64();
    vm.rng_state_ = r.u64();
    vm.gc_credit_ = r.i64();
    vm.suspended_ = r.boolean();
    const std::uint32_t nn = r.u32();
    for (std::uint32_t i = 0; i < nn && r.ok(); ++i) {
        std::string n = r.str();
        if (i < vm.names_.size()) {
            if (vm.names_[i] != n) {
                r.fail("the snapshot's names differ from the program's");
                return false;
            }
        } else {
            vm.name_ids_.emplace(n, i);
            vm.names_.push_back(std::move(n));
        }
    }
    while (vm.names_.size() > nn && r.ok()) {
        vm.name_ids_.erase(vm.names_.back());
        vm.names_.pop_back();
    }
    const std::uint32_t stack_size = r.u32();
    const std::uint32_t used = r.u32();
    if (!r.ok() || used > stack_size) {
        r.fail("the snapshot is damaged");
        return false;
    }
    if (vm.stack_.size() < stack_size) vm.stack_.resize(stack_size);
    for (std::uint32_t i = 0; i < used && r.ok(); ++i) vm.stack_[i] = r.value();
    const std::uint32_t ng = r.u32();
    if (ng != vm.globals.size()) {
        r.fail("the snapshot has a different number of globals");
        return false;
    }
    for (Value& g : vm.globals) g = r.value();
    const std::uint32_t nc = r.u32();
    vm.coros_.clear();
    for (std::uint32_t i = 0; i < nc && r.ok(); ++i) vm.coros_.push_back(static_cast<CoroObj*>(r.object()));
    for (VM::ManualList& l : vm.manual_) {
        const std::uint32_t n = r.u32();
        l.items.clear();
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            Object* o = r.object(false);
            if (o != nullptr) l.items.emplace_back(o, o->gen);
        }
        l.compact_at = static_cast<std::size_t>(r.u64());
    }
    return r.ok();
}

void SnapshotAccess::reset(VM& vm) {
    for (Value& v : vm.stack_) {
        Value old = v;
        v = Value{};
        release(old);
    }
    for (Value& v : vm.globals) {
        Value old = v;
        v = Value{};
        release(old);
    }
    std::vector<CoroObj*> coros;
    coros.swap(vm.coros_);
    for (CoroObj* c : coros) release(Value::object(c));
    vm.reset_arena(ArenaKind::Frame);
    vm.reset_arena(ArenaKind::Scene);
    vm.reset_arena(ArenaKind::Alloc);
    for (VM::ManualList& l : vm.manual_) l.items.clear();
    collect_all_cycles(false);
}

bool SnapshotReader::vm_state() { return SnapshotAccess::read_vm(*this, vm_); }

void snapshot_reset(VM& vm) { SnapshotAccess::reset(vm); }

} // namespace pluma

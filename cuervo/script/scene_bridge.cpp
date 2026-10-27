#include "cuervo/script/scene_bridge.hpp"

#include <format>
#include <optional>

#include "pluma/value.hpp"

namespace cuervo::script {

namespace {

using pluma::InstanceObj;
using pluma::RecordRT;
using pluma::Tag;
using pluma::Value;

int field_index(const RecordRT* rt, std::string_view name) {
    for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
        if (rt->field_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

const Value* field(const InstanceObj* o, std::string_view name) {
    int i = field_index(o->type, name);
    return i < 0 ? nullptr : &o->fields[static_cast<std::size_t>(i)];
}

bool is_instance(const Value& v) { return v.is_obj() && v.o->kind == pluma::ObjKind::Instance; }

// ------------------------------------------------------------ VM -> doc

scene::Value to_doc(pluma::VM& vm, const Value& v) {
    if (pluma::is_num_tag(v.tag)) {
        auto k = static_cast<scene::NumKind>(pluma::tag_num(v.tag));
        if (v.tag == Tag::F32) return scene::Value::number(v.f, k);
        if (v.tag == Tag::F64) return scene::Value::number(v.d, k);
        return scene::Value::integer(v.i, k);
    }
    switch (v.tag) {
        case Tag::Bool: return scene::Value::boolean(v.b);
        case Tag::Name: return scene::Value::name(vm.name_text(static_cast<std::uint32_t>(v.i)));
        case Tag::Enum: {
            const pluma::EnumRT* e = vm.enums[v.aux].get();
            for (const pluma::VariantRT& var : e->variants) {
                if (var.discriminant == v.i) return scene::Value::enum_value(e->name, var.name);
            }
            return scene::Value::enum_value(e->name, std::to_string(v.i));
        }
        case Tag::Vec: return scene::Value::vector({v.v2, v.v2 + v.aux});
        case Tag::Obj: break;
        default: return scene::Value::null();
    }
    switch (v.o->kind) {
        case pluma::ObjKind::Str: return scene::Value::str(static_cast<const pluma::StrObj*>(v.o)->s);
        case pluma::ObjKind::Vec: {
            const auto& c = static_cast<const pluma::VecObj*>(v.o)->c;
            return scene::Value::vector(c);
        }
        case pluma::ObjKind::List: {
            scene::Value out;
            out.kind = scene::ValueKind::List;
            for (const Value& item : static_cast<const pluma::ListObj*>(v.o)->items) out.items.push_back(to_doc(vm, item));
            return out;
        }
        case pluma::ObjKind::Instance: {
            const auto* o = static_cast<const InstanceObj*>(v.o);
            scene::Value out;
            out.kind = scene::ValueKind::Struct;
            out.type = o->type->name;
            for (std::size_t i = 0; i < o->fields.size(); ++i) {
                out.fields.push_back({o->type->field_names[i], {}, to_doc(vm, o->fields[i])});
            }
            return out;
        }
        default: return scene::Value::null(); // the checker rejects other exported types
    }
}

// ------------------------------------------------------------ doc -> VM

class Builder {
public:
    Builder(pluma::VM& vm, std::vector<std::string>& warnings) : vm_(vm), warnings_(warnings) {}

    // Converts `v` into a value shaped like `like` (the field's current value,
    // which has the declared type's representation). Returns an owned value,
    // or nullopt when the saved value doesn't fit.
    std::optional<Value> convert(const scene::Value& v, const Value& like, const std::string& where) {
        switch (v.kind) {
            case scene::ValueKind::Null: return std::nullopt;
            case scene::ValueKind::Bool:
                if (like.tag != Tag::Bool) return std::nullopt;
                return Value::boolean(v.b);
            case scene::ValueKind::Int: {
                Tag t = pluma::num_tag(static_cast<pluma::NumKind>(v.num));
                if (like.tag != t) return std::nullopt;
                return Value::integer(static_cast<pluma::NumKind>(v.num), v.i);
            }
            case scene::ValueKind::Float:
                if (v.num == scene::NumKind::F64) {
                    if (like.tag != Tag::F64) return std::nullopt;
                    return Value::f64(v.f);
                }
                if (like.tag != Tag::F32) return std::nullopt;
                return Value::f32(static_cast<float>(v.f));
            case scene::ValueKind::Name: {
                if (like.tag != Tag::Name) return std::nullopt;
                Value out;
                out.tag = Tag::Name;
                out.i = vm_.intern(v.text);
                return out;
            }
            case scene::ValueKind::Enum: {
                if (like.tag != Tag::Enum || vm_.enums[like.aux]->name != v.type) return std::nullopt;
                for (const pluma::VariantRT& var : vm_.enums[like.aux]->variants) {
                    if (var.name == v.text) {
                        Value out = like;
                        out.i = var.discriminant;
                        return out;
                    }
                }
                warnings_.push_back(std::format("{}: enum variant '{}.{}' no longer exists", where, v.type, v.text));
                return std::nullopt;
            }
            case scene::ValueKind::Vec: {
                if (!pluma::is_vec(like) || pluma::vec_size(like) != v.vec.size()) return std::nullopt;
                Value out = pluma::vec_value(v.vec.data(), static_cast<std::uint32_t>(v.vec.size()));
                pluma::retain(out);
                return out;
            }
            case scene::ValueKind::Str: {
                if (!like.is_obj() || like.o->kind != pluma::ObjKind::Str) return std::nullopt;
                Value out = pluma::str_value(v.text);
                pluma::retain(out);
                return out;
            }
            case scene::ValueKind::List: {
                if (!like.is_obj() || like.o->kind != pluma::ObjKind::List) return std::nullopt;
                // Elements take the shape of the list's current first element,
                // or (for an empty default) of the saved value itself.
                const auto* cur = static_cast<const pluma::ListObj*>(like.o);
                // Traced or not as the list it replaces (its element type's
                // acyclicity, pluma/heap.hpp): a traced list held by an
                // untraced object breaks the cycle collector's accounting.
                auto* l = pluma::new_list((cur->flags & pluma::objflag::Untraced) != 0);
                Value out = Value::object(l);
                pluma::retain(out);
                for (std::size_t i = 0; i < v.items.size(); ++i) {
                    std::optional<Value> item;
                    if (!cur->items.empty()) {
                        item = convert(v.items[i], cur->items[0], where);
                    } else {
                        item = fresh(v.items[i], where);
                    }
                    if (!item) {
                        pluma::release(out);
                        return std::nullopt;
                    }
                    l->items.push_back(*item);
                }
                return out;
            }
            case scene::ValueKind::Struct: {
                if (!is_instance(like)) return std::nullopt;
                const auto* lo = static_cast<const InstanceObj*>(like.o);
                if (!lo->type->is_struct || lo->type->name != v.type) return std::nullopt;
                return build_struct(v, lo->type, where);
            }
        }
        return std::nullopt;
    }

    // A value with no current value to imitate (list elements of an empty
    // default list): numbers, names, strings and vectors are self-describing;
    // structs and enums are looked up by name.
    std::optional<Value> fresh(const scene::Value& v, const std::string& where) {
        Value like;
        switch (v.kind) {
            case scene::ValueKind::Bool: like.tag = Tag::Bool; break;
            case scene::ValueKind::Int: like.tag = pluma::num_tag(static_cast<pluma::NumKind>(v.num)); break;
            case scene::ValueKind::Float: like.tag = v.num == scene::NumKind::F64 ? Tag::F64 : Tag::F32; break;
            case scene::ValueKind::Name: like.tag = Tag::Name; break;
            case scene::ValueKind::Vec: {
                Value out = pluma::vec_value(v.vec.data(), static_cast<std::uint32_t>(v.vec.size()));
                pluma::retain(out);
                return out;
            }
            case scene::ValueKind::Str: {
                Value out = pluma::str_value(v.text);
                pluma::retain(out);
                return out;
            }
            case scene::ValueKind::Enum: {
                for (std::size_t i = 0; i < vm_.enums.size(); ++i) {
                    if (vm_.enums[i]->name == v.type && vm_.enums[i]->plain) {
                        like.tag = Tag::Enum;
                        like.aux = static_cast<std::uint32_t>(i);
                    }
                }
                if (like.tag != Tag::Enum) return std::nullopt;
                break;
            }
            case scene::ValueKind::Struct: {
                const RecordRT* rt = vm_.find_struct(v.type);
                if (rt == nullptr) return std::nullopt;
                return build_struct(v, rt, where);
            }
            case scene::ValueKind::List: {
                auto* l = pluma::new_list();
                Value out = Value::object(l);
                pluma::retain(out);
                for (const scene::Value& item : v.items) {
                    auto x = fresh(item, where);
                    if (!x) {
                        pluma::release(out);
                        return std::nullopt;
                    }
                    l->items.push_back(*x);
                }
                return out;
            }
            case scene::ValueKind::Null: return std::nullopt;
        }
        return convert(v, like, where);
    }

    // Struct: defaults from its initializers, then the saved fields by name.
    std::optional<Value> build_struct(const scene::Value& v, const RecordRT* rt, const std::string& where) {
        Value out;
        if (auto ctor = vm_.proto_by_key(rt->name + ".<default>")) {
            out = vm_.invoke(*ctor, nullptr, 0);
        } else {
            out = Value::object(pluma::new_instance(rt, rt->field_names.size()));
            pluma::retain(out);
        }
        auto* o = static_cast<InstanceObj*>(out.o);
        for (const scene::Field& f : v.fields) {
            int i = field_index(rt, f.name);
            std::string at = std::format("{}.{}", where, f.name);
            if (i < 0) {
                warnings_.push_back(std::format("{}: field no longer exists; dropped", at));
                continue;
            }
            auto x = convert(f.value, o->fields[static_cast<std::size_t>(i)], at);
            if (!x) {
                warnings_.push_back(std::format("{}: saved value doesn't fit the field's type; kept the default", at));
                continue;
            }
            pluma::assign_owned(o->fields[static_cast<std::size_t>(i)], *x);
        }
        return out;
    }

private:
    pluma::VM& vm_;
    std::vector<std::string>& warnings_;
};

std::optional<std::uint32_t> zero_arg_init(pluma::VM& vm, const RecordRT* rt) {
    for (const RecordRT* r = rt; r != nullptr; r = r->base) {
        if (auto p = vm.proto_by_key(r->name + ".init")) {
            if (vm.protos[*p]->param_count == 1) return p;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

scene::SceneDoc save_tree(pluma::VM& vm, const Value& root, std::vector<std::string>& warnings) {
    scene::SceneDoc doc;
    auto visit = [&](auto& self, const Value& v, std::int32_t parent) -> void {
        if (!is_instance(v)) return;
        const auto* o = static_cast<const InstanceObj*>(v.o);
        if (const Value* t = field(o, "transient"); t != nullptr && t->tag == Tag::Bool && t->b && parent >= 0) return;
        scene::Node n;
        n.type = o->type->name;
        n.parent = parent;
        if (const Value* name = field(o, "name"); name != nullptr && name->is_obj()) {
            n.name = static_cast<const pluma::StrObj*>(name->o)->s;
        }
        const RecordRT* rt = o->type;
        for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
            if (i >= rt->field_exported.size() || !rt->field_exported[i] || rt->field_names[i] == "name") continue;
            n.fields.push_back({rt->field_names[i], rt->field_types[i], to_doc(vm, o->fields[i])});
        }
        auto index = static_cast<std::int32_t>(doc.nodes.size());
        doc.nodes.push_back(std::move(n));
        if (const Value* kids = field(o, "children"); kids != nullptr && kids->is_obj()) {
            // Copy: the list can't change during the walk, but be safe.
            std::vector<Value> items = static_cast<const pluma::ListObj*>(kids->o)->items;
            for (const Value& c : items) self(self, c, index);
        }
    };
    visit(visit, root, -1);
    if (doc.nodes.empty()) warnings.push_back("save: the root is not a node");
    return doc;
}

pluma::RunResult instantiate_tree(pluma::VM& vm, const scene::SceneDoc& doc, Value* out,
                                 std::vector<std::string>& warnings, std::vector<Value>* nodes) {
    std::vector<Value> made;
    pluma::RunResult r = vm.protect([&] {
        const RecordRT* node_rt = vm.find_class("Node");
        Builder b(vm, warnings);
        for (std::size_t i = 0; i < doc.nodes.size(); ++i) {
            const scene::Node& n = doc.nodes[i];
            const RecordRT* rt = vm.find_class(n.type);
            if (rt == nullptr || node_rt == nullptr || !rt->is_subclass_of(node_rt)) {
                vm.panic(std::format("scene node '{}': '{}' is not a class extending Node", n.name, n.type));
            }
            Value v;
            pluma::RunResult made_ok = vm.instantiate(rt, &v);
            if (!made_ok.ok) vm.panic(std::format("scene node '{}': {}", n.name, made_ok.panic->message));
            made.push_back(v);
            if (auto init = zero_arg_init(vm, rt)) pluma::release(vm.invoke(*init, &v, 1));
            auto* o = static_cast<InstanceObj*>(v.o);
            if (int ni = field_index(rt, "name"); ni >= 0) {
                Value s = pluma::str_value(n.name);
                pluma::assign(o->fields[static_cast<std::size_t>(ni)], s);
            }
            for (const scene::Field& f : n.fields) {
                std::string where = std::format("{} '{}'.{}", n.type, n.name, f.name);
                int fi = field_index(rt, f.name);
                if (fi < 0) {
                    warnings.push_back(std::format("{}: field no longer exists; dropped", where));
                    continue;
                }
                auto idx = static_cast<std::size_t>(fi);
                if (idx >= rt->field_exported.size() || !rt->field_exported[idx]) {
                    warnings.push_back(std::format("{}: field is no longer @export; dropped", where));
                    continue;
                }
                if (rt->field_types[idx] != f.type) {
                    warnings.push_back(std::format("{}: type changed from '{}' to '{}'; kept the default", where, f.type,
                                                   rt->field_types[idx]));
                    continue;
                }
                auto x = b.convert(f.value, o->fields[idx], where);
                if (!x) {
                    warnings.push_back(std::format("{}: saved value doesn't fit '{}'; kept the default", where,
                                                   rt->field_types[idx]));
                    continue;
                }
                pluma::assign_owned(o->fields[idx], *x);
            }
            if (n.parent >= 0) {
                const Value& parent = made[static_cast<std::size_t>(n.parent)];
                auto it = static_cast<const InstanceObj*>(parent.o)->type->methods.find("add_child");
                if (it == static_cast<const InstanceObj*>(parent.o)->type->methods.end()) vm.panic("Node.add_child missing");
                const Value args[2] = {parent, v};
                pluma::release(vm.invoke(it->second, args, 2));
            }
        }
    });
    if (r.ok && !made.empty()) {
        *out = made[0];
        pluma::retain(*out);
    } else {
        *out = Value::null();
    }
    if (nodes != nullptr && r.ok) {
        for (const Value& v : made) {
            pluma::retain(v);
            nodes->push_back(v);
        }
    }
    for (const Value& v : made) pluma::release(v);
    return r;
}

} // namespace cuervo::script

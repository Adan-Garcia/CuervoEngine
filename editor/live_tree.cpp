#include "editor/live_tree.hpp"

#include "pluma/bytecode.hpp"

namespace cuervo::editor {

namespace {

const pluma::InstanceObj* instance(const pluma::Value& v) {
    if (!v.is_obj() || v.o->kind != pluma::ObjKind::Instance) return nullptr;
    return static_cast<const pluma::InstanceObj*>(v.o);
}

const pluma::Value* field(const pluma::Value& v, std::string_view name) {
    const pluma::InstanceObj* o = instance(v);
    if (o == nullptr) return nullptr;
    for (std::size_t i = 0; i < o->type->field_names.size() && i < o->fields.size(); ++i) {
        if (o->type->field_names[i] == name) return &o->fields[i];
    }
    return nullptr;
}

} // namespace

std::vector<pluma::Value> live_children(const pluma::Value& node) {
    const pluma::Value* c = field(node, "children");
    if (c == nullptr || !c->is_obj() || c->o->kind != pluma::ObjKind::List) return {};
    return static_cast<const pluma::ListObj*>(c->o)->items;
}

std::string live_class(const pluma::Value& node) {
    const pluma::InstanceObj* o = instance(node);
    return o != nullptr ? o->type->name : std::string("?");
}

std::string live_name(const pluma::Value& node) {
    const pluma::Value* n = field(node, "name");
    if (n == nullptr || !n->is_obj() || n->o->kind != pluma::ObjKind::Str) return {};
    return static_cast<const pluma::StrObj*>(n->o)->s;
}

std::vector<LiveField> live_fields(pluma::VM& vm, const pluma::Value& node, std::size_t max_length) {
    std::vector<LiveField> out;
    const pluma::InstanceObj* o = instance(node);
    if (o == nullptr) return out;
    for (std::size_t i = 0; i < o->type->field_names.size() && i < o->fields.size(); ++i) {
        const std::string& name = o->type->field_names[i];
        if (name == "parent" || name == "children") continue;
        const std::string type = i < o->type->field_types.size() ? o->type->field_types[i] : std::string();
        out.push_back({name, type, vm.describe(o->fields[i], max_length)});
    }
    return out;
}

std::size_t live_count(const pluma::Value& root) {
    if (instance(root) == nullptr) return 0;
    std::size_t n = 1;
    for (const pluma::Value& c : live_children(root)) n += live_count(c);
    return n;
}

} // namespace cuervo::editor

#include "editor/class_registry.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "cuervo/script/scene_bridge.hpp"
#include "pluma/vm.hpp"

namespace cuervo::editor {

const FieldSpec* ClassSpec::field(std::string_view field_name) const {
    for (const FieldSpec& f : fields) {
        if (f.name == field_name) return &f;
    }
    return nullptr;
}

ClassRegistry ClassRegistry::from_vm(pluma::VM& vm) {
    ClassRegistry reg;
    const pluma::RecordRT* node_rt = vm.find_class("Node");
    if (node_rt == nullptr) return reg;
    for (const auto& cls : vm.classes) {
        const pluma::RecordRT* rt = cls.get();
        if (rt == nullptr || !rt->is_subclass_of(node_rt)) continue;
        ClassSpec spec;
        spec.name = rt->name;
        spec.base = rt->base != nullptr ? rt->base->name : std::string();
        for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
            if (i >= rt->field_exported.size() || !rt->field_exported[i] || rt->field_names[i] == "name") continue;
            spec.fields.push_back({rt->field_names[i], rt->field_types[i], scene::Value::null()});
        }
        // Defaults: build the node exactly as a scene file without fields
        // would (scene_bridge), then read it back the way saving does.
        scene::SceneDoc probe;
        probe.nodes.push_back({spec.name, "", -1, {}});
        std::vector<std::string> warnings;
        pluma::Value root;
        pluma::RunResult made = script::instantiate_tree(vm, probe, &root, warnings);
        if (made.ok && root.is_obj()) {
            scene::SceneDoc saved = script::save_tree(vm, root, warnings);
            if (!saved.nodes.empty()) {
                for (const scene::Field& f : saved.nodes[0].fields) {
                    for (FieldSpec& fs : spec.fields) {
                        if (fs.name == f.name) fs.default_value = f.value;
                    }
                }
            }
        } else if (made.panic) {
            spec.default_error = made.panic->message;
        }
        pluma::release(root);
        reg.classes_.push_back(std::move(spec));
    }
    std::sort(reg.classes_.begin(), reg.classes_.end(),
              [](const ClassSpec& a, const ClassSpec& b) { return a.name < b.name; });
    for (const auto& e : vm.enums) {
        if (!e || !e->plain) continue;
        EnumSpec spec;
        spec.name = e->name;
        for (const pluma::VariantRT& v : e->variants) spec.variants.push_back(v.name);
        reg.enums_.push_back(std::move(spec));
    }
    std::sort(reg.enums_.begin(), reg.enums_.end(), [](const EnumSpec& a, const EnumSpec& b) { return a.name < b.name; });
    return reg;
}

const ClassSpec* ClassRegistry::find(std::string_view name) const {
    auto it = std::lower_bound(classes_.begin(), classes_.end(), name,
                               [](const ClassSpec& c, std::string_view n) { return c.name < n; });
    return it != classes_.end() && it->name == name ? &*it : nullptr;
}

const EnumSpec* ClassRegistry::find_enum(std::string_view name) const {
    for (const EnumSpec& e : enums_) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

bool ClassRegistry::is_a(std::string_view name, std::string_view base) const {
    for (const ClassSpec* c = find(name); c != nullptr; c = find(c->base)) {
        if (c->name == base) return true;
    }
    return false;
}

namespace {

bool parse_hex_color(std::string_view s, float out[4]) {
    if (!s.empty() && s.front() == '#') s.remove_prefix(1);
    if (s.size() != 6 && s.size() != 8) return false;
    out[3] = 1.0f;
    for (std::size_t i = 0; i < s.size() / 2; ++i) {
        int v = 0;
        for (std::size_t k = 0; k < 2; ++k) {
            const char c = s[i * 2 + k];
            const int d = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        out[i] = static_cast<float>(v) / 255.0f;
    }
    return true;
}

} // namespace

bool coerce_value(scene::Value& v, const scene::Value& like, const ClassRegistry* registry, std::string& error) {
    using scene::ValueKind;
    const auto mismatch = [&] {
        error = std::format("'{}' doesn't fit a value like '{}'", scene::format_value(v), scene::format_value(like));
        return false;
    };
    switch (like.kind) {
        case ValueKind::Null: return true; // unknown shape: take it as written
        case ValueKind::Bool: return v.kind == ValueKind::Bool || mismatch();
        case ValueKind::Float:
            if (v.kind == ValueKind::Int) {
                v = scene::Value::number(static_cast<double>(v.i), like.num);
            } else if (v.kind != ValueKind::Float) {
                return mismatch();
            }
            v.num = like.num;
            if (like.num == scene::NumKind::F32) v.f = static_cast<double>(static_cast<float>(v.f));
            return true;
        case ValueKind::Int:
            if (v.kind == ValueKind::Float && std::trunc(v.f) == v.f && std::abs(v.f) < 9.0e15) {
                v = scene::Value::integer(static_cast<std::int64_t>(v.f), like.num);
            }
            if (v.kind != ValueKind::Int) return mismatch();
            v.num = like.num;
            return true;
        case ValueKind::Str:
            if (v.kind == ValueKind::Name) v = scene::Value::str(v.text);
            return v.kind == ValueKind::Str || mismatch();
        case ValueKind::Name:
            if (v.kind == ValueKind::Str) v = scene::Value::name(v.text);
            return v.kind == ValueKind::Name || mismatch();
        case ValueKind::Vec:
            if (v.kind != ValueKind::Vec || v.vec.size() != like.vec.size()) {
                error = std::format("expected a vec{} like {}", like.vec.size(), scene::format_value(like));
                return false;
            }
            return true;
        case ValueKind::Enum: {
            if (v.kind == ValueKind::Str || v.kind == ValueKind::Name) v = scene::Value::enum_value(like.type, v.text);
            if (v.kind != ValueKind::Enum || v.type != like.type) return mismatch();
            if (const EnumSpec* e = registry != nullptr ? registry->find_enum(like.type) : nullptr) {
                if (std::find(e->variants.begin(), e->variants.end(), v.text) == e->variants.end()) {
                    error = std::format("'{}' has no variant '{}'", like.type, v.text);
                    return false;
                }
            }
            return true;
        }
        case ValueKind::Struct: {
            if ((v.kind == ValueKind::Str || v.kind == ValueKind::Name) && like.type == "Color" &&
                like.fields.size() == 4) {
                float c[4];
                if (!parse_hex_color(v.text, c)) {
                    error = std::format("'{}' is not a hex color (rrggbb or rrggbbaa)", v.text);
                    return false;
                }
                scene::Value out = like;
                for (std::size_t i = 0; i < 4; ++i) out.fields[i].value = scene::Value::number(c[i], scene::NumKind::F32);
                v = std::move(out);
                return true;
            }
            if (v.kind != ValueKind::Struct || v.type != like.type) return mismatch();
            scene::Value out = like;
            for (scene::Field& f : v.fields) {
                auto it = std::find_if(out.fields.begin(), out.fields.end(),
                                       [&](const scene::Field& o) { return o.name == f.name; });
                if (it == out.fields.end()) {
                    error = std::format("'{}' has no field '{}'", like.type, f.name);
                    return false;
                }
                if (!coerce_value(f.value, it->value, registry, error)) return false;
                it->value = std::move(f.value);
            }
            v = std::move(out);
            return true;
        }
        case ValueKind::List:
            if (v.kind != ValueKind::List) return mismatch();
            if (!like.items.empty()) {
                for (scene::Value& item : v.items) {
                    if (!coerce_value(item, like.items[0], registry, error)) return false;
                }
            }
            return true;
    }
    return mismatch();
}

} // namespace cuervo::editor

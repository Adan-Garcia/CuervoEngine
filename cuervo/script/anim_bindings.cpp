// ScriptHost natives for the animation system (DESIGN.md 4.17 "Clarified
// (Milestone 9.4)", prelude animation.plm): reading and writing an `@export`
// field of a node by name with the value packed in a vec4.
//
// This is the one piece of reflection the animation tracks need; everything
// else (tracks, interpolation, blending, the state machine) is Pluma code.
// Writing folds the written bits into a digest that joins the state hash, so
// a replay notices animation drift even when nothing else reads the values.

#include <bit>
#include <cmath>
#include <format>

#include "cuervo/script/script_host.hpp"

namespace cuervo::script {

namespace {

namespace nv = pluma::native;
using pluma::InstanceObj;
using pluma::NativeCall;
using pluma::Tag;
using pluma::Value;

// Property kinds, mirrored by `PropKind` in animation.plm.
enum Kind : int {
    Missing = 0,
    Float = 1,
    Int = 2,
    Bool = 3,
    Vec2 = 4,
    Vec3 = 5,
    Vec4 = 6,
    ColorK = 7,
    EnumK = 8,
    NameK = 9,
    Other = 10
};

int find_field(const pluma::RecordRT* rt, std::string_view name) {
    for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
        if (rt->field_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

// The exported field `name` of the node, or nullptr.
Value* exported_field(InstanceObj* o, std::string_view name) {
    if (o == nullptr) return nullptr;
    int i = find_field(o->type, name);
    if (i < 0) return nullptr;
    auto idx = static_cast<std::size_t>(i);
    if (idx >= o->type->field_exported.size() || !o->type->field_exported[idx]) return nullptr;
    return &o->fields[idx];
}

bool is_color(const Value& v) {
    if (!v.is_obj() || v.o->kind != pluma::ObjKind::Instance) return false;
    const auto* o = static_cast<const InstanceObj*>(v.o);
    return o->type->is_struct && o->type->name == "Color" && o->fields.size() == 4;
}

Kind kind_of(const Value& v) {
    switch (v.tag) {
        case Tag::F32:
        case Tag::F64: return Float;
        case Tag::Bool: return Bool;
        case Tag::Name: return NameK;
        case Tag::Enum: return EnumK;
        case Tag::Vec:
        case Tag::Obj: break;
        default: return pluma::is_num_tag(v.tag) ? Int : Other;
    }
    if (pluma::is_vec(v)) {
        switch (pluma::vec_size(v)) {
            case 2: return Vec2;
            case 3: return Vec3;
            case 4: return Vec4;
            default: return Other;
        }
    }
    return is_color(v) ? ColorK : Other;
}

float as_float(const Value& v) { return v.tag == Tag::F64 ? static_cast<float>(v.d) : v.f; }

// Deterministic round-half-up (no libm).
std::int64_t round_to_int(float x) { return static_cast<std::int64_t>(std::floor(x + 0.5f)); }

} // namespace

void ScriptHost::bind_animation() {
    pluma::Bindings& b = bindings_;
    auto host = this;

    b.add_native("Anim.kind", [](NativeCall& c) {
        const Value* f = exported_field(nv::object(c, 0), nv::str(c, 1));
        return Value::integer(pluma::NumKind::I32, f == nullptr ? Missing : kind_of(*f));
    });

    b.add_native("Anim.read", [](NativeCall& c) {
        float out[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const Value* f = exported_field(nv::object(c, 0), nv::str(c, 1));
        if (f != nullptr) {
            switch (kind_of(*f)) {
                case Float: out[0] = as_float(*f); break;
                case Int: out[0] = static_cast<float>(f->i); break;
                case Bool: out[0] = f->b ? 1.0f : 0.0f; break;
                case EnumK: out[0] = static_cast<float>(f->i); break;
                case Vec2:
                case Vec3:
                case Vec4:
                    for (std::uint32_t i = 0; i < pluma::vec_size(*f); ++i) out[i] = pluma::vec_data(*f)[i];
                    break;
                case ColorK: {
                    const auto* o = static_cast<const InstanceObj*>(f->o);
                    for (std::size_t i = 0; i < 4; ++i) out[i] = as_float(o->fields[i]);
                    break;
                }
                default: break;
            }
        }
        return nv::owned(pluma::vec_value(out, 4));
    });

    // Anim.write(node, field, value: vec4, text: str) -> bool. The field's own
    // type decides how the packed value is read; `text` is the name of a
    // `name` field. False when the field is missing, not exported or of a type
    // tracks can't drive.
    b.add_native("Anim.write", [host](NativeCall& c) {
        InstanceObj* node = nv::object(c, 0);
        Value* f = exported_field(node, nv::str(c, 1));
        if (f == nullptr) return Value::boolean(false);
        const float* v = nv::vec(c, 2);
        const Kind k = kind_of(*f);
        switch (k) {
            case Float:
                if (f->tag == Tag::F64) f->d = static_cast<double>(v[0]);
                else f->f = v[0];
                break;
            case Int: f->i = round_to_int(v[0]); break;
            case Bool: f->b = v[0] >= 0.5f; break;
            case EnumK: f->i = round_to_int(v[0]); break;
            case NameK: f->i = static_cast<std::int64_t>(c.vm.intern(nv::str(c, 3))); break;
            case Vec2:
            case Vec3:
            case Vec4: {
                float d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (std::uint32_t i = 0; i < pluma::vec_size(*f); ++i) d[i] = v[i];
                Value packed = pluma::vec_value(d, pluma::vec_size(*f));
                pluma::assign(*f, packed);
                break;
            }
            case ColorK: {
                auto* o = static_cast<InstanceObj*>(f->o);
                for (std::size_t i = 0; i < 4; ++i) {
                    if (o->fields[i].tag == Tag::F64) o->fields[i].d = static_cast<double>(v[i]);
                    else o->fields[i].f = v[i];
                }
                break;
            }
            default: return Value::boolean(false);
        }
        // Order-dependent FNV-style fold of what was written.
        auto fold = [&](std::uint64_t x) { host->anim_digest_ = (host->anim_digest_ ^ x) * 0x100000001b3ULL; };
        fold(static_cast<std::uint64_t>(k) + 1);
        if (k == NameK) {
            fold(static_cast<std::uint64_t>(f->i));
        } else {
            for (int i = 0; i < 4; ++i) fold(std::bit_cast<std::uint32_t>(v[i]));
        }
        return Value::boolean(true);
    });

    // Anim.call(node, method): calls a method with no parameters.
    b.add_native("Anim.call", [](NativeCall& c) {
        if (!c.args[0].is_obj()) return Value::boolean(false);
        Value out;
        pluma::RunResult r = c.vm.call_method(c.args[0], nv::str(c, 1), {}, &out);
        if (!r.ok) c.vm.panic(std::format("animation method track '{}': {}", nv::str(c, 1), r.panic->message));
        pluma::release(out);
        return Value::boolean(true);
    });
}

} // namespace cuervo::script

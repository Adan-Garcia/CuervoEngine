#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cuervo/scene/scene_file.hpp"

namespace pluma {
class VM;
}

// What the editor knows about node classes: every compiled class extending
// Node (engine prelude and project scripts alike) with its `@export` fields
// (LANGUAGE.md 9.13), their declared types and default values, plus the
// plain enums and structs those fields use. This is the same metadata the
// scene system saves and loads by (cuervo/script/scene_bridge.hpp): a
// field's default is what save_tree records for a freshly built node, so the
// inspector shows exactly what a file without that field would load.
namespace cuervo::editor {

struct FieldSpec {
    std::string name;
    std::string type;          // declared type name, as stored in scene files
    scene::Value default_value; // Null if the class couldn't be instantiated
};

struct ClassSpec {
    std::string name;
    std::string base; // empty for Node
    std::vector<FieldSpec> fields; // exported fields except `name`, in slot order
    std::string default_error;     // why defaults are missing (init() panicked...)

    const FieldSpec* field(std::string_view field_name) const;
};

struct EnumSpec {
    std::string name;
    std::vector<std::string> variants;
};

class ClassRegistry {
public:
    // Reads the compiled program of `vm`. Instantiates each node class once
    // (field initializers and a zero-argument init(), never ready()) to learn
    // its defaults; nothing is added to any scene.
    static ClassRegistry from_vm(pluma::VM& vm);

    const ClassSpec* find(std::string_view name) const;
    const EnumSpec* find_enum(std::string_view name) const;
    const std::vector<ClassSpec>& classes() const { return classes_; } // sorted by name
    const std::vector<EnumSpec>& enums() const { return enums_; }
    // `name` is `base` or derives from it.
    bool is_a(std::string_view name, std::string_view base) const;

private:
    std::vector<ClassSpec> classes_;
    std::vector<EnumSpec> enums_;
};

// Makes a parsed or edited value fit a field whose current/default value is
// `like` (the declared type's shape): int literals become floats for float
// fields, number kinds follow the field (`3` -> u8), a "rrggbb" string or
// #name becomes a Color, a bare variant name becomes the field's enum, and
// struct fields missing from the input keep `like`'s. Returns false (and
// sets `error`) when the value can't fit.
bool coerce_value(scene::Value& v, const scene::Value& like, const ClassRegistry* registry, std::string& error);

} // namespace cuervo::editor

#include "pluma/sema.hpp"

#include <tuple>

namespace pluma {

std::string_view to_string(NumKind k) {
    switch (k) {
        case NumKind::I8: return "i8";
        case NumKind::I16: return "i16";
        case NumKind::I32: return "int";
        case NumKind::I64: return "i64";
        case NumKind::U8: return "u8";
        case NumKind::U16: return "u16";
        case NumKind::U32: return "u32";
        case NumKind::U64: return "u64";
        case NumKind::F32: return "float";
        case NumKind::F64: return "f64";
    }
    return "?";
}

bool TypeTable::Key::operator<(const Key& o) const {
    return std::tie(tag, num, dim, decl, args, ret) < std::tie(o.tag, o.num, o.dim, o.decl, o.args, o.ret);
}

TypeTable::TypeTable() {
    Type t;
    t.tag = TypeTag::Error;
    error_ = intern(t);
    t.tag = TypeTag::Void;
    void_t_ = intern(t);
    t.tag = TypeTag::Null;
    null_ = intern(t);
    t.tag = TypeTag::Bool;
    bool_t_ = intern(t);
    t.tag = TypeTag::Str;
    str_ = intern(t);
    t.tag = TypeTag::Name;
    name_ = intern(t);
    for (int i = 0; i < 10; ++i) {
        Type n;
        n.tag = TypeTag::Num;
        n.num = static_cast<NumKind>(i);
        nums_[i] = intern(n);
    }
}

const Type* TypeTable::intern(Type t) {
    Key key{t.tag, t.tag == TypeTag::Num ? t.num : NumKind::I32, t.dim, t.decl, t.args, t.ret};
    auto it = types_.find(key);
    if (it != types_.end()) return it->second.get();
    if (t.tag != TypeTag::Num) t.num = NumKind::I32;
    auto owned = std::make_unique<Type>(std::move(t));
    const Type* out = owned.get();
    types_.emplace(std::move(key), std::move(owned));
    return out;
}

const Type* TypeTable::class_(ClassInfo* c, std::vector<const Type*> args) {
    Type t;
    t.tag = TypeTag::Class;
    t.decl = c;
    t.args = std::move(args);
    return intern(std::move(t));
}

const Type* TypeTable::struct_(StructInfo* s, std::vector<const Type*> args) {
    Type t;
    t.tag = TypeTag::Struct;
    t.decl = s;
    t.args = std::move(args);
    return intern(std::move(t));
}

const Type* TypeTable::enum_(EnumInfo* e) {
    Type t;
    t.tag = TypeTag::Enum;
    t.decl = e;
    return intern(std::move(t));
}

const Type* TypeTable::trait(TraitInfo* tr) {
    Type t;
    t.tag = TypeTag::Trait;
    t.decl = tr;
    return intern(std::move(t));
}

const Type* TypeTable::list(const Type* element) {
    Type t;
    t.tag = TypeTag::List;
    t.args = {element};
    return intern(std::move(t));
}

const Type* TypeTable::fn(std::vector<const Type*> params, const Type* ret) {
    Type t;
    t.tag = TypeTag::Fn;
    t.args = std::move(params);
    t.ret = ret;
    return intern(std::move(t));
}

const Type* TypeTable::nullable(const Type* inner) {
    if (inner->is_nullable() || inner->is_error() || inner->is_void()) return inner;
    Type t;
    t.tag = TypeTag::Nullable;
    t.args = {inner};
    return intern(std::move(t));
}

const Type* TypeTable::param(GenericParamInfo* p) {
    Type t;
    t.tag = TypeTag::Param;
    t.decl = p;
    return intern(std::move(t));
}

const Type* TypeTable::vec(std::uint32_t dim) {
    Type t;
    t.tag = TypeTag::Vec;
    t.num = NumKind::F32;
    t.dim = dim;
    return intern(std::move(t));
}

const Type* TypeTable::task() {
    Type t;
    t.tag = TypeTag::Task;
    return intern(std::move(t));
}

const Type* TypeTable::signal(std::vector<const Type*> params) {
    Type t;
    t.tag = TypeTag::Signal;
    t.args = std::move(params);
    return intern(std::move(t));
}


const Type* TypeTable::map(const Type* key, const Type* value) {
    Type t;
    t.tag = TypeTag::Map;
    t.args = {key, value};
    return intern(std::move(t));
}

const Type* TypeTable::set(const Type* element) {
    Type t;
    t.tag = TypeTag::Set;
    t.args = {element};
    return intern(std::move(t));
}

const Type* TypeTable::array(const Type* element, std::uint32_t size) {
    Type t;
    t.tag = TypeTag::Array;
    t.args = {element};
    t.dim = size;
    return intern(std::move(t));
}

const Type* TypeTable::str_builder() {
    Type t;
    t.tag = TypeTag::StrBuilder;
    return intern(std::move(t));
}

const Type* TypeTable::deque(const Type* element) {
    Type t;
    t.tag = TypeTag::Deque;
    t.args = {element};
    return intern(std::move(t));
}

const Type* TypeTable::pqueue(const Type* element) {
    Type t;
    t.tag = TypeTag::PQueue;
    t.args = {element};
    return intern(std::move(t));
}

const Type* TypeTable::result(const Type* ok, const Type* err) {
    Type t;
    t.tag = TypeTag::Result;
    t.args = {ok, err};
    return intern(std::move(t));
}

const Type* TypeTable::with_args(const Type* t, std::vector<const Type*> args) {
    switch (t->tag) {
        case TypeTag::Class: return class_(t->cls(), std::move(args));
        case TypeTag::Struct: return struct_(t->st(), std::move(args));
        case TypeTag::List: return list(args[0]);
        case TypeTag::Fn: return fn(std::move(args), t->ret);
        case TypeTag::Signal: return signal(std::move(args));
        case TypeTag::Nullable: return nullable(args[0]);
        case TypeTag::Map: return map(args[0], args[1]);
        case TypeTag::Set: return set(args[0]);
        case TypeTag::Array: return array(args[0], t->dim);
        case TypeTag::Deque: return deque(args[0]);
        case TypeTag::PQueue: return pqueue(args[0]);
        case TypeTag::Result: return result(args[0], args[1]);
        default: return t;
    }
}

namespace {

std::string args_string(const std::vector<const Type*>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out += ", ";
        out += type_name(args[i]);
    }
    return out;
}

} // namespace

std::string type_name(const Type* t) {
    if (t == nullptr) return "<none>";
    switch (t->tag) {
        case TypeTag::Error: return "<error>";
        case TypeTag::Void: return "void";
        case TypeTag::Null: return "null";
        case TypeTag::Bool: return "bool";
        case TypeTag::Num: return std::string(to_string(t->num));
        case TypeTag::Str: return "str";
        case TypeTag::Name: return "name";
        case TypeTag::Class:
        case TypeTag::Struct: {
            std::string out(t->tag == TypeTag::Class ? t->cls()->name : t->st()->name);
            if (!t->args.empty()) out += "<" + args_string(t->args) + ">";
            return out;
        }
        case TypeTag::Enum: return std::string(t->en()->name);
        case TypeTag::Trait: return std::string(t->tr()->name);
        case TypeTag::List: return "List<" + type_name(t->args[0]) + ">";
        case TypeTag::Fn: {
            std::string out = "fn(" + args_string(t->args) + ")";
            if (t->ret != nullptr && !t->ret->is_void()) out += " -> " + type_name(t->ret);
            return out;
        }
        case TypeTag::Nullable: {
            const Type* inner = t->args[0];
            if (inner->tag == TypeTag::Fn) return "(" + type_name(inner) + ")?";
            return type_name(inner) + "?";
        }
        case TypeTag::Param: return std::string(t->param()->name);
        case TypeTag::Vec: return "vec" + std::to_string(t->dim);
        case TypeTag::Task: return "Task";
        case TypeTag::Signal: return "Signal<" + args_string(t->args) + ">";
        case TypeTag::Map: return "Map<" + args_string(t->args) + ">";
        case TypeTag::Set: return "Set<" + type_name(t->args[0]) + ">";
        case TypeTag::Array: return "[" + type_name(t->args[0]) + "; " + std::to_string(t->dim) + "]";
        case TypeTag::StrBuilder: return "StrBuilder";
        case TypeTag::Deque: return "Deque<" + type_name(t->args[0]) + ">";
        case TypeTag::PQueue: return "PriorityQueue<" + type_name(t->args[0]) + ">";
        case TypeTag::Result: return "Result<" + args_string(t->args) + ">";
    }
    return "?";
}

bool contains_param(const Type* t) {
    if (t->tag == TypeTag::Param) return true;
    for (const Type* a : t->args) {
        if (contains_param(a)) return true;
    }
    return t->ret != nullptr && contains_param(t->ret);
}

const Type* substitute(TypeTable& types, const Type* t, const Subst& subst) {
    if (subst.empty() || !contains_param(t)) return t;
    switch (t->tag) {
        case TypeTag::Param: {
            auto it = subst.find(t->param());
            return it != subst.end() ? it->second : t;
        }
        case TypeTag::Fn: {
            std::vector<const Type*> args;
            args.reserve(t->args.size());
            for (const Type* a : t->args) args.push_back(substitute(types, a, subst));
            return types.fn(std::move(args), substitute(types, t->ret, subst));
        }
        default: {
            if (t->args.empty()) return t;
            std::vector<const Type*> args;
            args.reserve(t->args.size());
            for (const Type* a : t->args) args.push_back(substitute(types, a, subst));
            return types.with_args(t, std::move(args));
        }
    }
}

Program::Program() {
    struct Builtin {
        BuiltinTrait trait;
        std::string_view name;
        std::string_view method;
    };
    static constexpr Builtin builtins[] = {
        {BuiltinTrait::Add, "Add", "add"},       {BuiltinTrait::Sub, "Sub", "sub"},
        {BuiltinTrait::Mul, "Mul", "mul"},       {BuiltinTrait::Div, "Div", "div"},
        {BuiltinTrait::Rem, "Rem", "rem"},       {BuiltinTrait::Neg, "Neg", "neg"},
        {BuiltinTrait::Eq, "Eq", "eq"},          {BuiltinTrait::Ord, "Ord", "cmp"},
        {BuiltinTrait::Index, "Index", "index"}, {BuiltinTrait::IndexSet, "IndexSet", "index_set"},
        {BuiltinTrait::Iterable, "Iterable", "iter"}, {BuiltinTrait::Display, "Display", "to_str"},
        {BuiltinTrait::Default, "Default", ""},
    };
    for (const Builtin& b : builtins) {
        auto t = std::make_unique<TraitInfo>();
        t->name = b.name;
        t->builtin = b.trait;
        t->op_method = b.method;
        t->done = true;
        builtin_traits_[static_cast<int>(b.trait)] = t.get();
        globals.emplace(std::string(b.name), Symbol{SymKind::Trait, t.get(), {}, true, true});
        traits.push_back(std::move(t));
    }
}

} // namespace pluma

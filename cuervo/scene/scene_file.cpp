#include "cuervo/scene/scene_file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <set>

#include "cuervo/core/binary_io.hpp"
#include "cuervo/core/hash.hpp"
#include "cuervo/platform/platform.hpp"

namespace cuervo::scene {

// ------------------------------------------------------------ values

Value::Value() = default;
Value::~Value() = default;
Value::Value(const Value&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(const Value&) = default;
Value& Value::operator=(Value&&) noexcept = default;

Value Value::null() { return {}; }

bool operator==(const Value& a, const Value& b) {
    return a.kind == b.kind && a.num == b.num && a.b == b.b && a.i == b.i && a.f == b.f && a.text == b.text &&
           a.type == b.type && a.vec == b.vec && a.fields == b.fields && a.items == b.items;
}

Value Value::boolean(bool v) {
    Value x;
    x.kind = ValueKind::Bool;
    x.b = v;
    return x;
}
Value Value::integer(std::int64_t v, NumKind k) {
    Value x;
    x.kind = ValueKind::Int;
    x.num = k;
    x.i = v;
    return x;
}
Value Value::number(double v, NumKind k) {
    Value x;
    x.kind = ValueKind::Float;
    x.num = k;
    x.f = k == NumKind::F32 ? static_cast<double>(static_cast<float>(v)) : v;
    return x;
}
Value Value::str(std::string v) {
    Value x;
    x.kind = ValueKind::Str;
    x.text = std::move(v);
    return x;
}
Value Value::name(std::string v) {
    Value x;
    x.kind = ValueKind::Name;
    x.text = std::move(v);
    return x;
}
Value Value::vector(std::vector<float> v) {
    Value x;
    x.kind = ValueKind::Vec;
    x.vec = std::move(v);
    return x;
}
Value Value::enum_value(std::string type, std::string variant) {
    Value x;
    x.kind = ValueKind::Enum;
    x.type = std::move(type);
    x.text = std::move(variant);
    return x;
}

Value Value::list(std::vector<Value> items) {
    Value x;
    x.kind = ValueKind::List;
    x.items = std::move(items);
    return x;
}

std::string_view num_kind_name(NumKind k) {
    static constexpr std::array<std::string_view, 10> names{"i8",  "i16", "int", "i64",   "u8",
                                                            "u16", "u32", "u64", "float", "f64"};
    return names[static_cast<std::size_t>(k)];
}

namespace {

bool is_float_kind(NumKind k) { return k == NumKind::F32 || k == NumKind::F64; }

// Literal suffix of a number kind in the text view (none for int / float).
std::string_view num_suffix(NumKind k) {
    static constexpr std::array<std::string_view, 10> s{"i8", "i16", "", "i64", "u8", "u16", "u32", "u64", "", "f64"};
    return s[static_cast<std::size_t>(k)];
}

} // namespace

int SceneDoc::find(int parent, std::string_view name) const {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent == parent && nodes[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::string validate(const SceneDoc& doc) {
    if (doc.nodes.empty()) return "the scene has no nodes";
    if (doc.nodes[0].parent != -1) return "node 0 must be the root (parent -1)";
    for (std::size_t i = 1; i < doc.nodes.size(); ++i) {
        std::int32_t p = doc.nodes[i].parent;
        if (p < 0 || static_cast<std::size_t>(p) >= i) {
            return std::format("node {} ('{}') has parent {}; nodes must be in pre-order with one root", i,
                               doc.nodes[i].name, p);
        }
    }
    // Pre-order: a node's subtree is contiguous, so a node's parent must be
    // the previous node or one of its ancestors.
    for (std::size_t i = 1; i < doc.nodes.size(); ++i) {
        std::int32_t a = static_cast<std::int32_t>(i - 1);
        while (a != -1 && a != doc.nodes[i].parent) a = doc.nodes[static_cast<std::size_t>(a)].parent;
        if (a == -1) return std::format("node {} ('{}') breaks pre-order", i, doc.nodes[i].name);
    }
    return {};
}

std::uint64_t schema_hash(const SceneDoc& doc) {
    std::set<std::string> entries; // sorted: independent of node order
    for (const Node& n : doc.nodes) {
        entries.insert(n.type);
        for (const Field& f : n.fields) entries.insert(std::format("{}.{}:{}", n.type, f.name, f.type));
    }
    StateHasher h;
    for (const std::string& e : entries) {
        h.bytes(e.data(), e.size());
        h.value(std::uint8_t{0});
    }
    return h.digest();
}

// ------------------------------------------------------------ migrations

namespace {

// v1 nodes had no names; name them after their type (DESIGN.md 4.12: old
// files always load).
void migrate_v1_names(SceneDoc& doc) {
    for (Node& n : doc.nodes) {
        if (n.name.empty()) n.name = n.type;
    }
}

constexpr Migration migration_table[] = {
    {1, "v1 -> v2: nodes get names (named after their type)", &migrate_v1_names},
};

} // namespace

std::span<const Migration> migrations() { return migration_table; }

// ------------------------------------------------------------ binary

namespace {

constexpr std::size_t max_depth = 64;

// Strings are either inline (v1) or indices into the string table (v2).
class StringOut {
public:
    explicit StringOut(bool table) : table_(table) {}
    void put(ByteWriter& w, const std::string& s) {
        if (!table_) {
            w.str(s);
            return;
        }
        auto it = index_.find(s);
        if (it == index_.end()) {
            it = index_.emplace(s, static_cast<std::uint32_t>(strings_.size())).first;
            strings_.push_back(s);
        }
        w.u32(it->second);
    }
    const std::vector<std::string>& strings() const { return strings_; }

private:
    bool table_;
    std::map<std::string, std::uint32_t> index_;
    std::vector<std::string> strings_;
};

void write_value(ByteWriter& w, StringOut& s, const Value& v) {
    w.u8(static_cast<std::uint8_t>(v.kind));
    switch (v.kind) {
        case ValueKind::Null: break;
        case ValueKind::Bool: w.u8(v.b ? 1 : 0); break;
        case ValueKind::Int:
            w.u8(static_cast<std::uint8_t>(v.num));
            w.i64(v.i);
            break;
        case ValueKind::Float:
            w.u8(static_cast<std::uint8_t>(v.num));
            if (v.num == NumKind::F64) {
                w.f64(v.f);
            } else {
                w.f32(static_cast<float>(v.f));
            }
            break;
        case ValueKind::Str:
        case ValueKind::Name: s.put(w, v.text); break;
        case ValueKind::Vec:
            w.u32(static_cast<std::uint32_t>(v.vec.size()));
            for (float c : v.vec) w.f32(c);
            break;
        case ValueKind::Enum:
            s.put(w, v.type);
            s.put(w, v.text);
            break;
        case ValueKind::Struct:
            s.put(w, v.type);
            w.u32(static_cast<std::uint32_t>(v.fields.size()));
            for (const Field& f : v.fields) {
                s.put(w, f.name);
                write_value(w, s, f.value);
            }
            break;
        case ValueKind::List:
            w.u32(static_cast<std::uint32_t>(v.items.size()));
            for (const Value& item : v.items) write_value(w, s, item);
            break;
    }
}

void write_nodes(ByteWriter& w, StringOut& s, const SceneDoc& doc, bool names) {
    w.u32(static_cast<std::uint32_t>(doc.nodes.size()));
    for (const Node& n : doc.nodes) {
        s.put(w, n.type);
        if (names) s.put(w, n.name);
        w.i32(n.parent);
        w.u32(static_cast<std::uint32_t>(n.fields.size()));
        for (const Field& f : n.fields) {
            s.put(w, f.name);
            s.put(w, f.type);
            write_value(w, s, f.value);
        }
    }
}

void write_header(ByteWriter& w, std::uint32_t version, std::uint64_t hash) {
    w.raw(magic, 4);
    w.u32(version);
    w.u64(hash);
}

class StringIn {
public:
    StringIn(ByteReader& r, bool table) : r_(r), table_(table) {}
    bool read_table() {
        std::uint32_t n = r_.count(4);
        strings_.reserve(n);
        for (std::uint32_t i = 0; i < n && r_.ok(); ++i) strings_.push_back(r_.str());
        return r_.ok();
    }
    std::string get() {
        if (!table_) return r_.str();
        std::uint32_t i = r_.u32();
        if (i >= strings_.size()) {
            r_.fail(std::format("string index {} out of range", i));
            return {};
        }
        return strings_[i];
    }

private:
    ByteReader& r_;
    bool table_;
    std::vector<std::string> strings_;
};

Value read_value(ByteReader& r, StringIn& s, std::size_t depth) {
    Value v;
    if (depth > max_depth) {
        r.fail("values nested too deeply");
        return v;
    }
    std::uint8_t kind = r.u8();
    if (kind > static_cast<std::uint8_t>(ValueKind::List)) {
        r.fail(std::format("unknown value kind {}", kind));
        return v;
    }
    v.kind = static_cast<ValueKind>(kind);
    auto read_num = [&](bool want_float) {
        std::uint8_t k = r.u8();
        if (k > static_cast<std::uint8_t>(NumKind::F64) || is_float_kind(static_cast<NumKind>(k)) != want_float) {
            r.fail(std::format("bad number kind {}", k));
            return NumKind::I32;
        }
        return static_cast<NumKind>(k);
    };
    switch (v.kind) {
        case ValueKind::Null: break;
        case ValueKind::Bool: v.b = r.u8() != 0; break;
        case ValueKind::Int:
            v.num = read_num(false);
            v.i = r.i64();
            break;
        case ValueKind::Float:
            v.num = read_num(true);
            v.f = v.num == NumKind::F64 ? r.f64() : static_cast<double>(r.f32());
            break;
        case ValueKind::Str:
        case ValueKind::Name: v.text = s.get(); break;
        case ValueKind::Vec: {
            std::uint32_t n = r.count(4);
            v.vec.resize(n);
            for (float& c : v.vec) c = r.f32();
            break;
        }
        case ValueKind::Enum:
            v.type = s.get();
            v.text = s.get();
            break;
        case ValueKind::Struct: {
            v.type = s.get();
            std::uint32_t n = r.count(5);
            for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
                Field f;
                f.name = s.get();
                f.value = read_value(r, s, depth + 1);
                v.fields.push_back(std::move(f));
            }
            break;
        }
        case ValueKind::List: {
            std::uint32_t n = r.count(1);
            for (std::uint32_t i = 0; i < n && r.ok(); ++i) v.items.push_back(read_value(r, s, depth + 1));
            break;
        }
    }
    return v;
}

} // namespace

std::vector<std::byte> write_binary(const SceneDoc& doc) {
    StringOut strings(true);
    ByteWriter body;
    write_nodes(body, strings, doc, true);
    ByteWriter w;
    write_header(w, format_version, schema_hash(doc));
    w.u32(static_cast<std::uint32_t>(strings.strings().size()));
    for (const std::string& s : strings.strings()) w.str(s);
    std::vector<std::byte> b = body.take();
    w.raw(b.data(), b.size());
    return w.take();
}

std::vector<std::byte> write_binary_v1(const SceneDoc& doc) {
    StringOut strings(false);
    ByteWriter w;
    write_header(w, 1, schema_hash(doc));
    write_nodes(w, strings, doc, false);
    return w.take();
}

std::optional<SceneDoc> read_binary(std::span<const std::byte> data, std::string& error, LoadInfo* info) {
    ByteReader r(data, "scene");
    char m[4] = {};
    r.raw(m, 4);
    if (!r.ok() || !std::equal(m, m + 4, magic)) {
        error = "not a scene file (bad magic)";
        return std::nullopt;
    }
    LoadInfo li;
    li.version = r.u32();
    li.schema_hash = r.u64();
    if (li.version < 1 || li.version > format_version) {
        error = std::format("scene format version {} is not supported (this build reads 1..{})", li.version,
                            format_version);
        return std::nullopt;
    }
    const bool v2 = li.version >= 2;
    StringIn strings(r, v2);
    if (v2) strings.read_table();
    SceneDoc doc;
    std::uint32_t count = r.count(13);
    for (std::uint32_t i = 0; i < count && r.ok(); ++i) {
        Node n;
        n.type = strings.get();
        if (v2) n.name = strings.get();
        n.parent = r.i32();
        std::uint32_t fields = r.count(9);
        for (std::uint32_t k = 0; k < fields && r.ok(); ++k) {
            Field f;
            f.name = strings.get();
            f.type = strings.get();
            f.value = read_value(r, strings, 0);
            n.fields.push_back(std::move(f));
        }
        doc.nodes.push_back(std::move(n));
    }
    if (r.ok() && !r.at_end()) r.fail("trailing data");
    if (!r.ok()) {
        error = r.error();
        return std::nullopt;
    }
    if (std::string bad = validate(doc); !bad.empty()) {
        error = bad;
        return std::nullopt;
    }
    for (const Migration& mig : migrations()) {
        if (mig.from >= li.version) {
            mig.apply(doc);
            li.migrated.emplace_back(mig.what);
        }
    }
    if (info != nullptr) *info = std::move(li);
    return doc;
}

// ------------------------------------------------------------ text dump

namespace {

void put_float(std::string& out, double v, NumKind k) {
    char buf[64];
    std::to_chars_result r = k == NumKind::F64 ? std::to_chars(buf, buf + sizeof buf, v)
                                               : std::to_chars(buf, buf + sizeof buf, static_cast<float>(v));
    std::string_view s(buf, static_cast<std::size_t>(r.ptr - buf));
    out += s;
    if (s.find_first_of(".en") == std::string_view::npos) out += ".0"; // keep it a float literal
    out += num_suffix(k);
}

bool is_ident(std::string_view s) {
    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

void put_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{{{:x}}}", static_cast<unsigned>(c));
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void put_value(std::string& out, const Value& v) {
    switch (v.kind) {
        case ValueKind::Null: out += "null"; break;
        case ValueKind::Bool: out += v.b ? "true" : "false"; break;
        case ValueKind::Int:
            if (v.num == NumKind::U64) {
                out += std::to_string(static_cast<std::uint64_t>(v.i));
            } else {
                out += std::to_string(v.i);
            }
            out += num_suffix(v.num);
            break;
        case ValueKind::Float: put_float(out, v.f, v.num); break;
        case ValueKind::Str: put_string(out, v.text); break;
        case ValueKind::Name:
            out += '#';
            if (is_ident(v.text)) {
                out += v.text;
            } else {
                put_string(out, v.text);
            }
            break;
        case ValueKind::Vec:
            out += '(';
            for (std::size_t i = 0; i < v.vec.size(); ++i) {
                if (i > 0) out += ", ";
                put_float(out, v.vec[i], NumKind::F32);
            }
            out += ')';
            break;
        case ValueKind::Enum: out += std::format("{}.{}", v.type, v.text); break;
        case ValueKind::Struct:
            out += v.type;
            out += " {";
            for (std::size_t i = 0; i < v.fields.size(); ++i) {
                out += i > 0 ? ", " : " ";
                out += v.fields[i].name;
                out += " = ";
                put_value(out, v.fields[i].value);
            }
            out += v.fields.empty() ? "}" : " }";
            break;
        case ValueKind::List:
            out += '[';
            for (std::size_t i = 0; i < v.items.size(); ++i) {
                if (i > 0) out += ", ";
                put_value(out, v.items[i]);
            }
            out += ']';
            break;
    }
}

} // namespace

std::string dump_text(const SceneDoc& doc, const LoadInfo* info) {
    std::string out;
    std::uint32_t version = info != nullptr ? info->version : format_version;
    std::uint64_t hash = info != nullptr ? info->schema_hash : schema_hash(doc);
    out += std::format("scene version {} schema {:016x}\n", version, hash);
    if (info != nullptr) {
        for (const std::string& m : info->migrated) out += std::format("// migrated: {}\n", m);
    }
    // Children of each node, in order.
    std::vector<std::vector<std::size_t>> children(doc.nodes.size());
    for (std::size_t i = 1; i < doc.nodes.size(); ++i) {
        children[static_cast<std::size_t>(doc.nodes[i].parent)].push_back(i);
    }
    auto emit = [&](auto& self, std::size_t i, std::size_t depth) -> void {
        const Node& n = doc.nodes[i];
        std::string pad(depth * 4, ' ');
        out += std::format("{}node {} ", pad, n.type);
        put_string(out, n.name);
        out += " {\n";
        for (const Field& f : n.fields) {
            out += std::format("{}    {}: {} = ", pad, f.name, f.type);
            put_value(out, f.value);
            out += '\n';
        }
        for (std::size_t c : children[i]) self(self, c, depth + 1);
        out += pad + "}\n";
    };
    if (!doc.nodes.empty()) emit(emit, 0, 0);
    return out;
}

// ------------------------------------------------------------ text parse

namespace {

class TextParser {
public:
    explicit TextParser(std::string_view text) : s_(text) {}

    std::optional<SceneDoc> parse(std::string& error) {
        skip();
        if (word_is("scene")) skip_line();
        skip();
        if (!word_is("node")) return fail(error, "expected 'node'");
        parse_node(-1, 0);
        skip();
        if (ok_ && pos_ < s_.size()) fail_msg("only one root node is allowed");
        if (!ok_) {
            error = error_;
            return std::nullopt;
        }
        if (std::string bad = validate(doc_); !bad.empty()) {
            error = bad;
            return std::nullopt;
        }
        return std::move(doc_);
    }

    // One value in the text view's syntax, nothing else after it.
    std::optional<Value> parse_single_value(std::string& error) {
        Value v = value(0);
        skip();
        if (ok_ && pos_ < s_.size()) fail_msg("unexpected text after the value");
        if (!ok_) {
            error = error_;
            return std::nullopt;
        }
        return v;
    }

private:
    std::optional<SceneDoc> fail(std::string& error, std::string_view message) {
        fail_msg(message);
        error = error_;
        return std::nullopt;
    }
    void fail_msg(std::string_view message) {
        if (!ok_) return;
        ok_ = false;
        std::size_t line = 1 + static_cast<std::size_t>(std::count(s_.begin(), s_.begin() + static_cast<std::ptrdiff_t>(pos_), '\n'));
        error_ = std::format("line {}: {}", line, message);
    }

    void skip() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (s_.substr(pos_, 2) == "//") {
                skip_line();
            } else {
                break;
            }
        }
    }
    void skip_line() {
        while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
    }
    bool peek(char c) {
        skip();
        return pos_ < s_.size() && s_[pos_] == c;
    }
    bool eat(char c) {
        if (!peek(c)) return false;
        ++pos_;
        return true;
    }
    void expect(char c) {
        if (!eat(c)) fail_msg(std::format("expected '{}'", c));
    }
    static bool word_char(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }
    std::string_view word() {
        skip();
        std::size_t start = pos_;
        while (pos_ < s_.size() && word_char(s_[pos_])) ++pos_;
        return s_.substr(start, pos_ - start);
    }
    bool word_is(std::string_view w) {
        skip();
        if (s_.substr(pos_, w.size()) != w) return false;
        if (pos_ + w.size() < s_.size() && word_char(s_[pos_ + w.size()])) return false;
        pos_ += w.size();
        return true;
    }
    // A type name: word characters plus <, >, ?, ',' inside angle brackets.
    std::string type_name() {
        skip();
        std::size_t start = pos_;
        int depth = 0;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (word_char(c) || c == '?') {
                ++pos_;
            } else if (c == '<') {
                ++depth;
                ++pos_;
            } else if (c == '>' && depth > 0) {
                --depth;
                ++pos_;
            } else if ((c == ',' || c == ' ') && depth > 0) {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) fail_msg("expected a type name");
        return std::string(s_.substr(start, pos_ - start));
    }
    std::string string_lit() {
        std::string out;
        if (!eat('"')) {
            fail_msg("expected a string");
            return out;
        }
        while (ok_) {
            if (pos_ >= s_.size() || s_[pos_] == '\n') {
                fail_msg("unterminated string");
                break;
            }
            char c = s_[pos_++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            char e = pos_ < s_.size() ? s_[pos_++] : '\0';
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u': {
                    std::size_t close = s_.find('}', pos_);
                    unsigned cp = 0;
                    if (pos_ >= s_.size() || s_[pos_] != '{' || close == std::string_view::npos ||
                        std::from_chars(s_.data() + pos_ + 1, s_.data() + close, cp, 16).ptr != s_.data() + close) {
                        fail_msg("bad \\u{...} escape");
                        break;
                    }
                    pos_ = close + 1;
                    append_utf8(out, cp);
                    break;
                }
                default: fail_msg("unknown escape in string");
            }
        }
        return out;
    }
    static void append_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }

    // A number token: sign, digits, '.', exponent, inf/nan, and a suffix.
    std::optional<Value> number() {
        skip();
        std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            bool exp_sign = (c == '-' || c == '+') && pos_ > start && (s_[pos_ - 1] == 'e' || s_[pos_ - 1] == 'E');
            if (word_char(c) || c == '.' || exp_sign) {
                ++pos_;
            } else {
                break;
            }
        }
        std::string_view tok = s_.substr(start, pos_ - start);
        static constexpr std::array<std::pair<std::string_view, NumKind>, 10> suffixes{{
            {"i8", NumKind::I8},   {"i16", NumKind::I16}, {"i32", NumKind::I32}, {"i64", NumKind::I64},
            {"u8", NumKind::U8},   {"u16", NumKind::U16}, {"u32", NumKind::U32}, {"u64", NumKind::U64},
            {"f32", NumKind::F32}, {"f64", NumKind::F64},
        }};
        std::optional<NumKind> kind;
        for (const auto& [suf, k] : suffixes) {
            if (tok.size() > suf.size() && tok.ends_with(suf)) {
                kind = k;
                tok.remove_suffix(suf.size());
                break;
            }
        }
        std::string_view body = tok;
        if (!body.empty() && body[0] == '+') body.remove_prefix(1);
        bool is_float = body.find_first_of(".eEn") != std::string_view::npos; // n: inf / nan
        if (kind) is_float = is_float_kind(*kind);
        if (body.empty()) return std::nullopt;
        const char* first = body.data();
        const char* last = body.data() + body.size();
        if (is_float) {
            double d = 0.0;
            if (body == "inf" || body == "-inf" || body == "nan" || body == "-nan") {
                d = body.starts_with('-') ? -HUGE_VAL : HUGE_VAL;
                if (body.ends_with("nan")) d = std::nan("");
            } else if (std::from_chars(first, last, d).ptr != last) {
                return std::nullopt;
            }
            return Value::number(d, kind.value_or(NumKind::F32));
        }
        NumKind k = kind.value_or(NumKind::I32);
        if (k == NumKind::U64) {
            std::uint64_t u = 0;
            if (std::from_chars(first, last, u).ptr != last) return std::nullopt;
            return Value::integer(static_cast<std::int64_t>(u), k);
        }
        std::int64_t i = 0;
        if (std::from_chars(first, last, i).ptr != last) return std::nullopt;
        return Value::integer(i, k);
    }

    Value value(std::size_t depth) {
        Value v;
        if (depth > max_depth) {
            fail_msg("values nested too deeply");
            return v;
        }
        skip();
        if (pos_ >= s_.size()) {
            fail_msg("expected a value");
            return v;
        }
        char c = s_[pos_];
        if (c == '"') return Value::str(string_lit());
        if (c == '#') {
            ++pos_;
            if (pos_ < s_.size() && s_[pos_] == '"') return Value::name(string_lit());
            return Value::name(std::string(word()));
        }
        if (c == '(') {
            ++pos_;
            v.kind = ValueKind::Vec;
            while (ok_ && !eat(')')) {
                if (!v.vec.empty()) expect(',');
                auto n = number();
                if (!n || n->kind != ValueKind::Float || n->num != NumKind::F32) {
                    fail_msg("vector components are float literals, e.g. (1.0, 2.5)");
                    return v;
                }
                v.vec.push_back(static_cast<float>(n->f));
            }
            return v;
        }
        if (c == '[') {
            ++pos_;
            v.kind = ValueKind::List;
            while (ok_ && !eat(']')) {
                if (!v.items.empty()) expect(',');
                v.items.push_back(value(depth + 1));
            }
            return v;
        }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            auto n = number();
            if (!n) fail_msg("bad number");
            return n.value_or(Value{});
        }
        std::size_t save = pos_;
        std::string_view w = word();
        if (w.empty()) {
            fail_msg("expected a value");
            return v;
        }
        if (w == "null") return Value::null();
        if (w == "true" || w == "false") return Value::boolean(w == "true");
        if (w == "inf" || w == "nan") {
            pos_ = save;
            auto n = number();
            if (!n) fail_msg("bad number");
            return n.value_or(Value{});
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            std::string_view variant = word();
            if (variant.empty()) fail_msg("expected an enum variant after '.'");
            return Value::enum_value(std::string(w), std::string(variant));
        }
        if (eat('{')) {
            v.kind = ValueKind::Struct;
            v.type = std::string(w);
            while (ok_ && !eat('}')) {
                if (!v.fields.empty()) expect(',');
                Field f;
                f.name = std::string(word());
                if (f.name.empty()) {
                    fail_msg("expected a field name");
                    return v;
                }
                expect('=');
                f.value = value(depth + 1);
                v.fields.push_back(std::move(f));
            }
            return v;
        }
        fail_msg(std::format("unexpected '{}'", w));
        return v;
    }

    void parse_node(std::int32_t parent, std::size_t depth) {
        if (depth > max_depth) {
            fail_msg("nodes nested too deeply");
            return;
        }
        Node n;
        n.parent = parent;
        n.type = std::string(word());
        if (n.type.empty()) fail_msg("expected the node's type");
        n.name = string_lit();
        expect('{');
        auto index = static_cast<std::int32_t>(doc_.nodes.size());
        doc_.nodes.push_back(std::move(n));
        while (ok_ && !eat('}')) {
            if (pos_ >= s_.size()) {
                fail_msg("missing '}'");
                return;
            }
            if (word_is("node")) {
                parse_node(index, depth + 1);
                continue;
            }
            Field f;
            f.name = std::string(word());
            if (f.name.empty()) {
                fail_msg("expected a field or 'node'");
                return;
            }
            expect(':');
            f.type = type_name();
            expect('=');
            f.value = value(0);
            doc_.nodes[static_cast<std::size_t>(index)].fields.push_back(std::move(f));
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    bool ok_ = true;
    std::string error_;
    SceneDoc doc_;
};

} // namespace

std::optional<SceneDoc> parse_text(std::string_view text, std::string& error) { return TextParser(text).parse(error); }

std::string format_value(const Value& v) {
    std::string out;
    put_value(out, v);
    return out;
}

std::optional<Value> parse_value(std::string_view text, std::string& error) {
    return TextParser(text).parse_single_value(error);
}

// ------------------------------------------------------------ files

std::optional<SceneDoc> load_file(const std::filesystem::path& path, std::string& error, LoadInfo* info) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::format("can't open '{}'", platform::utf8(path));
        return std::nullopt;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto doc = read_binary(std::as_bytes(std::span(bytes)), error, info);
    if (!doc) error = std::format("{}: {}", platform::utf8(path), error);
    return doc;
}

bool save_file(const std::filesystem::path& path, const SceneDoc& doc, std::string& error) {
    if (std::string bad = validate(doc); !bad.empty()) {
        error = bad;
        return false;
    }
    std::vector<std::byte> bytes = write_binary(doc);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        error = std::format("can't write '{}'", platform::utf8(path));
        return false;
    }
    return true;
}

} // namespace cuervo::scene

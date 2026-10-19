#pragma once

// Minimal JSON value, parser and serializer for the language server
// (pluma/lsp.hpp) and the debug adapter (pluma/dap.hpp): both speak JSON over
// `Content-Length`-framed stdio, and `pluma/` takes no third-party
// dependencies.

#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pluma::json {


enum class Kind : std::uint8_t { Null, Bool, Int, Float, String, Array, Object };

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

struct Value {
    Kind kind = Kind::Null;
    bool b = false;
    std::int64_t i = 0;
    double f = 0.0;
    std::string s;
    Array a;
    Object o;

    static Value null() { return {}; }
    static Value boolean(bool v) {
        Value x;
        x.kind = Kind::Bool;
        x.b = v;
        return x;
    }
    static Value integer(std::int64_t v) {
        Value x;
        x.kind = Kind::Int;
        x.i = v;
        return x;
    }
    static Value string(std::string_view v) {
        Value x;
        x.kind = Kind::String;
        x.s = std::string(v);
        return x;
    }
    static Value array(Array v = {}) {
        Value x;
        x.kind = Kind::Array;
        x.a = std::move(v);
        return x;
    }
    static Value object(Object v = {}) {
        Value x;
        x.kind = Kind::Object;
        x.o = std::move(v);
        return x;
    }

    bool is_null() const { return kind == Kind::Null; }
    bool is_bool() const { return kind == Kind::Bool; }
    bool is_int() const { return kind == Kind::Int; }
    bool is_string() const { return kind == Kind::String; }
    bool is_array() const { return kind == Kind::Array; }
    bool is_object() const { return kind == Kind::Object; }

    const Value* get(std::string_view key) const {
        if (kind != Kind::Object) return nullptr;
        auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }

    std::string_view str_or(std::string_view key, std::string_view fallback = {}) const {
        const Value* v = get(key);
        return v != nullptr && v->is_string() ? std::string_view(v->s) : fallback;
    }

    std::int64_t int_or(std::string_view key, std::int64_t fallback = 0) const {
        const Value* v = get(key);
        if (v == nullptr) return fallback;
        if (v->kind == Kind::Int) return v->i;
        if (v->kind == Kind::Float) return static_cast<std::int64_t>(v->f);
        return fallback;
    }

    bool bool_or(std::string_view key, bool fallback = false) const {
        const Value* v = get(key);
        return v != nullptr && v->is_bool() ? v->b : fallback;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    std::optional<Value> parse() {
        skip_ws();
        auto v = parse_value();
        if (!v) return std::nullopt;
        skip_ws();
        if (pos_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool consume(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) != lit) return false;
        pos_ += lit.size();
        return true;
    }

    std::optional<Value> parse_value() {
        skip_ws();
        if (pos_ >= s_.size()) return std::nullopt;
        char c = s_[pos_];
        if (c == 'n') {
            if (!consume("null")) return std::nullopt;
            return Value::null();
        }
        if (c == 't') {
            if (!consume("true")) return std::nullopt;
            return Value::boolean(true);
        }
        if (c == 'f') {
            if (!consume("false")) return std::nullopt;
            return Value::boolean(false);
        }
        if (c == '"') {
            auto str = parse_string();
            if (!str) return std::nullopt;
            return Value::string(*str);
        }
        if (c == '[') return parse_array();
        if (c == '{') return parse_object();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        return std::nullopt;
    }

    std::optional<std::string> parse_string() {
        if (pos_ >= s_.size() || s_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return std::nullopt;
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) return std::nullopt;
                    std::uint32_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                        else return std::nullopt;
                    }
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> parse_number() {
        std::size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        bool is_flt = false;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            is_flt = true;
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            is_flt = true;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        std::string num_str(s_.substr(start, pos_ - start));
        try {
            if (is_flt) {
                Value v;
                v.kind = Kind::Float;
                v.f = std::stod(num_str);
                return v;
            }
            return Value::integer(std::stoll(num_str));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<Value> parse_array() {
        ++pos_; // '['
        Array arr;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return Value::array(std::move(arr));
        }
        for (;;) {
            auto elem = parse_value();
            if (!elem) return std::nullopt;
            arr.push_back(std::move(*elem));
            skip_ws();
            if (pos_ >= s_.size()) return std::nullopt;
            if (s_[pos_] == ']') {
                ++pos_;
                return Value::array(std::move(arr));
            }
            if (s_[pos_] != ',') return std::nullopt;
            ++pos_;
        }
    }

    std::optional<Value> parse_object() {
        ++pos_; // '{'
        Object obj;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return Value::object(std::move(obj));
        }
        for (;;) {
            skip_ws();
            auto key = parse_string();
            if (!key) return std::nullopt;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return std::nullopt;
            ++pos_;
            auto val = parse_value();
            if (!val) return std::nullopt;
            obj[*key] = std::move(*val);
            skip_ws();
            if (pos_ >= s_.size()) return std::nullopt;
            if (s_[pos_] == '}') {
                ++pos_;
                return Value::object(std::move(obj));
            }
            if (s_[pos_] != ',') return std::nullopt;
            ++pos_;
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

inline void dump_escaped(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

inline void dump_to(std::string& out, const Value& v) {
    switch (v.kind) {
        case Kind::Null: out += "null"; break;
        case Kind::Bool: out += v.b ? "true" : "false"; break;
        case Kind::Int: out += std::to_string(v.i); break;
        case Kind::Float: out += std::format("{}", v.f); break;
        case Kind::String: dump_escaped(out, v.s); break;
        case Kind::Array: {
            out += '[';
            for (std::size_t i = 0; i < v.a.size(); ++i) {
                if (i > 0) out += ',';
                dump_to(out, v.a[i]);
            }
            out += ']';
            break;
        }
        case Kind::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, val] : v.o) {
                if (!first) out += ',';
                first = false;
                dump_escaped(out, k);
                out += ':';
                dump_to(out, val);
            }
            out += '}';
            break;
        }
    }
}

inline std::string dump(const Value& v) {
    std::string out;
    dump_to(out, v);
    return out;
}

} // namespace pluma::json

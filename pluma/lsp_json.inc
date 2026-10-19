// Included inside `namespace pluma::lsp` in `pluma/lsp.cpp`: LSP-specific
// JSON helpers and JSON-RPC 2.0 messages over pluma/json.hpp.

namespace json {

using namespace ::pluma::json;

inline Value pos_json(Position p) {
    return Value::object({
        {"line", Value::integer(p.line)},
        {"character", Value::integer(p.character)},
    });
}

inline Value range_json(Range r) {
    return Value::object({
        {"start", pos_json(r.start)},
        {"end", pos_json(r.end)},
    });
}

inline Value location_json(const Location& l) {
    return Value::object({
        {"uri", Value::string(l.uri)},
        {"range", range_json(l.range)},
    });
}

inline Value edit_json(const TextEdit& e) {
    return Value::object({
        {"range", range_json(e.range)},
        {"newText", Value::string(e.new_text)},
    });
}

inline Position parse_pos(const Value* v) {
    if (v == nullptr || !v->is_object()) return {};
    return {
        static_cast<std::uint32_t>( std::max<std::int64_t>(0, v->int_or("line", 0))),
        static_cast<std::uint32_t>(std::max<std::int64_t>(0, v->int_or("character", 0))),
    };
}

inline Range parse_range(const Value* v) {
    if (v == nullptr || !v->is_object()) return {};
    return {parse_pos(v->get("start")), parse_pos(v->get("end"))};
}

inline std::string make_response(const Value& id, Value result) {
    return dump(Value::object({
        {"jsonrpc", Value::string("2.0")},
        {"id", id},
        {"result", std::move(result)},
    }));
}

inline std::string make_error_response(const Value& id, int code, std::string_view message) {
    return dump(Value::object({
        {"jsonrpc", Value::string("2.0")},
        {"id", id},
        {"error", Value::object({
                      {"code", Value::integer(code)},
                      {"message", Value::string(message)},
                  })},
    }));
}

inline std::string make_notification(std::string_view method, Value params) {
    return dump(Value::object({
        {"jsonrpc", Value::string("2.0")},
        {"method", Value::string(method)},
        {"params", std::move(params)},
    }));
}

} // namespace json

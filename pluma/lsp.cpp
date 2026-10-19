#include "pluma/lsp.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>

#include "pluma/ast.hpp"
#include "pluma/builtins.hpp"
#include "pluma/checker.hpp"
#include "pluma/format.hpp"
#include "pluma/json.hpp"
#include "pluma/lexer.hpp"
#include "pluma/parser.hpp"
#include "pluma/printer.hpp"
#include "pluma/tir.hpp"

namespace pluma::lsp {

#include "pluma/lsp_json.inc"

namespace {

namespace fs = std::filesystem;

bool is_ident_start(char c) {
    auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == '_';
}

bool is_ident_char(char c) {
    auto u = static_cast<unsigned char>(c);
    return is_ident_start(c) || (u >= '0' && u <= '9');
}

bool is_valid_identifier(std::string_view s) {
    if (s.empty() || !is_ident_start(s[0])) return false;
    for (char c : s) {
        if (!is_ident_char(c)) return false;
    }
    return true;
}

std::string normalize_uri(std::string_view uri) {
    if (uri.starts_with("file://")) {
        return path_to_uri(uri_to_path(uri));
    }
    if (!uri.empty() && (uri[0] == '/' || (uri.size() > 2 && uri[1] == ':'))) {
        return path_to_uri(uri);
    }
    return std::string(uri);
}

std::optional<std::string> slurp_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// LSP positions count UTF-16 code units within a line (the protocol default;
// VS Code requires it), while the compiler's spans are UTF-8 byte offsets.

// Byte length of the UTF-8 sequence starting with `lead` (1 for stray bytes).
std::uint32_t utf8_length(unsigned char lead) {
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    if (lead >= 0xE0) return 3;
    if (lead >= 0xC2 && lead < 0xE0) return 2;
    return 1;
}

// Convert a byte offset within a single file's text into a 0-based LSP Position.
Position offset_to_pos(std::string_view text, std::uint32_t offset) {
    offset = std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(text.size()));
    std::uint32_t line = 0;
    std::uint32_t character = 0;
    for (std::uint32_t i = 0; i < offset;) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\n') {
            ++line;
            character = 0;
            ++i;
            continue;
        }
        const std::uint32_t n = utf8_length(c);
        if (i + n > offset) break; // offset inside a sequence: stop at its start
        character += n == 4 ? 2 : 1; // astral characters are surrogate pairs
        i += n;
    }
    return {line, character};
}

// Convert a 0-based LSP Position into a byte offset within `text`.
std::uint32_t pos_to_offset(std::string_view text, Position pos) {
    std::uint32_t cur_line = 0;
    std::uint32_t idx = 0;
    while (idx < text.size() && cur_line < pos.line) {
        if (text[idx] == '\n') ++cur_line;
        ++idx;
    }
    std::uint32_t units = 0;
    while (idx < text.size() && text[idx] != '\n' && text[idx] != '\r' && units < pos.character) {
        const std::uint32_t n = utf8_length(static_cast<unsigned char>(text[idx]));
        units += n == 4 ? 2 : 1;
        idx = std::min<std::uint32_t>(idx + n, static_cast<std::uint32_t>(text.size()));
    }
    return idx;
}

constexpr bool span_empty(Span s) { return s.begin >= s.end; }

// Find the span of `name` inside `haystack` in `source` (used when an entity
// stores the full declaration span instead of just the identifier span).
Span find_ident_span(const SourceFile& source, Span haystack, std::string_view name) {
    std::string_view full = source.text();
    if (name.empty() || span_empty(haystack) || haystack.end > full.size()) return haystack;
    std::uint32_t pos = haystack.begin;
    while (pos + name.size() <= haystack.end) {
        auto found = full.substr(pos, haystack.end - pos).find(name);
        if (found == std::string_view::npos) break;
        std::uint32_t idx = pos + static_cast<std::uint32_t>(found);
        bool ok_before = (idx == 0) || !is_ident_char(full[idx - 1]);
        bool ok_after = (idx + name.size() >= full.size()) || !is_ident_char(full[idx + name.size()]);
        if (ok_before && ok_after) {
            return {idx, static_cast<std::uint32_t>(idx + name.size())};
        }
        pos = idx + 1;
    }
    return haystack;
}

// Extract leading `//` comment lines immediately preceding the declaration line
// of `decl_span` within `source` (respecting file part boundaries).
std::string extract_doc_comment(const SourceFile& source, Span decl_span) {
    std::string_view full = source.text();
    if (span_empty(decl_span) || decl_span.begin > full.size()) return {};
    LineCol lc = source.line_col(decl_span.begin);
    if (lc.line <= 1) return {};

    std::string_view file_name = source.location(decl_span.begin).path;
    // Locate the start of `decl_span`'s line in `full`.
    std::uint32_t line_start = decl_span.begin;
    while (line_start > 0 && full[line_start - 1] != '\n') --line_start;

    std::vector<std::string> doc_lines;
    std::uint32_t cur = line_start;
    while (cur > 0) {
        std::uint32_t prev_end = cur - 1; // points at '\n'
        if (source.location(prev_end).path != file_name) break;
        std::uint32_t prev_start = prev_end;
        while (prev_start > 0 && full[prev_start - 1] != '\n') --prev_start;
        if (source.location(prev_start).path != file_name) break;

        std::string_view line = full.substr(prev_start, prev_end - prev_start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);

        if (line.starts_with("//")) {
            std::string_view body = line.substr(2);
            if (body.starts_with('/')) body.remove_prefix(1);
            if (body.starts_with(' ')) body.remove_prefix(1);
            // Skip section separator lines like `// --------` or `// ========`
            bool all_punct = !body.empty();
            for (char c : body) {
                if (c != '-' && c != '=' && c != ' ') {
                    all_punct = false;
                    break;
                }
            }
            if (!all_punct) {
                doc_lines.emplace_back(body);
            }
            cur = prev_start;
            continue;
        }
        if (line.starts_with('@')) {
            cur = prev_start;
            continue;
        }
        break;
    }
    std::reverse(doc_lines.begin(), doc_lines.end());
    std::string out;
    for (std::size_t i = 0; i < doc_lines.size(); ++i) {
        if (i > 0) out += '\n';
        out += doc_lines[i];
    }
    return out;
}

std::string format_param(const ParamInfo& p) {
    std::string s(p.name);
    if (p.type != nullptr && !p.type->is_error()) {
        s += ": ";
        s += type_name(p.type);
    }
    if (p.ast != nullptr && p.ast->default_value != nullptr) {
        s += " = ";
        s += print(p.ast->default_value);
    }
    return s;
}

std::string format_generics(const std::vector<GenericParamInfo*>& generics) {
    if (generics.empty()) return {};
    std::string out = "<";
    for (std::size_t i = 0; i < generics.size(); ++i) {
        if (i > 0) out += ", ";
        out += generics[i]->name;
        if (!generics[i]->bounds.empty()) {
            out += ": ";
            for (std::size_t b = 0; b < generics[i]->bounds.size(); ++b) {
                if (b > 0) out += " + ";
                out += generics[i]->bounds[b]->name;
            }
        }
    }
    out += '>';
    return out;
}

std::string_view short_fn_name(std::string_view qualified) {
    auto dot = qualified.rfind('.');
    return dot == std::string_view::npos ? qualified : qualified.substr(dot + 1);
}

std::string format_fn_signature(const Function* fn) {
    if (fn == nullptr) return {};
    std::string out;
    if (fn->kind == FnKind::Static) out += "static ";
    out += "fn ";
    out += short_fn_name(fn->name);
    out += format_generics(fn->generics);
    out += '(';
    for (std::size_t i = 0; i < fn->params.size(); ++i) {
        if (i > 0) out += ", ";
        out += format_param(fn->params[i]);
    }
    out += ')';
    if (fn->ret != nullptr && !fn->ret->is_void() && !fn->ret->is_error()) {
        out += " -> ";
        out += type_name(fn->ret);
    }
    return out;
}

struct BuiltinDoc {
    Builtin builtin;
    std::string_view label;
    std::string_view signature;
    std::vector<std::string_view> params;
    std::string_view doc;
};

const std::vector<BuiltinDoc>& builtin_docs() {
    static const std::vector<BuiltinDoc> docs = {
        {Builtin::Print, "print", "fn print(x: any)", {"x: any"}, "Print a value to the host output."},
        {Builtin::Assert, "assert", "fn assert(cond: bool, message: str = \"\")", {"cond: bool", "message: str = \"\""},
         "Panic if `cond` is false."},
        {Builtin::Panic, "panic", "fn panic(message: str)", {"message: str"}, "Abort the current tick with `message`."},
        {Builtin::Min, "min", "fn min<T>(a: T, b: T) -> T", {"a: T", "b: T"}, "Smaller of two numeric values."},
        {Builtin::Max, "max", "fn max<T>(a: T, b: T) -> T", {"a: T", "b: T"}, "Larger of two numeric values."},
        {Builtin::Abs, "abs", "fn abs<T>(x: T) -> T", {"x: T"}, "Absolute value."},
        {Builtin::Clamp, "clamp", "fn clamp<T>(x: T, lo: T, hi: T) -> T", {"x: T", "lo: T", "hi: T"},
         "Clamp `x` to `[lo, hi]`."},
        {Builtin::Sqrt, "sqrt", "fn sqrt(x: float) -> float", {"x: float"}, "IEEE-754 square root."},
        {Builtin::Floor, "floor", "fn floor(x: float) -> float", {"x: float"}, "Round toward negative infinity."},
        {Builtin::Ceil, "ceil", "fn ceil(x: float) -> float", {"x: float"}, "Round toward positive infinity."},
        {Builtin::Round, "round", "fn round(x: float) -> float", {"x: float"}, "Round to nearest integer."},
        {Builtin::Sin, "sin", "fn sin(x: float) -> float", {"x: float"}, "Deterministic sine of `x` (radians)."},
        {Builtin::Cos, "cos", "fn cos(x: float) -> float", {"x: float"}, "Deterministic cosine of `x` (radians)."},
        {Builtin::Tan, "tan", "fn tan(x: float) -> float", {"x: float"}, "Deterministic tangent of `x` (radians)."},
        {Builtin::Asin, "asin", "fn asin(x: float) -> float", {"x: float"}, "Deterministic arc sine in radians."},
        {Builtin::Acos, "acos", "fn acos(x: float) -> float", {"x: float"}, "Deterministic arc cosine in radians."},
        {Builtin::Atan, "atan", "fn atan(x: float) -> float", {"x: float"}, "Deterministic arc tangent in radians."},
        {Builtin::Atan2, "atan2", "fn atan2(y: float, x: float) -> float", {"y: float", "x: float"},
         "Deterministic two-argument arctangent in radians."},
        {Builtin::Exp, "exp", "fn exp(x: float) -> float", {"x: float"}, "Exponential function e^x."},
        {Builtin::Log, "log", "fn log(x: float) -> float", {"x: float"}, "Natural logarithm."},
        {Builtin::Log2, "log2", "fn log2(x: float) -> float", {"x: float"}, "Base-2 logarithm."},
        {Builtin::Log10, "log10", "fn log10(x: float) -> float", {"x: float"}, "Base-10 logarithm."},
        {Builtin::Pow, "pow", "fn pow(base: float, exp: float) -> float", {"base: float", "exp: float"}, "Raise base to exp power."},
        {Builtin::Sign, "sign", "fn sign<T>(x: T) -> T", {"x: T"}, "Sign of `x`: -1, 0, or 1."},
        {Builtin::Lerp, "lerp", "fn lerp(a: float, b: float, t: float) -> float", {"a: float", "b: float", "t: float"}, "Linear interpolation."},
        {Builtin::InverseLerp, "inverse_lerp", "fn inverse_lerp(a: float, b: float, v: float) -> float", {"a: float", "b: float", "v: float"}, "Inverse linear interpolation."},
        {Builtin::Remap, "remap", "fn remap(in_min: float, in_max: float, out_min: float, out_max: float, v: float) -> float", {"in_min: float", "in_max: float", "out_min: float", "out_max: float", "v: float"}, "Remap value from one range to another."},
        {Builtin::Smoothstep, "smoothstep", "fn smoothstep(edge0: float, edge1: float, x: float) -> float", {"edge0: float", "edge1: float", "x: float"}, "Hermite interpolation between edges."},
        {Builtin::Step, "step", "fn step(edge: float, x: float) -> float", {"edge: float", "x: float"}, "Step function (0 if x < edge, 1 otherwise)."},
        {Builtin::MoveToward, "move_toward", "fn move_toward(from: float, to: float, max_delta: float) -> float", {"from: float", "to: float", "max_delta: float"}, "Move from toward to by at most max_delta."},
        {Builtin::RadToDeg, "rad_to_deg", "fn rad_to_deg(rad: float) -> float", {"rad: float"}, "Convert radians to degrees."},
        {Builtin::DegToRad, "deg_to_rad", "fn deg_to_rad(deg: float) -> float", {"deg: float"}, "Convert degrees to radians."},
        {Builtin::Wrap, "wrap", "fn wrap(val: float, min: float, max: float) -> float", {"val: float", "min: float", "max: float"}, "Wrap value within range [min, max)."},
        {Builtin::WrapAngle, "wrap_angle", "fn wrap_angle(angle: float) -> float", {"angle: float"}, "Wrap angle to [-pi, pi]."},
        {Builtin::FromAngle, "from_angle", "fn from_angle(angle: float, length: float = 1.0) -> vec2",
         {"angle: float", "length: float = 1.0"}, "Unit or scaled `vec2` in direction `angle` (radians)."},
        // vecN methods
        {Builtin::VecLength, "length", "fn length() -> float", {}, "Euclidean length of the vector."},
        {Builtin::VecLengthSq, "length_squared", "fn length_squared() -> float", {}, "Squared length of the vector."},
        {Builtin::VecDot, "dot", "fn dot(other: vecN) -> float", {"other: vecN"}, "Dot product."},
        {Builtin::VecNormalized, "normalized", "fn normalized() -> vecN", {},
         "Unit vector in the same direction (or zero vector if length is 0)."},
        {Builtin::VecDistance, "distance", "fn distance(other: vecN) -> float", {"other: vecN"},
         "Distance to `other`."},
        {Builtin::VecDistanceSq, "distance_squared", "fn distance_squared() -> float", {"other: vecN"},
         "Squared distance to `other`."},
        {Builtin::VecLerp, "lerp", "fn lerp(to: vecN, t: float) -> vecN", {"to: vecN", "t: float"},
         "Linear interpolation toward `to` by `t`."},
        {Builtin::VecAbs, "abs", "fn abs() -> vecN", {}, "Component-wise absolute value."},
        {Builtin::VecMin, "min", "fn min(other: vecN) -> vecN", {"other: vecN"}, "Component-wise minimum."},
        {Builtin::VecMax, "max", "fn max(other: vecN) -> vecN", {"other: vecN"}, "Component-wise maximum."},
        {Builtin::VecClamp, "clamp", "fn clamp(lo: vecN, hi: vecN) -> vecN", {"lo: vecN", "hi: vecN"},
         "Component-wise clamp between `lo` and `hi`."},
        {Builtin::VecReflect, "reflect", "fn reflect(normal: vecN) -> vecN", {"normal: vecN"},
         "Reflect vector off plane with normal."},
        {Builtin::VecProject, "project", "fn project(onto: vecN) -> vecN", {"onto: vecN"},
         "Project vector onto another vector."},
        {Builtin::VecSlide, "slide", "fn slide(normal: vecN) -> vecN", {"normal: vecN"},
         "Slide vector along a surface with normal."},
        {Builtin::VecClampLength, "clamp_length", "fn clamp_length(min_len: float, max_len: float) -> vecN",
         {"min_len: float", "max_len: float"}, "Clamp vector length between min_len and max_len."},
        {Builtin::VecRotated, "rotated", "fn rotated(angle: float) -> vec2", {"angle: float"},
         "Rotate 2D vector by `angle` radians."},
        {Builtin::VecAngle, "angle", "fn angle() -> float", {}, "Direction angle in radians (`atan2(y, x)`)."},
        {Builtin::VecPerpendicular, "perpendicular", "fn perpendicular() -> vec2", {},
         "2D perpendicular vector (-y, x)."},
        {Builtin::VecAngleTo, "angle_to", "fn angle_to(target: vec2) -> float", {"target: vec2"},
         "Signed angle to target vector in radians."},
        {Builtin::VecSlerp, "slerp", "fn slerp(target: vec2, t: float) -> vec2", {"target: vec2", "t: float"},
         "Spherical linear interpolation along the circular arc between two vectors."},
        {Builtin::VecCross2, "cross", "fn cross(other: vec2) -> float", {"other: vec2"},
         "2D perpendicular dot product (cross product)."},
        {Builtin::VecCross3, "cross", "fn cross(other: vec3) -> vec3", {"other: vec3"},
         "3D cross product."},
        {Builtin::VecFloor, "floor", "fn floor() -> vecN", {}, "Component-wise floor."},
        {Builtin::VecCeil, "ceil", "fn ceil() -> vecN", {}, "Component-wise ceiling."},
        {Builtin::VecRound, "round", "fn round() -> vecN", {}, "Component-wise rounding (halves away from zero)."},
        {Builtin::VecFract, "fract", "fn fract() -> vecN", {}, "Component-wise fractional part (`x - floor(x)`)."},
        {Builtin::VecSign, "sign", "fn sign() -> vecN", {}, "Component-wise sign: -1, 0 or 1."},
        {Builtin::VecSnapped, "snapped", "fn snapped(step: float) -> vecN", {"step: float"},
         "Round each component to the nearest multiple of `step`."},
        {Builtin::VecBounce, "bounce", "fn bounce(normal: vecN) -> vecN", {"normal: vecN"},
         "Bounce off a surface with `normal` (the opposite of `reflect`)."},
        {Builtin::VecMoveToward, "move_toward", "fn move_toward(to: vecN, delta: float) -> vecN",
         {"to: vecN", "delta: float"}, "Move toward `to` by at most `delta`, never overshooting."},
        {Builtin::VecDirectionTo, "direction_to", "fn direction_to(to: vecN) -> vecN", {"to: vecN"},
         "Unit vector pointing at `to` (zero when equal)."},
        {Builtin::VecLimitLength, "limit_length", "fn limit_length(max_len: float) -> vecN", {"max_len: float"},
         "Shorten to `max_len` if longer; direction is kept."},
        {Builtin::WrappingAdd, "wrapping_add", "fn wrapping_add(a: T, b: T) -> T", {"a: T", "b: T"},
         "Integer addition that wraps around instead of panicking."},
        {Builtin::WrappingSub, "wrapping_sub", "fn wrapping_sub(a: T, b: T) -> T", {"a: T", "b: T"},
         "Integer subtraction that wraps around."},
        {Builtin::WrappingMul, "wrapping_mul", "fn wrapping_mul(a: T, b: T) -> T", {"a: T", "b: T"},
         "Integer multiplication that wraps around."},
        {Builtin::CheckedAdd, "checked_add", "fn checked_add(a: T, b: T) -> T?", {"a: T", "b: T"},
         "Integer addition, or null on overflow."},
        {Builtin::CheckedSub, "checked_sub", "fn checked_sub(a: T, b: T) -> T?", {"a: T", "b: T"},
         "Integer subtraction, or null on overflow."},
        {Builtin::CheckedMul, "checked_mul", "fn checked_mul(a: T, b: T) -> T?", {"a: T", "b: T"},
         "Integer multiplication, or null on overflow."},
        {Builtin::SaturatingAdd, "saturating_add", "fn saturating_add(a: T, b: T) -> T", {"a: T", "b: T"},
         "Integer addition clamped to the type's range."},
        {Builtin::SaturatingSub, "saturating_sub", "fn saturating_sub(a: T, b: T) -> T", {"a: T", "b: T"},
         "Integer subtraction clamped to the type's range."},
        {Builtin::SaturatingMul, "saturating_mul", "fn saturating_mul(a: T, b: T) -> T", {"a: T", "b: T"},
         "Integer multiplication clamped to the type's range."},
        // Task methods
        {Builtin::TaskCancel, "cancel", "fn cancel()", {}, "Cancel the running coroutine task."},
        {Builtin::TaskIsDone, "is_done", "fn is_done() -> bool", {}, "True once the coroutine has finished or been cancelled."},
        // Signal methods
        {Builtin::SignalConnect, "connect", "fn connect(handler: fn(...))", {"handler: fn(...)"},
         "Subscribe `handler` to this signal."},
        {Builtin::SignalDisconnect, "disconnect", "fn disconnect(handler: fn(...))", {"handler: fn(...)"},
         "Unsubscribe `handler` from this signal."},
        // List<T> methods
        {Builtin::ListNew, "List", "fn List<T>() -> List<T>", {}, "Construct an empty `List<T>`."},
        {Builtin::ListLen, "len", "fn len() -> int", {}, "Number of elements in the list."},
        {Builtin::ListPush, "push", "fn push(item: T)", {"item: T"}, "Append `item` to the end of the list."},
        {Builtin::ListPop, "pop", "fn pop() -> T", {}, "Remove and return the last element."},
        {Builtin::ListClear, "clear", "fn clear()", {}, "Remove all elements from the list."},
        {Builtin::ListInsert, "insert", "fn insert(index: int, item: T)", {"index: int", "item: T"},
         "Insert `item` at `index`."},
        {Builtin::ListRemove, "remove", "fn remove(index: int) -> T", {"index: int"},
         "Remove and return the element at `index`."},
        {Builtin::ListIsEmpty, "is_empty", "fn is_empty() -> bool", {}, "True when `len() == 0`."},
        {Builtin::ListContains, "contains", "fn contains(item: T) -> bool", {"item: T"},
         "True if `item` is in the list."},
        // str methods
        {Builtin::StrLen, "len", "fn len() -> int", {}, "Byte length of the string."},
        {Builtin::StrContains, "contains", "fn contains(sub: str) -> bool", {"sub: str"},
         "True if the string contains `sub`."},
        {Builtin::StrStartsWith, "starts_with", "fn starts_with(prefix: str) -> bool", {"prefix: str"},
         "True if the string begins with `prefix`."},
        {Builtin::StrEndsWith, "ends_with", "fn ends_with(suffix: str) -> bool", {"suffix: str"},
         "True if the string ends with `suffix`."},
        // Milestone 9.2a (LANGUAGE.md 9.16 - 9.19)
        {Builtin::ParseInt, "parse_int", "fn parse_int(s: str, radix: int = 10) -> int?", {"s: str", "radix: int = 10"}, "Parse an integer (optional sign, digits in `radix`); null if `s` is not one or does not fit in `int`."},
        {Builtin::ParseFloat, "parse_float", "fn parse_float(s: str) -> float?", {"s: str"}, "Parse a float (decimal or exponent form, `inf`, `nan`); null if `s` is not one."},
        {Builtin::ResultOk, "Ok", "fn Ok(value: T) -> Result<T, E>", {"value: T"}, "A successful `Result`; its type comes from where it is used."},
        {Builtin::ResultErr, "Err", "fn Err(error: E) -> Result<T, E>", {"error: E"}, "A failed `Result`; its type comes from where it is used."},
        {Builtin::MapNew, "Map", "fn Map<K, V>() -> Map<K, V>", {}, "An empty insertion-ordered hash map."},
        {Builtin::SetNew, "Set", "fn Set<T>() -> Set<T>", {}, "An empty insertion-ordered hash set."},
        {Builtin::DequeNew, "Deque", "fn Deque<T>() -> Deque<T>", {}, "An empty double-ended queue."},
        {Builtin::PQueueNew, "PriorityQueue", "fn PriorityQueue<T>(less: fn(T, T) -> bool = <) -> PriorityQueue<T>", {"less: fn(T, T) -> bool"}, "An empty min-heap; equal elements come out in push order."},
        {Builtin::StrBuilderNew, "StrBuilder", "fn StrBuilder() -> StrBuilder", {}, "An empty string builder."},
        {Builtin::ListSort, "sort", "fn sort(less: fn(T, T) -> bool = <)", {"less: fn(T, T) -> bool"}, "Stable sort, by `<` or by `less(a, b)`. NaN sorts last."},
        {Builtin::ListSortBy, "sort_by", "fn sort_by(key: fn(T) -> K)", {"key: fn(T) -> K"}, "Stable sort by a key computed once per element."},
        {Builtin::ListReverse, "reverse", "fn reverse()", {}, "Reverse in place."},
        {Builtin::ListIndexOf, "index_of", "fn index_of(item: T) -> int", {"item: T"}, "Index of the first equal element, or -1."},
        {Builtin::ListSlice, "slice", "fn slice(start: int, end: int = len) -> List<T>", {"start: int", "end: int"}, "Copy of `[start, end)`, clamped to the list."},
        {Builtin::ListExtend, "extend", "fn extend(other: List<T>)", {"other: List<T>"}, "Append every element of `other`."},
        {Builtin::ListSwap, "swap", "fn swap(i: int, j: int)", {"i: int", "j: int"}, "Swap two elements."},
        {Builtin::ListFirst, "first", "fn first() -> T?", {}, "The first element, or null when empty."},
        {Builtin::ListLast, "last", "fn last() -> T?", {}, "The last element, or null when empty."},
        {Builtin::ListMap, "map", "fn map(f: fn(T) -> U) -> List<U>", {"f: fn(T) -> U"}, "A new list of `f(x)` for each element."},
        {Builtin::ListFilter, "filter", "fn filter(keep: fn(T) -> bool) -> List<T>", {"keep: fn(T) -> bool"}, "A new list of the elements `keep` accepts."},
        {Builtin::ListReduce, "reduce", "fn reduce(init: U, f: fn(U, T) -> U) -> U", {"init: U", "f: fn(U, T) -> U"}, "Fold the elements left to right."},
        {Builtin::ListAny, "any", "fn any(pred: fn(T) -> bool) -> bool", {"pred: fn(T) -> bool"}, "True if `pred` holds for some element."},
        {Builtin::ListAll, "all", "fn all(pred: fn(T) -> bool) -> bool", {"pred: fn(T) -> bool"}, "True if `pred` holds for every element."},
        {Builtin::ListCount, "count", "fn count(pred: fn(T) -> bool) -> int", {"pred: fn(T) -> bool"}, "How many elements satisfy `pred`."},
        {Builtin::ListUnique, "unique", "fn unique() -> List<T>", {}, "First occurrences, in order."},
        {Builtin::ListShuffle, "shuffle", "fn shuffle()", {}, "Shuffle in place with the host's seeded random stream."},
        {Builtin::ListMin, "min", "fn min() -> T?", {}, "The smallest element (first of equals), or null when empty."},
        {Builtin::ListMax, "max", "fn max() -> T?", {}, "The largest element (first of equals), or null when empty."},
        {Builtin::ListSum, "sum", "fn sum() -> T", {}, "Sum of the numbers or vectors (zero when empty)."},
        {Builtin::ListZip, "zip", "fn zip(other: List<U>, f: fn(T, U) -> R) -> List<R>", {"other: List<U>", "f: fn(T, U) -> R"}, "`f` of paired elements, up to the shorter length."},
        {Builtin::ListFill, "fill", "fn fill(item: T)", {"item: T"}, "Set every element to `item`."},
        {Builtin::ListToList, "to_list", "fn to_list() -> List<T>", {}, "A new list with the same elements."},
        {Builtin::MapLen, "len", "fn len() -> int", {}, "Number of entries."},
        {Builtin::MapIsEmpty, "is_empty", "fn is_empty() -> bool", {}, "True when there are no entries."},
        {Builtin::MapHas, "has", "fn has(key: K) -> bool", {"key: K"}, "True if `key` has an entry."},
        {Builtin::MapGet, "get", "fn get(key: K) -> V?", {"key: K"}, "The value of `key`, or null."},
        {Builtin::MapGetOr, "get_or", "fn get_or(key: K, default: V) -> V", {"key: K", "default: V"}, "The value of `key`, or `default`."},
        {Builtin::MapRemove, "remove", "fn remove(key: K) -> bool", {"key: K"}, "Remove `key`; true if it was there."},
        {Builtin::MapClear, "clear", "fn clear()", {}, "Remove every entry."},
        {Builtin::MapKeys, "keys", "fn keys() -> List<K>", {}, "The keys, in insertion order."},
        {Builtin::MapValues, "values", "fn values() -> List<V>", {}, "The values, in insertion order."},
        {Builtin::SetLen, "len", "fn len() -> int", {}, "Number of elements."},
        {Builtin::SetIsEmpty, "is_empty", "fn is_empty() -> bool", {}, "True when empty."},
        {Builtin::SetAdd, "add", "fn add(item: T) -> bool", {"item: T"}, "Add `item`; true if it was new."},
        {Builtin::SetRemove, "remove", "fn remove(item: T) -> bool", {"item: T"}, "Remove `item`; true if it was there."},
        {Builtin::SetContains, "contains", "fn contains(item: T) -> bool", {"item: T"}, "True if `item` is in the set."},
        {Builtin::SetClear, "clear", "fn clear()", {}, "Remove every element."},
        {Builtin::SetToList, "to_list", "fn to_list() -> List<T>", {}, "The elements, in insertion order."},
        {Builtin::SetUnion, "union", "fn union(other: Set<T>) -> Set<T>", {"other: Set<T>"}, "Elements of either set."},
        {Builtin::SetIntersection, "intersection", "fn intersection(other: Set<T>) -> Set<T>", {"other: Set<T>"}, "Elements of both sets."},
        {Builtin::SetDifference, "difference", "fn difference(other: Set<T>) -> Set<T>", {"other: Set<T>"}, "Elements not in `other`."},
        {Builtin::DequeLen, "len", "fn len() -> int", {}, "Number of elements."},
        {Builtin::DequeIsEmpty, "is_empty", "fn is_empty() -> bool", {}, "True when empty."},
        {Builtin::DequePushBack, "push_back", "fn push_back(item: T)", {"item: T"}, "Add at the back."},
        {Builtin::DequePushFront, "push_front", "fn push_front(item: T)", {"item: T"}, "Add at the front."},
        {Builtin::DequePopBack, "pop_back", "fn pop_back() -> T", {}, "Remove and return the back element."},
        {Builtin::DequePopFront, "pop_front", "fn pop_front() -> T", {}, "Remove and return the front element."},
        {Builtin::DequeFront, "front", "fn front() -> T", {}, "The front element."},
        {Builtin::DequeBack, "back", "fn back() -> T", {}, "The back element."},
        {Builtin::DequeClear, "clear", "fn clear()", {}, "Remove every element."},
        {Builtin::DequeContains, "contains", "fn contains(item: T) -> bool", {"item: T"}, "True if `item` is in the deque."},
        {Builtin::DequeToList, "to_list", "fn to_list() -> List<T>", {}, "The elements, front to back."},
        {Builtin::PQPush, "push", "fn push(item: T)", {"item: T"}, "Add `item`."},
        {Builtin::PQPop, "pop", "fn pop() -> T", {}, "Remove and return the smallest element."},
        {Builtin::PQPeek, "peek", "fn peek() -> T", {}, "The smallest element."},
        {Builtin::PQLen, "len", "fn len() -> int", {}, "Number of elements."},
        {Builtin::PQIsEmpty, "is_empty", "fn is_empty() -> bool", {}, "True when empty."},
        {Builtin::PQClear, "clear", "fn clear()", {}, "Remove every element."},
        {Builtin::PQToList, "to_list", "fn to_list() -> List<T>", {}, "The elements in pop order."},
        {Builtin::SbAppend, "append", "fn append(x: any) -> StrBuilder", {"x: any"}, "Append `x` as text (like interpolation); returns the builder."},
        {Builtin::SbLen, "len", "fn len() -> int", {}, "Length in bytes."},
        {Builtin::SbClear, "clear", "fn clear()", {}, "Empty the builder."},
        {Builtin::SbToStr, "to_str", "fn to_str() -> str", {}, "The text built so far."},
        {Builtin::SbIsEmpty, "is_empty", "fn is_empty() -> bool", {}, "True when empty."},
        {Builtin::StrSplit, "split", "fn split(sep: str) -> List<str>", {"sep: str"}, "Split on every `sep` (empty parts kept)."},
        {Builtin::StrJoin, "join", "fn join(parts: List<str>) -> str", {"parts: List<str>"}, "Join `parts` with this string between them."},
        {Builtin::StrReplace, "replace", "fn replace(from: str, to: str) -> str", {"from: str", "to: str"}, "Replace every `from` with `to`."},
        {Builtin::StrTrim, "trim", "fn trim() -> str", {}, "Without leading and trailing ASCII whitespace."},
        {Builtin::StrTrimStart, "trim_start", "fn trim_start() -> str", {}, "Without leading ASCII whitespace."},
        {Builtin::StrTrimEnd, "trim_end", "fn trim_end() -> str", {}, "Without trailing ASCII whitespace."},
        {Builtin::StrToUpper, "to_upper", "fn to_upper() -> str", {}, "ASCII letters to upper case."},
        {Builtin::StrToLower, "to_lower", "fn to_lower() -> str", {}, "ASCII letters to lower case."},
        {Builtin::StrFind, "find", "fn find(sub: str, start: int = 0) -> int", {"sub: str", "start: int = 0"}, "Byte index of `sub` at or after `start`, or -1."},
        {Builtin::StrSubstr, "substr", "fn substr(start: int, length: int = len) -> str", {"start: int", "length: int"}, "Bytes `[start, start + length)`, clamped; panics if it splits a character."},
        {Builtin::StrByteAt, "byte_at", "fn byte_at(i: int) -> int", {"i: int"}, "The byte at index `i` (0..255)."},
        {Builtin::StrChars, "chars", "fn chars() -> List<str>", {}, "The characters (UTF-8 code points)."},
        {Builtin::StrCharCount, "char_count", "fn char_count() -> int", {}, "Number of characters (code points)."},
        {Builtin::StrRepeat, "repeat", "fn repeat(n: int) -> str", {"n: int"}, "The string `n` times."},
        {Builtin::StrPadLeft, "pad_left", "fn pad_left(width: int, fill: str = \" \") -> str", {"width: int", "fill: str = \" \""}, "Padded on the left to `width` characters."},
        {Builtin::StrPadRight, "pad_right", "fn pad_right(width: int, fill: str = \" \") -> str", {"width: int", "fill: str = \" \""}, "Padded on the right to `width` characters."},
        {Builtin::StrLines, "lines", "fn lines() -> List<str>", {}, "Lines without their `\\n` / `\\r\\n`."},
        {Builtin::StrIsEmpty, "is_empty", "fn is_empty() -> bool", {}, "True for \"\"."},
        {Builtin::StrParseInt, "parse_int", "fn parse_int(radix: int = 10) -> int?", {"radix: int = 10"}, "Parse as an integer, or null."},
        {Builtin::StrParseFloat, "parse_float", "fn parse_float() -> float?", {}, "Parse as a float, or null."},
        {Builtin::NumToHex, "to_hex", "fn to_hex() -> str", {}, "Lower-case hexadecimal text (integers)."},
        {Builtin::NumToRadix, "to_radix", "fn to_radix(base: int) -> str", {"base: int"}, "Text in base 2..36 (integers)."},
        {Builtin::NumToFixed, "to_fixed", "fn to_fixed(digits: int) -> str", {"digits: int"}, "Fixed-point text with `digits` decimals, rounded exactly (floats)."},
        {Builtin::ResIsOk, "is_ok", "fn is_ok() -> bool", {}, "True for `Ok`."},
        {Builtin::ResIsErr, "is_err", "fn is_err() -> bool", {}, "True for `Err`."},
        {Builtin::ResUnwrap, "unwrap", "fn unwrap() -> T", {}, "The `Ok` value; panics on `Err`."},
        {Builtin::ResUnwrapErr, "unwrap_err", "fn unwrap_err() -> E", {}, "The `Err` value; panics on `Ok`."},
        {Builtin::ResUnwrapOr, "unwrap_or", "fn unwrap_or(default: T) -> T", {"default: T"}, "The `Ok` value, or `default`."},
        {Builtin::ResExpect, "expect", "fn expect(message: str) -> T", {"message: str"}, "The `Ok` value; panics with `message` on `Err`."},
        {Builtin::ResOk, "ok", "fn ok() -> T?", {}, "The `Ok` value, or null."},
        {Builtin::ResErr, "err", "fn err() -> E?", {}, "The `Err` value, or null."},
        {Builtin::OptUnwrap, "unwrap", "fn unwrap() -> T", {}, "The value; panics on null."},
        {Builtin::OptIsSome, "is_some", "fn is_some() -> bool", {}, "True when not null."},
        {Builtin::OptIsNone, "is_none", "fn is_none() -> bool", {}, "True when null."},
        {Builtin::OptExpect, "expect", "fn expect(message: str) -> T", {"message: str"}, "The value; panics with `message` on null."},
    };
    return docs;
}

const BuiltinDoc* find_builtin_doc(Builtin b) {
    for (const auto& d : builtin_docs()) {
        if (d.builtin == b) return &d;
    }
    return nullptr;
}

// ---------------------------------------------------------------- symbol occurrences

enum class TargetKind : std::uint8_t {
    None,
    Class,
    Struct,
    Enum,
    Variant,
    Trait,
    Function,
    Field,
    Global,
    Local,
    GenericParam,
    Param,
    Builtin,
};

struct SymTarget {
    TargetKind kind = TargetKind::None;
    const void* ptr = nullptr;
    std::uint32_t sub_index = 0; // Variant index on EnumInfo, or Param index on Function
    Builtin builtin = Builtin::Count;

    bool operator==(const SymTarget& o) const {
        return kind == o.kind && ptr == o.ptr && sub_index == o.sub_index && builtin == o.builtin;
    }
    bool valid() const { return kind != TargetKind::None; }
};

struct Occurrence {
    Span span;               // identifier token span in the combined SourceFile
    SymTarget target;
    const Type* type = nullptr;
    bool is_decl = false;
    std::string hover_md;    // pre-built or lazy markdown
};

struct PartSlice {
    std::string name;        // URI for workspace files, or prelude filename
    std::string text;
    std::uint32_t base = 0;
    std::uint32_t end = 0;
    bool is_prelude = false;
};

struct Analysis {
    std::unique_ptr<SourceFile> source_ptr;
    Arena ast_arena;
    Module mod;
    Program program;
    std::vector<Diagnostic> diags;
    std::vector<PartSlice> slices;
    std::vector<Occurrence> occurrences;
    std::set<std::string> interned_names; // all `#name` literals seen across prelude + workspace

    const SourceFile& source() const { return *source_ptr; }

    const ClassInfo* find_class(std::string_view name) const {
        auto it = program.globals.find(name);
        if (it != program.globals.end() && it->second.kind == Program::SymKind::Class) {
            return static_cast<const ClassInfo*>(it->second.ptr);
        }
        return nullptr;
    }

    const StructInfo* find_struct(std::string_view name) const {
        auto it = program.globals.find(name);
        if (it != program.globals.end() && it->second.kind == Program::SymKind::Struct) {
            return static_cast<const StructInfo*>(it->second.ptr);
        }
        return nullptr;
    }

    const EnumInfo* find_enum(std::string_view name) const {
        auto it = program.globals.find(name);
        if (it != program.globals.end() && it->second.kind == Program::SymKind::Enum) {
            return static_cast<const EnumInfo*>(it->second.ptr);
        }
        return nullptr;
    }

    const TraitInfo* find_trait(std::string_view name) const {
        auto it = program.globals.find(name);
        if (it != program.globals.end() && it->second.kind == Program::SymKind::Trait) {
            return static_cast<const TraitInfo*>(it->second.ptr);
        }
        return nullptr;
    }

    const Function* find_fn(std::string_view name) const {
        auto it = program.globals.find(name);
        if (it != program.globals.end() && it->second.kind == Program::SymKind::Function) {
            return static_cast<const Function*>(it->second.ptr);
        }
        return nullptr;
    }

    const PartSlice* slice_for_uri(std::string_view uri) const {
        for (const auto& s : slices) {
            if (!s.is_prelude && s.name == uri) return &s;
        }
        return nullptr;
    }

    const PartSlice* slice_at(std::uint32_t offset) const {
        for (const auto& s : slices) {
            if (offset >= s.base && offset <= s.end) return &s;
        }
        return nullptr;
    }

    Span to_local_span(const PartSlice& s, Span combined) const {
        std::uint32_t st = combined.begin >= s.base ? combined.begin - s.base : 0;
        std::uint32_t en = combined.end >= s.base ? combined.end - s.base : st;
        st = std::min<std::uint32_t>(st, static_cast<std::uint32_t>(s.text.size()));
        en = std::min<std::uint32_t>(en, static_cast<std::uint32_t>(s.text.size()));
        return {st, en};
    }

    Range span_to_range(const PartSlice& s, Span combined) const {
        Span loc = to_local_span(s, combined);
        return {offset_to_pos(s.text, loc.begin), offset_to_pos(s.text, loc.end)};
    }

    std::optional<Location> span_to_location(Span combined) const {
        if (span_empty(combined)) return std::nullopt;
        const PartSlice* s = slice_at(combined.begin);
        if (s == nullptr || s->is_prelude) return std::nullopt;
        return Location{s->name, span_to_range(*s, combined)};
    }

    std::uint32_t uri_pos_to_offset(std::string_view uri, Position pos) const {
        const PartSlice* s = slice_for_uri(uri);
        if (s == nullptr) return 0;
        return s->base + pos_to_offset(s->text, pos);
    }

    std::optional<Span> target_decl_span(const SymTarget& t) const {
        switch (t.kind) {
            case TargetKind::Class: {
                auto* c = static_cast<const ClassInfo*>(t.ptr);
                return c->decl != nullptr ? c->decl->name.span : find_ident_span(source(), c->span, c->name);
            }
            case TargetKind::Struct: {
                auto* s = static_cast<const StructInfo*>(t.ptr);
                return s->decl != nullptr ? s->decl->name.span : find_ident_span(source(), s->span, s->name);
            }
            case TargetKind::Enum: {
                auto* e = static_cast<const EnumInfo*>(t.ptr);
                return e->decl != nullptr ? e->decl->name.span : find_ident_span(source(), e->span, e->name);
            }
            case TargetKind::Variant: {
                auto* e = static_cast<const EnumInfo*>(t.ptr);
                if (t.sub_index < e->variants.size()) {
                    if (e->decl != nullptr && t.sub_index < e->decl->variants.size()) {
                        return e->decl->variants[t.sub_index].name.span;
                    }
                    return find_ident_span(source(), e->variants[t.sub_index].span, e->variants[t.sub_index].name);
                }
                return std::nullopt;
            }
            case TargetKind::Trait: {
                auto* tr = static_cast<const TraitInfo*>(t.ptr);
                if (tr->decl != nullptr) return tr->decl->name.span;
                if (!span_empty(tr->span)) return find_ident_span(source(), tr->span, tr->name);
                return std::nullopt;
            }
            case TargetKind::Function: {
                auto* fn = static_cast<const Function*>(t.ptr);
                if (fn->decl != nullptr) return fn->decl->name.span;
                return find_ident_span(source(), fn->span, short_fn_name(fn->name));
            }
            case TargetKind::Field: {
                auto* f = static_cast<const FieldInfo*>(t.ptr);
                if (f->decl != nullptr) return f->decl->name.span;
                return find_ident_span(source(), f->span, f->name);
            }
            case TargetKind::Global: {
                auto* g = static_cast<const GlobalVar*>(t.ptr);
                if (g->decl != nullptr) return g->decl->name.span;
                return find_ident_span(source(), g->span, g->name);
            }
            case TargetKind::Local: {
                auto* l = static_cast<const LocalVar*>(t.ptr);
                return find_ident_span(source(), l->span, l->name);
            }
            case TargetKind::GenericParam: {
                auto* gp = static_cast<const GenericParamInfo*>(t.ptr);
                return find_ident_span(source(), gp->span, gp->name);
            }
            case TargetKind::Param: {
                auto* fn = static_cast<const Function*>(t.ptr);
                if (t.sub_index < fn->params.size()) {
                    const ParamInfo& p = fn->params[t.sub_index];
                    if (p.ast != nullptr && !span_empty(p.ast->name.span)) return p.ast->name.span;
                    return find_ident_span(source(), p.span, p.name);
                }
                return std::nullopt;
            }
            case TargetKind::None:
            case TargetKind::Builtin:
                return std::nullopt;
        }
        return std::nullopt;
    }

    std::string describe_target(const SymTarget& t, const Type* expr_type = nullptr) const {
        std::string code;
        std::string doc;
        switch (t.kind) {
            case TargetKind::Class: {
                auto* c = static_cast<const ClassInfo*>(t.ptr);
                if (c->manual) code += "manual ";
                code += "class ";
                code += c->name;
                code += format_generics(c->generics);
                if (c->base != nullptr) {
                    code += " extends ";
                    code += type_name(c->base_type != nullptr ? c->base_type : c->base->self_type);
                }
                if (!c->traits.empty()) {
                    code += " with ";
                    for (std::size_t i = 0; i < c->traits.size(); ++i) {
                        if (i > 0) code += ", ";
                        code += c->traits[i]->name;
                    }
                }
                doc = extract_doc_comment(source(), c->span);
                break;
            }
            case TargetKind::Struct: {
                auto* s = static_cast<const StructInfo*>(t.ptr);
                code += "struct ";
                code += s->name;
                code += format_generics(s->generics);
                if (!s->traits.empty()) {
                    code += " with ";
                    for (std::size_t i = 0; i < s->traits.size(); ++i) {
                        if (i > 0) code += ", ";
                        code += s->traits[i]->name;
                    }
                }
                doc = extract_doc_comment(source(), s->span);
                break;
            }
            case TargetKind::Enum: {
                auto* e = static_cast<const EnumInfo*>(t.ptr);
                code += "enum ";
                code += e->name;
                if (e->decl != nullptr && e->decl->backing != nullptr) {
                    code += ": ";
                    code += to_string(e->backing);
                }
                doc = extract_doc_comment(source(), e->span);
                break;
            }
            case TargetKind::Variant: {
                auto* e = static_cast<const EnumInfo*>(t.ptr);
                if (t.sub_index < e->variants.size()) {
                    const auto& v = e->variants[t.sub_index];
                    code += e->name;
                    code += '.';
                    code += v.name;
                    if (!v.field_types.empty()) {
                        code += '(';
                        for (std::size_t i = 0; i < v.field_types.size(); ++i) {
                            if (i > 0) code += ", ";
                            if (i < v.field_names.size() && !v.field_names[i].empty()) {
                                code += v.field_names[i];
                                code += ": ";
                            }
                            code += type_name(v.field_types[i]);
                        }
                        code += ')';
                    } else if (e->plain) {
                        code += std::format(" = {}", v.discriminant);
                    }
                    doc = extract_doc_comment(source(), v.span);
                }
                break;
            }
            case TargetKind::Trait: {
                auto* tr = static_cast<const TraitInfo*>(t.ptr);
                code += "trait ";
                code += tr->name;
                doc = extract_doc_comment(source(), tr->span);
                break;
            }
            case TargetKind::Function: {
                auto* fn = static_cast<const Function*>(t.ptr);
                code = format_fn_signature(fn);
                doc = extract_doc_comment(source(), fn->span);
                break;
            }
            case TargetKind::Field: {
                auto* f = static_cast<const FieldInfo*>(t.ptr);
                if (f->is_signal) {
                    code += "signal ";
                    code += f->name;
                    code += '(';
                    if (f->type != nullptr && f->type->tag == TypeTag::Signal) {
                        for (std::size_t i = 0; i < f->type->args.size(); ++i) {
                            if (i > 0) code += ", ";
                            if (i < f->signal_params.size() && !f->signal_params[i].empty()) {
                                code += f->signal_params[i];
                                code += ": ";
                            }
                            code += type_name(f->type->args[i]);
                        }
                    }
                    code += ')';
                } else {
                    if (f->exported) code += "@export ";
                    if (f->weak) code += "weak ";
                    code += "var ";
                    code += f->name;
                    if (f->type != nullptr) {
                        code += ": ";
                        code += type_name(f->type);
                    }
                }
                doc = extract_doc_comment(source(), f->span);
                break;
            }
            case TargetKind::Global: {
                auto* g = static_cast<const GlobalVar*>(t.ptr);
                code += g->is_const ? "const " : "var ";
                code += g->qualified.empty() ? std::string(g->name) : g->qualified;
                if (g->type != nullptr) {
                    code += ": ";
                    code += type_name(g->type);
                }
                if (g->is_const) {
                    switch (g->value.kind) {
                        case ConstValue::Kind::Int: code += std::format(" = {}", g->value.i); break;
                        case ConstValue::Kind::Float: code += std::format(" = {}", g->value.f); break;
                        case ConstValue::Kind::Bool: code += g->value.b ? " = true" : " = false"; break;
                        case ConstValue::Kind::Str: code += std::format(" = \"{}\"", g->value.s); break;
                        case ConstValue::Kind::Name: code += std::format(" = #{}", g->value.s); break;
                        default: break;
                    }
                }
                doc = extract_doc_comment(source(), g->span);
                break;
            }
            case TargetKind::Local: {
                auto* l = static_cast<const LocalVar*>(t.ptr);
                code += l->is_param ? "(param) " : (l->is_mutable ? "var " : "const ");
                code += l->name;
                const Type* ty = l->type != nullptr ? l->type : expr_type;
                if (ty != nullptr) {
                    code += ": ";
                    code += type_name(ty);
                }
                break;
            }
            case TargetKind::GenericParam: {
                auto* gp = static_cast<const GenericParamInfo*>(t.ptr);
                code += "type param ";
                code += gp->name;
                if (!gp->bounds.empty()) {
                    code += ": ";
                    for (std::size_t i = 0; i < gp->bounds.size(); ++i) {
                        if (i > 0) code += " + ";
                        code += gp->bounds[i]->name;
                    }
                }
                break;
            }
            case TargetKind::Param: {
                auto* fn = static_cast<const Function*>(t.ptr);
                if (t.sub_index < fn->params.size()) {
                    code = "(param) " + format_param(fn->params[t.sub_index]);
                }
                break;
            }
            case TargetKind::Builtin: {
                if (const auto* bd = find_builtin_doc(t.builtin)) {
                    code = std::string(bd->signature);
                    doc = std::string(bd->doc);
                } else {
                    code = std::format("fn {}", builtin_name(t.builtin));
                }
                break;
            }
            case TargetKind::None:
                if (expr_type != nullptr && !expr_type->is_error()) {
                    code = type_name(expr_type);
                }
                break;
        }
        if (code.empty()) return {};
        std::string md = "```pluma\n" + code + "\n```";
        if (!doc.empty()) {
            md += "\n\n" + doc;
        }
        return md;
    }
};

class OccurrenceBuilder {
public:
    explicit OccurrenceBuilder(Analysis& a) : a_(a) {}

    void run() {
        index_program_decls();
        for (const Decl* d : a_.mod.decls) {
            visit_ast_decl(d, {});
        }
        for (const auto& fn : a_.program.functions) {
            if (fn->body != nullptr) {
                visit_tir_block(fn->body);
            }
            for (const auto& p : fn->params) {
                if (p.default_value != nullptr) {
                    visit_tir_expr(p.default_value);
                }
            }
        }
        for (const auto& g : a_.program.global_vars) {
            if (g->init != nullptr) {
                visit_tir_expr(g->init);
            }
        }
        for (const auto& c : a_.program.classes) {
            for (const auto& f : c->fields) {
                if (f->owner == c.get() && f->init != nullptr) {
                    visit_tir_expr(f->init);
                }
            }
        }
        for (const auto& s : a_.program.structs) {
            for (const auto& f : s->fields) {
                if (f->owner == s.get() && f->init != nullptr) {
                    visit_tir_expr(f->init);
                }
            }
        }

        // Sort occurrences by span start, preferring tighter spans and richer targets.
        std::stable_sort(a_.occurrences.begin(), a_.occurrences.end(),
                         [](const Occurrence& x, const Occurrence& y) {
                             if (x.span.begin != y.span.begin) return x.span.begin < y.span.begin;
                             if (x.span.end != y.span.end) return x.span.end < y.span.end;
                             return x.is_decl > y.is_decl;
                         });
    }

private:
    Analysis& a_;

    void add(Span span, SymTarget target, const Type* type = nullptr, bool is_decl = false) {
        if (span_empty(span) || span.end > a_.source().text().size()) return;
        for (auto& existing : a_.occurrences) {
            if (existing.span.begin == span.begin && existing.span.end == span.end) {
                if (!existing.target.valid() && target.valid()) existing.target = target;
                if (existing.type == nullptr && type != nullptr) existing.type = type;
                if (is_decl) existing.is_decl = true;
                return;
            }
        }
        a_.occurrences.push_back(Occurrence{span, target, type, is_decl, {}});
    }

    SymTarget resolve_type_name(std::string_view name,
                                const std::vector<GenericParamInfo*>& generics) const {
        for (const GenericParamInfo* gp : generics) {
            if (gp->name == name) return {TargetKind::GenericParam, gp};
        }
        if (const ClassInfo* c = a_.find_class(name)) return {TargetKind::Class, c};
        if (const StructInfo* s = a_.find_struct(name)) return {TargetKind::Struct, s};
        if (const EnumInfo* e = a_.find_enum(name)) return {TargetKind::Enum, e};
        if (const TraitInfo* tr = a_.find_trait(name)) return {TargetKind::Trait, tr};
        return {};
    }

    void index_program_decls() {
        for (const auto& c : a_.program.classes) {
            Span sp = c->decl != nullptr ? c->decl->name.span : find_ident_span(a_.source(), c->span, c->name);
            add(sp, {TargetKind::Class, c.get()}, c->self_type, true);
            for (const auto* gp : c->generics) {
                add(find_ident_span(a_.source(), gp->span, gp->name), {TargetKind::GenericParam, gp}, nullptr, true);
            }
            for (const auto* f : c->fields) {
                if (f->owner != c.get()) continue;
                Span fsp = f->decl != nullptr ? f->decl->name.span : find_ident_span(a_.source(), f->span, f->name);
                add(fsp, {TargetKind::Field, f}, f->type, true);
            }
        }
        for (const auto& s : a_.program.structs) {
            Span sp = s->decl != nullptr ? s->decl->name.span : find_ident_span(a_.source(), s->span, s->name);
            add(sp, {TargetKind::Struct, s.get()}, s->self_type, true);
            for (const auto* gp : s->generics) {
                add(find_ident_span(a_.source(), gp->span, gp->name), {TargetKind::GenericParam, gp}, nullptr, true);
            }
            for (const auto* f : s->fields) {
                if (f->owner != s.get()) continue;
                Span fsp = f->decl != nullptr ? f->decl->name.span : find_ident_span(a_.source(), f->span, f->name);
                add(fsp, {TargetKind::Field, f}, f->type, true);
            }
        }
        for (const auto& e : a_.program.enums) {
            Span sp = e->decl != nullptr ? e->decl->name.span : find_ident_span(a_.source(), e->span, e->name);
            add(sp, {TargetKind::Enum, e.get()}, e->type, true);
            for (std::size_t i = 0; i < e->variants.size(); ++i) {
                Span vsp = (e->decl != nullptr && i < e->decl->variants.size())
                               ? e->decl->variants[i].name.span
                               : find_ident_span(a_.source(), e->variants[i].span, e->variants[i].name);
                add(vsp, {TargetKind::Variant, e.get(), static_cast<std::uint32_t>(i)}, e->type, true);
            }
        }
        for (const auto& tr : a_.program.traits) {
            if (tr->decl != nullptr) {
                add(tr->decl->name.span, {TargetKind::Trait, tr.get()}, nullptr, true);
            }
            for (const auto& [mname, fn] : tr->methods) {
                if (fn->decl != nullptr) {
                    add(fn->decl->name.span, {TargetKind::Function, fn}, nullptr, true);
                }
            }
        }
        for (const auto& fn : a_.program.functions) {
            if (fn->decl != nullptr && !span_empty(fn->decl->name.span)) {
                add(fn->decl->name.span, {TargetKind::Function, fn.get()}, nullptr, true);
            }
            for (const auto* gp : fn->generics) {
                add(find_ident_span(a_.source(), gp->span, gp->name), {TargetKind::GenericParam, gp}, nullptr, true);
            }
            std::size_t self_offset = fn->has_self() ? 1 : 0;
            for (std::size_t i = 0; i < fn->params.size(); ++i) {
                Span psp = (fn->params[i].ast != nullptr && !span_empty(fn->params[i].ast->name.span))
                               ? fn->params[i].ast->name.span
                               : find_ident_span(a_.source(), fn->params[i].span, fn->params[i].name);
                if (i + self_offset < fn->param_vars.size() && fn->param_vars[i + self_offset] != nullptr) {
                    LocalVar* lv = fn->param_vars[i + self_offset];
                    add(psp, {TargetKind::Local, lv}, lv->type, true);
                } else {
                    add(psp, {TargetKind::Param, fn.get(), static_cast<std::uint32_t>(i)}, fn->params[i].type, true);
                }
            }
        }
        for (const auto& g : a_.program.global_vars) {
            Span gsp = g->decl != nullptr ? g->decl->name.span : find_ident_span(a_.source(), g->span, g->name);
            add(gsp, {TargetKind::Global, g.get()}, g->type, true);
        }
    }

    void visit_ast_type(const TypeExpr* t, const std::vector<GenericParamInfo*>& generics) {
        if (t == nullptr) return;
        switch (t->kind) {
            case TypeKind::Named: {
                const auto* nt = as<NamedType>(t);
                SymTarget tgt = resolve_type_name(nt->name.name, generics);
                if (tgt.valid()) add(nt->name.span, tgt, nullptr, false);
                for (const TypeExpr* arg : nt->args) visit_ast_type(arg, generics);
                break;
            }
            case TypeKind::Array: {
                const auto* at = as<ArrayType>(t);
                visit_ast_type(at->element, generics);
                visit_ast_expr(at->size, generics, nullptr);
                break;
            }
            case TypeKind::Fn: {
                const auto* ft = as<FnType>(t);
                for (const TypeExpr* p : ft->params) visit_ast_type(p, generics);
                visit_ast_type(ft->ret, generics);
                break;
            }
            case TypeKind::Nullable: {
                const auto* nlt = as<NullableType>(t);
                visit_ast_type(nlt->inner, generics);
                break;
            }
            case TypeKind::Error:
                break;
        }
    }

    void visit_ast_decl(const Decl* d, std::vector<GenericParamInfo*> generics,
                        const RecordInfo* enclosing_rec = nullptr) {
        if (d == nullptr) return;
        switch (d->kind) {
            case DeclKind::Class: {
                const auto* cd = as<ClassDecl>(d);
                const ClassInfo* ci = a_.find_class(cd->name.name);
                if (ci != nullptr) generics = ci->generics;
                visit_ast_type(cd->extends, generics);
                for (const TypeExpr* tr : cd->traits) visit_ast_type(tr, generics);
                for (const GenericParam& gp : cd->generics) {
                    for (const TypeExpr* b : gp.bounds) visit_ast_type(b, generics);
                }
                for (const Decl* m : cd->members) visit_ast_decl(m, generics, ci);
                break;
            }
            case DeclKind::Struct: {
                const auto* sd = as<StructDecl>(d);
                const StructInfo* si = a_.find_struct(sd->name.name);
                if (si != nullptr) generics = si->generics;
                for (const TypeExpr* tr : sd->traits) visit_ast_type(tr, generics);
                for (const GenericParam& gp : sd->generics) {
                    for (const TypeExpr* b : gp.bounds) visit_ast_type(b, generics);
                }
                for (const Decl* m : sd->members) visit_ast_decl(m, generics, si);
                break;
            }
            case DeclKind::Trait: {
                const auto* td = as<TraitDecl>(d);
                for (const TypeExpr* st : td->supertraits) visit_ast_type(st, generics);
                for (const Decl* m : td->members) visit_ast_decl(m, generics, nullptr);
                break;
            }
            case DeclKind::Enum: {
                const auto* ed = as<EnumDecl>(d);
                visit_ast_type(ed->backing, generics);
                for (const EnumVariant& v : ed->variants) {
                    for (const Param& p : v.params) {
                        visit_ast_type(p.type, generics);
                        visit_ast_expr(p.default_value, generics, enclosing_rec);
                    }
                    visit_ast_expr(v.value, generics, enclosing_rec);
                }
                break;
            }
            case DeclKind::Fn: {
                const auto* fd = as<FnDecl>(d);
                for (const auto& fn : a_.program.functions) {
                    if (fn->decl == fd) {
                        for (auto* gp : fn->generics) generics.push_back(gp);
                        break;
                    }
                }
                for (const GenericParam& gp : fd->generics) {
                    for (const TypeExpr* b : gp.bounds) visit_ast_type(b, generics);
                }
                for (const Param& p : fd->params) {
                    visit_ast_type(p.type, generics);
                    visit_ast_expr(p.default_value, generics, enclosing_rec);
                }
                visit_ast_type(fd->ret, generics);
                visit_ast_stmt(fd->body, generics, enclosing_rec);
                break;
            }
            case DeclKind::Var: {
                const auto* vd = as<VarDecl>(d);
                visit_ast_type(vd->type, generics);
                visit_ast_expr(vd->init, generics, enclosing_rec);
                visit_ast_stmt(vd->getter, generics, enclosing_rec);
                visit_ast_stmt(vd->setter, generics, enclosing_rec);
                break;
            }
            case DeclKind::Const: {
                const auto* cd = as<ConstDecl>(d);
                visit_ast_type(cd->type, generics);
                visit_ast_expr(cd->value, generics, enclosing_rec);
                break;
            }
            case DeclKind::Signal: {
                const auto* sg = as<SignalDecl>(d);
                if (enclosing_rec != nullptr) {
                    if (auto it = enclosing_rec->field_map.find(sg->name.name); it != enclosing_rec->field_map.end()) {
                        add(sg->name.span, {TargetKind::Field, it->second}, it->second->type, true);
                    }
                }
                for (const Param& p : sg->params) {
                    visit_ast_type(p.type, generics);
                }
                break;
            }
            case DeclKind::Error:
                break;
        }
    }

    void visit_ast_pattern(const Pattern* p, const std::vector<GenericParamInfo*>& generics,
                           const RecordInfo* rec) {
        if (p == nullptr) return;
        switch (p->kind) {
            case PatternKind::Literal:
                visit_ast_expr(as<LiteralPattern>(p)->value, generics, rec);
                break;
            case PatternKind::Range: {
                const auto* rp = as<RangePattern>(p);
                visit_ast_expr(rp->lo, generics, rec);
                visit_ast_expr(rp->hi, generics, rec);
                break;
            }
            case PatternKind::Path: {
                const auto* pp = as<PathPattern>(p);
                if (pp->path.size() == 2) {
                    if (const EnumInfo* en = a_.find_enum(pp->path[0].name)) {
                        add(pp->path[0].span, {TargetKind::Enum, en}, en->type, false);
                        int vi = en->variant_index(pp->path[1].name);
                        if (vi >= 0) {
                            add(pp->path[1].span, {TargetKind::Variant, en, static_cast<std::uint32_t>(vi)},
                                en->type, false);
                        }
                    }
                } else if (pp->path.size() == 1) {
                    SymTarget tgt = resolve_type_name(pp->path[0].name, generics);
                    if (tgt.valid()) add(pp->path[0].span, tgt, nullptr, false);
                }
                for (const Pattern* arg : pp->args) visit_ast_pattern(arg, generics, rec);
                break;
            }
            case PatternKind::Or: {
                const auto* op = as<OrPattern>(p);
                for (const Pattern* alt : op->alternatives) visit_ast_pattern(alt, generics, rec);
                break;
            }
            case PatternKind::Wildcard:
            case PatternKind::Error:
                break;
        }
    }

    void visit_ast_stmt(const Stmt* s, const std::vector<GenericParamInfo*>& generics,
                        const RecordInfo* rec) {
        if (s == nullptr) return;
        switch (s->kind) {
            case StmtKind::Expr:
                visit_ast_expr(as<ExprStmt>(s)->expr, generics, rec);
                break;
            case StmtKind::Decl:
                visit_ast_decl(as<DeclStmt>(s)->decl, generics, rec);
                break;
            case StmtKind::Assign: {
                const auto* asg = as<AssignStmt>(s);
                visit_ast_expr(asg->target, generics, rec);
                visit_ast_expr(asg->value, generics, rec);
                break;
            }
            case StmtKind::Block: {
                const auto* blk = as<BlockStmt>(s);
                for (const Stmt* st : blk->stmts) visit_ast_stmt(st, generics, rec);
                break;
            }
            case StmtKind::If: {
                const auto* ifs = as<IfStmt>(s);
                for (const IfBranch& b : ifs->branches) {
                    visit_ast_expr(b.cond, generics, rec);
                    visit_ast_stmt(b.body, generics, rec);
                }
                visit_ast_stmt(ifs->else_body, generics, rec);
                break;
            }
            case StmtKind::Match: {
                const auto* ms = as<MatchStmt>(s);
                visit_ast_expr(ms->subject, generics, rec);
                for (const MatchArm& arm : ms->arms) {
                    visit_ast_pattern(arm.pattern, generics, rec);
                    visit_ast_expr(arm.guard, generics, rec);
                    visit_ast_stmt(arm.body, generics, rec);
                }
                break;
            }
            case StmtKind::For: {
                const auto* fs = as<ForStmt>(s);
                visit_ast_expr(fs->iterable, generics, rec);
                visit_ast_stmt(fs->body, generics, rec);
                break;
            }
            case StmtKind::Foreach: {
                const auto* fe = as<ForeachStmt>(s);
                visit_ast_expr(fe->pool, generics, rec);
                visit_ast_stmt(fe->body, generics, rec);
                break;
            }
            case StmtKind::While: {
                const auto* ws = as<WhileStmt>(s);
                visit_ast_expr(ws->cond, generics, rec);
                visit_ast_stmt(ws->body, generics, rec);
                break;
            }
            case StmtKind::Loop:
                visit_ast_stmt(as<LoopStmt>(s)->body, generics, rec);
                break;
            case StmtKind::Return:
                visit_ast_expr(as<ReturnStmt>(s)->value, generics, rec);
                break;
            case StmtKind::Defer:
                visit_ast_stmt(as<DeferStmt>(s)->body, generics, rec);
                break;
            case StmtKind::Unsafe:
                visit_ast_stmt(as<UnsafeStmt>(s)->body, generics, rec);
                break;
            case StmtKind::Emit: {
                const auto* em = as<EmitStmt>(s);
                if (rec != nullptr) {
                    if (auto it = rec->field_map.find(em->signal.name); it != rec->field_map.end()) {
                        add(em->signal.span, {TargetKind::Field, it->second}, it->second->type, false);
                    }
                }
                for (const Arg& a : em->args) visit_ast_expr(a.value, generics, rec);
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Yield:
            case StmtKind::Error:
                break;
        }
    }

    void visit_ast_expr(const Expr* e, const std::vector<GenericParamInfo*>& generics,
                        const RecordInfo* rec) {
        if (e == nullptr) return;
        switch (e->kind) {
            case ExprKind::Name: {
                const auto* nl = as<NameLit>(e);
                if (!nl->name.empty()) a_.interned_names.emplace(nl->name);
                break;
            }
            case ExprKind::String: {
                const auto* sl = as<StringLit>(e);
                for (const StringPart& pt : sl->parts) {
                    visit_ast_expr(pt.expr, generics, rec);
                }
                break;
            }
            case ExprKind::Identifier: {
                const auto* id = as<IdentExpr>(e);
                for (const TypeExpr* ta : id->type_args) visit_ast_type(ta, generics);
                SymTarget tgt = resolve_type_name(id->name, generics);
                if (tgt.valid()) {
                    Span sp = find_ident_span(a_.source(), id->span, id->name);
                    add(sp, tgt, nullptr, false);
                }
                break;
            }
            case ExprKind::ListLit: {
                for (const Expr* el : as<ListLit>(e)->elements) visit_ast_expr(el, generics, rec);
                break;
            }
            case ExprKind::MapLit: {
                for (const MapEntry& me : as<MapLit>(e)->entries) {
                    visit_ast_expr(me.key, generics, rec);
                    visit_ast_expr(me.value, generics, rec);
                }
                break;
            }
            case ExprKind::StructLit: {
                const auto* sl = as<StructLit>(e);
                visit_ast_type(sl->type, generics);
                const StructInfo* st = sl->type != nullptr ? a_.find_struct(sl->type->name.name) : nullptr;
                for (const FieldInit& fi : sl->fields) {
                    if (st != nullptr) {
                        if (auto fit = st->field_map.find(fi.name.name); fit != st->field_map.end()) {
                            add(fi.name.span, {TargetKind::Field, fit->second}, fit->second->type, false);
                        }
                    }
                    visit_ast_expr(fi.value, generics, rec);
                }
                break;
            }
            case ExprKind::Unary:
                visit_ast_expr(as<UnaryExpr>(e)->operand, generics, rec);
                break;
            case ExprKind::Binary: {
                const auto* b = as<BinaryExpr>(e);
                visit_ast_expr(b->lhs, generics, rec);
                visit_ast_expr(b->rhs, generics, rec);
                break;
            }
            case ExprKind::Range: {
                const auto* r = as<RangeExpr>(e);
                visit_ast_expr(r->lo, generics, rec);
                visit_ast_expr(r->hi, generics, rec);
                visit_ast_expr(r->step, generics, rec);
                break;
            }
            case ExprKind::Ternary: {
                const auto* t = as<TernaryExpr>(e);
                visit_ast_expr(t->cond, generics, rec);
                visit_ast_expr(t->then_expr, generics, rec);
                visit_ast_expr(t->else_expr, generics, rec);
                break;
            }
            case ExprKind::Is: {
                const auto* is = as<IsExpr>(e);
                visit_ast_expr(is->operand, generics, rec);
                visit_ast_pattern(is->pattern, generics, rec);
                break;
            }
            case ExprKind::As: {
                const auto* as_e = as<AsExpr>(e);
                visit_ast_expr(as_e->operand, generics, rec);
                visit_ast_type(as_e->type, generics);
                break;
            }
            case ExprKind::Call: {
                const auto* c = as<CallExpr>(e);
                visit_ast_expr(c->callee, generics, rec);
                for (const Arg& a : c->args) visit_ast_expr(a.value, generics, rec);
                break;
            }
            case ExprKind::Index: {
                const auto* ix = as<IndexExpr>(e);
                visit_ast_expr(ix->object, generics, rec);
                visit_ast_expr(ix->index, generics, rec);
                break;
            }
            case ExprKind::Member: {
                const auto* m = as<MemberExpr>(e);
                visit_ast_expr(m->object, generics, rec);
                for (const TypeExpr* ta : m->type_args) visit_ast_type(ta, generics);
                break;
            }
            case ExprKind::Try:
                visit_ast_expr(as<TryExpr>(e)->operand, generics, rec);
                break;
            case ExprKind::Lambda: {
                const auto* l = as<LambdaExpr>(e);
                for (const Param& p : l->params) {
                    visit_ast_type(p.type, generics);
                    visit_ast_expr(p.default_value, generics, rec);
                }
                visit_ast_type(l->ret, generics);
                visit_ast_stmt(l->body, generics, rec);
                visit_ast_expr(l->expr_body, generics, rec);
                break;
            }
            case ExprKind::New: {
                const auto* n = as<NewExpr>(e);
                visit_ast_type(n->type, generics);
                for (const Arg& a : n->args) visit_ast_expr(a.value, generics, rec);
                break;
            }
            case ExprKind::Alloc: {
                const auto* al = as<AllocExpr>(e);
                visit_ast_type(al->type, generics);
                for (const Arg& a : al->args) visit_ast_expr(a.value, generics, rec);
                break;
            }
            case ExprKind::Await:
                visit_ast_expr(as<AwaitExpr>(e)->operand, generics, rec);
                break;
            case ExprKind::Start:
                visit_ast_expr(as<StartExpr>(e)->operand, generics, rec);
                break;
            case ExprKind::Free:
                visit_ast_expr(as<FreeExpr>(e)->operand, generics, rec);
                break;
            default:
                break;
        }
    }

    // Extract the trailing member identifier span from an expression like `obj.member` or `obj.member(args)`.
    Span member_ident_span(Span full_span, Span receiver_span, std::string_view member_name) const {
        Span search = full_span;
        if (!span_empty(receiver_span) && receiver_span.end >= full_span.begin && receiver_span.end <= full_span.end) {
            search.begin = receiver_span.end;
        }
        return find_ident_span(a_.source(), search, member_name);
    }

    void visit_tir_pattern(const tir::Pattern* p) {
        if (p == nullptr) return;
        switch (p->kind) {
            case tir::PK::Bind: {
                const auto* bp = tir::as<tir::BindPat>(p);
                if (bp->var != nullptr) {
                    add(find_ident_span(a_.source(), bp->var->span, bp->var->name),
                        {TargetKind::Local, bp->var}, bp->var->type, true);
                }
                break;
            }
            case tir::PK::Variant: {
                const auto* vp = tir::as<tir::VariantPat>(p);
                if (vp->en != nullptr && vp->variant < vp->en->variants.size()) {
                    const auto& v = vp->en->variants[vp->variant];
                    add(find_ident_span(a_.source(), vp->span, v.name),
                        {TargetKind::Variant, vp->en, vp->variant}, vp->en->type, false);
                }
                for (const tir::Pattern* sub : vp->args) visit_tir_pattern(sub);
                break;
            }
            case tir::PK::Class: {
                const auto* cp = tir::as<tir::ClassPat>(p);
                if (cp->cls != nullptr) {
                    add(find_ident_span(a_.source(), cp->span, cp->cls->name),
                        {TargetKind::Class, cp->cls}, cp->cls->self_type, false);
                }
                if (cp->bind != nullptr) {
                    add(find_ident_span(a_.source(), cp->bind->span, cp->bind->name),
                        {TargetKind::Local, cp->bind}, cp->bind->type, true);
                }
                break;
            }
            case tir::PK::NonNull: {
                const auto* np = tir::as<tir::NonNullPat>(p);
                if (np->bind != nullptr) {
                    add(find_ident_span(a_.source(), np->bind->span, np->bind->name),
                        {TargetKind::Local, np->bind}, np->bind->type, true);
                }
                break;
            }
            case tir::PK::Or: {
                const auto* op = tir::as<tir::OrPat>(p);
                for (const tir::Pattern* alt : op->alternatives) visit_tir_pattern(alt);
                break;
            }
            default:
                break;
        }
    }

    void visit_tir_block(const tir::Block* b) {
        if (b == nullptr) return;
        for (const tir::Stmt* s : b->stmts) visit_tir_stmt(s);
    }

    void visit_tir_stmt(const tir::Stmt* s) {
        if (s == nullptr) return;
        switch (s->kind) {
            case tir::SK::Expr:
                visit_tir_expr(tir::as<tir::ExprStmt>(s)->expr);
                break;
            case tir::SK::Var: {
                const auto* vs = tir::as<tir::VarStmt>(s);
                if (vs->var != nullptr) {
                    add(find_ident_span(a_.source(), vs->var->span, vs->var->name),
                        {TargetKind::Local, vs->var}, vs->var->type, true);
                }
                visit_tir_expr(vs->init);
                break;
            }
            case tir::SK::Assign: {
                const auto* asg = tir::as<tir::Assign>(s);
                visit_tir_expr(asg->target);
                visit_tir_expr(asg->value);
                break;
            }
            case tir::SK::Block:
                visit_tir_block(tir::as<tir::Block>(s));
                break;
            case tir::SK::If: {
                const auto* ifs = tir::as<tir::If>(s);
                visit_tir_expr(ifs->cond);
                visit_tir_block(ifs->then_body);
                visit_tir_stmt(ifs->else_body);
                break;
            }
            case tir::SK::While: {
                const auto* ws = tir::as<tir::While>(s);
                visit_tir_expr(ws->cond);
                visit_tir_block(ws->body);
                break;
            }
            case tir::SK::Loop:
                visit_tir_block(tir::as<tir::Loop>(s)->body);
                break;
            case tir::SK::ForRange: {
                const auto* fr = tir::as<tir::ForRange>(s);
                if (fr->var != nullptr) {
                    add(find_ident_span(a_.source(), fr->var->span, fr->var->name),
                        {TargetKind::Local, fr->var}, fr->var->type, true);
                }
                visit_tir_expr(fr->lo);
                visit_tir_expr(fr->hi);
                visit_tir_expr(fr->step);
                visit_tir_block(fr->body);
                break;
            }
            case tir::SK::ForList: {
                const auto* fl = tir::as<tir::ForList>(s);
                if (fl->index != nullptr) {
                    add(find_ident_span(a_.source(), fl->index->span, fl->index->name),
                        {TargetKind::Local, fl->index}, fl->index->type, true);
                }
                if (fl->elem != nullptr) {
                    add(find_ident_span(a_.source(), fl->elem->span, fl->elem->name),
                        {TargetKind::Local, fl->elem}, fl->elem->type, true);
                }
                visit_tir_expr(fl->list);
                visit_tir_block(fl->body);
                break;
            }
            case tir::SK::Match: {
                const auto* ms = tir::as<tir::Match>(s);
                visit_tir_expr(ms->subject);
                for (const tir::MatchArm& arm : ms->arms) {
                    visit_tir_pattern(arm.pattern);
                    visit_tir_expr(arm.guard);
                    visit_tir_stmt(arm.body);
                }
                break;
            }
            case tir::SK::Return:
                visit_tir_expr(tir::as<tir::Return>(s)->value);
                break;
            case tir::SK::Defer:
                visit_tir_stmt(tir::as<tir::Defer>(s)->body);
                break;
            case tir::SK::Break:
            case tir::SK::Continue:
                break;
        }
    }

    void visit_tir_expr(const tir::Expr* e) {
        if (e == nullptr) return;
        switch (e->kind) {
            case tir::EK::Local: {
                const auto* l = tir::as<tir::Local>(e);
                if (l->var != nullptr && !span_empty(e->span)) {
                    add(find_ident_span(a_.source(), e->span, l->var->name),
                        {TargetKind::Local, l->var}, e->type, false);
                }
                break;
            }
            case tir::EK::Capture: {
                const auto* c = tir::as<tir::Capture>(e);
                if (c->var != nullptr && !span_empty(e->span)) {
                    add(find_ident_span(a_.source(), e->span, c->var->name),
                        {TargetKind::Local, c->var}, e->type, false);
                }
                break;
            }
            case tir::EK::Global: {
                const auto* g = tir::as<tir::Global>(e);
                if (g->var != nullptr && !span_empty(e->span)) {
                    add(find_ident_span(a_.source(), e->span, g->var->name),
                        {TargetKind::Global, g->var}, e->type, false);
                }
                break;
            }
            case tir::EK::FnRef: {
                const auto* fr = tir::as<tir::FnRef>(e);
                if (fr->fn != nullptr && !span_empty(e->span)) {
                    add(find_ident_span(a_.source(), e->span, short_fn_name(fr->fn->name)),
                        {TargetKind::Function, fr->fn}, e->type, false);
                }
                break;
            }
            case tir::EK::Unary:
                visit_tir_expr(tir::as<tir::Unary>(e)->operand);
                break;
            case tir::EK::Binary: {
                const auto* b = tir::as<tir::Binary>(e);
                visit_tir_expr(b->lhs);
                visit_tir_expr(b->rhs);
                break;
            }
            case tir::EK::Logical: {
                const auto* lg = tir::as<tir::Logical>(e);
                visit_tir_expr(lg->lhs);
                visit_tir_expr(lg->rhs);
                break;
            }
            case tir::EK::Coalesce: {
                const auto* co = tir::as<tir::Coalesce>(e);
                visit_tir_expr(co->lhs);
                visit_tir_expr(co->rhs);
                break;
            }
            case tir::EK::Ternary: {
                const auto* t = tir::as<tir::Ternary>(e);
                visit_tir_expr(t->cond);
                visit_tir_expr(t->then_expr);
                visit_tir_expr(t->else_expr);
                break;
            }
            case tir::EK::Cast:
                visit_tir_expr(tir::as<tir::Cast>(e)->operand);
                break;
            case tir::EK::Call: {
                const auto* c = tir::as<tir::Call>(e);
                if (c->fn != nullptr && !span_empty(e->span)) {
                    std::string_view mname = short_fn_name(c->fn->name);
                    Span msp = find_ident_span(a_.source(), e->span, mname);
                    add(msp, {TargetKind::Function, c->fn}, c->fn->ret, false);
                }
                for (const tir::Expr* arg : c->args) visit_tir_expr(arg);
                break;
            }
            case tir::EK::MethodCall: {
                const auto* mc = tir::as<tir::MethodCall>(e);
                bool implicit_self = (mc->receiver != nullptr &&
                                      a_.source().slice(mc->receiver->span) != "self" &&
                                      mc->receiver->kind == tir::EK::Local &&
                                      tir::as<tir::Local>(mc->receiver)->var != nullptr &&
                                      tir::as<tir::Local>(mc->receiver)->var->name == "self");
                if (!implicit_self) {
                    visit_tir_expr(mc->receiver);
                }
                if (mc->fn != nullptr && mc->fn->accessor_of != nullptr && !span_empty(e->span)) {
                    // A property read/assignment through its get/set: the field.
                    FieldInfo* fi = mc->fn->accessor_of;
                    Span recv_span = (!implicit_self && mc->receiver != nullptr) ? mc->receiver->span : Span{};
                    Span fsp = member_ident_span(e->span, recv_span, fi->name);
                    add(fsp, {TargetKind::Field, fi}, fi->type, false);
                } else if (mc->fn != nullptr && !span_empty(e->span)) {
                    std::string_view mname = short_fn_name(mc->fn->name);
                    Span recv_span = (!implicit_self && mc->receiver != nullptr) ? mc->receiver->span : Span{};
                    Span msp = member_ident_span(e->span, recv_span, mname);
                    add(msp, {TargetKind::Function, mc->fn}, mc->fn->ret, false);
                }
                for (const tir::Expr* arg : mc->args) visit_tir_expr(arg);
                break;
            }
            case tir::EK::DynMethodCall: {
                const auto* dm = tir::as<tir::DynMethodCall>(e);
                visit_tir_expr(dm->receiver);
                for (const tir::Expr* arg : dm->args) visit_tir_expr(arg);
                break;
            }
            case tir::EK::ClosureCall: {
                const auto* cc = tir::as<tir::ClosureCall>(e);
                visit_tir_expr(cc->callee);
                for (const tir::Expr* arg : cc->args) visit_tir_expr(arg);
                break;
            }
            case tir::EK::BuiltinCall: {
                const auto* bc = tir::as<tir::BuiltinCall>(e);
                for (const tir::Expr* arg : bc->args) visit_tir_expr(arg);
                if (bc->builtin != Builtin::Echo && !span_empty(e->span)) {
                    std::string_view bname = builtin_name(bc->builtin);
                    Span recv_span;
                    if (!bc->args.empty() && bc->builtin >= Builtin::VecLength && !is_free_builtin(bc->builtin)) {
                        recv_span = bc->args[0]->span;
                    }
                    Span bsp = member_ident_span(e->span, recv_span, bname);
                    SymTarget bt{TargetKind::Builtin, nullptr, 0, bc->builtin};
                    add(bsp, bt, e->type, false);
                }
                break;
            }
            case tir::EK::New: {
                const auto* nw = tir::as<tir::New>(e);
                if (nw->cls != nullptr && !span_empty(e->span)) {
                    Span csp = find_ident_span(a_.source(), e->span, nw->cls->name);
                    add(csp, {TargetKind::Class, nw->cls}, e->type, false);
                }
                for (const tir::Expr* arg : nw->args) visit_tir_expr(arg);
                break;
            }
            case tir::EK::StructNew: {
                const auto* sn = tir::as<tir::StructNew>(e);
                if (sn->st != nullptr && !span_empty(e->span)) {
                    Span ssp = find_ident_span(a_.source(), e->span, sn->st->name);
                    add(ssp, {TargetKind::Struct, sn->st}, e->type, false);
                }
                for (const tir::Expr* f : sn->fields) visit_tir_expr(f);
                break;
            }
            case tir::EK::EnumNew: {
                const auto* en = tir::as<tir::EnumNew>(e);
                if (en->en != nullptr && !span_empty(e->span)) {
                    Span esp = find_ident_span(a_.source(), e->span, en->en->name);
                    add(esp, {TargetKind::Enum, en->en}, e->type, false);
                    if (en->variant < en->en->variants.size()) {
                        std::string_view vname = en->en->variants[en->variant].name;
                        Span vsp = member_ident_span(e->span, esp, vname);
                        add(vsp, {TargetKind::Variant, en->en, en->variant}, e->type, false);
                    }
                }
                for (const tir::Expr* arg : en->args) visit_tir_expr(arg);
                break;
            }
            case tir::EK::Field: {
                const auto* fd = tir::as<tir::Field>(e);
                bool implicit_self = (fd->object != nullptr &&
                                      a_.source().slice(fd->object->span) != "self" &&
                                      fd->object->kind == tir::EK::Local &&
                                      tir::as<tir::Local>(fd->object)->var != nullptr &&
                                      tir::as<tir::Local>(fd->object)->var->name == "self");
                if (!implicit_self) {
                    visit_tir_expr(fd->object);
                }
                if (fd->field != nullptr && !span_empty(e->span)) {
                    Span obj_span = (!implicit_self && fd->object != nullptr) ? fd->object->span : Span{};
                    Span fsp = member_ident_span(e->span, obj_span, fd->field->name);
                    add(fsp, {TargetKind::Field, fd->field}, fd->field->type, false);
                }
                break;
            }
            case tir::EK::Index: {
                const auto* ix = tir::as<tir::Index>(e);
                visit_tir_expr(ix->object);
                visit_tir_expr(ix->index);
                break;
            }
            case tir::EK::Lambda: {
                const auto* lm = tir::as<tir::Lambda>(e);
                if (lm->fn != nullptr && lm->fn->body != nullptr) {
                    visit_tir_block(lm->fn->body);
                }
                break;
            }
            case tir::EK::ToStr:
                visit_tir_expr(tir::as<tir::ToStr>(e)->operand);
                break;
            case tir::EK::Concat: {
                for (const tir::Expr* pt : tir::as<tir::Concat>(e)->parts) visit_tir_expr(pt);
                break;
            }
            case tir::EK::Is: {
                const auto* is_e = tir::as<tir::Is>(e);
                visit_tir_expr(is_e->operand);
                visit_tir_pattern(is_e->pattern);
                break;
            }
            case tir::EK::Copy:
                visit_tir_expr(tir::as<tir::Copy>(e)->operand);
                break;
            case tir::EK::ListLit: {
                for (const tir::Expr* el : tir::as<tir::ListLit>(e)->elements) visit_tir_expr(el);
                break;
            }
            case tir::EK::SafeNav: {
                const auto* sn = tir::as<tir::SafeNav>(e);
                visit_tir_expr(sn->object);
                visit_tir_expr(sn->body);
                break;
            }
            case tir::EK::VecNew: {
                for (const tir::Expr* pt : tir::as<tir::VecNew>(e)->parts) visit_tir_expr(pt);
                break;
            }
            case tir::EK::Swizzle:
                visit_tir_expr(tir::as<tir::Swizzle>(e)->operand);
                break;
            case tir::EK::VecWith: {
                const auto* vw = tir::as<tir::VecWith>(e);
                visit_tir_expr(vw->operand);
                visit_tir_expr(vw->value);
                break;
            }
            case tir::EK::Await:
                visit_tir_expr(tir::as<tir::Await>(e)->operand);
                break;
            case tir::EK::Free:
                visit_tir_expr(tir::as<tir::Free>(e)->operand);
                break;
            case tir::EK::Try:
                visit_tir_expr(tir::as<tir::Try>(e)->operand);
                break;
            case tir::EK::BoundMethod:
                visit_tir_expr(tir::as<tir::BoundMethod>(e)->receiver);
                break;
            case tir::EK::Start: {
                const auto* st = tir::as<tir::Start>(e);
                visit_tir_expr(st->call);
                visit_tir_expr(st->owner);
                break;
            }
            case tir::EK::Const:
            case tir::EK::Default:
            case tir::EK::Hole:
                break;
        }
    }
};

// Discover all `.plm` files in `root_path` (sorted for determinism) and merge
// with `open_docs` (in-memory edits override disk files).
std::map<std::string, std::string> collect_workspace_files(
    std::string_view root_path,
    const std::map<std::string, std::string>& open_docs) {
    std::map<std::string, std::string> files = open_docs;
    if (!root_path.empty()) {
        std::error_code ec;
        fs::path root(root_path);
        if (fs::exists(root, ec) && fs::is_directory(root, ec)) {
            for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;
                if (entry.path().extension() != ".plm") continue;
                std::string uri = path_to_uri(entry.path().string());
                if (files.find(uri) == files.end()) {
                    if (auto text = slurp_file(entry.path())) {
                        files.emplace(std::move(uri), std::move(*text));
                    }
                }
            }
        }
    }
    return files;
}

std::unique_ptr<Analysis> build_analysis(
    const ServerOptions& options,
    const std::map<std::string, std::string>& open_docs,
    std::string_view override_uri = {},
    std::optional<std::string> override_text = std::nullopt) {
    auto a = std::make_unique<Analysis>();
    auto ws_files = collect_workspace_files(options.root_path, open_docs);
    if (!override_uri.empty() && override_text.has_value()) {
        ws_files[std::string(override_uri)] = *override_text;
    }

    std::vector<SourceFile::Part> parts;
    parts.reserve(options.preludes.size() + ws_files.size());
    std::uint32_t cursor = 0;

    std::set<std::string, std::less<>> native_names = options.native_names;
    for (const auto& pre : options.preludes) {
        std::uint32_t len = static_cast<std::uint32_t>(pre.text.size());
        a->slices.push_back(PartSlice{pre.path, pre.text, cursor, cursor + len, true});
        parts.push_back(SourceFile::Part{pre.path, pre.text});
        cursor += len + 1;
    }

    for (const auto& [uri, text] : ws_files) {
        std::uint32_t len = static_cast<std::uint32_t>(text.size());
        a->slices.push_back(PartSlice{uri, text, cursor, cursor + len, false});
        parts.push_back(SourceFile::Part{uri, text});
        cursor += len + 1;
    }

    a->source_ptr = SourceFile::combine("<workspace>", parts);
    DiagnosticSink sink(*a->source_ptr);
    a->mod = parse_module(*a->source_ptr, a->ast_arena, sink);

    // If the host provided preludes without an explicit native_names set,
    // accept any `@native` declared in the preludes so prelude declarations
    // never produce false "unknown @native" errors.
    if (native_names.empty() && !options.preludes.empty()) {
        std::uint32_t prelude_end = 0;
        for (const auto& s : a->slices) {
            if (s.is_prelude) prelude_end = s.end;
        }
        for (const Decl* d : a->mod.decls) {
            if (d == nullptr || d->span.begin > prelude_end) continue;
            if (const auto* fd = as<FnDecl>(d)) {
                native_names.emplace(fd->name.name);
            } else if (const auto* cd = as<ClassDecl>(d)) {
                for (const Decl* m : cd->members) {
                    if (m != nullptr && m->kind == DeclKind::Fn) {
                        native_names.emplace(std::string(cd->name.name) + "." + std::string(m->name.name));
                    }
                }
            } else if (const auto* sd = as<StructDecl>(d)) {
                for (const Decl* m : sd->members) {
                    if (m != nullptr && m->kind == DeclKind::Fn) {
                        native_names.emplace(std::string(sd->name.name) + "." + std::string(m->name.name));
                    }
                }
            }
        }
    }

    a->program.native_names = std::move(native_names);
    CheckOptions chk_opts;
    chk_opts.keep_on_error = true;
    check_module(a->program, *a->source_ptr, a->mod, sink, chk_opts);

    a->diags = sink.all();
    if (options.check_style) {
        DiagnosticSink style_sink(*a->source_ptr);
        check_style(*a->source_ptr, a->mod, style_sink);
        for (Diagnostic d : style_sink.all()) {
            const PartSlice* sl = a->slice_at(d.span.begin);
            if (sl != nullptr && !sl->is_prelude) {
                a->diags.push_back(std::move(d));
            }
        }
    }

    OccurrenceBuilder builder(*a);
    builder.run();
    return a;
}

const Occurrence* find_occurrence_at(const Analysis& a, std::uint32_t offset) {
    const Occurrence* best = nullptr;
    for (const auto& occ : a.occurrences) {
        if (offset >= occ.span.begin && offset <= occ.span.end) {
            if (best == nullptr ||
                (occ.span.end - occ.span.begin) < (best->span.end - best->span.begin) ||
                (!best->target.valid() && occ.target.valid())) {
                best = &occ;
            }
        }
    }
    return best;
}

// Find the type of the expression in `[begin, end)` within the TIR.
const Type* find_tir_expr_type(const Analysis& a, Span target_span) {
    const Type* best_type = nullptr;
    std::uint32_t best_len = ~0u;

    auto check_e = [&](auto& self, const tir::Expr* e) -> void {
        if (e == nullptr) return;
        if (!span_empty(e->span) && e->type != nullptr && !e->type->is_error()) {
            if (e->span.begin <= target_span.begin && e->span.end >= target_span.end) {
                std::uint32_t len = e->span.end - e->span.begin;
                if (len <= best_len) {
                    best_len = len;
                    best_type = e->type;
                }
            }
        }
        switch (e->kind) {
            case tir::EK::Unary: self(self, tir::as<tir::Unary>(e)->operand); break;
            case tir::EK::Binary: {
                const auto* b = tir::as<tir::Binary>(e);
                self(self, b->lhs);
                self(self, b->rhs);
                break;
            }
            case tir::EK::Logical: {
                const auto* lg = tir::as<tir::Logical>(e);
                self(self, lg->lhs);
                self(self, lg->rhs);
                break;
            }
            case tir::EK::Coalesce: {
                const auto* co = tir::as<tir::Coalesce>(e);
                self(self, co->lhs);
                self(self, co->rhs);
                break;
            }
            case tir::EK::Ternary: {
                const auto* t = tir::as<tir::Ternary>(e);
                self(self, t->cond);
                self(self, t->then_expr);
                self(self, t->else_expr);
                break;
            }
            case tir::EK::Cast: self(self, tir::as<tir::Cast>(e)->operand); break;
            case tir::EK::Call:
                for (const tir::Expr* arg : tir::as<tir::Call>(e)->args) self(self, arg);
                break;
            case tir::EK::MethodCall: {
                const auto* mc = tir::as<tir::MethodCall>(e);
                if (mc->receiver != nullptr && mc->receiver->span != e->span) {
                    self(self, mc->receiver);
                }
                for (const tir::Expr* arg : mc->args) self(self, arg);
                break;
            }
            case tir::EK::DynMethodCall: {
                const auto* dm = tir::as<tir::DynMethodCall>(e);
                self(self, dm->receiver);
                for (const tir::Expr* arg : dm->args) self(self, arg);
                break;
            }
            case tir::EK::ClosureCall: {
                const auto* cc = tir::as<tir::ClosureCall>(e);
                self(self, cc->callee);
                for (const tir::Expr* arg : cc->args) self(self, arg);
                break;
            }
            case tir::EK::BuiltinCall:
                for (const tir::Expr* arg : tir::as<tir::BuiltinCall>(e)->args) self(self, arg);
                break;
            case tir::EK::New:
                for (const tir::Expr* arg : tir::as<tir::New>(e)->args) self(self, arg);
                break;
            case tir::EK::StructNew:
                for (const tir::Expr* f : tir::as<tir::StructNew>(e)->fields) self(self, f);
                break;
            case tir::EK::EnumNew:
                for (const tir::Expr* arg : tir::as<tir::EnumNew>(e)->args) self(self, arg);
                break;
            case tir::EK::Field: {
                const auto* fd = tir::as<tir::Field>(e);
                if (fd->object != nullptr && fd->object->span != e->span) {
                    self(self, fd->object);
                }
                break;
            }
            case tir::EK::Index: {
                const auto* ix = tir::as<tir::Index>(e);
                self(self, ix->object);
                self(self, ix->index);
                break;
            }
            case tir::EK::ToStr: self(self, tir::as<tir::ToStr>(e)->operand); break;
            case tir::EK::Concat:
                for (const tir::Expr* pt : tir::as<tir::Concat>(e)->parts) self(self, pt);
                break;
            case tir::EK::Is: self(self, tir::as<tir::Is>(e)->operand); break;
            case tir::EK::Copy: self(self, tir::as<tir::Copy>(e)->operand); break;
            case tir::EK::ListLit:
                for (const tir::Expr* el : tir::as<tir::ListLit>(e)->elements) self(self, el);
                break;
            case tir::EK::SafeNav: {
                const auto* sn = tir::as<tir::SafeNav>(e);
                self(self, sn->object);
                self(self, sn->body);
                break;
            }
            case tir::EK::VecNew:
                for (const tir::Expr* pt : tir::as<tir::VecNew>(e)->parts) self(self, pt);
                break;
            case tir::EK::Swizzle: self(self, tir::as<tir::Swizzle>(e)->operand); break;
            case tir::EK::VecWith: {
                const auto* vw = tir::as<tir::VecWith>(e);
                self(self, vw->operand);
                self(self, vw->value);
                break;
            }
            case tir::EK::Await: self(self, tir::as<tir::Await>(e)->operand); break;
            case tir::EK::Free: self(self, tir::as<tir::Free>(e)->operand); break;
            case tir::EK::Try: self(self, tir::as<tir::Try>(e)->operand); break;
            case tir::EK::BoundMethod: self(self, tir::as<tir::BoundMethod>(e)->receiver); break;
            case tir::EK::Start: {
                const auto* st = tir::as<tir::Start>(e);
                self(self, st->call);
                self(self, st->owner);
                break;
            }
            default: break;
        }
    };

    auto check_s = [&](auto& self_s, const tir::Stmt* s) -> void {
        if (s == nullptr) return;
        switch (s->kind) {
            case tir::SK::Expr: check_e(check_e, tir::as<tir::ExprStmt>(s)->expr); break;
            case tir::SK::Var: check_e(check_e, tir::as<tir::VarStmt>(s)->init); break;
            case tir::SK::Assign: {
                const auto* asg = tir::as<tir::Assign>(s);
                check_e(check_e, asg->target);
                check_e(check_e, asg->value);
                break;
            }
            case tir::SK::Block:
                for (const tir::Stmt* st : tir::as<tir::Block>(s)->stmts) self_s(self_s, st);
                break;
            case tir::SK::If: {
                const auto* ifs = tir::as<tir::If>(s);
                check_e(check_e, ifs->cond);
                if (ifs->then_body != nullptr) {
                    for (const tir::Stmt* st : ifs->then_body->stmts) self_s(self_s, st);
                }
                self_s(self_s, ifs->else_body);
                break;
            }
            case tir::SK::While: {
                const auto* ws = tir::as<tir::While>(s);
                check_e(check_e, ws->cond);
                if (ws->body != nullptr) {
                    for (const tir::Stmt* st : ws->body->stmts) self_s(self_s, st);
                }
                break;
            }
            case tir::SK::Loop:
                if (const auto* lp = tir::as<tir::Loop>(s); lp->body != nullptr) {
                    for (const tir::Stmt* st : lp->body->stmts) self_s(self_s, st);
                }
                break;
            case tir::SK::ForRange: {
                const auto* fr = tir::as<tir::ForRange>(s);
                check_e(check_e, fr->lo);
                check_e(check_e, fr->hi);
                check_e(check_e, fr->step);
                if (fr->body != nullptr) {
                    for (const tir::Stmt* st : fr->body->stmts) self_s(self_s, st);
                }
                break;
            }
            case tir::SK::ForList: {
                const auto* fl = tir::as<tir::ForList>(s);
                check_e(check_e, fl->list);
                if (fl->body != nullptr) {
                    for (const tir::Stmt* st : fl->body->stmts) self_s(self_s, st);
                }
                break;
            }
            case tir::SK::Match: {
                const auto* ms = tir::as<tir::Match>(s);
                check_e(check_e, ms->subject);
                for (const tir::MatchArm& arm : ms->arms) {
                    check_e(check_e, arm.guard);
                    self_s(self_s, arm.body);
                }
                break;
            }
            case tir::SK::Return: check_e(check_e, tir::as<tir::Return>(s)->value); break;
            case tir::SK::Defer: self_s(self_s, tir::as<tir::Defer>(s)->body); break;
            default: break;
        }
    };

    for (const auto& fn : a.program.functions) {
        if (fn->body != nullptr) {
            for (const tir::Stmt* st : fn->body->stmts) check_s(check_s, st);
        }
    }
    for (const auto& g : a.program.global_vars) check_e(check_e, g->init);
    return best_type;
}

void add_type_completions(const Analysis& a, const Type* raw_type, bool safe_nav,
                          std::map<std::string, CompletionItem>& items) {
    if (raw_type == nullptr) return;
    const Type* t = safe_nav ? a.program.types.non_null(raw_type) : raw_type;
    if (t->tag == TypeTag::Nullable) {
        // Auto-unwrapped or nullable: still show underlying members for convenience.
        t = t->args[0];
    }

    auto put = [&](CompletionItem item) {
        items.try_emplace(item.label, std::move(item));
    };

    switch (t->tag) {
        case TypeTag::Class: {
            for (const ClassInfo* c = t->cls(); c != nullptr; c = c->base) {
                for (const FieldInfo* f : c->fields) {
                    if (f->owner != c) continue;
                    put(CompletionItem{
                        std::string(f->name),
                        f->is_signal ? CompletionKind::Event : CompletionKind::Field,
                        f->type != nullptr ? type_name(f->type) : "",
                        extract_doc_comment(a.source(), f->span),
                        std::string(f->name),
                    });
                }
                for (const auto& [mname, fn] : c->methods) {
                    if (fn->kind == FnKind::Static || mname == "init") continue;
                    put(CompletionItem{
                        std::string(mname),
                        CompletionKind::Method,
                        format_fn_signature(fn),
                        extract_doc_comment(a.source(), fn->span),
                        std::string(mname),
                    });
                }
            }
            break;
        }
        case TypeTag::Struct: {
            const StructInfo* s = t->st();
            if (s != nullptr) {
                for (const FieldInfo* f : s->fields) {
                    put(CompletionItem{
                        std::string(f->name),
                        CompletionKind::Field,
                        f->type != nullptr ? type_name(f->type) : "",
                        extract_doc_comment(a.source(), f->span),
                        std::string(f->name),
                    });
                }
                for (const auto& [mname, fn] : s->methods) {
                    if (fn->kind == FnKind::Static) continue;
                    put(CompletionItem{
                        std::string(mname),
                        CompletionKind::Method,
                        format_fn_signature(fn),
                        extract_doc_comment(a.source(), fn->span),
                        std::string(mname),
                    });
                }
            }
            break;
        }
        case TypeTag::Trait: {
            const TraitInfo* tr = t->tr();
            if (tr != nullptr) {
                for (const auto& [mname, fn] : tr->methods) {
                    put(CompletionItem{
                        std::string(mname),
                        CompletionKind::Method,
                        format_fn_signature(fn),
                        extract_doc_comment(a.source(), fn->span),
                        std::string(mname),
                    });
                }
            }
            break;
        }
        case TypeTag::Vec: {
            static constexpr std::string_view comps[4] = {"x", "y", "z", "w"};
            for (std::uint32_t i = 0; i < t->dim && i < 4; ++i) {
                put(CompletionItem{std::string(comps[i]), CompletionKind::Field, "float",
                                   "Vector component.", std::string(comps[i])});
            }
            for (Builtin b : {Builtin::VecLength, Builtin::VecLengthSq, Builtin::VecDot,
                              Builtin::VecNormalized, Builtin::VecDistance, Builtin::VecDistanceSq,
                              Builtin::VecLerp, Builtin::VecAbs, Builtin::VecMin, Builtin::VecMax,
                              Builtin::VecClamp, Builtin::VecReflect, Builtin::VecProject,
                              Builtin::VecSlide, Builtin::VecClampLength, Builtin::VecRotated,
                              Builtin::VecAngle, Builtin::VecPerpendicular, Builtin::VecAngleTo,
                              Builtin::VecSlerp, Builtin::VecCross2, Builtin::VecCross3,
                              Builtin::VecFloor, Builtin::VecCeil, Builtin::VecRound, Builtin::VecFract,
                              Builtin::VecSign, Builtin::VecSnapped, Builtin::VecBounce,
                              Builtin::VecMoveToward, Builtin::VecDirectionTo, Builtin::VecLimitLength}) {
                if ((b == Builtin::VecRotated || b == Builtin::VecAngle ||
                     b == Builtin::VecPerpendicular || b == Builtin::VecAngleTo ||
                     b == Builtin::VecSlerp || b == Builtin::VecCross2) && t->dim != 2) continue;
                if (b == Builtin::VecCross3 && t->dim != 3) continue;
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::List: {
            for (Builtin b : {Builtin::ListLen, Builtin::ListPush, Builtin::ListPop,
                              Builtin::ListClear, Builtin::ListInsert, Builtin::ListRemove,
                              Builtin::ListIsEmpty, Builtin::ListContains, Builtin::ListSort, Builtin::ListSortBy, Builtin::ListReverse, Builtin::ListIndexOf, Builtin::ListSlice, Builtin::ListExtend, Builtin::ListSwap, Builtin::ListFirst, Builtin::ListLast, Builtin::ListMap, Builtin::ListFilter, Builtin::ListReduce, Builtin::ListAny, Builtin::ListAll, Builtin::ListCount, Builtin::ListUnique, Builtin::ListShuffle, Builtin::ListMin, Builtin::ListMax, Builtin::ListSum, Builtin::ListZip, Builtin::ListFill, Builtin::ListToList}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Str: {
            for (Builtin b : {Builtin::StrLen, Builtin::StrContains,
                              Builtin::StrStartsWith, Builtin::StrEndsWith, Builtin::StrSplit, Builtin::StrJoin, Builtin::StrReplace, Builtin::StrTrim, Builtin::StrTrimStart, Builtin::StrTrimEnd, Builtin::StrToUpper, Builtin::StrToLower, Builtin::StrFind, Builtin::StrSubstr, Builtin::StrByteAt, Builtin::StrChars, Builtin::StrCharCount, Builtin::StrRepeat, Builtin::StrPadLeft, Builtin::StrPadRight, Builtin::StrLines, Builtin::StrIsEmpty, Builtin::StrParseInt, Builtin::StrParseFloat}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Task: {
            for (Builtin b : {Builtin::TaskCancel, Builtin::TaskIsDone}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Array: {
            for (Builtin b : {Builtin::ListLen, Builtin::ListIsEmpty, Builtin::ListContains, Builtin::ListSort, Builtin::ListSortBy, Builtin::ListReverse, Builtin::ListIndexOf, Builtin::ListSlice, Builtin::ListSwap, Builtin::ListFirst, Builtin::ListLast, Builtin::ListMap, Builtin::ListFilter, Builtin::ListReduce, Builtin::ListAny, Builtin::ListAll, Builtin::ListCount, Builtin::ListUnique, Builtin::ListShuffle, Builtin::ListMin, Builtin::ListMax, Builtin::ListSum, Builtin::ListZip, Builtin::ListFill, Builtin::ListToList}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Map: {
            for (Builtin b : {Builtin::MapLen, Builtin::MapIsEmpty, Builtin::MapHas, Builtin::MapGet, Builtin::MapGetOr, Builtin::MapRemove, Builtin::MapClear, Builtin::MapKeys, Builtin::MapValues}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Set: {
            for (Builtin b : {Builtin::SetLen, Builtin::SetIsEmpty, Builtin::SetAdd, Builtin::SetRemove, Builtin::SetContains, Builtin::SetClear, Builtin::SetToList, Builtin::SetUnion, Builtin::SetIntersection, Builtin::SetDifference}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Deque: {
            for (Builtin b : {Builtin::DequeLen, Builtin::DequeIsEmpty, Builtin::DequePushBack, Builtin::DequePushFront, Builtin::DequePopBack, Builtin::DequePopFront, Builtin::DequeFront, Builtin::DequeBack, Builtin::DequeClear, Builtin::DequeContains, Builtin::DequeToList}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::PQueue: {
            for (Builtin b : {Builtin::PQPush, Builtin::PQPop, Builtin::PQPeek, Builtin::PQLen, Builtin::PQIsEmpty, Builtin::PQClear, Builtin::PQToList}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::StrBuilder: {
            for (Builtin b : {Builtin::SbAppend, Builtin::SbLen, Builtin::SbClear, Builtin::SbToStr, Builtin::SbIsEmpty}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Result: {
            for (Builtin b : {Builtin::ResIsOk, Builtin::ResIsErr, Builtin::ResUnwrap, Builtin::ResUnwrapErr, Builtin::ResUnwrapOr, Builtin::ResExpect, Builtin::ResOk, Builtin::ResErr}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Nullable: {
            for (Builtin b : {Builtin::ResUnwrapOr, Builtin::OptUnwrap, Builtin::OptIsSome, Builtin::OptIsNone, Builtin::OptExpect}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Num: {
            for (Builtin b : {Builtin::NumToHex, Builtin::NumToRadix, Builtin::NumToFixed}) {
                if ((b == Builtin::NumToFixed) != t->is_floating()) continue;
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        case TypeTag::Signal: {
            for (Builtin b : {Builtin::SignalConnect, Builtin::SignalDisconnect}) {
                if (const auto* bd = find_builtin_doc(b)) {
                    put(CompletionItem{std::string(bd->label), CompletionKind::Method,
                                       std::string(bd->signature), std::string(bd->doc),
                                       std::string(bd->label)});
                }
            }
            break;
        }
        default:
            break;
    }
}

void add_static_completions(const Analysis& a, std::string_view type_ident,
                            std::map<std::string, CompletionItem>& items) {
    auto put = [&](CompletionItem item) {
        items.try_emplace(item.label, std::move(item));
    };
    if (const EnumInfo* en = a.find_enum(type_ident); en != nullptr) {
        for (std::size_t i = 0; i < en->variants.size(); ++i) {
            const auto& v = en->variants[i];
            std::string detail(en->name);
            detail += '.';
            detail += v.name;
            if (!v.field_types.empty()) {
                detail += '(';
                for (std::size_t k = 0; k < v.field_types.size(); ++k) {
                    if (k > 0) detail += ", ";
                    if (k < v.field_names.size() && !v.field_names[k].empty()) {
                        detail += v.field_names[k];
                        detail += ": ";
                    }
                    detail += type_name(v.field_types[k]);
                }
                detail += ')';
            }
            put(CompletionItem{
                std::string(v.name),
                CompletionKind::EnumMember,
                std::move(detail),
                extract_doc_comment(a.source(), v.span),
                std::string(v.name),
            });
        }
        return;
    }

    const RecordInfo* rec = a.find_class(type_ident);
    if (rec == nullptr) {
        rec = a.find_struct(type_ident);
    }
    if (rec != nullptr) {
        for (const auto& [mname, fn] : rec->methods) {
            if (fn->kind != FnKind::Static) continue;
            put(CompletionItem{
                std::string(mname),
                CompletionKind::Function,
                format_fn_signature(fn),
                extract_doc_comment(a.source(), fn->span),
                std::string(mname),
            });
        }
        for (const auto& [sname, gv] : rec->statics) {
            put(CompletionItem{
                std::string(sname),
                gv->is_const ? CompletionKind::Constant : CompletionKind::Variable,
                gv->type != nullptr ? type_name(gv->type) : "",
                extract_doc_comment(a.source(), gv->span),
                std::string(sname),
            });
        }
    }
}

} // namespace

// ---------------------------------------------------------------- URI / Path helpers

std::string path_to_uri(std::string_view path) {
    if (path.starts_with("file://")) return std::string(path);
    std::error_code ec;
    fs::path p(path);
    if (p.is_relative() && !path.starts_with('/') && !path.starts_with('\\')) {
        p = fs::absolute(p, ec);
    }
    p = p.lexically_normal();
    std::string s = p.generic_string();
    if (!s.starts_with('/')) s = "/" + s;
    return "file://" + s;
}

std::string uri_to_path(std::string_view uri) {
    if (uri.starts_with("file://")) {
        uri.remove_prefix(7);
    }
    std::string out;
    out.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(uri[i + 1]);
            int lo = hex(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += uri[i];
    }
    if (out.size() >= 3 && out[0] == '/' &&
        ((out[1] >= 'a' && out[1] <= 'z') || (out[1] >= 'A' && out[1] <= 'Z')) && out[2] == ':') {
        out.erase(0, 1);
        return fs::path(out).lexically_normal().string();
    }
    if (out.starts_with('/')) {
        return out;
    }
    return fs::path(out).lexically_normal().string();
}

// ---------------------------------------------------------------- Server

Server::Server(ServerOptions options) : options_(std::move(options)) {
    if (!options_.root_path.empty()) {
        set_root_path(options_.root_path);
    }
}

Server::~Server() = default;

void Server::set_root_path(std::string path) {
    if (path.empty()) {
        options_.root_path.clear();
        return;
    }
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(path), ec).lexically_normal();
    options_.root_path = p.string();
}

void Server::set_root_uri(std::string_view uri) {
    if (uri.empty()) return;
    set_root_path(uri_to_path(uri));
}

void Server::ensure_root_from_uri(std::string_view uri) {
    if (!options_.root_path.empty() || !uri.starts_with("file://")) return;
    std::error_code ec;
    fs::path p = fs::path(uri_to_path(uri)).parent_path();
    fs::path fallback = p;
    for (fs::path cur = p; !cur.empty() && cur != cur.root_path(); cur = cur.parent_path()) {
        if (fs::exists(cur / "project.toml", ec)) {
            options_.root_path = cur.string();
            return;
        }
    }
    if (!fallback.empty() && fs::exists(fallback, ec)) {
        options_.root_path = fallback.string();
    }
}

void Server::open_document(std::string_view uri, std::string text, int version) {
    std::string norm = normalize_uri(uri);
    ensure_root_from_uri(norm);
    open_docs_[norm] = std::move(text);
    doc_versions_[norm] = version;
}

void Server::change_document(std::string_view uri, std::string text, int version) {
    std::string norm = normalize_uri(uri);
    ensure_root_from_uri(norm);
    open_docs_[norm] = std::move(text);
    doc_versions_[norm] = version;
}

void Server::change_document_range(std::string_view uri, Range range, std::string_view text, int version) {
    std::string norm = normalize_uri(uri);
    ensure_root_from_uri(norm);
    auto cur = document_text(norm).value_or("");
    std::uint32_t start = pos_to_offset(cur, range.start);
    std::uint32_t end = std::max(start, pos_to_offset(cur, range.end));
    cur.replace(start, end - start, text);
    open_docs_[norm] = std::move(cur);
    doc_versions_[norm] = version;
}

void Server::close_document(std::string_view uri) {
    std::string norm = normalize_uri(uri);
    open_docs_.erase(norm);
    doc_versions_.erase(norm);
}

std::optional<std::string> Server::document_text(std::string_view uri) const {
    std::string norm = normalize_uri(uri);
    if (auto it = open_docs_.find(norm); it != open_docs_.end()) {
        return it->second;
    }
    if (norm.starts_with("file://")) {
        return slurp_file(uri_to_path(norm));
    }
    return std::nullopt;
}

std::map<std::string, std::vector<DiagnosticItem>> Server::all_diagnostics() {
    auto a = build_analysis(options_, open_docs_);
    std::map<std::string, std::vector<DiagnosticItem>> result;
    for (const auto& s : a->slices) {
        if (!s.is_prelude) {
            result.try_emplace(s.name, std::vector<DiagnosticItem>{});
        }
    }
    for (const Diagnostic& d : a->diags) {
        const PartSlice* s = a->slice_at(d.span.begin);
        if (s == nullptr || s->is_prelude) continue;
        DiagnosticItem item;
        item.range = a->span_to_range(*s, d.span);
        item.severity = d.severity;
        item.message = d.message;
        if (d.fix.has_value()) {
            item.fix = TextEdit{
                a->span_to_range(*s, d.fix->span),
                d.fix->replacement,
            };
        }
        result[s->name].push_back(std::move(item));
    }
    return result;
}

std::vector<DiagnosticItem> Server::diagnostics(std::string_view uri) {
    std::string norm = normalize_uri(uri);
    auto all = all_diagnostics();
    if (auto it = all.find(norm); it != all.end()) {
        return it->second;
    }
    return {};
}

std::vector<CodeAction> Server::code_actions(std::string_view uri, Range range) {
    std::string norm = normalize_uri(uri);
    auto diags = diagnostics(norm);
    std::vector<CodeAction> actions;
    for (const auto& d : diags) {
        if (!d.fix.has_value()) continue;
        if (!d.range.overlaps(range)) continue;
        CodeAction act;
        act.title = "Fix: " + d.message;
        act.kind = "quickfix";
        act.uri = norm;
        act.edits.push_back(*d.fix);
        actions.push_back(std::move(act));
    }
    return actions;
}

std::optional<Location> Server::definition(std::string_view uri, Position pos) {
    std::string norm = normalize_uri(uri);
    auto a = build_analysis(options_, open_docs_);
    std::uint32_t offset = a->uri_pos_to_offset(norm, pos);
    const Occurrence* occ = find_occurrence_at(*a, offset);
    if (occ == nullptr || !occ->target.valid()) return std::nullopt;
    auto decl_span = a->target_decl_span(occ->target);
    if (!decl_span) return std::nullopt;
    return a->span_to_location(*decl_span);
}

std::optional<HoverInfo> Server::hover(std::string_view uri, Position pos) {
    std::string norm = normalize_uri(uri);
    auto a = build_analysis(options_, open_docs_);
    const PartSlice* slice = a->slice_for_uri(norm);
    if (slice == nullptr) return std::nullopt;
    std::uint32_t offset = a->uri_pos_to_offset(norm, pos);
    const Occurrence* occ = find_occurrence_at(*a, offset);
    if (occ == nullptr) return std::nullopt;
    std::string md = a->describe_target(occ->target, occ->type);
    if (md.empty()) return std::nullopt;
    return HoverInfo{std::move(md), a->span_to_range(*slice, occ->span)};
}

std::vector<Location> Server::references(std::string_view uri, Position pos, bool include_declaration) {
    std::string norm = normalize_uri(uri);
    auto a = build_analysis(options_, open_docs_);
    std::uint32_t offset = a->uri_pos_to_offset(norm, pos);
    const Occurrence* occ = find_occurrence_at(*a, offset);
    if (occ == nullptr || !occ->target.valid()) return {};

    SymTarget target = occ->target;
    std::vector<Location> locs;
    for (const auto& o : a->occurrences) {
        if (!(o.target == target)) continue;
        if (!include_declaration && o.is_decl) continue;
        if (auto loc = a->span_to_location(o.span)) {
            if (std::find(locs.begin(), locs.end(), *loc) == locs.end()) {
                locs.push_back(std::move(*loc));
            }
        }
    }
    return locs;
}

std::optional<Range> Server::prepare_rename(std::string_view uri, Position pos) {
    std::string norm = normalize_uri(uri);
    auto a = build_analysis(options_, open_docs_);
    const PartSlice* slice = a->slice_for_uri(norm);
    if (slice == nullptr) return std::nullopt;
    std::uint32_t offset = a->uri_pos_to_offset(norm, pos);
    const Occurrence* occ = find_occurrence_at(*a, offset);
    if (occ == nullptr || !occ->target.valid()) return std::nullopt;
    if (occ->target.kind == TargetKind::Builtin) return std::nullopt;
    // Refuse renaming symbols declared in the host prelude.
    auto decl_span = a->target_decl_span(occ->target);
    if (!decl_span || !a->span_to_location(*decl_span).has_value()) return std::nullopt;
    return a->span_to_range(*slice, occ->span);
}

std::optional<WorkspaceEdit> Server::rename(std::string_view uri, Position pos, std::string_view new_name) {
    if (!is_valid_identifier(new_name)) return std::nullopt;
    std::string norm = normalize_uri(uri);
    auto a = build_analysis(options_, open_docs_);
    std::uint32_t offset = a->uri_pos_to_offset(norm, pos);
    const Occurrence* occ = find_occurrence_at(*a, offset);
    if (occ == nullptr || !occ->target.valid() || occ->target.kind == TargetKind::Builtin) {
        return std::nullopt;
    }
    auto decl_span = a->target_decl_span(occ->target);
    if (!decl_span || !a->span_to_location(*decl_span).has_value()) {
        return std::nullopt;
    }

    WorkspaceEdit edit;
    SymTarget target = occ->target;
    for (const auto& o : a->occurrences) {
        if (!(o.target == target)) continue;
        if (auto loc = a->span_to_location(o.span)) {
            auto& vec = edit.changes[loc->uri];
            TextEdit te{loc->range, std::string(new_name)};
            if (std::find(vec.begin(), vec.end(), te) == vec.end()) {
                vec.push_back(std::move(te));
            }
        }
    }
    if (edit.changes.empty()) return std::nullopt;
    return edit;
}

std::vector<DocumentSymbol> Server::document_symbols(std::string_view uri) {
    std::string norm = normalize_uri(uri);
    auto a = build_analysis(options_, open_docs_);
    const PartSlice* slice = a->slice_for_uri(norm);
    if (slice == nullptr) return {};

    auto make_sym = [&](const Decl* d, SymbolKind kind, std::string detail = {}) -> DocumentSymbol {
        DocumentSymbol sym;
        sym.name = std::string(d->name.name);
        sym.detail = std::move(detail);
        sym.kind = kind;
        sym.range = a->span_to_range(*slice, d->span);
        sym.selection_range = a->span_to_range(*slice, span_empty(d->name.span) ? d->span : d->name.span);
        return sym;
    };

    auto add_member_syms = [&](const List<Decl*>& members, std::vector<DocumentSymbol>& out) {
        for (const Decl* m : members) {
            if (m == nullptr) continue;
            switch (m->kind) {
                case DeclKind::Fn: {
                    SymbolKind sk = m->name.name == "init" ? SymbolKind::Constructor : SymbolKind::Method;
                    out.push_back(make_sym(m, sk));
                    break;
                }
                case DeclKind::Var:
                    out.push_back(make_sym(m, m->is_static ? SymbolKind::Variable : SymbolKind::Field));
                    break;
                case DeclKind::Const:
                    out.push_back(make_sym(m, SymbolKind::Constant));
                    break;
                case DeclKind::Signal:
                    out.push_back(make_sym(m, SymbolKind::Event));
                    break;
                default:
                    break;
            }
        }
    };

    std::vector<DocumentSymbol> symbols;
    for (const Decl* d : a->mod.decls) {
        if (d == nullptr || d->span.begin < slice->base || d->span.begin > slice->end) continue;
        if (d->name.empty()) continue;
        switch (d->kind) {
            case DeclKind::Class: {
                auto sym = make_sym(d, SymbolKind::Class);
                add_member_syms(as<ClassDecl>(d)->members, sym.children);
                symbols.push_back(std::move(sym));
                break;
            }
            case DeclKind::Struct: {
                auto sym = make_sym(d, SymbolKind::Struct);
                add_member_syms(as<StructDecl>(d)->members, sym.children);
                symbols.push_back(std::move(sym));
                break;
            }
            case DeclKind::Enum: {
                auto sym = make_sym(d, SymbolKind::Enum);
                for (const EnumVariant& v : as<EnumDecl>(d)->variants) {
                    DocumentSymbol vs;
                    vs.name = std::string(v.name.name);
                    vs.kind = SymbolKind::EnumMember;
                    vs.range = a->span_to_range(*slice, v.span);
                    vs.selection_range = a->span_to_range(*slice, v.name.span);
                    sym.children.push_back(std::move(vs));
                }
                symbols.push_back(std::move(sym));
                break;
            }
            case DeclKind::Trait: {
                auto sym = make_sym(d, SymbolKind::Interface);
                add_member_syms(as<TraitDecl>(d)->members, sym.children);
                symbols.push_back(std::move(sym));
                break;
            }
            case DeclKind::Fn:
                symbols.push_back(make_sym(d, SymbolKind::Function));
                break;
            case DeclKind::Var:
                symbols.push_back(make_sym(d, SymbolKind::Variable));
                break;
            case DeclKind::Const:
                symbols.push_back(make_sym(d, SymbolKind::Constant));
                break;
            default:
                break;
        }
    }
    return symbols;
}

std::vector<WorkspaceSymbol> Server::workspace_symbols(std::string_view query) {
    auto a = build_analysis(options_, open_docs_);
    std::string q_lower(query);
    std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto matches = [&](std::string_view name) {
        if (q_lower.empty()) return true;
        std::string n_lower(name);
        std::transform(n_lower.begin(), n_lower.end(), n_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return n_lower.find(q_lower) != std::string::npos;
    };

    std::vector<WorkspaceSymbol> result;
    for (const auto& slice : a->slices) {
        if (slice.is_prelude) continue;
        auto doc_syms = document_symbols(slice.name);
        for (const auto& ds : doc_syms) {
            if (matches(ds.name)) {
                result.push_back(WorkspaceSymbol{
                    ds.name,
                    ds.kind,
                    Location{slice.name, ds.selection_range},
                    {},
                });
            }
            for (const auto& child : ds.children) {
                if (matches(child.name)) {
                    result.push_back(WorkspaceSymbol{
                        child.name,
                        child.kind,
                        Location{slice.name, child.selection_range},
                        ds.name,
                    });
                }
            }
        }
    }
    return result;
}

std::vector<TextEdit> Server::format_document(std::string_view uri) {
    auto text = document_text(uri);
    if (!text) return {};
    SourceFile sf(std::string(uri), *text);
    FormatResult res = format_source(sf);
    if (!res.ok || res.formatted == *text) return {};
    Position end_pos = offset_to_pos(*text, static_cast<std::uint32_t>(text->size()));
    return {TextEdit{Range{{0, 0}, end_pos}, std::move(res.formatted)}};
}

std::optional<SignatureHelp> Server::signature_help(std::string_view uri, Position pos) {
    std::string norm = normalize_uri(uri);
    auto doc = document_text(norm);
    if (!doc) return std::nullopt;
    std::uint32_t local_off = pos_to_offset(*doc, pos);

    // Scan backward from `local_off` to find the enclosing unclosed `(` and count
    // top-level commas to determine `active_parameter`.
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;
    std::uint32_t commas = 0;
    std::optional<std::uint32_t> open_paren;
    for (std::uint32_t i = local_off; i > 0;) {
        --i;
        char c = (*doc)[i];
        if (c == ')') ++paren_depth;
        else if (c == ']') ++bracket_depth;
        else if (c == '}') ++brace_depth;
        else if (c == '[') { if (bracket_depth > 0) --bracket_depth; }
        else if (c == '{') { if (brace_depth > 0) --brace_depth; }
        else if (c == ',' && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
            ++commas;
        } else if (c == '(') {
            if (paren_depth > 0) {
                --paren_depth;
            } else {
                open_paren = i;
                break;
            }
        }
    }
    if (!open_paren) return std::nullopt;

    // Extract the callee identifier right before `(`.
    std::uint32_t end_id = *open_paren;
    while (end_id > 0 && ((*doc)[end_id - 1] == ' ' || (*doc)[end_id - 1] == '\t')) --end_id;
    std::uint32_t start_id = end_id;
    while (start_id > 0 && is_ident_char((*doc)[start_id - 1])) --start_id;
    if (start_id == end_id) return std::nullopt;
    std::string_view callee_name = std::string_view(*doc).substr(start_id, end_id - start_id);

    auto a = build_analysis(options_, open_docs_);
    const PartSlice* slice = a->slice_for_uri(norm);
    if (slice == nullptr) return std::nullopt;
    std::uint32_t combined_id_pos = slice->base + start_id;

    const Function* target_fn = nullptr;
    const EnumInfo* target_enum = nullptr;
    std::uint32_t target_variant = 0;
    std::optional<Builtin> target_builtin;

    if (const Occurrence* occ = find_occurrence_at(*a, combined_id_pos)) {
        if (occ->target.kind == TargetKind::Function) {
            target_fn = static_cast<const Function*>(occ->target.ptr);
        } else if (occ->target.kind == TargetKind::Class) {
            const auto* cls = static_cast<const ClassInfo*>(occ->target.ptr);
            for (const ClassInfo* c = cls; c != nullptr; c = c->base) {
                if (c->init != nullptr) {
                    target_fn = c->init;
                    break;
                }
            }
        } else if (occ->target.kind == TargetKind::Variant) {
            target_enum = static_cast<const EnumInfo*>(occ->target.ptr);
            target_variant = occ->target.sub_index;
        } else if (occ->target.kind == TargetKind::Builtin) {
            target_builtin = occ->target.builtin;
        }
    }

    // Fallback when the call is still incomplete (`foo(` without closing `)`):
    // resolve by receiver or global function/class/builtin name.
    if (target_fn == nullptr && target_enum == nullptr && !target_builtin) {
        std::uint32_t before = start_id;
        while (before > 0 && ((*doc)[before - 1] == ' ' || (*doc)[before - 1] == '\t')) --before;
        if (before > 0 && (*doc)[before - 1] == '.') {
            std::uint32_t dot_pos = before - 1;
            std::uint32_t r_end = dot_pos;
            while (r_end > 0 && ((*doc)[r_end - 1] == ' ' || (*doc)[r_end - 1] == '\t')) --r_end;
            std::uint32_t r_start = r_end;
            while (r_start > 0 && is_ident_char((*doc)[r_start - 1])) --r_start;
            std::string_view recv_name = std::string_view(*doc).substr(r_start, r_end - r_start);
            if (const EnumInfo* en = a->find_enum(recv_name); en != nullptr) {
                int vi = en->variant_index(callee_name);
                if (vi >= 0) {
                    target_enum = en;
                    target_variant = static_cast<std::uint32_t>(vi);
                }
            } else if (const ClassInfo* cls = a->find_class(recv_name); cls != nullptr) {
                if (auto mit = cls->methods.find(callee_name); mit != cls->methods.end()) {
                    target_fn = mit->second;
                }
            } else if (const StructInfo* st = a->find_struct(recv_name); st != nullptr) {
                if (auto mit = st->methods.find(callee_name); mit != st->methods.end()) {
                    target_fn = mit->second;
                }
            }
            if (target_fn == nullptr && r_start < r_end) {
                if (const Occurrence* rocc = find_occurrence_at(*a, slice->base + r_start)) {
                    const Type* rt = rocc->type != nullptr ? a->program.types.non_null(rocc->type) : nullptr;
                    if (rt != nullptr && rt->tag == TypeTag::Class) {
                        for (const ClassInfo* c = rt->cls(); c != nullptr; c = c->base) {
                            if (auto mit = c->methods.find(callee_name); mit != c->methods.end()) {
                                target_fn = mit->second;
                                break;
                            }
                        }
                    } else if (rt != nullptr && rt->tag == TypeTag::Struct && rt->st() != nullptr) {
                        if (auto mit = rt->st()->methods.find(callee_name); mit != rt->st()->methods.end()) {
                            target_fn = mit->second;
                        }
                    }
                }
            }
        } else {
            std::uint32_t cur_off = combined_id_pos;
            for (const auto& c : a->program.classes) {
                Span cspan = c->decl != nullptr ? c->decl->span : c->span;
                if (cur_off >= cspan.begin && cur_off <= cspan.end) {
                    for (const ClassInfo* cur = c.get(); cur != nullptr && target_fn == nullptr; cur = cur->base) {
                        if (auto mit = cur->methods.find(callee_name); mit != cur->methods.end()) {
                            target_fn = mit->second;
                        }
                    }
                    break;
                }
            }
            if (target_fn == nullptr) {
                for (const auto& s : a->program.structs) {
                    Span sspan = s->decl != nullptr ? s->decl->span : s->span;
                    if (cur_off >= sspan.begin && cur_off <= sspan.end) {
                        if (auto mit = s->methods.find(callee_name); mit != s->methods.end()) {
                            target_fn = mit->second;
                        }
                        break;
                    }
                }
            }
            if (target_fn == nullptr) {
                if (const Function* fn = a->find_fn(callee_name); fn != nullptr) {
                    target_fn = fn;
                } else if (const ClassInfo* cls = a->find_class(callee_name); cls != nullptr) {
                    for (const ClassInfo* c = cls; c != nullptr; c = c->base) {
                        if (c->init != nullptr) {
                            target_fn = c->init;
                            break;
                        }
                    }
                } else {
                    for (const auto& bd : builtin_docs()) {
                        if (is_free_builtin(bd.builtin) && bd.label == callee_name) {
                            target_builtin = bd.builtin;
                            break;
                        }
                    }
                }
            }
        }
    }

    SignatureInfo sig;
    if (target_fn != nullptr) {
        sig.label = format_fn_signature(target_fn);
        sig.documentation = extract_doc_comment(a->source(), target_fn->span);
        for (const auto& p : target_fn->params) {
            sig.parameters.push_back(ParameterInfo{format_param(p), {}});
        }
    } else if (target_enum != nullptr && target_variant < target_enum->variants.size()) {
        const auto& v = target_enum->variants[target_variant];
        sig.label = std::string(target_enum->name) + "." + std::string(v.name) + "(";
        for (std::size_t i = 0; i < v.field_types.size(); ++i) {
            if (i > 0) sig.label += ", ";
            std::string plabel;
            if (i < v.field_names.size() && !v.field_names[i].empty()) {
                plabel += v.field_names[i];
                plabel += ": ";
            }
            plabel += type_name(v.field_types[i]);
            sig.label += plabel;
            sig.parameters.push_back(ParameterInfo{std::move(plabel), {}});
        }
        sig.label += ")";
        sig.documentation = extract_doc_comment(a->source(), v.span);
    } else if (target_builtin.has_value()) {
        if (const auto* bd = find_builtin_doc(*target_builtin)) {
            sig.label = std::string(bd->signature);
            sig.documentation = std::string(bd->doc);
            for (std::string_view p : bd->params) {
                sig.parameters.push_back(ParameterInfo{std::string(p), {}});
            }
        }
    } else {
        return std::nullopt;
    }

    SignatureHelp help;
    std::uint32_t max_param = sig.parameters.empty()
                                  ? 0
                                  : static_cast<std::uint32_t>(sig.parameters.size() - 1);
    help.active_parameter = std::min(commas, max_param);
    help.signatures.push_back(std::move(sig));
    return help;
}

std::vector<CompletionItem> Server::complete(std::string_view uri, Position pos) {
    std::string norm = normalize_uri(uri);
    auto doc = document_text(norm);
    if (!doc) return {};
    std::uint32_t local_off = pos_to_offset(*doc, pos);

    // Extract any identifier prefix immediately before the cursor.
    std::uint32_t prefix_start = local_off;
    while (prefix_start > 0 && is_ident_char((*doc)[prefix_start - 1])) {
        --prefix_start;
    }
    std::string_view prefix = std::string_view(*doc).substr(prefix_start, local_off - prefix_start);

    auto filter_and_return = [&](std::map<std::string, CompletionItem>& items) {
        std::vector<CompletionItem> out;
        for (auto& [k, item] : items) {
            if (prefix.empty() || std::string_view(item.label).starts_with(prefix)) {
                out.push_back(std::move(item));
            }
        }
        return out;
    };

    // Case 1: `#name` completion after `#`.
    if (prefix_start > 0 && (*doc)[prefix_start - 1] == '#') {
        auto a = build_analysis(options_, open_docs_);
        std::map<std::string, CompletionItem> items;
        for (const std::string& nm : a->interned_names) {
            if (nm == prefix) continue;
            items.emplace(nm, CompletionItem{nm, CompletionKind::Constant, "name",
                                             "Interned `#name` symbol.", nm});
        }
        return filter_and_return(items);
    }

    // Case 2: `@attribute` completion after `@`.
    if (prefix_start > 0 && (*doc)[prefix_start - 1] == '@') {
        std::map<std::string, CompletionItem> items;
        struct AttrDoc { std::string_view name; std::string_view detail; std::string_view doc; };
        static constexpr AttrDoc attrs[] = {
            {"export", "@export(range = lo..hi, step = s, hint = \"...\")",
             "Expose field in the scene inspector and save it in scene files."},
            {"onready", "@onready", "Evaluate field initializer in `ready()` after child nodes exist."},
            {"acyclic", "@acyclic", "Assert that instances of this class never participate in a reference cycle."},
            {"allow_slow", "@allow_slow", "Permit non-component classes in `foreach` queries."},
            {"native", "@native", "Bind declaration to a host C++ function."},
            {"deprecated", "@deprecated(\"reason\")", "Warn when this declaration is used."},
        };
        for (const auto& ad : attrs) {
            items.emplace(std::string(ad.name),
                          CompletionItem{std::string(ad.name), CompletionKind::Keyword,
                                         std::string(ad.detail), std::string(ad.doc),
                                         std::string(ad.name)});
        }
        return filter_and_return(items);
    }

    // Case 3: Dot completion (`expr.` or `expr?.` or `Type.`).
    std::uint32_t before_prefix = prefix_start;
    while (before_prefix > 0 && ((*doc)[before_prefix - 1] == ' ' || (*doc)[before_prefix - 1] == '\t')) {
        --before_prefix;
    }
    if (before_prefix > 0 && (*doc)[before_prefix - 1] == '.') {
        std::uint32_t dot_pos = before_prefix - 1;
        bool safe_nav = (dot_pos > 0 && (*doc)[dot_pos - 1] == '?');
        std::uint32_t recv_end = safe_nav ? dot_pos - 1 : dot_pos;
        while (recv_end > 0 && ((*doc)[recv_end - 1] == ' ' || (*doc)[recv_end - 1] == '\t')) {
            --recv_end;
        }

        // Replace `.prefix` (or `?.prefix`) with spaces in a patched copy of the
        // document so the parser and type checker cleanly resolve the receiver!
        std::string patched = *doc;
        for (std::uint32_t i = (safe_nav ? dot_pos - 1 : dot_pos); i < local_off; ++i) {
            if (patched[i] != '\n' && patched[i] != '\r') patched[i] = ' ';
        }
        auto a = build_analysis(options_, open_docs_, norm, patched);
        const PartSlice* slice = a->slice_for_uri(norm);
        if (slice == nullptr) return {};

        std::map<std::string, CompletionItem> items;

        // Check if the receiver is a simple identifier (`Canvas.`, `Phase.`, `self.`, `player.`).
        std::uint32_t r_start = recv_end;
        while (r_start > 0 && is_ident_char((*doc)[r_start - 1])) --r_start;
        std::string_view recv_ident = std::string_view(*doc).substr(r_start, recv_end - r_start);

        if (!recv_ident.empty() &&
            (r_start == 0 || ((*doc)[r_start - 1] != '.' && !is_ident_char((*doc)[r_start - 1])))) {
            add_static_completions(*a, recv_ident, items);
        }

        const Type* recv_type = nullptr;
        if (recv_end > 0) {
            std::uint32_t probe = slice->base + (recv_end - 1);
            if (const Occurrence* occ = find_occurrence_at(*a, probe)) {
                if (occ->type != nullptr && !occ->type->is_error()) {
                    // Only treat as instance receiver if it's not a bare type name
                    if (occ->target.kind != TargetKind::Class &&
                        occ->target.kind != TargetKind::Struct &&
                        occ->target.kind != TargetKind::Enum) {
                        recv_type = occ->type;
                    }
                }
            }
            if (recv_type == nullptr) {
                recv_type = find_tir_expr_type(*a, {slice->base + r_start, slice->base + recv_end});
            }
            // Also handle `self.` or bare field/param/global names when inside a class or struct method.
            if (recv_type == nullptr && !recv_ident.empty()) {
                std::uint32_t cur_off = slice->base + recv_end;
                for (const auto& fn : a->program.functions) {
                    Span fspan = fn->decl != nullptr ? fn->decl->span : fn->span;
                    if (cur_off >= fspan.begin && cur_off <= fspan.end) {
                        for (const auto& p : fn->params) {
                            if (p.name == recv_ident && p.type != nullptr && !p.type->is_error()) {
                                recv_type = p.type;
                                break;
                            }
                        }
                    }
                }
                if (recv_type == nullptr) {
                    for (const auto& c : a->program.classes) {
                        Span cspan = c->decl != nullptr ? c->decl->span : c->span;
                        if (cur_off >= cspan.begin && cur_off <= cspan.end) {
                            if (recv_ident == "self") {
                                recv_type = c->self_type;
                            } else if (recv_ident == "super" && c->base != nullptr) {
                                recv_type = c->base->self_type;
                            } else {
                                for (const ClassInfo* cur = c.get(); cur != nullptr && recv_type == nullptr;
                                     cur = cur->base) {
                                    auto it = cur->field_map.find(recv_ident);
                                    if (it != cur->field_map.end() && it->second != nullptr &&
                                        it->second->type != nullptr && !it->second->type->is_error()) {
                                        recv_type = it->second->type;
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
                if (recv_type == nullptr) {
                    for (const auto& s : a->program.structs) {
                        Span sspan = s->decl != nullptr ? s->decl->span : s->span;
                        if (cur_off >= sspan.begin && cur_off <= sspan.end) {
                            if (recv_ident == "self") {
                                recv_type = s->self_type;
                            } else {
                                auto it = s->field_map.find(recv_ident);
                                if (it != s->field_map.end() && it->second != nullptr &&
                                    it->second->type != nullptr && !it->second->type->is_error()) {
                                    recv_type = it->second->type;
                                }
                            }
                            break;
                        }
                    }
                }
                if (recv_type == nullptr) {
                    for (const auto& g : a->program.global_vars) {
                        if (g->name == recv_ident && g->type != nullptr && !g->type->is_error()) {
                            recv_type = g->type;
                            break;
                        }
                    }
                }
            }
        }

        if (recv_type != nullptr) {
            add_type_completions(*a, recv_type, safe_nav, items);
        }
        return filter_and_return(items);
    }

    // Case 4: General scope / type / keyword completion.
    auto a = build_analysis(options_, open_docs_);
    const PartSlice* slice = a->slice_for_uri(norm);
    if (slice == nullptr) return {};
    std::uint32_t combined_off = slice->base + local_off;

    std::map<std::string, CompletionItem> items;
    auto put = [&](CompletionItem item) {
        items.try_emplace(item.label, std::move(item));
    };

    // Enclosing function locals & parameters, and enclosing class/struct members.
    for (const auto& fn : a->program.functions) {
        Span fspan = fn->decl != nullptr ? fn->decl->span : fn->span;
        if (combined_off >= fspan.begin && combined_off <= fspan.end) {
            for (const auto& p : fn->params) {
                put(CompletionItem{
                    std::string(p.name),
                    CompletionKind::Variable,
                    format_param(p),
                    {},
                    std::string(p.name),
                });
            }
            for (const auto* gp : fn->generics) {
                put(CompletionItem{
                    std::string(gp->name),
                    CompletionKind::TypeParameter,
                    "type parameter",
                    {},
                    std::string(gp->name),
                });
            }
        }
    }
    for (const auto& occ : a->occurrences) {
        if (!occ.is_decl || occ.target.kind != TargetKind::Local) continue;
        const auto* lv = static_cast<const LocalVar*>(occ.target.ptr);
        if (lv == nullptr || lv->name.empty() || lv->name[0] == '<') continue;
        // Include locals declared before the cursor in the same file.
        if (occ.span.begin >= slice->base && occ.span.begin <= combined_off) {
            put(CompletionItem{
                std::string(lv->name),
                CompletionKind::Variable,
                lv->type != nullptr ? type_name(lv->type) : "",
                {},
                std::string(lv->name),
            });
        }
    }

    for (const auto& c : a->program.classes) {
        Span cspan = c->decl != nullptr ? c->decl->span : c->span;
        if (combined_off >= cspan.begin && combined_off <= cspan.end) {
            put(CompletionItem{"self", CompletionKind::Keyword, std::string(c->name), {}, "self"});
            if (c->base != nullptr) {
                put(CompletionItem{"super", CompletionKind::Keyword, std::string(c->base->name), {}, "super"});
            }
            add_type_completions(*a, c->self_type, false, items);
        }
    }
    for (const auto& s : a->program.structs) {
        Span sspan = s->decl != nullptr ? s->decl->span : s->span;
        if (combined_off >= sspan.begin && combined_off <= sspan.end) {
            put(CompletionItem{"self", CompletionKind::Keyword, std::string(s->name), {}, "self"});
            add_type_completions(*a, s->self_type, false, items);
        }
    }

    // Global classes, structs, enums, traits, functions, globals/consts.
    for (const auto& c : a->program.classes) {
        put(CompletionItem{
            std::string(c->name),
            CompletionKind::Class,
            "class " + std::string(c->name),
            extract_doc_comment(a->source(), c->span),
            std::string(c->name),
        });
    }
    for (const auto& s : a->program.structs) {
        put(CompletionItem{
            std::string(s->name),
            CompletionKind::Struct,
            "struct " + std::string(s->name),
            extract_doc_comment(a->source(), s->span),
            std::string(s->name),
        });
    }
    for (const auto& e : a->program.enums) {
        put(CompletionItem{
            std::string(e->name),
            CompletionKind::Enum,
            "enum " + std::string(e->name),
            extract_doc_comment(a->source(), e->span),
            std::string(e->name),
        });
    }
    for (const auto& tr : a->program.traits) {
        put(CompletionItem{
            std::string(tr->name),
            CompletionKind::Interface,
            "trait " + std::string(tr->name),
            extract_doc_comment(a->source(), tr->span),
            std::string(tr->name),
        });
    }
    for (const auto& fn : a->program.functions) {
        if (fn->owner != nullptr || fn->kind != FnKind::Free) continue;
        if (fn->name.empty() || fn->name[0] == '<') continue;
        put(CompletionItem{
            std::string(fn->name),
            CompletionKind::Function,
            format_fn_signature(fn.get()),
            extract_doc_comment(a->source(), fn->span),
            std::string(fn->name),
        });
    }
    for (const auto& gv : a->program.global_vars) {
        if (gv->qualified.find('.') != std::string::npos) continue;
        put(CompletionItem{
            std::string(gv->name),
            gv->is_const ? CompletionKind::Constant : CompletionKind::Variable,
            gv->type != nullptr ? type_name(gv->type) : "",
            extract_doc_comment(a->source(), gv->span),
            std::string(gv->name),
        });
    }

    // Built-in free functions, primitive types, and keywords.
    for (const auto& bd : builtin_docs()) {
        if (is_free_builtin(bd.builtin) && bd.label.front() != '<') {
            put(CompletionItem{
                std::string(bd.label),
                CompletionKind::Function,
                std::string(bd.signature),
                std::string(bd.doc),
                std::string(bd.label),
            });
        }
    }
    static constexpr std::string_view builtin_types[] = {
        "int", "float", "bool", "str", "name", "void",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
        "vec2", "vec3", "vec4", "List", "Task", "Signal", "Map", "Set", "Deque", "PriorityQueue",
        "StrBuilder", "Result",
    };
    for (std::string_view bt : builtin_types) {
        put(CompletionItem{std::string(bt), CompletionKind::Struct, "built-in type", {}, std::string(bt)});
    }
    static constexpr std::string_view keywords[] = {
        "var", "const", "fn", "class", "struct", "enum", "trait", "extends", "with",
        "if", "else", "match", "for", "foreach", "in", "while", "loop", "break", "continue",
        "return", "yield", "await", "start", "emit", "signal", "defer", "unsafe",
        "new", "alloc", "free", "manual", "weak", "pub", "static", "final",
        "true", "false", "null", "is", "as", "step",
    };
    for (std::string_view kw : keywords) {
        put(CompletionItem{std::string(kw), CompletionKind::Keyword, "keyword", {}, std::string(kw)});
    }

    return filter_and_return(items);
}

// ---------------------------------------------------------------- JSON-RPC 2.0 protocol

std::vector<std::string> Server::publish_diagnostics_notifications() {
    auto all = all_diagnostics();
    std::vector<std::string> out;

    // Clear any previously published URIs that are no longer in the workspace.
    for (const std::string& old_uri : published_uris_) {
        if (all.find(old_uri) == all.end()) {
            all.emplace(old_uri, std::vector<DiagnosticItem>{});
        }
    }
    published_uris_.clear();

    for (const auto& [uri, items] : all) {
        published_uris_.insert(uri);
        json::Array diag_arr;
        for (const auto& d : items) {
            int sev = 1;
            switch (d.severity) {
                case Severity::Error: sev = 1; break;
                case Severity::Warning: sev = 2; break;
                case Severity::Note: sev = 3; break;
            }
            json::Object dobj{
                {"range", json::range_json(d.range)},
                {"severity", json::Value::integer(sev)},
                {"source", json::Value::string("pluma")},
                {"message", json::Value::string(d.message)},
            };
            diag_arr.push_back(json::Value::object(std::move(dobj)));
        }
        out.push_back(json::make_notification(
            "textDocument/publishDiagnostics",
            json::Value::object({
                {"uri", json::Value::string(uri)},
                {"diagnostics", json::Value::array(std::move(diag_arr))},
            })));
    }
    return out;
}

namespace {

json::Value doc_symbol_json(const DocumentSymbol& s) {
    json::Array children;
    for (const auto& ch : s.children) {
        children.push_back(doc_symbol_json(ch));
    }
    json::Object obj{
        {"name", json::Value::string(s.name)},
        {"kind", json::Value::integer(static_cast<int>(s.kind))},
        {"range", json::range_json(s.range)},
        {"selectionRange", json::range_json(s.selection_range)},
    };
    if (!s.detail.empty()) obj["detail"] = json::Value::string(s.detail);
    if (!children.empty()) obj["children"] = json::Value::array(std::move(children));
    return json::Value::object(std::move(obj));
}

} // namespace

std::vector<std::string> Server::handle_message(std::string_view json_payload) {
    json::Parser parser(json_payload);
    auto msg_opt = parser.parse();
    if (!msg_opt || !msg_opt->is_object()) {
        return {json::make_error_response(json::Value::null(), -32700, "Parse error")};
    }
    const json::Value& msg = *msg_opt;
    const json::Value* id_ptr = msg.get("id");
    json::Value id = id_ptr != nullptr ? *id_ptr : json::Value::null();
    std::string_view method = msg.str_or("method");
    const json::Value* params = msg.get("params");

    auto doc_uri = [&]() -> std::string {
        if (params == nullptr) return {};
        if (const json::Value* td = params->get("textDocument")) {
            return normalize_uri(td->str_or("uri"));
        }
        return normalize_uri(params->str_or("uri"));
    };

    auto doc_pos = [&]() -> Position {
        if (params == nullptr) return {};
        return json::parse_pos(params->get("position"));
    };

    if (method == "initialize") {
        if (params != nullptr) {
            std::string_view root_uri = params->str_or("rootUri");
            std::string_view root_p = params->str_or("rootPath");
            if (!root_uri.empty()) set_root_uri(root_uri);
            else if (!root_p.empty()) set_root_path(std::string(root_p));
        }
        json::Value caps = json::Value::object({
            {"textDocumentSync", json::Value::object({
                                     {"openClose", json::Value::boolean(true)},
                                     {"change", json::Value::integer(2)}, // Incremental & Full supported
                                     {"save", json::Value::boolean(true)},
                                 })},
            {"completionProvider", json::Value::object({
                                       {"triggerCharacters",
                                        json::Value::array({
                                            json::Value::string("."),
                                            json::Value::string("#"),
                                            json::Value::string("@"),
                                        })},
                                   })},
            {"signatureHelpProvider", json::Value::object({
                                          {"triggerCharacters",
                                           json::Value::array({
                                               json::Value::string("("),
                                               json::Value::string(","),
                                           })},
                                      })},
            {"definitionProvider", json::Value::boolean(true)},
            {"hoverProvider", json::Value::boolean(true)},
            {"referencesProvider", json::Value::boolean(true)},
            {"documentSymbolProvider", json::Value::boolean(true)},
            {"workspaceSymbolProvider", json::Value::boolean(true)},
            {"renameProvider", json::Value::object({
                                   {"prepareProvider", json::Value::boolean(true)},
                               })},
            {"codeActionProvider", json::Value::boolean(true)},
            {"documentFormattingProvider", json::Value::boolean(true)},
        });
        json::Value result = json::Value::object({
            {"capabilities", std::move(caps)},
            {"serverInfo", json::Value::object({
                               {"name", json::Value::string("pluma-lsp")},
                               {"version", json::Value::string("0.1.0")},
                           })},
        });
        return {json::make_response(id, std::move(result))};
    }

    if (method == "initialized") {
        return {};
    }

    if (method == "shutdown") {
        shutdown_received_ = true;
        return {json::make_response(id, json::Value::null())};
    }

    if (method == "exit") {
        exit_requested_ = true;
        return {};
    }

    if (method == "textDocument/didOpen") {
        if (params != nullptr) {
            if (const json::Value* td = params->get("textDocument")) {
                open_document(td->str_or("uri"), std::string(td->str_or("text")),
                              static_cast<int>(td->int_or("version", 1)));
            }
        }
        return publish_diagnostics_notifications();
    }

    if (method == "textDocument/didChange") {
        if (params != nullptr) {
            std::string uri = doc_uri();
            int version = 2;
            if (const json::Value* td = params->get("textDocument")) {
                version = static_cast<int>(td->int_or("version", 2));
            }
            if (const json::Value* changes = params->get("contentChanges");
                changes != nullptr && changes->is_array()) {
                for (const json::Value& ch : changes->a) {
                    std::string_view text = ch.str_or("text");
                    if (const json::Value* rng = ch.get("range")) {
                        change_document_range(uri, json::parse_range(rng), text, version);
                    } else {
                        change_document(uri, std::string(text), version);
                    }
                }
            }
        }
        return publish_diagnostics_notifications();
    }

    if (method == "textDocument/didClose") {
        close_document(doc_uri());
        return publish_diagnostics_notifications();
    }

    if (method == "textDocument/didSave") {
        return publish_diagnostics_notifications();
    }

    if (method == "textDocument/completion") {
        auto items = complete(doc_uri(), doc_pos());
        json::Array arr;
        for (const auto& item : items) {
            json::Object obj{
                {"label", json::Value::string(item.label)},
                {"kind", json::Value::integer(static_cast<int>(item.kind))},
            };
            if (!item.detail.empty()) obj["detail"] = json::Value::string(item.detail);
            if (!item.documentation.empty()) {
                obj["documentation"] = json::Value::object({
                    {"kind", json::Value::string("markdown")},
                    {"value", json::Value::string(item.documentation)},
                });
            }
            if (!item.insert_text.empty() && item.insert_text != item.label) {
                obj["insertText"] = json::Value::string(item.insert_text);
            }
            arr.push_back(json::Value::object(std::move(obj)));
        }
        return {json::make_response(id, json::Value::array(std::move(arr)))};
    }

    if (method == "textDocument/definition") {
        auto loc = definition(doc_uri(), doc_pos());
        if (!loc) return {json::make_response(id, json::Value::null())};
        return {json::make_response(id, json::location_json(*loc))};
    }

    if (method == "textDocument/hover") {
        auto h = hover(doc_uri(), doc_pos());
        if (!h) return {json::make_response(id, json::Value::null())};
        return {json::make_response(
            id,
            json::Value::object({
                {"contents", json::Value::object({
                                 {"kind", json::Value::string("markdown")},
                                 {"value", json::Value::string(h->contents)},
                             })},
                {"range", json::range_json(h->range)},
            }))};
    }

    if (method == "textDocument/signatureHelp") {
        auto sh = signature_help(doc_uri(), doc_pos());
        if (!sh) return {json::make_response(id, json::Value::null())};
        json::Array sigs;
        for (const auto& s : sh->signatures) {
            json::Array params_arr;
            for (const auto& p : s.parameters) {
                json::Object pobj{{"label", json::Value::string(p.label)}};
                if (!p.documentation.empty()) {
                    pobj["documentation"] = json::Value::string(p.documentation);
                }
                params_arr.push_back(json::Value::object(std::move(pobj)));
            }
            json::Object sobj{
                {"label", json::Value::string(s.label)},
                {"parameters", json::Value::array(std::move(params_arr))},
            };
            if (!s.documentation.empty()) {
                sobj["documentation"] = json::Value::object({
                    {"kind", json::Value::string("markdown")},
                    {"value", json::Value::string(s.documentation)},
                });
            }
            sigs.push_back(json::Value::object(std::move(sobj)));
        }
        return {json::make_response(
            id,
            json::Value::object({
                {"signatures", json::Value::array(std::move(sigs))},
                {"activeSignature", json::Value::integer(sh->active_signature)},
                {"activeParameter", json::Value::integer(sh->active_parameter)},
            }))};
    }

    if (method == "textDocument/documentSymbol") {
        auto syms = document_symbols(doc_uri());
        json::Array arr;
        for (const auto& s : syms) arr.push_back(doc_symbol_json(s));
        return {json::make_response(id, json::Value::array(std::move(arr)))};
    }

    if (method == "workspace/symbol") {
        std::string_view query = params != nullptr ? params->str_or("query") : "";
        auto syms = workspace_symbols(query);
        json::Array arr;
        for (const auto& s : syms) {
            json::Object obj{
                {"name", json::Value::string(s.name)},
                {"kind", json::Value::integer(static_cast<int>(s.kind))},
                {"location", json::location_json(s.location)},
            };
            if (!s.container_name.empty()) {
                obj["containerName"] = json::Value::string(s.container_name);
            }
            arr.push_back(json::Value::object(std::move(obj)));
        }
        return {json::make_response(id, json::Value::array(std::move(arr)))};
    }

    if (method == "textDocument/references") {
        bool inc_decl = true;
        if (params != nullptr) {
            if (const json::Value* ctx = params->get("context")) {
                inc_decl = ctx->bool_or("includeDeclaration", true);
            }
        }
        auto locs = references(doc_uri(), doc_pos(), inc_decl);
        json::Array arr;
        for (const auto& l : locs) arr.push_back(json::location_json(l));
        return {json::make_response(id, json::Value::array(std::move(arr)))};
    }

    if (method == "textDocument/prepareRename") {
        auto rng = prepare_rename(doc_uri(), doc_pos());
        if (!rng) return {json::make_response(id, json::Value::null())};
        return {json::make_response(id, json::range_json(*rng))};
    }

    if (method == "textDocument/rename") {
        std::string_view new_name = params != nullptr ? params->str_or("newName") : "";
        auto edit = rename(doc_uri(), doc_pos(), new_name);
        if (!edit) {
            return {json::make_error_response(id, -32602, "Cannot rename this symbol")};
        }
        json::Object changes;
        for (const auto& [u, edits] : edit->changes) {
            json::Array earr;
            for (const auto& e : edits) earr.push_back(json::edit_json(e));
            changes[u] = json::Value::array(std::move(earr));
        }
        return {json::make_response(id, json::Value::object({{"changes", json::Value::object(std::move(changes))}}))};
    }

    if (method == "textDocument/codeAction") {
        Range rng = params != nullptr ? json::parse_range(params->get("range")) : Range{};
        auto actions = code_actions(doc_uri(), rng);
        json::Array arr;
        for (const auto& a : actions) {
            json::Array edits;
            for (const auto& e : a.edits) edits.push_back(json::edit_json(e));
            json::Object changes;
            changes[a.uri] = json::Value::array(std::move(edits));
            arr.push_back(json::Value::object({
                {"title", json::Value::string(a.title)},
                {"kind", json::Value::string(a.kind)},
                {"edit", json::Value::object({{"changes", json::Value::object(std::move(changes))}})},
            }));
        }
        return {json::make_response(id, json::Value::array(std::move(arr)))};
    }

    if (method == "textDocument/formatting") {
        auto edits = format_document(doc_uri());
        json::Array arr;
        for (const auto& e : edits) arr.push_back(json::edit_json(e));
        return {json::make_response(id, json::Value::array(std::move(arr)))};
    }

    // Ignore unknown notifications; return MethodNotFound for unknown requests.
    if (id_ptr == nullptr) return {};
    return {json::make_error_response(id, -32601, std::format("Method not found: {}", method))};
}

int Server::run_stdio(std::istream& in, std::ostream& out) {
    std::string line;
    while (!exit_requested_ && std::getline(in, line)) {
        std::size_t content_length = 0;
        while (!line.empty() && line != "\r") {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            constexpr std::string_view prefix = "Content-Length:";
            if (line.size() >= prefix.size()) {
                bool match = true;
                for (std::size_t i = 0; i < prefix.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(line[i])) !=
                        std::tolower(static_cast<unsigned char>(prefix[i]))) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    std::string_view rest = std::string_view(line).substr(prefix.size());
                    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
                        rest.remove_prefix(1);
                    }
                    // A malformed or absurd length skips the message instead of
                    // throwing or allocating gigabytes.
                    std::uint64_t n = 0;
                    const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), n);
                    constexpr std::uint64_t max_message = 64u << 20;
                    content_length =
                        parsed.ec == std::errc{} && n <= max_message ? static_cast<std::size_t>(n) : 0;
                }
            }
            if (!std::getline(in, line)) break;
        }
        if (content_length == 0) continue;

        std::string payload(content_length, '\0');
        in.read(payload.data(), static_cast<std::streamsize>(content_length));
        if (static_cast<std::size_t>(in.gcount()) != content_length) break;

        auto outgoing = handle_message(payload);
        for (const std::string& msg : outgoing) {
            out << "Content-Length: " << msg.size() << "\r\n\r\n" << msg;
        }
        out.flush();
    }
    return exit_code();
}

} // namespace pluma::lsp

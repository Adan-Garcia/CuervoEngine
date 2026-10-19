#pragma once

// Native functions and methods known to the type checker and implemented by
// the VM. The checker types each one specially (several are polymorphic); the
// compiler emits CALLN with the id; the VM's native table is indexed by it.

#include <cstdint>
#include <string_view>

namespace pluma {

enum class Builtin : std::uint16_t {
    // free functions
    Print,      // print(x: any)
    Assert,     // assert(cond: bool, message: str = "")
    Panic,      // panic(message: str)
    Min,        // min(a: N, b: N) -> N   (N numeric)
    Max,
    Abs,        // abs(x: N) -> N
    Clamp,      // clamp(x: N, lo: N, hi: N) -> N
    Sqrt,       // sqrt(x: F) -> F (IEEE-exact, so deterministic)
    Floor,      // floor/ceil/round(x: F) -> F (exact)
    Ceil,
    Round,
    Sin,        // sin/cos(x: float), atan2(y, x): through VmOptions::math (the
    Cos,        //   engine installs deterministic versions, DESIGN.md 4.1)
    Tan,
    Asin,
    Acos,
    Atan,
    Atan2,
    Exp,
    Log,
    Log2,
    Log10,
    Pow,
    Sign,
    Lerp,
    InverseLerp,
    Remap,
    Smoothstep,
    Step,
    MoveToward,
    RadToDeg,
    DegToRad,
    Wrap,
    WrapAngle,
    FromAngle,  // from_angle(angle: float, length = 1.0) -> vec2
    // vecN methods (receiver first)
    VecLength,
    VecLengthSq,
    VecDot,
    VecNormalized,
    VecDistance,
    VecDistanceSq,
    VecLerp,
    VecAbs,
    VecMin,
    VecMax,
    VecClamp,
    VecReflect,
    VecProject,
    VecSlide,
    VecClampLength,
    VecRotated,       // vec2 only
    VecAngle,         // vec2 only
    VecPerpendicular, // vec2 only
    VecAngleTo,       // vec2 only
    VecSlerp,         // vec2 only
    VecCross2,        // vec2 only (perp-dot -> float)
    VecCross3,        // vec3 only (cross -> vec3)
    // Task methods
    TaskCancel,
    TaskIsDone,
    // Signal methods; SignalEmit is `emit name(args)`
    SignalConnect,
    SignalDisconnect,
    SignalEmit,
    // List<T> methods (receiver first)
    ListNew,    // List<T>()
    ListLen,
    ListPush,
    ListPop,
    ListClear,
    ListInsert,
    ListRemove,
    ListIsEmpty,
    ListContains,
    // str methods
    StrLen,
    StrContains,
    StrStartsWith,
    StrEndsWith,
    // ---- Milestone 9.2a (LANGUAGE.md 9.16 - 9.19)
    ParseInt,
    ParseFloat,
    ResultOk,
    ResultErr,
    MapNew,
    SetNew,
    DequeNew,
    PQueueNew,
    StrBuilderNew,
    ArrayNew,
    MapLit,
    SetLit,
    DequeLit,
    ListSort,
    ListSortBy,
    ListReverse,
    ListIndexOf,
    ListSlice,
    ListExtend,
    ListSwap,
    ListFirst,
    ListLast,
    ListMap,
    ListFilter,
    ListReduce,
    ListAny,
    ListAll,
    ListCount,
    ListUnique,
    ListShuffle,
    ListMin,
    ListMax,
    ListSum,
    ListZip,
    ListFill,
    ListToList,
    MapLen,
    MapIsEmpty,
    MapHas,
    MapGet,
    MapGetOr,
    MapIndex,
    MapSet,
    MapRemove,
    MapClear,
    MapKeys,
    MapValues,
    SetLen,
    SetIsEmpty,
    SetAdd,
    SetRemove,
    SetContains,
    SetClear,
    SetToList,
    SetUnion,
    SetIntersection,
    SetDifference,
    DequeLen,
    DequeIsEmpty,
    DequePushBack,
    DequePushFront,
    DequePopBack,
    DequePopFront,
    DequeFront,
    DequeBack,
    DequeGet,
    DequeSet,
    DequeClear,
    DequeContains,
    DequeToList,
    PQPush,
    PQPop,
    PQPeek,
    PQLen,
    PQIsEmpty,
    PQClear,
    PQToList,
    SbAppend,
    SbLen,
    SbClear,
    SbToStr,
    SbIsEmpty,
    StrSplit,
    StrJoin,
    StrReplace,
    StrTrim,
    StrTrimStart,
    StrTrimEnd,
    StrToUpper,
    StrToLower,
    StrFind,
    StrSubstr,
    StrByteAt,
    StrChars,
    StrCharCount,
    StrRepeat,
    StrPadLeft,
    StrPadRight,
    StrLines,
    StrIsEmpty,
    StrParseInt,
    StrParseFloat,
    NumToHex,
    NumToRadix,
    NumToFixed,
    ResIsOk,
    ResIsErr,
    ResUnwrap,
    ResUnwrapErr,
    ResUnwrapOr,
    ResExpect,
    ResOk,
    ResErr,
    OptUnwrap,
    OptIsSome,
    OptIsNone,
    OptExpect,
    // internal
    Echo,       // REPL: print a top-level expression's value (repr form)
    // ---- Milestone 9.2b (after Echo: VM::call_native handles these itself, not call_lib) (LANGUAGE.md 9.20): integer arithmetic on any one integer kind
    WrappingAdd,
    WrappingSub,
    WrappingMul,
    CheckedAdd,     // -> T? (null on overflow)
    CheckedSub,
    CheckedMul,
    SaturatingAdd,
    SaturatingSub,
    SaturatingMul,
    // vecN methods
    VecFloor,
    VecCeil,
    VecRound,
    VecFract,
    VecSign,
    VecSnapped,
    VecBounce,
    VecMoveToward,
    VecDirectionTo,
    VecLimitLength,
    VecSetAt,       // v[i] = e lowers to v = v.<set_at>(i, e)
    Count,
};

// Called as free functions (or constructors), not as methods.
constexpr bool is_free_builtin(Builtin b) {
    return b <= Builtin::FromAngle || b == Builtin::ListNew ||
           (b >= Builtin::ParseInt && b <= Builtin::DequeLit) ||
           (b >= Builtin::WrappingAdd && b <= Builtin::SaturatingMul);
}

constexpr std::string_view builtin_name(Builtin b) {
    switch (b) {
        case Builtin::Print: return "print";
        case Builtin::Assert: return "assert";
        case Builtin::Panic: return "panic";
        case Builtin::Min: return "min";
        case Builtin::Max: return "max";
        case Builtin::Abs: return "abs";
        case Builtin::Clamp: return "clamp";
        case Builtin::Sqrt: return "sqrt";
        case Builtin::Floor: return "floor";
        case Builtin::Ceil: return "ceil";
        case Builtin::Round: return "round";
        case Builtin::Sin: return "sin";
        case Builtin::Cos: return "cos";
        case Builtin::Tan: return "tan";
        case Builtin::Asin: return "asin";
        case Builtin::Acos: return "acos";
        case Builtin::Atan: return "atan";
        case Builtin::Atan2: return "atan2";
        case Builtin::Exp: return "exp";
        case Builtin::Log: return "log";
        case Builtin::Log2: return "log2";
        case Builtin::Log10: return "log10";
        case Builtin::Pow: return "pow";
        case Builtin::Sign: return "sign";
        case Builtin::Lerp: return "lerp";
        case Builtin::InverseLerp: return "inverse_lerp";
        case Builtin::Remap: return "remap";
        case Builtin::Smoothstep: return "smoothstep";
        case Builtin::Step: return "step";
        case Builtin::MoveToward: return "move_toward";
        case Builtin::RadToDeg: return "rad_to_deg";
        case Builtin::DegToRad: return "deg_to_rad";
        case Builtin::Wrap: return "wrap";
        case Builtin::WrapAngle: return "wrap_angle";
        case Builtin::FromAngle: return "from_angle";
        case Builtin::VecLength: return "length";
        case Builtin::VecLengthSq: return "length_squared";
        case Builtin::VecDot: return "dot";
        case Builtin::VecNormalized: return "normalized";
        case Builtin::VecDistance: return "distance";
        case Builtin::VecDistanceSq: return "distance_squared";
        case Builtin::VecLerp: return "lerp";
        case Builtin::VecAbs: return "abs";
        case Builtin::VecMin: return "min";
        case Builtin::VecMax: return "max";
        case Builtin::VecClamp: return "clamp";
        case Builtin::VecReflect: return "reflect";
        case Builtin::VecProject: return "project";
        case Builtin::VecSlide: return "slide";
        case Builtin::VecClampLength: return "clamp_length";
        case Builtin::VecRotated: return "rotated";
        case Builtin::VecAngle: return "angle";
        case Builtin::VecPerpendicular: return "perpendicular";
        case Builtin::VecAngleTo: return "angle_to";
        case Builtin::VecSlerp: return "slerp";
        case Builtin::VecCross2:
        case Builtin::VecCross3: return "cross";
        case Builtin::TaskCancel: return "cancel";
        case Builtin::TaskIsDone: return "is_done";
        case Builtin::SignalConnect: return "connect";
        case Builtin::SignalDisconnect: return "disconnect";
        case Builtin::SignalEmit: return "emit";
        case Builtin::ListNew: return "List";
        case Builtin::ListLen: return "len";
        case Builtin::ListPush: return "push";
        case Builtin::ListPop: return "pop";
        case Builtin::ListClear: return "clear";
        case Builtin::ListInsert: return "insert";
        case Builtin::ListRemove: return "remove";
        case Builtin::ListIsEmpty: return "is_empty";
        case Builtin::ListContains: return "contains";
        case Builtin::StrLen: return "len";
        case Builtin::StrContains: return "contains";
        case Builtin::StrStartsWith: return "starts_with";
        case Builtin::StrEndsWith: return "ends_with";
        case Builtin::ParseInt: return "parse_int";
        case Builtin::ParseFloat: return "parse_float";
        case Builtin::ResultOk: return "Ok";
        case Builtin::ResultErr: return "Err";
        case Builtin::MapNew: return "Map";
        case Builtin::SetNew: return "Set";
        case Builtin::DequeNew: return "Deque";
        case Builtin::PQueueNew: return "PriorityQueue";
        case Builtin::StrBuilderNew: return "StrBuilder";
        case Builtin::ArrayNew: return "<array>";
        case Builtin::MapLit: return "<map literal>";
        case Builtin::SetLit: return "<set literal>";
        case Builtin::DequeLit: return "<deque literal>";
        case Builtin::ListSort: return "sort";
        case Builtin::ListSortBy: return "sort_by";
        case Builtin::ListReverse: return "reverse";
        case Builtin::ListIndexOf: return "index_of";
        case Builtin::ListSlice: return "slice";
        case Builtin::ListExtend: return "extend";
        case Builtin::ListSwap: return "swap";
        case Builtin::ListFirst: return "first";
        case Builtin::ListLast: return "last";
        case Builtin::ListMap: return "map";
        case Builtin::ListFilter: return "filter";
        case Builtin::ListReduce: return "reduce";
        case Builtin::ListAny: return "any";
        case Builtin::ListAll: return "all";
        case Builtin::ListCount: return "count";
        case Builtin::ListUnique: return "unique";
        case Builtin::ListShuffle: return "shuffle";
        case Builtin::ListMin: return "min";
        case Builtin::ListMax: return "max";
        case Builtin::ListSum: return "sum";
        case Builtin::ListZip: return "zip";
        case Builtin::ListFill: return "fill";
        case Builtin::ListToList: return "to_list";
        case Builtin::MapLen: return "len";
        case Builtin::MapIsEmpty: return "is_empty";
        case Builtin::MapHas: return "has";
        case Builtin::MapGet: return "get";
        case Builtin::MapGetOr: return "get_or";
        case Builtin::MapIndex: return "[]";
        case Builtin::MapSet: return "[]=";
        case Builtin::MapRemove: return "remove";
        case Builtin::MapClear: return "clear";
        case Builtin::MapKeys: return "keys";
        case Builtin::MapValues: return "values";
        case Builtin::SetLen: return "len";
        case Builtin::SetIsEmpty: return "is_empty";
        case Builtin::SetAdd: return "add";
        case Builtin::SetRemove: return "remove";
        case Builtin::SetContains: return "contains";
        case Builtin::SetClear: return "clear";
        case Builtin::SetToList: return "to_list";
        case Builtin::SetUnion: return "union";
        case Builtin::SetIntersection: return "intersection";
        case Builtin::SetDifference: return "difference";
        case Builtin::DequeLen: return "len";
        case Builtin::DequeIsEmpty: return "is_empty";
        case Builtin::DequePushBack: return "push_back";
        case Builtin::DequePushFront: return "push_front";
        case Builtin::DequePopBack: return "pop_back";
        case Builtin::DequePopFront: return "pop_front";
        case Builtin::DequeFront: return "front";
        case Builtin::DequeBack: return "back";
        case Builtin::DequeGet: return "[]";
        case Builtin::DequeSet: return "[]=";
        case Builtin::DequeClear: return "clear";
        case Builtin::DequeContains: return "contains";
        case Builtin::DequeToList: return "to_list";
        case Builtin::PQPush: return "push";
        case Builtin::PQPop: return "pop";
        case Builtin::PQPeek: return "peek";
        case Builtin::PQLen: return "len";
        case Builtin::PQIsEmpty: return "is_empty";
        case Builtin::PQClear: return "clear";
        case Builtin::PQToList: return "to_list";
        case Builtin::SbAppend: return "append";
        case Builtin::SbLen: return "len";
        case Builtin::SbClear: return "clear";
        case Builtin::SbToStr: return "to_str";
        case Builtin::SbIsEmpty: return "is_empty";
        case Builtin::StrSplit: return "split";
        case Builtin::StrJoin: return "join";
        case Builtin::StrReplace: return "replace";
        case Builtin::StrTrim: return "trim";
        case Builtin::StrTrimStart: return "trim_start";
        case Builtin::StrTrimEnd: return "trim_end";
        case Builtin::StrToUpper: return "to_upper";
        case Builtin::StrToLower: return "to_lower";
        case Builtin::StrFind: return "find";
        case Builtin::StrSubstr: return "substr";
        case Builtin::StrByteAt: return "byte_at";
        case Builtin::StrChars: return "chars";
        case Builtin::StrCharCount: return "char_count";
        case Builtin::StrRepeat: return "repeat";
        case Builtin::StrPadLeft: return "pad_left";
        case Builtin::StrPadRight: return "pad_right";
        case Builtin::StrLines: return "lines";
        case Builtin::StrIsEmpty: return "is_empty";
        case Builtin::StrParseInt: return "parse_int";
        case Builtin::StrParseFloat: return "parse_float";
        case Builtin::NumToHex: return "to_hex";
        case Builtin::NumToRadix: return "to_radix";
        case Builtin::NumToFixed: return "to_fixed";
        case Builtin::ResIsOk: return "is_ok";
        case Builtin::ResIsErr: return "is_err";
        case Builtin::ResUnwrap: return "unwrap";
        case Builtin::ResUnwrapErr: return "unwrap_err";
        case Builtin::ResUnwrapOr: return "unwrap_or";
        case Builtin::ResExpect: return "expect";
        case Builtin::ResOk: return "ok";
        case Builtin::ResErr: return "err";
        case Builtin::OptUnwrap: return "unwrap";
        case Builtin::OptIsSome: return "is_some";
        case Builtin::OptIsNone: return "is_none";
        case Builtin::OptExpect: return "expect";
        case Builtin::WrappingAdd: return "wrapping_add";
        case Builtin::WrappingSub: return "wrapping_sub";
        case Builtin::WrappingMul: return "wrapping_mul";
        case Builtin::CheckedAdd: return "checked_add";
        case Builtin::CheckedSub: return "checked_sub";
        case Builtin::CheckedMul: return "checked_mul";
        case Builtin::SaturatingAdd: return "saturating_add";
        case Builtin::SaturatingSub: return "saturating_sub";
        case Builtin::SaturatingMul: return "saturating_mul";
        case Builtin::VecFloor: return "floor";
        case Builtin::VecCeil: return "ceil";
        case Builtin::VecRound: return "round";
        case Builtin::VecFract: return "fract";
        case Builtin::VecSign: return "sign";
        case Builtin::VecSnapped: return "snapped";
        case Builtin::VecBounce: return "bounce";
        case Builtin::VecMoveToward: return "move_toward";
        case Builtin::VecDirectionTo: return "direction_to";
        case Builtin::VecLimitLength: return "limit_length";
        case Builtin::VecSetAt: return "<set component>";
        case Builtin::Echo: return "<echo>";
        case Builtin::Count: break;
    }
    return "?";
}

} // namespace pluma

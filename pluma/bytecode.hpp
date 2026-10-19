#pragma once

// Register bytecode for the VM (DESIGN.md 5.12, register VM per 7.1).
//
// Each function (Proto) has a fixed register window; instructions name
// registers directly. An instruction is 8 bytes: opcode, a small immediate
// `k` (numeric kind, argument count, flags) and three 16-bit operands. Jump
// targets are absolute and use b:c as one 32-bit operand.
//
// Frames reference registers by base offset into a value stack and never use
// the C++ stack for script-to-script calls, so a frame could be moved to the
// heap for coroutines (Phase 3).

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/source.hpp"
#include "pluma/value.hpp"

namespace pluma {

// Operand kinds for typed instructions (`k`). 0..9 are NumKind.
namespace opk {
constexpr std::uint8_t Bool = 10;
constexpr std::uint8_t Str = 11;
constexpr std::uint8_t Ref = 12;  // identity
constexpr std::uint8_t Name = 13;
constexpr std::uint8_t Enum = 14; // plain enum (discriminant)
constexpr std::uint8_t Dyn = 15;  // dispatch on tags (generics, structs, nullable values)
constexpr std::uint8_t Vec = 16;  // vecN, componentwise
constexpr std::uint8_t VecS = 17; // vecN op float (b vector, c scalar)
constexpr std::uint8_t SVec = 18; // float * vecN (b scalar, c vector)
constexpr std::uint8_t Inclusive = 0x80; // FORPREP / FORLOOP flag
// Dereferencing instructions (GetF, SetF, GetI, SetI, ListNext, CallV):
// skip the generation check of a manual reference (`unsafe { }`).
constexpr std::uint8_t Unchecked = 0x40;
constexpr std::uint8_t WeakField = 0x20;    // GetF / SetF: a `weak var` field
constexpr std::uint8_t UntracedList = 0x80; // NewList: the element type is acyclic
constexpr std::uint8_t ArrayList = 0x04;    // NewList: a fixed-size array [T; N] (ObjKind::Array)
constexpr std::uint8_t IterPair = 0x01;     // IterNext: also write the Map entry's value to a + 1
} // namespace opk

#define PLUMA_OPCODES(X)                                                                 \
    X(Nop)                                                                              \
    X(Mov)       /* a = b */                                                            \
    X(LoadK)     /* a = K[b] */                                                         \
    X(LoadI)     /* a = int16(b) of kind k */                                           \
    X(LoadNull)  /* a = null */                                                         \
    X(LoadBool)  /* a = b != 0 */                                                       \
    X(Clear)     /* release a; a = nil */                                               \
    X(GetG)      /* a = G[b] */                                                         \
    X(SetG)      /* G[a] = b */                                                         \
    X(GetUp)     /* a = captures[b] (raw: cell or value) */                             \
    X(GetUpC)    /* a = cell(captures[b]).value */                                      \
    X(SetUpC)    /* cell(captures[a]).value = b */                                      \
    X(NewCell)   /* a = new cell(b) (k = 1: untraced) */                                \
    X(GetCell)   /* a = cell(b).value */                                                \
    X(SetCell)   /* cell(a).value = b */                                                \
    X(Closure)   /* a = closure(proto b, captures c..) */                               \
    X(GetF)      /* a = b.fields[c] */                                                  \
    X(SetF)      /* a.fields[b] = c */                                                  \
    X(GetI)      /* a = b[c] */                                                         \
    X(SetI)      /* a[b] = c */                                                         \
    X(NewList)   /* a = [c registers from b] (k = Arena | UntracedList) */              \
    X(NewObj)    /* a = new instance of class b (k = Arena: manual) */                  \
    X(NewStruct) /* a = struct b with fields c.. */                                     \
    X(NewEnum)   /* a = enum b variant k with payload c.. */                            \
    X(Copy)      /* a = copy of b (deep for struct boxes) */                            \
    X(Add) X(Sub) X(Mul) X(Div) X(Rem) /* a = b op c (kind k) */                        \
    X(Neg)       /* a = -b */                                                           \
    X(BAnd) X(BOr) X(BXor) X(Shl) X(Shr)                                                \
    X(BNot)                                                                             \
    X(Not)                                                                              \
    X(Eq) X(Ne) X(Lt) X(Le) /* a = b op c (kind k) */                                   \
    X(Cast)      /* a = b converted from kind c to kind k */                            \
    X(Downcast)  /* a = b, panics unless an instance of class c */                      \
    X(InstOf)    /* a = b is an instance of class c */                                  \
    X(Variant)   /* a = variant index of enum object b */                               \
    X(GetPay)    /* a = payload field c of enum object b */                             \
    X(Jmp)       /* pc = b:c */                                                         \
    X(JmpF) X(JmpT) X(JmpNull) X(JmpNN) /* test a, jump to b:c */                       \
    X(ToStr)     /* a = str(b) */                                                       \
    X(Concat)    /* a = concat of c registers from b */                                 \
    X(Call)      /* a = proto b (args from c) */                                        \
    X(CallV)     /* a = vtable slot b of receiver c (args from c) */                    \
    X(CallC)     /* a = closure b (args from c) */                                      \
    X(CallN)     /* a = native b (k args from c) */                                     \
    X(CallD)     /* a = method named K[b] of receiver c (k args from c) */              \
    X(Ret)       /* return a */                                                         \
    X(Ret0)      /* return nothing */                                                   \
    X(ForPrep)   /* a=i, a+1=end, a+2=step; skip to b:c if empty (k = kind|flags) */    \
    X(ForLoop)   /* i += step; jump to b:c while in range */                            \
    X(ListNext)  /* if c < len(b): a = b[c], skip next instruction */                   \
    X(Inc)       /* a += 1 (kind k, no overflow check: loop counters) */                \
    X(CallX)     /* a = host native b (k args from c) (pluma/bindings.hpp) */            \
    X(Await)     /* suspend the coroutine (kind k, operand b); a = result on resume */  \
    X(VecNew)    /* a = vecK from c parts (floats/vectors) starting at b */             \
    X(Swz)       /* a = components of b; c = count, k = indices (2 bits each) */        \
    X(VecWith)   /* a = b with components k (2 bits each) set from c */                 \
    X(VecIdx)    /* a = b[c] (vector component, bounds-checked) */                      \
    X(NewSig)    /* a = new signal object */                                            \
    X(Free)      /* free manual object a (DESIGN.md 5.10) */                            \
    X(IterNext)  /* a (a+1: value, k & IterPair) = next of Map/Set/Deque b at cursor c; skip next if found */

enum class Op : std::uint8_t {
#define PLUMA_OP_ENUM(name) name,
    PLUMA_OPCODES(PLUMA_OP_ENUM)
#undef PLUMA_OP_ENUM
};

std::string_view to_string(Op op);

struct Instr {
    Op op = Op::Nop;
    std::uint8_t k = 0;
    std::uint16_t a = 0;
    std::uint16_t b = 0;
    std::uint16_t c = 0;
    std::uint32_t target() const { return (static_cast<std::uint32_t>(b) << 16) | c; }
    void set_target(std::uint32_t t) {
        b = static_cast<std::uint16_t>(t >> 16);
        c = static_cast<std::uint16_t>(t & 0xffff);
    }
};
static_assert(sizeof(Instr) == 8);

// Debug info for one named local (parameters included): the register that
// holds it while pc is in [begin, end). Panics read it to show a frame's
// locals (DESIGN.md 5.8: "the editor shows the stack trace and locals").
struct LocalDebug {
    std::string name;
    std::string type;
    std::uint16_t reg = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

class VM;

// Pre-resolved source location and expression slice for an instruction,
// populated by the AOT loader when SourceFile text is not shipped
// (ScriptForm::Native, DESIGN.md 5.12).
struct SpanLoc {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string text;
};

struct Proto {
    using AotFn = bool (*)(VM& vm, Value* out);

    Proto() = default;
    Proto(const Proto&) = delete;
    Proto& operator=(const Proto&) = delete;
    ~Proto() {
        for (Value& v : consts) release(v);
    }

    bool resolve_pc(std::uint32_t pc, std::string_view& file, std::uint32_t& line, std::uint32_t& column) const {
        if (source != nullptr && pc < spans.size()) {
            Location loc = source->location(spans[pc].begin);
            file = loc.path;
            line = loc.line;
            column = loc.column;
            return true;
        }
        if (pc < span_locs.size() && !span_locs[pc].file.empty()) {
            file = span_locs[pc].file;
            line = span_locs[pc].line;
            column = span_locs[pc].column;
            return true;
        }
        return false;
    }

    std::string_view slice_pc(std::uint32_t pc) const {
        if (source != nullptr && pc < spans.size()) return source->slice(spans[pc]);
        if (pc < span_locs.size()) return span_locs[pc].text;
        return {};
    }

    std::string name;
    std::string key;               // stable across recompiles: name, or Function::stable_key for lambdas
    std::uint32_t index = 0;
    std::uint16_t param_count = 0; // including self
    std::uint16_t reg_count = 0;
    std::uint16_t capture_count = 0;
    std::vector<Instr> code;
    std::vector<Span> spans; // per instruction, for panics
    std::vector<SpanLoc> span_locs; // fallback when `source == nullptr` (AOT native builds)
    std::vector<LocalDebug> locals; // named locals by register and pc range, for panics
    // Source line of each instruction (0 = none), built on first use by the
    // debug hooks (VM::line_of); empty until then.
    mutable std::vector<std::uint32_t> debug_lines;
    std::vector<Value> consts; // owned references
    const SourceFile* source = nullptr;
    AotFn aot_fn = nullptr;
    bool compiled = false;
};

struct RecordRT {
    std::string name;
    bool is_struct = false;
    const RecordRT* base = nullptr;
    std::vector<std::uint32_t> vtable; // proto indices
    std::map<std::string, std::uint32_t, std::less<>> methods; // by name, incl. inherited (dynamic dispatch)
    std::vector<std::string> field_names;
    std::vector<std::string> field_types; // type names, for hot reload (DESIGN.md 5.13)
    std::vector<bool> field_exported;     // `@export` fields (scene files, DESIGN.md 4.12)
    std::uint32_t field_init = 0;         // classes: proto running the field initializers
    // Frees InstanceObj::native (pluma/bindings.hpp). Inherited by subclasses.
    void (*finalizer)(void*) = nullptr;
    // Object::flags for new instances: objflag::Untraced when the type is
    // acyclic, objflag::Promised for an unproven `@acyclic` (pluma/acyclic.hpp).
    std::uint8_t gc_flags = 0;
    std::vector<bool> field_weak; // `weak var` fields (LANGUAGE.md 2.11)
    bool is_subclass_of(const RecordRT* other) const {
        for (const RecordRT* r = this; r != nullptr; r = r->base) {
            if (r == other) return true;
        }
        return false;
    }
};

struct VariantRT {
    std::string name;
    std::vector<std::string> field_names;
    std::int64_t discriminant = 0;
};

struct EnumRT {
    std::string name;
    bool plain = true;
    std::uint8_t gc_flags = 0; // like RecordRT::gc_flags, for payload objects
    std::vector<VariantRT> variants;
};

// Human-readable listing of a function's code.
std::string disassemble(const Proto& p);

} // namespace pluma

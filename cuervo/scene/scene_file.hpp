#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Scene files (DESIGN.md 4.12): a node tree with the typed `@export` fields of
// each node, stored in a versioned binary format, with migrations from every
// older version and a stable text view (`scene dump`) for git diffs.
//
// This layer is independent of the script VM: it knows type *names* and
// self-describing values only. cuervo/script/scene_bridge.hpp converts
// between a SceneDoc and live script objects.
//
// Binary layout, version 2 (little-endian, see cuervo/core/binary_io.hpp):
//   header   "ESCN"  u32 version  u64 schema_hash
//   strings  u32 count, then count x (u32 length, bytes)  -- every string below
//            is a u32 index into this table, in first-use order
//   nodes    u32 count, then per node (pre-order, parents first):
//            u32 type  u32 name  i32 parent (-1 = the root)  u32 field_count
//            field_count x (u32 name, u32 type, value)
//   value    u8 kind, then by kind:
//            Null -; Bool u8; Int u8 num i64; Float u8 num (f32 | f64 by num);
//            Str/Name u32; Vec u32 n, n x f32; Enum u32 type, u32 variant;
//            Struct u32 type, u32 n, n x (u32 name, value); List u32 n, n x value
//
// Version history (each bump adds a migration, `migrations()`):
//   1  first format: strings stored inline, nodes had no names
//   2  string table; every node has a name (v1 -> v2 names nodes after their type)
//
// The schema hash fingerprints the (node type, field, field type) triples
// the file uses, so a loader can tell cheaply whether the scripts' schema
// changed since the file was saved.
namespace cuervo::scene {

// Same order as the language's number kinds (pluma::NumKind).
enum class NumKind : std::uint8_t { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64 };

enum class ValueKind : std::uint8_t { Null, Bool, Int, Float, Str, Name, Vec, Enum, Struct, List };

struct Field;

struct Value {
    ValueKind kind = ValueKind::Null;
    NumKind num = NumKind::I32;   // Int, Float
    bool b = false;               // Bool
    std::int64_t i = 0;           // Int (unsigned kinds bit-identical)
    double f = 0.0;               // Float (F32 values are exactly representable floats)
    std::string text;             // Str, Name; Enum: the variant name
    std::string type;             // Enum, Struct: the type name
    std::vector<float> vec;       // Vec
    std::vector<Field> fields;    // Struct, in declaration order
    std::vector<Value> items;     // List

    // Special members are defined out of line, where Field is complete.
    Value();
    ~Value();
    Value(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(const Value&);
    Value& operator=(Value&&) noexcept;

    static Value null();
    static Value boolean(bool v);
    static Value integer(std::int64_t v, NumKind k = NumKind::I32);
    static Value number(double v, NumKind k = NumKind::F32);
    static Value str(std::string v);
    static Value name(std::string v);
    static Value vector(std::vector<float> v);
    static Value enum_value(std::string type, std::string variant);
    static Value list(std::vector<Value> items = {});

    // Every member compared (scene_file.cpp, where Field is complete).
    friend bool operator==(const Value&, const Value&);
};

struct Field {
    std::string name;
    std::string type; // the declared type name (`float`, `List<Color>`); empty inside structs
    Value value;
    friend bool operator==(const Field&, const Field&) = default;
};


struct Node {
    std::string type;        // script class name
    std::string name;        // Node.name
    std::int32_t parent = -1; // index of the parent node (always lower), -1 for the root
    std::vector<Field> fields;
    friend bool operator==(const Node&, const Node&) = default;
};

// One scene: nodes in pre-order (a parent before its children, siblings in
// order); node 0 is the root.
struct SceneDoc {
    std::vector<Node> nodes;

    // Index of the first child of `parent` named `name`, or -1.
    int find(int parent, std::string_view name) const;
    friend bool operator==(const SceneDoc&, const SceneDoc&) = default;
};

inline constexpr std::uint32_t format_version = 2;
inline constexpr char magic[4] = {'E', 'S', 'C', 'N'};

// Checks parents (pre-order, one root). Empty string on success.
std::string validate(const SceneDoc& doc);

std::uint64_t schema_hash(const SceneDoc& doc);

// A step from `from` to `from + 1`, applied after decoding an old file.
struct Migration {
    std::uint32_t from = 0;
    const char* what = "";
    void (*apply)(SceneDoc&) = nullptr;
};
std::span<const Migration> migrations();

struct LoadInfo {
    std::uint32_t version = 0;      // as stored in the file
    std::uint64_t schema_hash = 0;  // as stored in the file
    std::vector<std::string> migrated; // descriptions of the migrations applied
};

// Encodes `doc` in the current format version.
std::vector<std::byte> write_binary(const SceneDoc& doc);
// Decodes any supported version and migrates it to the current one. On
// failure returns nullopt and sets `error`.
std::optional<SceneDoc> read_binary(std::span<const std::byte> data, std::string& error, LoadInfo* info = nullptr);

// Version 1 encoder, kept so tests and tools can produce old files and check
// that migrations keep working.
std::vector<std::byte> write_binary_v1(const SceneDoc& doc);

// The stable text view (`cuervo-tools scene dump`): one line per field, nodes
// nested by indentation, floats in shortest round-trip form, so equal scenes
// always dump to identical text and `git diff` shows real changes. The
// header carries the stored version and schema hash.
std::string dump_text(const SceneDoc& doc, const LoadInfo* info = nullptr);
// Parses the text view back (`cuervo-tools scene build`); `//` comments and
// the header lines are optional. On failure returns nullopt and sets `error`
// ("line N: ...").
std::optional<SceneDoc> parse_text(std::string_view text, std::string& error);

// One value in the text view's syntax (`(1.0, 2.0)`, `Color { r = 1.0, ... }`,
// `Align.Center`, `#name`, `"text"`, `[1, 2]`); the editor's inspector and
// command scripts use these. parse_value reads exactly one value (with the
// literal's own kind: `3` is an int, `3.0` a float) or returns nullopt and
// sets `error`.
std::string format_value(const Value& v);
std::optional<Value> parse_value(std::string_view text, std::string& error);

// File helpers.
std::optional<SceneDoc> load_file(const std::filesystem::path& path, std::string& error, LoadInfo* info = nullptr);
bool save_file(const std::filesystem::path& path, const SceneDoc& doc, std::string& error);

std::string_view num_kind_name(NumKind k); // "int", "u8", "float", "f64", ...

} // namespace cuervo::scene

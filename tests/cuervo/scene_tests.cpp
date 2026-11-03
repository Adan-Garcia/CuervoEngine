// Scene files (cuervo/scene/scene_file.hpp, DESIGN.md 4.12): binary
// round-trip, migrations from older versions, the text dump's stability and
// its parser, and rejection of broken files.

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <format>
#include <filesystem>
#include <limits>

#include "cuervo/core/hash.hpp"
#include "cuervo/scene/scene_file.hpp"

using namespace cuervo;
using namespace cuervo::scene;

namespace {

SceneDoc sample() {
    SceneDoc doc;
    Node root;
    root.type = "CanvasLayer";
    root.name = "menu";
    root.fields.push_back({"layer", "int", Value::integer(10)});
    root.fields.push_back({"always_process", "bool", Value::boolean(true)});
    doc.nodes.push_back(root);

    Node box;
    box.type = "VBox";
    box.name = "items";
    box.parent = 0;
    box.fields.push_back({"anchor_min", "vec2", Value::vector({0.5f, 0.5f})});
    box.fields.push_back({"separation", "float", Value::number(12.0)});
    box.fields.push_back({"align", "name", Value::name("center")});
    doc.nodes.push_back(box);

    Node button;
    button.type = "Button";
    button.name = "start \"quoted\"";
    button.parent = 1;
    button.fields.push_back({"text", "str", Value::str("Start\nline\ttab \\ \x01")});
    Value color;
    color.kind = ValueKind::Struct;
    color.type = "Color";
    color.fields = {{"r", "", Value::number(1.0)}, {"g", "", Value::number(0.1)}, {"b", "", Value::number(1e-7)},
                    {"a", "", Value::number(-0.0)}};
    button.fields.push_back({"tint", "Color", color});
    Value list;
    list.kind = ValueKind::List;
    list.items = {Value::integer(-3, NumKind::I8), Value::integer(-1, NumKind::U64), Value::integer(7, NumKind::I64)};
    button.fields.push_back({"codes", "List<i64>", list});
    button.fields.push_back({"mode", "Mode", Value::enum_value("Mode", "Fast")});
    button.fields.push_back({"precise", "f64", Value::number(0.1, NumKind::F64)});
    button.fields.push_back({"far", "float", Value::number(3.4e38)});
    button.fields.push_back({"inf", "float", Value::number(std::numeric_limits<double>::infinity())});
    button.fields.push_back({"odd", "name", Value::name("has space")});
    button.fields.push_back({"empty", "List<str>", Value::list()});
    button.fields.push_back({"v5", "vec5", Value::vector({1.0f, 2.0f, 3.0f, 4.0f, 5.0f})});
    doc.nodes.push_back(button);

    Node sibling;
    sibling.type = "Label";
    sibling.name = "hint";
    sibling.parent = 0;
    doc.nodes.push_back(sibling);
    return doc;
}

constexpr const char* sample_dump = R"(scene version 2 schema SCHEMA
node CanvasLayer "menu" {
    layer: int = 10
    always_process: bool = true
    node VBox "items" {
        anchor_min: vec2 = (0.5, 0.5)
        separation: float = 12.0
        align: name = #center
        node Button "start \"quoted\"" {
            text: str = "Start\nline\ttab \\ \u{1}"
            tint: Color = Color { r = 1.0, g = 0.1, b = 1e-07, a = -0.0 }
            codes: List<i64> = [-3i8, 18446744073709551615u64, 7i64]
            mode: Mode = Mode.Fast
            precise: f64 = 0.1f64
            far: float = 3.4e+38
            inf: float = inf
            odd: name = #"has space"
            empty: List<str> = []
            v5: vec5 = (1.0, 2.0, 3.0, 4.0, 5.0)
        }
    }
    node Label "hint" {
    }
}
)";

std::string expected_dump(const SceneDoc& doc) {
    std::string text = sample_dump;
    text.replace(text.find("SCHEMA"), 6, std::format("{:016x}", schema_hash(doc)));
    return text;
}

} // namespace

TEST_CASE("scene files: binary round-trip keeps every value exactly") {
    const SceneDoc doc = sample();
    CHECK(validate(doc).empty());
    std::vector<std::byte> bytes = write_binary(doc);
    std::string error;
    LoadInfo info;
    auto back = read_binary(bytes, error, &info);
    REQUIRE_MESSAGE(back.has_value(), error);
    CHECK(*back == doc);
    CHECK(info.version == format_version);
    CHECK(info.schema_hash == schema_hash(doc));
    CHECK(info.migrated.empty());
    CHECK(write_binary(*back) == bytes); // stable encoding
    CHECK(back->find(0, "items") == 1);
    CHECK(back->find(1, "nope") == -1);
}

TEST_CASE("scene files: the text dump is stable and parses back to the same scene") {
    const SceneDoc doc = sample();
    const std::string text = dump_text(doc);
    CHECK(text == expected_dump(doc));
    std::string error;
    auto parsed = parse_text(text, error);
    REQUIRE_MESSAGE(parsed.has_value(), error);
    CHECK(*parsed == doc);
    CHECK(dump_text(*parsed) == text);
    // Dumping the decoded binary gives the same text (the textconv view).
    LoadInfo info;
    auto back = read_binary(write_binary(doc), error, &info);
    REQUIRE(back.has_value());
    CHECK(dump_text(*back, &info) == text);

    // Comments, missing header and free spacing are accepted.
    auto loose = parse_text("// a menu\nnode Panel \"p\" {\n  margins : vec4 =( 1.0,2.0 ,3.0, 4.0 )\n"
                            "  node Button \"b\" { text: str = \"x\" }\n}\n",
                            error);
    REQUIRE_MESSAGE(loose.has_value(), error);
    REQUIRE(loose->nodes.size() == 2);
    CHECK(loose->nodes[0].fields[0].value.vec == std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f});
    CHECK(loose->nodes[1].parent == 0);
}

TEST_CASE("scene files: the schema hash ignores node order and values") {
    SceneDoc a = sample();
    SceneDoc b = sample();
    b.nodes[2].fields[0].value = Value::str("changed");
    CHECK(schema_hash(a) == schema_hash(b));
    b.nodes[2].fields[0].type = "name";
    CHECK(schema_hash(a) != schema_hash(b));
}

TEST_CASE("scene files: version 1 files migrate to the current version") {
    SceneDoc doc = sample();
    std::vector<std::byte> v1 = write_binary_v1(doc);
    std::string error;
    LoadInfo info;
    auto back = read_binary(v1, error, &info);
    REQUIRE_MESSAGE(back.has_value(), error);
    CHECK(info.version == 1);
    REQUIRE(info.migrated.size() == 1);
    CHECK(info.migrated[0].find("v1 -> v2") == 0);
    // v1 had no node names: the migration names nodes after their type.
    REQUIRE(back->nodes.size() == doc.nodes.size());
    for (std::size_t i = 0; i < doc.nodes.size(); ++i) {
        CHECK(back->nodes[i].name == doc.nodes[i].type);
        CHECK(back->nodes[i].fields == doc.nodes[i].fields);
        CHECK(back->nodes[i].parent == doc.nodes[i].parent);
    }
    // The dump says where it came from; upgrading writes the current version.
    CHECK(dump_text(*back, &info).find("scene version 1 schema") == 0);
    CHECK(dump_text(*back, &info).find("// migrated: v1 -> v2") != std::string::npos);
    LoadInfo again;
    REQUIRE(read_binary(write_binary(*back), error, &again).has_value());
    CHECK(again.version == format_version);
    CHECK(again.migrated.empty());
    // Every version below the current one has a migration.
    CHECK(migrations().size() == format_version - 1);
}

TEST_CASE("scene files: migrating version 1 numbers siblings of one type so every node has its own path") {
    SceneDoc doc;
    auto add = [&](const char* type, std::int32_t parent) {
        Node n;
        n.type = type;
        n.parent = parent;
        doc.nodes.push_back(n);
    };
    add("Node2D", -1);
    add("Shape", 0);
    add("Shape", 0);
    add("Node2D", 0);
    add("Shape", 3); // a different parent: starts again at "Shape"
    add("Shape", 0);
    std::string error;
    auto back = read_binary(write_binary_v1(doc), error);
    REQUIRE_MESSAGE(back.has_value(), error);
    std::vector<std::string> names;
    for (const Node& n : back->nodes) names.push_back(n.name);
    CHECK(names == std::vector<std::string>{"Node2D", "Shape", "Shape2", "Node2D", "Shape", "Shape3"});
    // Saved in the current version, the names stay.
    auto again = read_binary(write_binary(*back), error);
    REQUIRE(again.has_value());
    CHECK(*again == *back);
}

TEST_CASE("scene files: broken input is rejected with a reason") {
    std::string error;
    const std::vector<std::byte> good = write_binary(sample());

    CHECK_FALSE(read_binary({}, error).has_value());
    CHECK(error.find("bad magic") != std::string::npos);

    std::vector<std::byte> future = good;
    future[4] = std::byte{99};
    CHECK_FALSE(read_binary(future, error).has_value());
    CHECK(error.find("version 99 is not supported") != std::string::npos);

    for (std::size_t cut : {good.size() - 1, good.size() / 2, std::size_t{20}}) {
        std::vector<std::byte> truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(read_binary(truncated, error).has_value());
    }
    std::vector<std::byte> trailing = good;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(read_binary(trailing, error).has_value());
    CHECK(error.find("trailing data") != std::string::npos);

    SceneDoc orphan = sample();
    orphan.nodes[3].parent = 2; // the previous node: still pre-order
    CHECK(validate(orphan).empty());
    orphan.nodes[2].parent = 3;
    CHECK_FALSE(validate(orphan).empty());
    SceneDoc out_of_order = sample();
    out_of_order.nodes.push_back({"Label", "late", 1, {}}); // node 1's subtree already closed
    CHECK(validate(out_of_order).find("breaks pre-order") != std::string::npos);
    CHECK(validate(SceneDoc{}) == "the scene has no nodes");

    CHECK_FALSE(parse_text("node A \"a\" {\n x: int = \n}", error).has_value());
    CHECK(error.find("line 3") == 0);
    CHECK_FALSE(parse_text("node A \"a\" {}\nnode B \"b\" {}", error).has_value());
    CHECK(error.find("only one root") != std::string::npos);
    CHECK_FALSE(parse_text("node A \"a\" { v: vec2 = (1, 2) }", error).has_value());
    CHECK(error.find("float literals") != std::string::npos);
    CHECK_FALSE(parse_text("node A \"a\" { s: str = \"open }", error).has_value());
    CHECK(error.find("unterminated") != std::string::npos);
}

TEST_CASE("scene files: save_file / load_file and the shmup's menus") {
    const auto dir = std::filesystem::temp_directory_path() / "engine_scene_tests";
    std::filesystem::create_directories(dir);
    std::string error;
    REQUIRE(save_file(dir / "s.scene", sample(), error));
    auto back = load_file(dir / "s.scene", error);
    REQUIRE_MESSAGE(back.has_value(), error);
    CHECK(*back == sample());
    CHECK_FALSE(load_file(dir / "missing.scene", error).has_value());

    // The example's scene files are current and parse back from their dumps.
    for (const char* name : {"title", "pause", "options", "game_over"}) {
        LoadInfo info;
        auto doc = load_file(std::filesystem::path(CUERVO_EXAMPLES_DIR) / "shmup/scenes" / (std::string(name) + ".scene"),
                             error, &info);
        REQUIRE_MESSAGE(doc.has_value(), error);
        CHECK(info.version == format_version);
        auto parsed = parse_text(dump_text(*doc), error);
        REQUIRE_MESSAGE(parsed.has_value(), error);
        CHECK(*parsed == *doc);
    }
}

// ------------------------------------------------------------ text scenes (11.2c)

namespace {

std::filesystem::path unique_dir(const char* tag) {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() /
               std::format("cuervo_{}_{}_{}", tag, stamp, counter.fetch_add(1));
    std::filesystem::create_directories(dir);
    return dir;
}

std::vector<std::filesystem::path> example_scenes() {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(CUERVO_EXAMPLES_DIR)) {
        if (e.is_regular_file() && e.path().extension() == ".scene") out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uint64_t state_hash(const SceneDoc& doc) {
    const auto bytes = write_binary(doc);
    StateHasher h;
    h.bytes(bytes.data(), bytes.size());
    return h.digest();
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("text scenes: every example scene dumps, parses and dumps identically") {
    const auto scenes = example_scenes();
    REQUIRE(scenes.size() >= 10);
    for (const auto& path : scenes) {
        std::string error;
        auto doc = load_file(path, error);
        REQUIRE_MESSAGE(doc.has_value(), error);
        const std::string once = dump_text(*doc);
        auto parsed = parse_text(once, error);
        REQUIRE_MESSAGE(parsed.has_value(), path.string(), ": ", error);
        CHECK_MESSAGE(*parsed == *doc, path.string());
        CHECK_MESSAGE(dump_text(*parsed) == once, path.string());
    }
}

TEST_CASE("text scenes: binary and text files load to the same state hash") {
    const auto dir = unique_dir("scene_hash");
    std::string error;
    for (const auto& path : example_scenes()) {
        auto doc = load_file(path, error);
        REQUIRE_MESSAGE(doc.has_value(), error);
        REQUIRE(save_file(dir / "t.scene", *doc, error, SaveFormat::Text));
        REQUIRE(save_file(dir / "b.scene", *doc, error, SaveFormat::Binary));
        auto t = load_file(dir / "t.scene", error);
        auto b = load_file(dir / "b.scene", error);
        REQUIRE_MESSAGE(t.has_value(), error);
        REQUIRE_MESSAGE(b.has_value(), error);
        CHECK(state_hash(*t) == state_hash(*b));
        CHECK(state_hash(*t) == state_hash(*doc));
        CHECK(slurp(dir / "t.scene").starts_with("scene version 2\n"));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("text scenes: floats round-trip exactly, including extremes") {
    SceneDoc doc;
    Node n;
    n.type = "Node2D";
    n.name = "n";
    const float fl[] = {0.1f, 1.0f / 3.0f, 1e-38f, 3.4028235e38f, 1.4e-45f, -0.0f, 123456.789f, 16777217.0f};
    for (std::size_t i = 0; i < std::size(fl); ++i) {
        n.fields.push_back({std::format("f{}", i), "float", Value::number(fl[i], NumKind::F32)});
    }
    n.fields.push_back({"d", "f64", Value::number(0.1 + 0.2, NumKind::F64)});
    n.fields.push_back({"big", "f64", Value::number(1.7976931348623157e308, NumKind::F64)});
    n.fields.push_back({"tiny", "f64", Value::number(5e-324, NumKind::F64)});
    n.fields.push_back({"v", "vec3", Value::vector({0.1f, -1e-30f, 7.0e10f})});
    doc.nodes.push_back(n);
    std::string error;
    auto parsed = parse_text(dump_text(doc), error);
    REQUIRE_MESSAGE(parsed.has_value(), error);
    CHECK(state_hash(*parsed) == state_hash(doc));
    CHECK(dump_text(*parsed) == dump_text(doc));
}

TEST_CASE("text scenes: the loader sniffs the format and rejects malformed text") {
    const auto dir = unique_dir("scene_sniff");
    std::string error;
    REQUIRE(save_file(dir / "a.scene", sample(), error));
    REQUIRE(save_file(dir / "b.scene", sample(), error, SaveFormat::Binary));
    auto a = load_file(dir / "a.scene", error);
    auto b = load_file(dir / "b.scene", error);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == *b);
    {
        std::ofstream bad(dir / "c.scene", std::ios::binary);
        bad << "node Panel \"p\" {\n";
    }
    CHECK_FALSE(load_file(dir / "c.scene", error).has_value());
    CHECK(error.find("c.scene") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("text scenes: a three-way merge of two edits to a text scene is clean") {
    if (std::system("git --version >/dev/null 2>&1") != 0 && std::system("git --version >nul 2>&1") != 0) {
        WARN("git not available; merge test skipped");
        return;
    }
    const auto dir = unique_dir("scene_merge");
    const std::string d = "\"" + dir.string() + "\"";
    auto git = [&](const std::string& args) { return std::system(("git -C " + d + " " + args).c_str()); };
    REQUIRE(git("init -q") == 0);
    REQUIRE(git("config user.email t@example.com") == 0);
    REQUIRE(git("config user.name t") == 0);
    REQUIRE(git("config core.autocrlf false") == 0);
    REQUIRE(git("config commit.gpgsign false") == 0);

    const SceneDoc base = sample();
    std::string error;
    const auto file = dir / "level.scene";
    REQUIRE(save_file(file, base, error));
    REQUIRE(git("add level.scene") == 0);
    REQUIRE(git("commit -q -m base") == 0);
    REQUIRE(git("branch -M main") == 0);
    REQUIRE(git("checkout -q -b side") == 0);

    // side: change the root's layer (the first field)
    SceneDoc side = base;
    side.nodes[0].fields[0].value = Value::integer(99);
    REQUIRE(save_file(file, side, error));
    REQUIRE(git("commit -q -am side") == 0);

    // main: rename a node further down the file
    REQUIRE(git("checkout -q main") == 0);
    SceneDoc main_doc = base;
    main_doc.nodes.back().name = "renamed";
    REQUIRE(save_file(file, main_doc, error));
    REQUIRE(git("commit -q -am main") == 0);

    REQUIRE(git("merge -q side -m merge") == 0);
    auto merged = load_file(file, error);
    REQUIRE_MESSAGE(merged.has_value(), error);
    CHECK(merged->nodes.back().name == "renamed");
    CHECK(merged->nodes[0].fields[0].value.i == 99);
    CHECK(merged->nodes.size() == base.nodes.size());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

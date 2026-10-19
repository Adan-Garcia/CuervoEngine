// Scene files (cuervo/scene/scene_file.hpp, DESIGN.md 4.12): binary
// round-trip, migrations from older versions, the text dump's stability and
// its parser, and rejection of broken files.

#include <doctest.h>

#include <cmath>
#include <format>
#include <filesystem>
#include <limits>

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

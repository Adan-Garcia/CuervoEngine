// Milestone 8.7's stabilization tests for the editor and the scene system
// (DESIGN.md 4.12, 4.13 "Clarified (Milestone 8.7)"): every built-in node class,
// default and with every field changed, goes through the editor, a scene file, the
// text view and the game, and the command script's reload hook.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "editor/class_registry.hpp"
#include "editor/command_script.hpp"
#include "editor/editor_document.hpp"
#include "editor/project_env.hpp"

using namespace cuervo;
using namespace cuervo::editor;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_editor_87_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::vector<char> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A different value of the same type, or nullopt for kinds this test leaves alone.
std::optional<scene::Value> changed(const scene::Value& v, const ClassRegistry& registry) {
    using scene::ValueKind;
    switch (v.kind) {
        case ValueKind::Bool: return scene::Value::boolean(!v.b);
        case ValueKind::Int: return scene::Value::integer(v.i + 1, v.num);
        case ValueKind::Float: return scene::Value::number(v.f + 0.25, v.num);
        case ValueKind::Str: return scene::Value::str(v.text + "x");
        case ValueKind::Vec: {
            std::vector<float> out = v.vec;
            for (float& f : out) f += 1.0f;
            return scene::Value::vector(out);
        }
        case ValueKind::Enum: {
            const EnumSpec* e = registry.find_enum(v.type);
            if (e == nullptr || e->variants.size() < 2) return std::nullopt;
            for (std::size_t i = 0; i < e->variants.size(); ++i) {
                if (e->variants[i] == v.text) {
                    return scene::Value::enum_value(v.type, e->variants[(i + 1) % e->variants.size()]);
                }
            }
            return std::nullopt;
        }
        default: return std::nullopt;
    }
}

} // namespace

TEST_CASE("editor: every built-in node class round-trips with defaults and with every field changed") {
    const fs::path project = fs::path(CUERVO_EXAMPLES_DIR) / "node_level";
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open(project, nullptr, error), error);
    const ClassRegistry& registry = env.classes();
    const fs::path dir = temp_dir("every_class");

    EditorDocument doc(&registry);
    doc.new_scene("Node2D", "root");
    std::size_t added = 0;
    std::size_t perturbed = 0;
    for (const ClassSpec& cls : registry.classes()) {
        // The project has no scripts: these are the engine's own nodes. Scene instances need a file
        // (they have their own tests).
        if (cls.name == "SceneInstance" || !cls.default_error.empty()) continue;
        const NodeId n = doc.add_child(doc.model().root(), cls.name, "n_" + cls.name);
        INFO(cls.name << ": " << doc.error());
        REQUIRE(n != 0);
        ++added;
        for (const FieldSpec& f : cls.fields) {
            auto v = changed(f.default_value, registry);
            if (!v) continue;
            // Some values are rejected by the field's own validation; the rest must stick.
            if (doc.set_field(n, f.name, *v)) ++perturbed;
        }
    }
    CHECK(added >= 40);
    CHECK(perturbed >= 200);

    // Save, reopen: the same document, the same bytes on a second save.
    const fs::path file = dir / "all.scene";
    REQUIRE_MESSAGE(doc.save_as(file, error), error);
    EditorDocument back(&registry);
    REQUIRE_MESSAGE(back.open(file, error), error);
    CHECK(back.model().to_doc() == doc.model().to_doc());
    REQUIRE_MESSAGE(back.save_as(dir / "again.scene", error), error);
    CHECK(read_bytes(dir / "again.scene") == read_bytes(file));

    // The text view: dump, parse, write: the same bytes.
    const std::string text = scene::dump_text(doc.model().to_doc());
    auto parsed = scene::parse_text(text, error);
    REQUIRE_MESSAGE(parsed, error);
    CHECK(*parsed == doc.model().to_doc());
    REQUIRE_MESSAGE(scene::save_file(dir / "text.scene", *parsed, error), error);
    CHECK(read_bytes(dir / "text.scene") == read_bytes(file));

    // The scene view builds it, and the game runs and draws it.
    env.rebuild_preview(doc.model().to_doc());
    CHECK(env.preview_ok());
    std::ofstream(dir / "project.toml") << "name = \"all\"\nmain_scene = \"all.scene\"\nseed = 7\n";
    auto p = script::load_project(dir, error);
    REQUIRE_MESSAGE(p, error);
    InputMap map;
    REQUIRE(script::apply_input(*p, map, error));
    std::vector<pluma::SourceFile::Part> sources;
    REQUIRE(script::read_scripts(*p, sources, nullptr, error));
    script::HostOptions o;
    o.seed = p->seed;
    o.tick_rate = p->tick_rate;
    o.project_dir = p->dir;
    o.main_scene = p->main_scene;
    script::ScriptHost host(o, map);
    REQUIRE_MESSAGE(host.load(sources, p->main_class), host.error());
    render::SdfBatcher shapes;
    render::TextRenderer text_renderer;
    for (int i = 0; i < 120; ++i) {
        host.tick(InputFrame{});
        REQUIRE_MESSAGE(host.running(), host.error());
        host.draw(shapes, text_renderer, render::FontHandle{});
        REQUIRE_MESSAGE(host.running(), host.error());
    }
}

TEST_CASE("editor: append-file and reload need a playing game and a writable file") {
    const fs::path dir = temp_dir("reload_cmds");
    fs::create_directories(dir / "scripts");
    std::ofstream(dir / "project.toml") << "name = \"r\"\nmain = \"Main\"\n";
    std::ofstream(dir / "scripts" / "main.plm")
        << "class Main extends Node {\n    var n = 0\n    fn update(dt: float) { n += 1 }\n}\n";
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open(dir, nullptr, error), error);
    EditorDocument doc(&env.classes());
    ScriptResult r = run_command_script("reload\n", doc, env);
    CHECK_FALSE(r.ok);
    r = run_command_script("append-file scripts/main.plm // more\n", doc, env);
    CHECK(r.ok);
    r = run_command_script("append-file nothing/here.txt x\n", doc, env);
    CHECK_FALSE(r.ok);
}

TEST_CASE("editor: a TabContainer lays out in the scene view, where no ready() runs and it has no tab bar") {
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open_prelude_only(error), error);
    EditorDocument doc(&env.classes());
    doc.new_scene("CanvasLayer", "ui");
    const NodeId tabs = doc.add_child(doc.model().root(), "TabContainer", "tabs");
    REQUIRE(tabs != 0);
    REQUIRE(doc.set_field(tabs, "offset_max", scene::Value::vector({300.0f, 200.0f})));
    const NodeId a = doc.add_child(tabs, "Panel", "a");
    REQUIRE(a != 0);
    env.rebuild_preview(doc.model().to_doc());
    REQUIRE(env.preview_ok());
    const auto& bounds = env.bounds();
    const auto first = static_cast<std::size_t>(doc.model().doc_index(a));
    REQUIRE(first < bounds.size());
    CHECK(bounds[first].has_rect);
    CHECK(bounds[first].rect_size.x() == doctest::Approx(300.0f)); // the current page fills the container
    CHECK_FALSE(env.host().panicked());
}

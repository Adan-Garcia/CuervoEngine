// Milestone 11.7c's editor tests (DESIGN.md 4.13): the raster tooling (image info,
// atlas editing, sprite-sheet slicing) and a sprite scene assembled by command
// script alone, no code: Sprite2D, AnimatedSprite2D, NinePatchRect and a particle
// emitter, saved, reloaded and resolved through the engine's sprite library.

#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/render/sprite_library.hpp"
#include "editor/asset_library.hpp"
#include "editor/command_script.hpp"
#include "editor/editor_document.hpp"
#include "editor/project_env.hpp"
#include "editor/sprite_tools.hpp"

using namespace cuervo;
using namespace cuervo::editor;
namespace fs = std::filesystem;

namespace {

// A QOI image of `frames` square frames in a row, each a flat color (every pixel an RGBA op).
std::string make_qoi(int width, int height, int frame_w) {
    std::string out = "qoif";
    for (int v : {width, height}) {
        for (int shift : {24, 16, 8, 0}) out += static_cast<char>((v >> shift) & 0xFF);
    }
    out += static_cast<char>(4);
    out += static_cast<char>(0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int frame = x / frame_w;
            out += static_cast<char>(0xFF);
            out += static_cast<char>(40 + frame * 50);
            out += static_cast<char>(y * 8);
            out += static_cast<char>(200);
            out += static_cast<char>(255);
        }
    }
    for (int i = 0; i < 7; ++i) out += static_cast<char>(0);
    out += static_cast<char>(1);
    return out;
}

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path make_project(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    write_file(dir / "project.toml",
               "name = \"Sprites\"\nmain_scene = \"scenes/level.scene\"\nvirtual_size = [640, 360]\n"
               "tick_rate = 60\nseed = 7\nclear_color = \"10131c\"\n");
    write_file(dir / "art/sheet.qoi", make_qoi(64, 16, 16)); // four 16x16 frames
    write_file(dir / "art/panel.qoi", make_qoi(24, 24, 24));
    return dir;
}

} // namespace

TEST_CASE("image_info reads QOI sizes and refuses paths outside the project") {
    const fs::path dir = make_project("cuervo_117c_info");
    const ImageInfo info = image_info(dir, "art/sheet.qoi");
    REQUIRE_MESSAGE(info.ok, info.error);
    CHECK(info.width == 64);
    CHECK(info.height == 16);
    CHECK_FALSE(image_info(dir, "../outside.png").ok);
    CHECK_FALSE(image_info(dir, "art/missing.png").ok);
    CHECK(asset_kind_of("a/b.qoi") == AssetKind::Image);
    CHECK(asset_kind_of("a/hero.atlas") == AssetKind::Atlas);
    fs::remove_all(dir);
}

TEST_CASE("AtlasEditor slices a sheet, names frames, saves and reopens identically") {
    const fs::path dir = make_project("cuervo_117c_atlas");
    AtlasEditor atlas = AtlasEditor::create(dir, "art/hero.atlas");
    std::string error;
    assets::GridOptions grid;
    grid.frame_w = 16;
    grid.frame_h = 16;
    grid.prefix = "run";
    REQUIRE_MESSAGE(atlas.add_sheet("art/sheet.qoi", grid, error), error);
    REQUIRE_MESSAGE(atlas.add_sprite("art/panel.qoi", "panel", 1.0f, error), error);
    CHECK(atlas.frame_names() == std::vector<std::string>{"panel", "run_0", "run_1", "run_2", "run_3"});
    const auto slices = atlas.sheet_frames(0, error);
    REQUIRE_MESSAGE(slices.has_value(), error);
    CHECK(slices->frames[2].x == 32);

    // A grid that doesn't fit, a missing image and a bad scale are refused with a reason.
    assets::GridOptions bad;
    bad.frame_w = 100;
    bad.frame_h = 100;
    CHECK_FALSE(atlas.add_sheet("art/sheet.qoi", bad, error));
    CHECK_FALSE(atlas.add_sprite("art/nope.png", "", 1.0f, error));
    CHECK_FALSE(atlas.add_sprite("art/panel.qoi", "x", 0.0f, error));
    CHECK(atlas.spec().sheets.size() == 1);

    REQUIRE_MESSAGE(atlas.save(error), error);
    const std::string first = read_file(dir / "art/hero.atlas");
    auto again = AtlasEditor::open(dir, "art/hero.atlas", error);
    REQUIRE_MESSAGE(again.has_value(), error);
    CHECK(again->to_json() == first); // byte-deterministic round trip
    CHECK(again->frame_names() == atlas.frame_names());
    std::string names_error;
    CHECK(atlas_frame_names(dir, "art/hero.atlas", names_error).size() == 5);
    CHECK_FALSE(again->remove_sheet(3));
    CHECK(again->remove_sprite(0));
    CHECK(again->frame_names().size() == 4);
    fs::remove_all(dir);
}

TEST_CASE("a sprite scene is assembled by command script and loads back") {
    const fs::path dir = make_project("cuervo_117c_scene");
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open(dir, nullptr, error), error);
    EditorDocument doc(&env.classes());
    const char* script = R"(
atlas new art/hero.atlas
atlas sheet art/sheet.qoi 16 16 run
atlas sprite art/panel.qoi panel
atlas expect-frames panel run_0 run_1 run_2 run_3
atlas save

new Node2D level
add / Sprite2D hero
set . asset "art/hero.atlas"
set . frame "run_1"
set . tint "ff8000"
move . 100 80
add / AnimatedSprite2D runner
frames . art/hero.atlas run_
set . fps 8.0
move . 200 80
add / NinePatchRect window
set . asset "art/hero.atlas"
set . frame "panel"
set . size (120.0, 60.0)
set . margin_left 6
move . 300 40
add / Emitter2D sparks
set . pattern Burst
set . count 24
set . life 0.8
set . gravity (0.0, 90.0)
set . fade true
move . 400 200
expect /runner fps 8.0
save scenes/level.scene
)";
    const ScriptResult r = run_command_script(script, doc, env);
    REQUIRE_MESSAGE(r.ok, "line " << r.line << ": " << r.error);
    CHECK(fs::exists(dir / "art/hero.atlas"));
    CHECK(fs::exists(dir / "scenes/level.scene"));

    // Reopened from disk: the same values; the frames list is the four run frames.
    EditorDocument back(&env.classes());
    REQUIRE_MESSAGE(back.open(dir / "scenes/level.scene", error), error);
    const NodeId runner = back.model().find_path("/runner");
    REQUIRE(runner != 0);
    const scene::Value frames = back.field_value(runner, "frames");
    REQUIRE(frames.kind == scene::ValueKind::List);
    CHECK(frames.items.size() == 4);
    CHECK(back.field_value(back.model().find_path("/hero"), "frame") == scene::Value::str("run_1"));

    // The engine resolves the atlas frames the scene names (what the scene view draws).
    render::SpriteLibrary library;
    const auto vfs = assets::Vfs::dev(dir);
    REQUIRE(vfs != nullptr);
    const render::SpriteRef frame = library.resolve(*vfs, "art/hero.atlas", "run_2", render::SpriteFilter::Nearest, 1.0f);
    CHECK(frame.ok);
    CHECK(frame.size.x() == 16.0f);
    CHECK(frame.size.y() == 16.0f);
    fs::remove_all(dir);
}

TEST_CASE("atlas command errors name the problem") {
    const fs::path dir = make_project("cuervo_117c_errors");
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open(dir, nullptr, error), error);
    EditorDocument doc(&env.classes());
    CHECK_FALSE(run_command_script("atlas sprite art/panel.qoi", doc, env).ok); // no atlas open
    const ScriptResult r = run_command_script("atlas new a.atlas\natlas sheet art/sheet.qoi 100 100", doc, env);
    CHECK_FALSE(r.ok);
    CHECK(r.line == 2);
    CHECK_FALSE(run_command_script("atlas new a.atlas\natlas expect-frames x", doc, env).ok);
    doc.new_scene("Node2D", "root");
    doc.add_child(doc.model().root(), "AnimatedSprite2D", "a");
    CHECK_FALSE(run_command_script("atlas new b.atlas\natlas save\nframes /a b.atlas", doc, env).ok); // no frames
    fs::remove_all(dir);
}

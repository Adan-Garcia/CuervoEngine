// Milestone 11.7b's editor tests (DESIGN.md 4.13): OS file drops imported into the
// project (the same function the window's drop event and the `drop` command
// run), saved layouts with floating panels, and the UI scale range.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "editor/asset_library.hpp"
#include "editor/command_script.hpp"
#include "editor/editor_document.hpp"
#include "editor/file_import.hpp"
#include "editor/layouts.hpp"
#include "editor/project_env.hpp"
#include "editor/settings.hpp"

using namespace cuervo;
using namespace cuervo::editor;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

// A 2x2 QOI image (every pixel an RGBA op).
std::string tiny_qoi() {
    std::string o = "qoif";
    for (int v : {2, 2}) {
        for (int shift : {24, 16, 8, 0}) o += static_cast<char>((v >> shift) & 0xFF);
    }
    o += static_cast<char>(4);
    o += static_cast<char>(0);
    for (int i = 0; i < 4; ++i) {
        o += static_cast<char>(0xFF);
        o += static_cast<char>(200);
        o += static_cast<char>(30);
        o += static_cast<char>(60);
        o += static_cast<char>(255);
    }
    for (int i = 0; i < 7; ++i) o += static_cast<char>(0);
    o += static_cast<char>(1);
    return o;
}

fs::path fresh(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir / "project");
    fs::create_directories(dir / "outside");
    write(dir / "project/project.toml", "name = \"Drop\"\nvirtual_size = [320, 180]\ntick_rate = 60\nseed = 1\n");
    return dir;
}

} // namespace

TEST_CASE("dropped files are imported by kind, never overwrite and refuse what is not an asset") {
    const fs::path root = fresh("cuervo_117b_import");
    const fs::path project = root / "project";
    write(root / "outside/hero.qoi", tiny_qoi());
    write(root / "outside/logo.svg", "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>");
    write(root / "outside/boom.wav", "RIFF....WAVE");
    write(root / "outside/ui.ttf", "font bytes");
    write(root / "outside/notes.txt", "hello");
    write(root / "outside/broken.qoi", "not an image at all");
    write(root / "outside/fake.svg", "<html></html>");

    const std::vector<fs::path> files{root / "outside/hero.qoi", root / "outside/logo.svg", root / "outside/boom.wav",
                                      root / "outside/ui.ttf", root / "outside/notes.txt", root / "outside/broken.qoi",
                                      root / "outside/fake.svg", root / "outside/missing.png"};
    const std::vector<ImportResult> r = import_files(project, files);
    REQUIRE(r.size() == files.size());
    CHECK(r[0].ok);
    CHECK(r[0].dest == "art/hero.qoi");
    CHECK(r[1].dest == "art/logo.svg");
    CHECK(r[2].dest == "sounds/boom.wav");
    CHECK(r[3].dest == "fonts/ui.ttf");
    CHECK_FALSE(r[4].ok); // a text file is not an asset
    CHECK_FALSE(r[5].ok); // not a decodable image: removed again
    CHECK_FALSE(fs::exists(project / "art/broken.qoi"));
    CHECK_FALSE(r[6].ok); // no <svg> element
    CHECK_FALSE(fs::exists(project / "art/fake.svg"));
    CHECK_FALSE(r[7].ok);
    CHECK(fs::exists(project / "art/hero.qoi"));

    // The same file again gets a new name; an explicit folder is honored but must stay inside.
    const std::vector<ImportResult> again = import_files(project, {root / "outside/hero.qoi"});
    CHECK(again[0].dest == "art/hero_2.qoi");
    const std::vector<ImportResult> in_folder = import_files(project, {root / "outside/hero.qoi"}, "sprites/hero");
    CHECK(in_folder[0].dest == "sprites/hero/hero.qoi");
    CHECK_FALSE(import_files(project, {root / "outside/hero.qoi"}, "../escape")[0].ok);
    CHECK(import_folder(AssetKind::Scene).empty());
    fs::remove_all(root);
}

TEST_CASE("the drop command imports a file and the asset browser sees it") {
    const fs::path root = fresh("cuervo_117b_drop");
    const fs::path project = root / "project";
    write(root / "outside/coin.qoi", tiny_qoi());
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open(project, nullptr, error), error);
    EditorDocument doc(&env.classes());
    AssetLibrary assets;
    assets.open(project);
    CommandContext ctx{doc, env, nullptr, nullptr, nullptr, {}, 0};
    ctx.assets = &assets;
    const std::string script = "drop \"" + (root / "outside/coin.qoi").generic_string() +
                               "\"\nasset expect art/coin.qoi\ndrop \"" + (root / "outside/coin.qoi").generic_string() +
                               "\" props\nasset expect props/coin.qoi\n";
    const ScriptResult r = run_command_script(script, ctx);
    REQUIRE_MESSAGE(r.ok, "line " << r.line << ": " << r.error);
    CHECK(assets.find("art/coin.qoi")->kind == AssetKind::Image);

    const ScriptResult bad = run_command_script("drop \"" + (root / "outside/nothing.png").generic_string() + "\"", ctx);
    CHECK_FALSE(bad.ok);
    CHECK_FALSE(run_command_script("drop", ctx).ok);
    fs::remove_all(root);
}

TEST_CASE("a saved layout keeps floating panels and hidden panels across a restart") {
    LayoutStore store;
    Layout mine;
    mine.left = 0.3f;
    mine.right = 0.2f;
    mine.set_hidden("Sprites", true);
    mine.set_hidden("Particles", true);
    // ImGui's ini text: a floating window with its own position and size, and a dock node.
    mine.dock = "[Window][Sprites]\nPos=1500,120\nSize=640,360\nCollapsed=0\n\n[Docking][Data]\nDockNode ID=0x1 Pos=0,0 Size=1600,900\n";
    std::string error;
    REQUIRE_MESSAGE(store.save_as("Two monitors", mine, error), error);
    store.set_active("Two monitors");
    store.set_current(mine);

    const fs::path file = fs::temp_directory_path() / "cuervo_117b_layouts" / "editor-layouts.txt";
    fs::remove_all(file.parent_path());
    fs::create_directories(file.parent_path());
    REQUIRE_MESSAGE(store.save(file, error), error);

    LayoutStore reloaded;
    REQUIRE_MESSAGE(reloaded.load(file, error), error);
    CHECK(reloaded.active() == "Two monitors");
    CHECK(reloaded.layout("Two monitors") == mine);
    CHECK(reloaded.layout("Two monitors").dock.find("Pos=1500,120") != std::string::npos);
    CHECK(reloaded.current().is_hidden("Particles"));
    CHECK_FALSE(reloaded.current().is_hidden("Console"));
    // Saved again, the file is byte-identical.
    const fs::path second = file.parent_path() / "again.txt";
    REQUIRE(reloaded.save(second, error));
    std::string ta, tb;
    {
        // Closed before remove_all below: Windows refuses to delete a directory holding open files.
        std::ifstream a(file, std::ios::binary);
        std::ifstream b(second, std::ios::binary);
        ta.assign(std::istreambuf_iterator<char>(a), std::istreambuf_iterator<char>());
        tb.assign(std::istreambuf_iterator<char>(b), std::istreambuf_iterator<char>());
    }
    CHECK(ta == tb);
    fs::remove_all(file.parent_path());
}

TEST_CASE("the UI scale setting round-trips up to 200 percent") {
    EditorSettings settings;
    settings.set_ui_scale(2.0f);
    settings.set_font_scale(2.0f);
    EditorSettings loaded;
    loaded.parse(settings.serialize());
    CHECK(loaded.ui_scale() == doctest::Approx(2.0f));
    CHECK(loaded.font_scale() == doctest::Approx(2.0f));
}

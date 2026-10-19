// Phase 6B of the editor, headless: play-in-editor on the runtime's own game
// path (same state hash as `cuervo --game`), Stop leaving the edited scene
// untouched, recording and verifying replays, panics with trace and locals,
// the input-map editor's project.toml round trip, asset operations, scene
// instancing, the debugger's live tree, and the command-script verbs that
// drive all of it.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cuervo/jobs/job_system.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/game_session.hpp"
#include "cuervo/script/project.hpp"
#include "editor/asset_library.hpp"
#include "editor/command_script.hpp"
#include "editor/editor_document.hpp"
#include "editor/input_map_doc.hpp"
#include "editor/live_tree.hpp"
#include "editor/play_controller.hpp"
#include "editor/project_env.hpp"

using namespace cuervo;
using namespace cuervo::editor;
namespace fs = std::filesystem;

namespace {

const fs::path examples = fs::path(CUERVO_EXAMPLES_DIR);
const std::string font = CUERVO_DEFAULT_FONT;
constexpr std::uint64_t shmup_hash_240 = 0xa3e57e79964421d3ull; // `cuervo --game examples/shmup` at tick 240

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_editor_6b_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A copy of a shipped example (tests edit it).
fs::path copy_example(std::string_view name, std::string_view as) {
    const fs::path dir = temp_dir(as);
    fs::copy(examples / std::string(name), dir, fs::copy_options::recursive);
    return dir;
}

script::SessionOptions shmup_play(unsigned jobs = 1) {
    script::SessionOptions o;
    o.dir = examples / "shmup";
    o.font_path = font;
    o.jobs = jobs;
    return o;
}

struct Editor {
    ProjectEnv env;
    EditorDocument doc;
    PlayController play;
    InputMapDoc input;
    AssetLibrary assets;
    explicit Editor(const fs::path& dir) {
        std::string error;
        REQUIRE_MESSAGE(env.open(dir, nullptr, error), error);
        doc.set_registry(&env.classes());
        REQUIRE_MESSAGE(input.load(dir, error), error);
        assets.open(dir);
    }
    ScriptResult run(std::string_view script) {
        CommandContext ctx{doc, env, &play, &input, &assets, font, 1};
        return run_command_script(script, ctx);
    }
};

} // namespace

// ------------------------------------------------------------ play-in-editor

TEST_CASE("play-in-editor: the shmup's state hash at tick 240 equals the runtime's") {
    PlayController play;
    std::string error;
    REQUIRE_MESSAGE(play.start(shmup_play(), error), error);
    CHECK(play.state() == PlayState::Playing);
    DeviceState devices;
    play.step(devices, {0.0f, 0.0f}, 240);
    CHECK(play.tick() == 240);
    CHECK(play.state_hash() == shmup_hash_240);
    CHECK(play.average_tick_ms() > 0.0);
    // Real time at the fixed rate: half a second of 60 Hz frames is ~30
    // ticks (a single long frame runs at most 8: the spiral-of-death guard).
    for (int i = 0; i < 30; ++i) play.update(1.0 / 60.0, devices, {0.0f, 0.0f});
    CHECK(play.tick() >= 269);
    CHECK(play.tick() <= 271);
    play.update(0.5, devices, {0.0f, 0.0f});
    const std::uint64_t t = play.tick();
    play.pause();
    play.update(1.0, devices, {0.0f, 0.0f});
    CHECK(play.tick() == t);
    play.stop();
    CHECK_FALSE(play.active());
    CHECK(play.session() == nullptr);
}

TEST_CASE("play-in-editor: Stop leaves the edited scene byte-identical") {
    const fs::path dir = copy_example("shmup", "stop_restores");
    Editor ed(dir);
    std::string error;
    REQUIRE_MESSAGE(ed.doc.open(dir / "scenes/title.scene", error), error);
    // An unsaved edit: play runs it, stop keeps it, nothing else changes.
    REQUIRE(ed.doc.set_field(ed.doc.model().root(), "layer", scene::Value::integer(11)));
    ed.doc.seal();
    const std::vector<std::byte> before = scene::write_binary(ed.doc.model().to_doc());
    const std::string file_before = read_text(dir / "scenes/title.scene");
    const std::uint64_t revision = ed.doc.revision();
    const std::size_t undo = ed.doc.stack().size();
    ed.env.rebuild_preview(ed.doc.model().to_doc());
    const std::size_t preview_nodes = ed.env.bounds().size();

    auto options = shmup_play();
    options.dir = dir;
    options.scene = std::make_shared<const scene::SceneDoc>(ed.doc.model().to_doc());
    options.scene_label = "scenes/title.scene";
    REQUIRE_MESSAGE(ed.play.start(options, error), error);
    DeviceState devices;
    devices.set_key(Key::Down, true); // the menu reacts: the game's tree changes
    ed.play.step(devices, {640.0f, 360.0f}, 120);
    CHECK(ed.play.tick() == 120);
    ed.play.stop();

    CHECK(scene::write_binary(ed.doc.model().to_doc()) == before);
    CHECK(read_text(dir / "scenes/title.scene") == file_before);
    CHECK(ed.doc.revision() == revision);
    CHECK(ed.doc.stack().size() == undo);
    CHECK(ed.doc.dirty()); // still the unsaved edit, nothing more
    CHECK(ed.env.bounds().size() == preview_nodes);
    // And through the command verbs.
    const ScriptResult r = ed.run("play scene\nstep 30\nexpect-state playing\nstop\nexpect-state stopped\n");
    CHECK_MESSAGE(r.ok, r.line << ": " << r.error);
    CHECK(scene::write_binary(ed.doc.model().to_doc()) == before);
}

TEST_CASE("play-in-editor: a recorded run replays on 1 and 8 threads; the timeline seeks") {
    PlayController play;
    std::string error;
    REQUIRE(play.start(shmup_play(), error));
    play.start_recording();
    DeviceState devices;
    for (int i = 0; i < 360; ++i) {
        devices.set_key(Key::Enter, i == 20);   // start the game from the title
        devices.set_key(Key::Z, i >= 100 && i < 250);
        devices.set_key(Key::Right, i >= 120 && i < 180);
        play.step(devices, {static_cast<float>(i * 3), 200.0f}, 1);
    }
    play.stop(); // keeps the recording
    const Replay recorded = play.recorded();
    REQUIRE(recorded.ticks() == 360);
    REQUIRE(recorded.hashes.size() == 360);

    const fs::path dir = temp_dir("replays");
    REQUIRE(save_replay(dir / "run.replay", recorded, error));
    auto loaded = load_replay(dir / "run.replay", error);
    REQUIRE_MESSAGE(loaded, error);
    for (unsigned jobs : {1u, 8u}) {
        INFO("jobs " << jobs);
        const script::ReplayCheck c = script::verify_replay(examples / "shmup", *loaded, shmup_play(jobs));
        CHECK_MESSAGE(c.error.empty(), c.error);
        CHECK(c.ok);
        CHECK(c.compared == 360);
    }

    // The viewer: play it back, seek back and forth, compare every hash.
    REQUIRE_MESSAGE(play.start_replay(shmup_play(8), *loaded, error), error);
    CHECK(play.mode() == PlayMode::Replay);
    CHECK(play.state() == PlayState::Paused);
    REQUIRE(play.seek(200, error));
    CHECK(play.tick() == 200);
    CHECK(play.state_hash() == recorded.hashes[199]);
    REQUIRE(play.seek(50, error)); // backwards: re-simulated from the start
    CHECK(play.tick() == 50);
    CHECK(play.state_hash() == recorded.hashes[49]);
    play.resume();
    play.speed = 4.0f;
    for (int i = 0; i < 60; ++i) play.update(1.0 / 60.0, devices, {}); // ~240 ticks at 4x
    CHECK(play.tick() >= 288);
    CHECK(play.tick() <= 292);
    REQUIRE(play.seek(360, error));
    CHECK(play.verified() == 360);
    play.pause();
    CHECK_FALSE(play.first_mismatch());
    play.resume();
    CHECK(play.state() == PlayState::Paused); // at the end
    jobs::configure(1);

    // A tampered recording is flagged at the right tick.
    Replay bad = *loaded;
    bad.hashes[299] ^= 0x10;
    REQUIRE(play.start_replay(shmup_play(), bad, error));
    REQUIRE(play.seek(360, error));
    REQUIRE(play.first_mismatch());
    CHECK(*play.first_mismatch() == 300);
    CHECK(play.mismatch_expected() == bad.hashes[299]);
    CHECK(play.mismatch_actual() == recorded.hashes[299]);
    play.stop();
}

TEST_CASE("play-in-editor: a panic pauses the game with its stack trace and locals") {
    const fs::path dir = temp_dir("panic");
    write_text(dir / "project.toml", "name = \"P\"\nmain = \"Main\"\n");
    write_text(dir / "scripts/main.plm",
               "class Main extends Node2D {\n"
               "    var hp = 3\n"
               "    fn hurt(amount: int) {\n"
               "        var left = hp - amount\n"
               "        var parts = [1, 2]\n"
               "        print(parts[left])\n"
               "    }\n"
               "    fn update(dt: float) {\n"
               "        if Time.tick == 5 {\n"
               "            hurt(7)\n"
               "        }\n"
               "    }\n"
               "}\n");
    PlayController play;
    script::SessionOptions o;
    o.dir = dir;
    o.jobs = 1;
    std::string error;
    REQUIRE_MESSAGE(play.start(o, error), error);
    DeviceState devices;
    play.step(devices, {}, 20);
    CHECK(play.tick() == 5); // stopped at the panic
    CHECK(play.state() == PlayState::Paused);
    REQUIRE(play.panic());
    const pluma::PanicInfo& p = *play.panic();
    CHECK(p.message == "index -4 is out of bounds for a list of length 2");
    REQUIRE(p.frames.size() >= 2);
    CHECK(p.frames[0].function == "Main.hurt");
    CHECK(p.frames[0].file == "scripts/main.plm");
    CHECK(p.frames[0].line == 6);
    std::string locals;
    for (const pluma::PanicLocal& l : p.frames[0].locals) locals += l.name + ": " + l.type + " = " + l.value + "\n";
    CHECK(locals.find("amount: int = 7\n") != std::string::npos);
    CHECK(locals.find("left: int = -4\n") != std::string::npos);
    CHECK(locals.find("parts: List<int> = [1, 2]\n") != std::string::npos);
    CHECK(locals.find("self: Main = Main {") != std::string::npos);
    CHECK(p.frames[1].function == "Main.update");
    // Resuming is refused until the script is fixed; a hot reload fixes it.
    play.resume();
    CHECK(play.state() == PlayState::Paused);
    write_text(dir / "scripts/main.plm",
               "class Main extends Node2D {\n    var hp = 3\n    fn update(dt: float) { hp += 1 }\n}\n");
    CHECK(play.reload(true) == script::GameSession::Reload::Reloaded);
    CHECK_FALSE(play.panic());
    play.resume();
    CHECK(play.state() == PlayState::Playing);
    play.step(devices, {}, 3);
    CHECK(play.tick() == 8);
}

TEST_CASE("play-in-editor: script errors at load are reported with file:line") {
    const fs::path dir = temp_dir("load_error");
    write_text(dir / "project.toml", "name = \"E\"\nmain = \"Main\"\n");
    write_text(dir / "scripts/main.plm", "class Main extends Node2D {\n    fn update(dt: float) { var x: int = \"no\" }\n}\n");
    PlayController play;
    script::SessionOptions o;
    o.dir = dir;
    o.jobs = 1;
    std::string error;
    CHECK_FALSE(play.start(o, error));
    CHECK(error.find("scripts/main.plm:2:") != std::string::npos);
    CHECK_FALSE(play.active());
}

// ------------------------------------------------------------ input map

TEST_CASE("input map: edit, undo/redo and save keep project.toml's other lines and comments") {
    const fs::path dir = copy_example("shmup", "input_map");
    const std::string original = read_text(dir / "project.toml");
    InputMapDoc map;
    std::string error;
    REQUIRE(map.load(dir, error));
    REQUIRE(map.actions().size() == 11);
    CHECK(map.action("shoot")->bindings.front() == "Key.Z");
    // No edits: saving rewrites nothing.
    REQUIRE(map.save(error));
    CHECK(read_text(dir / "project.toml") == original);

    CHECK(map.add_action("dash"));
    CHECK(map.bind("dash", "Key.LeftShift"));
    CHECK(map.bind("dash", "Pad.RightTrigger"));
    CHECK(map.unbind("bomb", "Key.L"));
    CHECK(map.rename_action("focus", "slow"));
    CHECK(map.remove_action("ui_click"));
    CHECK(map.add_axis_source("move", "Pad.RightStick"));
    // Rejected edits change nothing.
    CHECK_FALSE(map.bind("dash", "Key.Nope"));
    CHECK(map.error() == "unknown input 'Key.Nope'");
    CHECK_FALSE(map.add_action("shoot"));
    CHECK_FALSE(map.add_action("9x"));
    CHECK_FALSE(map.bind("dash", "Key.LeftShift"));
    // Undo/redo.
    CHECK(map.undo());
    CHECK(map.axis("move")->bindings.size() == 4);
    CHECK(map.redo());
    CHECK(map.axis("move")->bindings.size() == 5);
    CHECK(map.dirty());
    REQUIRE(map.save(error));
    CHECK_FALSE(map.dirty());

    const std::string saved = read_text(dir / "project.toml");
    // Comments and unrelated keys stay; the edits are there.
    CHECK(saved.find("# A shmup stage written entirely in the language") != std::string::npos);
    CHECK(saved.find("render_tier = \"compatible\"   # DESIGN.md 4.4") != std::string::npos);
    CHECK(saved.find("# Menus (engine UI, DESIGN.md 4.10)") != std::string::npos);
    CHECK((saved.find("slow = [\"Key.LeftShift\", \"Key.X\", \"Key.K\", \"Pad.RightShoulder\"]\n") != std::string::npos ||
           saved.find("slow = [\"Key.LeftShift\", \"Key.X\", \"Key.K\", \"Pad.RightShoulder\"]\r\n") != std::string::npos));
    CHECK((saved.find("bomb = [\"Key.C\", \"Pad.East\"]\n") != std::string::npos ||
           saved.find("bomb = [\"Key.C\", \"Pad.East\"]\r\n") != std::string::npos));
    CHECK(saved.find("ui_click") == std::string::npos);
    CHECK((saved.find("ui_cancel = [\"Key.Escape\", \"Key.Backspace\", \"Key.X\", \"Pad.East\"]\ndash = "
                      "[\"Key.LeftShift\", \"Pad.RightTrigger\"]\n") != std::string::npos ||
           saved.find("ui_cancel = [\"Key.Escape\", \"Key.Backspace\", \"Key.X\", \"Pad.East\"]\r\ndash = "
                      "[\"Key.LeftShift\", \"Pad.RightTrigger\"]\r\n") != std::string::npos));
    CHECK(saved.find("move = [\"Keys.Arrows\", \"Keys.WASD\", \"Pad.LeftStick\", \"Pad.Dpad\", \"Pad.RightStick\"]") !=
          std::string::npos);
    // The game reads it.
    auto project = script::load_project(dir, error);
    REQUIRE_MESSAGE(project, error);
    InputMap im;
    REQUIRE_MESSAGE(script::apply_input(*project, im, error), error);
    CHECK(im.action_index(Name("dash")));
    CHECK(im.action_index(Name("slow")));
    CHECK_FALSE(im.action_index(Name("focus")));

    // Undo across a save, then save again: back to the original bindings.
    while (map.undo()) {
    }
    REQUIRE(map.save(error));
    CHECK(read_text(dir / "project.toml") == original);
    // Reloading reads the same map.
    InputMapDoc again;
    REQUIRE(again.load(dir, error));
    CHECK(again.actions().size() == 11);
}

TEST_CASE("input map: capture picks the first pressed device input") {
    DeviceState d;
    CHECK_FALSE(captured_binding(d));
    d.set_key(Key::Space, true);
    CHECK(captured_binding(d) == "Key.Space");
    d.clear_transients();
    CHECK_FALSE(captured_binding(d)); // held, not newly pressed
    d.set_button(PadButton::North, true);
    CHECK(captured_binding(d) == "Pad.North");
    d.clear_transients();
    d.set_mouse_button(MouseButton::Right, true);
    CHECK(captured_binding(d) == "Mouse.Right");
    d.clear_transients();
    d.set_axis(PadAxis::LeftY, -0.9f);
    CHECK(captured_binding(d) == "Pad.LeftStickUp");
}

// ------------------------------------------------------------ assets

TEST_CASE("asset library: tree, kinds, folder/rename/delete and external changes") {
    const fs::path dir = copy_example("shmup", "assets");
    AssetLibrary lib;
    lib.open(dir);
    REQUIRE(lib.find("scenes/title.scene"));
    CHECK(lib.find("scenes/title.scene")->kind == AssetKind::Scene);
    CHECK(lib.find("scripts/game.plm")->kind == AssetKind::Script);
    CHECK(lib.find("assets/sfx")->kind == AssetKind::Folder);
    CHECK(lib.find("project.toml")->kind == AssetKind::Project);
    CHECK(lib.files_of(AssetKind::Scene).size() == 4);
    CHECK(asset_kind_of("a/b.ogg") == AssetKind::Sound);
    CHECK(asset_kind_of("font.TTF") == AssetKind::Font);
    CHECK(asset_kind_of("robot.svg") == AssetKind::Vector);
    // Folders first, then files, by name.
    CHECK(lib.root().children.front().kind == AssetKind::Folder);

    std::string error;
    REQUIRE(lib.create_folder("scenes", "levels", error));
    CHECK(fs::is_directory(dir / "scenes/levels"));
    CHECK_FALSE(lib.create_folder("scenes", "levels", error));
    CHECK_FALSE(lib.create_folder("scenes", "../escape", error));
    CHECK_FALSE(lib.create_folder("scenes", ".hidden", error));
    REQUIRE(lib.rename("scenes/pause.scene", "paused.scene", error));
    CHECK(fs::exists(dir / "scenes/paused.scene"));
    CHECK(lib.find("scenes/paused.scene"));
    CHECK_FALSE(lib.find("scenes/pause.scene"));
    CHECK_FALSE(lib.rename("scenes/paused.scene", "title.scene", error)); // exists
    REQUIRE(lib.remove("scenes/levels", error));
    CHECK_FALSE(fs::exists(dir / "scenes/levels"));
    CHECK_FALSE(lib.remove("project.toml", error));
    CHECK_FALSE(lib.remove("", error));
    CHECK_FALSE(lib.remove("nope.txt", error));

    // Another program writes, changes and deletes files: poll() sees it.
    CHECK_FALSE(lib.poll());
    write_text(dir / "scripts/extra.plm", "class Extra extends Node2D {}\n");
    std::vector<std::string> changed;
    REQUIRE(lib.poll(&changed));
    CHECK(changed == std::vector<std::string>{"scripts/extra.plm"});
    CHECK(lib.find("scripts/extra.plm"));
    fs::remove(dir / "scripts/extra.plm");
    changed.clear();
    REQUIRE(lib.poll(&changed));
    CHECK(changed == std::vector<std::string>{"scripts/extra.plm"});

    // The same through command verbs.
    Editor ed(dir);
    const ScriptResult r = ed.run("asset mkdir scenes/bosses\nasset expect scenes/bosses\n"
                                  "asset rename scenes/bosses big_bosses\nasset expect-missing scenes/bosses\n"
                                  "asset delete scenes/big_bosses\nasset expect-missing scenes/big_bosses\n");
    CHECK_MESSAGE(r.ok, r.line << ": " << r.error);
    CHECK_FALSE(ed.run("asset delete project.toml\n").ok);
}

// ------------------------------------------------------------ instancing

TEST_CASE("scene instancing: dropping a .scene adds a SceneInstance that the view and the game expand") {
    const fs::path dir = copy_example("editor_level", "instancing");
    Editor ed(dir);
    std::string error;
    REQUIRE_MESSAGE(ed.doc.open(dir / "scenes/level.scene", error), error);
    // A small scene to instance: one turret-like Node2D.
    ed.doc.new_scene("Node2D", "part");
    REQUIRE(ed.doc.add_child(ed.doc.model().root(), "Node2D", "piece") != 0);
    REQUIRE(ed.doc.save_as(dir / "scenes/part.scene", error));
    REQUIRE(ed.doc.open(dir / "scenes/level.scene", error));
    const std::size_t before = ed.doc.model().size();
    const NodeId id = ed.doc.instance_scene(ed.doc.model().root(), "scenes/part.scene", vec2{100.0f, 50.0f},
                                            "scenes/level.scene");
    REQUIRE(id != 0);
    CHECK(ed.doc.model().size() == before + 1);
    CHECK(ed.doc.model().get(id)->type == "SceneInstance");
    CHECK(ed.doc.model().get(id)->name == "part");
    CHECK(scene::format_value(ed.doc.field_value(id, "scene")) == "\"scenes/part.scene\"");
    CHECK(scene::format_value(ed.doc.field_value(id, "position")) == "(100.0, 50.0)");
    CHECK(ed.doc.instance_scene(ed.doc.model().root(), "scenes/level.scene", std::nullopt, "scenes/level.scene") == 0);
    CHECK(ed.doc.instance_scene(ed.doc.model().root(), "scripts/level.plm") == 0);
    // Undo removes it in one step.
    REQUIRE(ed.doc.undo());
    CHECK(ed.doc.model().size() == before);
    REQUIRE(ed.doc.redo());
    // The preview expands the instance (its nodes join the live tree).
    ed.env.rebuild_preview(ed.doc.model().to_doc());
    REQUIRE(ed.env.preview_ok());
    CHECK(live_count(ed.env.host().root()) == before + 1 + 2);
    // Played, the game loads it too; saving the scene keeps one node.
    const ScriptResult r = ed.run("play scene\nstep 5\nexpect-state playing\nstop\nsave\n");
    CHECK_MESSAGE(r.ok, r.line << ": " << r.error);
    auto saved = scene::load_file(dir / "scenes/level.scene", error);
    REQUIRE(saved);
    CHECK(saved->nodes.size() == before + 1);
    CHECK(ed.run("instance / scenes/part.scene 10 20\nexpect . scene \"scenes/part.scene\"\n"
                 "expect . position (10.0, 20.0)\n")
              .ok);
}

// ------------------------------------------------------------ debugger

TEST_CASE("debugger: the live tree reads nodes and fields without running script code") {
    PlayController play;
    std::string error;
    REQUIRE(play.start(shmup_play(), error));
    DeviceState devices;
    play.step(devices, {}, 60);
    script::ScriptHost& host = play.session()->host();
    const pluma::Value& root = host.root();
    CHECK(live_class(root) == "Game");
    const std::size_t nodes = live_count(root);
    CHECK(nodes > 3);
    const std::vector<LiveField> fields = live_fields(host.session()->vm(), root);
    bool has_score = false;
    for (const LiveField& f : fields) {
        CHECK(f.name != "children");
        CHECK(f.name != "parent");
        if (f.name == "score") has_score = true;
    }
    CHECK(has_score);
    // Reading changed nothing.
    const std::uint64_t h = play.state_hash();
    for (int i = 0; i < 3; ++i) (void)live_fields(host.session()->vm(), root);
    CHECK(play.state_hash() == h);
    CHECK(live_children(root).size() > 0);
}

// ------------------------------------------------------------ command scripts

TEST_CASE("command scripts drive play, record, replay and the input map") {
    const fs::path dir = copy_example("shmup", "commands");
    Editor ed(dir);
    const ScriptResult r = ed.run(
        "# the runtime's hash\n"
        "play\n"
        "step 240\n"
        "expect-tick 240\n"
        "expect-hash a3e57e79964421d3\n"
        "stop\n"
        "# a recorded session with input\n"
        "play-record\n"
        "step 20\n"
        "hold ui_accept\n"
        "step 2\n"
        "release ui_accept\n"
        "hold shoot\n"
        "step 100\n"
        "release shoot\n"
        "step 50\n"
        "record stop\n"
        "record save replays/run.replay\n"
        "stop\n"
        "replay load replays/run.replay\n"
        "expect-state paused\n"
        "replay seek 100\n"
        "expect-tick 100\n"
        "replay verify\n"
        "expect-tick 172\n"
        "stop\n"
        "# the input map\n"
        "input add-action dash\n"
        "input bind dash Key.LeftShift\n"
        "input expect dash Key.LeftShift\n"
        "input undo\n"
        "input expect dash\n"
        "input redo\n"
        "input save\n"
        "asset expect replays/run.replay\n");
    REQUIRE_MESSAGE(r.ok, r.line << ": " << r.error);
    CHECK(read_text(dir / "project.toml").find("dash = [\"Key.LeftShift\"]") != std::string::npos);
    // Verification across thread counts, as `cuervo-editor --replay-verify` does.
    std::string error;
    auto replay = load_replay(dir / "replays/run.replay", error);
    REQUIRE_MESSAGE(replay, error);
    // The input map changed (dash added): older replays are refused, by design.
    script::SessionOptions base;
    base.font_path = font;
    const script::ReplayCheck changed = script::verify_replay(dir, *replay, base);
    CHECK_FALSE(changed.ok);
    CHECK(changed.error.find("input actions") != std::string::npos);
    // In the unchanged project it verifies.
    const script::ReplayCheck c = script::verify_replay(examples / "shmup", *replay, base);
    CHECK_MESSAGE(c.ok, c.error);
    jobs::configure(1);

    // Failures name the line.
    ScriptResult bad = ed.run("play\nstep 10\nexpect-hash 0000000000000001\n");
    CHECK_FALSE(bad.ok);
    CHECK(bad.line == 3);
    CHECK(bad.error.find("expected state hash 0000000000000001") != std::string::npos);
    ed.play.stop();
    bad = ed.run("step 1\n");
    CHECK_FALSE(bad.ok);
    // Without a play controller the verbs are unavailable.
    CHECK(run_command_script("play\n", ed.doc, ed.env).error == "play is not available here");
}

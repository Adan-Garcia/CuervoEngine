// The runtime's simulation half (cuervo/script/game_session.hpp), replays
// (cuervo/input/replay.hpp), writing the input map back into project.toml
// (cuervo/script/project_edit.hpp), binding names and SceneInstance.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "cuervo/input/replay.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/script/game_session.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/project_edit.hpp"
#include "cuervo/script/scene_bridge.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

const fs::path shmup_dir = fs::path(CUERVO_EXAMPLES_DIR) / "shmup";
const std::string font = std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf";

// The shmup's state hash at tick 240 with no input (DESIGN.md 4.15 "Replay
// hash change"): `cuervo --game examples/shmup --screenshot` logs the same.
constexpr std::uint64_t shmup_hash_240 = 0xa3e57e79964421d3ull;

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_session_tests" / std::string(name);
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

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
script::SessionOptions shmup_options(unsigned jobs = 1) {
    script::SessionOptions o;
    o.dir = shmup_dir;
    o.font_path = font;
    o.jobs = jobs;
    return o;
}
#endif

} // namespace

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
TEST_CASE("game session: the shmup's hash at tick 240 matches the runtime") {
    script::GameSession s;
    std::string error;
    REQUIRE_MESSAGE(s.init(shmup_options(), error), error);
    DeviceState devices;
    for (int i = 0; i < 240; ++i) s.tick(s.sample(devices, {0.0f, 0.0f}));
    CHECK(s.host().tick_count() == 240);
    CHECK(s.host().state_hash() == shmup_hash_240);
}
#endif

TEST_CASE("replay: write/read round trip and corrupt files") {
    Replay r;
    r.project = "P";
    r.seed = 42;
    r.tick_rate = 120.0f;
    r.actions = {"shoot", "bomb"};
    r.axes = {"move.x", "move.y"};
    r.start_scene = "scenes/a.scene";
    r.scene = {std::byte{1}, std::byte{2}, std::byte{3}};
    InputFrame f;
    f.held = 0x8000000000000001ull;
    f.axes[0] = -32767;
    f.axes[7] = 5;
    f.pointer = {-3, 700};
    r.frames = {InputFrame{}, f};
    r.hashes = {7, 0xfedcba9876543210ull};
    const std::vector<std::byte> bytes = write_replay(r);
    std::string error;
    auto back = read_replay(bytes, error);
    REQUIRE_MESSAGE(back, error);
    CHECK(back->project == "P");
    CHECK(back->seed == 42);
    CHECK(back->tick_rate == 120.0f);
    CHECK(back->actions == r.actions);
    CHECK(back->axes == r.axes);
    CHECK(back->start_scene == r.start_scene);
    CHECK(back->scene == r.scene);
    CHECK(back->frames == r.frames);
    CHECK(back->hashes == r.hashes);
    CHECK(write_replay(*back) == bytes);

    std::vector<std::byte> cut(bytes.begin(), bytes.end() - 3);
    CHECK_FALSE(read_replay(cut, error));
    std::vector<std::byte> bad = bytes;
    bad[0] = std::byte{'X'};
    CHECK_FALSE(read_replay(bad, error));
    CHECK(error == "not a replay file (bad magic)");
}

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
TEST_CASE("game session: a recorded run replays bit for bit on 1 and 8 threads") {
    std::string error;
    Replay recorded;
    {
        script::GameSession s;
        REQUIRE_MESSAGE(s.init(shmup_options(1), error), error);
        // Start the game from the title menu, then fly and shoot.
        s.set_presses("30:ui_accept,90:shoot,91:shoot,92:shoot,120:bomb");
        s.start_recording();
        DeviceState devices;
        for (int i = 0; i < 400; ++i) {
            devices.set_key(Key::Left, i >= 150 && i < 200);
            devices.set_key(Key::Z, i >= 210 && i < 300);
            s.tick(s.sample(devices, {static_cast<float>(i), 10.0f}));
        }
        recorded = s.recorded();
    }
    REQUIRE(recorded.ticks() == 400);
    REQUIRE(recorded.hashes.size() == 400);
    CHECK(recorded.seed == 20261015u);
    CHECK(recorded.actions.size() == 11);
    // The input really changed the run: it diverges from an idle one.
    {
        script::GameSession idle;
        REQUIRE(idle.init(shmup_options(1), error));
        DeviceState devices;
        for (int i = 0; i < 400; ++i) idle.tick(idle.sample(devices, {0.0f, 0.0f}));
        CHECK(idle.host().state_hash() != recorded.hashes.back());
    }

    const fs::path dir = temp_dir("replay");
    REQUIRE(save_replay(dir / "run.replay", recorded, error));
    auto loaded = load_replay(dir / "run.replay", error);
    REQUIRE_MESSAGE(loaded, error);
    for (unsigned jobs : {1u, 8u}) {
        INFO("jobs " << jobs);
        script::ReplayCheck c = script::verify_replay(shmup_dir, *loaded, shmup_options(jobs));
        CHECK_MESSAGE(c.error.empty(), c.error);
        CHECK(c.ok);
        CHECK(c.ticks == 400);
        CHECK(c.compared == 400);
        CHECK(c.final_hash == recorded.hashes.back());
    }
    jobs::configure(1);

    // A tampered hash is the first mismatch.
    Replay tampered = *loaded;
    tampered.hashes[199] ^= 1;
    script::ReplayCheck c = script::verify_replay(shmup_dir, tampered, shmup_options(1));
    CHECK_FALSE(c.ok);
    REQUIRE(c.first_mismatch);
    CHECK(*c.first_mismatch == 200);
    CHECK(c.expected == tampered.hashes[199]);
    CHECK(c.actual == recorded.hashes[199]);
    // So is different input.
    Replay changed = *loaded;
    changed.frames[100].held ^= 1; // `shoot` at tick 101
    c = script::verify_replay(shmup_dir, changed, shmup_options(1));
    CHECK_FALSE(c.ok);
    // A project whose actions moved is refused.
    Replay moved = *loaded;
    std::swap(moved.actions[0], moved.actions[1]);
    c = script::verify_replay(shmup_dir, moved, shmup_options(1));
    CHECK_FALSE(c.ok);
    CHECK(c.error.find("input actions") != std::string::npos);
}
#endif

TEST_CASE("binding names round-trip") {
    for (std::size_t k = 1; k < key_count; ++k) {
        const Binding b = Binding::key(static_cast<Key>(k));
        const std::string name = script::binding_name(b);
        REQUIRE(!name.empty());
        auto back = script::parse_binding(name);
        REQUIRE(back);
        CHECK(back->source == b.source);
        CHECK(back->code == b.code);
    }
    for (const char* n : {"Pad.South", "Pad.DpadLeft", "Pad.LeftStickUp", "Pad.RightStickRight", "Pad.LeftTrigger",
                          "Mouse.Left", "Mouse.Middle", "Mouse.X2", "Wheel.Up", "Wheel.Down"}) {
        auto b = script::parse_binding(n);
        REQUIRE_MESSAGE(b, n);
        CHECK(script::binding_name(*b) == n);
    }
    CHECK_FALSE(script::parse_binding("Key.Nope"));
    CHECK_FALSE(script::parse_binding("Key.Unknown"));
    AxisSource x;
    AxisSource y;
    for (std::string_view n : script::axis2_source_names()) CHECK(script::parse_axis2(n, x, y));
}

TEST_CASE("project.toml: rewriting the input map keeps everything else") {
    const std::string toml =
        "# A game\n"
        "name = \"G\"   # its name\n"
        "\n"
        "[input.actions]\n"
        "# Fire!\n"
        "shoot = [\"Key.Z\", \"Pad.South\"]   # main button\n"
        "bomb = [\"Key.C\"]\n"
        "pause = [\"Key.P\"]\n"
        "\n"
        "[input.axes]\n"
        "move = [\"Keys.Arrows\"]\n"
        "\n"
        "[later.tool]\n"
        "x = 1\n";
    script::InputSections in = script::read_input_sections(toml);
    REQUIRE(in.actions.size() == 3);
    CHECK(in.actions[0].name == "shoot");
    CHECK(in.actions[0].bindings == std::vector<std::string>{"Key.Z", "Pad.South"});
    REQUIRE(in.axes.size() == 1);
    // Unchanged: byte-identical.
    CHECK(script::rewrite_input_sections(toml, in) == toml);

    // Rebind (comment kept), rename, remove, add an action and an axis.
    in.actions[0].bindings.push_back("Mouse.Left");
    in.actions[1].name = "smart_bomb";
    in.actions.erase(in.actions.begin() + 2);
    in.actions.push_back({"dash", {"Key.LeftShift"}, ""});
    in.axes.push_back({"aim", {"Pad.RightStick"}, ""});
    CHECK(script::rewrite_input_sections(toml, in) ==
          "# A game\n"
          "name = \"G\"   # its name\n"
          "\n"
          "[input.actions]\n"
          "# Fire!\n"
          "shoot = [\"Key.Z\", \"Pad.South\", \"Mouse.Left\"]   # main button\n"
          "smart_bomb = [\"Key.C\"]\n"
          "dash = [\"Key.LeftShift\"]\n"
          "\n"
          "[input.axes]\n"
          "move = [\"Keys.Arrows\"]\n"
          "aim = [\"Pad.RightStick\"]\n"
          "\n"
          "[later.tool]\n"
          "x = 1\n");

    // No input sections yet: they are appended.
    script::InputSections fresh;
    fresh.actions.push_back({"jump", {"Key.Space"}, ""});
    CHECK(script::rewrite_input_sections("name = \"G\"", fresh) == "name = \"G\"\n\n[input.actions]\njump = [\"Key.Space\"]\n");

    // The shipped shmup's file survives a no-op rewrite exactly.
    const std::string shmup = read_text(shmup_dir / "project.toml");
    CHECK(script::rewrite_input_sections(shmup, script::read_input_sections(shmup)) == shmup);
    CHECK(script::is_input_name("ui_accept"));
    CHECK_FALSE(script::is_input_name("9lives"));
    CHECK_FALSE(script::is_input_name("a-b"));
}

TEST_CASE("SceneInstance: a scene instanced as a child, by reference") {
    const fs::path dir = temp_dir("instance");
    write_text(dir / "project.toml", "name = \"I\"\nmain_scene = \"scenes/main.scene\"\n[scene]\ntransform_hierarchy = false\n");
    write_text(dir / "scripts/marker.plm", "class Marker extends Node2D {\n    @export var tag = 0\n}\n");
    std::string error;
    fs::create_directories(dir / "scenes");
    // The instanced scene: a Marker root at (10, 20) with a child.
    scene::SceneDoc part;
    part.nodes.push_back({"Marker", "part", -1, {{"position", "vec2", scene::Value::vector({10.0f, 20.0f})},
                                                 {"tag", "int", scene::Value::integer(7)}}});
    part.nodes.push_back({"Node2D", "child", 0, {}});
    REQUIRE(scene::save_file(dir / "scenes/part.scene", part, error));
    scene::SceneDoc main;
    main.nodes.push_back({"Node2D", "root", -1, {}});
    main.nodes.push_back({"SceneInstance", "inst", 0, {{"scene", "str", scene::Value::str("scenes/part.scene")},
                                                       {"position", "vec2", scene::Value::vector({100.0f, 0.0f})}}});
    REQUIRE(scene::save_file(dir / "scenes/main.scene", main, error));

    script::SessionOptions o;
    o.dir = dir;
    script::GameSession s;
    REQUIRE_MESSAGE(s.init(o, error), error);
    REQUIRE_MESSAGE(s.host().running(), s.host().error());
    pluma::VM& vm = s.host().session()->vm();
    // root -> inst -> part (transient, moved by the instance) -> child
    pluma::Value inst;
    REQUIRE(vm.call_method(s.host().root(), "find", std::vector<pluma::Value>{pluma::str_value("part")}, &inst).ok);
    REQUIRE(inst.is_obj());

    CHECK(vm.describe(inst, 400).find("position = vec2(110.0, 20.0)") != std::string::npos);
    CHECK(vm.describe(inst, 400).find("transient = true") != std::string::npos);
    pluma::release(inst);
    // Saving the tree keeps the reference, not a copy.
    std::vector<std::string> warnings;
    scene::SceneDoc saved = script::save_tree(vm, s.host().root(), warnings);
    CHECK(saved.nodes.size() == 2);

    // A scene that instances itself panics instead of recursing forever.
    scene::SceneDoc loop;
    loop.nodes.push_back({"SceneInstance", "self", -1, {{"scene", "str", scene::Value::str("scenes/loop.scene")}}});
    REQUIRE(scene::save_file(dir / "scenes/loop.scene", loop, error));
    write_text(dir / "project.toml", "name = \"I\"\nmain_scene = \"scenes/loop.scene\"\n");
    script::GameSession bad;
    REQUIRE(bad.init(o, error));
    CHECK(bad.host().panicked());
    CHECK(bad.host().error().find("instances itself") != std::string::npos);
}

TEST_CASE("game session: playing a scene from memory, and the panic's frames") {
    const fs::path dir = temp_dir("doc");
    write_text(dir / "project.toml", "name = \"D\"\n");
    write_text(dir / "scripts/bomb.plm",
               "class Bomb extends Node2D {\n"
               "    @export var fuse = 3\n"
               "    fn update(dt: float) {\n"
               "        fuse -= 1\n"
               "        var xs = [1, 2]\n"
               "        if fuse < 0 {\n"
               "            print(xs[fuse + 10])\n"
               "        }\n"
               "    }\n"
               "}\n");
    auto doc = std::make_shared<scene::SceneDoc>();
    doc->nodes.push_back({"Bomb", "b", -1, {{"fuse", "int", scene::Value::integer(1)}}});
    script::SessionOptions o;
    o.dir = dir;
    o.scene = doc;
    o.scene_label = "(edited)";
    script::GameSession s;
    std::string error;
    REQUIRE_MESSAGE(s.init(o, error), error);
    CHECK(s.replay_header().start_scene == "(edited)");
    CHECK(!s.replay_header().scene.empty());
    DeviceState devices;
    s.tick(s.sample(devices, {}));
    CHECK(s.host().running());
    s.tick(s.sample(devices, {}));
    REQUIRE(s.host().panicked());
    const auto& panic = s.host().last_panic();
    REQUIRE(panic);
    REQUIRE(!panic->frames.empty());
    CHECK(panic->frames[0].function == "Bomb.update");
    CHECK(panic->frames[0].line == 7);
    std::string locals;
    for (const pluma::PanicLocal& l : panic->frames[0].locals) locals += l.name + "=" + l.value + ";";
    CHECK(locals.find("dt=") != std::string::npos);
    CHECK(locals.find("xs=[1, 2];") != std::string::npos);
    // A fixed script resumes the game on reload.
    write_text(dir / "scripts/bomb.plm",
               "class Bomb extends Node2D {\n    @export var fuse = 3\n    fn update(dt: float) { fuse -= 1 }\n}\n");
    CHECK(s.reload(false, true, false) == script::GameSession::Reload::Reloaded);
    CHECK(s.host().running());
    CHECK_FALSE(s.host().last_panic());
}

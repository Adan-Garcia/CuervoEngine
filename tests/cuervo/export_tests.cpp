// The release pipeline (tools/cuervo_tools/export.hpp, docs/EXPORT.md):
// packing a real project is deterministic, strips what it should, imports
// sources to engine formats, and the packed game runs to the same state hash
// as the project directory, with saves going to the user directory.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cuervo/assets/manifest.hpp"
#include "cuervo/assets/pack.hpp"
#include "cuervo/input/devices.hpp"
#include "cuervo/script/game_session.hpp"
#include "tools/cuervo_tools/export.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

const fs::path shmup_dir = fs::path(CUERVO_EXAMPLES_DIR) / "shmup";
const std::string font = std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf";
constexpr std::uint64_t shmup_hash_240 = 0xa3e57e79964421d3ull;

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_export_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_text(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

tools::PackOptions options(assets::PackPlatform platform = assets::PackPlatform::Linux) {
    tools::PackOptions o;
    o.platform = platform;
    o.font_path = font;
    o.engine_version = "test";
    return o;
}

std::shared_ptr<const assets::Pack> pack_of(const fs::path& project, const tools::PackOptions& o,
                                            tools::PackSummary* summary = nullptr) {
    tools::PackSummary s;
    std::string error;
    auto bytes = tools::pack_project(project, o, summary != nullptr ? *summary : s, error);
    REQUIRE_MESSAGE(bytes, error);
    auto pack = assets::Pack::from_bytes(std::move(*bytes), error);
    REQUIRE_MESSAGE(pack, error);
    return pack;
}

} // namespace

TEST_CASE("pack_project: the shmup packs byte-identically twice, with its font and manifest") {
    std::string error;
    tools::PackSummary s1;
    tools::PackSummary s2;
    auto a = tools::pack_project(shmup_dir, options(), s1, error);
    REQUIRE_MESSAGE(a, error);
    auto b = tools::pack_project(shmup_dir, options(), s2, error);
    REQUIRE_MESSAGE(b, error);
    CHECK(*a == *b);
    CHECK(s1.content_hash == s2.content_hash);
    CHECK(s1.game == "Shmup");

    auto pack = assets::Pack::from_bytes(*a, error);
    REQUIRE(pack);
    CHECK(pack->platform() == assets::PackPlatform::Linux);
    CHECK(pack->contains("project.toml"));
    CHECK(pack->contains("scripts/game.plm"));
    CHECK(pack->contains("scenes/title.scene"));
    CHECK(pack->contains("assets/sfx/hit.wav"));
    CHECK(pack->contains(assets::default_font_entry));
    auto manifest = assets::parse_manifest(
        std::string_view(reinterpret_cast<const char*>(pack->find(assets::manifest_path)->data()),
                         pack->find(assets::manifest_path)->size()),
        error);
    REQUIRE_MESSAGE(manifest, error);
    CHECK(manifest->game == "Shmup");
    CHECK(manifest->scripts == assets::ScriptForm::Source);
    CHECK(manifest->platform == assets::PackPlatform::Linux);

    // A web pack is a different archive (platform tag) of the same files.
    auto web = pack_of(shmup_dir, options(assets::PackPlatform::Web));
    CHECK(web->platform() == assets::PackPlatform::Web);
    CHECK(web->entries().size() == pack->entries().size());
}

TEST_CASE("pack_project: hidden files never ship, --strip drops dev files, sources are imported") {
    const fs::path dir = temp_dir("strip");
    fs::copy(shmup_dir, dir / "game", fs::copy_options::recursive);
    const fs::path p = dir / "game";
    write_text(p / ".import/assets/x.efont", "cache");
    write_text(p / ".gitignore", ".import/\n");
    write_text(p / "replays/last.replay", "replay");
    write_text(p / "level.cmds", "commands");
    write_text(p / "notes.replay", "replay");
    fs::copy_file(fs::path(CUERVO_ASSETS_DIR) / "demo/robot.svg", p / "assets/robot.svg");
    fs::create_directories(p / "assets/fonts");
    fs::copy_file(font, p / "assets/fonts/ui.ttf");
    tools::PackSummary full_summary;
    auto full = pack_of(p, options(), &full_summary);
    CHECK_FALSE(full->contains(".import/assets/x.efont"));
    CHECK_FALSE(full->contains(".gitignore"));
    CHECK(full->contains("replays/last.replay")); // not stripped
    CHECK(full->contains("level.cmds"));
    // Sources become engine formats: the source itself is not shipped.
    CHECK(full->contains("assets/robot.evec"));
    CHECK_FALSE(full->contains("assets/robot.svg"));
    CHECK(full->contains("assets/fonts/ui.efont"));
    CHECK_FALSE(full->contains("assets/fonts/ui.ttf"));
    CHECK(full_summary.imported == 2);

    tools::PackOptions strip = options();
    strip.strip = true;
    auto stripped = pack_of(p, strip);
    CHECK_FALSE(stripped->contains("replays/last.replay"));
    CHECK_FALSE(stripped->contains("notes.replay"));
    CHECK_FALSE(stripped->contains("level.cmds"));
    CHECK(stripped->contains("scripts/game.plm")); // sources stay until bytecode/AOT exists
    CHECK(stripped->entries().size() == full->entries().size() - 3);
}

TEST_CASE("pack_project: the tier check gates the export") {
    const fs::path dir = temp_dir("tier");
    fs::copy(shmup_dir, dir / "game", fs::copy_options::recursive);
    // A compute shader in a compatible project is a tier error (GLES 3.0 has none).
    write_text(dir / "game/shaders/sim.comp", "#version 450\nvoid main() {}\n");
    tools::PackSummary s;
    std::string error;
    CHECK_FALSE(tools::pack_project(dir / "game", options(), s, error));
    CHECK(error.find("tier check") != std::string::npos);
    // A broken project.toml fails before anything is packed.
    write_text(dir / "game/project.toml", "name = \n");
    CHECK_FALSE(tools::pack_project(dir / "game", options(), s, error));
}

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
TEST_CASE("release session: the packed shmup reaches the dev state hash at tick 240") {
    const fs::path dir = temp_dir("session");
    std::string error;
    tools::PackSummary s;
    auto bytes = tools::pack_project(shmup_dir, options(), s, error);
    REQUIRE_MESSAGE(bytes, error);
    std::ofstream(dir / "game.pak", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));

    script::SessionOptions o;
    o.pack = dir / "game.pak";
    o.user_dir = dir / "user";
    o.jobs = 1;
    script::GameSession session;
    REQUIRE_MESSAGE(session.init(o, error), error);
    CHECK(session.packed());
    CHECK(session.script_files().empty()); // nothing on disk to watch
    CHECK(session.script_count() == 4);
    CHECK(session.font_atlas() != nullptr); // the pack's .cuervo/default.efont
#if CUERVO_FEATURE_AUDIO
    CHECK(session.audio().sound_count() == 7);
#endif
    DeviceState devices;
    for (int i = 0; i < 240; ++i) session.tick(session.sample(devices, {0.0f, 0.0f}));
    CHECK(session.host().state_hash() == shmup_hash_240);
    // Nothing to hot-reload in a pack.
    CHECK(session.reload(true, false, false) == script::GameSession::Reload::None);
}

TEST_CASE("release session (ScriptForm::Native): AOT-packed shmup omits .plm sources and reaches the dev state hash") {
    const fs::path dir = temp_dir("session_aot");
    std::string error;
    tools::PackOptions popts = options(assets::host_platform());
    popts.scripts = assets::ScriptForm::Native;
    popts.strip = true;

    tools::PackSummary s1;
    tools::PackSummary s2;
    auto bytes = tools::pack_project(shmup_dir, popts, s1, error);
    REQUIRE_MESSAGE(bytes, error);
    auto bytes2 = tools::pack_project(shmup_dir, popts, s2, error);
    REQUIRE_MESSAGE(bytes2, error);
    CHECK(*bytes == *bytes2);
    CHECK(s1.content_hash == s2.content_hash);

    auto pack = assets::Pack::from_bytes(*bytes, error);
    REQUIRE_MESSAGE(pack, error);
    CHECK_FALSE(pack->contains("scripts/game.plm"));
    CHECK_FALSE(pack->contains("scripts/player.plm"));
    CHECK_FALSE(pack->contains("scripts/enemies.plm"));
    CHECK_FALSE(pack->contains("scripts/menus.plm"));
    CHECK(pack->contains(assets::native_scripts_entry));

    auto manifest = assets::parse_manifest(
        std::string_view(reinterpret_cast<const char*>(pack->find(assets::manifest_path)->data()),
                         pack->find(assets::manifest_path)->size()),
        error);
    REQUIRE_MESSAGE(manifest, error);
    CHECK(manifest->scripts == assets::ScriptForm::Native);

    std::ofstream(dir / "game.pak", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));

    script::SessionOptions o;
    o.pack = dir / "game.pak";
    o.user_dir = dir / "user";
    o.jobs = 1;
    script::GameSession session;
    REQUIRE_MESSAGE(session.init(o, error), error);
    CHECK(session.packed());
    CHECK(session.native_scripts());
    CHECK(session.script_files().empty());
    CHECK(session.script_count() == 0); // no .plm sources shipped in the pack
    CHECK(session.font_atlas() != nullptr);
#if CUERVO_FEATURE_AUDIO
    CHECK(session.audio().sound_count() == 7);
#endif

    DeviceState devices;
    for (int i = 0; i < 240; ++i) session.tick(session.sample(devices, {0.0f, 0.0f}));
    CHECK(session.host().state_hash() == shmup_hash_240);
    CHECK(session.reload(true, false, false) == script::GameSession::Reload::None);
}
#endif

TEST_CASE("release session: saves go to the user directory and shadow packed scenes") {
    const fs::path dir = temp_dir("saves");
    write_text(dir / "game/project.toml", "name = \"Saver\"\nmain = \"Main\"\n");
    write_text(dir / "game/scripts/main.plm", R"(
class T {
    static var saved = false
    static var hp = 0
}
class Item extends Node {
    @export var hp = 3
}
class Main extends Node {
    fn ready() {
        var it = Item()
        it.hp = 42
        T.saved = Scene.save(it, "saves/slot.scene")
        T.hp = (Scene.load("saves/slot.scene") as Item).hp
    }
}
)");
    tools::PackOptions o = options();
    o.font_path.clear(); // no font: nothing measures text here
    o.tier_gate = false;
    tools::PackSummary s;
    std::string error;
    auto bytes = tools::pack_project(dir / "game", o, s, error);
    REQUIRE_MESSAGE(bytes, error);
    std::ofstream(dir / "game.pak", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    fs::remove_all(dir / "game"); // the release must not need the project

    script::SessionOptions so;
    so.pack = dir / "game.pak";
    so.user_dir = dir / "user";
    so.jobs = 1;
    script::GameSession session;
    REQUIRE_MESSAGE(session.init(so, error), error);
    CHECK_FALSE(session.host().panicked());
    CHECK(session.host().session()->global("T.saved").b);
    CHECK(session.host().session()->global("T.hp").i == 42);
    CHECK(fs::exists(dir / "user/saves/slot.scene"));
}

TEST_CASE("release session: a pack that isn't an exported game is refused") {
    const fs::path dir = temp_dir("not_a_game");
    assets::PackBuilder b;
    std::string error;
    REQUIRE(b.add("project.toml", {}, error));
    REQUIRE(b.write(dir / "x.pak", error));
    script::SessionOptions so;
    so.pack = dir / "x.pak";
    script::GameSession session;
    CHECK_FALSE(session.init(so, error));
    CHECK(error.find("not an exported game") != std::string::npos);
    so.pack = dir / "missing.pak";
    CHECK_FALSE(session.init(so, error));
}

TEST_CASE("export: launcher names") {
    CHECK(tools::launcher_name("Shmup") == "shmup");
    CHECK(tools::launcher_name("Shmup Deluxe!") == "shmup-deluxe");
    CHECK(tools::launcher_name("!!!") == "game");
}

TEST_CASE("export: third-party notices are copied into licenses/") {
    const fs::path root = temp_dir("notices_root");
    write_text(root / "THIRD_PARTY_NOTICES.md", "# notices\n");
    write_text(root / "third_party" / "licenses" / "b.txt", "B\n");
    write_text(root / "third_party" / "licenses" / "a.txt", "A\n");
    const fs::path out = temp_dir("notices_out");
    std::vector<fs::path> written;
    std::string error;
    REQUIRE_MESSAGE(tools::copy_third_party_notices(root, out, written, error), error);
    REQUIRE(written.size() == 3);
    CHECK(written[1].filename() == "a.txt"); // sorted
    CHECK(fs::is_regular_file(out / "licenses" / "THIRD_PARTY_NOTICES.md"));
    CHECK(fs::is_regular_file(out / "licenses" / "b.txt"));
    // A source tree without notices is an error, not a silent omission.
    CHECK_FALSE(tools::copy_third_party_notices(temp_dir("no_notices"), out, written, error));
    CHECK(error.find("third-party notices not found") != std::string::npos);

    // The repository's own notices exist and are what export ships.
    const fs::path repo = fs::path(CUERVO_EXAMPLES_DIR).parent_path();
    CHECK(fs::is_regular_file(repo / "THIRD_PARTY_NOTICES.md"));
}

// Replays recorded from netplay (replay format 4, a player dimension) in the editor's play controller:
// every player's frame is fed per tick and backward seeks (snapshots) keep the extra players' state.

#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cuervo/input/replay.hpp"
#include "cuervo/script/game_session.hpp"
#include "editor/play_controller.hpp"

using namespace cuervo;
using namespace cuervo::editor;
namespace fs = std::filesystem;

namespace {

script::SessionOptions netduel_options() {
    script::SessionOptions o;
    o.dir = fs::path(CUERVO_EXAMPLES_DIR) / "netduel";
    o.font_path = CUERVO_DEFAULT_FONT;
    o.audio_device = false;
    o.jobs = 1;
    return o;
}

InputFrame held_of(std::uint64_t held) {
    InputFrame f;
    f.held = held;
    return f;
}

Replay record_two_players(int ticks) {
    script::GameSession s;
    std::string error;
    REQUIRE_MESSAGE(s.init(netduel_options(), error), error);
    s.start_recording();
    const InputMap& map = s.input_map();
    const std::uint64_t right = std::uint64_t{1} << *map.action_index(Name("right"));
    const std::uint64_t left = std::uint64_t{1} << *map.action_index(Name("left"));
    const std::uint64_t shoot = std::uint64_t{1} << *map.action_index(Name("shoot"));
    for (int t = 0; t < ticks; ++t) {
        const InputFrame players[2] = {held_of(t > 10 ? right | shoot : 0), held_of(t > 30 ? left | shoot : 0)};
        s.tick_players(players);
    }
    return s.recorded();
}

} // namespace

TEST_CASE("play controller: a two-player replay verifies and seeks back to identical state") {
    const Replay recorded = record_two_players(240);
    REQUIRE(recorded.extra_players.size() == 1);

    // Through a file, as a netplay recording is opened.
    const fs::path file = fs::temp_directory_path() / "cuervo_editor_net_replay_a1.replay";
    std::string error;
    REQUIRE(save_replay(file, recorded, error));
    const auto loaded = load_replay(file, error);
    fs::remove(file);
    REQUIRE_MESSAGE(loaded, error);

    PlayController play;
    play.snapshot_interval = 50;
    REQUIRE_MESSAGE(play.start_replay(netduel_options(), *loaded, error), error);
    REQUIRE(play.seek(240, error));
    CHECK_FALSE(play.first_mismatch());
    CHECK(play.verified() == 240);
    CHECK(play.state_hash() == recorded.hashes[239]);

    // Backward seeks resume from snapshots (which hold the extra players) and land on the recorded hashes.
    for (std::uint64_t t : {170u, 100u, 33u, 0u, 199u, 60u, 240u}) {
        REQUIRE(play.seek(t, error));
        CHECK(play.tick() == t);
        if (t > 0) CHECK(play.state_hash() == recorded.hashes[t - 1]);
    }
    CHECK_FALSE(play.first_mismatch());
    REQUIRE(play.seek(200, error));
    REQUIRE(play.seek(130, error));
    CHECK(play.last_seek().from_snapshot);
    CHECK(play.last_seek().from == 100);

    // The same, restarting from tick 0 instead of a snapshot.
    play.snapshot_interval = 0;
    REQUIRE(play.start_replay(netduel_options(), *loaded, error));
    REQUIRE(play.seek(220, error));
    REQUIRE(play.seek(140, error));
    CHECK_FALSE(play.last_seek().from_snapshot);
    CHECK(play.state_hash() == recorded.hashes[139]);
    CHECK_FALSE(play.first_mismatch());
}

TEST_CASE("play controller: dropping the extra players diverges (they are really fed)") {
    Replay solo = record_two_players(120);
    solo.extra_players.clear();
    PlayController play;
    std::string error;
    REQUIRE_MESSAGE(play.start_replay(netduel_options(), solo, error), error);
    REQUIRE(play.seek(120, error));
    CHECK(play.first_mismatch().has_value());
}

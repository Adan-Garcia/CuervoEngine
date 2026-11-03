// Replays of networked sessions (docs/REPLAYS.md version 4): the player dimension, compatibility with
// version 3 files, corrupt input, and an offline replay of a two-player match to the same hashes.

#include <doctest.h>

#include <filesystem>

#include "cuervo/input/replay.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/script/game_session.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

InputFrame frame_of(std::uint64_t held, std::int16_t axis = 0) {
    InputFrame f;
    f.held = held;
    f.axes[0] = axis;
    return f;
}

Replay sample_replay() {
    Replay r;
    r.project = "x";
    r.seed = 9;
    r.actions = {"a", "b"};
    r.action_ids = {1, 2};
    r.axes = {"move.x"};
    for (int t = 0; t < 5; ++t) {
        r.frames.push_back(frame_of(static_cast<std::uint64_t>(t & 3), static_cast<std::int16_t>(t)));
        r.hashes.push_back(100u + static_cast<std::uint64_t>(t));
    }
    r.extra_players.push_back({});
    r.extra_players.push_back({});
    for (int t = 0; t < 5; ++t) {
        r.extra_players[0].push_back(frame_of(static_cast<std::uint64_t>((t + 1) & 3)));
        r.extra_players[1].push_back(frame_of(static_cast<std::uint64_t>(t), static_cast<std::int16_t>(-t)));
    }
    return r;
}

} // namespace

TEST_CASE("replay v4: the extra players round-trip") {
    const Replay r = sample_replay();
    const auto bytes = write_replay(r);
    std::string error;
    const auto back = read_replay(bytes, error);
    REQUIRE_MESSAGE(back, error);
    CHECK(back->extra_players.size() == 2);
    for (std::size_t p = 0; p < 2; ++p) {
        REQUIRE(back->extra_players[p].size() == 5);
        for (std::size_t t = 0; t < 5; ++t) {
            CHECK(back->extra_players[p][t].held == r.extra_players[p][t].held);
            CHECK(back->extra_players[p][t].axes == r.extra_players[p][t].axes);
        }
    }
    CHECK(back->hashes == r.hashes);
}

TEST_CASE("replay v4: older files still load, without extra players") {
    const Replay r = sample_replay();
    for (std::uint32_t version : {1u, 2u, 3u}) {
        INFO("version " << version);
        std::string error;
        const auto back = read_replay(write_replay(r, version), error);
        REQUIRE_MESSAGE(back, error);
        CHECK(back->extra_players.empty());
        CHECK(back->frames.size() == 5);
        CHECK(back->hashes == r.hashes);
    }
    // A single-player replay is a version 4 file with no extra players.
    Replay solo = r;
    solo.extra_players.clear();
    std::string error;
    const auto back = read_replay(write_replay(solo), error);
    REQUIRE_MESSAGE(back, error);
    CHECK(back->extra_players.empty());
}

TEST_CASE("replay v4: truncated or oversized player data is refused") {
    const auto bytes = write_replay(sample_replay());
    std::string error;
    for (std::size_t cut : {std::size_t{1}, std::size_t{40}, std::size_t{200}}) {
        INFO("cut " << cut);
        std::vector<std::byte> shorter(bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(read_replay(shorter, error));
    }
    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(read_replay(trailing, error));
    // The player count sits right after the hashes: 5 hashes of 8 bytes are followed by u32 2.
    auto huge = bytes;
    const std::size_t count_at = bytes.size() - 2 * 5 * 32 - 4;
    huge[count_at] = std::byte{0xff};
    huge[count_at + 1] = std::byte{0xff};
    CHECK_FALSE(read_replay(huge, error));
}

TEST_CASE("replay v4: adapting to a moved input map keeps every player's input") {
    InputMap map;
    map.set_action(Name("b"), {Binding::key(Key::B)});
    map.set_action(Name("a"), {Binding::key(Key::A)});
    map.set_axis(Name("move"), {AxisSource::digital(Binding::key(Key::Left), Binding::key(Key::Right))});
    Replay r = sample_replay();
    r.axes = map.axis_names();
    std::string error;
    REQUIRE_MESSAGE(adapt_replay_input(r, map, error), error);
    // "a" was bit 0 and is now bit 1, "b" the reverse.
    CHECK(r.extra_players[0][0].held == 2u);
    CHECK(r.extra_players[0][1].held == 1u);
    CHECK(r.extra_players[1][1].held == 2u);
    CHECK(r.frames[1].held == 2u);
}

TEST_CASE("replay v4: a two-player session replays offline to the same hashes") {
    const fs::path dir = fs::path(CUERVO_EXAMPLES_DIR) / "netduel";
    script::SessionOptions options;
    options.dir = dir;
    options.audio_device = false;
    options.configure_jobs = true;
    options.jobs = 1;
    std::string error;
    Replay recorded;
    {
        script::GameSession s;
        REQUIRE_MESSAGE(s.init(options, error), error);
        s.start_recording();
        const InputMap& map = s.input_map();
        const std::uint64_t right = std::uint64_t{1} << *map.action_index(Name("right"));
        const std::uint64_t shoot = std::uint64_t{1} << *map.action_index(Name("shoot"));
        const std::uint64_t left = std::uint64_t{1} << *map.action_index(Name("left"));
        for (int t = 0; t < 240; ++t) {
            const InputFrame p0 = frame_of(t > 10 ? right | shoot : 0);
            const InputFrame p1 = frame_of(t > 30 ? left | shoot : 0);
            const InputFrame players[2] = {p0, p1};
            s.tick_players(players);
        }
        recorded = s.recorded();
    }
    REQUIRE(recorded.ticks() == 240);
    REQUIRE(recorded.extra_players.size() == 1);
    REQUIRE(recorded.extra_players[0].size() == 240);

    const fs::path file = fs::temp_directory_path() / "cuervo_net_replay_test.replay";
    REQUIRE(save_replay(file, recorded, error));
    const auto loaded = load_replay(file, error);
    REQUIRE_MESSAGE(loaded, error);
    fs::remove(file);
    const script::ReplayCheck ok = script::verify_replay(dir, *loaded, options);
    CHECK_MESSAGE(ok.error.empty(), ok.error);
    CHECK(ok.ok);
    CHECK(ok.compared == 240);
    CHECK(ok.final_hash == recorded.hashes.back());

    // Player 1's input matters: dropping it (as a build ignoring the player dimension would) diverges.
    Replay one_player = *loaded;
    one_player.extra_players.clear();
    const script::ReplayCheck bad = script::verify_replay(dir, one_player, options);
    CHECK_FALSE(bad.ok);
    jobs::configure(1);
}

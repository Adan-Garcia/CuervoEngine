#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cuervo/input/input_map.hpp"

// Replay files (DESIGN.md 4.1, 4.8: "A replay is seed + input frames").
// The simulation is deterministic, so a recorded run is just where it
// started, the seed and the input frame of every tick; the state hash after
// every tick is stored too, so playing it back verifies that the build still
// reproduces the run bit for bit (docs/REPLAYS.md has the byte layout).
namespace cuervo {

struct Replay {
    // 4: the other players' frames of a networked session; 3: actions carry stable IDs; 2: frames carry
    // typed text; version 1 to 3 files still load
    static constexpr std::uint32_t format_version = 4;

    std::string project;   // the project's name (informational)
    std::uint64_t seed = 1;
    float tick_rate = 60.0f;
    // The input map's action and axis names in index order (bit i of
    // InputFrame::held is actions[i]); playback refuses a project whose
    // actions moved, since the bits would mean other actions.
    std::vector<std::string> actions;       // the input map's action names in index order (bit i of a frame)
    std::vector<std::uint32_t> action_ids;  // their stable IDs (InputMap::action_ids); empty in version 1 and 2 files
    std::vector<std::string> axes;
    // Empty: the run started from the project's main (main_scene or the main
    // class), as `cuervo --game` starts. Otherwise the scene that was played
    // (its project-relative path or a label), embedded in `scene` (the
    // scene file bytes), so unsaved editor edits replay too.
    std::string start_scene;
    std::vector<std::byte> scene;
    std::vector<InputFrame> frames;     // frames[i] is the input of tick i + 1 (player 0)
    // Players 1.. of a networked session (DESIGN.md 4.19): extra_players[p - 1][i] is player p's input of
    // tick i + 1, as delivered by lockstep or confirmed by rollback. Empty for a local game.
    std::vector<std::vector<InputFrame>> extra_players;
    std::vector<std::uint64_t> hashes;  // hashes[i] is the state hash after tick i + 1

    std::uint64_t ticks() const { return frames.size(); }
};

// `version` < 4 writes the older layouts (extra players dropped); < 3 (tests of the loaders; the IDs are dropped).
std::vector<std::byte> write_replay(const Replay& replay, std::uint32_t version = Replay::format_version);
std::optional<Replay> read_replay(std::span<const std::byte> data, std::string& error);
// Maps a replay recorded under another input map onto `map` (DESIGN.md 4.8): actions are matched by
// stable ID, else by name (version 1 and 2 files have only names), and the frames' held bits and axes are
// moved to the current indices; afterwards replay.actions/action_ids/axes equal the map's. Adding,
// removing or reordering actions therefore keeps a replay playable. False (with `error`) when the replay
// presses an action or moves an axis the map no longer has.
bool adapt_replay_input(Replay& replay, const InputMap& map, std::string& error);
bool save_replay(const std::filesystem::path& path, const Replay& replay, std::string& error);
std::optional<Replay> load_replay(const std::filesystem::path& path, std::string& error);

} // namespace cuervo

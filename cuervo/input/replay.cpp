#include "cuervo/input/replay.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>

#include "cuervo/core/binary_io.hpp"
#include "cuervo/platform/platform.hpp"

namespace cuervo {

namespace {

constexpr char magic[4] = {'C', 'R', 'P', 'L'};
// Bytes per stored frame: held (8), axes (8 x 2), pointer (2 x 2), and from
// version 2 the typed text code point (4).
constexpr std::size_t frame_bytes_v1 = 8 + InputFrame::max_axes * 2 + 2 * 2;
constexpr std::size_t frame_bytes_v2 = frame_bytes_v1 + 4;

} // namespace

std::vector<std::byte> write_replay(const Replay& replay, std::uint32_t version) {
    ByteWriter w;
    w.raw(magic, 4);
    w.u32(version);
    w.str(replay.project);
    w.u64(replay.seed);
    w.f32(replay.tick_rate);
    w.u32(static_cast<std::uint32_t>(replay.actions.size()));
    for (std::size_t i = 0; i < replay.actions.size(); ++i) {
        if (version >= 3) w.u32(i < replay.action_ids.size() ? replay.action_ids[i] : 0);
        w.str(replay.actions[i]);
    }
    w.u32(static_cast<std::uint32_t>(replay.axes.size()));
    for (const std::string& a : replay.axes) w.str(a);
    w.str(replay.start_scene);
    w.u32(static_cast<std::uint32_t>(replay.scene.size()));
    w.raw(replay.scene.data(), replay.scene.size());
    // Field by field, so the file never depends on struct padding.
    w.u32(static_cast<std::uint32_t>(replay.frames.size()));
    for (const InputFrame& f : replay.frames) {
        w.u64(f.held);
        for (std::int16_t a : f.axes) w.raw(&a, 2);
        for (std::int16_t p : f.pointer) w.raw(&p, 2);
        if (version >= 2) w.u32(f.text);
    }
    w.u32(static_cast<std::uint32_t>(replay.hashes.size()));
    for (std::uint64_t h : replay.hashes) w.u64(h);
    if (version >= 4) {
        // Version 4: the other players of a networked session (DESIGN.md 4.19), each with one frame per tick.
        w.u32(static_cast<std::uint32_t>(replay.extra_players.size()));
        for (const std::vector<InputFrame>& player : replay.extra_players) {
            for (std::size_t t = 0; t < replay.frames.size(); ++t) {
                const InputFrame f = t < player.size() ? player[t] : InputFrame{};
                w.u64(f.held);
                for (std::int16_t a : f.axes) w.raw(&a, 2);
                for (std::int16_t p : f.pointer) w.raw(&p, 2);
                w.u32(f.text);
            }
        }
    }
    return w.take();
}

std::optional<Replay> read_replay(std::span<const std::byte> data, std::string& error) {
    ByteReader r(data, "replay");
    char m[4] = {};
    r.raw(m, 4);
    if (!r.ok() || !std::equal(m, m + 4, magic)) {
        error = "not a replay file (bad magic)";
        return std::nullopt;
    }
    const std::uint32_t version = r.u32();
    if (version == 0 || version > Replay::format_version) {
        error = std::format("replay format version {} is newer than this build reads ({})", version,
                            Replay::format_version);
        return std::nullopt;
    }
    Replay out;
    out.project = r.str();
    out.seed = r.u64();
    out.tick_rate = r.f32();
    const std::uint32_t actions = r.count(version >= 3 ? 8 : 4);
    for (std::uint32_t i = 0; i < actions && r.ok(); ++i) {
        if (version >= 3) out.action_ids.push_back(r.u32());
        out.actions.push_back(r.str());
    }
    const std::uint32_t axes = r.count(4);
    for (std::uint32_t i = 0; i < axes && r.ok(); ++i) out.axes.push_back(r.str());
    out.start_scene = r.str();
    out.scene.resize(r.count(1));
    r.raw(out.scene.data(), out.scene.size());
    const std::uint32_t frames = r.count(version >= 2 ? frame_bytes_v2 : frame_bytes_v1);
    out.frames.resize(frames);
    for (InputFrame& f : out.frames) {
        f.held = r.u64();
        for (std::int16_t& a : f.axes) r.raw(&a, 2);
        for (std::int16_t& p : f.pointer) r.raw(&p, 2);
        if (version >= 2) f.text = r.u32();
    }
    const std::uint32_t hashes = r.count(8);
    out.hashes.resize(hashes);
    for (std::uint64_t& h : out.hashes) h = r.u64();
    if (version >= 4) {
        constexpr std::uint32_t max_players = 15;
        const std::uint32_t extras = r.u32();
        if (r.ok() && extras > max_players) {
            error = "replay: too many players";
            return std::nullopt;
        }
        for (std::uint32_t p = 0; p < extras && r.ok(); ++p) {
            if (static_cast<std::uint64_t>(frames) * frame_bytes_v2 > r.remaining()) {
                r.fail("truncated player frames");
                break;
            }
            std::vector<InputFrame>& player = out.extra_players.emplace_back(frames);
            for (InputFrame& f : player) {
                f.held = r.u64();
                for (std::int16_t& a : f.axes) r.raw(&a, 2);
                for (std::int16_t& p2 : f.pointer) r.raw(&p2, 2);
                f.text = r.u32();
            }
        }
    }
    if (!r.ok()) {
        error = r.error();
        return std::nullopt;
    }
    if (!r.at_end()) {
        error = version >= 4 ? "replay: unexpected data after the player frames"
                             : "replay: unexpected data after the hashes";
        return std::nullopt;
    }
    if (out.hashes.size() > out.frames.size()) {
        error = "replay: more hashes than ticks";
        return std::nullopt;
    }
    if (actions > InputFrame::max_actions || axes > InputFrame::max_axes || !(out.tick_rate > 0.0f)) {
        error = "replay: invalid header";
        return std::nullopt;
    }
    return out;
}

bool adapt_replay_input(Replay& replay, const InputMap& map, std::string& error) {
    const std::vector<std::string> names = map.action_names();
    const std::vector<std::uint32_t> ids = map.action_ids();
    const std::vector<std::string> axes = map.axis_names();
    if (replay.actions == names && replay.axes == axes && (replay.action_ids.empty() || replay.action_ids == ids)) {
        replay.action_ids = ids;
        return true;
    }
    std::uint64_t used_actions = 0;
    std::array<bool, InputFrame::max_axes> used_axes{};
    const auto scan = [&](const std::vector<InputFrame>& list) {
        for (const InputFrame& f : list) {
            used_actions |= f.held;
            for (std::size_t i = 0; i < f.axes.size(); ++i) used_axes[i] = used_axes[i] || f.axes[i] != 0;
        }
    };
    scan(replay.frames);
    for (const auto& player : replay.extra_players) scan(player);
    const auto find_name = [](const std::vector<std::string>& list, const std::string& name) -> int {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i] == name) return static_cast<int>(i);
        }
        return -1;
    };
    std::vector<int> action_to(replay.actions.size(), -1);
    for (std::size_t i = 0; i < replay.actions.size(); ++i) {
        if (i < replay.action_ids.size() && replay.action_ids[i] != 0) {
            if (auto idx = map.action_index_by_id(replay.action_ids[i])) action_to[i] = static_cast<int>(*idx);
        }
        if (action_to[i] < 0) action_to[i] = find_name(names, replay.actions[i]);
        if (action_to[i] < 0 && i < 64 && ((used_actions >> i) & 1u) != 0) {
            error = std::format("the replay presses the input action '{}', which the project no longer has",
                                replay.actions[i]);
            return false;
        }
    }
    std::vector<int> axis_to(replay.axes.size(), -1);
    for (std::size_t i = 0; i < replay.axes.size(); ++i) {
        axis_to[i] = find_name(axes, replay.axes[i]);
        if (axis_to[i] < 0 && i < used_axes.size() && used_axes[i]) {
            error = std::format("the replay moves the input axis '{}', which the project no longer has", replay.axes[i]);
            return false;
        }
    }
    const auto remap = [&](std::vector<InputFrame>& list) {
    for (InputFrame& f : list) {
        std::uint64_t held = 0;
        for (std::size_t i = 0; i < action_to.size() && i < 64; ++i) {
            if (action_to[i] >= 0 && ((f.held >> i) & 1u) != 0) held |= std::uint64_t{1} << action_to[i];
        }
        std::array<std::int16_t, InputFrame::max_axes> moved{};
        for (std::size_t i = 0; i < axis_to.size() && i < f.axes.size(); ++i) {
            if (axis_to[i] >= 0) moved[static_cast<std::size_t>(axis_to[i])] = f.axes[i];
        }
        f.held = held;
        f.axes = moved;
    }
    };
    remap(replay.frames);
    for (auto& player : replay.extra_players) remap(player);
    replay.actions = names;
    replay.action_ids = ids;
    replay.axes = axes;
    return true;
}

bool save_replay(const std::filesystem::path& path, const Replay& replay, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    const std::vector<std::byte> bytes = write_replay(replay);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        error = std::format("cannot write '{}'", platform::utf8(path));
        return false;
    }
    return true;
}

std::optional<Replay> load_replay(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::format("cannot read '{}'", platform::utf8(path));
        return std::nullopt;
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto r = read_replay(std::as_bytes(std::span(bytes)), error);
    if (!r) error = std::format("{}: {}", platform::utf8(path), error);
    return r;
}

} // namespace cuervo

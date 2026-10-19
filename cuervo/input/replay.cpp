#include "cuervo/input/replay.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

#include "cuervo/core/binary_io.hpp"
#include "cuervo/platform/platform.hpp"

namespace cuervo {

namespace {

constexpr char magic[4] = {'C', 'R', 'P', 'L'};
// Bytes per stored frame: held (8), axes (8 x 2), pointer (2 x 2).
constexpr std::size_t frame_bytes = 8 + InputFrame::max_axes * 2 + 2 * 2;

} // namespace

std::vector<std::byte> write_replay(const Replay& replay) {
    ByteWriter w;
    w.raw(magic, 4);
    w.u32(Replay::format_version);
    w.str(replay.project);
    w.u64(replay.seed);
    w.f32(replay.tick_rate);
    w.u32(static_cast<std::uint32_t>(replay.actions.size()));
    for (const std::string& a : replay.actions) w.str(a);
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
    }
    w.u32(static_cast<std::uint32_t>(replay.hashes.size()));
    for (std::uint64_t h : replay.hashes) w.u64(h);
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
    const std::uint32_t actions = r.count(4);
    for (std::uint32_t i = 0; i < actions && r.ok(); ++i) out.actions.push_back(r.str());
    const std::uint32_t axes = r.count(4);
    for (std::uint32_t i = 0; i < axes && r.ok(); ++i) out.axes.push_back(r.str());
    out.start_scene = r.str();
    out.scene.resize(r.count(1));
    r.raw(out.scene.data(), out.scene.size());
    const std::uint32_t frames = r.count(frame_bytes);
    out.frames.resize(frames);
    for (InputFrame& f : out.frames) {
        f.held = r.u64();
        for (std::int16_t& a : f.axes) r.raw(&a, 2);
        for (std::int16_t& p : f.pointer) r.raw(&p, 2);
    }
    const std::uint32_t hashes = r.count(8);
    out.hashes.resize(hashes);
    for (std::uint64_t& h : out.hashes) h = r.u64();
    if (!r.ok()) {
        error = r.error();
        return std::nullopt;
    }
    if (!r.at_end()) {
        error = "replay: unexpected data after the hashes";
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

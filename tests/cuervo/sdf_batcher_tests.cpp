#include <doctest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

#include "cuervo/render/sdf_batcher.hpp"
#include "recording_device.hpp"

using namespace cuervo;
using namespace cuervo::render;
using cuervo::test::RecordingDevice;

namespace {

std::array<std::uint8_t, 4> bytes_of(std::uint32_t packed) {
    std::array<std::uint8_t, 4> out{};
    std::memcpy(out.data(), &packed, 4);
    return out;
}

} // namespace

TEST_CASE("SdfInstance layout matches the sdf.vert instance stream") {
    CHECK(sizeof(SdfInstance) == 40);
    CHECK(offsetof(SdfInstance, center) == 0);
    CHECK(offsetof(SdfInstance, half_size) == 8);
    CHECK(offsetof(SdfInstance, rotation) == 16);
    CHECK(offsetof(SdfInstance, param) == 20);
    CHECK(offsetof(SdfInstance, outline_width) == 24);
    CHECK(offsetof(SdfInstance, shape) == 28);
    CHECK(offsetof(SdfInstance, color) == 32);
    CHECK(offsetof(SdfInstance, outline_color) == 36);
    CHECK(sizeof(SdfGlobals) == 48); // std140: three vec4
}

TEST_CASE("pack_rgba8 stores bytes r, g, b, a in memory, rounded and clamped") {
    CHECK(bytes_of(pack_rgba8({1.0f, 0.0f, 0.0f, 0.0f})) == std::array<std::uint8_t, 4>{255, 0, 0, 0});
    CHECK(bytes_of(pack_rgba8({0.0f, 0.0f, 0.0f, 1.0f})) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
    CHECK(bytes_of(pack_rgba8({0.5f, 0.25f, 1.0f / 255.0f, 0.999f})) == std::array<std::uint8_t, 4>{128, 64, 1, 255});
    CHECK(bytes_of(pack_rgba8({-1.0f, 2.0f, 0.0f, 1.0f})) == std::array<std::uint8_t, 4>{0, 255, 0, 255});
}

TEST_CASE("make_* pack each shape's half-extent and parameter") {
    const rhi::Color red{1.0f, 0.0f, 0.0f, 1.0f};

    const SdfInstance c = make_circle({10.0f, 20.0f}, 5.0f, red);
    CHECK(c.shape == SdfShape::Circle);
    CHECK(c.center == vec2(10.0f, 20.0f));
    CHECK(c.half_size == vec2(5.0f, 5.0f));
    CHECK(c.rotation == 0.0f);
    CHECK(c.outline_width == 0.0f);
    CHECK(c.color == pack_rgba8(red));

    const SdfInstance cap = make_capsule({0.0f, 0.0f}, 6.0f, 3.0f, 0.5f, red);
    CHECK(cap.shape == SdfShape::Capsule);
    CHECK(cap.half_size == vec2(9.0f, 3.0f)); // half_length + radius, radius
    CHECK(cap.rotation == 0.5f);

    const SdfInstance rect = make_rounded_rect({0.0f, 0.0f}, {8.0f, 4.0f}, 10.0f, -1.0f, red);
    CHECK(rect.shape == SdfShape::RoundedRect);
    CHECK(rect.half_size == vec2(8.0f, 4.0f));
    CHECK(rect.param == 4.0f); // corner radius clamped to the smaller half-extent
    CHECK(rect.rotation == -1.0f);

    const SdfInstance ring = make_ring({0.0f, 0.0f}, 6.0f, 2.0f, red);
    CHECK(ring.shape == SdfShape::Ring);
    CHECK(ring.half_size == vec2(6.0f, 6.0f));
    CHECK(ring.param == 2.0f);
    CHECK(make_ring({}, 3.0f, 10.0f, red).param == 3.0f); // thickness <= radius

    // Negative sizes collapse to zero instead of producing inside-out shapes.
    CHECK(make_circle({}, -4.0f, red).half_size == vec2(0.0f, 0.0f));
    CHECK(make_capsule({}, -2.0f, 1.0f, 0.0f, red).half_size == vec2(1.0f, 1.0f));
}

TEST_CASE("Outlines are packed per instance") {
    const SdfInstance c = make_circle({}, 5.0f, {0.0f, 0.0f, 1.0f, 1.0f}, {1.5f, {1.0f, 1.0f, 1.0f, 0.5f}});
    CHECK(c.outline_width == 1.5f);
    CHECK(bytes_of(c.outline_color) == std::array<std::uint8_t, 4>{255, 255, 255, 128});
    CHECK(make_circle({}, 5.0f, {}, {-3.0f, {}}).outline_width == 0.0f);
}

TEST_CASE("make_sdf_globals writes the affine as two std140 rows") {
    const Affine2 m{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const SdfGlobals g = make_sdf_globals(m, 0.75f);
    CHECK(g.row0[0] == 1.0f); // a
    CHECK(g.row0[1] == 3.0f); // c
    CHECK(g.row0[2] == 5.0f); // tx
    CHECK(g.row1[0] == 2.0f); // b
    CHECK(g.row1[1] == 4.0f); // d
    CHECK(g.row1[2] == 6.0f); // ty
    CHECK(g.params[0] == 0.75f);
}

TEST_CASE("SdfBatcher collects instances contiguously in submission order") {
    SdfBatcher batch; // no device: collects, draws nothing
    const rhi::Color white{1.0f, 1.0f, 1.0f, 1.0f};
    batch.circle({1.0f, 1.0f}, 2.0f, white);
    batch.capsule({2.0f, 2.0f}, 3.0f, 1.0f, 0.25f, white);
    batch.rounded_rect({3.0f, 3.0f}, {4.0f, 2.0f}, 1.0f, 0.0f, white);
    batch.ring({4.0f, 4.0f}, 5.0f, 1.0f, white);

    REQUIRE(batch.size() == 4);
    const std::span<const SdfInstance> instances = batch.instances();
    CHECK(instances[0].shape == SdfShape::Circle);
    CHECK(instances[1].shape == SdfShape::Capsule);
    CHECK(instances[2].shape == SdfShape::RoundedRect);
    CHECK(instances[3].shape == SdfShape::Ring);
    CHECK(&instances[3] - &instances[0] == 3); // one contiguous array
    CHECK(instances[3].center == vec2(4.0f, 4.0f));

    batch.flush(Affine2{}, 1.0f); // no device: a no-op
    CHECK(batch.size() == 4);     // flush keeps instances
    batch.clear();
    CHECK(batch.size() == 0);
}

TEST_CASE("SdfBatcher::flush uploads once and issues one instanced draw") {
    RecordingDevice device;
    SdfBatcher batch;
    REQUIRE(batch.init(device, 16));
    REQUIRE(device.pipelines.size() == 1);

    // Pipeline: stream 0 = per-vertex quad corners, stream 1 = per-instance SdfInstance.
    const RecordingDevice::Pipeline& desc = device.pipelines[0];
    REQUIRE(desc.streams.size() == 2);
    CHECK(desc.streams[0].rate == rhi::VertexRate::PerVertex);
    CHECK(desc.streams[0].stride == 2 * sizeof(float));
    CHECK(desc.streams[1].rate == rhi::VertexRate::PerInstance);
    CHECK(desc.streams[1].stride == sizeof(SdfInstance));
    CHECK(desc.streams[1].attributes == 6);
    CHECK(desc.blend == rhi::BlendMode::Premultiplied);
    CHECK(desc.has_spirv);
    CHECK(desc.has_es300);

    // More than the initial capacity: the instance buffer grows before upload.
    constexpr std::size_t count = 5000;
    for (std::size_t i = 0; i < count; ++i) {
        batch.circle({static_cast<float>(i), 0.0f}, 1.0f, {1.0f, 0.5f, 0.0f, 1.0f});
    }
    const int updates_before = device.updates;
    batch.flush(Affine2{}, 1.0f);

    REQUIRE(device.draws.size() == 1);
    const RecordingDevice::Draw& draw = device.draws[0];
    CHECK(draw.index_count == 6);
    CHECK(draw.instance_count == count);
    CHECK(draw.first_index == 0);
    CHECK(draw.first_instance == 0);
    CHECK(device.updates - updates_before == 2); // globals + one instance upload
    CHECK(batch.capacity() >= count);

    // The bound instance buffer holds exactly the batch's instances.
    const std::uint32_t instance_buffer = draw.vertex_buffers[1];
    REQUIRE(device.buffers.contains(instance_buffer));
    CHECK(device.descs[instance_buffer].usage == rhi::BufferUsage::Stream);
    const std::vector<std::byte>& uploaded = device.buffers[instance_buffer];
    REQUIRE(uploaded.size() >= count * sizeof(SdfInstance));
    CHECK(std::memcmp(uploaded.data(), batch.instances().data(), count * sizeof(SdfInstance)) == 0);

    // Globals were uploaded to the bound uniform buffer.
    REQUIRE(device.buffers.contains(draw.uniform_buffer));
    CHECK(device.buffers[draw.uniform_buffer].size() == sizeof(SdfGlobals));

    // An empty batch draws nothing.
    batch.clear();
    batch.flush(Affine2{}, 1.0f);
    CHECK(device.draws.size() == 1);

    batch.shutdown();
}

TEST_CASE("SdfBatcher::arc strokes with one Arc instance (no overlapping joints)") {
    SdfBatcher batch;
    const rhi::Color blue{0.0f, 0.5f, 1.0f, 0.5f};

    batch.arc({100.0f, 100.0f}, 50.0f, 0.0f, dmath::half_pi, blue, 4.0f);
    REQUIRE(batch.size() == 1);
    const SdfInstance& inst = batch.instances()[0];
    CHECK(inst.shape == SdfShape::Arc);
    CHECK(inst.color == pack_rgba8(blue));
    CHECK(inst.half_size == vec2(52.0f, 2.0f)); // (radius + half width, half width)
    CHECK(inst.rotation == doctest::Approx(dmath::pi * 0.25f));
    CHECK(inst.param == doctest::Approx(dmath::pi * 0.25f));

    // Either direction gives the same arc; sweeps past a full turn clamp to a ring.
    const SdfInstance back = make_arc({}, 10.0f, dmath::half_pi, 0.0f, 2.0f, blue);
    CHECK(back.rotation == doctest::Approx(dmath::pi * 0.25f));
    CHECK(back.param == doctest::Approx(dmath::pi * 0.25f));
    CHECK(make_arc({}, 10.0f, 0.0f, 10.0f, 2.0f, blue).param == doctest::Approx(dmath::pi));

    // Degenerate arcs: zero radius, zero width, zero sweep, zero alpha
    batch.clear();
    batch.arc({0.0f, 0.0f}, 0.0f, 0.0f, 1.0f, blue, 1.0f);
    CHECK(batch.size() == 0);
    batch.arc({0.0f, 0.0f}, 10.0f, 0.0f, 1.0f, blue, 0.0f);
    CHECK(batch.size() == 0);
    batch.arc({0.0f, 0.0f}, 10.0f, 1.0f, 1.0f, blue, 1.0f);
    CHECK(batch.size() == 0);
    batch.arc({0.0f, 0.0f}, 10.0f, 0.0f, 1.0f, {1.0f, 1.0f, 1.0f, 0.0f}, 1.0f);
    CHECK(batch.size() == 0);
}

TEST_CASE("SdfBatcher: make_sector and arc fill option") {
    const rhi::Color orange{1.0f, 0.5f, 0.0f, 1.0f};
    const rhi::Color cyan{0.0f, 1.0f, 1.0f, 1.0f};

    // 1. make_sector
    SdfInstance sec = make_sector({50.0f, 60.0f}, 25.0f, 0.0f, dmath::half_pi, orange);
    CHECK(sec.shape == SdfShape::Sector);
    CHECK(sec.center == vec2(50.0f, 60.0f));
    CHECK(sec.half_size == vec2(25.0f, 25.0f));
    CHECK(sec.rotation == doctest::Approx(dmath::pi * 0.25f));
    CHECK(sec.param == doctest::Approx(dmath::pi * 0.25f)); // half-angle
    CHECK(sec.color == pack_rgba8(orange));

    // 2. SdfBatcher::arc with fill only
    SdfBatcher batch;
    batch.arc({10.0f, 20.0f}, 30.0f, 0.0f, dmath::pi, orange, rhi::Color{0, 0, 0, 0}, 0.0f);
    REQUIRE(batch.size() == 1);
    CHECK(batch.instances()[0].shape == SdfShape::Sector);
    CHECK(batch.instances()[0].color == pack_rgba8(orange));

    // 3. SdfBatcher::arc with both fill and stroke: sector, then the stroke on top
    batch.clear();
    batch.arc({10.0f, 20.0f}, 30.0f, 0.0f, dmath::pi, orange, cyan, 2.0f);
    REQUIRE(batch.size() == 2);
    CHECK(batch.instances()[0].shape == SdfShape::Sector);
    CHECK(batch.instances()[0].color == pack_rgba8(orange));
    CHECK(batch.instances()[1].shape == SdfShape::Arc);
    CHECK(batch.instances()[1].color == pack_rgba8(cyan));
}


TEST_CASE("SdfBatcher grows its instance buffer by a quarter, not double") {
    // The stream buffer exists in several versions per backend; doubling past
    // 2^21 instances put 168 MB x 3 of mostly unused memory on the GPU and
    // pushed Vulkan out of the 256 MB BAR (the 1.9M-particle cliff).
    RecordingDevice device;
    SdfBatcher batch;
    REQUIRE(batch.init(device, 256));
    std::size_t last = batch.capacity();
    std::size_t growths = 0;
    for (std::size_t count = 1000; count <= 3'000'000; count += 1000) {
        batch.append(1000);
        batch.flush(Affine2{}, 1.0f);
        if (batch.capacity() != last) {
            CHECK(batch.capacity() <= last + last / 4 + 1000); // a quarter, or just enough for the count
            CHECK(batch.capacity() >= count);
            last = batch.capacity();
            ++growths;
        }
    }
    CHECK(growths > 10);
    CHECK(batch.capacity() < 3'000'000 + 3'000'000 / 4 + 1000);
    batch.shutdown();
}

TEST_CASE("SdfBatcher append hands out writable instances after earlier ones") {
    SdfBatcher batch;
    batch.add(make_circle({1.0f, 2.0f}, 3.0f, {1.0f, 1.0f, 1.0f, 1.0f}));
    const std::span<SdfInstance> fresh = batch.append(4);
    REQUIRE(fresh.size() == 4);
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        fresh[i] = make_circle({static_cast<float>(i), 0.0f}, 1.0f, {0.0f, 0.0f, 0.0f, 1.0f});
    }
    REQUIRE(batch.size() == 5);
    CHECK(batch.instances()[0].half_size.x() == 3.0f);
    CHECK(batch.instances()[3].center.x() == 2.0f);
}

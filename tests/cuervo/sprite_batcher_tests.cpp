#include <doctest.h>

#include <cstddef>
#include <cstring>
#include <vector>

#include "cuervo/assets/image.hpp"
#include "cuervo/render/sprite_batcher.hpp"
#include "recording_device.hpp"

using namespace cuervo;
using namespace cuervo::render;
using cuervo::test::RecordingDevice;

TEST_CASE("SpriteInstance layout matches the sprite.vert instance stream") {
    CHECK(sizeof(SpriteInstance) == 40);
    CHECK(offsetof(SpriteInstance, uv) == 24);
}

TEST_CASE("sprite_uv maps a texel rectangle and flips swap the pair") {
    const SpriteUv uv = sprite_uv(64, 32, 16, 8, 16, 8);
    CHECK(uv.u0 == 0.25f);
    CHECK(uv.v0 == 0.25f);
    CHECK(uv.u1 == 0.5f);
    CHECK(uv.v1 == 0.5f);
    const SpriteUv fx = sprite_uv(64, 32, 16, 8, 16, 8, true, false);
    CHECK(fx.u0 == 0.5f);
    CHECK(fx.u1 == 0.25f);
    CHECK(fx.v0 == 0.25f);
    const SpriteUv fy = sprite_uv(64, 32, 16, 8, 16, 8, false, true);
    CHECK(fy.v0 == 0.5f);
    CHECK(fy.v1 == 0.25f);
}

TEST_CASE("make_sprite stores half the size, the tint bytes and the uv rectangle") {
    const SpriteInstance s = make_sprite({10, 20}, {8, 6}, 0.5f, {0.0f, 0.5f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 0.5f});
    CHECK(s.half_size.x() == 4.0f);
    CHECK(s.half_size.y() == 3.0f);
    CHECK(s.rotation == 0.5f);
    std::uint8_t bytes[4];
    std::memcpy(bytes, &s.color, 4);
    CHECK(bytes[0] == 255);
    CHECK(bytes[1] == 0);
    CHECK(bytes[3] == 128);
    CHECK(s.uv[1] == 0.5f);
}

TEST_CASE("SpriteBatcher draws one instanced draw per texture run") {
    RecordingDevice device;
    SpriteBatcher batch;
    REQUIRE(batch.init(device, 16));
    REQUIRE(device.pipelines.size() == 1);
    const RecordingDevice::Pipeline& desc = device.pipelines[0];
    REQUIRE(desc.streams.size() == 2);
    CHECK(desc.streams[1].stride == sizeof(SpriteInstance));
    CHECK(desc.streams[1].attributes == 5);
    CHECK(desc.blend == rhi::BlendMode::Premultiplied);
    CHECK(desc.has_spirv);
    CHECK(desc.has_es300);
    REQUIRE(desc.textures.size() == 1);

    const rhi::TextureHandle a = device.create_texture({4, 4, rhi::TextureFormat::RGBA8, rhi::Filter::Nearest, nullptr, "a"});
    const rhi::TextureHandle b = device.create_texture({4, 4, rhi::TextureFormat::RGBA8, rhi::Filter::Linear, nullptr, "b"});
    const SpriteUv uv{};
    for (int i = 0; i < 3; ++i) batch.add(a, {static_cast<float>(i), 0}, {1, 1}, 0.0f, uv);
    for (int i = 0; i < 2; ++i) batch.add(b, {static_cast<float>(i), 1}, {1, 1}, 0.0f, uv);
    batch.add(a, {0, 2}, {1, 1}, 0.0f, uv); // a again: a third run, order is kept
    CHECK(batch.size() == 6);
    CHECK(batch.run_count() == 3);

    batch.flush(Affine2{});
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws[0].instance_count == 3);
    CHECK(device.draws[0].first_instance == 0);
    CHECK(device.draws[0].textures[0] == a.id);
    CHECK(device.draws[1].instance_count == 2);
    CHECK(device.draws[1].first_instance == 3);
    CHECK(device.draws[1].textures[0] == b.id);
    CHECK(device.draws[2].first_instance == 5);

    // A second flush draws only what was added since, even inside a run.
    batch.add(a, {1, 2}, {1, 1}, 0.0f, uv);
    batch.flush(Affine2{});
    REQUIRE(device.draws.size() == 4);
    CHECK(device.draws[3].instance_count == 1);
    CHECK(device.draws[3].first_instance == 6);
    batch.flush(Affine2{});
    CHECK(device.draws.size() == 4);
    batch.clear();
    CHECK(batch.size() == 0);
    CHECK(batch.run_count() == 0);
}

TEST_CASE("create_sprite_texture uploads the whole mip chain") {
    RecordingDevice device;
    assets::Image image;
    image.width = image.height = 8;
    image.rgba.assign(8 * 8 * 4, 200);
    const assets::TextureData data = assets::build_texture(image, true);
    REQUIRE(data.mip_count() == 4);
    const rhi::TextureHandle t = create_sprite_texture(device, data, rhi::Filter::Linear);
    REQUIRE(t);
    const rhi::TextureDesc& desc = device.textures.at(t.id).desc;
    CHECK(desc.mip_levels == 4);
    CHECK_FALSE(desc.srgb);
    CHECK(desc.filter == rhi::Filter::Linear);
}

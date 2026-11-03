#include <doctest.h>

#include <algorithm>
#include <vector>

#include <filesystem>
#include <fstream>

#include "cuervo/render/post_fx.hpp"
#include "cuervo/script/project.hpp"
#include "recording_device.hpp"

using namespace cuervo;
using cuervo::test::RecordingDevice;

TEST_CASE("make_identity_lut: an N^3 cube as an (N*N x N) strip, red along x, green along y, blue slices") {
    const std::vector<std::uint8_t> lut = render::make_identity_lut(4);
    REQUIRE(lut.size() == 16 * 4 * 4);
    const auto at = [&](int x, int y) { return &lut[static_cast<std::size_t>((y * 16 + x) * 4)]; };
    CHECK(at(0, 0)[0] == 0);
    CHECK(at(3, 0)[0] == 255);   // red ramps within a slice
    CHECK(at(0, 3)[1] == 255);   // green ramps down the rows
    CHECK(at(4, 0)[2] == 85);    // slice 1 holds blue = 1/3
    CHECK(at(15, 3)[0] == 255);  // the last texel is white
    CHECK(at(15, 3)[1] == 255);
    CHECK(at(15, 3)[2] == 255);
    CHECK(at(7, 2)[3] == 255);
}

TEST_CASE("PostFx with nothing enabled is one copy pass into the destination") {
    RecordingDevice device;
    device.allow_plain_draws = true;
    render::PostFx post;
    REQUIRE(post.init(device));
    REQUIRE(device.pipelines.size() == 3);
    CHECK(device.pipelines[0].blend == rhi::BlendMode::None);                  // down
    CHECK(device.pipelines[1].blend == rhi::BlendMode::AdditivePremultiplied); // up
    CHECK(device.pipelines[2].blend == rhi::BlendMode::None);                  // composite

    const rhi::RenderTargetHandle scene = device.create_render_target({320, 180, rhi::Filter::Linear, "scene"});
    post.apply(scene, {320, 180}, {}, {});
    REQUIRE(device.plain_draws.size() == 1);
    CHECK(device.plain_draws[0].vertex_count == 3);
    CHECK(device.plain_draws[0].target == 0);
    CHECK(device.plain_draws[0].textures[0] == device.render_target_texture(scene).id);
    CHECK(device.targets.size() == 1); // no pyramid was built
    post.shutdown();
}

TEST_CASE("PostFx bloom: a down chain, an additive up chain, then the composite") {
    RecordingDevice device;
    device.allow_plain_draws = true;
    render::PostFx post;
    REQUIRE(post.init(device));
    const rhi::RenderTargetHandle scene = device.create_render_target({256, 128, rhi::Filter::Linear, "scene"});
    const rhi::RenderTargetHandle dest = device.create_render_target({256, 128, rhi::Filter::Linear, "dest"});

    render::PostSettings settings;
    settings.bloom = true;
    settings.bloom_levels = 3;
    settings.vignette = 0.5f;
    post.apply(scene, {256, 128}, dest, settings);

    // Pyramid levels: 128x64, 64x32, 32x16.
    REQUIRE(device.targets.size() == 5);
    std::vector<ivec2> sizes;
    for (const auto& [id, desc] : device.targets) {
        if (id != scene.id && id != dest.id) sizes.push_back({desc.width, desc.height});
    }
    std::sort(sizes.begin(), sizes.end(), [](ivec2 a, ivec2 b) { return a.x() > b.x(); });
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[0] == ivec2(128, 64));
    CHECK(sizes[1] == ivec2(64, 32));
    CHECK(sizes[2] == ivec2(32, 16));

    // 3 down, 2 up, 1 composite; the last pass draws into the destination.
    REQUIRE(device.plain_draws.size() == 6);
    const auto& draws = device.plain_draws;
    CHECK(draws[0].pipeline == draws[1].pipeline);
    CHECK(draws[0].pipeline == draws[2].pipeline);
    CHECK(draws[3].pipeline == draws[4].pipeline);
    CHECK(draws[3].pipeline != draws[0].pipeline);
    CHECK(draws[5].target == dest.id);
    CHECK(draws[0].textures[0] == device.render_target_texture(scene).id);
    // The composite reads the scene and the largest pyramid level.
    CHECK(draws[5].textures[0] == device.render_target_texture(scene).id);
    CHECK(draws[5].textures[1] == device.render_target_texture({draws[4].target}).id);

    // The same size and level count reuse the pyramid; a new size rebuilds it.
    const std::size_t targets = device.targets.size();
    post.apply(scene, {256, 128}, dest, settings);
    CHECK(device.targets.size() == targets);
    post.apply(scene, {200, 100}, dest, settings);
    CHECK(device.targets.size() == targets);
    CHECK(std::any_of(device.targets.begin(), device.targets.end(),
                      [](const auto& entry) { return entry.second.width == 100 && entry.second.height == 50; }));
    post.shutdown();
    CHECK(device.targets.size() == 2); // the pyramid is released with the effect
}

TEST_CASE("PostFx custom passes bind textures to samplers and draw into the current target") {
    RecordingDevice device;
    device.allow_plain_draws = true;
    render::PostFx post;
    REQUIRE(post.init(device));
    const rhi::PipelineHandle custom = post.create_pass_pipeline(post.vertex(), rhi::BlendMode::Additive, "custom");
    REQUIRE(custom);
    REQUIRE(device.pipelines.size() == 4);
    CHECK(device.pipelines[3].blend == rhi::BlendMode::Additive);

    const rhi::RenderTargetHandle a = device.create_render_target({64, 64, rhi::Filter::Linear, "a"});
    rhi::TextureDesc other_desc;
    other_desc.width = other_desc.height = 4;
    const rhi::TextureHandle other = device.create_texture(other_desc);
    device.set_render_target(a);
    const rhi::TextureHandle inputs[] = {other, device.render_target_texture(a)};
    post.fullscreen(custom, inputs, {}, false);
    REQUIRE(device.plain_draws.size() == 1);
    CHECK(device.plain_draws[0].target == a.id);
    CHECK(device.plain_draws[0].pipeline == custom.id);
    CHECK(device.plain_draws[0].textures[0] == other.id);
    CHECK(device.plain_draws[0].textures[1] == inputs[1].id);
    CHECK(post.passes() == 1);
    post.shutdown();
}

namespace {
std::optional<script::Project> load_toml(const std::string& body, std::string& error) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cuervo_post_project";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "project.toml") << "name = \"T\"\nmain = \"Main\"\n" << body;
    return script::load_project(dir, error);
}
} // namespace

TEST_CASE("project.toml: [render] color_space and [render.post]") {
    std::string error;
    auto plain = load_toml("", error);
    REQUIRE(plain);
    CHECK_FALSE(plain->linear_color);
    CHECK_FALSE(plain->post_enabled);

    auto p = load_toml("[render]\ncolor_space = \"linear\"\n[render.post]\nbloom = true\nbloom_levels = 4\n"
                       "vignette = 0.5\ncrt = true\nlut = \"assets/grade.png\"\n", error);
    REQUIRE_MESSAGE(p, error);
    CHECK(p->linear_color);
    CHECK(p->post_enabled);
    CHECK(p->post.bloom);
    CHECK(p->post.bloom_levels == 4);
    CHECK(p->post.vignette == doctest::Approx(0.5f));
    CHECK(p->post.crt);
    CHECK(p->post_lut == "assets/grade.png");

    CHECK_FALSE(load_toml("[render]\ncolor_space = \"hdr\"\n", error));
    CHECK(error.find("color_space") != std::string::npos);
    CHECK_FALSE(load_toml("[render.post]\nvignette = 3\n", error));
    CHECK_FALSE(load_toml("[render.post]\nlut = \"../x.png\"\n", error));
    CHECK_FALSE(load_toml("[render]\nbogus = 1\n", error));
}

TEST_CASE("srgb_to_linear follows the sRGB transfer function") {
    const rhi::Color c = render::srgb_to_linear({0.5f, 0.0f, 1.0f, 0.25f});
    CHECK(c.r == doctest::Approx(0.2140f).epsilon(0.001));
    CHECK(c.g == 0.0f);
    CHECK(c.b == doctest::Approx(1.0f));
    CHECK(c.a == 0.25f); // alpha stays linear
}

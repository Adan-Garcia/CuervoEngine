#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/render/sprite_batcher.hpp"
#include "cuervo/render/sprite_library.hpp"

#include "cuervo/assets/atlas_file.hpp"
#include "cuervo/assets/svg_raster.hpp"

// SVG raster fallback, atlas spec files and the .catlas format (milestone 11.2a, slice 3).
using namespace cuervo::assets;

namespace {

const char* kRed = R"x(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><rect width="8" height="8" fill="#ff0000"/></svg>)x";
const char* kHalf = R"x(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><rect width="4" height="8" fill="#0000ff"/></svg>)x";
const char* kGrad = R"x(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="4"><defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#000000"/><stop offset="1" stop-color="#ffffff"/></linearGradient></defs><rect width="16" height="4" fill="url(#g)"/></svg>)x";
const char* kFilter = R"x(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><defs><filter id="f"><feGaussianBlur stdDeviation="2"/></filter></defs><rect width="8" height="8" fill="#00ff00" filter="url(#f)"/></svg>)x";

const std::uint8_t* px(const Image& im, int x, int y) {
    return &im.rgba[static_cast<std::size_t>(y * im.width + x) * 4];
}

FileReader reader(const std::map<std::string, std::string>& files) {
    return [&files](std::string_view path, std::vector<std::byte>& out, std::string& error) {
        auto it = files.find(std::string(path));
        if (it == files.end()) {
            error = "missing " + std::string(path);
            return false;
        }
        out.assign(reinterpret_cast<const std::byte*>(it->second.data()),
                   reinterpret_cast<const std::byte*>(it->second.data()) + it->second.size());
        return true;
    };
}

} // namespace

TEST_CASE("svg raster: a solid rect fills its image, scale sets the size") {
    auto r = rasterize_svg(kRed, 1.0f);
    REQUIRE(r.raster);
    CHECK(r.unsupported.empty());
    CHECK(r.raster->image.width == 8);
    const Image& im = r.raster->image;
    for (int y : {0, 3, 7}) {
        for (int x : {0, 4, 7}) {
            CHECK(px(im, x, y)[0] == 255);
            CHECK(px(im, x, y)[1] == 0);
            CHECK(px(im, x, y)[3] == 255);
        }
    }
    auto r2 = rasterize_svg(kRed, 2.0f);
    REQUIRE(r2.raster);
    CHECK(r2.raster->image.width == 16);
    CHECK(r2.raster->image.height == 16);
    CHECK(px(r2.raster->image, 15, 15)[3] == 255);
}

TEST_CASE("svg raster: partial coverage is antialiased, outside stays transparent") {
    auto r = rasterize_svg(kHalf, 1.0f);
    REQUIRE(r.raster);
    const Image& im = r.raster->image;
    CHECK(px(im, 1, 4)[2] == 255);
    CHECK(px(im, 1, 4)[3] == 255);
    CHECK(px(im, 6, 4)[3] == 0);
    // Half a pixel of coverage at a fractional edge.
    auto f = rasterize_svg(R"x(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect width="1.5" height="4"/></svg>)x", 1.0f);
    REQUIRE(f.raster);
    CHECK(px(f.raster->image, 0, 0)[3] == 255);
    CHECK(px(f.raster->image, 1, 0)[3] == 128);
    CHECK(px(f.raster->image, 2, 0)[3] == 0);
}

TEST_CASE("svg raster: linear gradients ramp left to right") {
    auto r = rasterize_svg(kGrad, 1.0f);
    REQUIRE(r.raster);
    const Image& im = r.raster->image;
    CHECK(px(im, 0, 1)[0] < 24);
    CHECK(px(im, 15, 1)[0] > 230);
    CHECK(px(im, 8, 1)[0] > px(im, 4, 1)[0]);
}

TEST_CASE("svg raster: unsupported features are reported, deterministic bytes") {
    auto r = rasterize_svg(kFilter, 1.0f);
    REQUIRE(r.raster);
    CHECK_FALSE(r.unsupported.empty());
    CHECK(needs_raster_fallback(r.unsupported));
    auto a = rasterize_svg(kGrad, 3.0f);
    auto b = rasterize_svg(kGrad, 3.0f);
    CHECK(a.raster->image.rgba == b.raster->image.rgba);
    CHECK_FALSE(rasterize_svg("<svg", 1.0f).raster);
}

TEST_CASE("atlas spec: parse sprites, sheets and rejects bad input") {
    auto ok = parse_atlas_spec(R"({"max_size":512,"padding":2,"extrude":false,
        "sprites":["a/red.svg",{"path":"a/half.svg","scale":2,"name":"half"}],
        "sheets":[{"image":"a/sheet.png","frame_w":8,"frame_h":8}]})");
    REQUIRE(ok.spec);
    CHECK(ok.spec->max_size == 512);
    CHECK(ok.spec->padding == 2);
    CHECK_FALSE(ok.spec->extrude);
    REQUIRE(ok.spec->sprites.size() == 2);
    CHECK(ok.spec->sprites[1].name == "half");
    CHECK(ok.spec->sprites[1].scale == 2.0f);
    CHECK(ok.spec->sheets[0].grid.prefix == "sheet");
    CHECK_FALSE(parse_atlas_spec("{}").spec);
    CHECK_FALSE(parse_atlas_spec("nope").spec);
    CHECK_FALSE(parse_atlas_spec(R"({"sprites":[3]})").spec);
    CHECK_FALSE(parse_atlas_spec(R"({"sheets":[{"image":"x.png"}]})").spec);
}

TEST_CASE("catlas: build is deterministic and round-trips; svg sprites pack by name") {
    std::map<std::string, std::string> files{{"a/red.svg", kRed}, {"a/half.svg", kHalf}, {"a/grad.svg", kGrad}};
    const char* spec = R"({"sprites":["a/red.svg","a/half.svg",{"path":"a/grad.svg","name":"grad","scale":2}]})";
    const char* reordered = R"({"sprites":[{"path":"a/grad.svg","name":"grad","scale":2},"a/half.svg","a/red.svg"]})";
    auto a = build_atlas_from_json(spec, reader(files));
    auto b = build_atlas_from_json(reordered, reader(files));
    REQUIRE(a.atlas);
    REQUIRE(b.atlas);
    CHECK(a.error.empty());
    const auto bytes = encode_catlas(*a.atlas);
    CHECK(bytes == encode_catlas(*b.atlas));
    auto back = decode_catlas(bytes);
    REQUIRE(back);
    CHECK(encode_catlas(*back) == bytes);
    REQUIRE(back->find("a/red") != nullptr);
    CHECK(back->find("a/red")->w == 8);
    REQUIRE(back->find("grad") != nullptr);
    CHECK(back->find("grad")->w == 32);
    CHECK(back->find("missing") == nullptr);
    CHECK(back->pages[0].mip_count() > 1);
    CHECK_FALSE(decode_catlas(std::span<const std::byte>(bytes).first(bytes.size() - 3)));
    CHECK_FALSE(decode_catlas({}));
    auto missing = build_atlas_from_json(R"({"sprites":["nope.svg"]})", reader(files));
    CHECK_FALSE(missing.atlas);
    CHECK_FALSE(missing.error.empty());
}

// ---- SpriteLibrary and 9-slice ------------------------------------------------------------

namespace {

struct TempProject {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "cuervo_sprite_lib_test";
    TempProject() {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "art");
    }
    ~TempProject() { std::filesystem::remove_all(dir); }
    void write(const std::string& rel, const std::string& text) const {
        std::ofstream(dir / rel, std::ios::binary) << text;
    }
};

} // namespace

TEST_CASE("sprite library: svg, atlas frames and missing files") {
    TempProject p;
    p.write("art/red.svg", kRed);
    p.write("art/half.svg", kHalf);
    p.write("art/ui.atlas", R"({"sprites":["art/red.svg",{"path":"art/half.svg","name":"half","scale":2}]})");
    auto vfs = cuervo::assets::Vfs::dev(p.dir);
    cuervo::render::SpriteLibrary lib;
    auto svg = lib.resolve(*vfs, "art/red.svg", "", cuervo::render::SpriteFilter::Default, 1.0f);
    REQUIRE(svg.ok);
    CHECK(svg.size.x() == 8.0f);
    CHECK(svg.frame_w == 8.0f);
    // A higher draw scale rasterizes a bigger image but keeps the document size.
    auto big = lib.resolve(*vfs, "art/red.svg", "", cuervo::render::SpriteFilter::Default, 3.0f);
    CHECK(big.size.x() == 8.0f);
    CHECK(big.frame_w == 32.0f);
    auto frames = lib.frames(*vfs, "art/ui.atlas");
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == "art/red");
    auto half = lib.resolve(*vfs, "art/ui.atlas", "half", cuervo::render::SpriteFilter::Nearest, 1.0f);
    REQUIRE(half.ok);
    CHECK(half.size.x() == 16.0f);
    CHECK(half.uv.u1 > half.uv.u0);
    CHECK_FALSE(lib.resolve(*vfs, "art/ui.atlas", "nope", cuervo::render::SpriteFilter::Default, 1.0f).ok);
    CHECK_FALSE(lib.resolve(*vfs, "art/missing.png", "", cuervo::render::SpriteFilter::Default, 1.0f).ok);
    CHECK(lib.size(*vfs, "art/ui.atlas", "half").x() == 16.0f);
}

TEST_CASE("nine patch: pieces tile the rectangle, corners keep their size") {
    cuervo::render::SpriteBatcher b;
    const cuervo::rhi::TextureHandle tex{};
    const cuervo::render::NinePatchMargins m{4, 4, 4, 4};
    CHECK(cuervo::render::add_nine_patch(b, tex, {0, 0, 1, 1}, 16, 16, m, {50, 40}, {100, 60}, 0.0f) == 9);
    REQUIRE(b.size() == 9);
    float area = 0.0f;
    for (const auto& i : b.instances()) area += 4.0f * i.half_size.x() * i.half_size.y();
    CHECK(area == doctest::Approx(6000.0f));
    // Top-left corner: 4x4 at the rectangle's top-left (0, 10).
    const auto& tl = b.instances()[0];
    CHECK(tl.half_size.x() == 2.0f);
    CHECK(tl.center.x() == doctest::Approx(2.0f));
    CHECK(tl.center.y() == doctest::Approx(12.0f));
    CHECK(tl.uv[2] == doctest::Approx(0.25f));
    // No center piece; borders wider than the rectangle shrink instead of overlapping.
    b.clear();
    CHECK(cuervo::render::add_nine_patch(b, tex, {0, 0, 1, 1}, 16, 16, m, {0, 0}, {6, 100}, 0.0f, {1, 1, 1, 1}, false) == 6);
    float width = 0.0f;
    for (int i = 0; i < 2; ++i) width += 2.0f * b.instances()[static_cast<std::size_t>(i)].half_size.x();
    CHECK(width == doctest::Approx(6.0f));
    b.clear();
    CHECK(cuervo::render::add_nine_patch(b, tex, {0, 0, 1, 1}, 16, 16, m, {0, 0}, {0, 10}, 0.0f) == 0);
}

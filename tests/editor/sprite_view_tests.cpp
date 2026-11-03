// SpriteView: preview source resolution, zoom maths, slice tool (the Sprite drawer tab's logic).
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cuervo/assets/atlas_file.hpp"
#include "editor/sprite_view.hpp"

using namespace cuervo;
using namespace cuervo::editor;
namespace fs = std::filesystem;

namespace {
// A QOI image of `frames` square frames in a row, each a flat color (every pixel an RGBA op).
std::string make_qoi(int width, int height, int frame_w) {
    std::string out = "qoif";
    for (int v : {width, height}) {
        for (int shift : {24, 16, 8, 0}) out += static_cast<char>((v >> shift) & 0xFF);
    }
    out += static_cast<char>(4);
    out += static_cast<char>(0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int frame = x / frame_w;
            out += static_cast<char>(0xFF);
            out += static_cast<char>(40 + frame * 50);
            out += static_cast<char>(y * 8);
            out += static_cast<char>(200);
            out += static_cast<char>(255);
        }
    }
    for (int i = 0; i < 7; ++i) out += static_cast<char>(0);
    out += static_cast<char>(1);
    return out;
}

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}


fs::path project() {
    const fs::path dir = fs::temp_directory_path() / "cuervo_sprite_view";
    fs::remove_all(dir);
    write_file(dir / "art/sheet.qoi", make_qoi(64, 16, 16));
    return dir;
}
} // namespace

TEST_CASE("SpriteView previews a plain image whole and slices it by a grid") {
    const fs::path dir = project();
    SpriteView v;
    v.set_path(dir, "art/sheet.qoi");
    REQUIRE(v.info().ok);
    CHECK(v.is_image());
    const auto s = v.shown(dir);
    REQUIRE(s);
    CHECK(s->w == 64);
    CHECK(s->h == 16);
    v.fw = 16;
    v.fh = 16;
    std::string err;
    CHECK(v.slice(err).size() == 4);
    v.fw = 32;
    CHECK(v.slice(err).size() == 2);
}

TEST_CASE("SpriteView adds a sheet to an atlas, then previews a frame as a sub-rectangle") {
    const fs::path dir = project();
    SpriteView v;
    v.set_path(dir, "art/sheet.qoi");
    v.fw = 16;
    v.fh = 16;
    std::string msg;
    REQUIRE_MESSAGE(v.add_sheet(dir, msg), msg);
    CHECK(fs::exists(dir / "art/sheet.atlas"));
    v.set_path(dir, v.target);
    REQUIRE(v.is_atlas());
    REQUIRE(v.frames().size() == 4);
    v.set_frame(v.frames()[2]);
    const auto s = v.shown(dir);
    REQUIRE(s);
    CHECK(s->image == "art/sheet.qoi");
    CHECK(s->x == 32);
    CHECK(s->w == 16);
    CHECK(s->full_w == 64);
    // Removing the sheet empties the frame list and marks the atlas unsaved.
    CHECK(v.remove_sheet(0));
    CHECK(v.frames().empty());
    CHECK(v.atlas_dirty());
}

TEST_CASE("SpriteView zoom keeps the point under the pointer fixed") {
    SpriteView v;
    v.zoom = 2.0f;
    v.pan_x = 10.0f;
    v.pan_y = 5.0f;
    const float ix = (50.0f - v.pan_x) / v.zoom;
    v.zoom_about(2.0f, 50.0f, 30.0f);
    CHECK(v.zoom == doctest::Approx(4.0f));
    CHECK((50.0f - v.pan_x) / v.zoom == doctest::Approx(ix));
    v.fit(200.0f, 100.0f, 64.0f, 16.0f);
    CHECK(v.pan_x * 2 + 64.0f * v.zoom == doctest::Approx(200.0f));
    v.zoom_about(1000.0f, 0.0f, 0.0f);
    CHECK(v.zoom == doctest::Approx(32.0f));
}

TEST_CASE("sprite view: an SVG is rasterized and a packed atlas frame is cropped, keyed by content") {
    const fs::path dir = fs::temp_directory_path() / "cuervo_sprite_raster";
    fs::remove_all(dir);
    write_file(dir / "art/box.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='16' viewBox='0 0 32 16'><rect width='32' height='16' fill='#ff0000'/></svg>");
    SpriteView sv;
    sv.set_path(dir, "art/box.svg");
    auto shown = sv.shown(dir);
    REQUIRE(shown);
    REQUIRE(shown->pixels);
    CHECK(shown->w == shown->pixels->width);
    CHECK(shown->w >= 256); // small SVGs are drawn at least 256 px wide so they stay sharp
    CHECK(shown->w == 2 * shown->h);
    CHECK(shown->pixels->rgba[0] == 255); // red, opaque
    CHECK(shown->pixels->rgba[3] == 255);
    const std::string first_key = shown->image;
    sv.set_path(dir, "art/box.svg", true); // a reload (file changed on disk) must change the key
    CHECK(sv.shown(dir)->image != first_key);

    assets::PackedAtlas atlas;
    assets::TextureData page;
    page.width = 8;
    page.height = 4;
    page.data.assign(8 * 4 * 4, 0);
    for (int y = 0; y < 4; ++y) for (int x = 4; x < 8; ++x) { // the right half green
        page.data[(static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)) * 4 + 1] = 200;
        page.data[(static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)) * 4 + 3] = 255;
    }
    page.level_offsets = {0};
    atlas.pages.push_back(page);
    atlas.frames.push_back({"left", 0, 0, 0, 4, 4});
    atlas.frames.push_back({"right", 0, 4, 0, 4, 4});
    const auto bytes = assets::encode_catlas(atlas);
    fs::create_directories(dir / "art");
    std::ofstream(dir / "art/sheet.catlas", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    sv.set_path(dir, "art/sheet.catlas");
    REQUIRE(sv.frames().size() == 2);
    sv.set_frame("right");
    shown = sv.shown(dir);
    REQUIRE(shown);
    REQUIRE(shown->pixels);
    CHECK(shown->w == 4);
    CHECK(shown->pixels->rgba[1] == 200);
    sv.set_frame("left");
    CHECK(sv.shown(dir)->pixels->rgba[3] == 0);
    fs::remove_all(dir);
}

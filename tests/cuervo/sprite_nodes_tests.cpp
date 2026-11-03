// Sprite2D, AnimatedSprite2D and NinePatchRect (milestone 11.2a, slice 3): drawing through
// the sprite batcher in call order, tick-driven flipbooks and their state hash, and the
// scene-file round trip of the new fields. Headless: batchers without a device collect
// instances, the sprite library loads images without uploading them.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cuervo/render/font_import.hpp"
#include "cuervo/render/sprite_batcher.hpp"
#include "cuervo/render/sprite_library.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

fs::path project_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_sprite_node_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    std::ofstream(d / "red.svg") << R"x(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16" fill="#ff0000"/></svg>)x";
    std::ofstream(d / "hero.atlas") << R"x({"sprites":[
        {"path":"red.svg","name":"hero_0"},{"path":"red.svg","name":"hero_1","scale":2},{"path":"red.svg","name":"hero_2","scale":3}]})x";
    return d;
}

const char* kMain = R"(
class T {
    static var frame = 0
    static var done = 0
    static var an: AnimatedSprite2D
}
class Main extends Node {
    fn ready() {
        add_child(Shape())
        var sp = Sprite2D()
        sp.asset = "red.svg"
        sp.position = vec2(10.0, 10.0)
        add_child(sp)
        add_child(Shape())
        var np = NinePatchRect()
        np.asset = "red.svg"
        np.size = vec2(40.0, 30.0)
        np.margin_left = 3.0
        np.margin_top = 3.0
        np.margin_right = 3.0
        np.margin_bottom = 3.0
        add_child(np)
        var an = AnimatedSprite2D()
        an.asset = "hero.atlas"
        an.frames = AnimatedSprite2D.sequence("hero", 3)
        an.fps = 30.0
        an.looping = false
        add_child(an)
        T.an = an
    }
    fn update(dt: float) {
        T.frame = T.an.frame
        if !T.an.playing {
            T.done = 1
        }
    }
}
)";

struct Game {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;
    explicit Game(const fs::path& dir) {
        script::HostOptions o;
        o.project_dir = dir;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", kMain}}, "Main"), host->error());
    }
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            host->tick(InputFrame{});
            REQUIRE_MESSAGE(host->running(), host->error());
        }
    }
};

} // namespace

TEST_CASE("sprite nodes: draw through the sprite batcher in call order") {
    const fs::path dir = project_dir("draw");
    Game g(dir);
    g.run(1);

    render::SdfBatcher shapes;
    render::TextRenderer text;
    auto font_file = render::load_font_file(std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf");
    REQUIRE(font_file.has_value());
    const render::FontHandle font = text.add_font(std::move(*font_file));
    render::SpriteBatcher sprites;
    render::SpriteLibrary library;
    struct Flush {
        script::DrawFlush what;
        std::size_t shapes;
        std::size_t sprites;
    };
    std::vector<Flush> log;
    std::size_t drawn = 0;
    script::DrawTargets t;
    t.shapes = &shapes;
    t.text = &text;
    t.font = font;
    t.sprites = &library;
    t.sprite_batcher = &sprites;
    t.flush = [&](script::DrawFlush what, const Affine2&, float) {
        log.push_back({what, shapes.size(), sprites.size()});
        if (what == script::DrawFlush::Sprites || what == script::DrawFlush::Layer) {
            drawn += sprites.size();
            sprites.clear();
        }
    };
    g.host->draw(t);
    REQUIRE(g.host->running());

    // Sprite2D (1) + NinePatchRect (9) + AnimatedSprite2D (1).
    CHECK(drawn == 11);
    // The Shape before the sprite went out first; the sprite went out before the shape after it.
    bool sprite_flush_before_second_shape = false;
    for (std::size_t i = 0; i + 1 < log.size(); ++i) {
        if (log[i].what == script::DrawFlush::Sprites && log[i].sprites == 1) sprite_flush_before_second_shape = true;
    }
    CHECK(sprite_flush_before_second_shape);
    CHECK(library.loaded() == 2); // red.svg and hero.atlas

    // Without sprite targets the nodes draw nothing and nothing breaks.
    log.clear();
    script::DrawTargets none = t;
    none.sprites = nullptr;
    none.sprite_batcher = nullptr;
    g.host->draw(none);
    CHECK(g.host->running());
}

TEST_CASE("sprite nodes: AnimatedSprite2D advances in whole ticks, deterministically, and stops") {
    const fs::path dir = project_dir("anim");
    Game a(dir), b(dir);
    // fps 30 at 60 ticks/s: two ticks per frame (the node starts at frame 0 in tick 1).
    a.run(1);
    CHECK(a.host->session()->global("T.frame").i == 0);
    a.run(5);
    const std::int64_t mid = a.host->session()->global("T.frame").i;
    CHECK(mid >= 1);
    CHECK(mid <= 2);
    a.run(20);
    CHECK(a.host->session()->global("T.frame").i == 2); // last frame, not looping
    CHECK(a.host->session()->global("T.done").i == 1);
    b.run(26);
    CHECK(a.host->state_hash() == b.host->state_hash());

    // Drawing is render only: it never changes the state hash.
    render::SdfBatcher shapes;
    render::TextRenderer text;
    render::SpriteBatcher sprites;
    render::SpriteLibrary library;
    script::DrawTargets t;
    t.shapes = &shapes;
    t.text = &text;
    t.sprites = &library;
    t.sprite_batcher = &sprites;
    const std::uint64_t before = a.host->state_hash();
    a.host->draw(t);
    CHECK(a.host->state_hash() == before);

    // A different number of ticks is a different state.
    Game c(dir);
    c.run(3);
    CHECK(c.host->state_hash() != a.host->state_hash());
}

TEST_CASE("sprite nodes: new fields round-trip through scene files") {
    const fs::path dir = project_dir("scene");
    std::string src = R"(
class T {
    static var saved = 0
}
class Main extends Node {
    fn ready() {
        var sp = Sprite2D()
        sp.name = "sprite"
        sp.asset = "red.svg"
        sp.frame = "idle"
        sp.filter = #nearest
        sp.flip_h = true
        sp.size = vec2(32.0, 24.0)
        var an = AnimatedSprite2D()
        an.name = "anim"
        an.asset = "hero.atlas"
        an.frames = AnimatedSprite2D.sequence("hero", 3)
        an.fps = 8.0
        an.looping = false
        var np = NinePatchRect()
        np.name = "panel"
        np.asset = "red.svg"
        np.size = vec2(120.0, 60.0)
        np.margin_left = 5.0
        np.draw_center = false
        np.filter = #linear
        var all = List<Node>()
        all.push(sp)
        all.push(an)
        all.push(np)
        for n in all {
            if Scene.save(n, "out/{n.name}.scene") {
                var back = Scene.load("out/{n.name}.scene")
                if Scene.save(back, "out/{n.name}_again.scene") {
                    T.saved += 1
                }
            }
        }
    }
}
)";
    InputMap map;
    script::HostOptions o;
    o.project_dir = dir;
    script::ScriptHost host(o, map);
    REQUIRE_MESSAGE(host.load({{"main.plm", src}}, "Main"), host.error());
    host.tick(InputFrame{});
    REQUIRE_MESSAGE(host.running(), host.error());
    CHECK(host.session()->global("T.saved").i == 3);

    struct Expect {
        const char* node;
        std::vector<std::string> lines;
    };
    const std::vector<Expect> expects = {
        {"sprite", {"node Sprite2D \"sprite\"", "frame: str = \"idle\"", "filter: name = #nearest", "flip_h: bool = true"}},
        {"anim", {"node AnimatedSprite2D \"anim\"", "fps: float = 8.0", "looping: bool = false", "\"hero_2\""}},
        {"panel", {"node NinePatchRect \"panel\"", "margin_left: float = 5.0", "draw_center: bool = false", "filter: name = #linear"}},
    };
    for (const Expect& e : expects) {
        CAPTURE(e.node);
        const fs::path first = dir / "out" / (std::string(e.node) + ".scene");
        const fs::path again = dir / "out" / (std::string(e.node) + "_again.scene");
        REQUIRE(fs::exists(first));
        auto read = [](const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        CHECK(read(first) == read(again));
        std::string err;
        auto doc = scene::load_file(first, err);
        REQUIRE_MESSAGE(doc.has_value(), err);
        const std::string text = scene::dump_text(*doc);
        for (const std::string& line : e.lines) {
            CAPTURE(line);
            CHECK(text.find(line) != std::string::npos);
        }
    }
}

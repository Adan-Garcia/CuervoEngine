// TileMapLayer tests (milestone 9.5, DESIGN.md 4.17): cells, coordinates,
// autotiling, drawing and scene serialization.

#include <doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

struct Host {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    explicit Host(const std::string& source) {
        script::HostOptions o;
        o.project_dir = fs::temp_directory_path() / "cuervo_tilemap_tests";
        fs::create_directories(o.project_dir);
        o.transform_hierarchy = true;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
        REQUIRE_MESSAGE(host->running(), host->error());
    }

    void run(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            host->tick(InputFrame{});
            REQUIRE_MESSAGE(host->running(), host->error());
        }
    }

    std::int64_t integer(std::string_view n) { return host->session()->global(n).i; }
    bool boolean(std::string_view n) { return host->session()->global(n).b; }
    vec2 v2(std::string_view n) {
        const pluma::Value v = host->session()->global(n);
        const float* d = pluma::vec_data(v);
        return {d[0], d[1]};
    }
};

} // namespace

TEST_CASE("tilemap: cell get, set, erase, clear") {
    Host h(R"(
var g_count = 0
var g_cell_1_2 = -1
var g_cell_neg = -1
var g_has_1_2 = false
var g_has_9_9 = false
var g_after_erase = -1

class Main extends Node {
    fn ready() {
        var map = TileMapLayer()
        add_child(map)

        map.set_cell(1, 2, 42)
        map.set_cell(-3, -5, 7)
        map.set_cell(10, 10, 99)

        g_count = map.cell_count()
        g_cell_1_2 = map.get_cell(1, 2)
        g_cell_neg = map.get_cell(-3, -5)
        g_has_1_2 = map.has_cell(1, 2)
        g_has_9_9 = map.has_cell(9, 9)

        map.erase_cell(1, 2)
        g_after_erase = map.get_cell(1, 2)
    }
}
)");
    h.run();
    CHECK(h.integer("g_count") == 3);
    CHECK(h.integer("g_cell_1_2") == 42);
    CHECK(h.integer("g_cell_neg") == 7);
    CHECK(h.boolean("g_has_1_2") == true);
    CHECK(h.boolean("g_has_9_9") == false);
    CHECK(h.integer("g_after_erase") == -1);
}

TEST_CASE("tilemap: coordinates and used rect") {
    Host h(R"(
var g_local_pos = vec2(0.0, 0.0)
var g_map_pos = ivec2.of(0, 0)
var g_rect_pos = vec2(0.0, 0.0)
var g_rect_sz = vec2(0.0, 0.0)

class Main extends Node {
    fn ready() {
        var map = TileMapLayer()
        map.tile_size = vec2(16.0, 16.0)
        add_child(map)

        // Center of cell (2, 3) should be (2.5 * 16, 3.5 * 16) = (40.0, 56.0)
        g_local_pos = map.map_to_local(ivec2.of(2, 3))
        g_map_pos = map.local_to_map(vec2(40.0, 56.0))

        map.set_cell(2, 2, 1)
        map.set_cell(5, 7, 2)

        var r = map.get_used_rect()
        g_rect_pos = r.pos
        g_rect_sz = r.size
    }
}
)");
    h.run();
    CHECK(h.v2("g_local_pos").x() == 40.0f);
    CHECK(h.v2("g_local_pos").y() == 56.0f);
    CHECK(h.v2("g_rect_pos").x() == 2.0f);
    CHECK(h.v2("g_rect_pos").y() == 2.0f);
    CHECK(h.v2("g_rect_sz").x() == 4.0f); // 5 - 2 + 1 = 4
    CHECK(h.v2("g_rect_sz").y() == 6.0f); // 7 - 2 + 1 = 6
}

TEST_CASE("tilemap: autotile mask and connections") {
    Host h(R"(
var g_tile_isolated = -1
var g_tile_horizontal = -1
var g_tile_cross = -1

class Main extends Node {
    fn ready() {
        var map = TileMapLayer()
        add_child(map)

        // Create tileset with terrain autotiles
        var ts = TileSet {}
        // Tile 0: isolated (peering_bits = 0)
        ts.tiles.push(TileDef { id = 10, terrain = 1, peering_bits = 0 })
        // Tile 1: East + West (peering_bits = 2 | 8 = 10)
        ts.tiles.push(TileDef { id = 11, terrain = 1, peering_bits = 10 })
        // Tile 2: 4 cardinal directions (1 | 2 | 4 | 8 = 15)
        ts.tiles.push(TileDef { id = 12, terrain = 1, peering_bits = 15 })

        map.tileset = ts

        // Place horizontal line: (0, 0), (1, 0), (2, 0)
        map.set_cell_autotile(0, 0, 1)
        map.set_cell_autotile(1, 0, 1)
        map.set_cell_autotile(2, 0, 1)

        // Middle tile (1, 0) has East and West neighbors
        g_tile_horizontal = map.get_cell(1, 0)

        // Add North and South to (1, 0)
        map.set_cell_autotile(1, -1, 1)
        map.set_cell_autotile(1, 1, 1)

        // Now (1, 0) has all 4 cardinals
        g_tile_cross = map.get_cell(1, 0)

        // Isolated tile at (10, 10)
        map.set_cell_autotile(10, 10, 1)
        g_tile_isolated = map.get_cell(10, 10)
    }
}
)");
    h.run();
    CHECK(h.integer("g_tile_isolated") == 10);
    CHECK(h.integer("g_tile_horizontal") == 11);
    CHECK(h.integer("g_tile_cross") == 12);
}

TEST_CASE("tilemap: scene serialization round-trip") {
    Host h(R"(
var g_roundtrip_ok = false

class Main extends Node {
    fn ready() {
        var map = TileMapLayer()
        map.tile_size = vec2(32.0, 32.0)
        add_child(map)
        map.set_cell(0, 0, 1)
        map.set_cell(1, 0, 2)
        map.set_cell(2, 3, 5)

        Scene.save(map, "test_tilemap.scene")
        var loaded = Scene.load("test_tilemap.scene")
        if loaded is TileMapLayer m {
            if m.get_cell(0, 0) == 1 && m.get_cell(1, 0) == 2 && m.get_cell(2, 3) == 5 {
                if m.cell_count() == 3 {
                    g_roundtrip_ok = true
                }
            }
        }
    }
}
)");
    h.run();
    CHECK(h.boolean("g_roundtrip_ok") == true);
}

TEST_CASE("tilemap: edge cases - duplicate cells, autotile erase, fallback matching, and zero tile_size") {
    Host h(R"(
var g_dedup_count = 0
var g_dedup_val = -1
var g_neighbor_after_autotile_erase = -1
var g_erased_gone = false
var g_zero_size_safe = false

class Main extends Node {
    fn ready() {
        var map = TileMapLayer()
        add_child(map)

        // 1. Duplicate cells in raw `cells` list should deduplicate cleanly
        map.cells.push(TileCell { x = 3, y = 4, tile_id = 1 })
        map.cells.push(TileCell { x = 3, y = 4, tile_id = 99 })
        map.cells.push(TileCell { x = 1, y = 1, tile_id = 5 })
        g_dedup_count = map.cell_count()
        g_dedup_val = map.get_cell(3, 4)

        // 2. set_cell_autotile with terrain < 0 erases and updates neighbors
        map.clear()
        var ts = TileSet {}
        ts.tiles.push(TileDef { id = 10, terrain = 1, peering_bits = 0 })
        ts.tiles.push(TileDef { id = 11, terrain = 1, peering_bits = 2 }) // East only
        ts.tiles.push(TileDef { id = 12, terrain = 1, peering_bits = 10 }) // East + West
        map.tileset = ts

        map.set_cell_autotile(0, 0, 1)
        map.set_cell_autotile(1, 0, 1)
        map.set_cell_autotile(2, 0, 1)
        // Erase (2, 0) via set_cell_autotile(2, 0, -1); (1, 0) should update from East+West to West-only (fallback to isolated 10)
        map.set_cell_autotile(2, 0, -1)
        g_erased_gone = !map.has_cell(2, 0)
        // (0, 0) still has East neighbor (1, 0), so (0, 0) should be 11
        g_neighbor_after_autotile_erase = map.get_cell(0, 0)

        // 3. Zero tile_size fallback safety
        map.tile_size = vec2(0.0, -16.0)
        var lp = map.map_to_local(ivec2.of(1, 1))
        var mp = map.local_to_map(lp)
        g_zero_size_safe = (mp.x == 1 && mp.y == 1 && lp.x > 0.0 && lp.y > 0.0)
    }
}
)");
    h.run();
    CHECK(h.integer("g_dedup_count") == 2);
    CHECK(h.integer("g_dedup_val") == 99);
    CHECK(h.boolean("g_erased_gone") == true);
    CHECK(h.integer("g_neighbor_after_autotile_erase") == 11);
    CHECK(h.boolean("g_zero_size_safe") == true);
}

// NavigationRegion2D & NavigationAgent2D tests (milestone 9.5, DESIGN.md 4.17):
// grid obstacles, A* pathfinding, deterministic tie-breaking, waypoint following.

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
        o.project_dir = fs::temp_directory_path() / "cuervo_nav_tests";
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
    float num(std::string_view n) { return host->session()->global(n).f; }
    vec2 v2(std::string_view n) {
        const pluma::Value v = host->session()->global(n);
        const float* d = pluma::vec_data(v);
        return {d[0], d[1]};
    }
};

} // namespace

TEST_CASE("navigation: grid walkable and baking from tilemap") {
    Host h(R"(
var g_walkable_0_0 = false
var g_walkable_1_1 = false
var g_walkable_after_bake = false
var g_obstacle_after_bake = true

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        add_child(nav)

        nav.set_cell_walkable(0, 0, true)
        nav.set_cell_walkable(1, 1, false)

        g_walkable_0_0 = nav.is_cell_walkable(0, 0)
        g_walkable_1_1 = nav.is_cell_walkable(1, 1)

        // Create a tilemap with ground and walls
        var map = TileMapLayer()
        map.tile_size = vec2(32.0, 32.0)
        var ts = TileSet {}
        ts.tiles.push(TileDef { id = 0, walkable = true })
        ts.tiles.push(TileDef { id = 1, walkable = false })
        map.tileset = ts

        map.set_cell(5, 5, 0) // ground
        map.set_cell(5, 6, 1) // wall

        nav.bake_from_tilemap(map)

        g_walkable_after_bake = nav.is_cell_walkable(5, 5)
        g_obstacle_after_bake = nav.is_cell_walkable(5, 6)
    }
}
)");
    h.run();
    CHECK(h.boolean("g_walkable_0_0") == true);
    CHECK(h.boolean("g_walkable_1_1") == false);
    CHECK(h.boolean("g_walkable_after_bake") == true);
    CHECK(h.boolean("g_obstacle_after_bake") == false);
}

TEST_CASE("navigation: deterministic A* path around obstacle") {
    Host h(R"(
var g_path_len = 0
var g_reached_end = false
var g_avoided_obstacle = false

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = false // cardinal for clear step count
        add_child(nav)

        // Wall blocking straight line from (0, 0) to (4, 0):
        // Wall at (2, 0), (2, -1), (2, 1)
        nav.set_cell_walkable(2, 0, false)
        nav.set_cell_walkable(2, -1, false)
        nav.set_cell_walkable(2, 1, false)

        // Path from (0, 0) world (16, 16) to (4, 0) world (144, 16)
        var p_start = nav.map_to_global(ivec2.of(0, 0))
        var p_target = nav.map_to_global(ivec2.of(4, 0))

        var path = nav.get_path(p_start, p_target)
        g_path_len = path.len()

        if path.len() > 0 {
            var end_pt = path[path.len() - 1]
            g_reached_end = (end_pt - p_target).length() < 1.0

            // Ensure no waypoint went through the obstacle (2, 0)
            var hit_obstacle = false
            for pt in path {
                var cell = nav.global_to_map(pt)
                if (cell.x == 2 && cell.y == 0) || (cell.x == 2 && cell.y == 1) || (cell.x == 2 && cell.y == -1) {
                    hit_obstacle = true
                }
            }
            g_avoided_obstacle = !hit_obstacle
        }
    }
}
)");
    h.run();
    CHECK(h.integer("g_path_len") > 0);
    CHECK(h.boolean("g_reached_end") == true);
    CHECK(h.boolean("g_avoided_obstacle") == true);
}

TEST_CASE("navigation: agent waypoint progression and signals") {
    Host h(R"(
var g_waypoints_hit = 0
var g_nav_done = false
var g_target_hit = false

class Main extends Node {
    var agent: NavigationAgent2D
    var nav: NavigationRegion2D

    fn ready() {
        nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        add_child(nav)

        agent = NavigationAgent2D()
        agent.path_desired_distance = 4.0
        agent.target_desired_distance = 4.0
        agent.max_speed = 100.0
        add_child(agent)

        agent.position = vec2(16.0, 16.0)

        // Connect signals
        agent.waypoint_reached.connect(fn(idx: int) {
            g_waypoints_hit = g_waypoints_hit + 1
        })
        agent.navigation_finished.connect(fn() {
            g_nav_done = true
        })
        agent.target_reached.connect(fn() {
            g_target_hit = true
        })

        // Set target 3 cells away: (3 * 32 + 16, 16) = (112, 16)
        agent.set_target_position(vec2(112.0, 16.0))
    }

    fn update(dt: float) {
        if !agent.is_navigation_finished() {
            var next_pos = agent.get_next_path_position()
            var diff = next_pos - agent.position
            var dist = diff.length()
            if dist <= 10.0 {
                agent.position = next_pos
            } else {
                agent.position = agent.position + (diff / dist) * 10.0
            }
        }
    }
}
)");
    // Run enough ticks for agent to travel 96 pixels at 10 px/tick (~12 ticks)
    h.run(15);
    CHECK(h.integer("g_waypoints_hit") > 0);
    CHECK(h.boolean("g_nav_done") == true);
    CHECK(h.boolean("g_target_hit") == true);
}

TEST_CASE("navigation: nested agent resolves region in level and avoids obstacle") {
    Host h(R"(
var g_path_waypoints = 0
var g_avoided_wall = false
var g_found_region = false

class Main extends Node {
    fn ready() {
        var level = Node2D()
        add_child(level)

        var nav = NavigationRegion2D()
        nav.name = "NavRegion"
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = false
        level.add_child(nav)

        // Wall at (2, 0)
        nav.set_cell_walkable(2, 0, false)
        nav.set_cell_walkable(2, 1, false)
        nav.set_cell_walkable(2, -1, false)

        var char = Node2D()
        char.name = "Character"
        char.position = nav.map_to_global(ivec2.of(0, 0))
        level.add_child(char)

        var agent = NavigationAgent2D()
        agent.name = "Agent"
        // region_path is empty: auto-resolves region from parent/level tree
        char.add_child(agent)

        // Target at (4, 0)
        var target_pos = nav.map_to_global(ivec2.of(4, 0))
        agent.set_target_position(target_pos)

        var path = agent.get_current_navigation_path()
        g_path_waypoints = path.len()
        g_found_region = path.len() > 1

        var hit_wall = false
        for pt in path {
            var c = nav.global_to_map(pt)
            if c.x == 2 && (c.y == 0 || c.y == 1 || c.y == -1) {
                hit_wall = true
            }
        }
        g_avoided_wall = !hit_wall && g_found_region
    }
}
)");
    h.run();
    CHECK(h.boolean("g_found_region") == true);
    CHECK(h.boolean("g_avoided_wall") == true);
}

TEST_CASE("navigation: continuous agent movement avoids obstacles tick-by-tick") {
    Host h(R"(
var g_hit_wall = false
var g_reached_target = false
var g_steps_taken = 0

class Main extends Node {
    var agent: NavigationAgent2D
    var nav: NavigationRegion2D
    var char: Node2D

    fn ready() {
        var level = Node2D()
        add_child(level)

        nav = NavigationRegion2D()
        nav.name = "NavRegion"
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = false
        level.add_child(nav)

        // Wall blocking straight line from (0, 0) to (4, 0):
        // Wall at (2, 0), (2, -1), (2, 1)
        nav.set_cell_walkable(2, 0, false)
        nav.set_cell_walkable(2, -1, false)
        nav.set_cell_walkable(2, 1, false)

        char = Node2D()
        char.name = "Character"
        char.position = nav.map_to_global(ivec2.of(0, 0))
        level.add_child(char)

        agent = NavigationAgent2D()
        agent.name = "Agent"
        agent.path_desired_distance = 4.0
        agent.target_desired_distance = 4.0
        char.add_child(agent)

        agent.target_reached.connect(fn() {
            g_reached_target = true
        })

        // Target at (4, 0)
        var target_pos = nav.map_to_global(ivec2.of(4, 0))
        agent.set_target_position(target_pos)
    }

    fn update(dt: float) {
        if !agent.is_navigation_finished() {
            var next_pos = agent.get_next_path_position()
            var diff = next_pos - char.position
            var dist = diff.length()
            if dist <= 8.0 {
                char.position = next_pos
            } else {
                char.position = char.position + (diff / dist) * 8.0
            }
            g_steps_taken = g_steps_taken + 1

            // Continuous obstacle check at every single simulation step
            var c = nav.global_to_map(char.position)
            if !nav.is_cell_walkable(c.x, c.y) {
                g_hit_wall = true
            }
        }
    }
}
)");
    // Run enough ticks for character to navigate around the wall to target (8 steps * 32px / 8px/tick = ~32 ticks)
    h.run(45);
    CHECK(h.integer("g_steps_taken") > 0);
    CHECK(h.boolean("g_reached_target") == true);
    CHECK(h.boolean("g_hit_wall") == false);
}

TEST_CASE("navigation: strict deterministic tie-breaking") {
    Host h(R"(
var g_same_paths = false

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = true
        add_child(nav)

        // Symmetrical open grid from (0, 0) to (5, 5)
        var p0 = nav.map_to_global(ivec2.of(0, 0))
        var p1 = nav.map_to_global(ivec2.of(5, 5))

        var path1 = nav.get_path(p0, p1)
        var path2 = nav.get_path(p0, p1)

        if path1.len() == path2.len() && path1.len() > 0 {
            var all_eq = true
            for i in 0..path1.len() {
                if (path1[i] - path2[i]).length() > 0.0001 {
                    all_eq = false
                }
            }
            g_same_paths = all_eq
        }
    }
}
)");
    h.run();
    CHECK(h.boolean("g_same_paths") == true);
}

TEST_CASE("navigation: search methods (AStar, Dijkstra, Greedy) and heuristics") {
    Host h(R"(
var g_astar_valid = false
var g_dijkstra_valid = false
var g_greedy_valid = false
var g_euclidean_valid = false
var g_manhattan_valid = false
var g_chebyshev_valid = false

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = true
        add_child(nav)

        // Wall at (2, 0)
        nav.set_cell_walkable(2, 0, false)
        nav.set_cell_walkable(2, 1, false)

        var p_start = nav.map_to_global(ivec2.of(0, 0))
        var p_target = nav.map_to_global(ivec2.of(4, 0))

        // 1. Standard A*
        var path_astar = nav.get_path_with(p_start, p_target, PathMethod.AStar, PathHeuristic.Octile, false)
        g_astar_valid = path_astar.len() > 0

        // 2. Dijkstra
        var path_dijkstra = nav.get_path_with(p_start, p_target, PathMethod.Dijkstra, PathHeuristic.Octile, false)
        g_dijkstra_valid = path_dijkstra.len() > 0

        // 3. Greedy best-first
        var path_greedy = nav.get_path_with(p_start, p_target, PathMethod.Greedy, PathHeuristic.Octile, false)
        g_greedy_valid = path_greedy.len() > 0

        // 4. Euclidean heuristic
        var path_euc = nav.get_path_with(p_start, p_target, PathMethod.AStar, PathHeuristic.Euclidean, false)
        g_euclidean_valid = path_euc.len() > 0

        // 5. Manhattan heuristic
        var path_man = nav.get_path_with(p_start, p_target, PathMethod.AStar, PathHeuristic.Manhattan, false)
        g_manhattan_valid = path_man.len() > 0

        // 6. Chebyshev heuristic
        var path_cheb = nav.get_path_with(p_start, p_target, PathMethod.AStar, PathHeuristic.Chebyshev, false)
        g_chebyshev_valid = path_cheb.len() > 0
    }
}
)");
    h.run();
    CHECK(h.boolean("g_astar_valid") == true);
    CHECK(h.boolean("g_dijkstra_valid") == true);
    CHECK(h.boolean("g_greedy_valid") == true);
    CHECK(h.boolean("g_euclidean_valid") == true);
    CHECK(h.boolean("g_manhattan_valid") == true);
    CHECK(h.boolean("g_chebyshev_valid") == true);
}

TEST_CASE("navigation: line of sight, path smoothing, and simplification") {
    Host h(R"(
var g_clear_sight = false
var g_blocked_sight = true
var g_smoothed_shorter = false
var g_simplified_shorter = false

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = true
        add_child(nav)

        // Wall pillar at (2, 2)
        nav.set_cell_walkable(2, 2, false)

        var p0 = nav.map_to_global(ivec2.of(0, 0))
        var p_open = nav.map_to_global(ivec2.of(4, 0))
        var p_blocked = nav.map_to_global(ivec2.of(4, 4))

        // Straight sightline across open cells
        g_clear_sight = nav.is_straight_walkable(p0, p_open)
        // Blocked sightline across (2, 2)
        g_blocked_sight = nav.is_straight_walkable(p0, p_blocked)

        // Raw path from (0, 0) to (5, 0)
        var p_end = nav.map_to_global(ivec2.of(5, 0))
        var raw_path = nav.get_path_with(p0, p_end, PathMethod.AStar, PathHeuristic.Octile, false)
        var smooth_p = nav.smooth_path(raw_path)
        var simple_p = nav.simplify_path(raw_path)

        // Smoothing should prune intermediate cardinal steps down to direct line
        g_smoothed_shorter = smooth_p.len() < raw_path.len()
        g_simplified_shorter = simple_p.len() < raw_path.len()
    }
}
)");
    h.run();
    CHECK(h.boolean("g_clear_sight") == true);
    CHECK(h.boolean("g_blocked_sight") == false);
    CHECK(h.boolean("g_smoothed_shorter") == true);
    CHECK(h.boolean("g_simplified_shorter") == true);
}

TEST_CASE("navigation: reachable area and multi-target pathfinding") {
    Host h(R"(
var g_reachable_count = 0
var g_nearest_found = false
var g_nearest_is_closest = false

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = false
        add_child(nav)

        var p_start = nav.map_to_global(ivec2.of(0, 0))

        // Reachable cells within 2 cardinal steps (cost <= 2.0):
        // (0,0), (1,0), (-1,0), (0,1), (0,-1), (2,0), (-2,0), (0,2), (0,-2), (1,1), (1,-1), (-1,1), (-1,-1) = 13 cells
        var cells = nav.get_reachable_cells(p_start, 2.0)
        g_reachable_count = cells.len()

        // Nearest target among [far, close, blocked]
        var target_close = nav.map_to_global(ivec2.of(1, 0))
        var target_far = nav.map_to_global(ivec2.of(5, 0))
        var targets = List<vec2>()
        targets.push(target_far)
        targets.push(target_close)

        var nearest_path = nav.find_path_to_nearest(p_start, targets)
        g_nearest_found = !nearest_path.is_empty()
        if nearest_path.len() > 0 {
            var end_pt = nearest_path[nearest_path.len() - 1]
            g_nearest_is_closest = (end_pt - target_close).length() < 1.0
        }
    }
}
)");
    h.run();
    CHECK(h.integer("g_reachable_count") == 13);
    CHECK(h.boolean("g_nearest_found") == true);
    CHECK(h.boolean("g_nearest_is_closest") == true);
}

TEST_CASE("navigation: obstacle inflation and rectangular manipulation") {
    Host h(R"(
var g_rect_walkable = true
var g_cost_applied = false
var g_inflated_blocked = false

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        add_child(nav)

        // 1. Rectangular walkability
        nav.set_walkable_rect(ivec2.of(1, 1), ivec2.of(3, 3), false)
        g_rect_walkable = nav.is_cell_walkable(2, 2) // should be false

        // 2. Rectangular cost
        nav.set_cost_rect(ivec2.of(4, 4), ivec2.of(5, 5), 3.5)
        g_cost_applied = nav.get_cell_cost(4, 5) == 3.5

        // 3. Obstacle inflation
        nav.clear_obstacles()
        nav.set_cell_walkable(0, 0, false)
        nav.inflate_obstacles(1)
        // (1, 0) should now be blocked by inflation radius 1
        g_inflated_blocked = !nav.is_cell_walkable(1, 0) && !nav.is_cell_walkable(1, 1)
    }
}
)");
    h.run();
    CHECK(h.boolean("g_rect_walkable") == false);
    CHECK(h.boolean("g_cost_applied") == true);
    CHECK(h.boolean("g_inflated_blocked") == true);
}

TEST_CASE("navigation: dijkstra map flow field and mass pathfinding") {
    Host h(R"(
var g_target_cost = -1.0
var g_neighbor_cost = -1.0
var g_downhill_dir_x = 0.0
var g_downhill_dir_y = 0.0
var g_has_cell = false
var g_avoided_obstacle = false
var g_mass_paths_valid = 0

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = false
        add_child(nav)

        // Wall at (2, -1), (2, 0), (2, 1)
        nav.set_cell_walkable(2, -1, false)
        nav.set_cell_walkable(2, 0, false)
        nav.set_cell_walkable(2, 1, false)

        var target_pos = nav.map_to_global(ivec2.of(4, 0))
        var dmap = nav.compute_dijkstra_map_to(target_pos)

        // Target cell cost should be 0.0
        g_target_cost = dmap.get_cost_at(target_pos)
        // (3, 0) is adjacent to target, cost should be 1.0
        var p_3_0 = nav.map_to_global(ivec2.of(3, 0))
        g_neighbor_cost = dmap.get_cost_at(p_3_0)

        // Direction at (3, 0) should point right toward (4, 0) downhill -> (1.0, 0.0)
        var dir_3_0 = dmap.get_direction_at(p_3_0)
        g_downhill_dir_x = dir_3_0.x
        g_downhill_dir_y = dir_3_0.y

        g_has_cell = dmap.has_position(target_pos) && !dmap.has_cell(ivec2.of(2, 0))

        // Path from (0, 0) behind the wall to target (4, 0)
        var p_start = nav.map_to_global(ivec2.of(0, 0))
        var path = dmap.get_path_from(p_start)

        if path.len() > 0 {
            var hit_obstacle = false
            for pt in path {
                var c = nav.global_to_map(pt)
                if c.x == 2 && (c.y >= -1 && c.y <= 1) {
                    hit_obstacle = true
                }
            }
            g_avoided_obstacle = !hit_obstacle
        }

        // Mass path query from 4 different spawn points simultaneously using same dmap
        var spawns = List<vec2>()
        spawns.push(nav.map_to_global(ivec2.of(0, -2)))
        spawns.push(nav.map_to_global(ivec2.of(0, 2)))
        spawns.push(nav.map_to_global(ivec2.of(1, -2)))
        spawns.push(nav.map_to_global(ivec2.of(1, 2)))

        var valid_count = 0
        for sp in spawns {
            var mass_path = dmap.get_path_from(sp)
            if mass_path.len() > 0 {
                var last_pt = mass_path[mass_path.len() - 1]
                if (last_pt - target_pos).length() < 1.0 {
                    valid_count += 1
                }
            }
        }
        g_mass_paths_valid = valid_count
    }
}
)");
    h.run();
    CHECK(h.num("g_target_cost") == 0.0f);
    CHECK(h.num("g_neighbor_cost") == 1.0f);
    CHECK(h.num("g_downhill_dir_x") == doctest::Approx(1.0f));
    CHECK(h.num("g_downhill_dir_y") == doctest::Approx(0.0f));
    CHECK(h.boolean("g_has_cell") == true);
    CHECK(h.boolean("g_avoided_obstacle") == true);
    CHECK(h.integer("g_mass_paths_valid") == 4);
}

TEST_CASE("navigation: dijkstra map multi-target and fleeing") {
    Host h(R"(
var g_multi_targets_count = 0
var g_flows_to_left = false
var g_flows_to_right = false
var g_flee_points_away = false

class Main extends Node {
    fn ready() {
        var nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = false
        add_child(nav)

        // Two targets: Left (-5, 0) and Right (5, 0)
        var target_left = nav.map_to_global(ivec2.of(-5, 0))
        var target_right = nav.map_to_global(ivec2.of(5, 0))
        var targets = List<vec2>()
        targets.push(target_left)
        targets.push(target_right)

        var dmap = nav.compute_dijkstra_map(targets)
        g_multi_targets_count = dmap.get_targets().len()

        // Cell (-3, 0) is closer to left target -> flow direction should be (-1.0, 0.0)
        var p_left_query = nav.map_to_global(ivec2.of(-3, 0))
        var dir_left = dmap.get_direction_at(p_left_query)
        g_flows_to_left = dir_left.x < -0.9

        // Cell (3, 0) is closer to right target -> flow direction should be (1.0, 0.0)
        var p_right_query = nav.map_to_global(ivec2.of(3, 0))
        var dir_right = dmap.get_direction_at(p_right_query)
        g_flows_to_right = dir_right.x > 0.9

        // Fleeing: at (-4, 0), target is to the left (-5, 0).
        // Flee direction should move to a neighbor with strictly higher cost than current cell (-4, 0)!
        var p_flee_query = nav.map_to_global(ivec2.of(-4, 0))
        var flee_dir = dmap.get_flee_direction_at(p_flee_query)
        var cur_cost = dmap.get_cost_at(p_flee_query)
        var next_pos = p_flee_query + flee_dir * 32.0
        var next_cost = dmap.get_cost_at(next_pos)
        g_flee_points_away = flee_dir.length() > 0.9 && next_cost > cur_cost
    }
}
)");
    h.run();
    CHECK(h.integer("g_multi_targets_count") == 2);
    CHECK(h.boolean("g_flows_to_left") == true);
    CHECK(h.boolean("g_flows_to_right") == true);
    CHECK(h.boolean("g_flee_points_away") == true);
}

TEST_CASE("navigation: agent dijkstra map integration") {
    Host h(R"(
var g_agent_dir_valid = false
var g_agent_nav_finished = false
var g_agent_flee_valid = false

class Main extends Node {
    var agent: NavigationAgent2D
    var nav: NavigationRegion2D
    var dmap: DijkstraMap

    fn ready() {
        nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = false
        add_child(nav)

        agent = NavigationAgent2D()
        agent.position = nav.map_to_global(ivec2.of(0, 0))
        agent.path_desired_distance = 4.0
        agent.target_desired_distance = 4.0
        add_child(agent)

        var target_pos = nav.map_to_global(ivec2.of(2, 0))
        dmap = nav.compute_dijkstra_map_to(target_pos)

        // Agent queries direction from dmap
        var dir = agent.get_dijkstra_direction(dmap)
        g_agent_dir_valid = dir.x > 0.9

        var flee = agent.get_dijkstra_flee_direction(dmap)
        var cur_c = dmap.get_cost_at(agent.position)
        var next_c = dmap.get_cost_at(agent.position + flee * 32.0)
        g_agent_flee_valid = flee.length() > 0.9 && next_c > cur_c

        // Agent follows dmap path
        agent.set_path_from_dijkstra_map(dmap)
    }

    fn update(dt: float) {
        if !agent.is_navigation_finished() {
            var next_wp = agent.get_next_path_position()
            var to_wp = next_wp - agent.position
            if to_wp.length() > 0.001 {
                agent.position += to_wp.normalized() * 300.0 * dt
            }
            agent.update(dt)
        } else {
            g_agent_nav_finished = true
        }
    }
}
)");
    h.run(30);
    CHECK(h.boolean("g_agent_dir_valid") == true);
    CHECK(h.boolean("g_agent_flee_valid") == true);
    CHECK(h.boolean("g_agent_nav_finished") == true);
}

TEST_CASE("navigation: edge cases - cardinal nearest walkable, supercover DDA, 1-step nearest target, and desired distances") {
    Host h(R"(
var g_nearest_is_cardinal = false
var g_diagonal_pinch_blocked = true
var g_1step_nearest_ok = false
var g_agent_reached_tight_target = false
var g_agent_clamped_obstacle_target = false

class Main extends Node {
    var agent: NavigationAgent2D
    var nav: NavigationRegion2D

    fn ready() {
        nav = NavigationRegion2D()
        nav.cell_size = vec2(32.0, 32.0)
        nav.allow_diagonal = true
        add_child(nav)

        // 1. find_nearest_walkable should prefer cardinal neighbor (dist^2 = 1) over diagonal corner (-1, -1) (dist^2 = 2)
        nav.set_cell_walkable(0, 0, false)
        if nav.find_nearest_walkable(0, 0)? nw {
            var d2 = nw.x * nw.x + nw.y * nw.y
            g_nearest_is_cardinal = (d2 == 1)
        }
        nav.clear_obstacles()

        // 2. Supercover DDA blocks diagonal pinch between (1, 0) and (0, 1)
        nav.set_cell_walkable(1, 0, false)
        nav.set_cell_walkable(0, 1, false)
        g_diagonal_pinch_blocked = nav.is_straight_walkable(
            nav.map_to_global(ivec2.of(0, 0)),
            nav.map_to_global(ivec2.of(1, 1))
        )
        nav.clear_obstacles()

        // 3. find_path_to_nearest with 1-step paths (where p.len() == 1)
        var start_pt = nav.map_to_global(ivec2.of(0, 0))
        var one_cell_away = nav.map_to_global(ivec2.of(1, 0)) // 32px away
        var same_cell_close = start_pt + vec2(6.0, 0.0)       // 6px away
        var cands = List<vec2>()
        cands.push(one_cell_away)
        cands.push(same_cell_close)
        var best_p = nav.find_path_to_nearest(start_pt, cands)
        if !best_p.is_empty() {
            g_1step_nearest_ok = (best_p[best_p.len() - 1] - same_cell_close).length() < 0.5
        }

        // 4. NavigationAgent2D with path_desired_distance (20.0) > target_desired_distance (3.0)
        // and target inside an obstacle cell (3, 0)
        nav.set_cell_walkable(3, 0, false)
        agent = NavigationAgent2D()
        agent.position = nav.map_to_global(ivec2.of(0, 0))
        agent.path_desired_distance = 20.0
        agent.target_desired_distance = 3.0
        add_child(agent)

        agent.target_reached.connect(fn() {
            g_agent_reached_tight_target = true
        })

        // Set target inside blocked cell (3, 0); should clamp to nearest walkable (2, 0)
        agent.set_target_position(nav.map_to_global(ivec2.of(3, 0)))
        var clamped_cell = nav.global_to_map(agent.target_position)
        g_agent_clamped_obstacle_target = nav.is_cell_walkable(clamped_cell.x, clamped_cell.y)
    }

    fn update(dt: float) {
        if !agent.is_navigation_finished() {
            var next_wp = agent.get_next_path_position()
            var diff = next_wp - agent.position
            var dist = diff.length()
            if dist <= 6.0 {
                agent.position = next_wp
            } else {
                agent.position += (diff / dist) * 6.0
            }
            agent.update(dt)
        }
    }
}
)");
    h.run(25);
    CHECK(h.boolean("g_nearest_is_cardinal") == true);
    CHECK(h.boolean("g_diagonal_pinch_blocked") == false);
    CHECK(h.boolean("g_1step_nearest_ok") == true);
    CHECK(h.boolean("g_agent_clamped_obstacle_target") == true);
    CHECK(h.boolean("g_agent_reached_tight_target") == true);
}

// Simulation snapshots (DESIGN.md 4.8, milestone 11.7a): after ScriptHost::restore the game continues
// bit for bit like the uninterrupted run, whatever the job count or SIMD setting, and
// snapshot_hash() covers state that state_hash() does not.

#include <doctest.h>

#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

#include "cuervo/jobs/job_system.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/physics/physics_world.hpp"
#include "cuervo/script/game_session.hpp"
#include "pluma/vm.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION

namespace {

const std::string font = std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf";

struct Example {
    const char* name;
    const char* press;
    std::uint64_t save_at;
    std::uint64_t end_at;
};

// The inputs of tools/determinism_matrix.py, saved part-way through.
const Example examples[] = {
    {"shmup", "30:ui_accept,90:shoot,91:shoot,92:shoot,93:shoot,150:bomb", 120, 240},
    {"tictak", "10:ui_down,20:ui_accept,40:ui_right,50:ui_accept,70:ui_down,80:ui_accept,100:pause,110:ui_accept", 105,
     240},
    {"garden", "20:more,40:more,60:more", 90, 240},
#if CUERVO_FEATURE_PHYSICS
    {"lantern_cave", "10-114:move_right,60:jump,124:jump,138-240:move_right", 130, 240},
#endif
};

struct SimdSwitch {
    bool was = simd::enabled();
    ~SimdSwitch() { simd::set_enabled(was); }
};

std::unique_ptr<script::GameSession> open(const Example& e, unsigned jobs) {
    auto s = std::make_unique<script::GameSession>();
    script::SessionOptions o;
    o.dir = fs::path(CUERVO_EXAMPLES_DIR) / e.name;
    o.font_path = font;
    o.jobs = jobs;
    std::string error;
    REQUIRE_MESSAGE(s->init(o, error), error);
    s->set_presses(e.press);
    return s;
}

void run_to(script::GameSession& s, std::uint64_t tick) {
    DeviceState devices;
    while (s.host().tick_count() < tick) s.tick(s.sample(devices, {0.0f, 0.0f}));
}

struct Marks {
    std::uint64_t state = 0, snap = 0;
    bool operator==(const Marks&) const = default;
    friend std::ostream& operator<<(std::ostream& os, const Marks& m) {
        return os << std::hex << "state " << m.state << " snapshot " << m.snap;
    }
};

Marks marks(const script::ScriptHost& h) { return {h.state_hash(), h.snapshot_hash()}; }

} // namespace

TEST_CASE("snapshot: restore continues bit for bit (jobs 1/4, SIMD on/off)") {
    SimdSwitch restore_simd;
    for (const Example& e : examples) {
        for (unsigned jobs : {1u, 4u}) {
            for (bool simd_on : {true, false}) {
                simd::set_enabled(simd_on);
                CAPTURE(e.name);
                CAPTURE(jobs);
                CAPTURE(simd_on);
                auto a = open(e, jobs);
                run_to(*a, e.save_at);
                script::SimSnapshot snap;
                std::string error;
                REQUIRE_MESSAGE(a->host().save(snap, error), error);
                const Marks at_save = marks(a->host());
                StateHasher hp0, hw0;
                a->host().particles().hash_deep(hp0);
#if CUERVO_FEATURE_PHYSICS
                a->host().physics_world().hash_deep(hw0);
#endif
                REQUIRE(at_save.snap != 0);
                run_to(*a, e.end_at);
                const Marks uninterrupted = marks(a->host());
                CHECK(uninterrupted.snap != at_save.snap);

                // Back into the same game.
                REQUIRE_MESSAGE(a->host().restore(snap, error), error);
                CHECK(a->host().tick_count() == e.save_at);
                {
                    StateHasher hp1, hw1;
                    a->host().particles().hash_deep(hp1);
#if CUERVO_FEATURE_PHYSICS
                    a->host().physics_world().hash_deep(hw1);
#endif
                    CHECK(hp0.digest() == hp1.digest());
                    CHECK(hw0.digest() == hw1.digest());
                }
                {
                    script::SimSnapshot again;
                    REQUIRE_MESSAGE(a->host().save(again, error), error);
                    std::size_t at = 0;
                    while (at < again.image.size() && at < snap.image.size() && again.image[at] == snap.image[at]) ++at;
                    INFO("sizes " << snap.image.size() << " vs " << again.image.size() << ", first difference at " << at);
                    CHECK(again.image == snap.image);
                }
                CHECK(marks(a->host()) == at_save);
                run_to(*a, e.end_at);
                CHECK(marks(a->host()) == uninterrupted);

                // And into a fresh game running the same scripts (a different run to a different point first).
                auto b = open(e, jobs);
                run_to(*b, e.save_at / 2);
                REQUIRE_MESSAGE(b->host().restore(snap, error), error);
                CHECK(marks(b->host()) == at_save);
                run_to(*b, e.end_at);
                CHECK(marks(b->host()) == uninterrupted);
            }
        }
    }
}

TEST_CASE("snapshot: the snapshot hash does not depend on jobs or SIMD") {
    SimdSwitch restore_simd;
    const Example& e = examples[0];
    std::vector<Marks> all;
    for (unsigned jobs : {1u, 4u}) {
        for (bool simd_on : {true, false}) {
            simd::set_enabled(simd_on);
            auto a = open(e, jobs);
            run_to(*a, e.end_at);
            all.push_back(marks(a->host()));
        }
    }
    for (const Marks& m : all) CHECK(m == all[0]);
}

TEST_CASE("snapshot: restoring many times does not leak or drift") {
    const Example& e = examples[0];
    auto a = open(e, 1);
    run_to(*a, e.save_at);
    script::SimSnapshot snap;
    std::string error;
    REQUIRE_MESSAGE(a->host().save(snap, error), error);
    const Marks at_save = marks(a->host());
    for (int i = 0; i < 4; ++i) {
        run_to(*a, e.save_at + 30);
        REQUIRE_MESSAGE(a->host().restore(snap, error), error);
        CHECK(marks(a->host()) == at_save);
    }
}


namespace {

// Coroutines, weak references into garbage cycles, strings, collections, the shared RNG and frame
// arena objects: everything that lives in the script heap rather than in the particle pools.
const char* heap_game = R"(
class Cell {
    var n = 0
    var back: Cell
    weak var peer: Cell
}
var log = ""
var cells = List<Cell>()
var tally = Map<str, int>()
var latest: Cell

class Main extends Node {
    fn ready() {
        start(churn(1))
        start(churn(3))
    }
    fn churn(every: int) {
        var i = 0
        while true {
            var c = Cell()
            c.n = Random.range_int(0, 1000)
            c.back = c
            if cells.len() > 0 {
                c.peer = cells[cells.len() - 1]
            }
            cells.push(c)
            if cells.len() > 5 {
                cells = cells.slice(1)
            }
            var tmp = new(frame) Cell()
            tmp.n = c.n
            latest = c
            log = "{log}{c.n % 10}"
            if log.len() > 40 {
                log = "{c.n % 10}"
            }
            tally["k{i % 4}"] = tally.has("k{i % 4}") ? tally["k{i % 4}"] + c.n : c.n
            i += 1
            await wait_ticks(every)
        }
    }
}
)";

std::unique_ptr<script::ScriptHost> load_heap_game(InputMap& map, std::uint64_t seed) {
    script::HostOptions o;
    o.project_dir = fs::temp_directory_path() / "cuervo_snapshot_tests";
    fs::create_directories(o.project_dir);
    o.seed = seed;
    auto host = std::make_unique<script::ScriptHost>(o, map);
    REQUIRE_MESSAGE(host->load({{"main.plm", heap_game}}, "Main"), host->error());
    return host;
}

} // namespace

TEST_CASE("snapshot: coroutines, weak references and collections restore exactly (long runs, other seeds)") {
    for (std::uint64_t seed : {1ull, 7ull, 123456789ull}) {
        InputMap map;
        auto a = load_heap_game(map, seed);
        for (int i = 0; i < 700; ++i) a->tick(InputFrame{});
        script::SimSnapshot snap;
        std::string error;
        REQUIRE_MESSAGE(a->save(snap, error), error);
        const std::uint64_t at_save = a->snapshot_hash();
        for (int i = 0; i < 2500; ++i) a->tick(InputFrame{});
        const std::uint64_t end_hash = a->snapshot_hash();
        const std::uint64_t end_state = a->state_hash();
        REQUIRE(a->running());

        REQUIRE_MESSAGE(a->restore(snap, error), error);
        CHECK(a->snapshot_hash() == at_save);
        for (int i = 0; i < 2500; ++i) a->tick(InputFrame{});
        CHECK(a->snapshot_hash() == end_hash);
        CHECK(a->state_hash() == end_state);

        auto b = load_heap_game(map, seed + 1); // a different run, restored over
        for (int i = 0; i < 90; ++i) b->tick(InputFrame{});
        REQUIRE_MESSAGE(b->restore(snap, error), error);
        for (int i = 0; i < 2500; ++i) b->tick(InputFrame{});
        CHECK(b->snapshot_hash() == end_hash);
    }
}

TEST_CASE("snapshot: snapshot_hash sees state that state_hash does not") {
    InputMap map;
    auto a = load_heap_game(map, 5);
    auto b = load_heap_game(map, 5);
    for (int i = 0; i < 50; ++i) {
        a->tick(InputFrame{});
        b->tick(InputFrame{});
    }
    CHECK(a->state_hash() == b->state_hash());
    CHECK(a->snapshot_hash() == b->snapshot_hash());
    // A string global is invisible to state_hash (only numbers are folded in).
    pluma::VM& vm = a->session()->vm();
    const auto idx = vm.global_index("log");
    REQUIRE(idx);
    pluma::assign(vm.globals[*idx], pluma::str_value("tampered"));
    CHECK(a->state_hash() == b->state_hash());
    CHECK(a->snapshot_hash() != b->snapshot_hash());
}

TEST_CASE("snapshot: a snapshot of other scripts is refused") {
    InputMap map;
    auto a = load_heap_game(map, 5);
    for (int i = 0; i < 20; ++i) a->tick(InputFrame{});
    script::SimSnapshot snap;
    std::string error;
    REQUIRE_MESSAGE(a->save(snap, error), error);

    script::HostOptions o;
    o.project_dir = fs::temp_directory_path() / "cuervo_snapshot_tests";
    script::ScriptHost other(o, map);
    REQUIRE(other.load({{"main.plm", "var x = 1\nclass Main extends Node { }\n"}}, "Main"));
    for (int i = 0; i < 3; ++i) other.tick(InputFrame{});
    const std::uint64_t before = other.snapshot_hash();
    CHECK_FALSE(other.restore(snap, error));
    CHECK(error.find("different code") != std::string::npos);
    CHECK(other.running());
    CHECK(other.snapshot_hash() == before);
}

#endif

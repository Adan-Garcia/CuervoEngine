// The state hash doesn't depend on which unused features a player was built
// with (script_host.cpp, ScriptHost::state_hash): a stripped player (and the
// web build) hashes what the missing features hold in a game that doesn't
// use them. These tests run in the full build and check those stand-ins
// against the real thing.

#include <doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<script::ScriptHost> load(InputMap& map, const std::string& source) {
    script::HostOptions o;
    o.project_dir = fs::temp_directory_path() / "cuervo_state_hash_tests";
    fs::create_directories(o.project_dir);
    auto host = std::make_unique<script::ScriptHost>(o, map);
    REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
    return host;
}

// A game that uses neither the UI nor particles, pausing now and then.
const char* plain_game = R"(
var score = 0
var at = vec2(0.0, 0.0)
class Main extends Node {
    fn update(dt: float) {
        score += Random.range_int(0, 3)
        at = at + vec2(1.0, 0.5)
        if Time.tick % 7 == 0 {
            Scene.paused = !Scene.paused
        }
    }
}
)";

} // namespace

#if CUERVO_FEATURE_UI
TEST_CASE("state hash: the UI statics of a game without UI are the stripped player's stand-ins") {
    InputMap map;
    auto host = load(map, plain_game);
    for (int i = 0; i < 60; ++i) host->tick(InputFrame{});
    const pluma::VM& vm = host->session()->vm();
    std::size_t anchor = vm.global_names.size();
    for (std::size_t i = 0; i < vm.global_names.size(); ++i) {
        if (vm.global_names[i] == script::unused_ui_anchor) anchor = i;
    }
    REQUIRE(anchor < vm.global_names.size());
    // The UI prelude's globals come right after the anchor, and nothing else
    // of a prelude sits between them and the game's globals.
    StateHasher real;
    std::size_t i = anchor + 1;
    REQUIRE(i < vm.global_names.size());
    CHECK(vm.global_names[i].starts_with("UI."));
    for (; i < vm.global_names.size() && vm.global_names[i].starts_with("UI."); ++i) {
        script::hash_global(real, vm.globals[i]);
    }
    CHECK(vm.global_names[i] == "score");
    StateHasher stand_in;
    for (const pluma::Value& g : script::unused_ui_globals()) script::hash_global(stand_in, g);
    CHECK(real.digest() == stand_in.digest());
}
#endif

#if CUERVO_FEATURE_PARTICLES
TEST_CASE("state hash: an unused particle system hashes its RNG and its unpaused ticks") {
    InputMap map;
    auto host = load(map, plain_game);
    std::uint64_t unpaused = 0;
    for (int i = 0; i < 60; ++i) {
        const std::uint64_t before = host->particles().ticks();
        host->tick(InputFrame{});
        unpaused += host->particles().ticks() - before;
        // What a player without particles computes (ScriptHost::state_hash).
        StateHasher p;
        Pcg32 probe = host->rng();
        p.value(probe.next_u32());
        p.value(probe.next_u32());
        p.value(unpaused);
        CHECK(p.digest() == host->particles().state_hash());
    }
    CHECK(unpaused < 60); // the pauses were skipped, as the stand-in's counter skips them
}
#endif

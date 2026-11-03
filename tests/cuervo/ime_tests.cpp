#include <doctest.h>

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>

#include "cuervo/input/input_map.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;

namespace {
SDL_Event editing(const char* text, int start) {
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_EDITING;
    e.edit.text = text;
    e.edit.start = start;
    e.edit.length = 0;
    return e;
}
SDL_Event committed(const char* text) {
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text;
    return e;
}
} // namespace

TEST_CASE("IME: composition is UI-side, only committed text reaches the input frame") {
    Window window; // no OS window: handle_event only needs the device state for text events
    DeviceState devices;
    InputMap map;

    // Japanese: "ni" typed as composition (U+306B), the caret after one character.
    window.handle_event(editing("\xE3\x81\xAB", 1), devices);
    CHECK(devices.composition == "\xE3\x81\xAB");
    CHECK(devices.composition_cursor == 1);
    InputFrame f = map.sample(devices);
    CHECK(f.text == 0); // nothing committed yet, nothing in the replayed frame

    // Conversion replaces the composition, still uncommitted.
    window.handle_event(editing("\xE6\x97\xA5\xE6\x9C\xAC", 2), devices); // nihon -> 日本
    CHECK(devices.composition == "\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(map.sample(devices).text == 0);

    // Commit: two code points are delivered one per tick, the composition ends.
    window.handle_event(committed("\xE6\x97\xA5\xE6\x9C\xAC"), devices);
    CHECK(devices.composition.empty());
    CHECK(devices.composition_cursor == -1);
    CHECK(map.sample(devices).text == 0x65E5);
    CHECK(map.sample(devices).text == 0x672C);
    CHECK(map.sample(devices).text == 0);

    // Cancelling the composition (empty editing event) clears it.
    window.handle_event(editing("a", 1), devices);
    window.handle_event(editing("", 0), devices);
    CHECK(devices.composition.empty());
}

TEST_CASE("IME: scripts read the live composition, replays see none") {
    namespace fs = std::filesystem;
    InputMap map;
    script::HostOptions o;
    o.project_dir = fs::temp_directory_path() / "cuervo_ime_tests";
    fs::create_directories(o.project_dir);
    script::ScriptHost host(o, map);
    REQUIRE_MESSAGE(host.load({{"main.plm", R"(
class T { static var comp = "x"
          static var cur = 0 }
class Main extends Node {
    fn update(dt: float) {
        T.comp = Input.composition()
        T.cur = Input.composition_cursor()
    }
}
)"}},
                              "Main"),
                    host.error());
    host.tick(InputFrame{});
    auto comp = [&] { return static_cast<const pluma::StrObj*>(host.session()->global("T.comp").o)->s; };
    host.tick(InputFrame{});
    CHECK(comp().empty());
    host.set_composition("\xE3\x81\x82", 1);
    host.tick(InputFrame{});
    CHECK(comp() == "\xE3\x81\x82");
    CHECK(host.session()->global("T.cur").i == 1);
}

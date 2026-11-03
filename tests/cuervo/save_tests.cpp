// Save slots, migrations, backends and settings (milestone 11.4, DESIGN.md 7.2
// "Save data"), plus the Save / Settings script API.

#include <doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cuervo/save/save_store.hpp"
#include "cuervo/save/settings.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

save::SlotData sample() {
    save::SlotData d;
    d.set("level", std::int64_t{7});
    d.set("hp", 0.25);
    d.set("boss_dead", true);
    d.set("name", std::string("Corvid"));
    return d;
}

} // namespace

TEST_CASE("save: slot roundtrip through memory and file backends") {
    const fs::path dir = fs::temp_directory_path() / "cuervo_save_tests_roundtrip";
    fs::remove_all(dir);
    save::SaveStore stores[] = {save::SaveStore(std::make_unique<save::MemoryBackend>()),
                                save::SaveStore(std::make_unique<save::FileBackend>(dir))};
    for (save::SaveStore& store : stores) {
        std::string error;
        CHECK(store.save("slot1", sample(), &error));
        CHECK(store.exists("slot1"));
        save::LoadInfo info;
        const auto back = store.load("slot1", error, &info);
        REQUIRE_MESSAGE(back.has_value(), error);
        CHECK(*back == sample());
        CHECK(info.version == 1);
        CHECK(back->get_int("level") == 7);
        CHECK(back->get_int("missing", 3) == 3);
        CHECK(back->get_str("level", "x") == "x"); // other type: fallback
        CHECK(store.slots() == std::vector<std::string>{"slot1"});
        CHECK(store.remove("slot1"));
        CHECK_FALSE(store.exists("slot1"));
        CHECK_FALSE(store.load("slot1", error).has_value());
    }
    fs::remove_all(dir);
}

TEST_CASE("save: bad names, corruption and truncation are refused") {
    save::SaveStore store(std::make_unique<save::MemoryBackend>());
    std::string error;
    CHECK_FALSE(store.save("../evil", sample(), &error));
    CHECK_FALSE(save::valid_slot_name(""));
    CHECK_FALSE(save::valid_slot_name("a b"));
    CHECK(save::valid_slot_name("slot_1-b"));

    REQUIRE(store.save("s", sample()));
    auto& mem = static_cast<save::MemoryBackend&>(store.backend());
    auto good = mem.files["s"];
    auto flipped = good;
    flipped[6] = static_cast<std::byte>(~static_cast<unsigned>(flipped[6]));
    mem.files["s"] = flipped;
    CHECK_FALSE(store.load("s", error).has_value());
    CHECK(error.find("corrupt") != std::string::npos);
    mem.files["s"] = std::vector<std::byte>(good.begin(), good.begin() + 10);
    CHECK_FALSE(store.load("s", error).has_value());
    mem.files["s"] = {};
    CHECK_FALSE(store.load("s", error).has_value());
}

namespace {
void v1_to_v2(save::SlotData& d) { // "gold" was an int, becomes a float "coins"
    d.set("coins", static_cast<double>(d.get_int("gold")));
    d.fields.erase("gold");
}
void v2_to_v3(save::SlotData& d) { d.set("name", d.get_str("name", "nameless") + "!"); }
} // namespace

TEST_CASE("save: migrations chain from any older version") {
    save::SaveStore store(std::make_unique<save::MemoryBackend>());
    store.declare("game", 3, {{2, "name gets a bang", &v2_to_v3}, {1, "gold becomes coins", &v1_to_v2}});

    save::SlotData v1;
    v1.set("gold", std::int64_t{40});
    REQUIRE(store.save_versioned("game", v1, 1));
    std::string error;
    save::LoadInfo info;
    auto d = store.load("game", error, &info);
    REQUIRE_MESSAGE(d.has_value(), error);
    CHECK(info.version == 1);
    REQUIRE(info.migrated.size() == 2);
    CHECK(info.migrated[0] == "gold becomes coins");
    CHECK(d->get_float("coins") == 40.0);
    CHECK(d->get_str("name") == "nameless!");
    CHECK(d->find("gold") == nullptr);

    // From v2 only the second step runs; a current file runs none.
    save::SlotData v2;
    v2.set("name", std::string("Ann"));
    REQUIRE(store.save_versioned("game", v2, 2));
    d = store.load("game", error, &info);
    REQUIRE(d.has_value());
    CHECK(info.migrated.size() == 1);
    CHECK(d->get_str("name") == "Ann!");
    REQUIRE(store.save("game", *d));
    d = store.load("game", error, &info);
    CHECK(info.version == 3);
    CHECK(info.migrated.empty());

    // Newer than this build, and a gap in the chain, are errors.
    REQUIRE(store.save_versioned("game", v2, 9));
    CHECK_FALSE(store.load("game", error).has_value());
    CHECK(error.find("newer") != std::string::npos);
    store.declare("gap", 3, {{2, "only the last", &v2_to_v3}});
    REQUIRE(store.save_versioned("gap", v2, 1));
    CHECK_FALSE(store.load("gap", error).has_value());
    CHECK(error.find("no migration from version 1") != std::string::npos);
}

namespace {
struct RecordingTarget final : save::SettingsTarget {
    bool fullscreen = false, vsync = true, muted = false;
    float ui_scale = 0.0f;
    float volume[5] = {-1, -1, -1, -1, -1};
    std::map<std::string, std::string> bindings;
    void set_fullscreen(bool b) override { fullscreen = b; }
    void set_vsync(bool b) override { vsync = b; }
    void set_ui_scale(float s) override { ui_scale = s; }
    void set_bus_volume(std::size_t bus, float v) override { volume[bus] = v; }
    void set_muted(bool b) override { muted = b; }
    bool set_binding(const std::string& action, const std::string& binding) override {
        if (action == "bogus") return false;
        bindings[action] = binding;
        return true;
    }
};
} // namespace

TEST_CASE("settings: roundtrip, defaults, clamping and apply") {
    save::SaveStore store(std::make_unique<save::MemoryBackend>());
    std::string error;
    CHECK(save::load_settings(store, &error) == save::Settings{}); // nothing saved yet
    CHECK_FALSE(error.empty());

    save::Settings s;
    s.fullscreen = true;
    s.vsync = false;
    s.ui_scale = 1.5f;
    s.volume = {0.5f, 0.25f, 0.75f, 1.0f, 0.0f};
    s.muted = true;
    s.bindings["jump"] = "Key.Z";
    s.bindings["bogus"] = "Key.Q";
    REQUIRE(save::save_settings(store, s));
    CHECK(save::load_settings(store) == s);

    RecordingTarget t;
    CHECK(save::apply_settings(s, t) == 1); // "bogus" refused
    CHECK(t.fullscreen);
    CHECK_FALSE(t.vsync);
    CHECK(t.ui_scale == 1.5f);
    CHECK(t.volume[1] == 0.25f);
    CHECK(t.muted);
    CHECK(t.bindings["jump"] == "Key.Z");

    // Out-of-range stored values are clamped.
    save::SlotData d = save::to_slot(s);
    d.set("audio.sfx", 9.0);
    d.set("graphics.ui_scale", 100.0);
    const save::Settings clamped = save::from_slot(d);
    CHECK(clamped.volume[2] == 1.0f);
    CHECK(clamped.ui_scale == 3.0f);
}

namespace {
struct Host {
    InputMap map;
    std::shared_ptr<save::SaveStore> store = std::make_shared<save::SaveStore>(std::make_unique<save::MemoryBackend>());
    RecordingTarget target;
    std::unique_ptr<script::ScriptHost> host;

    explicit Host(const std::string& source, std::shared_ptr<save::SaveStore> shared = nullptr) {
        if (shared) store = std::move(shared);
        script::HostOptions o;
        o.project_dir = fs::temp_directory_path() / "cuervo_save_tests";
        fs::create_directories(o.project_dir);
        o.save_store = store;
        host = std::make_unique<script::ScriptHost>(o, map);
        host->save_context().target = &target;
        REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
        host->tick(InputFrame{});
        REQUIRE_MESSAGE(host->running(), host->error());
    }
    std::int64_t integer(std::string_view n) { return host->session()->global(n).i; }
};
} // namespace

TEST_CASE("save script API: write, commit, load in a fresh host, migrate by version") {
    auto shared = std::make_shared<save::SaveStore>(std::make_unique<save::MemoryBackend>());
    Host writer(R"(
class T { static var ok = false }
class Main extends Node {
    fn ready() {
        Save.set_int("slot1", "level", 4)
        Save.set_float("slot1", "hp", 0.5)
        Save.set_bool("slot1", "flag", true)
        Save.set_str("slot1", "who", "crow")
        T.ok = Save.commit("slot1", 2)
    }
}
)",
                shared);
    CHECK(writer.host->session()->global("T.ok").b);
    CHECK(shared->exists("slot1"));

    Host reader(R"(
class T { static var level = 0
          static var hp = 0.0
          static var flag = false
          static var who = ""
          static var ver = 0
          static var missing = 0
          static var loaded = false
          static var none = true }
class Main extends Node {
    fn ready() {
        T.none = Save.load("nope")
        T.loaded = Save.load("slot1")
        T.level = Save.get_int("slot1", "level", -1)
        T.hp = Save.get_float("slot1", "hp", 0.0)
        T.flag = Save.get_bool("slot1", "flag", false)
        T.who = Save.get_str("slot1", "who", "")
        T.ver = Save.version("slot1")
        T.missing = Save.get_int("slot1", "other", 9)
    }
}
)",
                shared);
    auto g = [&](const char* n) { return reader.host->session()->global(n); };
    CHECK_FALSE(g("T.none").b);
    CHECK(g("T.loaded").b);
    CHECK(g("T.level").i == 4);
    CHECK(g("T.hp").f == 0.5f);
    CHECK(g("T.flag").b);
    CHECK(g("T.ver").i == 2);
    CHECK(g("T.missing").i == 9);
}

TEST_CASE("settings script API applies on change and reloads from disk") {
    auto shared = std::make_shared<save::SaveStore>(std::make_unique<save::MemoryBackend>());
    Host first(R"(
class T { static var saved = false }
class Main extends Node {
    fn ready() {
        Settings.set_fullscreen(true)
        Settings.set_volume(#music, 0.3)
        Settings.set_ui_scale(2.0)
        Settings.set_muted(true)
        Settings.set_binding(#jump, "Key.Z")
        T.saved = Settings.save()
    }
}
)",
               shared);
    CHECK(first.host->session()->global("T.saved").b);
    CHECK(first.target.fullscreen); // applied immediately
    CHECK(first.target.volume[1] == 0.3f);
    CHECK(first.target.ui_scale == 2.0f);
    CHECK(first.target.bindings["jump"] == "Key.Z");

    Host second(R"(
class T { static var vol = 0.0
          static var fs = false }
class Main extends Node {
    fn ready() {
        Settings.load()
        T.vol = Settings.volume(#music)
        T.fs = Settings.fullscreen()
    }
}
)",
                shared);
    CHECK(second.host->session()->global("T.fs").b);
    CHECK(second.host->session()->global("T.vol").f == 0.3f);
    CHECK(second.target.fullscreen); // load applies too
    CHECK(second.target.muted);
    CHECK(second.target.bindings["jump"] == "Key.Z");
}

// The web backend's shape (milestone 11.9): a write-through wrapper that asks
// for a flush after every successful mutation, so IndexedDB follows the
// synchronous in-memory store.
TEST_CASE("save: write-through backend flushes after successful writes and removes") {
    int flushes = 0;
    save::SaveStore store(std::make_unique<save::WriteThroughBackend>(std::make_unique<save::MemoryBackend>(),
                                                                      [&flushes] { ++flushes; }));
    std::string error;
    CHECK(store.save("a", sample(), &error));
    CHECK(flushes == 1);
    CHECK_FALSE(store.save("bad name", sample(), &error)); // rejected before the backend
    CHECK(flushes == 1);
    CHECK(store.exists("a"));
    auto loaded = store.load("a", error);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == sample());
    CHECK(flushes == 1); // reads do not flush
    CHECK(store.remove("a"));
    CHECK(flushes == 2);
    CHECK_FALSE(store.remove("a")); // nothing removed, nothing to flush
    CHECK(flushes == 2);
    CHECK(store.slots().empty());
}

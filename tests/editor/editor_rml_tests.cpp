// State behind the RmlUi shell (DESIGN.md 4.13): the persisted layout section and play through
// EditorController. The shell itself needs a window; its rules live in settings and the controller.

#include <doctest.h>

#include <string>

#include "editor/editor_controller.hpp"
#include "editor/frame_policy.hpp"
#include "editor/projects.hpp"
#include "editor/settings.hpp"
#include "cuervo/platform/platform.hpp"
#include <filesystem>
#include <fstream>

using namespace cuervo;
using namespace cuervo::editor;

TEST_CASE("rml layout: sizes, collapsed state and active tabs survive a round trip") {
    EditorSettings a;
    RmlLayout& l = a.rml_layout();
    l.left_width = 333;
    l.right_width = 410;
    l.drawer_height = 150;
    l.left_collapsed = true;
    l.drawer_open = true;
    l.rail = "assets";
    l.center = "script";
    l.drawer = "history";
    l.theme = "light";
    l.ui_scale = 1.5f;
    l.closed_sections = "Advanced;Signals";
    l.tree_scene = "scenes/level.scene";
    l.tree_collapsed = "/walls;/hud";
    a.set_ui_scale(1.25f);
    EditorSettings b;
    b.parse(a.serialize());
    CHECK(b.rml_layout() == a.rml_layout());
    CHECK(b.ui_scale() == doctest::Approx(1.25f)); // the shared keys stay in place
}

TEST_CASE("rml layout: bad values are clamped or ignored, an older file keeps the defaults") {
    EditorSettings s;
    s.parse("ui_scale = 1\n[rml_layout]\nleft_width = 99999\nright_width = -4\nrail = nonsense\ndrawer = console\nui_scale = 99\nunknown = 3\n");
    const RmlLayout& l = s.rml_layout();
    CHECK(l.left_width == 800);
    CHECK(l.right_width == 200);
    CHECK(l.rail == "scene");
    CHECK(l.drawer == "console");
    CHECK(l.ui_scale == 0.0f);
    EditorSettings old;
    old.parse("ui_scale = 1.5\nfont_scale = 1\n[last_scenes]\n/p = scenes/a.scene\n");
    CHECK(old.rml_layout() == RmlLayout{});
    CHECK(old.ui_scale() == doctest::Approx(1.5f));
    CHECK(old.last_scene("/p").empty() == false);
}

TEST_CASE("rml layout: last_scenes after the layout section still parse") {
    EditorSettings s;
    s.rml_layout().left_width = 300;
    std::string text = s.serialize() + "[last_scenes]\n/proj = scenes/x.scene\n";
    EditorSettings t;
    t.parse(text);
    CHECK(t.rml_layout().left_width == 300);
    CHECK(t.last_scene("/proj") == "scenes/x.scene");
}

TEST_CASE("controller: play starts, steps and stops without a window") {
    EditorController c;
    std::string error;
    REQUIRE_MESSAGE(c.env().open(std::string(CUERVO_EXAMPLES_DIR) + "/editor_level", nullptr, error), error);
    c.doc().set_registry(&c.env().classes());
    REQUIRE(c.doc().open(c.env().resolve(c.env().project().main_scene), error));
    c.set_play_setup({{}, 1, false});
    REQUIRE(c.start_play(true));
    CHECK(c.play().active());
    DeviceState none;
    c.play().pause();
    const std::uint64_t t0 = c.play().tick();
    c.play().step(none, {0.0f, 0.0f}, 3);
    CHECK(c.play().tick() == t0 + 3);
    c.stop_play();
    CHECK_FALSE(c.play().active());
}

#include "editor/rml_widgets.hpp"

TEST_CASE("widget kit: tooltips split the shortcut, escaping covers attributes") {
    const w::Tip t = w::split_tip("Save scene (Ctrl+S)");
    CHECK(t.text == "Save scene");
    CHECK(t.keys == "Ctrl+S");
    CHECK(w::split_tip("Frame scene").keys.empty());
    CHECK(w::split_tip("a ()").keys.empty());
    CHECK(w::esc("a<b>\"&") == "a&lt;b&gt;&quot;&amp;");
    CHECK(w::tip_body(t).find("class=\"kbd\">Ctrl+S") != std::string::npos);
}

TEST_CASE("widget kit: every widget carries its state classes and a data key") {
    CHECK(w::toggle("enabled", true).find("tog on") != std::string::npos);
    CHECK(w::toggle("enabled", false, true).find("disabled") != std::string::npos);
    CHECK(w::toggle("enabled", false).find("tog on") == std::string::npos);
    const std::string s = w::scrub("position.x", "-148.0", "x", true);
    CHECK(s.find("scrub mod") != std::string::npos);
    CHECK(s.find("ax-x\">X") != std::string::npos); // axis is a letter too, not color alone
    CHECK(w::text_field("f", "", "Filter", true).find("clear hidden") != std::string::npos);
    CHECK(w::text_field("f", "abc", "", true).find("clear hidden") == std::string::npos);
    CHECK(w::dropdown("mode", "Burst").find("data-dd=\"mode\"") != std::string::npos);
    const std::string seg = w::segmented("view", {{"2d", "2D"}, {"wire", "Wire"}}, "wire");
    CHECK(seg.find("segi active\" data-segi=\"wire") != std::string::npos);
    CHECK(seg.find("segi active\" data-segi=\"2d") == std::string::npos);
    CHECK(w::badge("3", "err").find("tag err") != std::string::npos);
    const std::string r = w::row("data-id=\"4\"", "ship", "Ship", {.selected = true, .disabled = true, .indent = 16}, "node");
    CHECK(r.find("row selected disabled") != std::string::npos);
    CHECK(r.find("padding-left: 22dp") != std::string::npos);
    const std::string sec = w::section("Advanced", "Advanced", false, "5 hidden");
    CHECK(sec.find("data-sect=\"Advanced\"") != std::string::npos);
    CHECK(sec.find("sect open") == std::string::npos);
    const std::string p = w::prop("fire_rate", "fire_rate", "<x/>", {.modified = true});
    CHECK(p.find("prop mod") != std::string::npos);
    CHECK(p.find("data-reset=\"fire_rate\"") != std::string::npos);
    CHECK(w::prop("a", "a", "<x/>").find("data-reset") == std::string::npos);
    const std::string m = w::popup_menu("data-act", {{"a", "Cut", "Ctrl+X"}, {"-", "", ""}, {"b", "Paste", "", true, true}});
    CHECK(m.find("msep") != std::string::npos);
    CHECK(m.find("mi off") != std::string::npos);
}

#include "editor/console_log.hpp"

TEST_CASE("console filter: chips map severities to bits, copy/clear keep counts") {
    using cuervo::log::Level;
    using namespace cuervo::editor;
    CHECK(console_filter_bit(Level::Debug) == 1);
    CHECK(console_filter_bit(Level::Warn) == 2);
    CHECK(console_filter_bit(Level::Error) == 4);
    CHECK(console_filter_shows(7, Level::Warn));
    CHECK_FALSE(console_filter_shows(5, Level::Warn));
    ConsoleLog log(10);
    log.add(Level::Info, "a:1", "one");
    log.add(Level::Warn, "a:2", "two");
    log.add(Level::Error, "a:3", "three");
    CHECK(log.count(Level::Error) == 1);
    log.clear();
    CHECK(log.count(Level::Error) == 0);
    std::uint64_t seen = 0;
    std::vector<ConsoleLine> out;
    log.snapshot(seen, out);
    CHECK(out.empty());
}

TEST_CASE("welcome: example projects are the folders with a project.toml, sorted") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "cuervo_welcome_examples";
    fs::remove_all(root);
    for (const char* name : {"zeta", "alpha", "not_a_project"}) fs::create_directories(root / name);
    std::ofstream(root / "zeta" / "project.toml") << "name = \"z\"\n";
    std::ofstream(root / "alpha" / "project.toml") << "name = \"a\"\n";
    std::ofstream(root / "stray.txt") << "x";
    const auto found = list_example_projects(root);
    REQUIRE(found.size() == 2);
    CHECK(found[0].first == "alpha");
    CHECK(found[1].first == "zeta");
    CHECK(list_example_projects(root / "missing").empty());
    fs::remove_all(root);
}

TEST_CASE("welcome: removing a recent project persists") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "cuervo_welcome_recents";
    fs::remove_all(dir);
    fs::create_directories(dir / "a");
    fs::create_directories(dir / "b");
    RecentProjects r;
    r.add(dir / "a");
    r.add(dir / "b");
    r.remove(platform::utf8(dir / "a"));
    std::string error;
    REQUIRE(r.save(dir / "recent.txt", error));
    RecentProjects loaded;
    REQUIRE(loaded.load(dir / "recent.txt"));
    REQUIRE(loaded.list().size() == 1);
    // add() stores the canonical spelling (long name on Windows, where temp paths use 8.3 names).
    CHECK(loaded.list()[0].path == platform::utf8(std::filesystem::weakly_canonical(dir / "b")));
    fs::remove_all(dir);
}

TEST_CASE("named layouts: save, load, delete and the round trip through the settings text") {
    EditorSettings a;
    RmlPanels wide;
    wide.left_width = 400;
    wide.right_width = 500;
    wide.drawer_open = true;
    wide.drawer_height = 320;
    wide.right_collapsed = true;
    wide.rail = "assets";
    wide.center = "script";
    wide.drawer = "profiler";
    REQUIRE(a.save_layout_as("  Debug = [wide]  ", wide)); // sanitised to "Debug  wide" without '=', '[' and ']'
    REQUIRE(a.save_layout_as("Art", RmlPanels{}));
    CHECK_FALSE(a.save_layout_as("  = [] ", wide));
    CHECK(a.layouts().size() == 2);
    CHECK(a.layouts().count("Debug  wide") == 1);

    // Theme, scale and the user lists are not part of a layout.
    a.rml_layout().theme = "light";
    a.rml_layout().add_favorites = "Sprite2D";
    EditorSettings b;
    b.parse(a.serialize());
    REQUIRE(b.layouts().size() == 2);
    CHECK(b.layouts().at("Debug  wide") == wide);
    CHECK(b.layouts().at("Art") == RmlPanels{});
    CHECK(b.rml_layout().theme == "light");

    // Loading applies the panels and leaves everything else alone.
    RmlLayout live = b.rml_layout();
    live.set_panels(b.layouts().at("Debug  wide"));
    CHECK(live.left_width == 400);
    CHECK(live.drawer == "profiler");
    CHECK(live.theme == "light");
    CHECK(live.add_favorites == "Sprite2D");

    CHECK(b.delete_layout("Art"));
    CHECK_FALSE(b.delete_layout("Art"));
    EditorSettings c;
    c.parse(b.serialize());
    CHECK(c.layouts().size() == 1);
    CHECK(EditorSettings::clean_layout_name(std::string(100, 'x')).size() == EditorSettings::max_layout_name);
    CHECK(EditorSettings::clean_layout_name("a\nb") == "ab");
}

TEST_CASE("settings: script editor format on save and per-project breakpoints round trip") {
    EditorSettings a;
    CHECK(a.format_on_save()); // on unless the user turned it off
    a.set_format_on_save(false);
    a.set_breakpoints("/proj/one", {{"scripts/a.plm", 12}, {"scripts/b c.plm", 3}});
    a.set_breakpoints("/proj/two", {{"x.plm", 1}});
    a.set_last_scene("/proj/one", "scenes/a.scene");
    EditorSettings b;
    b.parse(a.serialize());
    CHECK_FALSE(b.format_on_save());
    const auto one = b.breakpoints("/proj/one");
    REQUIRE(one.size() == 2);
    CHECK(one[1] == std::pair<std::string, int>{"scripts/b c.plm", 3});
    CHECK(b.breakpoints("/proj/two").size() == 1);
    CHECK(b.last_scene("/proj/one") == "scenes/a.scene");
    b.set_breakpoints("/proj/two", {});
    CHECK(b.breakpoints("/proj/two").empty());
    EditorSettings old;
    old.parse("ui_scale = 1\n[last_scenes]\n/p = s.scene\n");
    CHECK(old.format_on_save());
    CHECK(old.breakpoints("/p").empty());
}

TEST_CASE("frame policy: idle blocks, input settles, the tick and busy states draw") {
    using namespace cuervo::editor;
    FrameState idle{false, false, 5.0, false, 100};
    CHECK(may_block_for_events(idle));
    CHECK_FALSE(should_draw(idle));         // nothing changed: no frame, near-zero idle cost
    idle.tick_due = true;
    CHECK(should_draw(idle));               // the 4 Hz tick draws one

    FrameState after_key{false, false, 0.05, false, 100};
    CHECK_FALSE(may_block_for_events(after_key)); // polls, never sleeps, while settling
    CHECK(should_draw(after_key));          // several frames follow a key event
    after_key.since_input_s = 0.39;
    CHECK(should_draw(after_key));          // still drawing at the end of the settle window
    after_key.since_input_s = 0.41;
    CHECK_FALSE(should_draw(after_key));

    FrameState pending{false, true, 5.0, false, 100}; // a debounce or animation is pending
    CHECK_FALSE(may_block_for_events(pending));
    CHECK(should_draw(pending));
    CHECK(should_draw(FrameState{false, false, 5.0, false, 3})); // warm-up frames always draw
    CHECK(should_draw(FrameState{true, false, 5.0, false, 100}));
}

TEST_CASE("settings: [last_scenes] keys are normalized on parse, so a differently spelled path still matches") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "cuervo_settings_keys";
    fs::create_directories(dir / "sub");
    EditorSettings s;
    s.parse("[last_scenes]\n" + platform::utf8(dir / "sub" / ".." / "sub") + " = scenes/a.scene\n");
    CHECK(s.last_scene(dir / "sub") == "scenes/a.scene");
    fs::remove_all(dir);
}

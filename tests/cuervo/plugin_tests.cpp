// Native plugins (milestone 10.5c, docs/PLUGINS.md): the C ABI host, the C++
// wrapper (examples/plugins/orbit), the Rust crate (examples/plugins/noise)
// and the .NET host (examples/plugins/hues), in the VM and in AOT modules.

#include <doctest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/plugin/cuervo_plugin.hpp"
#include "cuervo/script/plugin_host.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "pluma/aot.hpp"
#include "tools/cuervo_tools/export.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

const fs::path examples = CUERVO_EXAMPLES_DIR;

// A plugin linked into the test itself, over the C++ wrapper.
struct Counter {
    int hits = 0;
};

} // namespace

CUERVO_PLUGIN(probe) {
    plugin.function("add", [](std::int32_t a, std::int32_t b) { return a + b; });
    plugin.function("scale", [](cuervo::plugin::Vec2 v, float k) { return cuervo::plugin::Vec2{{v[0] * k, v[1] * k}}; });
    plugin.function("greet", [](std::string_view who) { return std::string("hello, ") + std::string(who); });
    plugin.function("wide", [](std::int64_t x) -> std::int64_t { return x * 1000000000LL; });
    plugin.function("half", [](double x) { return x / 2.0; });
    plugin.function("fail", [](std::int32_t) -> std::int32_t { throw std::runtime_error("probe says no"); });
    plugin.function("Box.bump", [](cuervo::plugin::Object self) {
        auto* c = self.native<Counter>();
        if (c == nullptr) {
            c = new Counter();
            self.set_native(c);
        }
        ++c->hits;
        self.set_int("count", c->hits);
        self.set_float("ratio", self.get_float("ratio") * 2.0);
        return self;
    });
    plugin.function("Box.kind", [](cuervo::plugin::Object self) { return std::string(self.type()); });
    plugin.finalizer<Counter>("Box");
    plugin.importer(".upper", ".txt", [](std::string_view, std::span<const std::uint8_t> data) {
        std::vector<std::uint8_t> out(data.begin(), data.end());
        for (auto& c : out) c = static_cast<std::uint8_t>(std::toupper(c));
        return out;
    });
}

namespace {

const char* probe_api = R"(
@native pub fn add(a: int, b: int) -> int {}
@native pub fn scale(v: vec2, k: float) -> vec2 {}
@native pub fn greet(who: str) -> str {}
@native pub fn wide(x: i64) -> i64 {}
@native pub fn half(x: f64) -> f64 {}
@native pub fn fail(x: int) -> int {}
pub class Box {
    var count = 0
    var ratio = 1.5
    @native fn bump() -> Box {}
    @native fn kind() -> str {}
}
)";

std::shared_ptr<script::PluginSet> probe_set() {
    auto set = std::make_shared<script::PluginSet>();
    std::string error;
    REQUIRE_MESSAGE(set->add_entry("probe", "1.0.0", true, cuervo_plugin_entry_probe, error), error);
    return set;
}

struct Run {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;
    std::string error;
    bool load(std::shared_ptr<const script::PluginSet> plugins, std::vector<pluma::SourceFile::Part> parts,
              const std::string& main) {
        script::HostOptions o;
        o.plugins = std::move(plugins);
        host = std::make_unique<script::ScriptHost>(o, map);
        const bool ok = host->load(parts, main);
        error = host->error();
        return ok;
    }
    std::string global(std::string_view name) {
        return host->session()->vm().format(host->session()->global(name), false);
    }
};

fs::path temp_project(std::string_view name) {
    const fs::path d = fs::temp_directory_path() / std::format("cuervo_plugin_{}_{}", name, std::random_device{}());
    fs::create_directories(d);
    return d;
}

void write_text(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string text_of(const assets::VfsFile& f) { return std::string(f.text()); }

} // namespace

TEST_CASE("plugins: natives marshal every argument and result type through the ABI") {
    Run r;
    REQUIRE_MESSAGE(r.load(probe_set(),
                           {{"packages/probe/scripts/probe.plm", probe_api, "probe"},
                            {"scripts/main.plm", R"(
import probe
var a = probe.add(2, 40)
var s = probe.scale(vec2(1.0, -2.0), 3.0)
var g = probe.greet("cuervo")
var w = probe.wide(5)
var h = probe.half(3.0)
var b = probe.Box()
var bumped = b.bump().bump().count
var ratio = b.ratio
var kind = b.kind()
class Main extends Node {}
)"}},
                           "Main"),
                    r.error);
    CHECK(r.global("a") == "42");
    CHECK(r.global("s") == "vec2(3.0, -6.0)");
    CHECK(r.global("g") == "hello, cuervo");
    CHECK(r.global("w") == "5000000000");
    CHECK(r.global("h") == "1.5");
    CHECK(r.global("bumped") == "2");
    CHECK(r.global("ratio") == "6.0");
    CHECK(r.global("kind") == "probe.Box");
}

TEST_CASE("plugins: a C++ exception in a native is a script panic") {
    Run r;
    CHECK_FALSE(r.load(probe_set(),
                       {{"packages/probe/scripts/probe.plm", probe_api, "probe"},
                        {"scripts/main.plm", "import probe\nvar x = probe.fail(1)\nclass Main extends Node {}\n"}},
                       "Main"));
    CHECK(r.error.find("probe says no") != std::string::npos);
}

TEST_CASE("plugins: names stay in the plugin's namespace; importers convert assets") {
    auto set = probe_set();
    REQUIRE(set->plugins().size() == 1);
    CHECK(set->plugins()[0].natives.size() == 8);
    const script::PluginImporter* imp = set->importer_for("notes/readme.upper");
    REQUIRE(imp != nullptr);
    CHECK(imp->output_extension == ".txt");
    std::vector<std::byte> out;
    std::string error;
    const std::string text = "quiet";
    REQUIRE(script::PluginSet::run_importer(*imp, "notes/readme.upper",
                                            std::as_bytes(std::span(text.data(), text.size())), out, error));
    CHECK(std::string(reinterpret_cast<const char*>(out.data()), out.size()) == "QUIET");
    CHECK(set->importer_for("notes/readme.txt") == nullptr);

    // A plugin that registers outside its namespace is refused.
    script::PluginSet bad;
    CHECK_FALSE(bad.add_entry("other", "1.0.0", true, cuervo_plugin_entry_probe, error));
    CHECK(error.find("outside its namespace") != std::string::npos);
}

TEST_CASE("plugins: importers convert sources on read in development (Vfs::add_derived)") {
    const fs::path dir = temp_project("derived");
    write_text(dir / "notes/readme.upper", "quiet");
    write_text(dir / "notes/hand.upper", "made");
    write_text(dir / "notes/hand.txt", "by hand"); // a real file wins over a derived one
    auto vfs = assets::Vfs::dev(dir);
    std::string error;
    CHECK_FALSE(vfs->exists("notes/readme.txt"));
    script::add_importers(probe_set(), *vfs);
    CHECK(vfs->exists("notes/readme.txt"));
    auto f = vfs->open("notes/readme.txt", error);
    REQUIRE_MESSAGE(f.has_value(), error);
    CHECK(text_of(*f) == "QUIET");
    f = vfs->open("notes/hand.txt", error);
    REQUIRE(f.has_value());
    CHECK(text_of(*f) == "by hand");
    CHECK(vfs->list("notes", ".txt") == std::vector<std::string>{"notes/hand.txt", "notes/readme.txt"});
    CHECK(vfs->list("notes").size() == 4);
    CHECK_FALSE(vfs->open("notes/none.txt", error).has_value());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("plugins: package.toml [plugin], ABI versions and missing libraries") {
    const fs::path root = fs::temp_directory_path() / std::format("cuervo_plugin_test_{}", std::random_device{}());
    fs::create_directories(root / "p");
    auto load = [&](const std::string& manifest) {
        std::ofstream(root / "p" / "package.toml") << manifest;
        std::string error;
        auto vfs = assets::Vfs::dev(root / "p");
        auto m = script::load_package_manifest(*vfs, "", error);
        if (!m) return error;
        std::vector<script::ResolvedPackage> pkgs{{*m, vfs, "", root / "p"}};
        auto set = script::PluginSet::load(pkgs, error);
        return set ? std::string("loaded") : error;
    };
    CHECK(load("name = \"p\"\nversion = \"1.0.0\"\n[plugin]\nlinux = \"x.so\"\n").find("needs 'abi'") !=
          std::string::npos);
    CHECK(load("name = \"p\"\nversion = \"1.0.0\"\n[plugin]\nabi = 99\nlinux = \"x.so\"\nwindows = \"x.dll\"\n")
              .find("built for plugin ABI 99") != std::string::npos);
    CHECK(load("name = \"p\"\nversion = \"1.0.0\"\n[plugin]\nabi = 1\nlinux = \"missing.so\"\nwindows = \"m.dll\"\n")
              .find("can't load") != std::string::npos);
    CHECK(load("name = \"p\"\nversion = \"1.0.0\"\n[plugin]\nabi = 1\nbogus = \"x\"\n").find("unknown key 'bogus'") !=
          std::string::npos);
    std::error_code ec;
    fs::remove_all(root, ec);
}

#if CUERVO_TEST_PLUGIN_ORBIT
TEST_CASE("plugins: the C++ example plugin adds a node and functions (examples/plugins/orbit)") {
    std::string error;
    auto vfs = assets::Vfs::dev(examples / "plugins" / "orbit");
    auto m = script::load_package_manifest(*vfs, "", error);
    REQUIRE_MESSAGE(m.has_value(), error);
    std::vector<script::ResolvedPackage> pkgs{{*m, vfs, "", examples / "plugins" / "orbit"}};
    auto set = script::PluginSet::load(pkgs, error);
    REQUIRE_MESSAGE(set != nullptr, error);
    CHECK(set->plugins()[0].kind == "native");
    CHECK(set->plugins()[0].sim_safe);
    std::vector<pluma::SourceFile::Part> parts;
    for (const std::string& f : vfs->list("scripts", ".plm")) {
        std::string text;
        REQUIRE(vfs->read_text(f, text, error));
        parts.push_back({"packages/orbit/" + f, text, "orbit"});
    }
    parts.push_back({"scripts/main.plm", R"(
import orbit
var o = orbit.Orbiter()
var e = 0.0
var d = orbit.describe()
var n = orbit.orbits_in(60.0, 1.6)
class Main extends Node {
    fn ready() {
        o.radius = 10.0
        o.speed = 2.0
        add_child(o)
        e = o.energy()
    }
}
)"});
    Run r;
    REQUIRE_MESSAGE(r.load(set, parts, "Main"), r.error);
    for (int i = 0; i < 240; ++i) r.host->tick(InputFrame{});
    CHECK(r.global("e") == "200.0");
    CHECK(r.global("d") == "orbit 1.0.0 (C++)");
    CHECK(r.global("n") == "15");
    // 240 ticks at 2 rad/s: 8 rad, one lap.
    CHECK(r.host->session()->vm().format(r.host->session()->global("o"), false).find("laps = 1") !=
          std::string::npos);

    // AOT: the static library linked into the module, registered from its table.
    auto statics = script::static_plugins(pkgs, error);
    REQUIRE_MESSAGE(statics.has_value(), error);
    REQUIRE(statics->size() == 1);
    auto so = script::compile_aot_scripts(parts, error, 1, set, *statics);
    REQUIRE_MESSAGE(so.has_value(), error);
    auto module = pluma::AotModule::open_bytes(*so, error);
    REQUIRE_MESSAGE(module != nullptr, error);
    auto linked = std::make_shared<script::PluginSet>();
    REQUIRE_MESSAGE(linked->add_aot_plugins(*module, error), error);
    REQUIRE(linked->plugins().size() == 1);
    CHECK(linked->plugins()[0].kind == "static");
    InputMap map;
    script::HostOptions o;
    o.plugins = linked;
    script::ScriptHost aot(o, map);
    REQUIRE_MESSAGE(aot.load_native(*so, "Main"), aot.error());
    for (int i = 0; i < 240; ++i) aot.tick(InputFrame{});
    CHECK(aot.session()->vm().format(aot.session()->global("e"), false) == "200.0");
}
#endif

#if CUERVO_TEST_PLUGIN_ORBIT
TEST_CASE("plugins: export runs plugin importers and ships the converted file") {
    const fs::path dir = temp_project("export");
    write_text(dir / "project.toml", std::format("name = \"importer_game\"\nmain = \"Main\"\n"
                                                 "[dependencies.orbit]\npath = \"{}\"\nversion = \"1.0.0\"\n",
                                                 (examples / "plugins" / "orbit").generic_string()));
    write_text(dir / "scripts/main.plm", "import orbit\nclass Main extends Node {}\n");
    write_text(dir / "notes/readme.upper", "caw");

    // Development: the project's plugins convert on read.
    std::string error;
    auto vfs = assets::Vfs::dev(dir);
    auto plugins = script::load_project_plugins(*vfs, error);
    REQUIRE_MESSAGE(plugins != nullptr, error);
    script::add_importers(plugins, *vfs);
    auto f = vfs->open("notes/readme.txt", error);
    REQUIRE_MESSAGE(f.has_value(), error);
    CHECK(text_of(*f) == "CAW");

    // Release: the pack holds the converted file, not the source.
    tools::PackOptions o;
#if defined(_WIN32)
    o.platform = assets::PackPlatform::Windows;
#else
    o.platform = assets::PackPlatform::Linux;
#endif
    o.font_path = std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf";
    o.engine_version = "test";
    tools::PackSummary summary;
    auto bytes = tools::pack_project(dir, o, summary, error);
    REQUIRE_MESSAGE(bytes.has_value(), error);
    auto pack = assets::Pack::from_bytes(std::move(*bytes), error);
    REQUIRE_MESSAGE(pack != nullptr, error);
    CHECK_FALSE(pack->contains("notes/readme.upper"));
    REQUIRE(pack->contains("notes/readme.txt"));
    const auto data = *pack->find("notes/readme.txt");
    CHECK(std::string(reinterpret_cast<const char*>(data.data()), data.size()) == "CAW");
    CHECK(summary.imported >= 1);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

#if CUERVO_TEST_PLUGIN_NOISE
TEST_CASE("plugins: the Rust example plugin in the VM and linked into AOT (examples/plugins/noise)") {
    std::string error;
    auto vfs = assets::Vfs::dev(examples / "plugins" / "noise");
    auto m = script::load_package_manifest(*vfs, "", error);
    REQUIRE_MESSAGE(m.has_value(), error);
    std::vector<script::ResolvedPackage> pkgs{{*m, vfs, "", examples / "plugins" / "noise"}};
    auto set = script::PluginSet::load(pkgs, error);
    REQUIRE_MESSAGE(set != nullptr, error);
    std::vector<pluma::SourceFile::Part> parts;
    for (const std::string& f : vfs->list("scripts", ".plm")) {
        std::string text;
        REQUIRE(vfs->read_text(f, text, error));
        parts.push_back({"packages/noise/" + f, text, "noise"});
    }
    parts.push_back({"scripts/main.plm", R"(
import noise
var w = noise.Wisp()
var v = noise.value(1.25, 2.5, 7)
var h = noise.hash(3, 4, 7)
var d = noise.describe()
var at = vec2(0.0, 0.0)
class Main extends Node {
    fn ready() { add_child(w) }
    fn update(dt: float) { at = w.position }
}
)"});
    Run r;
    REQUIRE_MESSAGE(r.load(set, parts, "Main"), r.error);
    for (int i = 0; i < 120; ++i) r.host->tick(InputFrame{});
    const std::vector<std::string> names{"v", "h", "d", "at"};
    std::vector<std::string> vm_values;
    for (const std::string& n : names) vm_values.push_back(r.global(n));
    CHECK(vm_values[3] != r.host->session()->vm().format(pluma::vec2_value(0.0f, 0.0f), false));

#if defined(__MINGW32__)
    // MinGW-w64 links Rust's GNU-target static library (examples/plugins/CMakeLists.txt).
    if (!fs::exists(examples / "plugins" / "noise" / "lib" / "libnoise_gnu.a")) {
        MESSAGE("AOT half skipped: Rust target x86_64-pc-windows-gnu not installed");
        return;
    }
#endif
    auto statics = script::static_plugins(pkgs, error);
    REQUIRE_MESSAGE(statics.has_value(), error);
    REQUIRE(statics->size() == 1);
    auto so = script::compile_aot_scripts(parts, error, 1, set, *statics);
    REQUIRE_MESSAGE(so.has_value(), error);
    auto module = pluma::AotModule::open_bytes(*so, error);
    REQUIRE_MESSAGE(module != nullptr, error);
    auto linked = std::make_shared<script::PluginSet>();
    REQUIRE_MESSAGE(linked->add_aot_plugins(*module, error), error);
    REQUIRE(linked->plugins().size() == 1);
    CHECK(linked->plugins()[0].kind == "static");
    InputMap map;
    script::HostOptions o;
    o.plugins = linked;
    script::ScriptHost aot(o, map);
    REQUIRE_MESSAGE(aot.load_native(*so, "Main"), aot.error());
    for (int i = 0; i < 120; ++i) aot.tick(InputFrame{});
    for (std::size_t i = 0; i < names.size(); ++i) {
        CHECK(aot.session()->vm().format(aot.session()->global(names[i]), false) == vm_values[i]);
    }
}
#endif

#if CUERVO_TEST_PLUGIN_ORBIT && CUERVO_TEST_PLUGIN_NOISE && CUERVO_TEST_PLUGIN_HUES
TEST_CASE("examples/plugin_demo: C++, Rust and C# plugins in one game") {
    std::string error;
    auto vfs = assets::Vfs::dev(examples / "plugin_demo");
    auto project = script::load_project(*vfs, error);
    REQUIRE_MESSAGE(project.has_value(), error);
    std::vector<pluma::SourceFile::Part> parts;
    REQUIRE_MESSAGE(script::read_scripts(*vfs, parts, nullptr, error), error);
    auto plugins = script::load_project_plugins(*vfs, error);
    REQUIRE_MESSAGE(plugins != nullptr, error);
    REQUIRE(plugins->plugins().size() == 3);
    CHECK(plugins->plugins()[0].kind == "native");
    CHECK(plugins->plugins()[1].kind == "native");
    CHECK(plugins->plugins()[2].kind == "dotnet");
    CHECK(plugins->unsafe_plugins() == std::vector<std::string>{"hues"});
    Run r;
    REQUIRE_MESSAGE(r.load(plugins, parts, project->main_class), r.error);
    for (int i = 0; i < 300; ++i) {
        r.host->tick(InputFrame{});
        REQUIRE_MESSAGE(r.host->running(), r.host->error());
    }
    pluma::VM& vm = r.host->session()->vm();
    pluma::Value laps;
    REQUIRE(vm.call_method(r.host->root(), "total_laps", {}, &laps).ok);
    CHECK(laps.i >= 1);
    pluma::Value drift;
    REQUIRE(vm.call_method(r.host->root(), "wisp_drift", {}, &drift).ok);
    CHECK(drift.f > 1.0f);
}
#endif

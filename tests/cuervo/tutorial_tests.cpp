// examples/tutorial (the game docs/GUIDE.md builds) loads and plays headless:
// the guide's final code must keep running as the engine changes.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim_line(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

} // namespace

// The guide teaches Starfall step by step and says "every file below is the final version in
// examples/tutorial/" (DESIGN.md 4.16 "Clarified (Milestone 8.4)"): so every line of the game's files
// must appear in one of the guide's code blocks.
TEST_CASE("tutorial: docs/GUIDE.md shows every line of examples/tutorial") {
    const fs::path examples = CUERVO_EXAMPLES_DIR;
    const fs::path root = examples / "tutorial";
    const std::string guide = read_text(examples.parent_path() / "docs" / "GUIDE.md");
    REQUIRE_FALSE(guide.empty());

    std::set<std::string> guide_lines; // lines of the guide's pluma and toml code blocks
    std::istringstream in(guide);
    std::string line, lang;
    bool fenced = false;
    while (std::getline(in, line)) {
        if (line.rfind("```", 0) == 0) {
            fenced = !fenced;
            lang = fenced ? line.substr(3) : "";
            continue;
        }
        if (fenced && (lang == "pluma" || lang == "toml")) guide_lines.insert(trim_line(line));
    }
    REQUIRE_FALSE(guide_lines.empty());

    std::vector<fs::path> files = {root / "project.toml"};
    for (const auto& e : fs::directory_iterator(root / "scripts")) files.push_back(e.path());
    for (const fs::path& f : files) {
        std::istringstream src(read_text(f));
        std::string l;
        while (std::getline(src, l)) {
            const std::string t = trim_line(l);
            if (t.empty()) continue;
            CHECK_MESSAGE(guide_lines.count(t) == 1, f.filename().string() << " has a line the guide doesn't show: " << t);
        }
    }
}

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
namespace {

struct Tutorial {
    script::Project project;
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    Tutorial() {
        std::string error;
        auto p = script::load_project(fs::path(CUERVO_EXAMPLES_DIR) / "tutorial", error);
        REQUIRE_MESSAGE(p.has_value(), error);
        project = *p;
        REQUIRE_MESSAGE(script::apply_input(project, map, error), error);
        std::vector<pluma::SourceFile::Part> sources;
        REQUIRE_MESSAGE(script::read_scripts(project, sources, nullptr, error), error);
        script::HostOptions o;
        o.seed = project.seed;
        o.tick_rate = project.tick_rate;
        o.project_dir = project.dir;
        o.virtual_size = vec2{static_cast<float>(project.virtual_size.x()), static_cast<float>(project.virtual_size.y())};
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load(sources, project.main_class), host->error());
    }

    void run(int ticks, InputFrame input = {}) {
        for (int i = 0; i < ticks; ++i) {
            host->tick(input);
            REQUIRE_MESSAGE(host->running(), host->error());
        }
    }

    pluma::Value game_field(std::string_view field) {
        pluma::Value g = host->session()->global("Game.main");
        REQUIRE(g.is_obj());
        auto* inst = static_cast<pluma::InstanceObj*>(g.o);
        for (std::size_t i = 0; i < inst->type->field_names.size(); ++i) {
            if (inst->type->field_names[i] == field) return inst->fields[i];
        }
        FAIL("no field " << field);
        return {};
    }
};

} // namespace

TEST_CASE("tutorial: Starfall plays headless, loses its lives and offers a retry") {
    Tutorial t;
    t.run(60);
    CHECK(t.game_field("playing").b);
    CHECK(t.game_field("lives").i == 3);
#if CUERVO_FEATURE_PARTICLES
    CHECK(t.host->particles().particle_count() > 0); // it's raining
#endif

    // Nobody steers, so the rain wears the player down within a minute or two.
    int ticks = 0;
    while (t.game_field("playing").b && ticks < 60 * 180) {
        t.run(60);
        ticks += 60;
    }
    CHECK_FALSE(t.game_field("playing").b);
    CHECK(t.game_field("lives").i == 0);

    // The menu's button (focused by show_game_over) restarts the round.
    const auto accept = t.map.action_index(Name("ui_accept"));
    REQUIRE(accept.has_value());
    InputFrame press;
    press.held = std::uint64_t{1} << *accept;
    t.run(1, press);
    t.run(1);
    CHECK(t.game_field("playing").b);
    CHECK(t.game_field("lives").i == 3);
    t.run(120);

    // Deterministic like every game: the same run, the same state.
    Tutorial again;
    again.run(60);
    Tutorial other;
    other.run(60);
    CHECK(again.host->state_hash() == other.host->state_hash());
}
#endif

// Undocked panels (11.7b): the layout text keeps each OS window's position and size, and a
// window saved on a monitor that is gone is pulled onto one that is there.
#include <doctest.h>

#include <span>
#include <string>

#include "editor/layouts.hpp"

using namespace cuervo::editor;

namespace {

const std::string dock_text =
    "[Window][Scene]\nPos=0,56\nSize=303,844\nDockId=0x00000001,0\n\n"
    "[Window][Console]\nViewportPos=2100,140\nViewportId=0xF7BA560E\nSize=420,320\nCollapsed=0\n\n"
    "[Window][Assets]\nViewportPos=-1500,-900\nViewportId=0x1234\nSize=300,200\n\n"
    "[Docking][Data]\nDockSpace ID=0x333DDF44 Window=0xA5E3B5DE Pos=0,0 Size=1600,844 Split=X\n";

} // namespace

TEST_CASE("dock_viewports reads the position and size of undocked windows") {
    const auto views = dock_viewports(dock_text);
    REQUIRE(views.size() == 2); // docked windows have no ViewportPos
    CHECK(views[0].window == "Console");
    CHECK(views[0].rect == ScreenRect{2100, 140, 420, 320});
    CHECK(views[1].window == "Assets");
    CHECK(views[1].rect.x == -1500);
    CHECK(dock_viewports("").empty());
}

TEST_CASE("viewport positions survive a layout save and load") {
    LayoutStore store;
    Layout l;
    l.dock = dock_text;
    l.hidden = {"History"};
    std::string error;
    REQUIRE(store.save_as("Two monitors", l, error));
    LayoutStore loaded;
    REQUIRE(loaded.parse(store.serialize(), error));
    CHECK(loaded.layout("Two monitors").dock == dock_text);
    CHECK(dock_viewports(loaded.layout("Two monitors").dock) == dock_viewports(dock_text));
}

TEST_CASE("clamp_dock_viewports moves windows from a missing monitor onto a visible one") {
    const ScreenRect only{0, 0, 1920, 1080};
    const std::string fixed = clamp_dock_viewports(dock_text, std::span(&only, 1));
    const auto views = dock_viewports(fixed);
    REQUIRE(views.size() == 2);
    CHECK(views[0].rect == ScreenRect{1500, 140, 420, 320}); // pulled in, size kept
    CHECK(views[1].rect == ScreenRect{0, 0, 300, 200});
    CHECK(fixed.find("DockId=0x00000001,0") != std::string::npos); // the rest is untouched
    CHECK(fixed.find("[Docking][Data]") != std::string::npos);
}

TEST_CASE("clamp_dock_viewports leaves visible windows and the no-monitor case alone") {
    const ScreenRect both[] = {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};
    const auto views = dock_viewports(clamp_dock_viewports(dock_text, both));
    CHECK(views[0].rect == ScreenRect{2100, 140, 420, 320}); // on the second monitor: stays
    CHECK(clamp_dock_viewports(dock_text, {}) == dock_text);
    // A window with under a quarter visible moves; a quarter or more is enough to stay.
    const ScreenRect one{0, 0, 1000, 800};
    const std::string edge = "[Window][P]\nViewportPos=950,100\nSize=400,300\n";
    CHECK(dock_viewports(clamp_dock_viewports(edge, std::span(&one, 1)))[0].rect.x == 600);
    const std::string ok = "[Window][P]\nViewportPos=800,100\nSize=400,300\n";
    CHECK(dock_viewports(clamp_dock_viewports(ok, std::span(&one, 1)))[0].rect.x == 800);
}

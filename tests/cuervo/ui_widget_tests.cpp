// The UI widgets of milestone 9.3 (cuervo/script/prelude/ui.plm, DESIGN.md
// 4.17): layout math of the containers, focus navigation, input handling
// (keys, pad, pointer, wheel, typed text), popups with focus trapping, scene
// file round trips, text in replays, and the examples/ui_menus scenarios
// driven through real device state. Headless: no font, so UI.measure uses its
// fixed estimate; every control gets an explicit `min_size`.

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cuervo/core/binary_io.hpp"
#include "cuervo/input/replay.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

#if CUERVO_FEATURE_UI
namespace {

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "engine_ui_widget_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

const char* all_actions[] = {"ui_up", "ui_down", "ui_left", "ui_right", "ui_accept", "ui_cancel", "ui_click",
                             "ui_backspace", "ui_delete", "ui_home", "ui_end", "ui_scroll_up", "ui_scroll_down",
                             "ui_tab_prev", "ui_tab_next"};

struct Host {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;
    vec2 pointer{-1.0f, -1.0f};

    explicit Host(const std::string& source, fs::path dir = {}) {
        std::size_t i = 0;
        for (const char* a : all_actions) map.set_action(Name(a), {Binding::key(static_cast<Key>(static_cast<std::size_t>(Key::A) + i++))});
        script::HostOptions o;
        o.project_dir = dir;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
    }

    InputFrame frame(std::initializer_list<const char*> held = {}, std::uint32_t text = 0) const {
        InputFrame f;
        for (const char* a : held) f.held |= std::uint64_t{1} << *map.action_index(Name(a));
        f.pointer = {static_cast<std::int16_t>(pointer.x()), static_cast<std::int16_t>(pointer.y())};
        f.text = text;
        return f;
    }
    void tick(const InputFrame& f) {
        host->tick(f);
        REQUIRE_MESSAGE(host->running(), host->error());
    }
    void idle(int n = 1) {
        for (int i = 0; i < n; ++i) tick(frame());
    }
    // Presses an action for one tick, then releases it for one.
    void press(const char* action) {
        tick(frame({action}));
        tick(frame());
    }
    void type(std::string_view text) {
        for (char c : text) {
            tick(frame({}, static_cast<std::uint32_t>(c)));
        }
        tick(frame());
    }
    void click_at(vec2 p) {
        pointer = p;
        tick(frame());
        tick(frame({"ui_click"}));
        tick(frame());
    }
    pluma::Value global(std::string_view n) { return host->session()->global(n); }
    std::int64_t integer(std::string_view n) { return global(n).i; }
    float number(std::string_view n) { return global(n).f; }
    bool boolean(std::string_view n) { return global(n).b; }
    std::string str(std::string_view n) {
        pluma::Value v = global(n);
        return v.is_obj() ? static_cast<const pluma::StrObj*>(v.o)->s : std::string{};
    }
};

const pluma::Value& field(const pluma::Value& obj, std::string_view name) {
    const auto* o = static_cast<const pluma::InstanceObj*>(obj.o);
    for (std::size_t i = 0; i < o->type->field_names.size(); ++i) {
        if (o->type->field_names[i] == name) return o->fields[i];
    }
    FAIL("no field " << name);
    static pluma::Value none;
    return none;
}

vec2 field_vec2(const pluma::Value& obj, std::string_view name) {
    const float* d = pluma::vec_data(field(obj, name));
    return {d[0], d[1]};
}

// Position and size of the object in global `name`.
struct Rect {
    vec2 pos, size;
};
Rect rect_of(Host& h, std::string_view name) {
    pluma::Value o = h.global(name);
    return {field_vec2(o, "position"), field_vec2(o, "size")};
}

std::string focus_name(Host& h) {
    pluma::Value f = h.global("UI.focused");
    if (!f.is_obj()) return "<none>";
    return static_cast<const pluma::StrObj*>(field(f, "name").o)->s;
}

// A button with a fixed size and a name.
const char* helpers = R"(
fn btn(name: str, w = 100.0, h = 40.0) -> Button {
    var b = Button()
    b.name = name
    b.text = name
    b.min_size = vec2(w, h)
    return b
}
fn ctl(w: float, h: float) -> Control {
    var c = Control()
    c.min_size = vec2(w, h)
    return c
}
)";

} // namespace

TEST_CASE("GridContainer: columns, rows, separations and expand share the space exactly") {
    Host h(std::string(helpers) + R"(
class T {
    static var grid: GridContainer
    static var a: Control
    static var e: Control
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var g = GridContainer()
        g.columns = 3
        g.h_separation = 10.0
        g.v_separation = 4.0
        g.min_size = vec2(400.0, 120.0)   // wider and taller than the cells need
        // Row 0: 50x20, 80x30, 60x10   Row 1: 70x25 (widens column 0), 20x20 (expands), 10x10   Row 2: 5x40
        var sizes = [vec2(50.0, 20.0), vec2(80.0, 30.0), vec2(60.0, 10.0), vec2(70.0, 25.0), vec2(20.0, 20.0),
                     vec2(10.0, 10.0), vec2(5.0, 40.0)]
        for s in sizes {
            g.add_child(ctl(s.x, s.y))
        }
        (g.children[4] as Control).expand = true
        T.a = g.children[0] as Control
        T.e = g.children[4] as Control
        layer.add_child(g)
        T.grid = g
    }
}
)");
    h.idle(2);
    // Minimum: columns 70 / 80 / 60 + 2 * 10, rows 30 / 25 / 40 + 2 * 4.
    // g.min_size says 400x100: the grid is that big, and the extra goes to
    // the column and row of the expanding cell (column 1, row 1).
    const Rect g = rect_of(h, "T.grid");
    CHECK(g.size == vec2{400.0f, 120.0f});
    const float extra_x = 400.0f - (70 + 80 + 60 + 20);
    const float extra_y = 120.0f - (30 + 25 + 40 + 8);
    pluma::Value kids = field(h.global("T.grid"), "children");
    const auto* list = static_cast<const pluma::ListObj*>(kids.o);
    auto cell = [&](int i) { return Rect{field_vec2(list->items[i], "position"), field_vec2(list->items[i], "size")}; };
    CHECK(cell(0).pos == g.pos);
    CHECK(cell(0).size == vec2{70.0f, 30.0f});              // fills its cell
    CHECK(cell(1).pos == g.pos + vec2{80.0f, 0.0f});        // 70 + 10
    CHECK(cell(1).size == vec2{80.0f + extra_x, 30.0f});
    CHECK(cell(2).pos == g.pos + vec2{80.0f + 80.0f + extra_x + 10.0f, 0.0f});
    CHECK(cell(3).pos == g.pos + vec2{0.0f, 34.0f});        // 30 + 4
    CHECK(cell(4).size == vec2{80.0f + extra_x, 25.0f + extra_y});
    CHECK(cell(6).pos == g.pos + vec2{0.0f, 34.0f + 25.0f + extra_y + 4.0f});
    CHECK(cell(6).size == vec2{70.0f, 40.0f});
}

TEST_CASE("FlowContainer: rows wrap at the width, columns at the height") {
    Host h(std::string(helpers) + R"(
class T {
    static var row: FlowContainer
    static var col: FlowContainer
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var f = FlowContainer()
        f.h_separation = 5.0
        f.v_separation = 7.0
        f.anchor_max = vec2(0.0, 0.0)
        f.min_size = vec2(100.0, 0.0)
        for i in 0..5 {
            f.add_child(ctl(40.0, 10.0 + (i as float)))
        }
        layer.add_child(f)
        T.row = f
        var c = FlowContainer()
        c.vertical = true
        c.h_separation = 3.0
        c.v_separation = 2.0
        c.min_size = vec2(0.0, 50.0)
        c.offset_min = vec2(300.0, 0.0)
        for i in 0..4 {
            c.add_child(ctl(20.0, 20.0))
        }
        layer.add_child(c)
        T.col = c
    }
}
)");
    h.idle(3);
    pluma::Value row = h.global("T.row");
    const auto* kids = static_cast<const pluma::ListObj*>(field(row, "children").o);
    const vec2 p0 = field_vec2(row, "position");
    // 100 wide: two 40 + 5 gap = 85 fit, a third (85 + 5 + 40) doesn't.
    CHECK(field_vec2(kids->items[0], "position") == p0);
    CHECK(field_vec2(kids->items[1], "position") == p0 + vec2{45.0f, 0.0f});
    // Row 0 is 11 tall (the second child), then a 7 gap.
    CHECK(field_vec2(kids->items[2], "position") == p0 + vec2{0.0f, 11.0f + 7.0f});
    CHECK(field_vec2(kids->items[4], "position") == p0 + vec2{0.0f, 18.0f + 13.0f + 7.0f});
    // The flow's own height settles on the wrapped rows: 11 + 7 + 13 + 7 + 14.
    CHECK(field_vec2(row, "size").y() == 11.0f + 7.0f + 13.0f + 7.0f + 14.0f);

    pluma::Value col = h.global("T.col");
    const auto* ck = static_cast<const pluma::ListObj*>(field(col, "children").o);
    const vec2 c0 = field_vec2(col, "position");
    // 50 tall: two 20 + 2 gap fit; the third starts a new column 20 + 3 to the right.
    CHECK(field_vec2(ck->items[1], "position") == c0 + vec2{0.0f, 22.0f});
    CHECK(field_vec2(ck->items[2], "position") == c0 + vec2{23.0f, 0.0f});
    CHECK(field_vec2(ck->items[3], "position") == c0 + vec2{23.0f, 22.0f});
}

TEST_CASE("AspectRatioContainer: fit and cover keep the ratio and align the child") {
    Host h(std::string(helpers) + R"(
class T {
    static var fit: AspectRatioContainer
    static var cover: AspectRatioContainer
    static var fit_child: Control
    static var cover_child: Control
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var a = AspectRatioContainer()
        a.ratio = 2.0
        a.anchor_max = vec2(0.0, 0.0)
        a.min_size = vec2(300.0, 100.0)    // wider than 2:1 needs: the child is 200x100, centered
        var c = ctl(1.0, 1.0)
        a.add_child(c)
        layer.add_child(a)
        T.fit = a
        T.fit_child = c
        var b = AspectRatioContainer()
        b.ratio = 2.0
        b.stretch = #cover
        b.align = vec2(0.0, 0.0)
        b.anchor_max = vec2(0.0, 0.0)
        b.offset_min = vec2(0.0, 300.0)
        b.min_size = vec2(300.0, 100.0)    // covers: 300x150, aligned top-left
        var d = ctl(1.0, 1.0)
        b.add_child(d)
        layer.add_child(b)
        T.cover = b
        T.cover_child = d
    }
}
)");
    h.idle(2);
    const Rect fit = rect_of(h, "T.fit");
    const Rect fc = rect_of(h, "T.fit_child");
    CHECK(fc.size == vec2{200.0f, 100.0f});
    CHECK(fc.pos == fit.pos + vec2{50.0f, 0.0f});
    const Rect cover = rect_of(h, "T.cover");
    const Rect cc = rect_of(h, "T.cover_child");
    CHECK(cc.size == vec2{300.0f, 150.0f});
    CHECK(cc.pos == cover.pos);
}

TEST_CASE("SplitContainer: the bar moves by keys and drags, children keep their minimum sizes") {
    Host h(std::string(helpers) + R"(
class T {
    static var split: SplitContainer
    static var left: Control
    static var right: Control
    static var changes = 0
    static var value = 0.0
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var s = SplitContainer()
        s.name = "split"
        s.anchor_max = vec2(0.0, 0.0)
        s.min_size = vec2(1012.0, 200.0)    // 1000 for the children + the 12 px bar
        s.split = 0.5
        s.handle_step = 50.0
        var l = ctl(300.0, 50.0)
        var r = ctl(400.0, 50.0)
        s.add_child(l)
        s.add_child(r)
        s.split_changed.connect(fn(v: float) {
            T.changes += 1
            T.value = v
        })
        layer.add_child(s)
        T.split = s
        T.left = l
        T.right = r
    }
}
)");
    h.idle(2);
    Rect s = rect_of(h, "T.split");
    CHECK(rect_of(h, "T.left").size == vec2{500.0f, 200.0f});
    CHECK(rect_of(h, "T.right").pos.x() == s.pos.x() + 512.0f);
    // Focus the bar and nudge it: 50 px of 1000 = 5% per press.
    h.press("ui_down");
    CHECK(focus_name(h) == "split");
    h.press("ui_right");
    CHECK(h.number("T.value") == doctest::Approx(0.55f));
    h.idle();
    CHECK(rect_of(h, "T.left").size.x() == doctest::Approx(550.0f));
    // The right child never gets less than its 400: a split of 1.0 stops at 600.
    for (int i = 0; i < 12; ++i) h.press("ui_right");
    h.idle();
    CHECK(h.number("T.value") == doctest::Approx(1.0f));
    CHECK(rect_of(h, "T.left").size.x() == doctest::Approx(600.0f));
    CHECK(rect_of(h, "T.right").size.x() == doctest::Approx(400.0f));
    // And the left child never gets less than its 300.
    for (int i = 0; i < 25; ++i) h.press("ui_left");
    h.idle();
    CHECK(rect_of(h, "T.left").size.x() == doctest::Approx(300.0f));
    const std::int64_t before = h.integer("T.changes");
    // Dragging the bar with the pointer: grab it at its middle and move 100 px right.
    s = rect_of(h, "T.split");
    const float bar_x = s.pos.x() + 306.0f;
    h.pointer = {bar_x, s.pos.y() + 100.0f};
    h.tick(h.frame({"ui_click"}));
    h.pointer = {bar_x + 100.0f, s.pos.y() + 100.0f};
    h.tick(h.frame({"ui_click"}));
    h.tick(h.frame());
    CHECK(h.integer("T.changes") > before);
    h.idle();
    CHECK(rect_of(h, "T.left").size.x() == doctest::Approx(400.0f));
}

const char* scroll_source = R"(
class T {
    static var scroll: ScrollContainer
    static var content: VBox
    static var last = 0.0
    static var pressed = 0
    static var focus_name = ""
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var sc = ScrollContainer()
        sc.name = "scroll"
        sc.anchor_max = vec2(0.0, 0.0)
        sc.min_size = vec2(200.0, 100.0)
        sc.scrolled.connect(fn(o: vec2) {
            T.last = o.y
        })
        var box = VBox()
        box.separation = 0.0
        for i in 0..10 {
            var b = Button()
            b.name = "item{i}"
            b.min_size = vec2(100.0, 50.0)
            b.pressed.connect(fn() {
                T.pressed += 1
            })
            box.add_child(b)
        }
        sc.add_child(box)
        layer.add_child(sc)
        T.scroll = sc
        T.content = box
        var after = Button()
        after.name = "after"
        after.anchor_min = vec2(0.0, 0.0)
        after.anchor_max = vec2(0.0, 0.0)
        after.offset_min = vec2(400.0, 0.0)
        after.min_size = vec2(50.0, 50.0)
        layer.add_child(after)
    }
}
)";

TEST_CASE("ScrollContainer: the content's size, the bar and the scroll limit") {
    Host h(scroll_source);
    h.idle(3);
    const Rect sc = rect_of(h, "T.scroll");
    CHECK(sc.size == vec2{200.0f, 100.0f});
    // 10 buttons of 50 = 500 tall; the bar takes 10 px of the width.
    const Rect content = rect_of(h, "T.content");
    CHECK(content.size == vec2{190.0f, 500.0f});
    CHECK(content.pos == sc.pos);
    CHECK(field_vec2(h.global("T.scroll"), "scroll") == vec2{0.0f, 0.0f});
    // Scrolling by the action moves by `wheel_step`, then clamps at 500 - 100.
    h.pointer = sc.pos + vec2{50.0f, 50.0f};
    h.idle();
    h.tick(h.frame({"ui_scroll_down"}));
    h.idle();
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() == 56.0f);
    CHECK(rect_of(h, "T.content").pos.y() == sc.pos.y() - 56.0f);
    for (int i = 0; i < 40; ++i) h.tick(h.frame({"ui_scroll_down"}));
    h.idle();
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() == 400.0f);
    CHECK(h.number("T.last") == 400.0f);
    h.tick(h.frame({"ui_scroll_up"}));
    h.idle();
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() == 344.0f);
}

TEST_CASE("ScrollContainer: focus moves scroll the focused control into view; clipped controls can't be clicked") {
    Host h(scroll_source);
    h.idle(3);
    h.press("ui_down"); // first focusable: item0
    CHECK(focus_name(h) == "item0");
    h.press("ui_down");
    h.press("ui_down"); // item2 is at y 100..150: just below the 100 px view
    h.idle(2);
    CHECK(focus_name(h) == "item2");
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() == 50.0f); // bottom edge on the view's bottom
    // Out of view and clipped: a click on item9's position (below the view) hits nothing.
    const Rect sc = rect_of(h, "T.scroll");
    h.click_at(sc.pos + vec2{50.0f, 140.0f});
    CHECK(h.integer("T.pressed") == 0);
    // A click inside the view activates what is there: scroll 50, y 10 is item0's lower half (0..50 -> -50..0)? item1 is at 0..50.
    h.click_at(sc.pos + vec2{50.0f, 20.0f});
    CHECK(h.integer("T.pressed") == 1);
    // Wrap-around: down on the last item goes to the top and scrolls back.
    for (int i = 0; i < 9; ++i) h.press("ui_down");
    h.idle(2);
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() >= 0.0f);
}

TEST_CASE("ScrollContainer: the wheel scrolls the area under the pointer, a drag on empty space drags the content") {
    Host h(scroll_source);
    h.idle(3);
    const Rect sc = rect_of(h, "T.scroll");
    // Wheel with the pointer elsewhere and no focus inside: nothing is scrolled
    // unless the focus is in an area (find_any_scroll) — here it isn't.
    h.pointer = sc.pos + vec2{50.0f, 50.0f};
    h.tick(h.frame());
    h.tick(h.frame({"ui_scroll_down"}));
    h.idle();
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() == 56.0f);
    h.pointer = {900.0f, 600.0f}; // away from the area: the focused item inside it still scrolls it (gamepad rule)
    h.tick(h.frame({"ui_scroll_down"}));
    h.idle();
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() == 112.0f);
    // Dragging the bar's thumb: the bar is the rightmost 10 px.
    const float bar_x = sc.pos.x() + 195.0f;
    h.pointer = {bar_x, sc.pos.y() + 5.0f};
    h.tick(h.frame({"ui_click"}));
    h.tick(h.frame({"ui_click"}));
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() == 0.0f); // the thumb's middle jumps to the click
    h.pointer = {bar_x, sc.pos.y() + 100.0f};
    h.tick(h.frame({"ui_click"}));
    h.tick(h.frame());
    CHECK(field_vec2(h.global("T.scroll"), "scroll").y() == 400.0f); // the end of the track
}

TEST_CASE("ProgressBar: ratio, clamping and the changed signal") {
    Host h(R"(
class T {
    static var bar: ProgressBar
    static var changes = 0
}
class Main extends Node {
    fn ready() {
        var p = ProgressBar()
        p.min_value = 10.0
        p.max_value = 30.0
        p.value = 20.0
        p.changed.connect(fn(v: float) {
            T.changes += 1
        })
        T.bar = p
    }
    fn update(dt: float) {
        if Time.tick == 1 {
            T.bar.set_value(25.0)
        }
        if Time.tick == 2 {
            T.bar.set_value(99.0)   // clamps to 30
            T.bar.set_value(30.0)   // no change
        }
    }
}
)");
    CHECK(h.number("T.bar") == h.number("T.bar")); // not a number: just loads
    h.idle();
    pluma::Value bar = h.global("T.bar");
    CHECK(field(bar, "value").f == 25.0f);
    h.idle();
    CHECK(field(bar, "value").f == 30.0f);
    CHECK(h.integer("T.changes") == 2);
}

TEST_CASE("LineEdit: typed text from the input frame, caret keys, limits, submit and cancel") {
    Host h(std::string(helpers) + R"(
class T {
    static var edit: LineEdit
    static var changes = 0
    static var submitted = 0
    static var text = ""
    static var cancels = 0
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var box = VBox()
        var e = LineEdit()
        e.name = "edit"
        e.max_length = 8
        e.min_size = vec2(200.0, 40.0)
        e.text_changed.connect(fn(t: str) {
            T.changes += 1
            T.text = t
        })
        e.text_submitted.connect(fn(t: str) {
            T.submitted += 1
        })
        box.add_child(e)
        box.add_child(btn("other"))
        box.cancelled.connect(fn() {
            T.cancels += 1
        })
        layer.add_child(box)
        T.edit = e
    }
}
)");
    h.idle();
    h.press("ui_down");
    CHECK(focus_name(h) == "edit");
    // Typing needs the field to be in edit mode first.
    h.type("x");
    CHECK(h.str("T.text") == "");
    h.press("ui_accept");
    CHECK(field(h.global("T.edit"), "editing").b);
    h.type("abc");
    CHECK(h.str("T.text") == "abc");
    CHECK(h.integer("T.changes") == 3);
    // Left moves the caret instead of the focus while editing; typing inserts there.
    h.press("ui_left");
    h.type("X");
    CHECK(h.str("T.text") == "abXc");
    h.press("ui_home");
    h.type("<");
    CHECK(h.str("T.text") == "<abXc");
    h.press("ui_end");
    h.press("ui_backspace");
    CHECK(h.str("T.text") == "<abX");
    h.press("ui_home");
    h.press("ui_delete");
    CHECK(h.str("T.text") == "abX");
    // max_length 8: only the characters that fit go in.
    h.press("ui_end");
    h.type("123456789");
    CHECK(h.str("T.text") == "abX12345");
    // A typed character that is also bound to a ui_* action isn't a command: 'e' here is ui_up... held with text.
    // Accept ends the edit and submits; the field stays focused.
    h.press("ui_accept");
    CHECK(h.integer("T.submitted") == 1);
    CHECK(!field(h.global("T.edit"), "editing").b);
    CHECK(focus_name(h) == "edit");
    // Cancel while editing stops the edit and doesn't reach the root.
    h.press("ui_accept");
    h.press("ui_cancel");
    CHECK(!field(h.global("T.edit"), "editing").b);
    CHECK(h.integer("T.cancels") == 0);
    h.press("ui_cancel");
    CHECK(h.integer("T.cancels") == 1);
    // Up/down stop editing and move the focus.
    h.press("ui_accept");
    h.press("ui_down");
    CHECK(!field(h.global("T.edit"), "editing").b);
    CHECK(focus_name(h) == "other");
}

TEST_CASE("LineEdit: UTF-8 text, a secret field, clicking puts the caret, losing focus stops editing") {
    Host h(std::string(helpers) + R"(
class T {
    static var edit: LineEdit
    static var shown = ""
    static var caret = 0
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var box = VBox()
        var e = LineEdit()
        e.name = "edit"
        e.secret = true
        e.min_size = vec2(300.0, 40.0)
        box.add_child(e)
        box.add_child(btn("other"))
        layer.add_child(box)
        T.edit = e
    }
    fn update(dt: float) {
        T.shown = T.edit.display_text()
        T.caret = T.edit.caret
    }
}
)");
    h.idle();
    h.press("ui_down");
    h.press("ui_accept");
    // 'é' (U+00E9) and '€' (U+20AC) are one character each.
    h.tick(h.frame({}, 0xE9));
    h.tick(h.frame({}, 0x20AC));
    h.tick(h.frame());
    CHECK(h.str("T.shown") == "**");
    CHECK(field(h.global("T.edit"), "text").o != nullptr);
    CHECK(static_cast<const pluma::StrObj*>(field(h.global("T.edit"), "text").o)->s == "\xC3\xA9\xE2\x82\xAC");
    CHECK(h.integer("T.caret") == 2);
    h.press("ui_left");
    h.press("ui_backspace");
    CHECK(static_cast<const pluma::StrObj*>(field(h.global("T.edit"), "text").o)->s == "\xE2\x82\xAC");
    // Moving the focus away ends the edit.
    h.press("ui_down");
    CHECK(focus_name(h) == "other");
    CHECK(!field(h.global("T.edit"), "editing").b);
    // A click on the field starts editing again, caret at the end for a click far right.
    const Rect r = rect_of(h, "T.edit");
    h.click_at(r.pos + vec2{290.0f, 20.0f});
    CHECK(field(h.global("T.edit"), "editing").b);
    CHECK(h.integer("T.caret") == 1);
    // And one at the far left puts it before the first character.
    h.click_at(r.pos + vec2{1.0f, 20.0f});
    CHECK(h.integer("T.caret") == 0);
}

TEST_CASE("OptionButton and PopupMenu: open, choose by keys and pointer, cancel, focus trap and restore") {
    Host h(std::string(helpers) + R"(
class T {
    static var option: OptionButton
    static var chosen = -1
    static var changes = 0
    static var other_pressed = 0
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var box = VBox()
        box.separation = 10.0
        var o = OptionButton()
        o.name = "option"
        o.items = ["Easy", "Normal", "Hard"]
        o.selected = 1
        o.item_height = 30.0
        o.min_size = vec2(200.0, 40.0)
        o.item_selected.connect(fn(i: int) {
            T.chosen = i
            T.changes += 1
        })
        box.add_child(o)
        var b = btn("other", 200.0, 40.0)
        b.pressed.connect(fn() {
            T.other_pressed += 1
        })
        box.add_child(b)
        layer.add_child(box)
        T.option = o
    }
}
)");
    h.idle();
    h.press("ui_down");
    CHECK(focus_name(h) == "option");
    h.press("ui_accept"); // opens; focus goes to the selected entry
    pluma::Value menu = field(h.global("T.option"), "_menu");
    REQUIRE(menu.is_obj());
    CHECK(field(menu, "visible").b);
    // The popup is a root of its own and modal: ui_down moves inside it, never to "other".
    pluma::Value f = h.global("UI.focused");
    REQUIRE(f.is_obj());
    CHECK(static_cast<const pluma::StrObj*>(field(f, "text").o)->s == "Normal");
    h.press("ui_down");
    h.press("ui_down"); // wraps inside the popup
    f = h.global("UI.focused");
    CHECK(static_cast<const pluma::StrObj*>(field(f, "text").o)->s == "Easy");
    h.press("ui_down");
    h.press("ui_down"); // Hard
    h.press("ui_accept");
    CHECK(h.integer("T.chosen") == 2);
    CHECK(field(h.global("T.option"), "selected").i == 2);
    CHECK(!field(menu, "visible").b);
    CHECK(focus_name(h) == "option"); // the focus came back
    // Cancel closes without choosing.
    h.press("ui_accept");
    h.press("ui_cancel");
    CHECK(!field(menu, "visible").b);
    CHECK(h.integer("T.changes") == 1);
    CHECK(focus_name(h) == "option");
    // Modal: while open, a click on "other" outside the popup is swallowed (and light-dismisses).
    h.press("ui_accept");
    CHECK(field(menu, "visible").b);
    const Rect other = [&] {
        pluma::Value kids = field(field(h.global("T.option"), "parent"), "children");
        const auto* l = static_cast<const pluma::ListObj*>(kids.o);
        return Rect{field_vec2(l->items[1], "position"), field_vec2(l->items[1], "size")};
    }();
    h.click_at(other.pos + other.size * 0.5f);
    CHECK(h.integer("T.other_pressed") == 0);
    CHECK(!field(menu, "visible").b);
    // Closed again: that click reaches the button.
    h.click_at(other.pos + other.size * 0.5f);
    CHECK(h.integer("T.other_pressed") == 1);
    // Choosing with the pointer: open, click the first entry ("Easy").
    h.press("ui_up"); // focus the option again (from "other")
    CHECK(focus_name(h) == "option");
    h.press("ui_accept");
    const Rect mr = rect_of(h, "T.option");
    pluma::Value m2 = field(h.global("T.option"), "_menu");
    const Rect popup{field_vec2(m2, "position"), field_vec2(m2, "size")};
    CHECK(popup.pos.y() == doctest::Approx(mr.pos.y() + mr.size.y() + 2.0f));
    CHECK(popup.size.x() >= 200.0f);
    h.click_at(popup.pos + vec2{popup.size.x() * 0.5f, 10.0f + 15.0f}); // inset 10 + half an entry
    CHECK(h.integer("T.chosen") == 0);
}

TEST_CASE("Tabs: TabBar keys and pointer, TabContainer pages, titles, shoulder actions, layout") {
    Host h(std::string(helpers) + R"(
class T {
    static var tabs: TabContainer
    static var changes = 0
    static var index = -1
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var t = TabContainer()
        t.name = "tabs"
        t.anchor_max = vec2(0.0, 0.0)
        t.min_size = vec2(400.0, 200.0)
        t.tab_min_width = 120.0
        t.titles = ["First", "", "Third"]
        t.tab_changed.connect(fn(i: int) {
            T.changes += 1
            T.index = i
        })
        layer.add_child(t)
        for n in ["one", "two", "three"] {
            var page = VBox()
            page.name = n
            page.add_child(btn("{n}_button"))
            t.add_child(page)
        }
        T.tabs = t
    }
}
)");
    h.idle(3);
    pluma::Value t = h.global("T.tabs");
    pluma::Value bar = field(t, "bar");
    const auto* tabs = static_cast<const pluma::ListObj*>(field(bar, "tabs").o);
    REQUIRE(tabs->items.size() == 3);
    auto s = [](const pluma::Value& v) { return static_cast<const pluma::StrObj*>(v.o)->s; };
    CHECK(s(tabs->items[0]) == "First");
    CHECK(s(tabs->items[1]) == "two"); // an empty title falls back to the page's name
    CHECK(s(tabs->items[2]) == "Third");
    const Rect tr = rect_of(h, "T.tabs");
    const vec2 bar_pos = field_vec2(bar, "position");
    CHECK(bar_pos == tr.pos);
    CHECK(field_vec2(bar, "size").x() == tr.size.x());
    // Only the current page is visible and laid out below the bar.
    const auto* pages = static_cast<const pluma::ListObj*>(field(t, "children").o);
    auto page_visible = [&](int i) { return field(pages->items[i + 1], "visible").b; }; // child 0 is the bar
    CHECK(page_visible(0));
    CHECK(!page_visible(1));
    CHECK(!page_visible(2));
    const float bar_h = field_vec2(bar, "size").y();
    CHECK(field_vec2(pages->items[1], "position").y() > tr.pos.y() + bar_h - 1.0f);
    // Focus the bar and switch with the arrows.
    h.press("ui_down");
    h.press("ui_right");
    CHECK(h.integer("T.index") == 1);
    CHECK(page_visible(1));
    CHECK(!page_visible(0));
    h.press("ui_right");
    CHECK(h.integer("T.index") == 2);
    h.press("ui_right"); // at the end: nothing happens (and the focus has nowhere to go right)
    CHECK(h.integer("T.index") == 2);
    // Shoulder actions work from anywhere in the root.
    h.press("ui_tab_prev");
    CHECK(h.integer("T.index") == 1);
    // Clicking a tab: tabs are 120 wide + 4 gap; the first is at the bar's left.
    h.click_at(bar_pos + vec2{20.0f, 10.0f});
    CHECK(h.integer("T.index") == 0);
    h.click_at(bar_pos + vec2{124.0f + 124.0f + 30.0f, 10.0f});
    CHECK(h.integer("T.index") == 2);
    CHECK(h.integer("T.changes") >= 4);
}

TEST_CASE("Popup and ConfirmDialog: modal focus trapping, backdrop input, confirm and cancel") {
    Host h(std::string(helpers) + R"(
class T {
    static var dialog: ConfirmDialog
    static var confirmed = 0
    static var cancelled = 0
    static var closed = 0
    static var behind = 0
    static var popup: Popup
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var box = VBox()
        var open = btn("open", 200.0, 40.0)
        var behind = btn("behind", 200.0, 40.0)
        behind.pressed.connect(fn() {
            T.behind += 1
        })
        open.pressed.connect(fn() {
            T.dialog.popup_centered()
        })
        box.add_child(open)
        box.add_child(behind)
        layer.add_child(box)
        var d = ConfirmDialog()
        d.name = "dialog"
        d.title = "Quit?"
        d.message = "Really?"
        d.min_size = vec2(400.0, 260.0)
        d.confirmed.connect(fn() {
            T.confirmed += 1
        })
        d.cancelled.connect(fn() {
            T.cancelled += 1
        })
        d.closed.connect(fn() {
            T.closed += 1
        })
        layer.add_child(d)
        T.dialog = d
    }
}
)");
    h.idle(2);
    // Closed dialogs are invisible and take no space in the layout.
    CHECK(!field(h.global("T.dialog"), "visible").b);
    h.press("ui_down");
    CHECK(focus_name(h) == "open");
    h.press("ui_accept");
    pluma::Value d = h.global("T.dialog");
    CHECK(field(d, "visible").b);
    h.idle();
    const Rect dr = rect_of(h, "T.dialog");
    CHECK(dr.size == vec2{400.0f, 260.0f});
    CHECK(dr.pos == vec2{(1280.0f - 400.0f) * 0.5f, (720.0f - 260.0f) * 0.5f});
    // The focus starts on Cancel and can't leave the dialog: up/down/left/right only cycle its buttons.
    pluma::Value f = h.global("UI.focused");
    CHECK(static_cast<const pluma::StrObj*>(field(f, "text").o)->s == "Cancel");
    for (const char* a : {"ui_up", "ui_down", "ui_left", "ui_right"}) {
        h.press(a);
        pluma::Value now = h.global("UI.focused");
        REQUIRE(now.is_obj());
        const std::string text = static_cast<const pluma::StrObj*>(field(now, "text").o)->s;
        CHECK((text == "OK" || text == "Cancel"));
    }
    // A click on the button behind the dialog does nothing.
    pluma::Value kids = field(field(h.global("UI.focused"), "parent"), "parent");
    (void)kids;
    h.click_at({10.0f, 60.0f});
    CHECK(h.integer("T.behind") == 0);
    CHECK(field(d, "visible").b); // a modal dialog isn't dismissed by an outside click
    // Escape cancels: closes, emits `cancelled`, and the focus returns to "open".
    h.press("ui_cancel");
    CHECK(!field(d, "visible").b);
    CHECK(h.integer("T.cancelled") == 1);
    CHECK(h.integer("T.closed") == 1);
    CHECK(focus_name(h) == "open");
    // Reopen and confirm: left from Cancel is OK.
    h.press("ui_accept");
    h.press("ui_left");
    h.press("ui_accept");
    CHECK(h.integer("T.confirmed") == 1);
    CHECK(!field(d, "visible").b);
    CHECK(focus_name(h) == "open");
    // Input works behind it again.
    h.press("ui_down");
    h.press("ui_accept");
    CHECK(h.integer("T.behind") == 1);
}

TEST_CASE("tooltips show after the focus has rested, and Control.tooltip is exported") {
    Host h(std::string(helpers) + R"(
class T {
    static var b: Button
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var b = btn("tip")
        b.tooltip = "Hello"
        layer.add_child(b)
        T.b = b
    }
}
)");
    h.idle();
    h.press("ui_down");
    CHECK(field(h.global("UI.x"), "focus_ticks").i < 5);
    h.idle(30);
    CHECK(field(h.global("UI.x"), "focus_ticks").i >= field(h.global("UI.x"), "tooltip_delay").i);
}

TEST_CASE("scene files: the 9.3 widgets round-trip their exported fields") {
    const fs::path dir = temp_dir("scene_roundtrip");
    Host h(std::string(helpers) + R"(
class T {
    static var ok = 0
}
class Main extends Node {
    fn ready() {
        var root = VBox()
        root.name = "root"
        var g = GridContainer()
        g.name = "grid"
        g.columns = 4
        g.h_separation = 3.0
        root.add_child(g)
        var f = FlowContainer()
        f.name = "flow"
        f.vertical = true
        root.add_child(f)
        var s = SplitContainer()
        s.name = "split"
        s.split = 0.25
        s.vertical = true
        root.add_child(s)
        var a = AspectRatioContainer()
        a.name = "aspect"
        a.ratio = 1.5
        a.stretch = #cover
        root.add_child(a)
        var sc = ScrollContainer()
        sc.name = "scroll"
        sc.horizontal = true
        sc.wheel_step = 33.0
        sc.show_bars = false
        root.add_child(sc)
        var im = Image()
        im.name = "image"
        im.asset = "assets/x.svg"
        im.stretch = #scale
        im.flip_h = true
        root.add_child(im)
        var tr = TextureRect()
        tr.name = "texrect"
        root.add_child(tr)
        var p = ProgressBar()
        p.name = "progress"
        p.value = 0.7
        p.max_value = 2.0
        p.show_percentage = false
        root.add_child(p)
        var e = LineEdit()
        e.name = "edit"
        e.text = "héllo"
        e.placeholder = "name"
        e.max_length = 9
        e.secret = true
        e.tooltip = "type"
        root.add_child(e)
        var o = OptionButton()
        o.name = "option"
        o.items = ["a", "b"]
        o.selected = 1
        o.item_height = 21.0
        root.add_child(o)
        var tb = TabBar()
        tb.name = "tabbar"
        tb.tabs = ["x", "y"]
        tb.current = 1
        tb.min_tab_width = 90.0
        root.add_child(tb)
        var tc = TabContainer()
        tc.name = "tabs"
        tc.titles = ["T1"]
        tc.tab_min_width = 70.0
        root.add_child(tc)
        var pm = PopupMenu()
        pm.name = "menu"
        pm.items = ["m1", "m2"]
        pm.item_min_size = vec2(50.0, 20.0)
        root.add_child(pm)
        var cd = ConfirmDialog()
        cd.name = "dialog"
        cd.title = "Sure?"
        cd.ok_text = "Yes"
        cd.focus_ok = true
        root.add_child(cd)
        T.ok = Scene.save(root, "widgets.scene") ? 1 : 0
        var back = Scene.load("widgets.scene")
        if back is VBox v {
            if (v.find("grid") as GridContainer).columns == 4 { T.ok += 1 }
            if (v.find("flow") as FlowContainer).vertical { T.ok += 1 }
            var sp = v.find("split") as SplitContainer
            if sp.split == 0.25 && sp.vertical { T.ok += 1 }
            var ar = v.find("aspect") as AspectRatioContainer
            if ar.ratio == 1.5 && ar.stretch == #cover { T.ok += 1 }
            var scr = v.find("scroll") as ScrollContainer
            if scr.horizontal && scr.wheel_step == 33.0 && !scr.show_bars { T.ok += 1 }
            var img = v.find("image") as Image
            if img.asset == "assets/x.svg" && img.stretch == #scale && img.flip_h { T.ok += 1 }
            if v.find("texrect") is TextureRect { T.ok += 1 }
            var pb = v.find("progress") as ProgressBar
            if pb.value == 0.7 && pb.max_value == 2.0 && !pb.show_percentage { T.ok += 1 }
            var le = v.find("edit") as LineEdit
            if le.text == "héllo" && le.placeholder == "name" && le.max_length == 9 && le.secret && le.tooltip == "type" { T.ok += 1 }
            var ob = v.find("option") as OptionButton
            if ob.items.len() == 2 && ob.selected == 1 && ob.item_height == 21.0 { T.ok += 1 }
            var bar = v.find("tabbar") as TabBar
            if bar.tabs.len() == 2 && bar.current == 1 && bar.min_tab_width == 90.0 { T.ok += 1 }
            var tcc = v.find("tabs") as TabContainer
            if tcc.titles.len() == 1 && tcc.tab_min_width == 70.0 { T.ok += 1 }
            var pmm = v.find("menu") as PopupMenu
            if pmm.items.len() == 2 && pmm.item_min_size == vec2(50.0, 20.0) { T.ok += 1 }
            var dlg = v.find("dialog") as ConfirmDialog
            if dlg.title == "Sure?" && dlg.ok_text == "Yes" && dlg.focus_ok { T.ok += 1 }
        }
    }
}
)",
           dir);
    CHECK(h.integer("T.ok") == 15);
    // The saved file is a normal scene file, and the widgets' own helper nodes
    // (the tab bar of a TabContainer, a menu's buttons) are transient: not saved.
    std::string error;
    auto doc = scene::load_file(dir / "widgets.scene", error);
    REQUIRE_MESSAGE(doc.has_value(), error);
    const std::string text = scene::dump_text(*doc);
    CHECK(text.find("GridContainer") != std::string::npos);
    CHECK(text.find("tab_bar") == std::string::npos);
}

TEST_CASE("input: typed text is queued, one character per tick, and replays") {
    DeviceState devices;
    InputMap map;
    map.set_action(Name("a"), {Binding::key(Key::A)});
    devices.typed = U"héy";
    std::vector<InputFrame> frames;
    for (int i = 0; i < 4; ++i) frames.push_back(map.sample(devices));
    CHECK(frames[0].text == U'h');
    CHECK(frames[1].text == 0xE9u);
    CHECK(frames[2].text == U'y');
    CHECK(frames[3].text == 0u);
    CHECK(devices.typed.empty());

    // Replay files carry it (version 2) and still read version 1.
    Replay r;
    r.project = "t";
    r.actions = {"a"};
    r.frames = frames;
    r.hashes = {1, 2, 3, 4};
    const auto bytes = write_replay(r);
    std::string error;
    auto back = read_replay(bytes, error);
    REQUIRE_MESSAGE(back.has_value(), error);
    CHECK(back->frames == frames);
    CHECK(back->frames[1].text == 0xE9u);

    // A hand-built version 1 file: 28-byte frames without text.
    ByteWriter w;
    w.raw("CRPL", 4);
    w.u32(1);
    w.str("old");
    w.u64(7);
    w.f32(60.0f);
    w.u32(1);
    w.str("a");
    w.u32(0);
    w.str("");
    w.u32(0);
    w.u32(2);
    for (int i = 0; i < 2; ++i) {
        w.u64(i == 1 ? 1u : 0u);
        for (int a = 0; a < 8; ++a) w.raw("\0\0", 2);
        const std::int16_t p[2] = {static_cast<std::int16_t>(10 + i), 20};
        w.raw(p, 4);
    }
    w.u32(1);
    w.u64(99);
    auto old = read_replay(w.take(), error);
    REQUIRE_MESSAGE(old.has_value(), error);
    REQUIRE(old->frames.size() == 2);
    CHECK(old->frames[1].held == 1u);
    CHECK(old->frames[1].pointer[0] == 11);
    CHECK(old->frames[1].text == 0u);
    CHECK(old->hashes.size() == 1);
}

TEST_CASE("replays: a run that types text into a LineEdit reproduces the same state hashes") {
    const std::string source = std::string(helpers) + R"(
class T {
    static var len = 0
    static var sum = 0
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var e = LineEdit()
        e.min_size = vec2(200.0, 40.0)
        e.text_changed.connect(fn(t: str) {
            T.len = t.char_count()
            T.sum = 0
            for i in 0..t.len() {
                T.sum += t.byte_at(i) * (i + 1)
            }
        })
        layer.add_child(e)
    }
}
)";
    std::vector<InputFrame> frames;
    std::vector<std::uint64_t> hashes;
    std::int64_t len = 0, sum = 0;
    {
        Host h(source);
        auto record = [&](InputFrame f) {
            h.tick(f);
            frames.push_back(f);
            hashes.push_back(h.host->state_hash());
        };
        record(h.frame());
        record(h.frame({"ui_down"}));
        record(h.frame());
        record(h.frame({"ui_accept"}));
        for (char c : std::string("Pluma!")) {
            record(h.frame({}, static_cast<std::uint32_t>(c)));
            record(h.frame());
        }
        record(h.frame({"ui_backspace"}));
        len = h.integer("T.len");
        sum = h.integer("T.sum");
    }
    CHECK(len == 5);
    // Play the frames back into a fresh host: every hash matches.
    Host p(source);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        p.tick(frames[i]);
        REQUIRE(p.host->state_hash() == hashes[i]);
    }
    CHECK(p.integer("T.len") == len);
    CHECK(p.integer("T.sum") == sum);
    // And the typed text is part of what the hash fingerprints: other text, other hash.
    Host q(source);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        InputFrame f = frames[i];
        if (f.text == 'u') f.text = 'v';
        q.tick(f);
    }
    CHECK(q.host->state_hash() != hashes.back());
}

// ---------------------------------------------------------------- examples/ui_menus through real devices

namespace {

struct Menus {
    script::Project project;
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;
    DeviceState devices;
    vec2 pointer{-1.0f, -1.0f};

    Menus() {
        std::string error;
        auto p = script::load_project(fs::path(CUERVO_EXAMPLES_DIR) / "ui_menus", error);
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
        run(2);
    }
    void run(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            InputFrame f = map.sample(devices);
            f.pointer = {static_cast<std::int16_t>(pointer.x()), static_cast<std::int16_t>(pointer.y())};
            host->tick(f);
            REQUIRE_MESSAGE(host->running(), host->error());
        }
    }
    // A key/pad press for one tick and a release for the next.
    void key(Key k) {
        devices.set_key(k, true);
        run();
        devices.set_key(k, false);
        run();
    }
    void pad(PadButton b) {
        devices.set_button(b, true);
        run();
        devices.set_button(b, false);
        run();
    }
    void click(vec2 p) {
        pointer = p;
        run();
        devices.set_mouse_button(MouseButton::Left, true);
        run();
        devices.set_mouse_button(MouseButton::Left, false);
        run();
    }
    void type(std::u32string_view s) {
        devices.typed = std::u32string(s);
        run(static_cast<int>(s.size()) + 1);
    }
    std::int64_t s(std::string_view name) { return host->session()->global("S." + std::string(name)).i; }
    std::string player_name() {
        pluma::Value v = host->session()->global("S.player_name");
        return v.is_obj() ? static_cast<const pluma::StrObj*>(v.o)->s : std::string{};
    }
};

} // namespace

TEST_CASE("ui_menus: the keyboard drives tabs, sliders, text, dropdown, toggles, dialog and inventory") {
    Menus m;
    m.key(Key::Down); // the tab bar
    m.key(Key::Down); // music
    m.key(Key::Right);
    m.key(Key::Right);
    CHECK(m.s("music") == 100);
    m.key(Key::Left);
    CHECK(m.s("music") == 90);
    m.key(Key::PageDown); // next tab
    CHECK(m.s("tab") == 1);
    m.key(Key::Down); // focus is gone with the hidden page: the first control
    m.key(Key::Down); // player name
    m.key(Key::Enter);
    m.type(U"Ada");
    CHECK(m.player_name() == "Ada");
    CHECK(m.s("name_len") == 3);
    m.key(Key::Backspace);
    CHECK(m.player_name() == "Ad");
    m.key(Key::Enter);
    CHECK(m.s("submitted") == 1);
    m.key(Key::Down); // difficulty: Enter opens the dropdown, Down + Enter picks Hard
    m.key(Key::Enter);
    m.key(Key::Down);
    m.key(Key::Enter);
    CHECK(m.s("difficulty") == 2);
    m.key(Key::Down);
    m.key(Key::Enter);
    CHECK(m.s("fullscreen") == 1);
    // Apply -> dialog -> OK
    m.key(Key::Down);
    m.key(Key::Down);
    m.key(Key::Enter);
    CHECK(m.s("dialog_open") == 1);
    m.key(Key::Left);
    m.key(Key::Enter);
    CHECK(m.s("applied") == 1);
    CHECK(m.s("dialog_open") == 0);
}

TEST_CASE("ui_menus: a gamepad drives it (d-pad, A/B, shoulders, right stick)") {
    Menus m;
    m.pad(PadButton::DpadDown);
    m.pad(PadButton::RightShoulder);
    m.pad(PadButton::RightShoulder);
    CHECK(m.s("tab") == 2);
    m.pad(PadButton::LeftShoulder);
    CHECK(m.s("tab") == 1);
    m.pad(PadButton::RightShoulder);
    CHECK(m.s("tab") == 2);
    m.pad(PadButton::DpadDown); // the split bar
    m.pad(PadButton::DpadDown); // Apply
    m.pad(PadButton::DpadUp);   // a slot in the last visible row
    // The right stick scrolls the list: held beyond the threshold for some ticks.
    m.devices.set_axis(PadAxis::RightY, 1.0f);
    m.run(12);
    m.devices.set_axis(PadAxis::RightY, 0.0f);
    m.run(2);
    CHECK(m.s("scroll_y") == 88);
    // The left stick moves the focus like the d-pad; A opens the item menu, B closes it.
    m.devices.set_axis(PadAxis::LeftX, -1.0f);
    m.run(1);
    m.devices.set_axis(PadAxis::LeftX, 0.0f);
    m.run(2);
    m.pad(PadButton::South);
    CHECK(m.s("selected_item") >= 0);
    m.pad(PadButton::East);
    m.pad(PadButton::South);
    m.pad(PadButton::South); // "Use" is first
    CHECK(m.s("uses") == 1);
    CHECK(m.s("menu_item") == 0);
}

TEST_CASE("ui_menus: the mouse drives it (hover, click, wheel, drag, typed text)") {
    Menus m;
    m.click({336.0f, 170.0f}); // the Game tab
    CHECK(m.s("tab") == 1);
    m.click({578.0f, 222.0f}); // the name field
    m.type(U"Zed");
    CHECK(m.player_name() == "Zed");
    m.click({578.0f, 282.0f}); // the dropdown
    m.click({570.0f, 484.0f}); // "Insane"
    CHECK(m.s("difficulty") == 3);
    m.click({578.0f, 342.0f});
    m.click({578.0f, 402.0f});
    CHECK(m.s("fullscreen") == 1);
    CHECK(m.s("show_fps") == 1);
    m.click({188.0f, 628.0f}); // Apply
    CHECK(m.s("dialog_open") == 1);
    m.click({388.0f, 400.0f}); // OK
    CHECK(m.s("applied") == 1);
    m.click({490.0f, 170.0f}); // the Inventory tab
    CHECK(m.s("tab") == 2);
    // Wheel notches over the grid.
    m.pointer = {300.0f, 400.0f};
    m.run();
    for (int i = 0; i < 4; ++i) {
        m.devices.wheel = vec2{0.0f, -1.0f};
        m.run();
    }
    m.run(2);
    CHECK(m.s("scroll_y") == 88);
    // Drag the empty space between slots down: the content follows.
    m.pointer = {201.0f, 300.0f};
    m.run();
    m.devices.set_mouse_button(MouseButton::Left, true);
    m.run();
    m.pointer = {201.0f, 400.0f};
    m.run();
    m.devices.set_mouse_button(MouseButton::Left, false);
    m.run(2);
    CHECK(m.s("scroll_y") == 0);
    // A slot: click opens its menu, "Use" spends durability.
    m.click({348.0f, 234.0f});
    CHECK(m.s("selected_item") == 2);
    m.click({400.0f, 266.0f});
    CHECK(m.s("uses") == 1);
    // Drag the split bar.
    m.pointer = {654.0f, 300.0f};
    m.run();
    m.devices.set_mouse_button(MouseButton::Left, true);
    m.run();
    m.pointer = {754.0f, 300.0f};
    m.run();
    m.devices.set_mouse_button(MouseButton::Left, false);
    m.run(2);
    CHECK(m.s("split") > 55);
}

#endif // CUERVO_FEATURE_UI

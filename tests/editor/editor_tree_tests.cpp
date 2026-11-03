// The RmlUi scene tree's rules (editor/tree_view.hpp): visible rows, virtualization window, drop
// targets, and that drops are one undoable step. Thousands of nodes stay cheap.

#include <doctest.h>

#include <chrono>
#include <string>

#include "editor/editor_controller.hpp"
#include "editor/tree_view.hpp"

using namespace cuervo;
using namespace cuervo::editor;

namespace {

struct Fixture {
    EditorController c;
    Fixture() {
        std::string error;
        REQUIRE_MESSAGE(c.env().open_prelude_only(error), error);
        c.doc().set_registry(&c.env().classes());
        c.doc().new_scene("Node2D", "root");
    }
    EditorDocument& d() { return c.doc(); }
    NodeId add(NodeId parent, const char* name) { return d().add_child(parent, "Node2D", name); }
};

} // namespace

TEST_CASE("tree: flatten honors collapsed nodes, reveal reopens ancestors") {
    Fixture f;
    const NodeId root = f.d().model().root();
    const NodeId a = f.add(root, "a");
    const NodeId b = f.add(a, "b");
    const NodeId c = f.add(b, "c");
    f.add(root, "z");
    TreeState t;
    CHECK(t.flatten(f.d().model()).size() == 5);
    t.set_expanded(a, false);
    const auto rows = t.flatten(f.d().model());
    CHECK(rows.size() == 3); // root, a (closed), z
    CHECK(rows[1].has_children);
    CHECK_FALSE(rows[1].expanded);
    t.reveal(f.d().model(), c);
    CHECK(t.flatten(f.d().model()).size() == 5);
    CHECK(t.flatten(f.d().model())[3].depth == 3);
    t.set_expanded(9999, false);
    t.prune(f.d().model());
    CHECK(t.expanded(9999));
}

TEST_CASE("tree: the row window covers the viewport plus overscan and never exceeds the list") {
    const RowWindow w = row_window(1000, 220.0f, 440.0f, 22.0f, 4);
    CHECK(w.first == 6);
    CHECK(w.last == 34);
    CHECK(row_window(10, 0, 440.0f, 22.0f, 4) == RowWindow{0, 10});
    CHECK(row_window(0, 0, 100, 22).last == 0);
    CHECK(row_window(1000, 1e9f, 440.0f, 22.0f, 4).first <= 1000);
    CHECK(row_at(100, 45.0f, 22.0f) == 2);
    CHECK(row_at(100, 1e6f, 22.0f) == 99);
}

TEST_CASE("tree: drop targets follow the quarter rule and refuse cycles, the root and no-ops") {
    Fixture f;
    const NodeId root = f.d().model().root();
    const NodeId a = f.add(root, "a");
    const NodeId b = f.add(root, "b");
    const NodeId c = f.add(a, "c");
    const SceneModel& m = f.d().model();
    DropTarget t = drop_target(m, b, a, 0.1f);
    CHECK(t.zone == DropZone::Before);
    CHECK(t.parent == root);
    CHECK(*t.index == 0);
    t = drop_target(m, a, b, 0.9f); // a moves after b: index counted after a leaves
    CHECK(t.zone == DropZone::After);
    CHECK(*t.index == 1);
    t = drop_target(m, b, a, 0.5f);
    CHECK(t.zone == DropZone::Into);
    CHECK(t.parent == a);
    CHECK_FALSE(drop_target(m, a, c, 0.5f).valid()); // into its own subtree
    CHECK_FALSE(drop_target(m, a, a, 0.5f).valid());
    CHECK_FALSE(drop_target(m, root, a, 0.5f).valid());
    CHECK_FALSE(drop_target(m, a, a, 0.1f).valid());
    CHECK_FALSE(drop_target(m, a, b, 0.1f).valid()); // a already sits right before b
    CHECK(drop_target(m, c, root, 0.1f).zone == DropZone::Into); // the root takes children only
}

TEST_CASE("tree: a drop reparents in one undo step and undo restores the order") {
    Fixture f;
    const NodeId root = f.d().model().root();
    const NodeId a = f.add(root, "a");
    const NodeId b = f.add(root, "b");
    const NodeId c = f.add(root, "c");
    const std::size_t steps = f.d().stack().size();
    CHECK(apply_drop(f.d(), c, drop_target(f.d().model(), c, a, 0.5f)));
    CHECK(f.d().model().get(c)->parent == a);
    CHECK(f.d().stack().size() == steps + 1);
    CHECK(apply_drop(f.d(), b, drop_target(f.d().model(), b, a, 0.1f)));
    CHECK(f.d().model().index_in_parent(b) == 0);
    CHECK(f.d().undo());
    CHECK(f.d().undo());
    CHECK(f.d().model().get(c)->parent == root);
    CHECK(f.d().model().index_in_parent(c) == 2);
    CHECK(move_sibling(f.d(), a, 1));
    CHECK(f.d().model().index_in_parent(a) == 1);
    CHECK_FALSE(move_sibling(f.d(), root, 1));
}

TEST_CASE("tree: range select between visible rows") {
    Fixture f;
    const NodeId root = f.d().model().root();
    const NodeId a = f.add(root, "a");
    const NodeId b = f.add(root, "b");
    const NodeId c = f.add(root, "c");
    TreeState t;
    const auto rows = t.flatten(f.d().model());
    CHECK(range_between(rows, c, a) == std::vector<NodeId>{a, b, c});
    CHECK(range_between(rows, a, 9999).empty());
}

TEST_CASE("tree: five thousand nodes flatten and window in well under a frame") {
    Fixture f;
    const NodeId root = f.d().model().root();
    NodeId parent = root;
    for (int i = 0; i < 5000; ++i) {
        const NodeId n = f.add(i % 50 == 0 ? root : parent, "n");
        if (i % 7 == 0) parent = n;
    }
    TreeState t;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t rows = 0;
    for (int i = 0; i < 20; ++i) rows = t.flatten(f.d().model()).size();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
    CHECK(rows == f.d().model().size());
    CHECK(rows >= 5000);
    CHECK(ms < 8.0);
    const RowWindow w = row_window(rows, 22.0f * 2500, 800.0f, 22.0f);
    CHECK(w.last - w.first < 60); // only a screenful of rows is ever built
}

TEST_CASE("tree: collapsed state saves as paths and loads into a rebuilt model") {
    Fixture f;
    const NodeId root = f.d().model().root();
    const NodeId a = f.add(root, "a");
    f.add(a, "b");
    const NodeId z = f.add(root, "z");
    f.add(z, "y");
    TreeState t;
    t.set_expanded(a, false);
    const std::string saved = t.save(f.d().model());
    CHECK(saved == "/a");
    // A fresh model (new ids): same paths, the saved node is collapsed again; a gone path is ignored.
    Fixture g;
    const NodeId a2 = g.add(g.d().model().root(), "a");
    g.add(a2, "b");
    TreeState u;
    u.load(g.d().model(), saved + ";/missing");
    CHECK_FALSE(u.expanded(a2));
    CHECK(u.flatten(g.d().model()).size() == 2);
}

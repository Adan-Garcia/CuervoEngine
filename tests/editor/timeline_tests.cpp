// Timeline: the animation drawer's edit model (clips, tracks and keys as one rewrite of `animations`).
#include <doctest.h>

#include <string>

#include "editor/editor_controller.hpp"
#include "editor/timeline.hpp"

using namespace cuervo;
using namespace cuervo::editor;

namespace {

struct Fixture {
    EditorController c;
    NodeId player = 0, sprite = 0;
    Timeline tl;
    Fixture() {
        std::string error;
        REQUIRE_MESSAGE(c.env().open_prelude_only(error), error);
        c.doc().set_registry(&c.env().classes());
        c.doc().new_scene("Node2D", "root");
        sprite = c.doc().add_child(c.doc().model().root(), "Node2D", "arm");
        player = c.doc().add_child(c.doc().model().root(), "AnimationPlayer", "anim");
        c.doc().select(player);
        tl.follow(c.doc(), find_animation_player(c.doc(), c.env().classes()));
    }
};

} // namespace

TEST_CASE("timeline: finds the player from the selection or a child of it") {
    Fixture f;
    CHECK(f.tl.player() == f.player);
    f.c.doc().select(f.c.doc().model().root());
    CHECK(find_animation_player(f.c.doc(), f.c.env().classes()) == f.player);
    f.c.doc().select(f.sprite);
    CHECK(find_animation_player(f.c.doc(), f.c.env().classes()) == 0);
}

TEST_CASE("timeline: clip, track and key edits are undoable rewrites of animations") {
    Fixture f;
    REQUIRE(f.tl.new_clip(f.c.doc(), "walk").empty());
    CHECK_FALSE(f.tl.new_clip(f.c.doc(), "walk").empty()); // duplicate refused
    f.tl.follow(f.c.doc(), f.player);
    REQUIRE(f.tl.clip() != nullptr);
    CHECK(f.tl.clip()->name == "walk");
    CHECK_FALSE(f.tl.add_track(f.c.doc(), "no_colon", "Property").empty());
    REQUIRE(f.tl.add_track(f.c.doc(), "arm:rotation", "Property").empty());
    f.tl.follow(f.c.doc(), f.player);
    CHECK(f.tl.track_index() == 0);
    CHECK_FALSE(f.tl.add_track(f.c.doc(), "arm:rotation", "Property").empty()); // duplicate
    f.tl.playhead = 10;
    f.tl.key_at_playhead(f.c.doc());
    f.tl.playhead = 30;
    f.tl.key_at_playhead(f.c.doc());
    f.tl.follow(f.c.doc(), f.player);
    REQUIRE(f.tl.track() != nullptr);
    CHECK(f.tl.track()->keys.size() == 2);
    // Move the second key; a tick that is taken is refused.
    f.tl.select_track(0, 1);
    f.tl.move_key(f.c.doc(), 10);
    f.tl.follow(f.c.doc(), f.player);
    CHECK(f.tl.track()->keys[1].time == 30);
    f.tl.move_key(f.c.doc(), 45);
    f.tl.follow(f.c.doc(), f.player);
    CHECK(f.tl.track()->keys[1].time == 45);
    // Edit value and interpolation of the selected key.
    anim::Key k = *f.tl.key();
    k.value[0] = 1.5f;
    k.interp = "Step";
    f.tl.edit_key(f.c.doc(), k, false);
    f.tl.follow(f.c.doc(), f.player);
    CHECK(f.tl.key()->value[0] == doctest::Approx(1.5f));
    CHECK(f.tl.key()->interp == "Step");
    // Deleting a key, then undoing, brings it back.
    f.tl.delete_key(f.c.doc(), 0, 0);
    f.tl.follow(f.c.doc(), f.player);
    CHECK(f.tl.track()->keys.size() == 1);
    f.c.doc().undo();
    f.tl.follow(f.c.doc(), f.player);
    CHECK(f.tl.track()->keys.size() == 2);
    // Length and loop mode.
    f.tl.set_length(f.c.doc(), 90, false);
    f.tl.set_mode(f.c.doc(), "Loop");
    f.tl.follow(f.c.doc(), f.player);
    CHECK(f.tl.clip()->length == 90);
    CHECK(f.tl.clip()->mode == "Loop");
    f.tl.delete_clip(f.c.doc());
    f.tl.follow(f.c.doc(), f.player);
    CHECK(f.tl.clip() == nullptr);
}

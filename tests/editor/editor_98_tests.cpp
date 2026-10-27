// Milestone 9.8's editor tests (DESIGN.md 4.17 "Clarified (Milestone 9.8)"):
// the animation timeline's data layer (editor/anim_edit) and its scrub
// preview, and the editor's node icons for the 9.x nodes.

#include <doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "editor/anim_edit.hpp"
#include "editor/editor_document.hpp"
#include "editor/icons.hpp"
#include "editor/project_env.hpp"
#include "pluma/value.hpp"

using namespace cuervo;
using namespace cuervo::editor;

namespace {

const pluma::Value* field_of(const pluma::Value& v, std::string_view name) {
    if (!v.is_obj() || v.o->kind != pluma::ObjKind::Instance) return nullptr;
    const auto* o = static_cast<const pluma::InstanceObj*>(v.o);
    for (std::size_t i = 0; i < o->type->field_names.size(); ++i) {
        if (o->type->field_names[i] == name) return &o->fields[i];
    }
    return nullptr;
}

// The preview's child of the root named `name`.
pluma::Value child_named(const pluma::Value& root, std::string_view name) {
    const pluma::Value* children = field_of(root, "children");
    REQUIRE(children != nullptr);
    for (const pluma::Value& c : static_cast<const pluma::ListObj*>(children->o)->items) {
        const pluma::Value* n = field_of(c, "name");
        if (n != nullptr && static_cast<const pluma::StrObj*>(n->o)->s == name) return c;
    }
    FAIL("no child named " << name);
    return {};
}

anim::Key key(int time, float x, float y = 0.0f) {
    anim::Key k;
    k.time = time;
    k.value = {x, y, 0.0f, 0.0f};
    return k;
}

anim::Clip slide_clip() {
    anim::Clip c;
    c.name = "slide";
    c.length = 10;
    c.mode = "Loop";
    anim::Track t;
    t.path = "box:position";
    anim::put_key(t, key(10, 100.0f, 20.0f));
    anim::put_key(t, key(0, 0.0f, 20.0f));
    c.tracks.push_back(t);
    anim::Track ev;
    ev.kind = "Event";
    anim::Key k;
    k.time = 5;
    k.text = "half";
    ev.keys.push_back(k);
    c.tracks.push_back(ev);
    return c;
}

} // namespace

TEST_CASE("timeline data: keys stay sorted, one per tick") {
    anim::Track t;
    anim::put_key(t, key(20, 2.0f));
    anim::put_key(t, key(0, 0.0f));
    CHECK(anim::put_key(t, key(10, 1.0f)) == 1);
    CHECK(anim::put_key(t, key(10, 5.0f)) == 1); // same tick: replaced
    REQUIRE(t.keys.size() == 3);
    CHECK(t.keys[0].time == 0);
    CHECK(t.keys[1].value[0] == 5.0f);
    CHECK(t.keys[2].time == 20);
    t.keys.push_back(key(0, 9.0f));
    anim::normalize(t);
    REQUIRE(t.keys.size() == 3);
    CHECK(t.keys[0].value[0] == 9.0f); // the last key placed on tick 0 wins
}

TEST_CASE("timeline data: values pack like Anim.write, paths split at the last ':'") {
    CHECK(anim::pack(scene::Value::number(1.5))->at(0) == 1.5f);
    CHECK(anim::pack(scene::Value::boolean(true))->at(0) == 1.0f);
    CHECK(anim::pack(scene::Value::integer(7))->at(0) == 7.0f);
    const auto v = anim::pack(scene::Value::vector({3.0f, 4.0f}));
    CHECK((*v == std::array<float, 4>{3.0f, 4.0f, 0.0f, 0.0f}));
    scene::Value color;
    color.kind = scene::ValueKind::Struct;
    color.type = "Color";
    color.fields = {{"r", {}, scene::Value::number(0.5)}, {"g", {}, scene::Value::number(0.25)},
                    {"b", {}, scene::Value::number(1.0)}, {"a", {}, scene::Value::number(0.75)}};
    CHECK((*anim::pack(color) == std::array<float, 4>{0.5f, 0.25f, 1.0f, 0.75f}));
    CHECK_FALSE(anim::pack(scene::Value::str("no")).has_value());
    std::string node, field;
    REQUIRE(anim::split_path("Visual/Arm:rotation", node, field));
    CHECK(node == "Visual/Arm");
    CHECK(field == "rotation");
    REQUIRE(anim::split_path(":energy", node, field));
    CHECK(node.empty());
    CHECK_FALSE(anim::split_path("Visual/Arm", node, field));
}

TEST_CASE("timeline: clips written to an AnimationPlayer read back, save, and pose the preview") {
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open_prelude_only(error), error);
    EditorDocument doc(&env.classes());
    doc.new_scene("Node2D", "root");
    const NodeId box = doc.add_child(doc.model().root(), "Shape", "box");
    const NodeId player = doc.add_child(doc.model().root(), "AnimationPlayer", "anim");
    REQUIRE(box != 0);
    REQUIRE(player != 0);
    CHECK(anim::read(doc.field_value(player, "animations")).empty());

    const std::vector<anim::Clip> clips{slide_clip()};
    REQUIRE_MESSAGE(doc.set_field(player, "animations", anim::write(clips)), doc.error());
    const std::vector<anim::Clip> back = anim::read(doc.field_value(player, "animations"));
    REQUIRE(back.size() == 1);
    CHECK(back[0].name == "slide");
    CHECK(back[0].length == 10);
    CHECK(back[0].mode == "Loop");
    REQUIRE(back[0].tracks.size() == 2);
    CHECK(back[0].tracks[0].keys[1].value[0] == 100.0f);
    CHECK(back[0].tracks[1].kind == "Event");
    CHECK(back[0].tracks[1].keys[0].text == "half");
    // The same bytes after a scene file round trip.
    const scene::SceneDoc saved = doc.model().to_doc();
    const std::vector<std::byte> bytes = scene::write_binary(saved);
    const std::optional<scene::SceneDoc> loaded = scene::read_binary(bytes, error);
    REQUIRE_MESSAGE(loaded.has_value(), error);
    CHECK(*loaded == saved);
    const std::optional<scene::SceneDoc> text = scene::parse_text(scene::dump_text(saved), error);
    REQUIRE_MESSAGE(text.has_value(), error);
    CHECK(*text == saved);

    // Scrubbing: the preview's box sits where tick 5 of the clip puts it.
    env.rebuild_preview(doc.model().to_doc());
    REQUIRE(env.preview_ok());
    REQUIRE_MESSAGE(env.preview_animation(static_cast<std::size_t>(doc.model().doc_index(player)), "slide", 5.0f,
                                          error),
                    error);
    const pluma::Value* pos = field_of(child_named(env.host().root(), "box"), "position");
    REQUIRE(pos != nullptr);
    CHECK(pluma::vec_data(*pos)[0] == doctest::Approx(50.0f));
    CHECK(pluma::vec_data(*pos)[1] == doctest::Approx(20.0f));
    // Not a player: refused. An unknown clip: the player reports and ignores it.
    CHECK_FALSE(env.preview_animation(static_cast<std::size_t>(doc.model().doc_index(box)), "slide", 1.0f, error));
    CHECK(env.preview_animation(static_cast<std::size_t>(doc.model().doc_index(player)), "nope", 1.0f, error));
    CHECK_FALSE(env.host().panicked());
    // Undo takes the clips away in one step.
    REQUIRE(doc.undo());
    CHECK(anim::read(doc.field_value(player, "animations")).empty());
}

TEST_CASE("icons: the lighting, tilemap collision and animation nodes have their own") {
    CHECK(icon_for_class(nullptr, "PointLight2D") == "light");
    CHECK(icon_for_class(nullptr, "DirectionalLight2D") == "sun");
    CHECK(icon_for_class(nullptr, "AmbientLight2D") == "moon");
    CHECK(icon_for_class(nullptr, "LightOccluder2D") == "occluder");
    CHECK(icon_for_class(nullptr, "TileMapCollision2D") == "static_body");
    CHECK(icon_for_class(nullptr, "AnimationPlayer") == "animation");
    for (const char* name : {"light", "sun", "moon", "occluder", "animation"}) CHECK(has_icon(name));
}

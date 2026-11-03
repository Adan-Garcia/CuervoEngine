// Milestone 11.7c: the particle panel's data layer (editor/particle_preview).

#include <doctest.h>

#include <string>

#include "editor/editor_document.hpp"
#include "editor/particle_preview.hpp"
#include "editor/project_env.hpp"

using namespace cuervo;
using namespace cuervo::editor;

TEST_CASE("particle preview follows the Emitter2D fields it is tuned with") {
    ProjectEnv env;
    std::string error;
    REQUIRE_MESSAGE(env.open_prelude_only(error), error);
    EditorDocument doc(&env.classes());
    doc.new_scene("Node2D", "root");
    const NodeId plain = doc.add_child(doc.model().root(), "Node2D", "plain");
    CHECK_FALSE(read_emitter_params(doc, plain).valid);
    const NodeId e = doc.add_child(doc.model().root(), "Emitter2D", "sparks");
    REQUIRE(e != 0);
    REQUIRE(doc.set_field(e, "count", scene::Value::integer(10)));
    REQUIRE(doc.set_field(e, "pattern", scene::Value::enum_value("EmitPattern", "Arc")));
    REQUIRE(doc.set_field(e, "speed", scene::Value::number(100.0)));
    REQUIRE(doc.set_field(e, "life", scene::Value::number(1.0)));
    REQUIRE(doc.set_field(e, "gravity", scene::Value::vector({0.0f, 50.0f})));

    const EmitterParams p = read_emitter_params(doc, e);
    REQUIRE(p.valid);
    CHECK(p.count == 10);
    CHECK(p.pattern == "Arc");
    CHECK(p.gravity.y() == 50.0f);

    EmitterPreview a(7);
    EmitterPreview b(7);
    for (int i = 0; i < 10; ++i) {
        a.step(p, 1.0f / 60.0f);
        b.step(p, 1.0f / 60.0f);
    }
    CHECK(a.particles().size() == 10); // one shot of 10 so far (interval 0.25 s)
    REQUIRE(a.particles().size() == b.particles().size());
    CHECK(a.particles()[3].position.x() == b.particles()[3].position.x()); // same seed, same preview

    // Particles die after `life`; a one-shot fires once.
    EmitterParams once = p;
    once.one_shot = true;
    EmitterPreview c(1);
    for (int i = 0; i < 240; ++i) c.step(once, 1.0f / 60.0f);
    CHECK(c.shots() == 1);
    CHECK(c.particles().empty());

    // Fade and end scale shape color and size over the lifetime.
    EmitterParams look = p;
    look.fade = true;
    look.end_scale = 3.0f;
    look.size = 2.0f;
    PreviewParticle q;
    q.life = 2.0f;
    q.age = 1.0f;
    CHECK(EmitterPreview::color_of(look, q)[3] == doctest::Approx(0.5f));
    CHECK(EmitterPreview::radius_of(look, q) == doctest::Approx(4.0f));
}

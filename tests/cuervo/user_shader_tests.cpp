#include <doctest.h>

#include "cuervo/render/user_shader.hpp"

using namespace cuervo;

TEST_CASE("wrap_user_glsl: @export declarations become packed macros on PostParams") {
    const std::string src =
        "@export float strength = 0.5;\n"
        "@export vec3 tint = vec3(1.0, 0.8, 0.6);\n"
        "@export vec2 offset;\n"
        "@export float gain = 2;\n"
        "void main() { o_color = texture(u_src, v_uv) * strength; }\n";
    std::string error;
    const auto w = render::wrap_user_glsl(src, error);
    REQUIRE_MESSAGE(w, error);
    REQUIRE(w->exports.size() == 4);
    CHECK(w->exports[0].name == "strength");
    CHECK(w->exports[0].slot == 0);
    CHECK(w->exports[0].defaults[0] == 0.5f);
    CHECK(w->exports[1].slot == 1);       // a vec3 fits behind `strength` in v0 (.yzw)
    CHECK(w->exports[1].defaults[1] == 0.8f);
    CHECK(w->exports[2].slot == 4);       // v0 is full: the vec2 starts v1 (never straddles)
    CHECK(w->exports[3].slot == 6);       // then the float lands behind it
    CHECK(w->glsl.find("#define strength (params.v0.x)") != std::string::npos);
    CHECK(w->glsl.find("#define tint (params.v0.yzw)") != std::string::npos);
    CHECK(w->glsl.find("#define offset (params.v1.xy)") != std::string::npos);
    CHECK(w->glsl.find("@export") == std::string::npos);
    CHECK(w->glsl.starts_with("#version 450"));
    CHECK(w->glsl.find("void main() {\n    user_main();") != std::string::npos);
}

TEST_CASE("wrap_user_glsl: errors name the line") {
    std::string error;
    CHECK_FALSE(render::wrap_user_glsl("void main() {}\n@export mat4 m;\n", error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK_FALSE(render::wrap_user_glsl("#version 330\nvoid main() {}\n", error));
    CHECK(error.find("#version") != std::string::npos);
    CHECK_FALSE(render::wrap_user_glsl("@export float a;\n@export float a;\n", error));
    std::string many;
    for (int i = 0; i < 13; ++i) many += "@export float f" + std::to_string(i) + ";\n";
    CHECK_FALSE(render::wrap_user_glsl(many, error));
    CHECK(error.find("12") != std::string::npos);
}

TEST_CASE(".cshader round-trips and rejects damage") {
    render::CompiledShader s;
    s.spirv = {0x07230203u, 1u, 2u, 3u};
    s.glsl330 = "#version 330 core\n";
    s.glsl_es300 = "#version 300 es\n";
    s.wgsl = "@fragment fn main() {}";
    s.uniform_blocks = {{"PostParams", 0}};
    s.textures = {{"u_src", 0}, {"u_aux", 1}};
    render::ExportedUniform e;
    e.name = "strength";
    e.components = 2;
    e.slot = 5;
    e.defaults[0] = 0.25f;
    s.exports = {e};

    const std::vector<std::byte> bytes = render::encode_cshader(s);
    const auto back = render::decode_cshader(bytes);
    REQUIRE(back);
    CHECK(back->spirv == s.spirv);
    CHECK(back->glsl_es300 == s.glsl_es300);
    CHECK(back->wgsl == s.wgsl);
    CHECK(back->textures == s.textures);
    CHECK(back->exports == s.exports);
    CHECK(render::encode_cshader(*back) == bytes); // deterministic

    std::vector<std::byte> truncated(bytes.begin(), bytes.end() - 3);
    CHECK_FALSE(render::decode_cshader(truncated));
    std::vector<std::byte> bad_magic = bytes;
    bad_magic[0] = std::byte{'X'};
    CHECK_FALSE(render::decode_cshader(bad_magic));

    // UserShader: the ShaderSource views the loaded strings; defaults and setters hit v0..v2.
    const render::UserShader shader(*back);
    CHECK(shader.source().glsl330 == s.glsl330);
    CHECK(shader.source().textures.size() == 2);
    render::PostParams p = shader.defaults();
    CHECK(p.v1[1] == 0.25f); // slot 5 = v1.y
    const float values[] = {3.0f, 4.0f};
    CHECK(shader.set(p, "strength", values));
    CHECK(p.v1[1] == 3.0f);
    CHECK(p.v1[2] == 4.0f);
    CHECK_FALSE(shader.set(p, "nope", values));
}

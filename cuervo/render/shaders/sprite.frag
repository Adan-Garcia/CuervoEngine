#version 450
// Textured sprite: texture times tint. The atlas holds straight-alpha
// display-referred colour (sprite.vert); the output is premultiplied for
// BlendMode::Premultiplied, like every other batcher.

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_color;

layout(set = 1, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) out vec4 o_color;

void main() {
    vec4 c = texture(u_atlas, v_uv) * v_color;
    o_color = vec4(c.rgb * c.a, c.a);
}

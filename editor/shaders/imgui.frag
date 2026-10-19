#version 450
// Dear ImGui: vertex color times the bound texture (the font atlas, whose
// white texel covers untextured shapes). Straight alpha, BlendMode::Alpha.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(set = 1, binding = 0) uniform sampler2D u_texture;

layout(location = 0) out vec4 o_color;

void main() {
    o_color = v_color * texture(u_texture, v_uv);
}

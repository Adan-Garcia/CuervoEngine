#version 450
// Bloom pyramid, upward step: a 3x3 tent filter over the smaller level, added
// onto the larger one (BlendMode::AdditivePremultiplied).
//   v0.xy: texel size of the source   v1.x: radius (texels)   v1.y: weight

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_screen;

layout(std140, set = 0, binding = 0) uniform PostParams {
    vec4 v0;
    vec4 v1;
    vec4 v2;
    vec4 v3;
} params;

layout(set = 1, binding = 0) uniform sampler2D u_src;

layout(location = 0) out vec4 o_color;

void main() {
    vec2 t = params.v0.xy * params.v1.x;
    vec3 c = texture(u_src, v_uv + vec2(-t.x, -t.y)).rgb + texture(u_src, v_uv + vec2(0.0, -t.y)).rgb * 2.0 +
             texture(u_src, v_uv + vec2(t.x, -t.y)).rgb + texture(u_src, v_uv + vec2(-t.x, 0.0)).rgb * 2.0 +
             texture(u_src, v_uv).rgb * 4.0 + texture(u_src, v_uv + vec2(t.x, 0.0)).rgb * 2.0 +
             texture(u_src, v_uv + vec2(-t.x, t.y)).rgb + texture(u_src, v_uv + vec2(0.0, t.y)).rgb * 2.0 +
             texture(u_src, v_uv + vec2(t.x, t.y)).rgb;
    o_color = vec4(c * (params.v1.y / 16.0), 1.0);
}

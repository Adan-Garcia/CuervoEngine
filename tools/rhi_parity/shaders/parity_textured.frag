#version 450
// RHI parity scenes: samples binding 0 (RGBA8) times binding 1 (R8, which
// samples as (r, 0, 0, 1)) mixed in by params.tint.a.

layout(location = 0) in vec2 v_uv;

layout(std140, set = 0, binding = 0) uniform Params {
    vec4 offset_scale;
    vec4 tint;
} params;

layout(set = 1, binding = 0) uniform sampler2D u_color;
layout(set = 1, binding = 1) uniform sampler2D u_mask;

layout(location = 0) out vec4 o_color;

void main() {
    vec4 c = texture(u_color, v_uv);
    vec4 m = texture(u_mask, v_uv);
    o_color = vec4(mix(c.rgb, m.rgb, params.tint.a) * params.tint.rgb, c.a);
}

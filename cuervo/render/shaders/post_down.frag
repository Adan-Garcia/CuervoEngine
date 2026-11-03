#version 450
// Bloom pyramid, downward step: a 5-tap filter (centre x4 + four diagonals) over
// the larger level. The first step also keeps only the bright part.
//   v0.xy: texel size of the source   v1.x: threshold   v1.y: knee width
//   v1.z:  1 on the first step (apply the threshold)

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

vec3 tap(vec2 uv) {
    vec3 c = texture(u_src, uv).rgb;
    if (params.v1.z > 0.5) {
        float b = max(c.r, max(c.g, c.b));
        c *= smoothstep(params.v1.x, params.v1.x + max(params.v1.y, 0.0001), b);
    }
    return c;
}

void main() {
    vec2 t = params.v0.xy;
    vec3 c = tap(v_uv) * 4.0 + tap(v_uv + vec2(-t.x, -t.y)) + tap(v_uv + vec2(t.x, -t.y)) +
             tap(v_uv + vec2(-t.x, t.y)) + tap(v_uv + vec2(t.x, t.y));
    o_color = vec4(c * 0.125, 1.0);
}

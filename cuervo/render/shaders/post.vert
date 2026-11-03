#version 450
// Full-screen pass vertex stage (post-processing, custom blits; DESIGN.md 4.19).
// One triangle covers the target: a_pos is (-1,-1), (3,-1), (-1,3).
//   v_uv:     where to sample a *render target* (GL stores target rows bottom-up,
//             DeviceInfo::render_target_flip_v; PostParams.v3.w = 1 flips v)
//   v_screen: 0..1 over the target, y down on every backend (for effects that
//             depend on the screen position: vignette, scanlines)

layout(location = 0) in vec2 a_pos;

layout(std140, set = 0, binding = 0) uniform PostParams {
    vec4 v0;
    vec4 v1;
    vec4 v2;
    vec4 v3; // w: flip sampled targets' v
} params;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_screen;

void main() {
    float top_down = 0.5 - 0.5 * a_pos.y;
    v_screen = vec2(0.5 + 0.5 * a_pos.x, top_down);
    v_uv = vec2(v_screen.x, params.v3.w > 0.5 ? 1.0 - top_down : top_down);
    gl_Position = vec4(a_pos, 0.0, 1.0);
}

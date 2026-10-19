#version 450
// RHI parity scenes: textured quads.

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;

layout(std140, set = 0, binding = 0) uniform Params {
    vec4 offset_scale;
    vec4 tint;
} params;

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 p = a_position * params.offset_scale.zw + params.offset_scale.xy;
    gl_Position = vec4(p.x / 128.0 - 1.0, 1.0 - p.y / 128.0, 0.0, 1.0);
    v_uv = a_uv;
}

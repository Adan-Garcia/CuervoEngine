#version 450
// RHI parity scenes: per-instance offset (Float2), color (UByte4Norm) and an
// integer palette index (UInt1, an integer attribute), exercising instance
// streams and first_instance (emulated on GL).

layout(location = 0) in vec2 a_position;
layout(location = 2) in vec2 i_offset;
layout(location = 3) in vec4 i_color;
layout(location = 4) in uint i_palette;

layout(std140, set = 0, binding = 0) uniform Params {
    vec4 offset_scale;
    vec4 tint;
} params;

layout(std140, set = 0, binding = 1) uniform Palette {
    vec4 colors[4];
} palette;

layout(location = 0) out vec4 v_color;

void main() {
    vec2 p = a_position * params.offset_scale.zw + i_offset + params.offset_scale.xy;
    gl_Position = vec4(p.x / 128.0 - 1.0, 1.0 - p.y / 128.0, 0.0, 1.0);
    v_color = i_color * palette.colors[i_palette & 3u] * params.tint;
}

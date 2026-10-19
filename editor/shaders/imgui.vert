#version 450
// Dear ImGui geometry (editor/imgui_rhi.hpp): 2D vertices in ImGui display
// coordinates (top-left origin, y down), mapped to NDC by a scale + offset.
// Varying names must match imgui.frag: GLSL 330 / ES 300 link them by name.

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color; // straight alpha, from ImU32 (r in the low byte)

layout(std140, set = 0, binding = 0) uniform Globals {
    vec4 scale_offset; // xy: scale, zw: offset (display -> NDC)
} globals;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = vec4(a_position * globals.scale_offset.xy + globals.scale_offset.zw, 0.0, 1.0);
}

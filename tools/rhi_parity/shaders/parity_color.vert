#version 450
// RHI parity scenes (tools/rhi_parity): colored geometry in a 256x256 unit
// space (y down), optionally offset per instance. Stream layouts vary per
// pipeline, so attributes a pipeline doesn't feed are never declared here:
// see parity_instanced.vert for the per-instance inputs.
//
// Varying names must match parity_color.frag (GLSL 330 / ES 300 link by name).

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;

layout(std140, set = 0, binding = 0) uniform Params {
    vec4 offset_scale; // xy offset, zw scale (units)
    vec4 tint;
} params;

layout(location = 0) out vec4 v_color;

void main() {
    vec2 p = a_position * params.offset_scale.zw + params.offset_scale.xy;
    gl_Position = vec4(p.x / 128.0 - 1.0, 1.0 - p.y / 128.0, 0.0, 1.0);
#ifndef CUERVO_WEBGPU // WGSL has no point size (and needs none: points are 1 px)
    // Compatible-tier points are 1 px on every backend (rhi::Feature::PointsAndLines,
    // DESIGN.md 4.4 "Clarified (Milestone 10.2)"); Vulkan still needs it written.
    gl_PointSize = 1.0;
#endif
    v_color = a_color * params.tint;
}

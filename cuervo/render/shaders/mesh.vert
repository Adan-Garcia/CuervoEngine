#version 450
// Vector meshes (cuervo/render/mesh_batcher.hpp). Stream 0: mesh vertices in
// document space; stream 1: per-instance 2x3 transform + tint.
//
// Varying names must match mesh.frag: GLSL 330 / ES 300 link them by name.

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;   // straight alpha
layout(location = 2) in uint a_paint;   // 0xFFFFFFFF = solid, else gradient block offset
layout(location = 3) in vec3 i_row0;    // instance transform (a, c, tx)
layout(location = 4) in vec3 i_row1;    // (b, d, ty)
layout(location = 5) in vec4 i_tint;

layout(std140, set = 0, binding = 0) uniform Globals {
    vec4 row0; // world -> NDC affine (a, c, tx)
    vec4 row1; // (b, d, ty)
} globals;

layout(location = 0) out vec2 v_local;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_paint;

void main() {
    vec3 local = vec3(a_position, 1.0);
    vec3 world = vec3(dot(i_row0, local), dot(i_row1, local), 1.0);
    gl_Position = vec4(dot(globals.row0.xyz, world), dot(globals.row1.xyz, world), 0.0, 1.0);
    v_local = a_position; // gradients are defined in document (mesh) space
    v_color = a_color * i_tint;
    v_paint = a_paint;
}

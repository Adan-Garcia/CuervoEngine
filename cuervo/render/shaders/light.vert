#version 450
// Instanced 2D SDF lighting and shadow raymarching quads
// (cuervo/render/light_batcher.hpp).
// Varying names must match light.frag: GLSL 330 / ES 300 link them by name.

layout(location = 0) in vec2 a_corner;         // (+-1, +-1)
layout(location = 1) in vec2 i_center;
layout(location = 2) in vec2 i_half_size;
layout(location = 3) in vec4 i_params0;        // cos_dir, sin_dir, radius, source_radius
layout(location = 4) in vec4 i_params1;        // cos_outer, cos_inner, shadow_softness, falloff
layout(location = 5) in float i_energy;
layout(location = 6) in uint i_flags;          // bit0=shadow, bits1..2=kind, bits3..4=blend
layout(location = 7) in uint i_mask;           // occluder bitmask
layout(location = 8) in vec4 i_color;
layout(location = 9) in vec4 i_shadow_color;

layout(std140, set = 0, binding = 0) uniform LightView {
    vec4 row0; // world -> NDC affine (a, c, tx)
    vec4 row1; // (b, d, ty)
} view;

layout(location = 0) out vec2 v_world;
layout(location = 1) flat out vec2 v_center;
layout(location = 2) flat out vec4 v_params0;
layout(location = 3) flat out vec4 v_params1;
layout(location = 4) flat out float v_energy;
layout(location = 5) flat out uint v_flags;
layout(location = 6) flat out uint v_mask;
layout(location = 7) flat out vec4 v_color;
layout(location = 8) flat out vec4 v_shadow_color;

void main() {
    vec2 world_pos = i_center + a_corner * i_half_size;
    vec3 world = vec3(world_pos, 1.0);
    gl_Position = vec4(dot(view.row0.xyz, world), dot(view.row1.xyz, world), 0.0, 1.0);

    v_world = world_pos;
    v_center = i_center;
    v_params0 = i_params0;
    v_params1 = i_params1;
    v_energy = i_energy;
    v_flags = i_flags;
    v_mask = i_mask;
    v_color = i_color;
    v_shadow_color = i_shadow_color;
}

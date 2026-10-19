#version 450
// Instanced SDF shapes (cuervo/render/sdf_batcher.hpp). One unit quad
// (stream 0) is expanded per instance (stream 1) to the shape's bounding box
// plus outline and anti-aliasing padding.
//
// Varying names must match sdf.frag: GLSL 330 / ES 300 link them by name.

layout(location = 0) in vec2 a_corner;        // (+-1, +-1)
layout(location = 1) in vec2 i_center;
layout(location = 2) in vec2 i_half_size;
layout(location = 3) in vec3 i_params;        // rotation, param, outline_width
layout(location = 4) in uint i_shape;
layout(location = 5) in vec4 i_color;
layout(location = 6) in vec4 i_outline_color;

layout(std140, set = 0, binding = 0) uniform Globals {
    vec4 row0;   // world -> NDC affine (a, c, tx)
    vec4 row1;   // (b, d, ty)
    vec4 params; // x = world units per framebuffer pixel
} globals;

layout(location = 0) out vec2 v_local;
layout(location = 1) flat out vec2 v_half_size;
layout(location = 2) flat out vec2 v_shape_params; // param, outline_width
layout(location = 3) flat out uint v_shape;
layout(location = 4) flat out vec4 v_color;
layout(location = 5) flat out vec4 v_outline_color;

void main() {
    // 1.5 px of padding keeps the anti-aliased edge inside the quad.
    // Arc (shape 5) keeps its band half-thickness in half_size.y; its box is square.
    vec2 half_size = i_shape == 5u ? vec2(i_half_size.x) : i_half_size;
    vec2 extent = half_size + vec2(i_params.z + 1.5 * globals.params.x);
    vec2 local = a_corner * extent;

    // y-down world: positive angles turn clockwise on screen.
    float c = cos(i_params.x);
    float s = sin(i_params.x);
    vec3 world = vec3(i_center + vec2(c * local.x - s * local.y, s * local.x + c * local.y), 1.0);
    gl_Position = vec4(dot(globals.row0.xyz, world), dot(globals.row1.xyz, world), 0.0, 1.0);

    v_local = local;
    v_half_size = i_half_size;
    v_shape_params = i_params.yz;
    v_shape = i_shape;
    v_color = i_color;
    v_outline_color = i_outline_color;
}

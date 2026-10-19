#version 450
// Instanced MSDF glyph quads (cuervo/render/text_renderer.hpp). One unit quad
// (stream 0, corners in [0, 1]) is mapped per glyph (stream 1) onto a
// parallelogram in world space, so text can be translated, rotated, scaled
// and sheared by any affine transform.
//
// Varying names must match text.frag: GLSL 330 / ES 300 link them by name.

layout(location = 0) in vec2 a_corner;          // (0|1, 0|1)
layout(location = 1) in vec2 i_origin;          // world position of the quad's top-left corner
layout(location = 2) in vec2 i_axis_x;          // world vector along the quad's width
layout(location = 3) in vec2 i_axis_y;          // world vector along the quad's height
layout(location = 4) in vec4 i_uv;              // atlas uv of the top-left (xy) and bottom-right (zw) corners
layout(location = 5) in vec4 i_color;
layout(location = 6) in float i_distance_range; // atlas texels

layout(std140, set = 0, binding = 0) uniform Globals {
    vec4 row0; // world -> NDC affine (a, c, tx)
    vec4 row1; // (b, d, ty)
} globals;

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_color;
layout(location = 2) flat out float v_distance_range;

void main() {
    vec3 world = vec3(i_origin + a_corner.x * i_axis_x + a_corner.y * i_axis_y, 1.0);
    gl_Position = vec4(dot(globals.row0.xyz, world), dot(globals.row1.xyz, world), 0.0, 1.0);
    v_uv = mix(i_uv.xy, i_uv.zw, a_corner);
    v_color = i_color;
    v_distance_range = i_distance_range;
}

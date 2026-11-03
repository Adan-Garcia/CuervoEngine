#version 450
// Instanced textured quads (cuervo/render/sprite_batcher.hpp). One unit quad
// (stream 0, corners in [0, 1]) is placed per sprite (stream 1): scaled to the
// sprite's size, rotated about its center and mapped onto a source rectangle
// of the bound texture. Flips are baked into the instance uv rectangle.
//
// Varying names must match sprite.frag: GLSL 330 / ES 300 link them by name.

layout(location = 0) in vec2 a_corner;    // (0|1, 0|1); (0, 0) is the top-left
layout(location = 1) in vec2 i_center;
layout(location = 2) in vec2 i_half_size;
layout(location = 3) in float i_rotation;
layout(location = 4) in vec4 i_color;     // tint
layout(location = 5) in vec4 i_uv;        // texture uv of the (0, 0) corner (xy) and the (1, 1) corner (zw)

layout(std140, set = 0, binding = 0) uniform Globals {
    vec4 row0; // world -> NDC affine (a, c, tx)
    vec4 row1; // (b, d, ty)
} globals;

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_color;

// Authored colors are sRGB; with linear color (Globals.row0.w = 1, 4.19) shading
// and blending happen in linear space, so convert once per instance here.
vec4 working_color(vec4 c) {
    if (globals.row0.w > 0.5) {
        c.rgb = mix(c.rgb / 12.92, pow((c.rgb + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c.rgb));
    }
    return c;
}

void main() {
    vec2 local = (a_corner * 2.0 - 1.0) * i_half_size;
    // y-down world: positive angles turn clockwise on screen.
    float c = cos(i_rotation);
    float s = sin(i_rotation);
    vec3 world = vec3(i_center + vec2(c * local.x - s * local.y, s * local.x + c * local.y), 1.0);
    gl_Position = vec4(dot(globals.row0.xyz, world), dot(globals.row1.xyz, world), 0.0, 1.0);
    v_uv = mix(i_uv.xy, i_uv.zw, a_corner);
    v_color = working_color(i_color);
}

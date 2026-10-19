#version 450
// MSDF text (DESIGN.md 4.4 "Text"). The atlas stores a multi-channel signed
// distance field in rgb (the median of the three channels crosses 0.5 on the
// glyph edge, keeping corners sharp under magnification). The distance is
// converted to screen pixels from the atlas range and the screen-space
// derivative of the texture coordinates, giving a one-pixel anti-aliased edge
// at any zoom, rotation or scale mode. Outputs premultiplied alpha.

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_color;
layout(location = 2) flat in float v_distance_range;

layout(set = 1, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) out vec4 o_color;

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec4 field = texture(u_atlas, v_uv);
    // Distance range in screen pixels: atlas range (in uv units) times screen
    // pixels per uv unit. Never below 1 so tiny text stays smooth, not aliased.
    vec2 unit_range = vec2(v_distance_range) / vec2(textureSize(u_atlas, 0));
    vec2 screen_texture_size = vec2(1.0) / max(fwidth(v_uv), vec2(1e-6));
    float screen_range = max(0.5 * dot(unit_range, screen_texture_size), 1.0);
    float distance = screen_range * (median(field.r, field.g, field.b) - 0.5);
    float coverage = clamp(distance + 0.5, 0.0, 1.0);
    o_color = vec4(v_color.rgb * v_color.a, v_color.a) * coverage;
}

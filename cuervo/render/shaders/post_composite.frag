#version 450
// Final post-processing pass: scene + bloom, LUT grading, CRT, vignette, in
// the order a film/CRT pipeline would apply them. Effects are switched by
// uniforms, so an empty configuration is a plain copy. Colors are display
// referred (sRGB by default; 4.19 linear mode feeds the same pass).
//   v0: x bloom intensity   y vignette strength   z vignette radius   w LUT strength
//   v1: x CRT scanlines     y CRT curvature       z CRT mask          w LUT size (cube edge)
//   v2: xy target size in pixels   z 1 = the scene is linear (effects run on sRGB-encoded
//       values, 4.19 linear color)   w 1 = the destination encodes on write (convert back)
//   v3: x bloom on   y LUT on   z CRT on   w flip sampled targets' v

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_screen;

layout(std140, set = 0, binding = 0) uniform PostParams {
    vec4 v0;
    vec4 v1;
    vec4 v2;
    vec4 v3;
} params;

layout(set = 1, binding = 0) uniform sampler2D u_scene;
layout(set = 1, binding = 1) uniform sampler2D u_bloom;
layout(set = 1, binding = 2) uniform sampler2D u_lut; // N*N x N strip: r along x, g along y, blue slices side by side

layout(location = 0) out vec4 o_color;

vec3 to_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 apply_lut(vec3 c) {
    float n = params.v1.w;
    c = clamp(c, 0.0, 1.0);
    float b = c.b * (n - 1.0);
    float b0 = floor(b);
    float b1 = min(b0 + 1.0, n - 1.0);
    vec2 px = c.rg * (n - 1.0) + 0.5;
    vec2 uv0 = vec2((b0 * n + px.x) / (n * n), px.y / n);
    vec2 uv1 = vec2((b1 * n + px.x) / (n * n), px.y / n);
    return mix(texture(u_lut, uv0).rgb, texture(u_lut, uv1).rgb, b - b0);
}

void main() {
    vec2 uv = v_uv;
    vec2 screen = v_screen;
    float inside = 1.0;
    if (params.v3.z > 0.5) {
        // Barrel distortion of the screen, black outside the tube.
        vec2 p = screen * 2.0 - 1.0;
        p *= 1.0 + params.v1.y * vec2(p.y * p.y, p.x * p.x);
        screen = p * 0.5 + 0.5;
        uv = vec2(screen.x, params.v3.w > 0.5 ? 1.0 - screen.y : screen.y);
        inside = (screen.x >= 0.0 && screen.x <= 1.0 && screen.y >= 0.0 && screen.y <= 1.0) ? 1.0 : 0.0;
        uv = clamp(uv, 0.0, 1.0);
    }
    vec3 col = texture(u_scene, uv).rgb;
    if (params.v3.x > 0.5) {
        col += texture(u_bloom, uv).rgb * params.v0.x;
    }
    if (params.v2.z > 0.5) {
        col = to_srgb(col);
    }
    if (params.v3.y > 0.5) {
        col = mix(col, apply_lut(col), params.v0.w);
    }
    if (params.v3.z > 0.5) {
        // One scanline per two pixel rows (the sine peaks on pixel centers).
        float row = 0.5 + 0.5 * sin(screen.y * params.v2.y * 3.14159265);
        col *= mix(1.0, row, params.v1.x);
        float stripe = mod(floor(screen.x * params.v2.x), 3.0);
        vec3 mask = vec3(stripe < 0.5 ? 1.0 : 0.0, (stripe > 0.5 && stripe < 1.5) ? 1.0 : 0.0, stripe > 1.5 ? 1.0 : 0.0);
        col *= mix(vec3(1.0), 0.35 + 0.65 * mask * 1.5, params.v1.z);
    }
    if (params.v0.y > 0.0) {
        float r = length(v_screen - 0.5) * 1.4142136;
        col *= 1.0 - params.v0.y * smoothstep(params.v0.z, params.v0.z + 0.45, r);
    }
    col *= inside;
    if (params.v2.w > 0.5) {
        col = to_linear(col);
    }
    o_color = vec4(col, 1.0);
}

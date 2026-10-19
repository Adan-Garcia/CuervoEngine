#version 450
// Vector mesh paint: solid vertex color, or a linear / radial gradient
// evaluated per fragment from a block in the Paints window (layout in
// cuervo/vector/mesh.hpp). Outputs premultiplied alpha
// (BlendMode::Premultiplied). Edges are anti-aliased by MSAA.

layout(location = 0) in vec2 v_local;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_paint;

// 1024 vec4 = 16 KiB, the GLES 3.0 / WebGL2 minimum for GL_MAX_UNIFORM_BLOCK_SIZE.
layout(std140, set = 0, binding = 1) uniform Paints {
    vec4 data[1024];
} paints;

layout(location = 0) out vec4 o_color;

float stop_offset(int first, int i) {
    return paints.data[first + i / 4][i % 4];
}

vec4 gradient(int base, vec2 p) {
    vec4 r0 = paints.data[base];
    vec4 r1 = paints.data[base + 1];
    vec4 g = paints.data[base + 2];
    vec4 h = paints.data[base + 3];
    vec3 hp = vec3(p, 1.0);
    vec2 q = vec2(dot(r0.xyz, hp), dot(r1.xyz, hp)); // gradient space

    float t;
    if (r0.w < 0.5) {
        vec2 d = g.zw - g.xy;
        t = dot(q - g.xy, d) / max(dot(d, d), 1e-20);
    } else {
        // Radial with focal point f, center c, radius r: find t with
        // |q - f - t (c - f)| = t r (the focal point is kept inside the circle).
        vec2 cd = g.xy - g.zw;
        vec2 qd = q - g.zw;
        float r = h.x;
        float a = dot(cd, cd) - r * r; // < 0
        float b = dot(qd, cd);
        float c = dot(qd, qd);
        t = (b - sqrt(max(b * b - a * c, 0.0))) / min(a, -1e-20);
    }

    float spread = r1.w;
    if (spread < 0.5) {
        t = clamp(t, 0.0, 1.0);                       // pad
    } else if (spread < 1.5) {
        t = 1.0 - abs(mod(t, 2.0) - 1.0);             // reflect
    } else {
        t = fract(t);                                 // repeat
    }

    int n = int(h.y + 0.5);
    int colors = base + 4;
    int offsets = colors + n;
    if (t <= stop_offset(offsets, 0)) {
        return paints.data[colors];
    }
    for (int i = 1; i < n; ++i) {
        float o1 = stop_offset(offsets, i);
        if (t <= o1) {
            float o0 = stop_offset(offsets, i - 1);
            float f = o1 > o0 ? (t - o0) / (o1 - o0) : 1.0;
            return mix(paints.data[colors + i - 1], paints.data[colors + i], f);
        }
    }
    return paints.data[colors + n - 1];
}

void main() {
    vec4 tint = vec4(v_color.rgb * v_color.a, v_color.a); // premultiply
    if (v_paint == 0xFFFFFFFFu) {
        o_color = tint;
    } else {
        o_color = gradient(int(v_paint), v_local) * tint; // stops are premultiplied already
    }
}

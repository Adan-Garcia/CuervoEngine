#version 450
// Analytic SDF shapes with fwidth-based anti-aliasing. Outputs premultiplied
// alpha (the pipeline uses BlendMode::Premultiplied).

layout(location = 0) in vec2 v_local;
layout(location = 1) flat in vec2 v_half_size;
layout(location = 2) flat in vec2 v_shape_params; // param, outline_width
layout(location = 3) flat in uint v_shape;
layout(location = 4) flat in vec4 v_color;
layout(location = 5) flat in vec4 v_outline_color;

layout(location = 0) out vec4 o_color;

float sd_circle(vec2 p, float r) {
    return length(p) - r;
}

// Segment from (-half_length, 0) to (half_length, 0), radius r.
float sd_capsule(vec2 p, float half_length, float r) {
    p.x = max(abs(p.x) - half_length, 0.0);
    return length(p) - r;
}

float sd_rounded_rect(vec2 p, vec2 half_size, float r) {
    vec2 q = abs(p) - half_size + vec2(r);
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

float sd_ring(vec2 p, float outer, float thickness) {
    return abs(length(p) - (outer - 0.5 * thickness)) - 0.5 * thickness;
}

float sd_sector(vec2 p, float r, float half_angle) {
    if (half_angle >= 3.14159265) {
        return length(p) - r;
    }
    vec2 sc = vec2(sin(half_angle), cos(half_angle));
    vec2 q = vec2(abs(p.y), p.x);
    float l = length(q) - r;
    float m = length(q - sc * clamp(dot(q, sc), 0.0, r));
    return max(l, m * sign(sc.y * q.x - sc.x * q.y));
}

// Band of half-thickness t around the circle of radius r, round caps,
// symmetric about local +x (same frame as sd_sector).
float sd_arc(vec2 p, float r, float t, float half_angle) {
    vec2 q = vec2(abs(p.y), p.x);
    if (half_angle < 3.14159265) {
        vec2 sc = vec2(sin(half_angle), cos(half_angle));
        if (sc.y * q.x > sc.x * q.y) {
            return length(q - sc * r) - t;
        }
    }
    return abs(length(q) - r) - t;
}

void main() {
    float d;
    if (v_shape == 0u) {
        d = sd_circle(v_local, v_half_size.x);
    } else if (v_shape == 1u) {
        d = sd_capsule(v_local, v_half_size.x - v_half_size.y, v_half_size.y);
    } else if (v_shape == 2u) {
        d = sd_rounded_rect(v_local, v_half_size, v_shape_params.x);
    } else if (v_shape == 3u) {
        d = sd_ring(v_local, v_half_size.x, v_shape_params.x);
    } else if (v_shape == 4u) {
        d = sd_sector(v_local, v_half_size.x, v_shape_params.x);
    } else {
        d = sd_arc(v_local, v_half_size.x - v_half_size.y, v_half_size.y, v_shape_params.x);
    }

    // Distance change per pixel: a one-pixel-wide ramp centered on the edge,
    // independent of zoom and scale mode.
    float aa = max(fwidth(d), 1e-6);
    float fill = clamp(0.5 - d / aa, 0.0, 1.0);

    vec4 fill_color = vec4(v_color.rgb * v_color.a, v_color.a);
    float outline_width = v_shape_params.y;
    if (outline_width > 0.0) {
        vec4 line_color = vec4(v_outline_color.rgb * v_outline_color.a, v_outline_color.a);
        float cover = clamp(0.5 - (d - outline_width) / aa, 0.0, 1.0);
        o_color = mix(line_color, fill_color, fill) * cover;
    } else {
        o_color = fill_color * fill;
    }
}

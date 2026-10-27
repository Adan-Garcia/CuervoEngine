#version 450
// 2D SDF lighting and sphere-traced (raymarched) soft and hard shadows.
// Outputs premultiplied color (BlendMode::Premultiplied: src + dst * (1 - src.a)).

layout(location = 0) in vec2 v_world;
layout(location = 1) flat in vec2 v_center;
layout(location = 2) flat in vec4 v_params0;        // cos_dir, sin_dir, radius, source_radius
layout(location = 3) flat in vec4 v_params1;        // cos_outer, cos_inner, shadow_softness, falloff
layout(location = 4) flat in float v_energy;
layout(location = 5) flat in uint v_flags;          // bit0=shadow, bits1..2=kind, bits3..4=blend
layout(location = 6) flat in uint v_mask;
layout(location = 7) flat in vec4 v_color;
layout(location = 8) flat in vec4 v_shadow_color;

layout(std140, set = 0, binding = 1) uniform LightScene {
    uvec4 counts;               // x = occluder_count, y = light_count, z = max_steps, w = reserved
    vec4 ambient_color;         // rgb = ambient tint, a = darkness [0, 1]
    vec4 ambient_params;        // x = light_glow_scale, y = world_units_per_pixel
    vec4 occ_geom[128];         // xy = center, zw = (r, 0) | (hx, hy) | (half_len, r)
    vec4 occ_meta[128];         // x = cos_rot, y = sin_rot, z = corner_radius, w = shape + 4 * bevel
    uvec4 occ_masks[32];        // 128 uint occluder masks
    vec4 light_pos_rad[32];     // xy = center, z = radius, w = source_radius
    vec4 light_color[32];       // rgb = color.rgb * color.a, a = energy
    vec4 light_shadow[32];      // rgba = shadow_color
    vec4 light_params0[32];     // x = cos_dir, y = sin_dir, z = cos_outer, w = cos_inner
    vec4 light_params1[32];     // x = falloff, y = shadow_softness, z = first, w = count (occ_lists range)
    uvec4 light_flags[8];       // 32 uint light flags
    uvec4 light_masks[8];       // 32 uint light shadow masks
    uvec4 occ_lists[128];       // 512 occluder indices: each light's culled occluders
    vec4 light_params2[32];     // x = height above the plane (normal mapping)
} scene;

layout(location = 0) out vec4 o_color;

uint uvec4_at(uvec4 v, uint idx) {
    if (idx == 0u) return v.x;
    if (idx == 1u) return v.y;
    if (idx == 2u) return v.z;
    return v.w;
}

float sd_occluder(vec2 world_p, uint idx) {
    vec4 geom = scene.occ_geom[idx];
    vec4 meta = scene.occ_meta[idx];
    vec2 p = world_p - geom.xy;
    float shape = mod(meta.w, 4.0);
    if (shape < 0.5) {
        // Circle: geom.z = radius
        return length(p) - geom.z;
    }
    // Rotate into occluder's local frame using precomputed (cos_rot, sin_rot)
    vec2 local = vec2(meta.x * p.x + meta.y * p.y, -meta.y * p.x + meta.x * p.y);
    if (shape < 1.5) {
        // RoundedRect / Box: geom.zw = half_size, meta.z = corner_radius
        float r = meta.z;
        vec2 q = abs(local) - geom.zw + vec2(r);
        return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    }
    // Capsule / Segment: geom.z = half_length, geom.w = radius
    local.x = max(abs(local.x) - geom.z, 0.0);
    return length(local) - geom.w;
}

// Only the occluders near the light (occ_lists[first, first + count)):
// LightBatcher::flush culls them on the CPU.
float scene_sdf(vec2 world_p, uint mask, uint first, uint count) {
    float min_d = 1e6;
    for (uint k = first; k < first + count; ++k) {
        uint i = uvec4_at(scene.occ_lists[k >> 2u], k & 3u);
        uint occ_m = uvec4_at(scene.occ_masks[i >> 2u], i & 3u);
        if ((occ_m & mask) == 0u) {
            continue;
        }
        float d = sd_occluder(world_p, i);
        min_d = min(min_d, d);
    }
    return min_d;
}

// scene_sdf, plus the bevel of the nearest occluder (y).
vec2 scene_sdf_bevel(vec2 world_p, uint mask, uint first, uint count) {
    vec2 best = vec2(1e6, 0.0);
    for (uint k = first; k < first + count; ++k) {
        uint i = uvec4_at(scene.occ_lists[k >> 2u], k & 3u);
        uint occ_m = uvec4_at(scene.occ_masks[i >> 2u], i & 3u);
        if ((occ_m & mask) == 0u) {
            continue;
        }
        float d = sd_occluder(world_p, i);
        if (d < best.x) {
            best = vec2(d, floor(scene.occ_meta[i].w * 0.25));
        }
    }
    return best;
}

// How deep p is under the occluders' union along dir: walks out from `depth`
// until a sample is outside them, giving up at `limit`.
float exit_depth(vec2 p, vec2 dir, float depth, float limit, uint mask, uint first, uint count) {
    for (uint k = 0u; k < 8u && depth < limit; ++k) {
        float d = scene_sdf(p + dir * (depth + 0.5), mask, first, count);
        if (d >= 0.0) {
            break;
        }
        depth += max(-d, 0.5) + 0.5;
    }
    return depth;
}

// The union's edge seen from p inside it, walking out along dir: the first
// sample outside dropped onto the edge, where the union's distance is exact
// (inside, touching occluders read their seams as edges). Returns the edge
// point minus p (xy) and the edge's outward normal (zw), or a far point if
// the walk is still inside at `limit`.
vec4 edge_along(vec2 p, vec2 dir, float depth, float limit, uint mask, uint first, uint count) {
    for (uint k = 0u; k < 24u && depth < limit; ++k) {
        vec2 q = p + dir * (depth + 0.5);
        float d = scene_sdf(q, mask, first, count);
        if (d >= 0.0) {
            float e = 0.25;
            vec2 g = vec2(scene_sdf(q + vec2(e, 0.0), mask, first, count) - scene_sdf(q - vec2(e, 0.0), mask, first, count),
                          scene_sdf(q + vec2(0.0, e), mask, first, count) - scene_sdf(q - vec2(0.0, e), mask, first, count));
            float len = length(g);
            vec2 n = len > 1e-5 ? g / len : dir;
            return vec4(q - n * d - p, n);
        }
        depth += max(-d, 0.5) + 0.5;
    }
    return vec4(1e6, 1e6, dir);
}

// Normal mapping for occluders (walls): an occluder's surface faces the
// viewer and bends outward over its last `bevel` units (the SDF gradient
// gives the outward direction), so faces turned to a light at `to_light`
// (x, y in the plane, z = its height) catch it and faces turned away fall
// dark. Relative to a flat surface, which every other pixel is: 1 outside
// occluders and on a bevel-free face. A point on the surface (within the
// march's 0.25 contact) is on the face, as raymarch_shadow takes it: lit as
// open floor, it drew a bright line along faces turned from the light.
float surface_factor(vec2 p, vec3 to_light, uint mask, uint first, uint count) {
    vec2 db = scene_sdf_bevel(p, mask, first, count);
    if (db.x >= 0.25 || db.y <= 0.0) {
        return 1.0;
    }
    float e = 1.0;
    vec2 g = vec2(scene_sdf(p + vec2(e, 0.0), mask, first, count) - scene_sdf(p - vec2(e, 0.0), mask, first, count),
                  scene_sdf(p + vec2(0.0, e), mask, first, count) - scene_sdf(p - vec2(0.0, e), mask, first, count));
    float len = length(g);
    if (len < 1e-5) {
        return 1.0;
    }
    // Where two occluders touch (a tilemap's rectangles) the nearest edge may
    // be a seam, not the union's edge: walk out along the normal, and if that
    // crosses a seam, find the union's nearest edge point from walks all
    // around (straight out of a wall past a seam, or into the corner of an
    // inside corner). Without this the bevel went flat in a
    // wedge at every seam along a wall and in every inside corner.
    vec2 n2 = g / len;
    float start = max(-db.x, 0.0);
    float depth = exit_depth(p, n2, start, db.y, mask, first, count);
    if (depth > start + 0.75) {
        vec4 cands[17];
        vec2 dir = n2;
        uint best = 0u;
        for (uint k = 0u; k < 17u; ++k) {
            cands[k] = edge_along(p, dir, start, 2.0 * db.y, mask, first, count); // past a corner first
            if (length(cands[k].xy) < length(cands[best].xy)) {
                best = k;
            }
            // n2 first, then all around in 22.5 degree steps.
            dir = k == 0u ? vec2(1.0, 0.0) : vec2(dir.x * 0.92387953 - dir.y * 0.38268343, dir.x * 0.38268343 + dir.y * 0.92387953);
        }
        vec2 v = cands[best].xy;
        // An inside corner (rock wrapped around a corner of open air): no
        // walk lands on the corner itself, the nearest edge point. Meet the
        // nearest face with the nearest one turned from it, and take that
        // corner if open air lies just past it.
        uint other = best;
        for (uint k = 0u; k < 17u; ++k) {
            if (dot(cands[k].zw, cands[best].zw) < 0.87 &&
                (other == best || length(cands[k].xy) < length(cands[other].xy))) {
                other = k;
            }
        }
        if (other != best && length(cands[other].xy) < 1e5) {
            vec2 f1 = cands[best].zw;
            vec2 f2 = cands[other].zw;
            float det = f1.x * f2.y - f1.y * f2.x;
            if (abs(det) > 0.1) {
                float b1 = dot(cands[best].xy, f1);
                float b2 = dot(cands[other].xy, f2);
                vec2 c = vec2(b1 * f2.y - b2 * f1.y, f1.x * b2 - f2.x * b1) / det;
                if (length(c) < length(v) &&
                    scene_sdf(p + c + normalize(f1 + f2) * 0.75, mask, first, count) > 0.25) {
                    v = c;
                }
            }
        }
        depth = db.y;
        float l = length(v);
        if (l < depth && l > 1e-4) {
            depth = l;
            n2 = v / l;
        }
    }
    float tilt = 1.0 - smoothstep(0.0, 1.0, depth / db.y);
    vec3 n = normalize(vec3(n2 * (2.0 * tilt), 1.0));
    vec3 l = normalize(to_light);
    return min(max(dot(n, l), 0.0) / max(l.z, 0.1), 2.5);
}

float raymarch_shadow(vec2 p, vec2 ray_dir, float max_dist, float source_radius, float softness, uint mask,
                      uint first, uint count) {
    if (count == 0u || max_dist <= 0.5) {
        return 1.0;
    }
    float limit = max(max_dist - source_radius, 0.0);
    if (limit <= 0.5) {
        return 1.0;
    }
    // A point inside an occluder is on its face, lit like the floor
    // (surface_factor shades it): march out of it on its own budget, so only
    // other occluders shadow it. Penumbra only counts while the ray approaches
    // a surface (h shrinking), so a face isn't darkened by its own occluder.
    float d0 = scene_sdf(p, mask, first, count);
    float t = max(d0, 0.5);
    float h_prev = 1e6;
    // A point on the surface (closer than the march's 0.25 contact) counts as
    // on the face: marching from it hit the surface at once, a hairline of
    // shadow along every floor the light grazes.
    if (d0 < 0.25) {
        t = max(-d0, 1.0);
        float h = scene_sdf(p + ray_dir * t, mask, first, count);
        for (uint k = 0u; k < 32u && h < 0.5 && t < limit; ++k) {
            t += max(-h, 1.0);
            h = scene_sdf(p + ray_dir * t, mask, first, count);
        }
        if (h < 0.5) {
            return 1.0; // the light is inside this occluder
        }
        h_prev = h;
    }
    // Soft shadows model a round light `softness` units across: where the ray
    // passes an occluder, d from it at distance t from the point, the light's
    // disc reaches softness / 2 * t / max_dist to either side of the ray.
    // Shadows are sharp where they touch their occluder and widen toward the
    // light, and a surface the ray merely grazes near the light (a lantern
    // held over the floor) doesn't darken it. The penumbra is centered on the
    // hard shadow's edge: the march steps on into an occluder (d < 0) until
    // the whole disc is hidden (Quilez's soft shadows with an inner penumbra).
    // An outer-only penumbra a full source wide grew the shadow of an
    // occluder next to the light (a crate the lantern touches) far past its
    // outline, and swung it with every small move of the light.
    bool soft = softness > 0.001;
    float half_width = 0.5 * softness;
    float res = 1.0; // -1 = the source hidden, 1 = all of it in view
    uint max_steps = scene.counts.z;
    float max_step = max(limit / 48.0, 2.0);

    for (uint step = 0u; step < max_steps; ++step) {
        if (t >= limit) {
            break;
        }
        float h = scene_sdf(p + ray_dir * t, mask, first, count);
        if (!soft) {
            if (h < 0.25) {
                return 0.0;
            }
        } else if (h < h_prev) {
            // Closest approach between this sample and the previous one
            // (Quilez's improved soft shadows): steadier than h alone.
            float y = 0.0;
            float d = h;
            if (h > 0.0 && h_prev < 1e5) {
                y = h * h / (2.0 * h_prev);
                d = sqrt(max(h * h - y * y, 0.0));
            }
            float w = half_width * max(t - y, 1.0) / max_dist;
            res = min(res, d / max(w, 1e-3));
            if (res <= -1.0) {
                return 0.0;
            }
        }
        h_prev = h;
        // Capped so a penumbra's closest approach is sampled, not skipped.
        t += min(max(h, 0.5 + 0.02 * t), max_step);
    }
    if (!soft) {
        return 1.0;
    }
    res = max(res, -1.0);
    return 0.25 * (1.0 + res) * (1.0 + res) * (2.0 - res);
}

vec2 evaluate_point_light(vec2 p, vec2 light_pos, float radius, float source_radius,
                          vec2 spot_dir, float cos_outer, float cos_inner,
                          float falloff, float softness, bool shadow_on, uint mask, uint first, uint count,
                          float height) {
    vec2 to_light = light_pos - p;
    float dist2 = dot(to_light, to_light);
    float r2 = radius * radius;
    if (dist2 >= r2 || radius <= 0.001) {
        return vec2(0.0, 1.0);
    }
    float dist = sqrt(dist2);
    vec2 ray_dir = dist > 1e-4 ? (to_light / dist) : vec2(1.0, 0.0);

    float spot_atten = 1.0;
    if (cos_outer > -0.999) {
        float cos_angle = dot(-ray_dir, spot_dir);
        if (cos_angle <= cos_outer) {
            return vec2(0.0, 1.0);
        }
        float denom = max(cos_inner - cos_outer, 1e-4);
        float t_spot = clamp((cos_angle - cos_outer) / denom, 0.0, 1.0);
        spot_atten = t_spot * t_spot * (3.0 - 2.0 * t_spot);
    }

    float u = clamp(dist / radius, 0.0, 1.0);
    float w = 1.0 - u * u;
    float radial = (w * w) / (1.0 + max(falloff, 0.0) * u * u);
    float atten = radial * spot_atten;
    if (atten <= 1e-4) {
        return vec2(0.0, 1.0);
    }
    atten *= surface_factor(p, vec3(to_light, height), mask, first, count);

    float vis = 1.0;
    if (shadow_on) {
        vis = raymarch_shadow(p, ray_dir, dist, source_radius, softness, mask, first, count);
    }
    return vec2(atten, vis);
}

void main() {
    uint kind = (v_flags >> 1u) & 3u;
    bool shadow_on = (v_flags & 1u) != 0u;
    uint blend_mode = (v_flags >> 3u) & 3u;
    uint own_index = (v_flags >> 8u) & 31u;
    vec4 own = scene.light_params1[own_index];
    float own_height = scene.light_params2[own_index].x;
    uint own_first = uint(own.z);
    uint own_count = uint(own.w);

    if (kind == 0u) {
        // Single PointLight2D / SpotLight2D quad
        vec2 av = evaluate_point_light(v_world, v_center, v_params0.z, v_params0.w,
                                       v_params0.xy, v_params1.x, v_params1.y,
                                       v_params1.w, v_params1.z, shadow_on, v_mask, own_first, own_count,
                                       own_height);
        float atten = av.x;
        float vis = av.y;
        float lit_strength = atten * vis * v_energy * v_color.a;
        float shadow_strength = shadow_on ? clamp(atten * (1.0 - vis) * v_shadow_color.a, 0.0, 1.0) : 0.0;

        if (blend_mode == 2u) {
            // Subtract / darken
            float dark = clamp(lit_strength, 0.0, 1.0);
            o_color = vec4(vec3(0.0), dark);
        } else if (blend_mode == 1u) {
            // Mix
            float mix_a = clamp(atten * v_color.a, 0.0, 1.0);
            float out_a = mix(shadow_strength, mix_a * 0.5, vis);
            vec3 out_rgb = mix(v_shadow_color.rgb * shadow_strength, v_color.rgb * lit_strength, vis);
            o_color = vec4(out_rgb, out_a);
        } else {
            // Add (plus shadow_color darkening in shadowed regions inside light radius)
            vec3 lit_rgb = v_color.rgb * lit_strength;
            o_color = vec4(lit_rgb * (1.0 - shadow_strength) + v_shadow_color.rgb * shadow_strength, shadow_strength);
        }
    } else if (kind == 1u) {
        // Single DirectionalLight2D quad
        float vis = shadow_on
            ? raymarch_shadow(v_world, -v_params0.xy, v_params0.z, v_params0.w, v_params1.z, v_mask, own_first, own_count)
            : 1.0;
        // A directional light shines down at 45 degrees onto walls' faces.
        float face = surface_factor(v_world, vec3(-v_params0.xy, 1.0), v_mask, own_first, own_count);
        float lit_strength = vis * face * v_energy * v_color.a;
        float shadow_strength = shadow_on ? clamp((1.0 - vis) * v_shadow_color.a, 0.0, 1.0) : 0.0;
        vec3 lit_rgb = v_color.rgb * lit_strength;
        o_color = vec4(lit_rgb * (1.0 - shadow_strength) + v_shadow_color.rgb * shadow_strength, shadow_strength);
    } else {
        // AmbientLight2D + multi-light composite pass
        float base_darkness = clamp(scene.ambient_color.a, 0.0, 1.0);
        float glow_scale = scene.ambient_params.x;
        float remaining_dark = 1.0;
        vec3 accum_glow = vec3(0.0);
        vec3 accum_shadow_tint = vec3(0.0);

        uint n_lights = scene.counts.y;
        for (uint i = 0u; i < n_lights; ++i) {
            uint lflags = uvec4_at(scene.light_flags[i >> 2u], i & 3u);
            uint lmask = uvec4_at(scene.light_masks[i >> 2u], i & 3u);
            uint lkind = (lflags >> 1u) & 3u;
            bool lshadow = (lflags & 1u) != 0u;
            uint lblend = (lflags >> 3u) & 3u;

            vec4 pos_rad = scene.light_pos_rad[i];
            vec4 col_en = scene.light_color[i];
            vec4 sh_col = scene.light_shadow[i];
            vec4 lp0 = scene.light_params0[i];
            vec4 lp1 = scene.light_params1[i];

            float atten = 0.0;
            float vis = 1.0;
            if (lkind == 1u) {
                atten = surface_factor(v_world, vec3(-lp0.xy, 1.0), lmask, uint(lp1.z), uint(lp1.w));
                if (lshadow) {
                    vis = raymarch_shadow(v_world, -lp0.xy, pos_rad.z, pos_rad.w, lp1.y, lmask, uint(lp1.z), uint(lp1.w));
                }
            } else {
                vec2 av = evaluate_point_light(v_world, pos_rad.xy, pos_rad.z, pos_rad.w,
                                               lp0.xy, lp0.z, lp0.w,
                                               lp1.x, lp1.y, lshadow, lmask, uint(lp1.z), uint(lp1.w),
                                               scene.light_params2[i].x);
                atten = av.x;
                vis = av.y;
            }

            float energy = col_en.a;
            if (lblend == 2u) {
                // Subtract light increases darkness
                float sub = clamp(atten * vis * energy, 0.0, 1.0);
                remaining_dark = clamp(remaining_dark + (1.0 - remaining_dark) * sub, 0.0, 1.0);
            } else {
                float illum = clamp(atten * vis * energy, 0.0, 1.0);
                remaining_dark *= (1.0 - illum);
                accum_glow += col_en.rgb * (atten * vis * energy * glow_scale);
                if (lshadow && vis < 1.0 && atten > 0.0) {
                    float sh = atten * (1.0 - vis) * sh_col.a;
                    accum_shadow_tint += sh_col.rgb * sh;
                }
            }
        }

        float dark_alpha = clamp(base_darkness * remaining_dark, 0.0, 1.0);
        vec3 amb_rgb = (scene.ambient_color.rgb + accum_shadow_tint) * dark_alpha;
        o_color = vec4(amb_rgb + accum_glow, dark_alpha);
    }
}

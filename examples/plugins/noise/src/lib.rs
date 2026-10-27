//! The noise plugin's natives (scripts/noise.plm). Integer hashing plus IEEE
//! arithmetic and `floor`, so results are the same everywhere: sim-safe.

use cuervo_plugin::{cuervo_plugin, Object};

/// A 32-bit integer hash of a lattice point (a murmur3-style finalizer).
fn hash(x: i32, y: i32, seed: i32) -> u32 {
    let mut h = (x as u32).wrapping_mul(0x8da6_b343) ^ (y as u32).wrapping_mul(0xd816_3841) ^ (seed as u32).wrapping_mul(0xcb1a_b31f);
    h ^= h >> 16;
    h = h.wrapping_mul(0x85eb_ca6b);
    h ^= h >> 13;
    h = h.wrapping_mul(0xc2b2_ae35);
    h ^ (h >> 16)
}

/// The lattice value in [-1, 1].
fn lattice(x: i32, y: i32, seed: i32) -> f32 {
    (hash(x, y, seed) >> 8) as f32 / (1u32 << 23) as f32 - 1.0
}

/// Value noise: the lattice values around (x, y), blended with a smoothstep.
fn value(x: f32, y: f32, seed: i32) -> f32 {
    let (fx, fy) = (x.floor(), y.floor());
    let (ix, iy) = (fx as i32, fy as i32);
    let (tx, ty) = (x - fx, y - fy);
    let (sx, sy) = (tx * tx * (3.0 - 2.0 * tx), ty * ty * (3.0 - 2.0 * ty));
    let a = lattice(ix, iy, seed);
    let b = lattice(ix + 1, iy, seed);
    let c = lattice(ix, iy + 1, seed);
    let d = lattice(ix + 1, iy + 1, seed);
    let top = a + (b - a) * sx;
    let bottom = c + (d - c) * sx;
    top + (bottom - top) * sy
}

cuervo_plugin!(noise, |plugin| {
    plugin.function("value", |x: f32, y: f32, seed: i32| value(x, y, seed));
    plugin.function("hash", |x: i32, y: i32, seed: i32| (hash(x, y, seed) & 0x7fff_ffff) as i32);
    plugin.function("describe", || "noise 1.0.0 (Rust)".to_string());
    // Wisp.offset(): the node's displacement at its own time, from its fields.
    plugin.function("Wisp.offset", |wisp: Object| -> [f32; 2] {
        let t = wisp.get_float("t").unwrap_or(0.0) as f32;
        let amp = wisp.get_float("amplitude").unwrap_or(0.0) as f32;
        let seed = wisp.get_int("seed").unwrap_or(0) as i32;
        [value(t, 0.5, seed) * amp, value(0.5, t, seed + 1) * amp]
    });
});

#include "cuervo/particles/particle_render.hpp"

#include <vector>

#include "cuervo/jobs/job_system.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/particles/particle_system.hpp"

namespace cuervo::particles {

namespace {

// SIMD SDF instance packing (DESIGN.md 4.15): interpolation and half-extents
// for whole blocks, then interleaved into the 40-byte instances. Same
// expressions as the scalar loop below. Returns the first index not packed.
std::size_t simd_pack(const Emitter& emitter, std::size_t begin, std::size_t end, float alpha,
                      render::SdfInstance inst, std::span<render::SdfInstance> out) {
#if defined(CUERVO_SIMD_AVX2) || defined(CUERVO_SIMD_SSE2) || defined(CUERVO_SIMD_WASM)
    using simd::F;
    using simd::splat;
    constexpr std::size_t w = simd::width;
    const RenderConfig& rc = emitter.render();
    const ParticlePool& p = emitter.pool();
    const F a = splat(alpha);
    std::size_t i = begin;
    for (; i + w <= end; i += w) {
        const F px = simd::load(&p.prev_x[i]);
        const F py = simd::load(&p.prev_y[i]);
        const F size = simd::load(&p.size[i]);
        alignas(32) float cx[w], cy[w], hx[w], param[w];
        simd::store(cx, px + (simd::load(&p.x[i]) - px) * a);
        simd::store(cy, py + (simd::load(&p.y[i]) - py) * a);
        F half_x = size;          // Circle, RoundedRect, Ring
        F extra = splat(0.0f);    // Circle, Capsule
        switch (rc.shape) {
            case render::SdfShape::Circle: break;
            case render::SdfShape::Capsule: half_x = splat(rc.half_length) * size + size; break;
            case render::SdfShape::RoundedRect: extra = splat(rc.corner) * size; break;
            case render::SdfShape::Ring: extra = splat(rc.thickness) * size; break;
            case render::SdfShape::Sector:
            case render::SdfShape::Arc: break; // drawn as circles (pack_instances)
        }
        simd::store(hx, half_x);
        simd::store(param, extra);
        for (std::size_t k = 0; k < w; ++k) {
            inst.center = {cx[k], cy[k]};
            inst.half_size = {hx[k], p.size[i + k]};
            inst.param = param[k];
            inst.rotation = p.rotation[i + k];
            inst.color = p.color[i + k];
            out[i + k - begin] = inst;
        }
    }
    return i;
#else
    (void)emitter, (void)end, (void)alpha, (void)inst, (void)out;
    return begin;
#endif
}

} // namespace

std::size_t instance_count(const Emitter& emitter) { return emitter.render().visible ? emitter.size() : 0; }

void pack_instances(const Emitter& emitter, std::size_t begin, std::size_t end, float alpha,
                    std::span<render::SdfInstance> out) {
    const RenderConfig& rc = emitter.render();
    const ParticlePool& p = emitter.pool();
    render::SdfInstance inst;
    // Sectors and arcs need angles RenderConfig doesn't carry: particles draw
    // them as circles rather than as invisible zero-angle shapes.
    const bool circle_like = rc.shape == render::SdfShape::Sector || rc.shape == render::SdfShape::Arc;
    inst.shape = circle_like ? render::SdfShape::Circle : rc.shape;
    inst.outline_width = rc.outline_width;
    inst.outline_color = render::pack_rgba8(rc.outline_color);
    const std::size_t first = simd::enabled() ? simd_pack(emitter, begin, end, alpha, inst, out) : begin;
    for (std::size_t i = first; i < end; ++i) {
        const float s = p.size[i];
        inst.center = {p.prev_x[i] + (p.x[i] - p.prev_x[i]) * alpha, p.prev_y[i] + (p.y[i] - p.prev_y[i]) * alpha};
        inst.rotation = p.rotation[i];
        inst.color = p.color[i];
        // Half-extents and parameters as documented on SdfInstance.
        switch (rc.shape) {
            case render::SdfShape::Circle:
                inst.half_size = {s, s};
                inst.param = 0.0f;
                break;
            case render::SdfShape::Capsule:
                inst.half_size = {rc.half_length * s + s, s};
                inst.param = 0.0f;
                break;
            case render::SdfShape::RoundedRect:
                inst.half_size = {s, s};
                inst.param = rc.corner * s;
                break;
            case render::SdfShape::Ring:
                inst.half_size = {s, s};
                inst.param = rc.thickness * s;
                break;
            case render::SdfShape::Sector:
            case render::SdfShape::Arc:
                inst.half_size = {s, s};
                inst.param = 0.0f;
                break;
        }
        out[i - begin] = inst;
    }
}

std::size_t draw_emitter(const Emitter& emitter, render::SdfBatcher& batch, float alpha) {
    const std::size_t n = instance_count(emitter);
    if (n == 0) return 0;
    pack_instances(emitter, 0, n, alpha, batch.append(n));
    return n;
}

std::size_t draw_emitters(const ParticleSystem& system, render::SdfBatcher& batch, float alpha,
                          jobs::JobSystem* jobs) {
    struct Range {
        std::uint32_t emitter;
        std::size_t begin, end, out;
    };
    constexpr std::size_t grain = 4096;
    std::vector<Range> ranges;
    std::size_t total = 0;
    for (std::uint32_t id = 0; id < system.emitter_count(); ++id) {
        const std::size_t n = instance_count(system.emitter(id));
        for (std::size_t begin = 0; begin < n; begin += grain) {
            ranges.push_back({id, begin, std::min(n, begin + grain), total + begin});
        }
        total += n;
    }
    const std::span<render::SdfInstance> out = batch.append(total);
    const auto pack = [&](std::size_t k) {
        const Range& r = ranges[k];
        pack_instances(system.emitter(r.emitter), r.begin, r.end, alpha, out.subspan(r.out, r.end - r.begin));
    };
    if (jobs != nullptr) {
        jobs->run(ranges.size(), pack);
    } else {
        for (std::size_t k = 0; k < ranges.size(); ++k) pack(k);
    }
    return total;
}

} // namespace cuervo::particles

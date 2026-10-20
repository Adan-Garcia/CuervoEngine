#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "cuervo/math/vec.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/rhi/rhi.hpp"

// Instanced analytic SDF shapes (DESIGN.md 4.4 "Vector pipeline"): particles,
// UI panels and primitives. Every shape kind shares one pipeline,
// so a whole batch is one instanced draw. Instances live in one contiguous
// array that is uploaded once per flush.
//
// Coordinates are world units (y down), angles radians (clockwise on screen);
// see camera.hpp.
namespace cuervo::render {

namespace detail {
// std::allocator that leaves default-constructed elements alone. append()
// hands the new instances to a caller that overwrites every one, so
// value-initializing them first was a wasted pass over 40 B each (80 MB a
// frame at 2M particles). Only for trivially copyable element types.
template <class T>
struct UninitializedAllocator : std::allocator<T> {
    static_assert(std::is_trivially_copyable_v<T>);
    template <class U>
    struct rebind {
        using other = UninitializedAllocator<U>;
    };
    UninitializedAllocator() = default;
    template <class U>
    explicit UninitializedAllocator(const UninitializedAllocator<U>&) {}
    template <class U>
    void construct(U*) noexcept {}
    template <class U, class... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
};
} // namespace detail

enum class SdfShape : std::uint32_t {
    Circle = 0,
    Capsule = 1,     // a segment along the local x axis with round caps
    RoundedRect = 2,
    Ring = 3,
    Sector = 4,      // circular sector centered on local x with half-angle param
    Arc = 5,         // annular arc band with round caps, centered on local x
};

// GPU instance layout; must match cuervo/render/shaders/sdf.vert (stream 1).
// `half_size` is always the shape's local bounding half-extent:
//   Circle:      (r, r)                       param: unused
//   Capsule:     (half_length + r, r)         param: unused
//   RoundedRect: (half_w, half_h)             param: corner radius
//   Ring:        (outer_r, outer_r)           param: thickness
//   Sector:      (r, r)                       param: half-angle in radians
//   Arc:         (r + t, t)                   param: half-angle; t = half the band
//                (the quad is square, sized by half_size.x: sdf.vert)
struct SdfInstance {
    vec2 center;
    vec2 half_size;
    float rotation = 0.0f;
    float param = 0.0f;
    float outline_width = 0.0f; // grows the shape outwards; 0 = no outline
    SdfShape shape = SdfShape::Circle;
    std::uint32_t color = 0;         // RGBA8, see pack_rgba8
    std::uint32_t outline_color = 0;
};
static_assert(sizeof(SdfInstance) == 40, "SdfInstance must match the sdf.vert instance stream");

struct SdfOutline {
    float width = 0.0f;
    rhi::Color color{};
};

// Packs straight-alpha color as bytes r, g, b, a in memory (R8G8B8A8_UNORM).
std::uint32_t pack_rgba8(rhi::Color color);

SdfInstance make_circle(vec2 center, float radius, rhi::Color fill, const SdfOutline& outline = {});
SdfInstance make_capsule(vec2 center, float half_length, float radius, float rotation, rhi::Color fill,
                         const SdfOutline& outline = {});
SdfInstance make_rounded_rect(vec2 center, vec2 half_size, float corner_radius, float rotation, rhi::Color fill,
                              const SdfOutline& outline = {});
SdfInstance make_ring(vec2 center, float radius, float thickness, rhi::Color fill, const SdfOutline& outline = {});
SdfInstance make_sector(vec2 center, float radius, float start_angle, float end_angle, rhi::Color fill,
                        const SdfOutline& outline = {});
// A stroke of `width` centered on the circle of `radius`, from start_angle to
// end_angle: one instance, so translucent strokes have no overlapping joints.
SdfInstance make_arc(vec2 center, float radius, float start_angle, float end_angle, float width, rhi::Color color);

// std140 uniform block `Globals` of sdf.vert.
struct SdfGlobals {
    float row0[4]; // world -> NDC affine, first row (a, c, tx, 0)
    float row1[4]; // second row (b, d, ty, 0)
    float params[4]; // x = world units per framebuffer pixel (anti-aliasing padding)
};
static_assert(sizeof(SdfGlobals) == 48);

SdfGlobals make_sdf_globals(const Affine2& world_to_ndc, float world_units_per_pixel);

class SdfBatcher {
public:
    SdfBatcher() = default;
    ~SdfBatcher();
    SdfBatcher(const SdfBatcher&) = delete;
    SdfBatcher& operator=(const SdfBatcher&) = delete;

    // Creates the pipeline and buffers. Without init() the batcher still
    // collects instances (useful for tests) but flush() draws nothing.
    bool init(rhi::Device& device, std::size_t initial_capacity = 1024);
    void shutdown();

    void clear() {
        instances_.clear();
        flushed_ = 0;
    }
    void reserve(std::size_t count) { instances_.reserve(count); }
    void add(const SdfInstance& instance) { instances_.push_back(instance); }
    // Appends `count` instances for the caller to fill (several jobs may fill
    // disjoint parts of the span); the caller must write every one, they start
    // uninitialized. Valid until the next add/append/clear.
    std::span<SdfInstance> append(std::size_t count) {
        const std::size_t first = instances_.size();
        instances_.resize(first + count);
        return std::span<SdfInstance>(instances_).subspan(first, count);
    }

    void circle(vec2 center, float radius, rhi::Color fill, const SdfOutline& outline = {}) {
        add(make_circle(center, radius, fill, outline));
    }
    void capsule(vec2 center, float half_length, float radius, float rotation, rhi::Color fill,
                 const SdfOutline& outline = {}) {
        add(make_capsule(center, half_length, radius, rotation, fill, outline));
    }
    void rounded_rect(vec2 center, vec2 half_size, float corner_radius, float rotation, rhi::Color fill,
                      const SdfOutline& outline = {}) {
        add(make_rounded_rect(center, half_size, corner_radius, rotation, fill, outline));
    }
    void ring(vec2 center, float radius, float thickness, rhi::Color fill, const SdfOutline& outline = {}) {
        add(make_ring(center, radius, thickness, fill, outline));
    }
    void sector(vec2 center, float radius, float start_angle, float end_angle, rhi::Color fill,
                const SdfOutline& outline = {}) {
        add(make_sector(center, radius, start_angle, end_angle, fill, outline));
    }
    // Filled sector (if fill.a > 0) plus a stroke along the circumference (if
    // stroke.a > 0 and width > 0); the straight edges are not stroked.
    void arc(vec2 center, float radius, float start_angle, float end_angle, rhi::Color fill, rhi::Color stroke,
             float width = 1.0f);
    void arc(vec2 center, float radius, float start_angle, float end_angle, rhi::Color stroke, float width = 1.0f);

    std::span<const SdfInstance> instances() const { return instances_; }
    std::size_t size() const { return instances_.size(); }

    // Uploads the instances added since the previous flush (or clear) and
    // draws them with one instanced draw call into the device's current
    // viewport. Call between begin_frame and end_frame. Flushing several
    // times per frame layers shapes with text flushed in between (UI layers,
    // Canvas.new_layer). Instances are kept; call clear() before building the
    // next frame.
    void flush(const Affine2& world_to_ndc, float world_units_per_pixel);

    std::size_t capacity() const { return capacity_; }

private:
    bool ensure_capacity(std::size_t count);

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle quad_vertices_;
    rhi::BufferHandle quad_indices_;
    rhi::BufferHandle instance_buffer_;
    rhi::BufferHandle globals_;
    std::size_t capacity_ = 0;
    std::vector<SdfInstance, detail::UninitializedAllocator<SdfInstance>> instances_;
    std::size_t flushed_ = 0; // instances already drawn this frame
};

} // namespace cuervo::render

#include "svg_viewer.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "cuervo/core/log.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/vector/vector_asset.hpp"

using namespace cuervo;

namespace {

constexpr ivec2 virtual_size{1280, 720};
constexpr rhi::Color background{0.93f, 0.93f, 0.95f, 1.0f};
constexpr rhi::Color letterbox{0.0f, 0.0f, 0.0f, 1.0f};

double now_seconds() {
    return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
}

} // namespace

int run_svg_viewer(Window& window, rhi::Device& device, const SvgViewerOptions& options) {
    vector::VectorAsset asset;
    const double load_start = now_seconds();
    if (!asset.load(options.path)) {
        return EXIT_FAILURE;
    }
    const vector::VectorDocument& doc = asset.document();
    LOG_INFO("Loaded '{}' in {:.1f} ms: {}x{} units, {} shapes, {} gradients, {} clip paths", options.path,
             (now_seconds() - load_start) * 1000.0, doc.size.x(), doc.size.y(), doc.shapes.size(), doc.gradients.size(),
             doc.clip_paths.size());

    render::MeshBatcher batcher;
    if (!batcher.init(device)) {
        return EXIT_FAILURE;
    }

    // Copies laid out on a grid (1 = just the document).
    const int count = std::max(options.instances, 1);
    const int columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + columns - 1) / columns;
    const vec2 cell = doc.size * 1.1f;
    const vec2 extent = vec2(cell.x() * static_cast<float>(columns - 1), cell.y() * static_cast<float>(rows - 1)) + doc.size;

    const vec2 virtual_extent{static_cast<float>(virtual_size.x()), static_cast<float>(virtual_size.y())};
    render::Camera2D camera;
    auto reset_view = [&] {
        camera.position = extent * 0.5f;
        const float fit = std::min(virtual_extent.x() / std::max(extent.x(), 1e-3f),
                                   virtual_extent.y() / std::max(extent.y(), 1e-3f)) * 0.9f;
        camera.zoom = options.zoom > 0.0f ? options.zoom : fit;
    };
    reset_view();
    const float base_zoom = camera.zoom;

    render::MeshHandle mesh;
    std::uint64_t uploaded_revision = 0;
    DeviceState devices;
    const double start = now_seconds();
    double previous = start;
    double stats_start = start;
    double render_seconds = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t total_frames = 0;
    bool running = true;

    while (running) {
        const double now = now_seconds();
        const auto dt = static_cast<float>(std::min(now - previous, 0.1));
        previous = now;
        running = window.poll_events(devices);

        const ivec2 fb = window.pixel_size();
        const render::ViewportFit fit = render::fit_viewport(options.scale, virtual_size, fb);

        // --- view controls (render-side state, not simulation) ---------------------
        // The viewer reads devices directly instead of through an InputMap, so it
        // consumes the wheel and taps itself at the end of each frame.
        const auto key = [&](Key k) { return devices.key(k); };
        if (key(Key::Escape)) running = false;
        if (key(Key::Num0) || key(Key::Kp0)) reset_view();
        float zoom_steps = 0.0f;
        if (key(Key::Equals) || key(Key::KpPlus)) zoom_steps += dt * 3.0f;
        if (key(Key::Minus) || key(Key::KpMinus)) zoom_steps -= dt * 3.0f;
        camera.zoom = std::clamp(camera.zoom * std::pow(1.6f, zoom_steps), 1e-3f, 1e4f);
        if (options.animate_zoom) {
            camera.zoom = base_zoom * std::exp2(4.0f * std::sin(static_cast<float>(now - start) * 1.5f));
        }
        if (const float wheel = devices.take_wheel().y(); wheel != 0.0f) {
            // Zoom about the cursor: keep the world point under it fixed.
            const vec2 cursor = render::framebuffer_to_virtual(fit, devices.mouse_position);
            const vec2 before = camera.virtual_to_world(cursor, virtual_extent);
            camera.zoom = std::clamp(camera.zoom * std::pow(1.25f, wheel), 1e-3f, 1e4f);
            const vec2 after = camera.virtual_to_world(cursor, virtual_extent);
            camera.position += before - after;
        }
        const float pan = 600.0f * dt / camera.zoom; // virtual pixels per second
        if (key(Key::Left)) camera.position.x() -= pan;
        if (key(Key::Right)) camera.position.x() += pan;
        if (key(Key::Up)) camera.position.y() -= pan;
        if (key(Key::Down)) camera.position.y() += pan;
        devices.clear_transients();

        device.begin_frame(fb);
        const double render_start = now_seconds();
        device.clear(letterbox);
        device.set_viewport(fit.viewport);
        device.set_scissor(fit.viewport);
        device.clear(background);

        // Framebuffer pixels per document unit: picks the tessellation level
        // (re-flattened only when it moves by more than 2x; levels are cached).
        const float pixels_per_unit = std::min(fit.scale.x(), fit.scale.y()) * camera.zoom;
        const vector::VectorAsset::Lod lod = asset.mesh_for_scale(pixels_per_unit);
        if (lod.revision != uploaded_revision) {
            if (mesh) {
                batcher.update_mesh(mesh, *lod.mesh);
            } else {
                mesh = batcher.create_mesh(*lod.mesh);
            }
            uploaded_revision = lod.revision;
            LOG_INFO("Mesh level {} (scale {}): {} triangles, {} vertices, {} paint batches", lod.level,
                     vector::lod_scale(lod.level), lod.mesh->triangle_count(), lod.mesh->vertices.size(),
                     lod.mesh->batches.size());
        }
        batcher.clear();
        for (int i = 0; i < count; ++i) {
            const vec2 offset{cell.x() * static_cast<float>(i % columns), cell.y() * static_cast<float>(i / columns)};
            batcher.draw(mesh, Affine2::translate(offset));
        }
        batcher.flush(camera.view_projection(virtual_extent));

        const bool capture = !options.screenshot.empty() && total_frames >= 2;
        if (capture) {
            device.capture_frame(options.screenshot);
        }
        render_seconds += now_seconds() - render_start;
        device.end_frame();
        ++frames;
        ++total_frames;
        if (capture) {
            LOG_INFO("Screenshot written to '{}' (zoom {})", options.screenshot, camera.zoom);
            running = false;
        }

        if (now - stats_start >= 5.0) {
            const double elapsed = now - stats_start;
            LOG_INFO("svg | {} instances | level {} | {} tris/instance | {} draws | render {:.3f} ms/frame (CPU) | "
                     "{:.1f} FPS ({:.3f} ms/frame)",
                     count, lod.level, lod.mesh->triangle_count(), batcher.last_draw_calls(),
                     frames ? render_seconds * 1000.0 / static_cast<double>(frames) : 0.0,
                     static_cast<double>(frames) / elapsed, elapsed * 1000.0 / static_cast<double>(std::max<std::uint64_t>(frames, 1)));
            stats_start = now;
            frames = 0;
            render_seconds = 0.0;
        }
        if (options.quit_after > 0.0 && now - start >= options.quit_after) {
            LOG_INFO("--quit-after reached ({} frames, {} mesh builds)", total_frames, asset.builds());
            running = false;
        }
    }

    batcher.shutdown();
    return EXIT_SUCCESS;
}

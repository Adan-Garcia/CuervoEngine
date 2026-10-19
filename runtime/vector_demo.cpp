#include "vector_demo.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>

#include "cuervo/core/log.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/vector/vector_asset.hpp"

using namespace cuervo;

namespace {

constexpr ivec2 virtual_size{1280, 720};
constexpr rhi::Color background{0.93f, 0.93f, 0.95f, 1.0f};
constexpr rhi::Color letterbox{0.0f, 0.0f, 0.0f, 1.0f};
constexpr rhi::Color ink{0.11f, 0.17f, 0.25f, 1.0f};
constexpr rhi::Color accent{0.85f, 0.25f, 0.38f, 1.0f};
constexpr float base_zoom = 1.5f;    // the whole scene fits at this zoom
constexpr float max_zoom_steps = 5.0f; // animation: base_zoom * 2^0 .. 2^5 (1x..32x)

double now_seconds() {
    return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
}

// Zoomed in, the camera drifts towards the micro text between the legs, so a
// single zoom number (CLI, screenshots) fully determines the view.
vec2 focus_for_zoom(float zoom, vec2 micro_text) {
    const float t = std::clamp((std::log2(std::max(zoom / base_zoom, 1.0f)) - 1.0f) / 3.0f, 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return micro_text * smooth;
}

} // namespace

int run_vector_demo(Window& window, rhi::Device& device, const VectorDemoOptions& options) {
    vector::VectorAsset asset;
    if (!asset.load(options.svg_path)) {
        return EXIT_FAILURE;
    }
    const double font_start = now_seconds();
    std::optional<render::FontAtlas> atlas = render::load_font_file(options.font_path);
    if (!atlas) {
        return EXIT_FAILURE;
    }
    LOG_INFO("Font '{}': {} glyphs, {} kerning pairs, {}x{} MTSDF atlas, loaded in {:.1f} ms", options.font_path,
             atlas->glyphs.size(), atlas->kerning.size(), atlas->width, atlas->height,
             (now_seconds() - font_start) * 1000.0);

    render::MeshBatcher meshes;
    render::TextRenderer text;
    render::SdfBatcher panels;
    if (!meshes.init(device) || !text.init(device) || !panels.init(device, 16)) {
        return EXIT_FAILURE;
    }
    const render::FontHandle font = text.add_font(std::move(*atlas));
    if (!font) {
        return EXIT_FAILURE;
    }

    // --- scene (world units; the character's center is the origin) --------------
    const vec2 doc_size = asset.document().size;
    const Affine2 character = Affine2::translate(doc_size * -0.5f);
    const vec2 micro_text{0.0f, doc_size.y() * 0.5f - 46.0f}; // between the legs
    const vec2 virtual_extent{static_cast<float>(virtual_size.x()), static_cast<float>(virtual_size.y())};
    const render::Camera2D screen = render::Camera2D::screen(virtual_extent);

    render::Camera2D camera;
    bool animate = options.zoom <= 0.0f;
    float zoom = options.zoom > 0.0f ? options.zoom * base_zoom : base_zoom; // --zoom 1 = the whole scene
    vec2 pan{0.0f, 0.0f};

    render::MeshHandle mesh;
    std::uint64_t uploaded_revision = 0;
    DeviceState devices;
    const double start = now_seconds();
    double previous = start;
    double fps_start = start;
    int fps_frames = 0;
    double fps = 0.0;
    std::uint64_t frames = 0;
    bool running = true;

    while (running) {
        const double now = now_seconds();
        const auto dt = static_cast<float>(std::min(now - previous, 0.1));
        previous = now;
        running = window.poll_events(devices);

        // --- view controls (render-side state, not simulation) --------------------
        if (devices.key(Key::Escape)) running = false;
        if (devices.keys_tapped[static_cast<std::size_t>(Key::Space)]) animate = !animate;
        if (devices.key(Key::Num0) || devices.key(Key::Kp0)) {
            zoom = base_zoom;
            pan = {0.0f, 0.0f};
        }
        float zoom_steps = 0.0f;
        if (devices.key(Key::Equals) || devices.key(Key::KpPlus)) zoom_steps += dt * 2.0f;
        if (devices.key(Key::Minus) || devices.key(Key::KpMinus)) zoom_steps -= dt * 2.0f;
        zoom_steps += devices.take_wheel().y() * 0.25f;
        if (zoom_steps != 0.0f) {
            animate = false;
            zoom = std::clamp(zoom * std::exp2(zoom_steps), 0.05f, 1024.0f);
        }
        if (animate) {
            const auto t = static_cast<float>(now - start);
            zoom = base_zoom * std::exp2(max_zoom_steps * 0.5f * (1.0f - std::cos(t * 0.45f)));
        }
        const float pan_speed = 500.0f * dt / zoom;
        if (devices.key(Key::Left)) pan.x() -= pan_speed;
        if (devices.key(Key::Right)) pan.x() += pan_speed;
        if (devices.key(Key::Up)) pan.y() -= pan_speed;
        if (devices.key(Key::Down)) pan.y() += pan_speed;
        devices.clear_transients();
        camera.zoom = zoom;
        camera.position = focus_for_zoom(zoom, micro_text) + pan;

        const ivec2 fb = window.pixel_size();
        const render::ViewportFit fit = render::fit_viewport(options.scale, virtual_size, fb);
        device.begin_frame(fb);
        device.clear(letterbox);
        device.set_viewport(fit.viewport);
        device.set_scissor(fit.viewport);
        device.clear(background);

        // SVG: the mesh is re-flattened whenever the zoom crosses a power of
        // two, so curves stay smooth at every magnification.
        const float pixels_per_unit = std::min(fit.scale.x(), fit.scale.y()) * camera.zoom;
        const vector::VectorAsset::Lod lod = asset.mesh_for_scale(pixels_per_unit);
        if (lod.revision != uploaded_revision) {
            if (mesh) {
                meshes.update_mesh(mesh, *lod.mesh);
            } else {
                mesh = meshes.create_mesh(*lod.mesh);
            }
            uploaded_revision = lod.revision;
        }
        const Affine2 world_to_ndc = camera.view_projection(virtual_extent);
        meshes.clear();
        meshes.draw(mesh, character);
        meshes.flush(world_to_ndc);

        // World-space text: one atlas, magnified up to ~100x by the zoom.
        text.clear();
        render::TextStyle title;
        title.size = 30.0f;
        title.align = render::TextAlign::Center;
        title.baseline = render::TextBaseline::Bottom;
        text.draw(font, "Bolt, an SVG character", vec2{0.0f, -doc_size.y() * 0.5f - 14.0f}, title, ink);

        render::TextStyle caption;
        caption.size = 11.0f;
        caption.align = render::TextAlign::Center;
        text.draw(font, "Tessellated mesh, re-flattened per zoom level\nMSDF text from one 48 px atlas",
                  vec2{0.0f, doc_size.y() * 0.5f + 14.0f}, caption, ink);

        render::TextStyle paragraph;
        paragraph.size = 10.0f;
        paragraph.max_width = 150.0f;
        text.draw(font,
                  "Vector-first rendering: analytic SDF shapes, tessellated SVG and multi-channel signed "
                  "distance field text. Latin-1 too: Grüße, café, señor, ½ °C.",
                  vec2{-doc_size.x() * 0.5f - 175.0f, -60.0f}, paragraph, ink);

        render::TextStyle wave;
        wave.size = 18.0f;
        text.draw(font, "Hello!", Affine2::translate({doc_size.x() * 0.5f + 8.0f, -110.0f}) * Affine2::rotate(-0.35f),
                  wave, accent);

        render::TextStyle micro;
        micro.size = 2.2f;
        micro.align = render::TextAlign::Center;
        micro.baseline = render::TextBaseline::Middle;
        text.draw(font, "SN 2D-0001", micro_text, micro, ink);
        micro.size = 0.9f;
        text.draw(font, "still sharp at 32x", micro_text + vec2{0.0f, 2.2f}, micro, accent);
        text.flush(world_to_ndc);

        // Screen-space HUD on top: SDF panel + text with the screen camera.
        ++fps_frames;
        if (now - fps_start >= 0.5) {
            fps = fps_frames / (now - fps_start);
            fps_start = now;
            fps_frames = 0;
        }
        // Screenshots leave out the frame rate so captures are reproducible.
        const std::string rate = options.screenshot.empty() ? std::format("   {:.0f} FPS", fps) : std::string();
        const std::string hud = std::format("zoom {:.2f}x   mesh level {} ({} triangles){}   {}", zoom / base_zoom,
                                            lod.level, lod.mesh->triangle_count(), rate,
                                            rhi::backend_name(device.info().backend));
        render::TextStyle hud_style;
        hud_style.size = 16.0f;
        const render::TextLayout hud_layout = render::layout_text(*text.font(font), hud, hud_style);
        panels.clear();
        const vec2 hud_origin{24.0f, 22.0f};
        const vec2 half = hud_layout.size() * 0.5f + vec2{12.0f, 8.0f};
        panels.rounded_rect(hud_origin + hud_layout.size() * 0.5f, half, 8.0f, 0.0f, {1.0f, 1.0f, 1.0f, 0.85f},
                            {1.5f, {0.11f, 0.17f, 0.25f, 0.6f}});
        const float hud_pixels = std::min(fit.scale.x(), fit.scale.y());
        panels.flush(screen.view_projection(virtual_extent), hud_pixels > 0.0f ? 1.0f / hud_pixels : 1.0f);
        text.draw_layout(font, hud_layout, Affine2::translate(hud_origin), ink);
        render::TextStyle help;
        help.size = 13.0f;
        help.baseline = render::TextBaseline::Bottom;
        text.draw(font, "+/- or wheel: zoom    arrows: pan    space: animate    0: reset", vec2{24.0f, 700.0f}, help,
                  {0.11f, 0.17f, 0.25f, 0.7f});
        text.flush(screen.view_projection(virtual_extent));

        const bool capture = !options.screenshot.empty() && frames >= 2;
        if (capture) {
            device.capture_frame(options.screenshot);
        }
        device.end_frame();
        ++frames;
        if (capture) {
            LOG_INFO("Screenshot written to '{}' (zoom {:.2f}x, mesh level {})", options.screenshot, zoom / base_zoom,
                     lod.level);
            running = false;
        }
        if (options.quit_after > 0.0 && now - start >= options.quit_after) {
            LOG_INFO("--quit-after reached ({} frames, {} mesh builds, last zoom {:.2f}x)", frames, asset.builds(),
                     zoom / base_zoom);
            running = false;
        }
    }

    panels.shutdown();
    text.shutdown();
    meshes.shutdown();
    return EXIT_SUCCESS;
}

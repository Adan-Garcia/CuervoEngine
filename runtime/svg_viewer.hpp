#pragma once

#include <string>

#include "cuervo/platform/window.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/rhi/rhi.hpp"

// `cuervo --svg <file>`: shows an SVG (or ImportedVector .evec) file centered,
// rendered with the vector mesh pipeline (cuervo/vector + render::MeshBatcher).
//   mouse wheel / + / -   zoom (wheel zooms about the cursor)
//   arrow keys            pan
//   0                     reset the view
//   Escape                quit
struct SvgViewerOptions {
    std::string path;
    std::string screenshot;      // capture one frame and exit (deterministic view)
    float zoom = 0.0f;           // 0 = fit the document to the view
    int instances = 1;           // > 1: a grid of copies (instancing / perf test)
    bool animate_zoom = false;   // sweep the zoom over 1/16x..16x (exercises level changes)
    double quit_after = 0.0;
    cuervo::render::ScaleMode scale = cuervo::render::ScaleMode::Fit;
};

int run_svg_viewer(cuervo::Window& window, cuervo::rhi::Device& device, const SvgViewerOptions& options);

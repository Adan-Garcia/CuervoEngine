#pragma once

#include <string>

#include "cuervo/platform/window.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/rhi/rhi.hpp"

// `cuervo --demo vector`: DESIGN.md 6 milestone 2's "an SVG character drawn
// sharp at any zoom". An SVG character (tessellated mesh, re-flattened per
// zoom level) with MSDF text in world space -- a title, a wrapped paragraph,
// rotated text and micro text that only becomes readable when zoomed in --
// plus a screen-space HUD. The zoom sweeps 1x..32x unless --zoom fixes it.
//   + / -        zoom          arrows   pan
//   Space        toggle the zoom animation
//   0            reset         Escape   quit
struct VectorDemoOptions {
    std::string svg_path;    // the character
    std::string font_path;   // TTF/OTF or .efont
    std::string screenshot;  // capture the third frame and exit
    float zoom = 0.0f;       // relative to the fitted scene (1 = all of it); 0 = animate 1x..32x
    double quit_after = 0.0;
    cuervo::render::ScaleMode scale = cuervo::render::ScaleMode::Fit;
};

int run_vector_demo(cuervo::Window& window, cuervo::rhi::Device& device, const VectorDemoOptions& options);

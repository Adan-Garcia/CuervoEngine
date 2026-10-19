#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cuervo/brand/brand.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/vector/vector_asset.hpp"

// The editor's splash (DESIGN.md 4.16, milestone 8.1): the Cuervo Mark and the
// wordmark, as the kit's horizontal lockup, on the theme's page color. The art
// is the synced SVGs (assets/brand/logo) drawn by the engine's own importer and
// mesh renderer (DESIGN.md 4.5), so it is sharp at any window size: the
// two-tone mark for light or dark, the wordmark's currentColor filled with the
// mark's body color. The welcome screen and project picker are milestone 8.3.
namespace cuervo::editor {

class Splash {
public:
    // `brand_dir` holds logo/*.svg; `display_font` is a TTF for the caption
    // (Outfit). Returns false when the mark can't be imported (no splash then).
    bool init(rhi::Device& device, render::TextRenderer& text, const std::string& brand_dir,
              const std::string& display_font);
    void shutdown();
    bool ready() const { return ready_; }

    // Covers the whole framebuffer (`size`, pixels) at `opacity`; call after
    // the UI's draws so it sits on top.
    void render(rhi::Device& device, render::SdfBatcher& shapes, render::TextRenderer& text, ivec2 size,
                brand::Theme theme, float opacity, std::string_view caption);

    // Only the lockup (no page, no caption), `width` pixels wide with its middle at `center`
    // (pixels from the top left), over whatever is already drawn: the welcome screen's mark.
    // Call after the UI's draws, like render().
    void render_lockup(rhi::Device& device, ivec2 size, brand::Theme theme, vec2 center, float width);

private:
    struct Part {
        vector::VectorAsset asset;
        render::MeshHandle mesh;
        std::uint64_t revision = 0;
    };
    // [theme][0 = mark, 1 = wordmark]
    std::array<std::array<Part, 2>, 2> parts_;
    render::MeshBatcher meshes_;
    render::FontHandle font_;
    bool ready_ = false;

    void draw_part(Part& part, const Affine2& transform, float pixels_per_unit, rhi::Color tint);
};

} // namespace cuervo::editor

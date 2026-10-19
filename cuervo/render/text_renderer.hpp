#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "cuervo/math/affine2.hpp"
#include "cuervo/render/font.hpp"
#include "cuervo/render/text_layout.hpp"
#include "cuervo/rhi/rhi.hpp"

// MSDF text renderer (DESIGN.md 4.4 "Text"). Fonts are FontAtlas objects
// uploaded once as RGBA8 textures; each frame the caller queues text (laid
// out by text_layout.hpp) and flush() draws every glyph as an instanced quad:
// one instanced draw per run of consecutive text using the same font, so
// painter's order between texts is kept and a single-font UI is one draw.
//
// Same conventions as Camera2D: world units, y down, radians clockwise.
namespace cuervo::render {

struct FontHandle {
    std::uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    friend bool operator==(FontHandle, FontHandle) = default;
};

// GPU instance layout; must match text.vert (stream 1).
struct GlyphInstance {
    vec2 origin;   // world position of the quad's top-left corner
    vec2 axis_x;   // world vector along the quad's width
    vec2 axis_y;   // world vector along the quad's height
    vec2 uv_min;   // atlas uv at the top-left corner
    vec2 uv_max;   // atlas uv at the bottom-right corner
    std::uint32_t color = 0; // RGBA8 straight alpha (pack_rgba8)
    float distance_range = 0.0f; // the font's distance range, atlas texels
};
static_assert(sizeof(GlyphInstance) == 48, "GlyphInstance must match the text.vert instance stream");

// std140 block `Globals` of text.vert.
struct TextGlobals {
    float row0[4];
    float row1[4];
};
static_assert(sizeof(TextGlobals) == 32);

// Maps a laid-out glyph through `transform` into a GPU instance.
GlyphInstance make_glyph_instance(const LaidOutGlyph& glyph, const Affine2& transform, std::uint32_t color,
                                  float distance_range);

class TextRenderer {
public:
    struct DrawCall {
        FontHandle font;
        std::uint32_t first_instance = 0;
        std::uint32_t instance_count = 0;
    };

    TextRenderer() = default;
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Creates the pipeline and buffers. Without init() the renderer still
    // keeps fonts, lays out text and plans draws (for tests) but draws nothing.
    bool init(rhi::Device& device);
    void shutdown();

    // Takes the atlas and uploads it (if initialized). Returns an invalid
    // handle if the atlas fails validation or the upload fails.
    FontHandle add_font(FontAtlas atlas);
    void remove_font(FontHandle font);
    const FontAtlas* font(FontHandle font) const;
    // The font's atlas texture (invalid before init); e.g. for atlas debug views.
    rhi::TextureHandle texture(FontHandle font) const;

    void clear() {
        instances_.clear();
        runs_.clear();
        flushed_instances_ = 0;
        flushed_runs_ = 0;
    }

    // Lays out `text` and queues its glyphs, placed by `transform` (layout
    // space -> world). Returns the layout (box in layout space) for callers
    // that want to size backgrounds or hit-test.
    TextLayout draw(FontHandle font, std::string_view text, const Affine2& transform, const TextStyle& style,
                    rhi::Color color);
    TextLayout draw(FontHandle font, std::string_view text, vec2 position, const TextStyle& style, rhi::Color color) {
        return draw(font, text, Affine2::translate(position), style, color);
    }
    // Queues an already laid-out text (cache layouts of static text).
    void draw_layout(FontHandle font, const TextLayout& layout, const Affine2& transform, rhi::Color color);

    std::span<const GlyphInstance> instances() const { return instances_; }
    // The draw calls flush() issues for the queued text.
    std::span<const DrawCall> plan() const { return runs_; }

    // Uploads the text queued since the previous flush (or clear) and draws it
    // into the current viewport with `world_to_ndc`. Call between begin_frame
    // and end_frame. Flushing several times per frame layers text drawn with
    // different matrices (world labels, then a screen-space HUD) in order.
    // Instances are kept; clear() before building the next frame.
    void flush(const Affine2& world_to_ndc);

    std::size_t last_draw_calls() const { return last_draw_calls_; } // of the last flush

private:
    struct Font {
        FontAtlas atlas;
        rhi::TextureHandle texture;
    };

    bool ensure_capacity(std::size_t count);

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle quad_vertices_;
    rhi::BufferHandle quad_indices_;
    rhi::BufferHandle instance_buffer_;
    rhi::BufferHandle globals_;
    std::size_t capacity_ = 0;
    std::map<std::uint32_t, Font> fonts_;
    std::uint32_t next_font_ = 1;
    std::vector<GlyphInstance> instances_;
    std::vector<DrawCall> runs_;
    std::size_t flushed_instances_ = 0; // already drawn this frame
    std::size_t flushed_runs_ = 0;
    std::size_t last_draw_calls_ = 0;
};

} // namespace cuervo::render

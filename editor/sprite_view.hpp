#pragma once
// State and logic of the Sprite drawer tab (DESIGN.md 4.13, 11.7c): which image or atlas frame is
// previewed, the zoom and pan of the preview, and the grid slice tool. UI-free so the RmlUi shell and
// tests share it; the ImGui Sprites panel keeps its own copy of the same rules.
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cuervo/assets/image.hpp"
#include "cuervo/assets/sprite_sheet.hpp"
#include "editor/sprite_tools.hpp"

namespace cuervo::editor {

class SpriteView {
public:
    // Points the view at `asset` (project-relative); reloads only when it changed or `force`.
    void set_path(const std::filesystem::path& dir, const std::string& asset, bool force = false);
    const std::string& path() const { return path_; }
    bool is_image() const;
    bool is_atlas() const { return atlas_.has_value(); }
    bool is_packed() const { return path_.ends_with(".catlas"); }
    const std::vector<std::string>& frames() const { return frames_; }
    const std::string& frame() const { return frame_; }
    void set_frame(const std::string& f) { frame_ = f; }
    const ImageInfo& info() const { return info_; }
    const std::string& status() const { return status_; }

    // The pixels to show: image + rectangle (w == 0 whole image). Empty image: nothing to show.
    // `pixels` is set for what no image file holds (an SVG rasterized, one frame of a packed .catlas
    // cropped out); `image` is then a key that changes whenever the pixels do.
    struct Shown { std::string image; int x = 0, y = 0, w = 0, h = 0; int full_w = 0, full_h = 0; std::shared_ptr<const assets::Image> pixels; };
    std::optional<Shown> shown(const std::filesystem::path& dir) const;

    // Preview viewport (screen units of the view box).
    float zoom = 2.0f, pan_x = 0.0f, pan_y = 0.0f;
    void fit(float view_w, float view_h, float image_w, float image_h);
    // Zoom about a point of the view box, keeping the image point under it fixed.
    void zoom_about(float factor, float cx, float cy);

    // Slice tool.
    bool show_grid = true;
    int fw = 16, fh = 16, margin = 0, spacing = 0;
    std::string prefix = "frame", target;
    assets::GridOptions grid() const;
    // Frames the grid yields, or an error text.
    std::vector<assets::SheetFrame> slice(std::string& error) const;
    bool add_sheet(const std::filesystem::path& dir, std::string& message);
    bool add_whole(const std::filesystem::path& dir, std::string& message);
    bool remove_sprite(std::size_t i);
    bool remove_sheet(std::size_t i);
    bool atlas_dirty() const { return atlas_ && atlas_->dirty(); }
    bool save_atlas(std::string& error);
    const AtlasEditor* atlas() const { return atlas_ ? &*atlas_ : nullptr; }

private:
    bool open_target(const std::filesystem::path& dir, std::optional<AtlasEditor>& out, std::string& error);
    void reload(const std::filesystem::path& dir);

    std::string path_, frame_, status_;
    std::vector<std::string> frames_;
    ImageInfo info_;
    std::optional<AtlasEditor> atlas_;
    unsigned generation_ = 0;                          // bumps on every reload (keys the in-memory previews)
    mutable std::shared_ptr<const assets::Image> raster_;
    mutable std::string raster_key_;
    std::optional<Shown> shown_raster(const std::filesystem::path& dir) const;
};

} // namespace cuervo::editor

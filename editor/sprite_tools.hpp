#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/assets/sprite_sheet.hpp"

// Raster tooling of the editor (DESIGN.md 4.13 / 4.19 "11.2a", milestone 11.7c),
// headless so command scripts and tests drive exactly what the Sprites panel does:
//  - image_info: the size of an image file (PNG, JPEG, QOI) for the preview;
//  - AtlasEditor: edits an `*.atlas` source file (which images and sprite-sheet
//    grids pack into one atlas), lists the frames it will have, and writes the
//    JSON back byte-deterministically;
//  - atlas_frame_names: the frame names of an `.atlas` or packed `.catlas`, for
//    the Sprite2D frame picker and AnimatedSprite2D's frame list.
// Paths are project-relative with '/' separators.
namespace cuervo::editor {

struct ImageInfo {
    bool ok = false;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string error;
};
ImageInfo image_info(const std::filesystem::path& project_dir, std::string_view path);

class AtlasEditor {
public:
    // A new, empty atlas that saves to `path`.
    static AtlasEditor create(const std::filesystem::path& project_dir, std::string path);
    // Reads `path` (an `.atlas` file).
    static std::optional<AtlasEditor> open(const std::filesystem::path& project_dir, std::string path,
                                           std::string& error);

    const assets::AtlasSpec& spec() const { return spec_; }
    const std::string& path() const { return path_; }
    bool dirty() const { return dirty_; }

    // Adds one image or SVG (named by its path without extension unless `name`).
    bool add_sprite(std::string_view image, std::string_view name, float scale, std::string& error);
    // Adds a sprite sheet sliced by a fixed grid; the image must exist.
    bool add_sheet(std::string_view image, const assets::GridOptions& grid, std::string& error);
    bool remove_sprite(std::size_t index);
    bool remove_sheet(std::size_t index);
    void set_packing(std::int32_t max_size, std::int32_t padding, bool extrude, bool power_of_two);

    // The atlas's frames in order (sprites first, then each sheet's), as the
    // packer will name them. A sheet whose image can't be read contributes no
    // frames and sets `error` (when given).
    std::vector<std::string> frame_names(std::string* error = nullptr) const;
    // The slices of sheet `index`, for the overlay on its image.
    std::optional<assets::SpriteSheet> sheet_frames(std::size_t index, std::string& error) const;

    // Where the pixels of frame `name` are: an image (project-relative) and the rectangle in it
    // (w == 0: the whole image). Nullopt when the name is unknown or the image can't be read.
    struct FrameSource { std::string image; std::int32_t x = 0, y = 0, w = 0, h = 0; };
    std::optional<FrameSource> frame_source(std::string_view name) const;

    std::string to_json() const;
    bool save(std::string& error);

private:
    std::filesystem::path dir_;
    std::string path_;
    assets::AtlasSpec spec_;
    bool dirty_ = false;
};

// Frame names of an atlas asset: an `.atlas` (from its spec) or `.catlas`.
std::vector<std::string> atlas_frame_names(const std::filesystem::path& project_dir, std::string_view path,
                                           std::string& error);

} // namespace cuervo::editor

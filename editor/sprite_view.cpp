#include "editor/sprite_view.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>

#include "cuervo/assets/atlas_file.hpp"
#include "cuervo/assets/svg_raster.hpp"

namespace cuervo::editor {

namespace {
bool image_ext(const std::string& s) {
    for (const char* e : {".png", ".jpg", ".jpeg", ".qoi"}) if (s.ends_with(e)) return true;
    return false;
}
} // namespace

bool SpriteView::is_image() const { return image_ext(path_); }

void SpriteView::set_path(const std::filesystem::path& dir, const std::string& asset, bool force) {
    if (!force && asset == path_) return;
    const bool same = asset == path_;
    path_ = asset;
    if (!same) frame_.clear();
    reload(dir);
}

void SpriteView::reload(const std::filesystem::path& dir) {
    ++generation_;
    raster_.reset();
    raster_key_.clear();
    info_ = ImageInfo{};
    frames_.clear();
    atlas_.reset();
    status_.clear();
    if (path_.empty()) return;
    if (image_ext(path_)) {
        info_ = image_info(dir, path_);
        if (!info_.ok) status_ = info_.error;
        if (target.empty()) target = path_.substr(0, path_.rfind('.')) + ".atlas";
        fw = std::clamp(fw, 1, std::max(info_.width, 1));
        fh = std::clamp(fh, 1, std::max(info_.height, 1));
    } else if (path_.ends_with(".atlas")) {
        std::string error;
        atlas_ = AtlasEditor::open(dir, path_, error);
        if (!atlas_) status_ = error;
        else frames_ = atlas_->frame_names(&status_);
    } else if (path_.ends_with(".catlas")) {
        std::string error;
        frames_ = atlas_frame_names(dir, path_, error);
        status_ = error;
    } else if (path_.ends_with(".svg")) {
        status_.clear(); // rasterized on demand in shown()
    } else {
        status_ = "Only images and atlases are previewed here.";
    }
    if (frame_.empty() || std::find(frames_.begin(), frames_.end(), frame_) == frames_.end()) {
        frame_ = frames_.empty() ? std::string() : frames_.front();
    }
}

// SVG and packed atlases have no pixels on disk the image decoder can read: rasterize the SVG (at least
// 256 px on its long side so it stays sharp at the preview's default zoom) or crop the frame out of the
// .catlas page, and hand the pixels over by key.
std::optional<SpriteView::Shown> SpriteView::shown_raster(const std::filesystem::path& dir) const {
    const bool svg = path_.ends_with(".svg");
    if (!svg && !(path_.ends_with(".catlas") && !frame_.empty())) return std::nullopt;
    std::string key = std::format("{}/{}/{}", path_, svg ? std::string() : frame_, generation_);
    for (char& c : key) if (std::isalnum(static_cast<unsigned char>(c)) == 0) c = '_'; // survives as an RmlUi source string

    if (key != raster_key_) {
        raster_.reset();
        raster_key_ = key;
        std::ifstream in(dir / path_, std::ios::binary);
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && raw.empty()) return std::nullopt;
        if (svg) {
            const std::string text(raw.begin(), raw.end());
            assets::SvgRasterResult probe = assets::rasterize_svg(text, 1.0f, path_);
            if (!probe.raster) return std::nullopt;
            const float longest = std::max(probe.raster->document_size.x(), probe.raster->document_size.y());
            const float scale = longest > 0.0f && longest < 256.0f ? 256.0f / longest : 1.0f;
            if (scale > 1.0f) probe = assets::rasterize_svg(text, scale, path_);
            if (probe.raster) raster_ = std::make_shared<assets::Image>(std::move(probe.raster->image));
        } else if (const auto atlas = assets::decode_catlas({reinterpret_cast<const std::byte*>(raw.data()), raw.size()})) {
            const assets::AtlasFrame* f = atlas->find(frame_);
            if (f != nullptr && f->page >= 0 && static_cast<std::size_t>(f->page) < atlas->pages.size()) {
                const assets::TextureData& page = atlas->pages[static_cast<std::size_t>(f->page)];
                auto out = std::make_shared<assets::Image>();
                out->width = f->w;
                out->height = f->h;
                out->rgba.assign(static_cast<std::size_t>(f->w) * static_cast<std::size_t>(f->h) * 4, 0);
                const auto base = page.level(0);
                for (int y = 0; y < f->h; ++y) {
                    for (int x = 0; x < f->w; ++x) {
                        const std::size_t src = (static_cast<std::size_t>(f->y + y) * static_cast<std::size_t>(page.width) + static_cast<std::size_t>(f->x + x)) * 4;
                        if (src + 4 > base.size()) continue;
                        std::copy_n(&base[src], 4, &out->rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(f->w) + static_cast<std::size_t>(x)) * 4]);
                    }
                }
                raster_ = out;
            }
        }
    }
    if (!raster_ || raster_->width <= 0) return std::nullopt;
    Shown s;
    s.image = key;
    s.pixels = raster_;
    s.full_w = s.w = raster_->width;
    s.full_h = s.h = raster_->height;
    return s;
}

std::optional<SpriteView::Shown> SpriteView::shown(const std::filesystem::path& dir) const {
    if (auto r = shown_raster(dir)) return r;
    Shown s;
    if (image_ext(path_) && info_.ok) {
        s.image = path_;
        s.full_w = s.w = info_.width;
        s.full_h = s.h = info_.height;
        return s;
    }
    if (!atlas_ || frame_.empty()) return std::nullopt;
    const auto src = atlas_->frame_source(frame_);
    if (!src || !image_ext(src->image)) return std::nullopt;
    const ImageInfo i = image_info(dir, src->image);
    if (!i.ok) return std::nullopt;
    s.image = src->image;
    s.x = src->x;
    s.y = src->y;
    s.full_w = i.width;
    s.full_h = i.height;
    s.w = src->w > 0 ? src->w : i.width;
    s.h = src->h > 0 ? src->h : i.height;
    return s;
}

void SpriteView::fit(float vw, float vh, float iw, float ih) {
    if (iw <= 0 || ih <= 0 || vw <= 0 || vh <= 0) return;
    zoom = std::clamp(std::min(vw / iw, vh / ih) * 0.9f, 0.25f, 32.0f);
    if (zoom > 1.0f) zoom = std::floor(zoom * 2.0f) / 2.0f; // whole or half steps keep pixels crisp
    pan_x = (vw - iw * zoom) * 0.5f;
    pan_y = (vh - ih * zoom) * 0.5f;
}

void SpriteView::zoom_about(float factor, float cx, float cy) {
    const float z = std::clamp(zoom * factor, 0.25f, 32.0f);
    const float k = z / zoom;
    pan_x = cx - (cx - pan_x) * k;
    pan_y = cy - (cy - pan_y) * k;
    zoom = z;
}

assets::GridOptions SpriteView::grid() const {
    assets::GridOptions g;
    g.frame_w = fw;
    g.frame_h = fh;
    g.margin = margin;
    g.spacing = spacing;
    g.prefix = prefix;
    return g;
}

std::vector<assets::SheetFrame> SpriteView::slice(std::string& error) const {
    if (!info_.ok) { error = "no image"; return {}; }
    assets::SheetResult r = assets::slice_grid(info_.width, info_.height, grid());
    if (!r.sheet) { error = r.error; return {}; }
    return r.sheet->frames;
}

bool SpriteView::open_target(const std::filesystem::path& dir, std::optional<AtlasEditor>& out, std::string& error) {
    if (target.empty() || !target.ends_with(".atlas")) { error = "Atlas file name must end in .atlas"; return false; }
    out = std::filesystem::exists(dir / target) ? AtlasEditor::open(dir, target, error) : std::optional<AtlasEditor>(AtlasEditor::create(dir, target));
    return out.has_value();
}

bool SpriteView::add_sheet(const std::filesystem::path& dir, std::string& message) {
    std::string error;
    std::string e2;
    const auto frames = slice(e2);
    if (frames.empty()) { message = e2.empty() ? "The grid yields no frames." : e2; return false; }
    std::optional<AtlasEditor> t;
    if (!open_target(dir, t, error) || !t->add_sheet(path_, grid(), error) || !t->save(error)) { message = error; return false; }
    message = std::format("Added {} frames to {}", frames.size(), target);
    return true;
}

bool SpriteView::add_whole(const std::filesystem::path& dir, std::string& message) {
    std::string error;
    std::optional<AtlasEditor> t;
    if (!open_target(dir, t, error) || !t->add_sprite(path_, "", 1.0f, error) || !t->save(error)) { message = error; return false; }
    message = std::format("Added {} to {}", path_, target);
    return true;
}

bool SpriteView::remove_sprite(std::size_t i) {
    if (!atlas_ || !atlas_->remove_sprite(i)) return false;
    frames_ = atlas_->frame_names(&status_);
    return true;
}

bool SpriteView::remove_sheet(std::size_t i) {
    if (!atlas_ || !atlas_->remove_sheet(i)) return false;
    frames_ = atlas_->frame_names(&status_);
    return true;
}

bool SpriteView::save_atlas(std::string& error) { return atlas_ && atlas_->save(error); }

} // namespace cuervo::editor

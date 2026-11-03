#include "cuervo/assets/atlas.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace cuervo::assets {

namespace {

struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
};

bool contains(const Rect& a, const Rect& b) {
    return b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
}

class MaxRects {
public:
    MaxRects(std::int32_t w, std::int32_t h) : free_{{0, 0, w, h}} {}

    // Best short side fit; ties by long side, then free-rect order.
    bool place(std::int32_t w, std::int32_t h, Rect& out) {
        std::int64_t best_short = INT64_MAX;
        std::int64_t best_long = INT64_MAX;
        const Rect* best = nullptr;
        for (const Rect& f : free_) {
            if (f.w < w || f.h < h) {
                continue;
            }
            const std::int64_t dw = f.w - w;
            const std::int64_t dh = f.h - h;
            const std::int64_t short_side = std::min(dw, dh);
            const std::int64_t long_side = std::max(dw, dh);
            if (short_side < best_short || (short_side == best_short && long_side < best_long)) {
                best_short = short_side;
                best_long = long_side;
                best = &f;
            }
        }
        if (best == nullptr) {
            return false;
        }
        out = {best->x, best->y, w, h};
        split(out);
        return true;
    }

private:
    void split(const Rect& used) {
        std::vector<Rect> next;
        next.reserve(free_.size() + 4);
        for (const Rect& f : free_) {
            const bool overlap = used.x < f.x + f.w && used.x + used.w > f.x && used.y < f.y + f.h &&
                                 used.y + used.h > f.y;
            if (!overlap) {
                next.push_back(f);
                continue;
            }
            if (used.x > f.x) {
                next.push_back({f.x, f.y, used.x - f.x, f.h});
            }
            if (used.x + used.w < f.x + f.w) {
                next.push_back({used.x + used.w, f.y, f.x + f.w - (used.x + used.w), f.h});
            }
            if (used.y > f.y) {
                next.push_back({f.x, f.y, f.w, used.y - f.y});
            }
            if (used.y + used.h < f.y + f.h) {
                next.push_back({f.x, used.y + used.h, f.w, f.y + f.h - (used.y + used.h)});
            }
        }
        // Drop free rects contained in another one (keep the first of equals).
        std::vector<Rect> pruned;
        pruned.reserve(next.size());
        for (std::size_t i = 0; i < next.size(); ++i) {
            bool dominated = false;
            for (std::size_t j = 0; j < next.size() && !dominated; ++j) {
                if (i != j && contains(next[j], next[i]) && (!contains(next[i], next[j]) || j < i)) {
                    dominated = true;
                }
            }
            if (!dominated) {
                pruned.push_back(next[i]);
            }
        }
        free_ = std::move(pruned);
    }

    std::vector<Rect> free_;
};

struct Placement {
    std::size_t sprite = 0; // index into the sorted order
    Rect rect;              // sprite texels
};

// Greedily packs `order` into a w x h page; sprites that don't fit are left for
// the next page. `slot` is a sprite's footprint with extrusion and padding.
// The page is grown by `pad` on each axis so the last sprite needs no trailing gap.
std::vector<Placement> try_pack(const std::vector<AtlasInput>& in, const std::vector<std::size_t>& order,
                                std::int32_t w, std::int32_t h, std::int32_t ext, std::int32_t pad,
                                std::vector<std::size_t>& leftover) {
    MaxRects mr(w + pad, h + pad);
    std::vector<Placement> placed;
    leftover.clear();
    for (const std::size_t i : order) {
        const Image& im = in[i].image;
        Rect r;
        if (mr.place(im.width + 2 * ext + pad, im.height + 2 * ext + pad, r)) {
            placed.push_back({i, {r.x + ext, r.y + ext, im.width, im.height}});
        } else {
            leftover.push_back(i);
        }
    }
    return placed;
}

std::int32_t next_pot(std::int32_t v) {
    std::int32_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

Image render_page(const std::vector<AtlasInput>& in, const std::vector<Placement>& placed, std::int32_t w,
                  std::int32_t h, std::int32_t ext) {
    Image page;
    page.width = w;
    page.height = h;
    page.rgba.assign(std::size_t(w) * std::size_t(h) * 4, 0);
    auto px = [&](std::int32_t x, std::int32_t y) { return &page.rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4]; };
    for (const Placement& p : placed) {
        const Image& im = in[p.sprite].image;
        // Extrusion = blit with the source coordinate clamped, over the sprite rect grown by `ext`.
        for (std::int32_t dy = -ext; dy < im.height + ext; ++dy) {
            for (std::int32_t dx = -ext; dx < im.width + ext; ++dx) {
                const std::int32_t sx = std::clamp(dx, 0, im.width - 1);
                const std::int32_t sy = std::clamp(dy, 0, im.height - 1);
                std::memcpy(px(p.rect.x + dx, p.rect.y + dy),
                            &im.rgba[(std::size_t(sy) * std::size_t(im.width) + std::size_t(sx)) * 4], 4);
            }
        }
    }
    return page;
}

} // namespace

const AtlasFrame* Atlas::find(const std::string& name) const {
    const auto it = std::lower_bound(frames.begin(), frames.end(), name,
                                     [](const AtlasFrame& f, const std::string& n) { return f.name < n; });
    return it != frames.end() && it->name == name ? &*it : nullptr;
}

AtlasResult pack_atlas(const std::vector<AtlasInput>& sprites, const AtlasOptions& options) {
    const std::int32_t ext = options.extrude ? 1 : 0;
    const std::int32_t pad = std::max(0, options.padding);
    const std::int32_t max_size = options.power_of_two ? next_pot(std::max(1, options.max_size)) : std::max(1, options.max_size);

    for (std::size_t i = 0; i < sprites.size(); ++i) {
        const Image& im = sprites[i].image;
        if (im.width <= 0 || im.height <= 0 || im.rgba.size() != std::size_t(im.width) * std::size_t(im.height) * 4) {
            return {std::nullopt, "sprite '" + sprites[i].name + "' has an invalid image"};
        }
        if (im.width + 2 * ext > max_size || im.height + 2 * ext > max_size) {
            return {std::nullopt, "sprite '" + sprites[i].name + "' does not fit a " + std::to_string(max_size) + " page"};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (sprites[j].name == sprites[i].name) {
                return {std::nullopt, "duplicate sprite name '" + sprites[i].name + "'"};
            }
        }
    }

    std::vector<std::size_t> order(sprites.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Image& ia = sprites[a].image;
        const Image& ib = sprites[b].image;
        if (ia.height != ib.height) {
            return ia.height > ib.height;
        }
        if (ia.width != ib.width) {
            return ia.width > ib.width;
        }
        return sprites[a].name < sprites[b].name; // names are unique
    });

    Atlas atlas;
    std::vector<std::size_t> remaining = order;
    std::vector<std::size_t> leftover;
    while (!remaining.empty()) {
        // Candidate page sizes by ascending area (then narrower first); the
        // first that holds everything wins, else a full max_size page is filled.
        std::vector<std::pair<std::int32_t, std::int32_t>> sizes;
        if (options.power_of_two) {
            for (std::int32_t w = 1; w <= max_size; w <<= 1) {
                for (std::int32_t h = 1; h <= max_size; h <<= 1) {
                    sizes.emplace_back(w, h);
                }
            }
        } else {
            sizes.emplace_back(max_size, max_size);
        }
        std::stable_sort(sizes.begin(), sizes.end(), [](const auto& a, const auto& b) {
            return std::int64_t(a.first) * a.second < std::int64_t(b.first) * b.second;
        });
        std::int64_t need = 0;
        for (const std::size_t i : remaining) {
            need += std::int64_t(sprites[i].image.width + 2 * ext) * (sprites[i].image.height + 2 * ext);
        }
        std::vector<Placement> placed;
        std::int32_t pw = max_size;
        std::int32_t ph = max_size;
        bool done = false;
        for (const auto& [w, h] : sizes) {
            if (std::int64_t(w) * h < need) {
                continue;
            }
            auto attempt = try_pack(sprites, remaining, w, h, ext, pad, leftover);
            if (leftover.empty()) {
                placed = std::move(attempt);
                pw = w;
                ph = h;
                done = true;
                break;
            }
        }
        if (!done) {
            placed = try_pack(sprites, remaining, max_size, max_size, ext, pad, leftover);
            if (placed.empty()) {
                return {std::nullopt, "atlas packing made no progress"};
            }
        }
        const auto page_index = static_cast<std::int32_t>(atlas.pages.size());
        atlas.pages.push_back(render_page(sprites, placed, pw, ph, ext));
        for (const Placement& p : placed) {
            atlas.frames.push_back({sprites[p.sprite].name, page_index, p.rect.x, p.rect.y, p.rect.w, p.rect.h});
        }
        remaining = leftover;
    }
    std::sort(atlas.frames.begin(), atlas.frames.end(),
              [](const AtlasFrame& a, const AtlasFrame& b) { return a.name < b.name; });
    return {std::move(atlas), {}};
}

} // namespace cuervo::assets

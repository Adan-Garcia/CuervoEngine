// golden_diff: compares a screenshot (PPM from --screenshot) with a stored golden (QOI crop) within a
// tolerance, or writes the golden. Used by tests/editor_golden.cmake; see tests/golden/README.md.
//
//   golden_diff compare <actual.ppm> <golden.qoi> <x,y,w,h> [--tol 24] [--max-frac 0.01] [--diff out.ppm]
//   golden_diff update  <actual.ppm> <golden.qoi> <x,y,w,h>
//
// Tolerance: a pixel matches when every channel is within --tol of the golden (absorbs the anti-aliasing
// differences between GL drivers); the test fails when more than --max-frac of the crop does not match,
// so a moved row, a missing icon or a wrong colour token fails and sub-pixel text rendering does not.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "cuervo/assets/image.hpp"

namespace {

struct Rgb {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px; // RGB
};

bool read_ppm(const std::string& path, Rgb& out) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int maxv = 0;
    if (!(in >> magic >> out.w >> out.h >> maxv) || magic != "P6" || maxv != 255) return false;
    in.get();
    out.px.resize(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h) * 3);
    in.read(reinterpret_cast<char*>(out.px.data()), static_cast<std::streamsize>(out.px.size()));
    return static_cast<std::size_t>(in.gcount()) == out.px.size();
}

Rgb crop(const Rgb& src, int x, int y, int w, int h) {
    Rgb c;
    c.w = w;
    c.h = h;
    c.px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3, 0);
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            if (x + i >= src.w || y + j >= src.h) continue;
            std::memcpy(&c.px[(static_cast<std::size_t>(j) * w + i) * 3], &src.px[(static_cast<std::size_t>(y + j) * src.w + x + i) * 3], 3);
        }
    }
    return c;
}

// QOI encoder (https://qoiformat.org/qoi-specification.pdf), RGB only, so goldens stay small and the
// engine's own decode_image reads them back.
std::vector<std::uint8_t> encode_qoi(const Rgb& img) {
    std::vector<std::uint8_t> out;
    const auto put32 = [&](std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s)); };
    out.insert(out.end(), {'q', 'o', 'i', 'f'});
    put32(static_cast<std::uint32_t>(img.w));
    put32(static_cast<std::uint32_t>(img.h));
    out.push_back(3);
    out.push_back(0);
    std::uint8_t index[64][4] = {};
    std::uint8_t pr = 0, pg = 0, pb = 0;
    int run = 0;
    const std::size_t n = static_cast<std::size_t>(img.w) * static_cast<std::size_t>(img.h);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t r = img.px[i * 3], g = img.px[i * 3 + 1], b = img.px[i * 3 + 2];
        if (r == pr && g == pg && b == pb) {
            if (++run == 62 || i == n - 1) { out.push_back(static_cast<std::uint8_t>(0xC0 | (run - 1))); run = 0; }
            continue;
        }
        if (run > 0) { out.push_back(static_cast<std::uint8_t>(0xC0 | (run - 1))); run = 0; }
        const int hash = (r * 3 + g * 5 + b * 7 + 255 * 11) % 64;
        if (index[hash][0] == r && index[hash][1] == g && index[hash][2] == b && index[hash][3] == 255) {
            out.push_back(static_cast<std::uint8_t>(hash));
        } else {
            index[hash][0] = r; index[hash][1] = g; index[hash][2] = b; index[hash][3] = 255;
            const int dr = static_cast<std::int8_t>(r - pr), dg = static_cast<std::int8_t>(g - pg), db = static_cast<std::int8_t>(b - pb);
            const int drg = dr - dg, dbg = db - dg;
            if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                out.push_back(static_cast<std::uint8_t>(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
            } else if (dg >= -32 && dg <= 31 && drg >= -8 && drg <= 7 && dbg >= -8 && dbg <= 7) {
                out.push_back(static_cast<std::uint8_t>(0x80 | (dg + 32)));
                out.push_back(static_cast<std::uint8_t>(((drg + 8) << 4) | (dbg + 8)));
            } else {
                out.insert(out.end(), {0xFE, r, g, b});
            }
        }
        pr = r; pg = g; pb = b;
    }
    out.insert(out.end(), {0, 0, 0, 0, 0, 0, 0, 1});
    return out;
}

bool parse_rect(const char* s, int r[4]) { return std::sscanf(s, "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) == 4 && r[2] > 0 && r[3] > 0; }

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: golden_diff compare|update <actual.ppm> <golden.qoi> <x,y,w,h> [--tol N] [--max-frac F] [--diff out.ppm]\n");
        return 2;
    }
    const std::string mode = argv[1];
    int rect[4];
    Rgb actual;
    if (!read_ppm(argv[2], actual) || !parse_rect(argv[4], rect)) {
        std::fprintf(stderr, "golden_diff: cannot read '%s' as a P6 PPM or bad crop '%s'\n", argv[2], argv[4]);
        return 2;
    }
    const Rgb now = crop(actual, rect[0], rect[1], rect[2], rect[3]);
    if (mode == "update") {
        const std::vector<std::uint8_t> q = encode_qoi(now);
        std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char*>(q.data()), static_cast<std::streamsize>(q.size()));
        std::printf("golden_diff: wrote %s (%zu bytes, %dx%d)\n", argv[3], q.size(), now.w, now.h);
        return 0;
    }
    int tol = 24;
    double max_frac = 0.01;
    std::string diff_path;
    for (int i = 5; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "--tol") tol = std::atoi(argv[i + 1]);
        else if (std::string(argv[i]) == "--max-frac") max_frac = std::atof(argv[i + 1]);
        else if (std::string(argv[i]) == "--diff") diff_path = argv[i + 1];
    }
    std::ifstream in(argv[3], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "golden_diff: no golden at %s (regenerate: see tests/golden/README.md)\n", argv[3]);
        return 1;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto dec = cuervo::assets::decode_image(std::as_bytes(std::span(bytes)));
    if (!dec.image || dec.image->width != now.w || dec.image->height != now.h) {
        std::fprintf(stderr, "golden_diff: golden %s is unreadable or %dx%d, expected %dx%d\n", argv[3], dec.image ? dec.image->width : 0, dec.image ? dec.image->height : 0, now.w, now.h);
        return 1;
    }
    std::size_t bad = 0;
    Rgb diff = now;
    for (std::size_t i = 0; i < static_cast<std::size_t>(now.w) * static_cast<std::size_t>(now.h); ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(static_cast<int>(now.px[i * 3 + c]) - static_cast<int>(dec.image->rgba[i * 4 + c])));
        if (worst > tol) {
            ++bad;
            diff.px[i * 3] = 255; diff.px[i * 3 + 1] = 0; diff.px[i * 3 + 2] = 255;
        }
    }
    const double frac = static_cast<double>(bad) / (static_cast<double>(now.w) * now.h);
    std::printf("golden_diff: %s: %zu of %d pixels differ by more than %d (%.3f%%, limit %.3f%%)\n", argv[3], bad, now.w * now.h, tol, frac * 100.0, max_frac * 100.0);
    if (frac > max_frac) {
        if (!diff_path.empty()) {
            std::ofstream o(diff_path, std::ios::binary);
            o << "P6\n" << diff.w << ' ' << diff.h << "\n255\n";
            o.write(reinterpret_cast<const char*>(diff.px.data()), static_cast<std::streamsize>(diff.px.size()));
        }
        return 1;
    }
    return 0;
}

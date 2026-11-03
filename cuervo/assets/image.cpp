#include "cuervo/assets/image.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

#include "stb_image.h"
#if defined(CUERVO_HAS_WEBP)
#include "webp/decode.h"
#endif

namespace cuervo::assets {

namespace {

// sRGB byte -> linear light scaled to 0..65535, rounded. A literal table so
// no libm pow runs at import time (determinism rule).
constexpr std::array<std::uint16_t, 256> kSrgbToLinear = {
    0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219, 241, 264, 288, 313, 340, 367, 396, 427, 458,
    491, 526, 562, 599, 637, 677, 718, 761, 805, 851, 898, 947, 997, 1048, 1101, 1156, 1212, 1270,
    1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863, 1937, 2013, 2090, 2170, 2250, 2333, 2418,
    2504, 2592, 2681, 2773, 2866, 2961, 3058, 3157, 3258, 3360, 3464, 3570, 3678, 3788, 3900, 4014,
    4129, 4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124, 5257, 5392, 5530, 5669, 5810, 5953, 6099,
    6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500, 7666, 7834, 8004, 8177, 8352, 8528, 8708,
    8889, 9072, 9258, 9445, 9635, 9828, 10022, 10219, 10417, 10619, 10822, 11028, 11235, 11446, 11658,
    11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909, 14146, 14387, 14629, 14874,
    15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001, 18277, 18556,
    18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
    23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325, 25662, 26001, 26344, 26688, 27036, 27386,
    27739, 28094, 28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567,
    32957, 33350, 33745, 34143, 34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278,
    38706, 39138, 39572, 40009, 40449, 40891, 41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534,
    45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359, 48850, 49344, 49841, 50341, 50844, 51349,
    51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567, 57105, 57646, 58190, 58737,
    59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535};

// Linear 0..65535 -> nearest sRGB byte: binary search of the table above.
std::uint8_t linear_to_srgb(std::uint32_t linear) {
    const auto* first = kSrgbToLinear.data();
    const auto* it = std::lower_bound(first, first + 256, linear);
    const auto idx = static_cast<std::size_t>(it - first);
    if (idx == 0) {
        return 0;
    }
    if (idx >= 256) {
        return 255;
    }
    const std::uint32_t lo = kSrgbToLinear[idx - 1];
    const std::uint32_t hi = kSrgbToLinear[idx];
    return static_cast<std::uint8_t>(linear - lo <= hi - linear ? idx - 1 : idx);
}

std::uint32_t rd32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

// QOI (qoiformat.org spec): 14-byte header, ops, 8-byte end marker.
DecodeResult decode_qoi(std::span<const std::byte> bytes, std::int32_t max_dimension) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::size_t size = bytes.size();
    if (size < 14 + 8) {
        return {std::nullopt, "QOI: truncated header"};
    }
    const std::uint32_t w = rd32(p + 4);
    const std::uint32_t h = rd32(p + 8);
    if (w == 0 || h == 0 || w > static_cast<std::uint32_t>(max_dimension) ||
        h > static_cast<std::uint32_t>(max_dimension)) {
        return {std::nullopt, "QOI: invalid or oversized dimensions"};
    }
    if (p[12] != 3 && p[12] != 4) {
        return {std::nullopt, "QOI: invalid channel count"};
    }
    Image img;
    img.width = static_cast<std::int32_t>(w);
    img.height = static_cast<std::int32_t>(h);
    img.rgba.resize(std::size_t{w} * h * 4);

    std::array<std::array<std::uint8_t, 4>, 64> index{};
    std::array<std::uint8_t, 4> px = {0, 0, 0, 255};
    std::size_t pos = 14;
    const std::size_t end = size - 8;
    int run = 0;
    for (std::size_t o = 0; o < img.rgba.size(); o += 4) {
        if (run > 0) {
            --run;
        } else {
            if (pos >= end) {
                return {std::nullopt, "QOI: truncated pixel data"};
            }
            const std::uint8_t b = p[pos++];
            if (b == 0xFE) { // RGB
                if (pos + 3 > end) {
                    return {std::nullopt, "QOI: truncated pixel data"};
                }
                px[0] = p[pos++];
                px[1] = p[pos++];
                px[2] = p[pos++];
            } else if (b == 0xFF) { // RGBA
                if (pos + 4 > end) {
                    return {std::nullopt, "QOI: truncated pixel data"};
                }
                px[0] = p[pos++];
                px[1] = p[pos++];
                px[2] = p[pos++];
                px[3] = p[pos++];
            } else if ((b & 0xC0) == 0x00) { // INDEX
                px = index[b];
            } else if ((b & 0xC0) == 0x40) { // DIFF
                px[0] = static_cast<std::uint8_t>(px[0] + ((b >> 4) & 3) - 2);
                px[1] = static_cast<std::uint8_t>(px[1] + ((b >> 2) & 3) - 2);
                px[2] = static_cast<std::uint8_t>(px[2] + (b & 3) - 2);
            } else if ((b & 0xC0) == 0x80) { // LUMA
                if (pos >= end) {
                    return {std::nullopt, "QOI: truncated pixel data"};
                }
                const std::uint8_t b2 = p[pos++];
                const int dg = (b & 0x3F) - 32;
                px[0] = static_cast<std::uint8_t>(px[0] + dg - 8 + ((b2 >> 4) & 0x0F));
                px[1] = static_cast<std::uint8_t>(px[1] + dg);
                px[2] = static_cast<std::uint8_t>(px[2] + dg - 8 + (b2 & 0x0F));
            } else { // RUN
                run = b & 0x3F;
            }
            index[static_cast<std::size_t>((px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) & 63)] = px;
        }
        std::memcpy(&img.rgba[o], px.data(), 4);
    }
    return {std::move(img), {}};
}

DecodeResult decode_stb(std::span<const std::byte> bytes, std::int32_t max_dimension) {
    if (bytes.size() > 0x7fffffff) {
        return {std::nullopt, "image too large"};
    }
    int w = 0;
    int h = 0;
    int comp = 0;
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int len = static_cast<int>(bytes.size());
    // Read the header first so a huge declared size is rejected before allocating.
    if (stbi_info_from_memory(data, len, &w, &h, &comp) == 0) {
        return {std::nullopt, std::string("decode failed: ") + stbi_failure_reason()};
    }
    if (w <= 0 || h <= 0 || w > max_dimension || h > max_dimension) {
        return {std::nullopt, "invalid or oversized dimensions"};
    }
    stbi_uc* px = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
    if (px == nullptr) {
        return {std::nullopt, std::string("decode failed: ") + stbi_failure_reason()};
    }
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + std::size_t(w) * std::size_t(h) * 4);
    stbi_image_free(px);
    return {std::move(img), {}};
}

#if defined(CUERVO_HAS_WEBP)
// Lossy (VP8), lossless (VP8L) and alpha all decode to straight-alpha RGBA8;
// animated WebP is rejected by WebPGetFeatures-based checks below.
DecodeResult decode_webp(std::span<const std::byte> bytes, std::int32_t max_dimension) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    WebPBitstreamFeatures features;
    // Reads the header only, so a huge declared size is rejected before allocating.
    if (WebPGetFeatures(data, bytes.size(), &features) != VP8_STATUS_OK) {
        return {std::nullopt, "decode failed: malformed WebP"};
    }
    if (features.has_animation) {
        return {std::nullopt, "animated WebP is not supported"};
    }
    if (features.width <= 0 || features.height <= 0 || features.width > max_dimension || features.height > max_dimension) {
        return {std::nullopt, "invalid or oversized dimensions"};
    }
    int w = 0;
    int h = 0;
    std::uint8_t* px = WebPDecodeRGBA(data, bytes.size(), &w, &h);
    if (px == nullptr) {
        return {std::nullopt, "decode failed: malformed WebP"};
    }
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + std::size_t(w) * std::size_t(h) * 4);
    WebPFree(px);
    return {std::move(img), {}};
}
#endif

void put32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

std::uint32_t get32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= std::uint32_t(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    }
    return v;
}

constexpr std::uint32_t kCtexVersion = 1;

} // namespace

ImageFormat sniff_image_format(std::span<const std::byte> bytes) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (bytes.size() >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
        return ImageFormat::Png;
    }
    if (bytes.size() >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
        return ImageFormat::Jpeg;
    }
    if (bytes.size() >= 4 && std::memcmp(p, "qoif", 4) == 0) {
        return ImageFormat::Qoi;
    }
    if (bytes.size() >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) {
        return ImageFormat::WebP;
    }
    return ImageFormat::Unknown;
}

DecodeResult decode_image(std::span<const std::byte> bytes, std::int32_t max_dimension) {
    switch (sniff_image_format(bytes)) {
    case ImageFormat::Png:
    case ImageFormat::Jpeg:
        return decode_stb(bytes, max_dimension);
    case ImageFormat::Qoi:
        return decode_qoi(bytes, max_dimension);
    case ImageFormat::WebP:
#if defined(CUERVO_HAS_WEBP)
        return decode_webp(bytes, max_dimension);
#else
        return {std::nullopt, "WebP import is not available in this build (CUERVO_WEBP=OFF); convert to PNG"};
#endif
    case ImageFormat::Unknown:
        break;
    }
    return {std::nullopt, "unrecognised image format (PNG, JPEG, WebP and QOI are supported)"};
}

std::int32_t TextureData::level_width(std::int32_t lvl) const { return std::max(1, width >> lvl); }
std::int32_t TextureData::level_height(std::int32_t lvl) const { return std::max(1, height >> lvl); }

std::span<const std::uint8_t> TextureData::level(std::int32_t lvl) const {
    const auto i = static_cast<std::size_t>(lvl);
    const std::size_t begin = level_offsets[i];
    const std::size_t end = i + 1 < level_offsets.size() ? level_offsets[i + 1] : data.size();
    return {data.data() + begin, end - begin};
}

TextureData build_texture(const Image& image, bool srgb, std::int32_t max_levels) {
    TextureData tex;
    tex.width = image.width;
    tex.height = image.height;
    tex.srgb = srgb;
    std::int32_t full = 1;
    for (std::int32_t d = std::max(image.width, image.height); d > 1; d >>= 1) {
        ++full;
    }
    const std::int32_t levels = max_levels > 0 ? std::min(max_levels, full) : full;

    tex.level_offsets.push_back(0);
    tex.data = image.rgba;
    for (std::int32_t l = 1; l < levels; ++l) {
        const std::int32_t sw = tex.level_width(l - 1);
        const std::int32_t sh = tex.level_height(l - 1);
        const std::int32_t dw = tex.level_width(l);
        const std::int32_t dh = tex.level_height(l);
        const std::size_t src_off = tex.level_offsets.back();
        const std::size_t dst_off = tex.data.size();
        tex.level_offsets.push_back(dst_off);
        tex.data.resize(dst_off + std::size_t(dw) * std::size_t(dh) * 4);
        for (std::int32_t y = 0; y < dh; ++y) {
            for (std::int32_t x = 0; x < dw; ++x) {
                std::uint64_t sum[3] = {0, 0, 0};
                std::uint32_t asum = 0;
                std::uint32_t raw[4] = {0, 0, 0, 0};
                for (std::int32_t dy = 0; dy < 2; ++dy) {
                    for (std::int32_t dx = 0; dx < 2; ++dx) {
                        const std::int32_t sx = std::min(x * 2 + dx, sw - 1);
                        const std::int32_t sy = std::min(y * 2 + dy, sh - 1);
                        const std::size_t si = src_off + (std::size_t(sy) * std::size_t(sw) + std::size_t(sx)) * 4;
                        const std::uint32_t a = tex.data[si + 3];
                        asum += a;
                        for (std::size_t c = 0; c < 3; ++c) {
                            const std::uint32_t v = tex.data[si + c];
                            raw[c] += v;
                            sum[c] += std::uint64_t(srgb ? kSrgbToLinear[v] : v) * a;
                        }
                        raw[3] += a;
                    }
                }
                const std::size_t di = dst_off + (std::size_t(y) * std::size_t(dw) + std::size_t(x)) * 4;
                for (std::size_t c = 0; c < 3; ++c) {
                    if (asum == 0) {
                        tex.data[di + c] = static_cast<std::uint8_t>((raw[c] + 2) / 4); // transparent: plain average
                    } else if (srgb) {
                        tex.data[di + c] = linear_to_srgb(static_cast<std::uint32_t>((sum[c] + asum / 2) / asum));
                    } else {
                        tex.data[di + c] = static_cast<std::uint8_t>((sum[c] + asum / 2) / asum);
                    }
                }
                tex.data[di + 3] = static_cast<std::uint8_t>((raw[3] + 2) / 4);
            }
        }
    }
    return tex;
}

std::vector<std::byte> encode_ctex(const TextureData& t) {
    std::vector<std::byte> out;
    out.reserve(24 + t.data.size());
    for (char c : {'C', 'T', 'E', 'X'}) {
        out.push_back(static_cast<std::byte>(c));
    }
    put32(out, kCtexVersion);
    put32(out, static_cast<std::uint32_t>(t.width));
    put32(out, static_cast<std::uint32_t>(t.height));
    put32(out, t.srgb ? 1u : 0u);
    put32(out, static_cast<std::uint32_t>(t.mip_count()));
    const auto* bytes = reinterpret_cast<const std::byte*>(t.data.data());
    out.insert(out.end(), bytes, bytes + t.data.size());
    return out;
}

std::optional<TextureData> decode_ctex(std::span<const std::byte> bytes) {
    if (bytes.size() < 24 || std::memcmp(bytes.data(), "CTEX", 4) != 0 || get32(bytes.data() + 4) != kCtexVersion) {
        return std::nullopt;
    }
    TextureData t;
    const std::uint32_t w = get32(bytes.data() + 8);
    const std::uint32_t h = get32(bytes.data() + 12);
    t.srgb = (get32(bytes.data() + 16) & 1u) != 0;
    const std::uint32_t levels = get32(bytes.data() + 20);
    if (w == 0 || h == 0 || w > 65536 || h > 65536 || levels == 0 || levels > 17) {
        return std::nullopt;
    }
    t.width = static_cast<std::int32_t>(w);
    t.height = static_cast<std::int32_t>(h);
    std::size_t total = 0;
    for (std::uint32_t l = 0; l < levels; ++l) {
        t.level_offsets.push_back(total);
        total += std::size_t(t.level_width(static_cast<std::int32_t>(l))) *
                 std::size_t(t.level_height(static_cast<std::int32_t>(l))) * 4;
    }
    if (bytes.size() - 24 != total) {
        return std::nullopt;
    }
    const auto* src = reinterpret_cast<const std::uint8_t*>(bytes.data()) + 24;
    t.data.assign(src, src + total);
    return t;
}

ImportResult import_image(const std::filesystem::path& source, const std::filesystem::path& import_dir,
                          const ImportOptions& options) {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        return {{}, "cannot open " + source.string()};
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    if (!raw.empty()) {
        std::memcpy(bytes.data(), raw.data(), raw.size());
    }
    auto decoded = decode_image(bytes);
    if (!decoded.image) {
        return {{}, source.string() + ": " + decoded.error};
    }
    const auto ctex = encode_ctex(build_texture(*decoded.image, options.srgb, options.max_levels));
    std::error_code ec;
    std::filesystem::create_directories(import_dir, ec);
    const auto out_path = import_dir / (source.filename().string() + ".ctex");
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(ctex.data()), static_cast<std::streamsize>(ctex.size()));
    if (!out) {
        return {{}, "cannot write " + out_path.string()};
    }
    return {out_path, {}};
}

} // namespace cuervo::assets

#include "cuervo/vector/imported_vector.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>

#include "cuervo/core/log.hpp"
#include "cuervo/vector/svg.hpp"

namespace cuervo::vector {

namespace {

static_assert(std::endian::native == std::endian::little, "ImportedVector I/O assumes a little-endian host");

constexpr std::uint32_t flag_base_mesh = 1u << 0;

// Sanity caps so corrupt counts fail cleanly instead of allocating gigabytes.
constexpr std::uint64_t max_count = 1u << 26;

class Writer {
public:
    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void v2(vec2 v) { f32(v.x()); f32(v.y()); }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void color(const Color& c) { f32(c.r); f32(c.g); f32(c.b); f32(c.a); }
    void affine(const Affine2& m) { f32(m.a); f32(m.b); f32(m.c); f32(m.d); f32(m.tx); f32(m.ty); }
    void path(const Path& p) {
        u32(static_cast<std::uint32_t>(p.verbs.size()));
        for (Verb v : p.verbs) u8(static_cast<std::uint8_t>(v));
        u32(static_cast<std::uint32_t>(p.points.size()));
        for (vec2 pt : p.points) v2(pt);
    }
    void paint(const Paint& p) {
        u8(static_cast<std::uint8_t>(p.kind));
        color(p.color);
        u32(p.gradient);
    }
    void raw(const void* data, std::size_t size) {
        if (size == 0) {
            return; // empty vectors may hand us a null data pointer
        }
        const auto* b = static_cast<const std::byte*>(data);
        out_.insert(out_.end(), b, b + size);
    }
    std::vector<std::byte> take() { return std::move(out_); }

private:
    std::vector<std::byte> out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    void fail(std::string message) {
        if (ok_) {
            ok_ = false;
            error_ = std::format("ImportedVector offset {}: {}", pos_, message);
        }
    }

    bool raw(void* out, std::size_t size) {
        if (size == 0) {
            return ok_;
        }
        if (!ok_ || size > data_.size() - pos_) {
            fail("truncated");
            std::memset(out, 0, size);
            return false;
        }
        std::memcpy(out, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }
    std::uint8_t u8() { std::uint8_t v = 0; raw(&v, 1); return v; }
    std::uint32_t u32() { std::uint32_t v = 0; raw(&v, 4); return v; }
    std::int32_t i32() { std::int32_t v = 0; raw(&v, 4); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    vec2 v2() { const float x = f32(); return {x, f32()}; }
    std::uint32_t count(std::size_t element_size) {
        const std::uint32_t n = u32();
        if (n > max_count || static_cast<std::uint64_t>(n) * element_size > data_.size() - pos_) {
            fail("bad element count");
            return 0;
        }
        return n;
    }
    std::string str() {
        std::string s(count(1), '\0');
        raw(s.data(), s.size());
        return s;
    }
    Color color() { Color c; c.r = f32(); c.g = f32(); c.b = f32(); c.a = f32(); return c; }
    Affine2 affine() {
        Affine2 m;
        m.a = f32(); m.b = f32(); m.c = f32(); m.d = f32(); m.tx = f32(); m.ty = f32();
        return m;
    }
    template<typename E>
    E enumeration(std::uint8_t max) {
        const std::uint8_t v = u8();
        if (v > max) fail("bad enum value");
        return static_cast<E>(v);
    }
    Path path() {
        Path p;
        const std::uint32_t verbs = count(1);
        std::uint32_t expected_points = 0;
        p.verbs.reserve(verbs);
        for (std::uint32_t i = 0; i < verbs && ok_; ++i) {
            const Verb v = enumeration<Verb>(static_cast<std::uint8_t>(Verb::Close));
            if (i == 0 && v != Verb::MoveTo) fail("path must start with MoveTo");
            expected_points += verb_point_count(v);
            p.verbs.push_back(v);
        }
        const std::uint32_t points = count(8);
        if (points != expected_points) fail("path point count doesn't match its verbs");
        p.points.reserve(points);
        for (std::uint32_t i = 0; i < points && ok_; ++i) p.points.push_back(v2());
        return p;
    }
    Paint paint(std::size_t gradient_count) {
        Paint p;
        p.kind = enumeration<PaintKind>(static_cast<std::uint8_t>(PaintKind::Gradient));
        p.color = color();
        p.gradient = u32();
        if (p.kind == PaintKind::Gradient && p.gradient >= gradient_count) fail("gradient index out of range");
        return p;
    }
    bool at_end() const { return pos_ == data_.size(); }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
    std::string error_;
};

void write_mesh(Writer& w, const VectorMesh& m) {
    w.u32(static_cast<std::uint32_t>(m.vertices.size()));
    for (const MeshVertex& v : m.vertices) {
        w.v2(v.position);
        w.u32(v.color);
        w.u32(v.paint);
    }
    w.u32(static_cast<std::uint32_t>(m.indices.size()));
    w.raw(m.indices.data(), m.indices.size() * sizeof(std::uint32_t));
    w.u32(static_cast<std::uint32_t>(m.batches.size()));
    for (const MeshBatch& b : m.batches) {
        w.u32(b.first_index);
        w.u32(b.index_count);
        w.u32(static_cast<std::uint32_t>(b.paints.size()));
        w.raw(b.paints.data(), b.paints.size() * sizeof(float) * 4);
    }
}

void read_mesh(Reader& r, VectorMesh& m) {
    const std::uint32_t vertex_count = r.count(16);
    m.vertices.resize(vertex_count);
    for (MeshVertex& v : m.vertices) {
        v.position = r.v2();
        v.color = r.u32();
        v.paint = r.u32();
        m.bounds.add(v.position);
    }
    const std::uint32_t index_count = r.count(4);
    m.indices.resize(index_count);
    r.raw(m.indices.data(), m.indices.size() * sizeof(std::uint32_t));
    for (std::uint32_t i : m.indices) {
        if (i >= vertex_count) {
            r.fail("mesh index out of range");
            break;
        }
    }
    const std::uint32_t batch_count = r.count(12);
    m.batches.resize(batch_count);
    for (MeshBatch& b : m.batches) {
        b.first_index = r.u32();
        b.index_count = r.u32();
        const std::uint32_t paints = r.count(16);
        if (paints > paint_window_vec4 || static_cast<std::uint64_t>(b.first_index) + b.index_count > index_count) {
            r.fail("bad mesh batch");
            return;
        }
        b.paints.resize(paints);
        r.raw(b.paints.data(), b.paints.size() * sizeof(float) * 4);
    }
}

} // namespace

bool is_imported_vector(std::span<const std::byte> data) {
    return data.size() >= 4 && std::memcmp(data.data(), imported_vector_magic, 4) == 0;
}

std::vector<std::byte> serialize_imported_vector(const ImportedVector& asset) {
    Writer w;
    w.raw(imported_vector_magic, 4);
    w.u32(imported_vector_version);
    w.u32(asset.base_mesh ? flag_base_mesh : 0u);
    w.u32(0); // reserved

    const VectorDocument& doc = asset.document;
    w.v2(doc.size);
    w.u32(static_cast<std::uint32_t>(doc.gradients.size()));
    for (const Gradient& g : doc.gradients) {
        w.u8(static_cast<std::uint8_t>(g.kind));
        w.u8(static_cast<std::uint8_t>(g.spread));
        w.u8(g.bbox_units ? 1 : 0);
        w.affine(g.transform);
        w.v2(g.p0);
        w.v2(g.p1);
        w.f32(g.radius);
        w.u32(static_cast<std::uint32_t>(g.stops.size()));
        for (const GradientStop& s : g.stops) {
            w.f32(s.offset);
            w.color(s.color);
        }
    }
    w.u32(static_cast<std::uint32_t>(doc.clip_paths.size()));
    for (const ClipPath& c : doc.clip_paths) {
        w.u32(static_cast<std::uint32_t>(c.shapes.size()));
        for (const ClipShape& s : c.shapes) {
            w.path(s.path);
            w.affine(s.transform);
            w.u8(static_cast<std::uint8_t>(s.rule));
        }
    }
    w.u32(static_cast<std::uint32_t>(doc.shapes.size()));
    for (const Shape& s : doc.shapes) {
        w.str(s.id);
        w.path(s.path);
        w.affine(s.transform);
        w.paint(s.fill);
        w.u8(static_cast<std::uint8_t>(s.fill_rule));
        w.f32(s.fill_opacity);
        w.paint(s.stroke);
        w.f32(s.stroke_style.width);
        w.u8(static_cast<std::uint8_t>(s.stroke_style.join));
        w.u8(static_cast<std::uint8_t>(s.stroke_style.cap));
        w.f32(s.stroke_style.miter_limit);
        w.u32(static_cast<std::uint32_t>(s.stroke_style.dashes.size()));
        for (float d : s.stroke_style.dashes) w.f32(d);
        w.f32(s.stroke_style.dash_offset);
        w.f32(s.stroke_opacity);
        w.u8(s.stroke_first ? 1 : 0);
        w.f32(s.opacity);
        w.u32(static_cast<std::uint32_t>(s.clips.size()));
        for (std::uint32_t c : s.clips) w.u32(c);
    }
    if (asset.base_mesh) {
        w.i32(asset.base_mesh->level);
        write_mesh(w, asset.base_mesh->mesh);
    }
    return w.take();
}

bool deserialize_imported_vector(std::span<const std::byte> data, ImportedVector& out, std::string* error) {
    out = {};
    Reader r(data);
    auto failed = [&](std::string message) {
        if (error) *error = std::move(message);
        out = {};
        return false;
    };
    if (!is_imported_vector(data)) return failed("not an ImportedVector file (bad magic)");
    char magic[4];
    r.raw(magic, 4);
    const std::uint32_t version = r.u32();
    if (version == 0 || version > imported_vector_version) {
        return failed(std::format("ImportedVector version {} is newer than this engine supports ({}); re-import",
                                  version, imported_vector_version));
    }
    // Version 1 is the only version so far; older versions get migrated here.
    const std::uint32_t flags = r.u32();
    r.u32(); // reserved

    VectorDocument& doc = out.document;
    doc.size = r.v2();
    const std::uint32_t gradients = r.count(40);
    doc.gradients.resize(gradients);
    for (Gradient& g : doc.gradients) {
        g.kind = r.enumeration<GradientKind>(static_cast<std::uint8_t>(GradientKind::Radial));
        g.spread = r.enumeration<Spread>(static_cast<std::uint8_t>(Spread::Repeat));
        g.bbox_units = r.u8() != 0;
        g.transform = r.affine();
        g.p0 = r.v2();
        g.p1 = r.v2();
        g.radius = r.f32();
        g.stops.resize(r.count(20));
        for (GradientStop& s : g.stops) {
            s.offset = r.f32();
            s.color = r.color();
        }
    }
    doc.clip_paths.resize(r.count(4));
    for (ClipPath& c : doc.clip_paths) {
        c.shapes.resize(r.count(32));
        for (ClipShape& s : c.shapes) {
            s.path = r.path();
            s.transform = r.affine();
            s.rule = r.enumeration<FillRule>(static_cast<std::uint8_t>(FillRule::EvenOdd));
        }
        if (!r.ok()) break;
    }
    doc.shapes.resize(r.count(64));
    for (Shape& s : doc.shapes) {
        s.id = r.str();
        s.path = r.path();
        s.transform = r.affine();
        s.fill = r.paint(gradients);
        s.fill_rule = r.enumeration<FillRule>(static_cast<std::uint8_t>(FillRule::EvenOdd));
        s.fill_opacity = r.f32();
        s.stroke = r.paint(gradients);
        s.stroke_style.width = r.f32();
        s.stroke_style.join = r.enumeration<LineJoin>(static_cast<std::uint8_t>(LineJoin::Bevel));
        s.stroke_style.cap = r.enumeration<LineCap>(static_cast<std::uint8_t>(LineCap::Square));
        s.stroke_style.miter_limit = r.f32();
        s.stroke_style.dashes.resize(r.count(4));
        for (float& d : s.stroke_style.dashes) d = r.f32();
        s.stroke_style.dash_offset = r.f32();
        s.stroke_opacity = r.f32();
        s.stroke_first = r.u8() != 0;
        s.opacity = r.f32();
        s.clips.resize(r.count(4));
        for (std::uint32_t& c : s.clips) {
            c = r.u32();
            if (c >= doc.clip_paths.size()) r.fail("clip index out of range");
        }
        if (!r.ok()) break;
    }
    if (flags & flag_base_mesh) {
        LodMesh lod;
        lod.level = r.i32();
        read_mesh(r, lod.mesh);
        out.base_mesh = std::move(lod);
    }
    if (r.ok() && !r.at_end()) r.fail("trailing bytes");
    if (!r.ok()) return failed(r.error());
    return true;
}

std::optional<ImportedVector> import_svg(std::string_view svg_text, std::string_view name, int base_level) {
    SvgResult svg = parse_svg(svg_text, name);
    if (!svg.ok) {
        return std::nullopt;
    }
    ImportedVector asset;
    asset.document = std::move(svg.document);
    LodMesh lod;
    lod.level = base_level;
    DocumentTessellator tessellator;
    tessellator.build(asset.document, base_level, lod.mesh);
    asset.base_mesh = std::move(lod);
    return asset;
}

bool read_file(const std::string& path, std::vector<std::byte>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_ERROR("Can't open '{}'", path);
        return false;
    }
    const std::streamsize size = file.tellg();
    file.seekg(0);
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(size, 0)));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        LOG_ERROR("Can't read '{}'", path);
        return false;
    }
    return true;
}

bool write_file(const std::string& path, std::span<const std::byte> data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        LOG_ERROR("Can't write '{}'", path);
        return false;
    }
    return true;
}

std::optional<ImportedVector> load_vector_file(const std::string& path) {
    std::vector<std::byte> data;
    if (!read_file(path, data)) {
        return std::nullopt;
    }
    if (is_imported_vector(data)) {
        ImportedVector asset;
        std::string error;
        if (!deserialize_imported_vector(data, asset, &error)) {
            LOG_ERROR("'{}': {}", path, error);
            return std::nullopt;
        }
        return asset;
    }
#ifdef CUERVO_NO_SVG_IMPORT
    LOG_ERROR("'{}' is not an ImportedVector file (SVG import is compiled out of this build)", path);
    return std::nullopt;
#else
    return import_svg(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), path);
#endif
}

} // namespace cuervo::vector

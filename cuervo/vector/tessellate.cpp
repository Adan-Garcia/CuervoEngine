#include "cuervo/vector/tessellate.hpp"

#include <tesselator.h>

#include <algorithm>
#include <cmath>

#include "cuervo/core/log.hpp"

namespace cuervo::vector {

namespace {

static_assert(sizeof(vec2) == 2 * sizeof(TESSreal), "vec2 must be two packed floats for libtess2");

class Tess {
public:
    Tess() : tess_(tessNewTess(nullptr)) {}
    ~Tess() {
        if (tess_) {
            tessDeleteTess(tess_);
        }
    }
    Tess(const Tess&) = delete;
    Tess& operator=(const Tess&) = delete;

    explicit operator bool() const { return tess_ != nullptr; }
    bool empty() const { return !added_; }

    void add(std::span<const Polyline> contours) {
        for (const Polyline& c : contours) {
            if (c.points.size() < 3) {
                continue; // no area
            }
            tessAddContour(tess_, 2, c.points.data(), sizeof(vec2), static_cast<int>(c.points.size()));
            added_ = true;
        }
    }

    // A fixed normal keeps orientation meaningful: libtess2 would otherwise
    // pick one from the input, flipping the sign of every winding number for
    // mostly-clockwise input, and normalized regions would stop combining.
    bool run(int winding, int element_type, int poly_size) {
        if (!added_) {
            return false;
        }
        static const TESSreal normal[3] = {0.0f, 0.0f, 1.0f};
        return tessTesselate(tess_, winding, element_type, poly_size, 2, normal) != 0;
    }

    void triangles(TriMesh& out) const {
        const int vertex_count = tessGetVertexCount(tess_);
        const TESSreal* verts = tessGetVertices(tess_);
        const int element_count = tessGetElementCount(tess_);
        const TESSindex* elements = tessGetElements(tess_);
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        for (int i = 0; i < vertex_count; ++i) {
            out.vertices.emplace_back(verts[i * 2], verts[i * 2 + 1]);
        }
        for (int e = 0; e < element_count; ++e) {
            const TESSindex* tri = elements + e * 3;
            if (tri[0] == TESS_UNDEF || tri[1] == TESS_UNDEF || tri[2] == TESS_UNDEF) {
                continue;
            }
            out.indices.push_back(base + static_cast<std::uint32_t>(tri[0]));
            out.indices.push_back(base + static_cast<std::uint32_t>(tri[1]));
            out.indices.push_back(base + static_cast<std::uint32_t>(tri[2]));
        }
    }

    std::vector<Polyline> contours() const {
        std::vector<Polyline> out;
        const TESSreal* verts = tessGetVertices(tess_);
        const int element_count = tessGetElementCount(tess_);
        const TESSindex* elements = tessGetElements(tess_);
        out.reserve(static_cast<std::size_t>(element_count));
        for (int e = 0; e < element_count; ++e) {
            const TESSindex first = elements[e * 2];
            const TESSindex count = elements[e * 2 + 1];
            Polyline line;
            line.closed = true;
            line.points.reserve(static_cast<std::size_t>(count));
            for (TESSindex i = 0; i < count; ++i) {
                line.points.emplace_back(verts[(first + i) * 2], verts[(first + i) * 2 + 1]);
            }
            out.push_back(std::move(line));
        }
        return out;
    }

private:
    TESStesselator* tess_;
    bool added_ = false;
};

int to_tess_winding(FillRule rule) { return rule == FillRule::EvenOdd ? TESS_WINDING_ODD : TESS_WINDING_NONZERO; }

bool finite(std::span<const Polyline> contours) {
    for (const Polyline& c : contours) {
        for (vec2 p : c.points) {
            if (!std::isfinite(p.x()) || !std::isfinite(p.y())) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

double TriMesh::area() const {
    double sum = 0.0;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const vec2 a = vertices[indices[i]], b = vertices[indices[i + 1]], c = vertices[indices[i + 2]];
        const double cross = (static_cast<double>(b.x()) - a.x()) * (static_cast<double>(c.y()) - a.y()) -
                             (static_cast<double>(b.y()) - a.y()) * (static_cast<double>(c.x()) - a.x());
        sum += std::fabs(cross) * 0.5;
    }
    return sum;
}

bool tessellate_fill(std::span<const Polyline> contours, FillRule rule, TriMesh& out) {
    if (!finite(contours)) {
        LOG_WARN("tessellate_fill: non-finite coordinates, shape skipped");
        return false;
    }
    Tess tess;
    if (!tess) {
        return false;
    }
    tess.add(contours);
    if (tess.empty()) {
        return true; // nothing with area
    }
    if (!tess.run(to_tess_winding(rule), TESS_POLYGONS, 3)) {
        return false;
    }
    tess.triangles(out);
    return true;
}

std::vector<Polyline> normalize_region(std::span<const Polyline> contours, FillRule rule) {
    if (!finite(contours)) {
        return {};
    }
    Tess tess;
    if (!tess) {
        return {};
    }
    tess.add(contours);
    if (!tess.run(to_tess_winding(rule), TESS_BOUNDARY_CONTOURS, 0)) {
        return {};
    }
    return tess.contours();
}

std::vector<Polyline> union_regions(std::span<const Polyline> a, std::span<const Polyline> b) {
    Tess tess;
    tess.add(a);
    tess.add(b);
    if (!tess.run(TESS_WINDING_NONZERO, TESS_BOUNDARY_CONTOURS, 0)) {
        return {};
    }
    return tess.contours();
}

std::vector<Polyline> intersect_regions(std::span<const Polyline> a, std::span<const Polyline> b) {
    Tess tess;
    tess.add(a);
    tess.add(b);
    if (!tess.run(TESS_WINDING_ABS_GEQ_TWO, TESS_BOUNDARY_CONTOURS, 0)) {
        return {};
    }
    return tess.contours();
}

std::vector<Polyline> subtract_regions(std::span<const Polyline> a, std::span<const Polyline> b) {
    if (b.empty()) {
        return {a.begin(), a.end()};
    }
    // Reversed b has winding -1 inside: a only = +1, both = 0, b only = -1.
    std::vector<Polyline> reversed(b.begin(), b.end());
    for (Polyline& p : reversed) {
        std::reverse(p.points.begin(), p.points.end());
    }
    Tess tess;
    tess.add(a);
    tess.add(reversed);
    if (tess.empty() || !tess.run(TESS_WINDING_POSITIVE, TESS_BOUNDARY_CONTOURS, 0)) {
        return {};
    }
    return tess.contours();
}

bool tessellate_intersection(std::span<const Polyline> a, std::span<const Polyline> b, TriMesh& out) {
    if (a.empty() || b.empty()) {
        return true; // empty intersection
    }
    Tess tess;
    tess.add(a);
    tess.add(b);
    if (tess.empty()) {
        return true;
    }
    if (!tess.run(TESS_WINDING_ABS_GEQ_TWO, TESS_POLYGONS, 3)) {
        return false;
    }
    tess.triangles(out);
    return true;
}

} // namespace cuervo::vector

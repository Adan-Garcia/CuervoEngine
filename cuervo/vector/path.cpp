#include "cuervo/vector/path.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

// Note: cuervo/vector runs at import time and on the render side, never in
// the simulation, so <cmath> is allowed here (DESIGN.md 4.1 restricts libm in
// sim paths only). Arc conversion uses double precision for accuracy.

namespace cuervo::vector {

void Path::ensure_subpath() {
    if (!open_subpath_) {
        // A drawing command after Z (or at the very start) begins a new
        // subpath at the current point (SVG 1.1 8.3.3).
        verbs.push_back(Verb::MoveTo);
        points.push_back(current_);
        subpath_start_ = current_;
        open_subpath_ = true;
    }
}

void Path::move_to(vec2 p) {
    if (!verbs.empty() && verbs.back() == Verb::MoveTo) {
        points.back() = p; // consecutive moves: only the last one matters
    } else {
        verbs.push_back(Verb::MoveTo);
        points.push_back(p);
    }
    current_ = subpath_start_ = p;
    open_subpath_ = true;
}

void Path::line_to(vec2 p) {
    ensure_subpath();
    verbs.push_back(Verb::LineTo);
    points.push_back(p);
    current_ = p;
}

void Path::quad_to(vec2 control, vec2 end) {
    ensure_subpath();
    verbs.push_back(Verb::QuadTo);
    points.push_back(control);
    points.push_back(end);
    current_ = end;
}

void Path::cubic_to(vec2 control1, vec2 control2, vec2 end) {
    ensure_subpath();
    verbs.push_back(Verb::CubicTo);
    points.push_back(control1);
    points.push_back(control2);
    points.push_back(end);
    current_ = end;
}

void Path::close() {
    if (!open_subpath_) {
        return;
    }
    verbs.push_back(Verb::Close);
    current_ = subpath_start_;
    open_subpath_ = false;
}

void Path::arc_to(vec2 radii, float x_axis_rotation, bool large_arc, bool sweep, vec2 end) {
    const vec2 start = current_;
    if (start == end) {
        return; // SVG F.6.2: identical endpoints omit the arc entirely
    }
    double rx = std::fabs(static_cast<double>(radii.x()));
    double ry = std::fabs(static_cast<double>(radii.y()));
    if (rx == 0.0 || ry == 0.0) {
        line_to(end); // F.6.2: zero radius = straight line
        return;
    }
    const double phi = static_cast<double>(x_axis_rotation);
    const double cos_phi = std::cos(phi);
    const double sin_phi = std::sin(phi);
    const double x1 = start.x(), y1 = start.y(), x2 = end.x(), y2 = end.y();

    // F.6.5 step 1: to the ellipse's frame, midpoint at the origin.
    const double dx = (x1 - x2) * 0.5;
    const double dy = (y1 - y2) * 0.5;
    const double x1p = cos_phi * dx + sin_phi * dy;
    const double y1p = -sin_phi * dx + cos_phi * dy;

    // F.6.6: scale radii up if the ellipse can't reach both points.
    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }

    // Step 2: center in the rotated frame.
    const double rx2 = rx * rx, ry2 = ry * ry;
    const double num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    const double den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    double coef = den > 0.0 ? std::sqrt(std::max(0.0, num / den)) : 0.0;
    if (large_arc == sweep) {
        coef = -coef;
    }
    const double cxp = coef * rx * y1p / ry;
    const double cyp = -coef * ry * x1p / rx;

    // Step 3: center in user space.
    const double cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) * 0.5;
    const double cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) * 0.5;

    // Step 4: start angle and sweep.
    auto angle = [](double ux, double uy, double vx, double vy) {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    const double vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    const double theta1 = angle(1.0, 0.0, ux, uy);
    double delta = angle(ux, uy, vx, vy);
    constexpr double two_pi = 6.283185307179586476925;
    if (!sweep && delta > 0.0) {
        delta -= two_pi;
    } else if (sweep && delta < 0.0) {
        delta += two_pi;
    }

    // Split into segments of at most 90 degrees; each is one cubic with the
    // standard 4/3 tan(theta/4) handle length (max radial error ~2.7e-4 r).
    const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(delta) / (two_pi / 4.0) - 1e-9)));
    const double step = delta / segments;
    const double k = 4.0 / 3.0 * std::tan(step / 4.0);
    auto map = [&](double ex, double ey) {
        const double px = rx * ex, py = ry * ey;
        return vec2(static_cast<float>(cos_phi * px - sin_phi * py + cx),
                    static_cast<float>(sin_phi * px + cos_phi * py + cy));
    };
    double a = theta1;
    for (int i = 0; i < segments; ++i) {
        const double b = a + step;
        const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
        const vec2 c1 = map(ca - k * sa, sa + k * ca);
        const vec2 c2 = map(cb + k * sb, sb - k * cb);
        const vec2 p = i + 1 == segments ? end : map(cb, sb);
        cubic_to(c1, c2, p);
        a = b;
    }
}

void Path::arc(vec2 center, float radius, float start_angle, float end_angle, bool counter_clockwise) {
    if (radius <= 0.0f) {
        return;
    }
    double delta = static_cast<double>(end_angle - start_angle);
    constexpr double two_pi = 6.283185307179586476925;
    if (!counter_clockwise) {
        if (delta >= two_pi) {
            delta = two_pi;
        } else {
            delta = std::fmod(delta, two_pi);
            if (delta < 0.0) delta += two_pi;
        }
    } else {
        if (delta <= -two_pi) {
            delta = -two_pi;
        } else {
            delta = std::fmod(delta, two_pi);
            if (delta > 0.0) delta -= two_pi;
        }
    }

    const double s_ang = static_cast<double>(start_angle);
    const vec2 start_pt = center + vec2(static_cast<float>(radius * std::cos(s_ang)),
                                        static_cast<float>(radius * std::sin(s_ang)));
    if (verbs.empty()) {
        move_to(start_pt);
    } else if (current_ != start_pt) {
        line_to(start_pt);
    }

    if (std::fabs(delta) < 1e-9) {
        return;
    }

    const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(delta) / (two_pi / 4.0) - 1e-5)));
    const double step = delta / segments;
    const double k = 4.0 / 3.0 * std::tan(step / 4.0);
    double a = s_ang;
    for (int i = 0; i < segments; ++i) {
        const double b = a + step;
        const double ca = std::cos(a), sa = std::sin(a);
        const double cb = std::cos(b), sb = std::sin(b);
        const vec2 c1 = center + vec2(static_cast<float>(radius * (ca - k * sa)),
                                      static_cast<float>(radius * (sa + k * ca)));
        const vec2 c2 = center + vec2(static_cast<float>(radius * (cb + k * sb)),
                                      static_cast<float>(radius * (sb - k * cb)));
        const vec2 p = center + vec2(static_cast<float>(radius * cb),
                                     static_cast<float>(radius * sb));
        cubic_to(c1, c2, p);
        a = b;
    }
}

void Path::add_rect(vec2 origin, vec2 size, vec2 corner_radii) {
    const float x = origin.x(), y = origin.y(), w = size.x(), h = size.y();
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    // SVG 1.1 9.2: clamp radii to half the side.
    const float rx = std::clamp(corner_radii.x(), 0.0f, w * 0.5f);
    const float ry = std::clamp(corner_radii.y(), 0.0f, h * 0.5f);
    if (rx <= 0.0f || ry <= 0.0f) {
        move_to({x, y});
        line_to({x + w, y});
        line_to({x + w, y + h});
        line_to({x, y + h});
        close();
        return;
    }
    const vec2 r{rx, ry};
    move_to({x + rx, y});
    line_to({x + w - rx, y});
    arc_to(r, 0.0f, false, true, {x + w, y + ry});
    line_to({x + w, y + h - ry});
    arc_to(r, 0.0f, false, true, {x + w - rx, y + h});
    line_to({x + rx, y + h});
    arc_to(r, 0.0f, false, true, {x, y + h - ry});
    line_to({x, y + ry});
    arc_to(r, 0.0f, false, true, {x + rx, y});
    close();
}

void Path::add_ellipse(vec2 center, vec2 radii) {
    const float rx = radii.x(), ry = radii.y();
    if (rx <= 0.0f || ry <= 0.0f) {
        return;
    }
    const float cx = center.x(), cy = center.y();
    move_to({cx + rx, cy});
    arc_to(radii, 0.0f, false, true, {cx, cy + ry});
    arc_to(radii, 0.0f, false, true, {cx - rx, cy});
    arc_to(radii, 0.0f, false, true, {cx, cy - ry});
    arc_to(radii, 0.0f, false, true, {cx + rx, cy});
    close();
}

void Path::add_sector(vec2 center, float radius, float start_angle, float end_angle) {
    if (radius <= 0.0f) {
        return;
    }
    move_to(center);
    arc(center, radius, start_angle, end_angle);
    close();
}

void Path::transform(const Affine2& m) {
    for (vec2& p : points) {
        p = m.apply(p);
    }
    current_ = m.apply(current_);
    subpath_start_ = m.apply(subpath_start_);
}

namespace {

// Roots in (0, 1) of the derivative of a 1D cubic Bézier.
int cubic_extrema(float p0, float p1, float p2, float p3, float out[2]) {
    // B'(t)/3 = a t^2 + b t + c
    const double a = -p0 + 3.0 * p1 - 3.0 * p2 + p3;
    const double b = 2.0 * (p0 - 2.0 * p1 + p2);
    const double c = p1 - p0;
    int n = 0;
    auto push = [&](double t) {
        if (t > 0.0 && t < 1.0) {
            out[n++] = static_cast<float>(t);
        }
    };
    if (std::fabs(a) < 1e-12) {
        if (std::fabs(b) > 1e-12) {
            push(-c / b);
        }
        return n;
    }
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) {
        return 0;
    }
    const double s = std::sqrt(disc);
    push((-b + s) / (2.0 * a));
    push((-b - s) / (2.0 * a));
    return n;
}

vec2 eval_cubic(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t) {
    const float u = 1.0f - t;
    return p0 * (u * u * u) + p1 * (3.0f * u * u * t) + p2 * (3.0f * u * t * t) + p3 * (t * t * t);
}

vec2 eval_quad(vec2 p0, vec2 p1, vec2 p2, float t) {
    const float u = 1.0f - t;
    return p0 * (u * u) + p1 * (2.0f * u * t) + p2 * (t * t);
}

} // namespace

Bounds Path::bounds() const {
    Bounds b;
    std::size_t pi = 0;
    vec2 last{0.0f, 0.0f};
    for (Verb verb : verbs) {
        switch (verb) {
            case Verb::MoveTo:
            case Verb::LineTo:
                last = points[pi++];
                b.add(last);
                break;
            case Verb::QuadTo: {
                const vec2 c = points[pi], e = points[pi + 1];
                pi += 2;
                for (int axis = 0; axis < 2; ++axis) {
                    const auto ax = static_cast<std::size_t>(axis);
                    const float den = last[ax] - 2.0f * c[ax] + e[ax];
                    if (den != 0.0f) {
                        const float t = (last[ax] - c[ax]) / den;
                        if (t > 0.0f && t < 1.0f) {
                            b.add(eval_quad(last, c, e, t));
                        }
                    }
                }
                b.add(e);
                last = e;
                break;
            }
            case Verb::CubicTo: {
                const vec2 c1 = points[pi], c2 = points[pi + 1], e = points[pi + 2];
                pi += 3;
                for (int axis = 0; axis < 2; ++axis) {
                    const auto ax = static_cast<std::size_t>(axis);
                    float ts[2];
                    const int n = cubic_extrema(last[ax], c1[ax], c2[ax], e[ax], ts);
                    for (int i = 0; i < n; ++i) {
                        b.add(eval_cubic(last, c1, c2, e, ts[i]));
                    }
                }
                b.add(e);
                last = e;
                break;
            }
            case Verb::Close:
                break;
        }
    }
    return b;
}

std::uint64_t hash_path(const Path& path, std::uint64_t seed) {
    std::uint64_t h = seed;
    auto mix = [&h](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    };
    mix(static_cast<std::uint32_t>(path.verbs.size()));
    for (Verb v : path.verbs) {
        mix(static_cast<std::uint32_t>(v));
    }
    for (vec2 p : path.points) {
        mix(std::bit_cast<std::uint32_t>(p.x()));
        mix(std::bit_cast<std::uint32_t>(p.y()));
    }
    return h;
}

// --- path data parser ----------------------------------------------------------

namespace {

class PathParser {
public:
    explicit PathParser(std::string_view s) : s_(s) {}

    bool at_end() {
        skip_ws();
        return i_ >= s_.size();
    }

    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\f')) {
            ++i_;
        }
    }
    void skip_ws_comma() {
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ',') {
            ++i_;
            skip_ws();
        }
    }

    bool peek_command(char& c) {
        skip_ws();
        if (i_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i_])) && s_[i_] != 'e' && s_[i_] != 'E') {
            c = s_[i_];
            return true;
        }
        return false;
    }
    void advance() { ++i_; }

    // True if the next token starts a number (implicit command repetition).
    bool number_next() {
        skip_ws_comma();
        if (i_ >= s_.size()) {
            return false;
        }
        const char c = s_[i_];
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
    }

    bool number(float& out) {
        skip_ws_comma();
        const std::size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
            ++i_;
        }
        bool digits = false;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
            ++i_;
            digits = true;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                ++i_;
                digits = true;
            }
        }
        if (!digits) {
            i_ = start;
            return false;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            std::size_t save = i_++;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
                ++i_;
            }
            if (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
                    ++i_;
                }
            } else {
                i_ = save; // "1e" followed by a non-digit: not an exponent
            }
        }
        // std::strtod needs a terminated string; numbers are short.
        char buffer[64];
        const std::size_t length = std::min<std::size_t>(i_ - start, sizeof(buffer) - 1);
        std::memcpy(buffer, s_.data() + start, length);
        buffer[length] = '\0';
        out = static_cast<float>(std::strtod(buffer, nullptr));
        return true;
    }

    // Arc flags are a single '0' or '1' and may be packed without separators.
    bool flag(bool& out) {
        skip_ws_comma();
        if (i_ < s_.size() && (s_[i_] == '0' || s_[i_] == '1')) {
            out = s_[i_] == '1';
            ++i_;
            return true;
        }
        return false;
    }

    std::size_t position() const { return i_; }

private:
    std::string_view s_;
    std::size_t i_ = 0;
};

} // namespace

bool parse_path_data(std::string_view data, Path& out, std::string* error) {
    PathParser p(data);
    auto fail = [&](std::string message) {
        if (error) {
            *error = std::format("path data, offset {}: {}", p.position(), message);
        }
        return false;
    };

    char command = 0;
    vec2 last_control{0.0f, 0.0f}; // for S/T reflection
    char previous = 0;             // previous command (upper case)
    bool first = true;

    while (!p.at_end()) {
        char c = 0;
        if (p.peek_command(c)) {
            p.advance();
            command = c;
        } else if (command == 0 || !p.number_next()) {
            return fail("expected a command");
        } else if (command == 'M') {
            command = 'L'; // implicit lineto after moveto (8.3.2)
        } else if (command == 'm') {
            command = 'l';
        } else if (command == 'Z' || command == 'z') {
            return fail("numbers after closepath");
        }
        if (first && command != 'M' && command != 'm') {
            return fail("path data must start with a moveto");
        }
        first = false;

        const bool rel = std::islower(static_cast<unsigned char>(command)) != 0;
        const vec2 cur = out.current_point();
        const vec2 base = rel ? cur : vec2(0.0f);
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        float v[7];
        auto read = [&](int n) {
            for (int k = 0; k < n; ++k) {
                if (!p.number(v[k])) {
                    return false;
                }
            }
            return true;
        };

        switch (upper) {
            case 'M':
                if (!read(2)) return fail("moveto needs 2 numbers");
                out.move_to(base + vec2(v[0], v[1]));
                break;
            case 'L':
                if (!read(2)) return fail("lineto needs 2 numbers");
                out.line_to(base + vec2(v[0], v[1]));
                break;
            case 'H':
                if (!read(1)) return fail("horizontal lineto needs 1 number");
                out.line_to({(rel ? cur.x() : 0.0f) + v[0], cur.y()});
                break;
            case 'V':
                if (!read(1)) return fail("vertical lineto needs 1 number");
                out.line_to({cur.x(), (rel ? cur.y() : 0.0f) + v[0]});
                break;
            case 'C':
                if (!read(6)) return fail("curveto needs 6 numbers");
                last_control = base + vec2(v[2], v[3]);
                out.cubic_to(base + vec2(v[0], v[1]), last_control, base + vec2(v[4], v[5]));
                break;
            case 'S': {
                if (!read(4)) return fail("smooth curveto needs 4 numbers");
                // Reflect the previous cubic's second control point (8.3.6).
                const vec2 c1 = (previous == 'C' || previous == 'S') ? cur * 2.0f - last_control : cur;
                last_control = base + vec2(v[0], v[1]);
                out.cubic_to(c1, last_control, base + vec2(v[2], v[3]));
                break;
            }
            case 'Q':
                if (!read(4)) return fail("quadratic curveto needs 4 numbers");
                last_control = base + vec2(v[0], v[1]);
                out.quad_to(last_control, base + vec2(v[2], v[3]));
                break;
            case 'T': {
                if (!read(2)) return fail("smooth quadratic curveto needs 2 numbers");
                last_control = (previous == 'Q' || previous == 'T') ? cur * 2.0f - last_control : cur;
                out.quad_to(last_control, base + vec2(v[0], v[1]));
                break;
            }
            case 'A': {
                bool large = false, sweep = false;
                if (!read(3) || !p.flag(large) || !p.flag(sweep) || !p.number(v[3]) || !p.number(v[4])) {
                    return fail("arc needs rx ry rotation large-arc-flag sweep-flag x y");
                }
                constexpr float deg_to_rad = 3.14159265358979323846f / 180.0f;
                out.arc_to({v[0], v[1]}, v[2] * deg_to_rad, large, sweep, base + vec2(v[3], v[4]));
                break;
            }
            case 'Z':
                out.close();
                break;
            default:
                return fail(std::format("unknown command '{}'", command));
        }
        previous = upper;
    }
    return true;
}

} // namespace cuervo::vector

#include "editor/rml/script/code_view.hpp"

#include <algorithm>
#include <cmath>

namespace cuervo::editor {

namespace {

const Rml::String kNoLanguage;

// Premultiplied quads and triangles for one pass.
struct Batch {
    Rml::Mesh mesh;
    void quad(float x, float y, float w, float h, Rml::ColourbPremultiplied c) {
        if (w <= 0 || h <= 0) return;
        const int b = static_cast<int>(mesh.vertices.size());
        mesh.vertices.push_back({{x, y}, c, {0, 0}});
        mesh.vertices.push_back({{x + w, y}, c, {0, 0}});
        mesh.vertices.push_back({{x + w, y + h}, c, {0, 0}});
        mesh.vertices.push_back({{x, y + h}, c, {0, 0}});
        for (int k : {0, 1, 2, 0, 2, 3}) mesh.indices.push_back(b + k);
    }
    void tri(Rml::Vector2f a, Rml::Vector2f b, Rml::Vector2f c, Rml::ColourbPremultiplied col) {
        const int n = static_cast<int>(mesh.vertices.size());
        mesh.vertices.push_back({a, col, {0, 0}});
        mesh.vertices.push_back({b, col, {0, 0}});
        mesh.vertices.push_back({c, col, {0, 0}});
        for (int k : {0, 1, 2}) mesh.indices.push_back(n + k);
    }
    void disc(float cx, float cy, float r, Rml::ColourbPremultiplied col) {
        constexpr int n = 12;
        for (int i = 0; i < n; ++i) {
            const float a0 = 6.2831853f * static_cast<float>(i) / n, a1 = 6.2831853f * static_cast<float>(i + 1) / n;
            tri({cx, cy}, {cx + r * std::cos(a0), cy + r * std::sin(a0)}, {cx + r * std::cos(a1), cy + r * std::sin(a1)}, col);
        }
    }
    void render(Rml::RenderManager* rm) {
        if (mesh.indices.empty()) return;
        Rml::Geometry g = rm->MakeGeometry(std::move(mesh));
        g.Render({0, 0});
    }
};

// Tabs to spaces for drawing; `dcol` is the display column the text starts at.
std::string expand(std::string_view s, int dcol, int tab) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '\t') {
            const int n = tab - (dcol + static_cast<int>(out.size())) % tab;
            out.append(static_cast<std::size_t>(n), ' ');
            ++i;
        } else {
            const std::size_t e = next_boundary(s, i);
            out.append(s.substr(i, e - i));
            i = e;
        }
    }
    return out;
}

Rml::ColourbPremultiplied premul(Rml::Colourb c) { return c.ToPremultiplied(); }

constexpr const char* kProbes[] = {"p-text", "p-kw", "p-ty", "p-str", "p-num", "p-cm", "p-err", "p-warn", "p-note", "p-gut", "p-gutcur", "p-sel", "p-curline", "p-caret", "p-gutbg", "p-match", "p-matchcur", "p-fn", "p-const"};

} // namespace

void register_code_view() {
    static Rml::ElementInstancerGeneric<CodeView> instancer;
    Rml::Factory::RegisterElementInstancer("code-view", &instancer);
}

CodeView::CodeView(const Rml::String& tag_name) : Rml::Element(tag_name) {
    // Mousemove and Mouseout have no default-action phase in RmlUi, so they come through a listener.
    AddEventListener(Rml::EventId::Mousemove, this);
    AddEventListener(Rml::EventId::Mouseout, this);
}
CodeView::~CodeView() = default;

double CodeView::now() const { return Rml::GetSystemInterface() != nullptr ? Rml::GetSystemInterface()->GetElapsedTime() : 0.0; }
void CodeView::blink_reset() { blink_t0_ = now(); }

void CodeView::set_doc(ScriptWorkspace* ws, int doc_id) {
    ws_ = ws;
    doc_id_ = doc_id;
    seen_rev_ = ~std::uint64_t{0};
    geo_.clear();
    matches_.clear();
    match_cur_ = -1;
    scroll_x_ = 0;
    if (ScriptDoc* d = doc()) {
        // Remember nothing about the old document's scroll: show the caret.
        scroll_y_ = 0;
        reveal(d->buf.primary().head, false);
    }
}

void CodeView::OnResize() { clamp_scroll(); }

void CodeView::measure() {
    ratio_ = GetContext() != nullptr ? GetContext()->GetDensityIndependentPixelRatio() : 1.0f;
    face_ = GetFontFaceHandle();
    if (face_ == 0) return;
    Rml::FontEngineInterface* fe = Rml::GetFontEngineInterface();
    const Rml::FontMetrics& fm = fe->GetFontMetrics(face_);
    const Rml::TextShapingContext shaping{kNoLanguage};
    cw_ = std::max(1, fe->GetStringWidth(face_, "M", shaping));
    const int size = fm.size > 0 ? fm.size : 13;
    lh_ = std::max(static_cast<int>(std::lround(static_cast<float>(size) * 1.55f)), static_cast<int>(std::ceil(fm.ascent + fm.descent)) + 2);
    ascent_ = static_cast<int>(std::lround((static_cast<float>(lh_) - (fm.ascent + fm.descent)) * 0.5f + fm.ascent));
    const int ver = fe->GetVersion(face_);
    if (ver != face_version_) { face_version_ = ver; geo_.clear(); }
}

void CodeView::read_colors() {
    if (GetNumChildren() == 0) {
        Rml::ElementDocument* d = GetOwnerDocument();
        if (d == nullptr) return;
        for (const char* p : kProbes) {
            Rml::ElementPtr e = d->CreateElement("div");
            e->SetClass("p", true);
            e->SetClass(p, true);
            AppendChild(std::move(e));
        }
    }
    const auto col = [&](int i) { return premul(GetChild(i)->GetComputedValues().color()); };
    Colors c{};
    if (GetNumChildren() >= static_cast<int>(std::size(kProbes))) {
        c.text = col(0); c.kw = col(1); c.ty = col(2); c.str = col(3); c.num = col(4); c.cm = col(5); c.err = col(6); c.warn = col(7); c.note = col(8);
        c.gutter = col(9); c.gutter_cur = col(10); c.sel = col(11); c.cur_line = col(12); c.caret = col(13); c.gutter_bg = col(14); c.match = col(15); c.match_cur = col(16); c.fn = col(17); c.cst = col(18);
    }
    if (!have_colors_ || !(c == colors_)) { colors_ = c; have_colors_ = true; geo_.clear(); }
}

Rml::ColourbPremultiplied CodeView::color_of(Syn k) const {
    switch (k) {
    case Syn::Keyword: return colors_.kw;
    case Syn::Type: return colors_.ty;
    case Syn::String: return colors_.str;
    case Syn::Number: return colors_.num;
    case Syn::Comment: return colors_.cm;
    case Syn::Error: return colors_.err;
    case Syn::Function: return colors_.fn;
    case Syn::Constant: return colors_.cst;
    case Syn::Plain: break;
    }
    return colors_.text;
}

Rml::Vector2f CodeView::origin() const { return const_cast<CodeView*>(this)->GetAbsoluteOffset(Rml::BoxArea::Content); }

int CodeView::gutter_w() const {
    const ScriptDoc* d = doc();
    int digits = 3;
    if (d != nullptr) for (int n = d->buf.line_count(); n >= 1000; n /= 10) ++digits;
    // marker column + numbers + right padding
    return static_cast<int>(std::lround(16.0f * ratio_)) + digits * cw_ + static_cast<int>(std::lround(10.0f * ratio_));
}
float CodeView::text_x() const { return origin().x + static_cast<float>(gutter_w()) + std::round(8.0f * ratio_) - scroll_x_; }

void CodeView::clamp_scroll() {
    const ScriptDoc* d = doc();
    if (d == nullptr) { scroll_y_ = scroll_x_ = 0; return; }
    const float h = GetBox().GetSize(Rml::BoxArea::Content).y;
    const float max_y = std::max(0.0f, static_cast<float>(d->buf.line_count() * lh_) - std::max(h, static_cast<float>(lh_)) + std::max(h * 0.5f, 0.0f));
    scroll_y_ = std::clamp(std::round(scroll_y_), 0.0f, max_y);
    scroll_x_ = std::max(0.0f, std::round(scroll_x_));
}

void CodeView::reveal(TextPos p, bool center) {
    ScriptDoc* d = doc();
    if (d == nullptr) return;
    measure();
    const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.y <= static_cast<float>(lh_)) return; // not laid out yet
    const float top = static_cast<float>(p.line * lh_), bottom = top + static_cast<float>(lh_);
    if (center && (top < scroll_y_ || bottom > scroll_y_ + size.y)) scroll_y_ = top - (size.y - static_cast<float>(lh_)) * 0.5f;
    else if (top < scroll_y_) scroll_y_ = top;
    else if (bottom > scroll_y_ + size.y) scroll_y_ = bottom - size.y;
    const float avail = size.x - static_cast<float>(gutter_w()) - std::round(16.0f * ratio_);
    if (avail <= static_cast<float>(cw_ * 8)) { clamp_scroll(); return; } // not laid out yet
    const float cx = static_cast<float>(display_col(d->buf.line(std::clamp(p.line, 0, d->buf.line_count() - 1)), p.col, d->buf.indent().width) * cw_);
    if (cx < scroll_x_) scroll_x_ = std::max(0.0f, cx - static_cast<float>(cw_ * 4));
    else if (cx + static_cast<float>(cw_) > scroll_x_ + avail) scroll_x_ = cx + static_cast<float>(cw_) - avail + static_cast<float>(cw_ * 4);
    clamp_scroll();
}

Rml::Rectanglef CodeView::rect_of(TextPos p) const {
    const ScriptDoc* d = doc();
    if (d == nullptr) return {};
    p = d->buf.clamp(p);
    const float x = text_x() + static_cast<float>(display_col(d->buf.line(p.line), p.col, d->buf.indent().width) * cw_);
    const float y = origin().y + static_cast<float>(p.line * lh_) - scroll_y_;
    return Rml::Rectanglef::FromPositionSize({x, y}, {static_cast<float>(cw_), static_cast<float>(lh_)});
}
Rml::Rectanglef CodeView::caret_rect() const { const ScriptDoc* d = doc(); return d != nullptr ? rect_of(d->buf.primary().head) : Rml::Rectanglef{}; }

TextPos CodeView::pos_at(Rml::Vector2f p) const {
    const ScriptDoc* d = doc();
    if (d == nullptr) return {};
    const int line = std::clamp(static_cast<int>(std::floor((p.y - origin().y + scroll_y_) / static_cast<float>(lh_))), 0, d->buf.line_count() - 1);
    const int dcol = std::max(0, static_cast<int>(std::floor((p.x - text_x()) / static_cast<float>(cw_) + 0.5f)));
    return {line, col_at_display(d->buf.line(line), dcol, d->buf.indent().width)};
}

CodeView::LineGeo& CodeView::line_geo(ScriptDoc& d, int line) {
    const LineSyn& ls = d.syn.line(line);
    LineGeo& g = geo_[ls.stamp];
    g.used = frame_;
    if (!g.parts.empty() || ls.stamp == 0) return g;
    Rml::RenderManager* rm = GetRenderManager();
    Rml::FontEngineInterface* fe = Rml::GetFontEngineInterface();
    const std::string& text = d.buf.line(line);
    const int tab = d.buf.indent().width;
    const Rml::TextShapingContext shaping{kNoLanguage};
    std::size_t pos = 0;
    const auto emit = [&](std::size_t a, std::size_t b, Syn kind) {
        if (b <= a) return;
        const int dcol = display_col(text, static_cast<int>(a), tab);
        const std::string s = expand(std::string_view(text).substr(a, b - a), dcol, tab);
        if (s.find_first_not_of(' ') == std::string::npos) return;
        Rml::TexturedMeshList list;
        fe->GenerateString(*rm, face_, 0, s, {static_cast<float>(dcol * cw_), static_cast<float>(ascent_)}, color_of(kind), 1.0f, shaping, list);
        for (Rml::TexturedMesh& tm : list) g.parts.emplace_back(rm->MakeGeometry(std::move(tm.mesh)), tm.texture);
    };
    for (const SynSpan& sp : ls.spans) {
        emit(pos, std::min<std::size_t>(sp.col, text.size()), Syn::Plain);
        emit(sp.col, std::min<std::size_t>(sp.col + sp.len, text.size()), sp.kind);
        pos = std::min<std::size_t>(sp.col + sp.len, text.size());
    }
    emit(pos, text.size(), Syn::Plain);
    return g;
}

CodeView::LineGeo& CodeView::number_geo(int line, bool current) {
    LineGeo& g = geo_[(std::uint64_t{1} << 40) | (static_cast<std::uint64_t>(line) << 1) | (current ? 1u : 0u)];
    g.used = frame_;
    if (!g.parts.empty()) return g;
    Rml::RenderManager* rm = GetRenderManager();
    const std::string s = std::to_string(line + 1);
    const Rml::TextShapingContext shaping{kNoLanguage};
    Rml::TexturedMeshList list;
    // Right-aligned to the numbers column.
    const int digits = std::max(3, static_cast<int>(s.size()));
    (void)digits;
    Rml::GetFontEngineInterface()->GenerateString(*rm, face_, 0, s, {0.0f, static_cast<float>(ascent_)}, current ? colors_.gutter_cur : colors_.gutter, 1.0f, shaping, list);
    for (Rml::TexturedMesh& tm : list) g.parts.emplace_back(rm->MakeGeometry(std::move(tm.mesh)), tm.texture);
    return g;
}

void CodeView::OnRender() {
    Rml::RenderManager* rm = GetRenderManager();
    ScriptDoc* d = doc();
    if (rm == nullptr || d == nullptr) return;
    measure();
    read_colors();
    if (face_ == 0 || !have_colors_) return;
    ++frame_;
    d->syn.update(d->buf);
    clamp_scroll();

    const Rml::Vector2f org = origin();
    const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    const int tab = d->buf.indent().width;
    const int n = d->buf.line_count();
    first_vis_ = std::clamp(static_cast<int>(std::floor(scroll_y_ / static_cast<float>(lh_))), 0, n - 1);
    last_vis_ = std::clamp(static_cast<int>(std::ceil((scroll_y_ + size.y) / static_cast<float>(lh_))), first_vis_, n - 1);
    const float gw = static_cast<float>(gutter_w());
    const float tx = text_x();
    const auto line_y = [&](int l) { return org.y + static_cast<float>(l * lh_) - scroll_y_; };
    const bool blink_on = focused_ && (static_cast<long>((now() - blink_t0_) * 1.9) % 2 == 0);

    // Clip to the content box (partial lines at the edges).
    const Rml::Rectanglei old_scissor = rm->GetScissorRegion();
    rm->SetScissorRegion(Rml::Rectanglei::FromPositionSize({static_cast<int>(org.x), static_cast<int>(org.y)}, {static_cast<int>(size.x), static_cast<int>(size.y)}));

    Batch under;
    const TextPos head = d->buf.primary().head;
    // Gutter background, current line, selections, find matches.
    if (focused_ && !d->buf.has_selection()) under.quad(org.x + gw, line_y(head.line), size.x - gw, static_cast<float>(lh_), colors_.cur_line);
    const bool paused_here = d->id == paused_doc_ && paused_line_ >= first_vis_ && paused_line_ <= last_vis_;
    if (paused_here) {
        Rml::ColourbPremultiplied f = colors_.warn;
        for (Rml::byte* ch : {&f.red, &f.green, &f.blue, &f.alpha}) *ch = static_cast<Rml::byte>(*ch * 0.24f);
        under.quad(org.x + gw, line_y(paused_line_), size.x - gw, static_cast<float>(lh_), f);
    }
    const auto span_rect = [&](Batch& b, TextRange r, Rml::ColourbPremultiplied c, bool eol_pad) {
        for (int l = std::max(r.start.line, first_vis_); l <= std::min(r.end.line, last_vis_); ++l) {
            const std::string& s = d->buf.line(l);
            const int a = l == r.start.line ? display_col(s, r.start.col, tab) : 0;
            int e = l == r.end.line ? display_col(s, r.end.col, tab) : display_col(s, static_cast<int>(s.size()), tab) + (eol_pad ? 1 : 0);
            if (e <= a && !(l == r.end.line && l == r.start.line)) e = a + 1;
            b.quad(tx + static_cast<float>(a * cw_), line_y(l), static_cast<float>((e - a) * cw_), static_cast<float>(lh_), c);
        }
    };
    for (std::size_t i = 0; i < matches_.size(); ++i) {
        if (matches_[i].end.line < first_vis_ || matches_[i].start.line > last_vis_) continue;
        span_rect(under, matches_[i], static_cast<int>(i) == match_cur_ ? colors_.match_cur : colors_.match, false);
    }
    for (const Selection& s : d->buf.selections()) if (!s.empty()) span_rect(under, {s.start(), s.end()}, colors_.sel, true);
    // Bracket pair next to the caret: filled cells here, an underline over the text below (shape, not color alone).
    TextPos br_open, br_close;
    const bool br = !d->buf.has_selection() && d->buf.match_bracket(head, br_open, br_close);
    if (br) {
        for (const TextPos p : {br_open, br_close}) {
            if (p.line < first_vis_ || p.line > last_vis_) continue;
            const int a = display_col(d->buf.line(p.line), p.col, tab);
            under.quad(tx + static_cast<float>(a * cw_), line_y(p.line), static_cast<float>(cw_), static_cast<float>(lh_), colors_.match_cur);
        }
    }
    under.render(rm);

    // Text, clipped to the area right of the gutter.
    const Rml::Rectanglei full_clip = Rml::Rectanglei::FromPositionSize({static_cast<int>(org.x), static_cast<int>(org.y)}, {static_cast<int>(size.x), static_cast<int>(size.y)});
    rm->SetScissorRegion(Rml::Rectanglei::FromPositionSize({static_cast<int>(org.x + gw), static_cast<int>(org.y)}, {std::max(0, static_cast<int>(size.x - gw)), static_cast<int>(size.y)}));
    for (int l = first_vis_; l <= last_vis_; ++l) {
        LineGeo& g = line_geo(*d, l);
        for (auto& [geo, tex] : g.parts) geo.Render({tx, line_y(l)}, tex);
    }

    // Squiggles and caret over the text; gutter marks and numbers last, over the gutter.
    Batch over, gut;
    const float px = std::max(1.0f, std::round(ratio_));
    gut.quad(org.x, org.y, gw, size.y, colors_.gutter_bg);
    // Breakpoints: a filled disc in the marker column; the paused line adds a right-pointing arrow and a bar.
    if (d->bps != nullptr) {
        for (const int l1 : *d->bps) {
            const int l = l1 - 1;
            if (l < first_vis_ || l > last_vis_) continue;
            gut.disc(org.x + std::round(8.0f * ratio_), line_y(l) + static_cast<float>(lh_) * 0.5f, 4.6f * ratio_, colors_.err);
        }
    }
    if (paused_here) {
        const float cx = org.x + std::round(8.0f * ratio_), cy = line_y(paused_line_) + static_cast<float>(lh_) * 0.5f, r = 4.6f * ratio_;
        gut.quad(org.x, line_y(paused_line_), 2.0f * px, static_cast<float>(lh_), colors_.warn);
        gut.tri({cx - r, cy - r}, {cx - r, cy + r}, {cx + r * 1.2f, cy}, colors_.warn);
    }
    for (const Problem& p : d->problems) {
        const Rml::ColourbPremultiplied c = p.severity == ProblemSeverity::Error ? colors_.err : p.severity == ProblemSeverity::Warning ? colors_.warn : colors_.note;
        for (int l = std::max(p.range.start.line, first_vis_); l <= std::min(p.range.end.line, last_vis_); ++l) {
            const std::string& s = d->buf.line(l);
            const int a = l == p.range.start.line ? display_col(s, p.range.start.col, tab) : 0;
            int e = l == p.range.end.line ? display_col(s, p.range.end.col, tab) : display_col(s, static_cast<int>(s.size()), tab);
            if (e <= a) e = a + 1;
            const float y = line_y(l) + static_cast<float>(lh_) - 3.0f * px;
            for (int x = a * cw_; x < e * cw_; x += static_cast<int>(2 * px)) over.quad(tx + static_cast<float>(x), y + (((x / static_cast<int>(2 * px)) % 2) != 0 ? px : 0.0f), 2.0f * px, px, c);
        }
        // Gutter marks differ by shape (not color alone): error = disc, warning = triangle, note = bar.
        const int ml = p.range.start.line;
        if (ml >= first_vis_ && ml <= last_vis_) {
            const float cx = org.x + std::round(8.0f * ratio_), cy = line_y(ml) + static_cast<float>(lh_) * 0.5f, r = 3.5f * ratio_;
            if (p.severity == ProblemSeverity::Error) gut.disc(cx, cy, r, c);
            else if (p.severity == ProblemSeverity::Warning) gut.tri({cx, cy - r}, {cx + r * 1.1f, cy + r * 0.8f}, {cx - r * 1.1f, cy + r * 0.8f}, c);
            else gut.quad(cx - r, cy - px * 0.5f, 2.0f * r, px * 1.5f, c);
        }
    }
    if (br) {
        for (const TextPos p : {br_open, br_close}) {
            if (p.line < first_vis_ || p.line > last_vis_) continue;
            const int a = display_col(d->buf.line(p.line), p.col, tab);
            over.quad(tx + static_cast<float>(a * cw_), line_y(p.line) + static_cast<float>(lh_) - px, static_cast<float>(cw_), px, colors_.caret);
        }
    }
    if (blink_on) {
        for (const Selection& s : d->buf.selections()) {
            if (s.head.line < first_vis_ || s.head.line > last_vis_) continue;
            const float x = tx + static_cast<float>(display_col(d->buf.line(s.head.line), s.head.col, tab) * cw_);
            over.quad(x, line_y(s.head.line) + 1.0f, std::max(1.0f, std::round(1.5f * ratio_)), static_cast<float>(lh_ - 2), colors_.caret);
        }
    }
    over.render(rm);
    rm->SetScissorRegion(full_clip);
    gut.render(rm);
    const int num_right = static_cast<int>(gw) - static_cast<int>(std::lround(10.0f * ratio_));
    for (int l = first_vis_; l <= last_vis_; ++l) {
        LineGeo& num = number_geo(l, l == head.line);
        const float nx = org.x + static_cast<float>(num_right - static_cast<int>(std::to_string(l + 1).size()) * cw_);
        for (auto& [geo, tex] : num.parts) geo.Render({nx, line_y(l)}, tex);
    }
    rm->SetScissorRegion(old_scissor);

    // Prune geometry that was not used for a while (keeps memory flat while scrolling).
    if (geo_.size() > static_cast<std::size_t>((last_vis_ - first_vis_ + 1) * 4 + 256)) {
        for (auto it = geo_.begin(); it != geo_.end();) it = it->second.used + 2 < frame_ ? geo_.erase(it) : std::next(it);
    }
}

// ------------------------------------------------------------------ input

void CodeView::edit_done() {
    ScriptDoc* d = doc();
    if (d == nullptr) return;
    reveal(d->buf.primary().head, false);
    blink_reset();
    if (hooks.changed) hooks.changed();
    if (hooks.caret_moved) hooks.caret_moved();
}

void CodeView::do_move(Move m, bool extend) {
    ScriptDoc* d = doc();
    if (d == nullptr) return;
    const int page = std::max(1, static_cast<int>(GetBox().GetSize(Rml::BoxArea::Content).y / static_cast<float>(lh_)) - 1);
    d->buf.move(m, extend, page);
    reveal(d->buf.primary().head, false);
    blink_reset();
    if (hooks.caret_moved) hooks.caret_moved();
}

void CodeView::copy() {
    ScriptDoc* d = doc();
    if (d == nullptr) return;
    std::string text;
    if (d->buf.has_selection()) { text = d->buf.selected_text(); line_clip_ = false; }
    else { text = d->buf.line(d->buf.primary().head.line) + "\n"; line_clip_ = true; line_clip_text_ = text; }
    Rml::GetSystemInterface()->SetClipboardText(text);
}

void CodeView::cut() {
    ScriptDoc* d = doc();
    if (d == nullptr) return;
    copy();
    if (d->buf.has_selection()) d->buf.insert("");
    else d->buf.delete_line();
    edit_done();
}

void CodeView::paste() {
    ScriptDoc* d = doc();
    if (d == nullptr) return;
    Rml::String raw;
    Rml::GetSystemInterface()->GetClipboardText(raw);
    const std::string text = TextBuffer::normalize(raw);
    if (text.empty()) return;
    if (line_clip_ && text == line_clip_text_ && !d->buf.has_selection()) {
        // A copied whole line goes above the caret's line, caret keeps its column.
        const TextPos c = d->buf.primary().head;
        d->buf.replace({{c.line, 0}, {c.line, 0}}, text);
        d->buf.set_caret({c.line + 1, c.col});
    } else d->buf.insert(text);
    edit_done();
}

bool CodeView::key_down(Rml::Event& e) {
    ScriptDoc* d = doc();
    if (d == nullptr) return false;
    using K = Rml::Input::KeyIdentifier;
    const auto kid = static_cast<K>(e.GetParameter("key_identifier", 0));
    const bool ctrl = e.GetParameter("ctrl_key", 0) != 0, shift = e.GetParameter("shift_key", 0) != 0, alt = e.GetParameter("alt_key", 0) != 0;
    if (hooks.key && hooks.key(kid, ctrl, shift, alt)) return true;
    TextBuffer& b = d->buf;
    switch (kid) {
    case K::KI_LEFT: do_move(ctrl ? Move::WordLeft : Move::Left, shift); return true;
    case K::KI_RIGHT: do_move(ctrl ? Move::WordRight : Move::Right, shift); return true;
    case K::KI_UP:
        if (alt && !shift) { b.move_lines(-1); edit_done(); } else do_move(Move::Up, shift);
        return true;
    case K::KI_DOWN:
        if (alt && !shift) { b.move_lines(1); edit_done(); } else do_move(Move::Down, shift);
        return true;
    case K::KI_HOME: do_move(ctrl ? Move::DocStart : Move::SmartHome, shift); return true;
    case K::KI_END: do_move(ctrl ? Move::DocEnd : Move::LineEnd, shift); return true;
    case K::KI_PRIOR: do_move(Move::PageUp, shift); return true;
    case K::KI_NEXT: do_move(Move::PageDown, shift); return true;
    case K::KI_BACK: ctrl ? b.delete_word_back() : b.backspace(); edit_done(); return true;
    case K::KI_DELETE: ctrl ? b.delete_word_forward() : b.delete_forward(); edit_done(); return true;
    case K::KI_RETURN: case K::KI_NUMPADENTER: b.insert_newline(); edit_done(); return true;
    case K::KI_TAB: {
        if (ctrl) return false; // Ctrl+Tab belongs to the tab switcher
        const Selection& s = b.primary();
        if (shift) b.indent_lines(true);
        else if (s.start().line != s.end().line) b.indent_lines(false);
        else if (b.indent().tabs) b.insert("\t", true, now_ms());
        else {
            const int dc = display_col(b.line(s.head.line), s.head.col, b.indent().width);
            b.insert(std::string(static_cast<std::size_t>(b.indent().width - dc % b.indent().width), ' '), true, now_ms());
        }
        edit_done();
        return true;
    }
    case K::KI_A: if (!ctrl) return false; b.select_all(); if (hooks.caret_moved) hooks.caret_moved(); return true;
    case K::KI_Z: if (!ctrl) return false; (shift ? b.redo() : b.undo()); edit_done(); return true;
    case K::KI_Y: if (!ctrl) return false; b.redo(); edit_done(); return true;
    case K::KI_C: if (!ctrl) return false; copy(); return true;
    case K::KI_X: if (!ctrl) return false; cut(); return true;
    case K::KI_V: if (!ctrl) return false; paste(); return true;
    case K::KI_INSERT: if (ctrl) { copy(); return true; } if (shift) { paste(); return true; } return false;
    case K::KI_D: if (!ctrl) return false; b.duplicate_line(); edit_done(); return true;
    case K::KI_K: if (!(ctrl && shift)) return false; b.delete_line(); edit_done(); return true;
    case K::KI_OEM_2: if (!ctrl) return false; b.toggle_comment(); edit_done(); return true;
    case K::KI_ESCAPE:
        if (b.selections().size() > 1 || b.has_selection()) { b.set_caret(b.primary().head); if (hooks.caret_moved) hooks.caret_moved(); return true; }
        return false;
    default: return false;
    }
}

void CodeView::mouse_down(Rml::Event& e) {
    ScriptDoc* d = doc();
    if (d == nullptr) return;
    const Rml::Vector2f at{e.GetParameter("mouse_x", 0.0f), e.GetParameter("mouse_y", 0.0f)};
    if (e.GetParameter("button", 0) != 0) return;
    Focus();
    const bool shift = e.GetParameter("shift_key", 0) != 0, ctrl = e.GetParameter("ctrl_key", 0) != 0, alt = e.GetParameter("alt_key", 0) != 0;
    if (at.x < origin().x + static_cast<float>(gutter_w())) {
        const TextPos p = pos_at({text_x(), at.y});
        // The marker column toggles a breakpoint; the line numbers select the line.
        if (at.x < origin().x + std::round(20.0f * ratio_) && hooks.gutter_click) { hooks.gutter_click(p.line); return; }
        d->buf.set_selections({{{p.line, 0}, {std::min(p.line + 1, d->buf.line_count() - 1), p.line + 1 < d->buf.line_count() ? 0 : static_cast<int>(d->buf.line(p.line).size())}}});
        drag_mode_ = 2;
        drag_anchor_ = {p.line, 0};
        dragging_ = true;
        blink_reset();
        return;
    }
    const TextPos p = pos_at(at);
    const double t = now();
    const bool same = std::abs(at.x - last_click_at_.x) + std::abs(at.y - last_click_at_.y) < 6.0f * ratio_ && t - last_click_t_ < 0.45;
    click_count_ = same ? click_count_ % 3 + 1 : 1;
    last_click_t_ = t;
    last_click_at_ = at;
    if (ctrl && !shift && !alt && hooks.ctrl_click) { d->buf.set_caret(p); hooks.ctrl_click(p); blink_reset(); return; }
    drag_mode_ = click_count_ - 1;
    if (click_count_ == 2) {
        const TextRange w = d->buf.word_at(p);
        d->buf.select_range(w.empty() ? TextRange{p, p} : w);
        drag_anchor_ = w.start;
    } else if (click_count_ == 3) {
        const int l = p.line;
        d->buf.select_range({{l, 0}, l + 1 < d->buf.line_count() ? TextPos{l + 1, 0} : TextPos{l, static_cast<int>(d->buf.line(l).size())}});
        drag_anchor_ = {l, 0};
    } else if (alt) d->buf.add_caret(p);
    else { d->buf.set_caret(p, shift); drag_anchor_ = d->buf.primary().anchor; }
    dragging_ = true;
    blink_reset();
    if (hooks.caret_moved) hooks.caret_moved();
}

void CodeView::drag_to(Rml::Event& e) {
    ScriptDoc* d = doc();
    if (d == nullptr || !dragging_) return;
    const Rml::Vector2f at{e.GetParameter("mouse_x", 0.0f), e.GetParameter("mouse_y", 0.0f)};
    TextPos p = pos_at(at);
    const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    // Auto scroll while the pointer is past an edge.
    if (at.y < origin().y) scroll_y_ -= static_cast<float>(lh_);
    else if (at.y > origin().y + size.y) scroll_y_ += static_cast<float>(lh_);
    if (at.x < origin().x + static_cast<float>(gutter_w()) && scroll_x_ > 0) scroll_x_ -= static_cast<float>(cw_ * 2);
    else if (at.x > origin().x + size.x) scroll_x_ += static_cast<float>(cw_ * 2);
    clamp_scroll();
    if (drag_mode_ == 1) {
        const TextRange w = d->buf.word_at(p);
        const TextRange base = d->buf.word_at(drag_anchor_);
        const TextPos a = (w.empty() ? p : w.start) < base.start ? (w.empty() ? p : w.start) : base.start;
        const TextPos z = base.end < (w.empty() ? p : w.end) ? (w.empty() ? p : w.end) : base.end;
        d->buf.set_selections({{p < drag_anchor_ ? z : a, p < drag_anchor_ ? a : z}});
    } else if (drag_mode_ == 2) {
        const TextPos end = p.line + 1 < d->buf.line_count() ? TextPos{p.line + 1, 0} : TextPos{p.line, static_cast<int>(d->buf.line(p.line).size())};
        if (p.line >= drag_anchor_.line) d->buf.set_selections({{drag_anchor_, end}});
        else d->buf.set_selections({{{drag_anchor_.line + 1 < d->buf.line_count() ? drag_anchor_.line + 1 : drag_anchor_.line, 0}, {p.line, 0}}});
    } else {
        d->buf.set_selections({{drag_anchor_, p}});
    }
    blink_reset();
    if (hooks.caret_moved) hooks.caret_moved();
}

void CodeView::ProcessEvent(Rml::Event& e) {
    if (e.GetCurrentElement() != this) return;
    switch (e.GetId()) {
    case Rml::EventId::Mousemove:
        if (hooks.hover && !dragging_) {
            const Rml::Vector2f at{e.GetParameter("mouse_x", 0.0f), e.GetParameter("mouse_y", 0.0f)};
            if (at.x >= origin().x + static_cast<float>(gutter_w())) hooks.hover(pos_at(at), at);
            else hooks.hover({-1, 0}, at);
        }
        break;
    case Rml::EventId::Mouseout: if (hooks.hover) hooks.hover({-1, 0}, {}); break;
    default: break;
    }
}

void CodeView::ProcessDefaultAction(Rml::Event& e) {
    Rml::Element::ProcessDefaultAction(e);
    switch (e.GetId()) {
    case Rml::EventId::Mousedown: mouse_down(e); break;
    case Rml::EventId::Mouseup: dragging_ = false; break;
    case Rml::EventId::Dragstart: case Rml::EventId::Drag: drag_to(e); break;
    case Rml::EventId::Dragend: dragging_ = false; break;
    case Rml::EventId::Mousescroll: {
        const float dy = e.GetParameter("wheel_delta_y", 0.0f), dx = e.GetParameter("wheel_delta_x", 0.0f);
        if (e.GetParameter("shift_key", 0) != 0) scroll_x_ += (dy + dx) * static_cast<float>(cw_) * 6.0f;
        else { scroll_y_ += dy * static_cast<float>(lh_) * 3.0f; scroll_x_ += dx * static_cast<float>(cw_) * 6.0f; }
        clamp_scroll();
        e.StopPropagation();
        break;
    }
    case Rml::EventId::Keydown:
        if (key_down(e)) e.StopPropagation();
        break;
    case Rml::EventId::Textinput: {
        ScriptDoc* d = doc();
        const Rml::String text = e.GetParameter("text", Rml::String());
        if (d != nullptr && !text.empty() && text[0] >= ' ') {
            d->buf.type(text, now_ms());
            if (hooks.typed) hooks.typed(text);
            edit_done();
        }
        break;
    }
    case Rml::EventId::Focus:
        if (e.GetTargetElement() == this) {
            focused_ = true;
            blink_reset();
            Rml::GetSystemInterface()->ActivateKeyboard(caret_rect().TopLeft(), static_cast<float>(lh_));
        }
        break;
    case Rml::EventId::Blur:
        if (e.GetTargetElement() == this) { focused_ = false; dragging_ = false; Rml::GetSystemInterface()->DeactivateKeyboard(); }
        break;
    default: break;
    }
}

} // namespace cuervo::editor

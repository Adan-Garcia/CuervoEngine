#include "editor/text_buffer.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace cuervo::editor {

namespace {

// Decodes the code point at s[off]; `len` receives its byte length (1 for invalid bytes).
std::uint32_t decode(std::string_view s, std::size_t off, std::size_t& len) {
    const auto b = static_cast<unsigned char>(s[off]);
    len = 1;
    if (b < 0x80) return b;
    const int n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
    if (n == 1 || off + static_cast<std::size_t>(n) > s.size()) return b;
    std::uint32_t cp = b & (0xFFu >> (n + 1));
    for (int i = 1; i < n; ++i) {
        const auto c = static_cast<unsigned char>(s[off + static_cast<std::size_t>(i)]);
        if ((c & 0xC0) != 0x80) return b;
        cp = (cp << 6) | (c & 0x3F);
    }
    len = static_cast<std::size_t>(n);
    return cp;
}

bool is_extend(std::uint32_t cp) {
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F) ||
           cp == 0x200C || cp == 0x200D;
}
bool is_regional(std::uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }

bool is_wide(std::uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
           (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

int word_class(char c) {
    const auto u = static_cast<unsigned char>(c);
    if (c == ' ' || c == '\t') return 0;
    if (std::isalnum(u) != 0 || c == '_' || u >= 0x80) return 1;
    return 2;
}

TextPos after_text(TextPos p, std::string_view text) {
    for (char c : text) {
        if (c == '\n') { ++p.line; p.col = 0; }
        else ++p.col;
    }
    return p;
}

// Position `p` after an edit that replaced a range ending at `e` and ended at `n`.
TextPos shift(TextPos p, TextPos e, TextPos n) {
    if (p.line == e.line) return {n.line, n.col + (p.col - e.col)};
    return {p.line + (n.line - e.line), p.col};
}

std::string leading_ws(const std::string& l) {
    std::size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    return l.substr(0, i);
}

} // namespace

std::size_t next_boundary(std::string_view s, std::size_t off) {
    if (off >= s.size()) return s.size();
    std::size_t len = 0;
    std::uint32_t cp = decode(s, off, len);
    std::size_t p = off + len;
    if (cp == '\r' && p < s.size() && s[p] == '\n') return p + 1;
    bool ri = is_regional(cp);
    while (p < s.size()) {
        std::size_t l2 = 0;
        const std::uint32_t n = decode(s, p, l2);
        if (cp == 0x200D && n >= 0x80) { cp = n; p += l2; continue; } // ZWJ joins the next symbol
        if (is_extend(n)) { cp = n; p += l2; continue; }
        if (ri && is_regional(n)) { ri = false; cp = n; p += l2; continue; }
        break;
    }
    return p;
}

std::size_t prev_boundary(std::string_view s, std::size_t off) {
    if (off == 0) return 0;
    std::size_t prev = 0, cur = 0;
    while (cur < off) { prev = cur; cur = next_boundary(s, cur); }
    return prev;
}

int grapheme_cells(std::string_view cluster) {
    if (cluster.empty()) return 0;
    std::size_t len = 0;
    const std::uint32_t cp = decode(cluster, 0, len);
    if (is_wide(cp) || is_regional(cp)) return 2;
    if (cp >= 0x1F000 && cp <= 0x1FAFF && cluster.size() > len) {
        std::size_t l2 = 0;
        if (decode(cluster, len, l2) == 0xFE0F) return 2;
    }
    return 1;
}

int display_col(std::string_view line, int col, int tab) {
    int d = 0;
    std::size_t i = 0;
    const auto end = static_cast<std::size_t>(std::max(col, 0));
    while (i < end && i < line.size()) {
        const std::size_t n = next_boundary(line, i);
        if (line[i] == '\t') d += tab - d % std::max(tab, 1);
        else d += grapheme_cells(line.substr(i, n - i));
        i = n;
    }
    return d;
}

int col_at_display(std::string_view line, int dcol, int tab) {
    int d = 0;
    std::size_t i = 0;
    while (i < line.size()) {
        const std::size_t n = next_boundary(line, i);
        const int w = line[i] == '\t' ? tab - d % std::max(tab, 1) : grapheme_cells(line.substr(i, n - i));
        if (dcol < d + w) return static_cast<int>(i);
        d += w;
        i = n;
    }
    return static_cast<int>(line.size());
}

int utf16_len(std::string_view s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size();) {
        std::size_t len = 0;
        const std::uint32_t cp = decode(s, i, len);
        n += cp >= 0x10000 ? 2 : 1;
        i += len;
    }
    return n;
}

// ------------------------------------------------------------------ text

std::string TextBuffer::normalize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else out += s[i];
    }
    return out;
}

void TextBuffer::set_text(std::string_view text) {
    bom_ = text.starts_with("\xEF\xBB\xBF");
    if (bom_) text.remove_prefix(3);
    int crlf = 0, lf = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') (i > 0 && text[i - 1] == '\r') ? ++crlf : ++lf;
    }
    eol_ = crlf > lf ? LineEnding::CrLf : LineEnding::Lf;
    const std::string norm = normalize(text);
    const int old_count = line_count();
    lines_.clear();
    std::size_t s = 0;
    for (;;) {
        const std::size_t e = norm.find('\n', s);
        if (e == std::string::npos) { lines_.push_back(norm.substr(s)); break; }
        lines_.push_back(norm.substr(s, e - s));
        s = e + 1;
    }
    // Indent: tabs when most indented lines start with one, else the most common step between indent levels.
    int tab_lines = 0, space_lines = 0, prev = 0;
    std::map<int, int> steps;
    for (const std::string& l : lines_) {
        if (l.empty()) continue;
        if (l[0] == '\t') ++tab_lines;
        else if (l[0] == ' ') {
            ++space_lines;
            int n = 0;
            while (n < static_cast<int>(l.size()) && l[static_cast<std::size_t>(n)] == ' ') ++n;
            if (n > prev && n - prev <= 8) ++steps[n - prev];
            prev = n;
        } else prev = 0;
    }
    indent_ = {};
    if (tab_lines > space_lines) indent_.tabs = true;
    else if (!steps.empty()) {
        int best = 4, count = 0;
        for (auto [w, c] : steps) if (c > count) { best = w; count = c; }
        indent_.width = best;
    }
    sels_ = {{}};
    undo_.clear();
    redo_.clear();
    changes_.push_back({0, old_count, line_count()});
    ++revision_;
    state_id_ = saved_id_ = 0;
    next_id_ = 1;
    sealed_ = true;
    open_ = false;
    goal_col_ = -1;
}

std::string TextBuffer::text() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i > 0) out += '\n';
        out += lines_[i];
    }
    return out;
}

std::string TextBuffer::text_for_save() const {
    std::string out = bom_ ? "\xEF\xBB\xBF" : "";
    const char* nl = eol_ == LineEnding::CrLf ? "\r\n" : "\n";
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i > 0) out += nl;
        out += lines_[i];
    }
    return out;
}

TextPos TextBuffer::clamp(TextPos p) const {
    p.line = std::clamp(p.line, 0, line_count() - 1);
    const std::string& l = lines_[static_cast<std::size_t>(p.line)];
    p.col = std::clamp(p.col, 0, static_cast<int>(l.size()));
    if (p.col > 0 && p.col < static_cast<int>(l.size())) {
        const std::size_t b = prev_boundary(l, static_cast<std::size_t>(p.col) + 1);
        if (static_cast<int>(b) != p.col) p.col = static_cast<int>(b);
    }
    return p;
}

std::size_t TextBuffer::offset_of(TextPos p) const {
    p.line = std::clamp(p.line, 0, line_count() - 1);
    std::size_t off = 0;
    for (int i = 0; i < p.line; ++i) off += lines_[static_cast<std::size_t>(i)].size() + 1;
    return off + static_cast<std::size_t>(std::clamp(p.col, 0, static_cast<int>(lines_[static_cast<std::size_t>(p.line)].size())));
}

TextPos TextBuffer::pos_of(std::size_t offset) const {
    for (int i = 0; i < line_count(); ++i) {
        const std::size_t n = lines_[static_cast<std::size_t>(i)].size();
        if (offset <= n) return {i, static_cast<int>(offset)};
        offset -= n + 1;
    }
    return end_pos();
}

std::string TextBuffer::text_in(TextRange r) const {
    r.start = clamp(r.start);
    r.end = clamp(r.end);
    if (r.end < r.start) std::swap(r.start, r.end);
    if (r.start.line == r.end.line) return lines_[static_cast<std::size_t>(r.start.line)].substr(static_cast<std::size_t>(r.start.col), static_cast<std::size_t>(r.end.col - r.start.col));
    std::string out = lines_[static_cast<std::size_t>(r.start.line)].substr(static_cast<std::size_t>(r.start.col));
    for (int i = r.start.line + 1; i < r.end.line; ++i) out += '\n' + lines_[static_cast<std::size_t>(i)];
    out += '\n' + lines_[static_cast<std::size_t>(r.end.line)].substr(0, static_cast<std::size_t>(r.end.col));
    return out;
}

int TextBuffer::to_utf16(TextPos p) const {
    p = clamp(p);
    const std::string& l = lines_[static_cast<std::size_t>(p.line)];
    return utf16_len(std::string_view(l).substr(0, static_cast<std::size_t>(p.col)));
}

TextPos TextBuffer::from_utf16(int line, int character) const {
    line = std::clamp(line, 0, line_count() - 1);
    const std::string& l = lines_[static_cast<std::size_t>(line)];
    int units = 0;
    std::size_t i = 0;
    while (i < l.size() && units < character) {
        std::size_t len = 0;
        const std::uint32_t cp = decode(l, i, len);
        units += cp >= 0x10000 ? 2 : 1;
        i += len;
    }
    return {line, static_cast<int>(i)};
}

TextRange TextBuffer::word_at(TextPos p) const {
    p = clamp(p);
    const std::string& l = lines_[static_cast<std::size_t>(p.line)];
    auto is_w = [&](std::size_t i) { return i < l.size() && word_class(l[i]) == 1; };
    std::size_t a = static_cast<std::size_t>(p.col), b = a;
    if (!is_w(a) && !(a > 0 && is_w(a - 1))) return {p, p};
    while (a > 0 && is_w(a - 1)) --a;
    while (is_w(b)) ++b;
    return {{p.line, static_cast<int>(a)}, {p.line, static_cast<int>(b)}};
}

// ------------------------------------------------------------------ selection and movement

void TextBuffer::normalize_selections() {
    for (Selection& s : sels_) { s.anchor = clamp(s.anchor); s.head = clamp(s.head); }
    std::stable_sort(sels_.begin(), sels_.end(), [](const Selection& a, const Selection& b) { return a.start() < b.start(); });
    std::vector<Selection> out;
    for (const Selection& s : sels_) {
        if (!out.empty() && s.start() <= out.back().end()) {
            Selection& o = out.back();
            const TextPos e = o.end() < s.end() ? s.end() : o.end();
            o = (o.head < o.anchor) ? Selection{e, o.start()} : Selection{o.start(), e};
        } else out.push_back(s);
    }
    sels_ = out.empty() ? std::vector<Selection>{{}} : out;
}

void TextBuffer::set_selections(std::vector<Selection> s) {
    sels_ = std::move(s);
    normalize_selections();
    goal_col_ = -1;
    seal();
}

void TextBuffer::set_caret(TextPos p, bool extend) {
    p = clamp(p);
    if (extend) sels_ = {{sels_.back().anchor, p}};
    else sels_ = {{p, p}};
    goal_col_ = -1;
    seal();
}

void TextBuffer::add_caret(TextPos p) {
    p = clamp(p);
    sels_.push_back({p, p});
    normalize_selections();
    seal();
}

void TextBuffer::select_all() { set_selections({{{0, 0}, end_pos()}}); }

bool TextBuffer::has_selection() const {
    return std::any_of(sels_.begin(), sels_.end(), [](const Selection& s) { return !s.empty(); });
}

std::string TextBuffer::selected_text() const {
    std::string out;
    for (const Selection& s : sels_) {
        if (s.empty()) continue;
        if (!out.empty()) out += '\n';
        out += text_in({s.start(), s.end()});
    }
    return out;
}

void TextBuffer::move(Move m, bool extend, int page_lines) {
    const int tab = indent_.width;
    const bool vertical = m == Move::Up || m == Move::Down || m == Move::PageUp || m == Move::PageDown;
    for (Selection& s : sels_) {
        TextPos p = s.head;
        const std::string& l = lines_[static_cast<std::size_t>(p.line)];
        // Without extending, Left/Right collapse a selection to its edge.
        if (!extend && !s.empty() && (m == Move::Left || m == Move::Right)) {
            p = m == Move::Left ? s.start() : s.end();
            s = {p, p};
            continue;
        }
        switch (m) {
        case Move::Left:
            if (p.col > 0) p.col = static_cast<int>(prev_boundary(l, static_cast<std::size_t>(p.col)));
            else if (p.line > 0) p = {p.line - 1, static_cast<int>(lines_[static_cast<std::size_t>(p.line - 1)].size())};
            break;
        case Move::Right:
            if (p.col < static_cast<int>(l.size())) p.col = static_cast<int>(next_boundary(l, static_cast<std::size_t>(p.col)));
            else if (p.line + 1 < line_count()) p = {p.line + 1, 0};
            break;
        case Move::WordLeft: {
            if (p.col == 0) { if (p.line > 0) p = {p.line - 1, static_cast<int>(lines_[static_cast<std::size_t>(p.line - 1)].size())}; break; }
            std::size_t i = static_cast<std::size_t>(p.col);
            while (i > 0 && word_class(l[i - 1]) == 0) --i;
            if (i > 0) { const int c = word_class(l[i - 1]); while (i > 0 && word_class(l[i - 1]) == c) --i; }
            p.col = static_cast<int>(i);
            break;
        }
        case Move::WordRight: {
            if (p.col >= static_cast<int>(l.size())) { if (p.line + 1 < line_count()) p = {p.line + 1, 0}; break; }
            std::size_t i = static_cast<std::size_t>(p.col);
            while (i < l.size() && word_class(l[i]) == 0) ++i;
            if (i < l.size()) { const int c = word_class(l[i]); while (i < l.size() && word_class(l[i]) == c) ++i; }
            p.col = static_cast<int>(i);
            break;
        }
        case Move::LineStart: p.col = 0; break;
        case Move::LineEnd: p.col = static_cast<int>(l.size()); break;
        case Move::SmartHome: {
            const int first = static_cast<int>(leading_ws(l).size());
            p.col = p.col == first ? 0 : first;
            break;
        }
        case Move::DocStart: p = {0, 0}; break;
        case Move::DocEnd: p = end_pos(); break;
        case Move::Up: case Move::Down: case Move::PageUp: case Move::PageDown: {
            const int step = (m == Move::Up || m == Move::PageUp) ? -1 : 1;
            const int n = (m == Move::PageUp || m == Move::PageDown) ? std::max(page_lines, 1) : 1;
            const int want = goal_col_ >= 0 ? goal_col_ : display_col(l, p.col, tab);
            const int target = p.line + step * n;
            if (target < 0) p = {0, 0};
            else if (target >= line_count()) p = end_pos();
            else p = {target, col_at_display(lines_[static_cast<std::size_t>(target)], want, tab)};
            goal_col_ = want;
            break;
        }
        }
        s.head = p;
        if (!extend) s.anchor = p;
    }
    if (!vertical) goal_col_ = -1;
    const int keep = goal_col_;
    normalize_selections();
    goal_col_ = keep;
    seal();
}

// ------------------------------------------------------------------ edits

TextPos TextBuffer::apply(TextRange r, std::string_view text, std::string* removed) {
    r.start = clamp(r.start);
    r.end = clamp(r.end);
    if (r.end < r.start) std::swap(r.start, r.end);
    if (removed != nullptr) *removed = text_in(r);
    const std::string prefix = lines_[static_cast<std::size_t>(r.start.line)].substr(0, static_cast<std::size_t>(r.start.col));
    const std::string suffix = lines_[static_cast<std::size_t>(r.end.line)].substr(static_cast<std::size_t>(r.end.col));
    std::vector<std::string> parts;
    std::size_t s = 0;
    for (;;) {
        const std::size_t e = text.find('\n', s);
        if (e == std::string_view::npos) { parts.emplace_back(text.substr(s)); break; }
        parts.emplace_back(text.substr(s, e - s));
        s = e + 1;
    }
    const std::size_t last_len = parts.back().size();
    parts.front().insert(0, prefix);
    parts.back() += suffix;
    const int removed_lines = r.end.line - r.start.line + 1;
    const auto first = lines_.begin() + r.start.line;
    lines_.erase(first, first + removed_lines);
    lines_.insert(lines_.begin() + r.start.line, parts.begin(), parts.end());
    changes_.push_back({r.start.line, removed_lines, static_cast<int>(parts.size())});
    ++revision_;
    const int end_line = r.start.line + static_cast<int>(parts.size()) - 1;
    return {end_line, (parts.size() == 1 ? static_cast<int>(prefix.size()) : 0) + static_cast<int>(last_len)};
}

void TextBuffer::begin(bool typed, std::uint64_t now_ms, TextPos probe) {
    open_ = true;
    goal_col_ = -1;
    if (typed && !sealed_ && !undo_.empty() && redo_.empty() && undo_.back().typed && now_ms >= undo_.back().last_ms && now_ms - undo_.back().last_ms <= 1000 &&
        sels_.size() == 1 && sels_[0].empty() && sels_[0].head == probe) {
        undo_.back().last_ms = now_ms;
        return;
    }
    Group g;
    g.before = sels_;
    g.id = next_id_++;
    g.typed = typed;
    g.last_ms = now_ms;
    undo_.push_back(std::move(g));
    redo_.clear();
}

void TextBuffer::record(TextPos at, std::string removed, std::string inserted) { undo_.back().edits.push_back({at, std::move(removed), std::move(inserted)}); }

void TextBuffer::finish() {
    Group& g = undo_.back();
    g.after = sels_;
    state_id_ = g.id;
    sealed_ = !g.typed;
    open_ = false;
}

std::vector<TextPos> TextBuffer::run(std::vector<Planned>& plan) {
    std::vector<TextPos> carets;
    for (std::size_t k = 0; k < plan.size(); ++k) {
        Planned& pe = plan[k];
        std::string removed;
        const TextPos start = clamp(pe.r.start), old_end = clamp(pe.r.end);
        const TextPos n = apply({start, old_end}, pe.text, &removed);
        record(start, removed, pe.text);
        for (std::size_t j = k + 1; j < plan.size(); ++j) {
            plan[j].r.start = shift(plan[j].r.start, old_end, n);
            plan[j].r.end = shift(plan[j].r.end, old_end, n);
        }
        carets.push_back(pe.caret < 0 ? n : after_text(start, std::string_view(pe.text).substr(0, static_cast<std::size_t>(pe.caret))));
    }
    return carets;
}

void TextBuffer::insert(std::string_view text, bool typed, std::uint64_t now_ms) {
    const std::string norm = normalize(text);
    if (norm.empty() && !has_selection()) return;
    typed = typed && norm.find('\n') == std::string::npos;
    normalize_selections();
    begin(typed, now_ms, sels_.back().head);
    std::vector<Planned> plan;
    for (const Selection& s : sels_) plan.push_back({{s.start(), s.end()}, norm, -1});
    const std::vector<TextPos> carets = run(plan);
    sels_.clear();
    for (TextPos c : carets) sels_.push_back({c, c});
    normalize_selections();
    finish();
}

namespace {

char closer_of(char c) { return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c == '"' ? '"' : '\0'; }
bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || (static_cast<unsigned char>(c) & 0x80) != 0; }

// Which bytes of a line are code (not inside a string or a // comment).
std::vector<bool> code_mask(const std::string& l) {
    std::vector<bool> m(l.size(), true);
    bool str = false;
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (str) {
            m[i] = false;
            if (l[i] == '\\' && i + 1 < l.size()) m[++i] = false;
            else if (l[i] == '"') str = false;
        } else if (l[i] == '"') { str = true; m[i] = false; }
        else if (l[i] == '/' && i + 1 < l.size() && l[i + 1] == '/') { for (std::size_t j = i; j < l.size(); ++j) m[j] = false; break; }
    }
    return m;
}

} // namespace

bool TextBuffer::type(std::string_view text, std::uint64_t now_ms) {
    if (text.size() != 1) { insert(text, true, now_ms); return false; }
    const char c = text[0];
    const char close = closer_of(c);
    const bool is_close = c == ')' || c == ']' || c == '}' || c == '"';
    normalize_selections();
    const auto char_at = [&](TextPos p, int d) -> char {
        const std::string& l = lines_[static_cast<std::size_t>(p.line)];
        const int i = p.col + d;
        return i >= 0 && i < static_cast<int>(l.size()) ? l[static_cast<std::size_t>(i)] : '\0';
    };
    if (is_close && !has_selection()) {
        bool all = true;
        for (const Selection& s : sels_) all = all && char_at(s.head, 0) == c;
        if (all) {
            std::vector<Selection> next;
            for (const Selection& s : sels_) { TextPos p{s.head.line, s.head.col + 1}; next.push_back({p, p}); }
            set_selections(std::move(next));
            return true;
        }
    }
    if (close == '\0') { insert(text, true, now_ms); return false; }
    bool wrap = true, pair = true;
    for (const Selection& s : sels_) {
        wrap = wrap && !s.empty();
        const char nx = char_at(s.head, 0), pv = char_at(s.start(), -1);
        const bool next_ok = nx == '\0' || nx == ' ' || nx == '\t' || nx == ')' || nx == ']' || nx == '}' || nx == ',' || nx == ';';
        pair = pair && s.empty() && next_ok && (c != '"' || (!word_char(pv) && pv != '"' && pv != '\\'));
    }
    if (!wrap && !pair) { insert(text, true, now_ms); return false; }
    begin(false, 0, {});
    std::vector<Planned> plan;
    for (const Selection& s : sels_) {
        if (wrap) plan.push_back({{s.start(), s.end()}, std::string(1, c) + text_in({s.start(), s.end()}) + std::string(1, close), -1});
        else plan.push_back({{s.start(), s.end()}, std::string(1, c) + std::string(1, close), 1});
    }
    const std::vector<TextPos> carets = run(plan);
    sels_.clear();
    for (TextPos p : carets) sels_.push_back({p, p});
    normalize_selections();
    finish();
    return true;
}

bool TextBuffer::match_bracket(TextPos caret, TextPos& open, TextPos& close) const {
    caret = clamp(caret);
    const auto bracket_at = [&](TextPos p, char& ch) {
        const std::string& l = lines_[static_cast<std::size_t>(p.line)];
        if (p.col < 0 || p.col >= static_cast<int>(l.size())) return false;
        ch = l[static_cast<std::size_t>(p.col)];
        if (ch != '(' && ch != ')' && ch != '[' && ch != ']' && ch != '{' && ch != '}') return false;
        return static_cast<bool>(code_mask(l)[static_cast<std::size_t>(p.col)]);
    };
    TextPos at = caret;
    char ch = 0;
    if (!bracket_at(at, ch)) { at = {caret.line, caret.col - 1}; if (!bracket_at(at, ch)) return false; }
    const bool fwd = ch == '(' || ch == '[' || ch == '{';
    const char other = ch == '(' ? ')' : ch == '[' ? ']' : ch == '{' ? '}' : ch == ')' ? '(' : ch == ']' ? '[' : '{';
    int depth = 0;
    constexpr int kMaxLines = 4000;
    for (int l = at.line; fwd ? (l < line_count() && l - at.line < kMaxLines) : (l >= 0 && at.line - l < kMaxLines); l += fwd ? 1 : -1) {
        const std::string& s = lines_[static_cast<std::size_t>(l)];
        const std::vector<bool> m = code_mask(s);
        int i = fwd ? (l == at.line ? at.col : 0) : (l == at.line ? at.col : static_cast<int>(s.size()) - 1);
        for (; i >= 0 && i < static_cast<int>(s.size()); i += fwd ? 1 : -1) {
            if (!m[static_cast<std::size_t>(i)]) continue;
            if (s[static_cast<std::size_t>(i)] == ch) ++depth;
            else if (s[static_cast<std::size_t>(i)] == other && --depth == 0) {
                open = fwd ? at : TextPos{l, i};
                close = fwd ? TextPos{l, i} : at;
                return true;
            }
        }
    }
    return false;
}

void TextBuffer::insert_newline() {
    normalize_selections();
    begin(false, 0, {});
    const std::string unit = indent_.tabs ? "\t" : std::string(static_cast<std::size_t>(indent_.width), ' ');
    std::vector<Planned> plan;
    for (const Selection& s : sels_) {
        const std::string& l = lines_[static_cast<std::size_t>(s.start().line)];
        std::string base = leading_ws(l);
        base.resize(std::min(base.size(), static_cast<std::size_t>(s.start().col)));
        const std::string before = l.substr(0, static_cast<std::size_t>(s.start().col));
        const std::size_t b = before.find_last_not_of(" \t");
        const char oc = b != std::string::npos ? before[b] : '\0';
        const bool open = oc == '{' || oc == '(' || oc == '[';
        const std::string& el = lines_[static_cast<std::size_t>(s.end().line)];
        const bool close = s.end().col < static_cast<int>(el.size()) && el[static_cast<std::size_t>(s.end().col)] == closer_of(oc);
        Planned p{{s.start(), s.end()}, "\n" + base, -1};
        if (open) p.text += unit;
        if (open && close) { p.caret = static_cast<int>(p.text.size()); p.text += "\n" + base; }
        plan.push_back(std::move(p));
    }
    const std::vector<TextPos> carets = run(plan);
    sels_.clear();
    for (TextPos c : carets) sels_.push_back({c, c});
    normalize_selections();
    finish();
}

void TextBuffer::backspace() {
    normalize_selections();
    begin(false, 0, {});
    std::vector<Planned> plan;
    for (const Selection& s : sels_) {
        TextPos a = s.start();
        if (s.empty()) {
            const std::string& l = lines_[static_cast<std::size_t>(a.line)];
            const bool in_indent = !indent_.tabs && a.col >= indent_.width && l.find_first_not_of(' ') >= static_cast<std::size_t>(a.col);
            if (a.col == 0) {
                if (a.line == 0) continue;
                a = {a.line - 1, static_cast<int>(lines_[static_cast<std::size_t>(a.line - 1)].size())};
            } else if (in_indent) a.col -= a.col % indent_.width == 0 ? indent_.width : a.col % indent_.width;
            else a.col = static_cast<int>(prev_boundary(l, static_cast<std::size_t>(a.col)));
        }
        TextPos e = s.end();
        if (s.empty() && s.head.col > 0) {
            // Deleting an opener right before its closer removes both: ( ), [ ], { }, " ".
            const std::string& l = lines_[static_cast<std::size_t>(s.head.line)];
            const std::size_t i = static_cast<std::size_t>(s.head.col);
            if (i < l.size() && closer_of(l[i - 1]) == l[i]) e.col += 1;
        }
        plan.push_back({{a, e}, {}, -1});
    }
    if (plan.empty()) { undo_.pop_back(); open_ = false; return; }
    const std::vector<TextPos> carets = run(plan);
    sels_.clear();
    for (TextPos c : carets) sels_.push_back({c, c});
    normalize_selections();
    finish();
}

void TextBuffer::delete_forward() {
    normalize_selections();
    begin(false, 0, {});
    std::vector<Planned> plan;
    for (const Selection& s : sels_) {
        TextPos b = s.end();
        if (s.empty()) {
            const std::string& l = lines_[static_cast<std::size_t>(b.line)];
            if (b.col >= static_cast<int>(l.size())) { if (b.line + 1 >= line_count()) continue; b = {b.line + 1, 0}; }
            else b.col = static_cast<int>(next_boundary(l, static_cast<std::size_t>(b.col)));
        }
        plan.push_back({{s.start(), b}, {}, -1});
    }
    if (plan.empty()) { undo_.pop_back(); open_ = false; return; }
    const std::vector<TextPos> carets = run(plan);
    sels_.clear();
    for (TextPos c : carets) sels_.push_back({c, c});
    normalize_selections();
    finish();
}

void TextBuffer::delete_word_back() {
    if (!has_selection()) move(Move::WordLeft, true);
    backspace();
}
void TextBuffer::delete_word_forward() {
    if (!has_selection()) move(Move::WordRight, true);
    delete_forward();
}

void TextBuffer::delete_line() {
    normalize_selections();
    begin(false, 0, {});
    const int a = sels_.front().start().line, b = sels_.back().end().line;
    TextRange r;
    if (b + 1 < line_count()) r = {{a, 0}, {b + 1, 0}};
    else if (a > 0) r = {{a - 1, static_cast<int>(lines_[static_cast<std::size_t>(a - 1)].size())}, end_pos()};
    else r = {{0, 0}, end_pos()};
    std::string removed;
    const TextPos n = apply(r, {}, &removed);
    record(r.start, removed, {});
    const TextPos c = clamp({std::min(n.line, line_count() - 1), 0});
    sels_ = {{c, c}};
    finish();
}

void TextBuffer::duplicate_line() {
    normalize_selections();
    begin(false, 0, {});
    const Selection s = sels_.back();
    if (s.empty()) {
        const int ln = s.head.line;
        const TextPos at{ln, static_cast<int>(lines_[static_cast<std::size_t>(ln)].size())};
        const std::string add = "\n" + lines_[static_cast<std::size_t>(ln)];
        apply({at, at}, add, nullptr);
        record(at, {}, add);
        sels_ = {{{ln + 1, s.head.col}, {ln + 1, s.head.col}}};
    } else {
        const std::string add = text_in({s.start(), s.end()});
        const TextPos e = s.end();
        const TextPos n = apply({e, e}, add, nullptr);
        record(e, {}, add);
        sels_ = {{e, n}};
    }
    finish();
}

void TextBuffer::indent_lines(bool outdent) {
    normalize_selections();
    const std::string unit = indent_.tabs ? "\t" : std::string(static_cast<std::size_t>(indent_.width), ' ');
    const int a = sels_.front().start().line, b = sels_.back().end().line;
    begin(false, 0, {});
    std::vector<int> delta(static_cast<std::size_t>(b - a + 1), 0);
    for (int ln = a; ln <= b; ++ln) {
        const std::string& l = lines_[static_cast<std::size_t>(ln)];
        const auto k = static_cast<std::size_t>(ln - a);
        if (outdent) {
            std::size_t n = 0;
            if (!l.empty() && l[0] == '\t') n = 1;
            else while (n < l.size() && n < static_cast<std::size_t>(indent_.width) && l[n] == ' ') ++n;
            if (n == 0) continue;
            std::string removed;
            apply({{ln, 0}, {ln, static_cast<int>(n)}}, {}, &removed);
            record({ln, 0}, removed, {});
            delta[k] = -static_cast<int>(n);
        } else {
            if (l.empty() && a != b) continue;
            apply({{ln, 0}, {ln, 0}}, unit, nullptr);
            record({ln, 0}, {}, unit);
            delta[k] = static_cast<int>(unit.size());
        }
    }
    if (undo_.back().edits.empty()) { undo_.pop_back(); open_ = false; return; }
    auto adj = [&](TextPos p) {
        if (p.line >= a && p.line <= b) p.col = std::max(0, p.col + delta[static_cast<std::size_t>(p.line - a)]);
        return p;
    };
    for (Selection& s : sels_) { s.anchor = adj(s.anchor); s.head = adj(s.head); }
    finish();
}

void TextBuffer::toggle_comment() {
    normalize_selections();
    const int a = sels_.front().start().line, b = sels_.back().end().line;
    bool all = true, any = false;
    std::size_t min_ind = std::string::npos;
    for (int ln = a; ln <= b; ++ln) {
        const std::string& l = lines_[static_cast<std::size_t>(ln)];
        const std::size_t i = l.find_first_not_of(" \t");
        if (i == std::string::npos) continue;
        any = true;
        min_ind = std::min(min_ind, i);
        if (l.compare(i, 2, "//") != 0) all = false;
    }
    if (!any) return;
    begin(false, 0, {});
    std::vector<int> delta(static_cast<std::size_t>(b - a + 1), 0), at(static_cast<std::size_t>(b - a + 1), 0);
    for (int ln = a; ln <= b; ++ln) {
        const std::string& l = lines_[static_cast<std::size_t>(ln)];
        const std::size_t i = l.find_first_not_of(" \t");
        if (i == std::string::npos) continue;
        const auto k = static_cast<std::size_t>(ln - a);
        if (all) {
            const std::size_t n = l.compare(i, 3, "// ") == 0 ? 3 : 2;
            std::string removed;
            apply({{ln, static_cast<int>(i)}, {ln, static_cast<int>(i + n)}}, {}, &removed);
            record({ln, static_cast<int>(i)}, removed, {});
            delta[k] = -static_cast<int>(n);
            at[k] = static_cast<int>(i);
        } else {
            apply({{ln, static_cast<int>(min_ind)}, {ln, static_cast<int>(min_ind)}}, "// ", nullptr);
            record({ln, static_cast<int>(min_ind)}, {}, "// ");
            delta[k] = 3;
            at[k] = static_cast<int>(min_ind);
        }
    }
    auto adj = [&](TextPos p) {
        if (p.line >= a && p.line <= b) {
            const auto k = static_cast<std::size_t>(p.line - a);
            if (p.col >= at[k]) p.col = std::max(at[k], p.col + delta[k]);
        }
        return p;
    };
    for (Selection& s : sels_) { s.anchor = adj(s.anchor); s.head = adj(s.head); }
    finish();
}

void TextBuffer::move_lines(int dir) {
    normalize_selections();
    const int a = sels_.front().start().line, b = sels_.back().end().line;
    if ((dir < 0 && a == 0) || (dir > 0 && b + 1 >= line_count())) return;
    begin(false, 0, {});
    const int lo = dir < 0 ? a - 1 : a, hi = dir < 0 ? b : b + 1;
    const TextRange r{{lo, 0}, {hi, static_cast<int>(lines_[static_cast<std::size_t>(hi)].size())}};
    std::string block;
    for (int i = a; i <= b; ++i) block += (i > a ? "\n" : "") + lines_[static_cast<std::size_t>(i)];
    const std::string other = lines_[static_cast<std::size_t>(dir < 0 ? a - 1 : b + 1)];
    const std::string text = dir < 0 ? block + "\n" + other : other + "\n" + block;
    std::string removed;
    apply(r, text, &removed);
    record(r.start, removed, text);
    for (Selection& s : sels_) { s.anchor.line += dir; s.head.line += dir; }
    finish();
}

void TextBuffer::replace(TextRange r, std::string_view text) {
    const std::string norm = normalize(text);
    begin(false, 0, {});
    const TextPos a = clamp(r.start), b = clamp(r.end);
    const TextPos s = b < a ? b : a, e = b < a ? a : b;
    std::string removed;
    const TextPos n = apply({s, e}, norm, &removed);
    record(s, removed, norm);
    sels_ = {{n, n}};
    finish();
}

void TextBuffer::replace_all(std::vector<std::pair<TextRange, std::string>> edits) {
    if (edits.empty()) return;
    std::stable_sort(edits.begin(), edits.end(), [](const auto& x, const auto& y) { return x.first.start < y.first.start; });
    begin(false, 0, {});
    std::vector<Planned> plan;
    for (auto& [r, t] : edits) plan.push_back({r, normalize(t), -1});
    const std::vector<TextPos> carets = run(plan);
    sels_ = {{carets.back(), carets.back()}};
    finish();
}

// ------------------------------------------------------------------ undo

bool TextBuffer::undo() {
    if (undo_.empty()) return false;
    Group g = std::move(undo_.back());
    undo_.pop_back();
    for (auto it = g.edits.rbegin(); it != g.edits.rend(); ++it) apply({it->at, after_text(it->at, it->inserted)}, it->removed, nullptr);
    sels_ = g.before;
    normalize_selections();
    state_id_ = undo_.empty() ? 0 : undo_.back().id;
    redo_.push_back(std::move(g));
    sealed_ = true;
    goal_col_ = -1;
    return true;
}

bool TextBuffer::redo() {
    if (redo_.empty()) return false;
    Group g = std::move(redo_.back());
    redo_.pop_back();
    for (const Edit& e : g.edits) apply({e.at, after_text(e.at, e.removed)}, e.inserted, nullptr);
    sels_ = g.after;
    normalize_selections();
    state_id_ = g.id;
    undo_.push_back(std::move(g));
    sealed_ = true;
    goal_col_ = -1;
    return true;
}

void TextBuffer::mark_saved() {
    saved_id_ = state_id_;
    sealed_ = true;
}

} // namespace cuervo::editor

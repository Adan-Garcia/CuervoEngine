#include "editor/script_syntax.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "pluma/lexer.hpp"
#include "pluma/token.hpp"

namespace cuervo::editor {

namespace {

Syn carry_of(const pluma::LineState& st) {
    switch (st.mode) {
    case pluma::LineState::Mode::BlockComment: return Syn::Comment;
    case pluma::LineState::Mode::MultiString: return Syn::String;
    case pluma::LineState::Mode::Normal: break;
    }
    return Syn::Plain;
}

// The single seam to pluma's lexer: Lexer::tokenize_line carries only the multi-line state from one
// line to the next, so re-lexing can start at any line and stop at the first one whose state matches.
void lex_line(std::string_view text, pluma::LineState in, LineSyn& out) {
    const pluma::LineResult r = pluma::Lexer::tokenize_line(text, in);
    out.spans.clear();
    out.carry = carry_of(in);
    out.in = in;
    out.out = r.state;
    std::uint32_t at_end = ~0u; // end column of a directly preceding `@`
    for (const pluma::LineToken& t : r.tokens) {
        if (t.length == 0 || t.column + t.length > text.size()) continue;
        Syn k = Syn::Plain;
        if (t.comment) k = Syn::Comment;
        else switch (t.kind) {
        case pluma::TokenKind::Keyword: k = Syn::Keyword; break;
        case pluma::TokenKind::Identifier:
            if (at_end == t.column) k = Syn::Keyword; // annotation name
            else if (std::isupper(static_cast<unsigned char>(text[t.column])) != 0) k = Syn::Type;
            break;
        case pluma::TokenKind::At: k = Syn::Keyword; break;
        case pluma::TokenKind::Int: case pluma::TokenKind::Float: k = Syn::Number; break;
        case pluma::TokenKind::String: k = Syn::String; break;
        case pluma::TokenKind::Error: k = Syn::Error; break;
        default: break;
        }
        at_end = t.kind == pluma::TokenKind::At && !t.comment ? t.column + t.length : ~0u;
        if (k != Syn::Plain) out.spans.push_back({t.column, t.length, k});
    }
    out.lex = out.spans;
}

} // namespace

void SyntaxCache::update(TextBuffer& buf) {
    last_lexed_ = 0;
    if (lines_.empty()) lines_.resize(1);
    for (const LineChange& c : buf.take_changes()) {
        if (on_change) on_change(c);
        const auto first = lines_.begin() + std::min<std::ptrdiff_t>(c.first, static_cast<std::ptrdiff_t>(lines_.size()));
        const auto last = first + std::min<std::ptrdiff_t>(c.removed, lines_.end() - first);
        const auto at = lines_.erase(first, last);
        lines_.insert(at, static_cast<std::size_t>(c.added), LineSyn{});
    }
    const int n = buf.line_count();
    lines_.resize(static_cast<std::size_t>(n));
    int d0 = 0;
    while (d0 < n && !lines_[static_cast<std::size_t>(d0)].dirty) ++d0;
    if (d0 == n) return;
    pluma::LineState st = d0 > 0 ? lines_[static_cast<std::size_t>(d0 - 1)].out : pluma::LineState{};
    for (int i = d0; i < n; ++i) {
        LineSyn& ln = lines_[static_cast<std::size_t>(i)];
        // A clean line that starts in the state it had before keeps its cache (and everything after it).
        if (!ln.dirty && ln.in == st) break;
        std::vector<SynSpan> sem = std::move(ln.sem);
        const bool was_dirty = ln.dirty;
        lex_line(buf.line(i), st, ln);
        ln.sem.clear();
        if (!was_dirty) ln.sem = std::move(sem); // state-only change: the overlay still describes this text
        ln.dirty = false;
        ln.stamp = next_stamp_++;
        if (!ln.sem.empty()) merge(ln);
        st = ln.out;
        ++last_lexed_;
    }
}

void SyntaxCache::merge(LineSyn& ln) {
    // Semantic spans recolor only plain identifiers or types; keywords, strings and comments stay lexical.
    std::vector<SynSpan> out;
    out.reserve(ln.lex.size() + ln.sem.size());
    std::size_t j = 0;
    for (const SynSpan& l : ln.lex) {
        while (j < ln.sem.size() && ln.sem[j].col < l.col) out.push_back(ln.sem[j++]);
        if (j < ln.sem.size() && ln.sem[j].col == l.col) {
            out.push_back(l.kind == Syn::Type || l.kind == Syn::Plain ? ln.sem[j] : l);
            ++j;
        } else out.push_back(l);
    }
    while (j < ln.sem.size()) out.push_back(ln.sem[j++]);
    ln.spans = std::move(out);
}

void SyntaxCache::set_semantic(const std::vector<SemanticSpan>& toks) {
    std::vector<std::vector<SynSpan>> per(lines_.size());
    for (const SemanticSpan& t : toks)
        if (t.line < per.size()) per[t.line].push_back({t.col, t.len, t.kind});
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        LineSyn& ln = lines_[i];
        auto& v = per[i];
        if (ln.dirty || ln.sem.size() + v.size() == 0) continue;
        const bool same = v.size() == ln.sem.size() && std::equal(v.begin(), v.end(), ln.sem.begin(), [](const SynSpan& a, const SynSpan& b) { return a.col == b.col && a.len == b.len && a.kind == b.kind; });
        if (same) continue;
        ln.sem = std::move(v);
        ln.spans = ln.lex;
        if (!ln.sem.empty()) merge(ln);
        ln.stamp = next_stamp_++;
    }
}

} // namespace cuervo::editor

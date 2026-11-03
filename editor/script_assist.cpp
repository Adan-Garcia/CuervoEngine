#include "editor/script_assist.hpp"

#include <algorithm>
#include <cctype>

namespace cuervo::editor {

namespace {

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || (static_cast<unsigned char>(c) & 0x80) != 0; }

char letter_of(pluma::lsp::CompletionKind k) {
    using K = pluma::lsp::CompletionKind;
    switch (k) {
    case K::Method: case K::Function: case K::Constructor: return 'm';
    case K::Field: case K::Property: return 'f';
    case K::Variable: return 'v';
    case K::Class: case K::Struct: case K::Interface: case K::TypeParameter: case K::Module: return 'C';
    case K::Enum: case K::EnumMember: return 'e';
    case K::Keyword: return 'k';
    case K::Constant: return 'c';
    case K::Event: return 's';
    default: return 't';
    }
}

} // namespace

bool CompletionState::query(ScriptWorkspace& ws, ScriptDoc& d, int min_prefix) {
    ws.sync(d);
    const TextPos caret = d.buf.primary().head;
    const std::string& line = d.buf.line(caret.line);
    int s = caret.col;
    while (s > 0 && ident_char(line[static_cast<std::size_t>(s - 1)])) --s;
    const int plen = caret.col - s;
    const bool dot = s > 0 && line[static_cast<std::size_t>(s - 1)] == '.';
    if (plen < (dot ? 0 : min_prefix) || d.buf.has_selection()) { close(); return false; }
    d.syn.update(d.buf);
    // Inside a string or a comment there is nothing to complete.
    if (!d.syn.size() || caret.line >= d.syn.size()) { close(); return false; }
    for (const SynSpan& sp : d.syn.line(caret.line).spans)
        if ((sp.kind == Syn::String || sp.kind == Syn::Comment) && static_cast<int>(sp.col) < caret.col && caret.col <= static_cast<int>(sp.col + sp.len)) { close(); return false; }
    const std::vector<pluma::lsp::CompletionItem> items = ws.server().complete(ws.uri(d), ws.to_lsp(d, caret));
    std::vector<CompletionRow> out;
    out.reserve(items.size());
    for (const auto& it : items) {
        CompletionRow r;
        r.label = it.label;
        r.detail = it.detail;
        r.doc = it.documentation;
        r.insert = it.insert_text.empty() ? it.label : it.insert_text;
        r.letter = letter_of(it.kind);
        out.push_back(std::move(r));
    }
    if (out.size() == 1 && out[0].insert == line.substr(static_cast<std::size_t>(s), static_cast<std::size_t>(plen))) out.clear();
    // Keep the highlighted label across a refresh when it is still there.
    const std::string keep = active() && sel < static_cast<int>(rows.size()) ? rows[static_cast<std::size_t>(sel)].label : std::string();
    rows = std::move(out);
    anchor = {caret.line, s};
    sel = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].label == keep) sel = static_cast<int>(i);
    return active();
}

void CompletionState::move(int delta) {
    if (rows.empty()) return;
    const int n = static_cast<int>(rows.size());
    sel = ((sel + delta) % n + n) % n;
}

bool CompletionState::accept(TextBuffer& buf) {
    if (!active()) return false;
    const CompletionRow row = rows[static_cast<std::size_t>(sel)];
    const TextPos caret = buf.primary().head;
    close();
    buf.seal();
    buf.replace({anchor, caret}, row.insert);
    return true;
}

bool CompletionState::still_valid(const TextBuffer& buf) const {
    if (!active()) return false;
    const TextPos c = buf.primary().head;
    if (c.line != anchor.line || c.col < anchor.col || buf.has_selection()) return false;
    const std::string& line = buf.line(c.line);
    for (int i = anchor.col; i < c.col; ++i) if (!ident_char(line[static_cast<std::size_t>(i)])) return false;
    return true;
}

std::string escape_rml(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '&': o += "&amp;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string markdown_to_rml(std::string_view md) {
    std::string out;
    bool fence = false;
    std::size_t pos = 0;
    while (pos <= md.size()) {
        std::size_t e = md.find('\n', pos);
        if (e == std::string_view::npos) e = md.size();
        std::string_view ln = md.substr(pos, e - pos);
        pos = e + 1;
        if (ln.starts_with("```")) { fence = !fence; continue; }
        if (fence) { out += "<div class=\"mdcode\"><code>" + escape_rml(ln) + "</code></div>"; continue; }
        if (ln.empty()) continue;
        std::string row;
        bool code = false, bold = false;
        for (std::size_t i = 0; i < ln.size(); ++i) {
            if (ln[i] == '`') { code = !code; row += code ? "<code>" : "</code>"; continue; }
            if (!code && ln[i] == '*' && i + 1 < ln.size() && ln[i + 1] == '*') { bold = !bold; row += bold ? "<b>" : "</b>"; ++i; continue; }
            row += escape_rml(ln.substr(i, 1));
        }
        if (code) row += "</code>";
        if (bold) row += "</b>";
        out += "<div>" + row + "</div>";
    }
    return out;
}

SignatureView signature_view(const pluma::lsp::SignatureHelp& h) {
    SignatureView v;
    if (h.signatures.empty()) return v;
    const std::size_t si = std::min<std::size_t>(h.active_signature, h.signatures.size() - 1);
    const pluma::lsp::SignatureInfo& s = h.signatures[si];
    v.ok = true;
    v.index = static_cast<int>(si);
    v.count = static_cast<int>(h.signatures.size());
    v.doc = s.documentation;
    v.before = s.label;
    if (h.active_parameter < s.parameters.size()) {
        const pluma::lsp::ParameterInfo& p = s.parameters[h.active_parameter];
        // Parameters are searched in order so repeated names do not pick an earlier one.
        std::size_t from = s.label.find('(') == std::string::npos ? 0 : s.label.find('(');
        for (std::uint32_t i = 0; i < h.active_parameter; ++i) {
            const std::size_t q = s.label.find(s.parameters[i].label, from);
            if (q != std::string::npos) from = q + s.parameters[i].label.size();
        }
        const std::size_t at = s.label.find(p.label, from);
        if (at != std::string::npos && !p.label.empty()) {
            v.before = s.label.substr(0, at);
            v.active = p.label;
            v.after = s.label.substr(at + p.label.size());
        }
        if (!p.documentation.empty()) v.doc = p.documentation;
    }
    return v;
}

} // namespace cuervo::editor

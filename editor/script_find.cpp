#include "editor/script_find.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace cuervo::editor {

namespace {

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || (static_cast<unsigned char>(c) & 0x80) != 0; }

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

bool build_regex(std::string_view query, const FindOptions& opt, std::regex& re, std::string& error) {
    std::string pat(query);
    if (opt.whole_word) pat = "\\b(?:" + pat + ")\\b";
    auto flags = std::regex::ECMAScript;
    if (!opt.match_case) flags |= std::regex::icase;
    try {
        re = std::regex(pat, flags);
    } catch (const std::regex_error& e) {
        error = e.what();
        return false;
    }
    return true;
}

} // namespace

FindResult find_all(const TextBuffer& buf, std::string_view query, const FindOptions& opt) {
    FindResult res;
    if (query.empty()) return res;
    if (opt.regex) {
        std::regex re;
        if (!build_regex(query, opt, re, res.error)) return res;
        if (buf.text().size() > kMaxRegexBytes) { res.limited = true; return res; }
        for (int l = 0; l < buf.line_count(); ++l) {
            const std::string& line = buf.line(l);
            if (line.size() > kMaxRegexLine) { res.limited = true; continue; }
            auto it = std::sregex_iterator(line.begin(), line.end(), re);
            for (; it != std::sregex_iterator(); ++it) {
                if (it->length() == 0) continue;
                if (res.ranges.size() >= kMaxMatches) { res.truncated = true; return res; }
                const int c = static_cast<int>(it->position());
                res.ranges.push_back({{l, c}, {l, c + static_cast<int>(it->length())}});
            }
        }
        return res;
    }
    const std::string q = opt.match_case ? std::string(query) : lower(query);
    for (int l = 0; l < buf.line_count(); ++l) {
        const std::string& line = buf.line(l);
        const std::string hay = opt.match_case ? std::string() : lower(line);
        const std::string& h = opt.match_case ? line : hay;
        for (std::size_t p = h.find(q); p != std::string::npos; p = h.find(q, p + q.size())) {
            const std::size_t e = p + q.size();
            if (opt.whole_word && ((p > 0 && word_char(line[p - 1]) && word_char(q.front())) || (e < line.size() && word_char(line[e]) && word_char(q.back())))) continue;
            if (res.ranges.size() >= kMaxMatches) { res.truncated = true; return res; }
            res.ranges.push_back({{l, static_cast<int>(p)}, {l, static_cast<int>(e)}});
        }
    }
    return res;
}

int find_index(const std::vector<TextRange>& r, TextPos from, bool backward, bool after) {
    if (r.empty()) return -1;
    const int n = static_cast<int>(r.size());
    if (!backward) {
        for (int i = 0; i < n; ++i) if (after ? from < r[static_cast<std::size_t>(i)].start : !(r[static_cast<std::size_t>(i)].start < from)) return i;
        return 0;
    }
    for (int i = n - 1; i >= 0; --i) if (r[static_cast<std::size_t>(i)].start < from) return i;
    return n - 1;
}

std::string find_replacement(const TextBuffer& buf, TextRange m, std::string_view query, const FindOptions& opt, std::string_view repl) {
    if (!opt.regex) return std::string(repl);
    std::regex re;
    std::string err;
    if (!build_regex(query, opt, re, err)) return std::string(repl);
    const std::string& line = buf.line(m.start.line);
    std::smatch sm;
    const auto from = line.begin() + m.start.col;
    auto fl = std::regex_constants::match_continuous;
    if (m.start.col > 0) fl |= std::regex_constants::match_prev_avail;
    if (std::regex_search(from, line.end(), sm, re, fl)) return sm.format(std::string(repl));
    return std::string(repl);
}

int find_replace_all(TextBuffer& buf, std::string_view query, const FindOptions& opt, std::string_view repl) {
    const FindResult r = find_all(buf, query, opt);
    if (r.ranges.empty()) return 0;
    std::vector<std::pair<TextRange, std::string>> edits;
    edits.reserve(r.ranges.size());
    for (const TextRange& m : r.ranges) edits.emplace_back(m, find_replacement(buf, m, query, opt, repl));
    buf.replace_all(std::move(edits));
    return static_cast<int>(r.ranges.size());
}

void FindSession::refresh(TextBuffer& buf, bool jump) {
    revision = buf.revision();
    result = find_all(buf, query, opt);
    const Selection& sel = buf.primary();
    current = -1;
    for (std::size_t i = 0; i < result.ranges.size(); ++i)
        if (result.ranges[i].start == sel.start() && result.ranges[i].end == sel.end()) current = static_cast<int>(i);
    if (jump && !result.ranges.empty()) select(buf, find_index(result.ranges, sel.start(), false, false));
}

void FindSession::select(TextBuffer& buf, int idx) {
    message.clear();
    current = idx;
    if (idx < 0 || idx >= static_cast<int>(result.ranges.size())) return;
    buf.seal();
    buf.select_range(result.ranges[static_cast<std::size_t>(idx)]);
}

bool FindSession::step(TextBuffer& buf, bool back) {
    if (revision != buf.revision()) refresh(buf, false);
    if (result.ranges.empty()) return false;
    const Selection& s = buf.primary();
    select(buf, find_index(result.ranges, back ? s.start() : s.end(), back, false));
    return true;
}

bool FindSession::replace_current(TextBuffer& buf) {
    if (revision != buf.revision()) refresh(buf, false);
    const Selection s = buf.primary();
    int hit = -1;
    for (std::size_t i = 0; i < result.ranges.size(); ++i)
        if (result.ranges[i].start == s.start() && result.ranges[i].end == s.end()) hit = static_cast<int>(i);
    if (hit < 0) return step(buf, false); // nothing selected yet: first select the next match
    const TextRange r = result.ranges[static_cast<std::size_t>(hit)];
    const std::string text = find_replacement(buf, r, query, opt, replacement);
    buf.seal();
    buf.replace(r, text);
    refresh(buf, false);
    if (!result.ranges.empty()) select(buf, find_index(result.ranges, buf.primary().head, false, false));
    return true;
}

int FindSession::replace_everything(TextBuffer& buf) {
    if (query.empty()) return 0;
    buf.seal();
    const int n = find_replace_all(buf, query, opt, replacement);
    refresh(buf, false);
    message = n > 0 ? "Replaced " + std::to_string(n) : "";
    return n;
}

} // namespace cuervo::editor

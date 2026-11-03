#pragma once
// Find and replace over a TextBuffer (AUDIT.md section 15, S-2). UI-free. Matches never span lines
// (a regex is applied line by line), so a search costs O(text) and a replace is one `replace_all`
// undo step. Bounds: at most kMaxMatches results, and regex search is skipped on a document over
// kMaxRegexBytes or a line over kMaxRegexLine (the result carries `limited`), because std::regex
// cannot be interrupted.

#include <string>
#include <string_view>
#include <vector>

#include "editor/text_buffer.hpp"

namespace cuervo::editor {

struct FindOptions {
    bool match_case = false;
    bool whole_word = false;
    bool regex = false;
    bool operator==(const FindOptions&) const = default;
};

struct FindResult {
    std::vector<TextRange> ranges; // ascending, non-overlapping, never empty ranges
    bool truncated = false;        // stopped at kMaxMatches
    bool limited = false;          // regex skipped parts of a huge document
    std::string error;             // invalid regex (ranges is empty)
};

inline constexpr std::size_t kMaxMatches = 20000;
inline constexpr std::size_t kMaxRegexBytes = 4u << 20;
inline constexpr std::size_t kMaxRegexLine = 20000;

FindResult find_all(const TextBuffer& buf, std::string_view query, const FindOptions& opt);

// Index of the first match at or after `from` (or the last one before it when `backward`), wrapping.
// With `after` the match starting exactly at `from` is skipped (stepping to the next one). -1 when empty.
int find_index(const std::vector<TextRange>& r, TextPos from, bool backward, bool after);

// The replacement text for match `m` (regex mode expands $1, $&; plain mode returns `repl`).
std::string find_replacement(const TextBuffer& buf, TextRange m, std::string_view query, const FindOptions& opt, std::string_view repl);

// Replaces every match as one undo step; returns the count.
int find_replace_all(TextBuffer& buf, std::string_view query, const FindOptions& opt, std::string_view repl);

// The state behind the find bar, UI-free so it can be tested: query, options, results and the current match.
class FindSession {
public:
    std::string query, replacement, message;
    FindOptions opt;
    FindResult result;
    int current = -1; // index into result.ranges, -1 = none

    // Recomputes the matches (call when the query, options or buffer revision changed). With `jump` the
    // nearest match at or after the caret is selected.
    void refresh(TextBuffer& buf, bool jump);
    void select(TextBuffer& buf, int idx);          // selects match idx (sealed, so caret moves are not coalesced)
    bool step(TextBuffer& buf, bool back);          // next/previous match with wrap; false when none
    bool replace_current(TextBuffer& buf);          // replaces the selected match, then selects the next one
    int replace_everything(TextBuffer& buf);        // one undo step; sets `message`
    std::uint64_t revision = ~std::uint64_t{0};     // buffer revision `result` is for
};

} // namespace cuervo::editor

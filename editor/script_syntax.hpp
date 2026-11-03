#pragma once
// Syntax highlighting for the script editor (AUDIT.md section 15): a per-line span cache over the
// TextBuffer. After an edit only the dirty lines are re-lexed, starting at the nearest line that does
// not begin inside a block comment or multi-line string, and stopping at the first later line whose
// start state matches the old cache. All pluma::Lexer use is in script_syntax.cpp:
// the seam is `lex_line` (Lexer::tokenize_line with the carried state).

#include <cstdint>
#include <functional>
#include <vector>

#include "editor/text_buffer.hpp"
#include "pluma/lexer.hpp"

namespace cuervo::editor {

enum class Syn : std::uint8_t { Plain, Keyword, Type, String, Number, Comment, Error, Function, Constant };

struct SynSpan {
    std::uint32_t col = 0, len = 0; // bytes within the line
    Syn kind = Syn::Plain;
};

struct LineSyn {
    std::vector<SynSpan> spans;           // ascending, non-overlapping; gaps are Plain
    Syn carry = Syn::Plain;               // Comment or String when the line starts inside a multi-line token
    pluma::LineState in, out;             // lexer state at the start and end of the line
    std::vector<SynSpan> lex, sem;        // lexical spans, and the semantic overlay (functions, constants, types)
    std::uint64_t stamp = 0;              // changes whenever the line's text or spans changed
    bool dirty = true;
};

class SyntaxCache {
public:
    // Brings the cache up to date with `buf` (consumes buf.take_changes()).
    void update(TextBuffer& buf);
    // Told about every line change the update consumes (the workspace moves breakpoints with their lines).
    std::function<void(const LineChange&)> on_change;
    const LineSyn& line(int i) const { return lines_[static_cast<std::size_t>(i)]; }
    int size() const { return static_cast<int>(lines_.size()); }
    // Semantic overlay from the language server (one entry per name-resolved identifier). Lines edited since
    // the server saw the text are skipped; they keep their lexical colors until the next refresh.
    struct SemanticSpan { std::uint32_t line = 0, col = 0, len = 0; Syn kind = Syn::Plain; };
    void set_semantic(const std::vector<SemanticSpan>& toks);
    // Lines re-lexed by the last update (tests: proves incremental behaviour).
    int last_lexed() const { return last_lexed_; }

private:
    static void merge(LineSyn& ln);
    std::vector<LineSyn> lines_;
    std::uint64_t next_stamp_ = 1;
    int last_lexed_ = 0;
};

} // namespace cuervo::editor

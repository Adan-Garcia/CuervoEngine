#pragma once
// UI-free text buffer for the script editor (AUDIT.md section 15): UTF-8 lines, grapheme-aware caret
// movement, a selection set (multi-caret in the model, the UI drives one), grouped undo/redo with
// typing coalesced, line-ending and indent detection, and UTF-16 position conversion for LSP.
// Text is held with "\n" only; `text_for_save()` puts the detected line ending back.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cuervo::editor {

struct TextPos {
    int line = 0;
    int col = 0; // byte offset in the line, always on a grapheme boundary
    bool operator==(const TextPos&) const = default;
    bool operator<(const TextPos& o) const { return line != o.line ? line < o.line : col < o.col; }
    bool operator<=(const TextPos& o) const { return !(o < *this); }
};

struct TextRange {
    TextPos start, end;
    bool empty() const { return start == end; }
    bool operator==(const TextRange&) const = default;
};

struct Selection {
    TextPos anchor, head; // head is the caret
    TextPos start() const { return head < anchor ? head : anchor; }
    TextPos end() const { return head < anchor ? anchor : head; }
    bool empty() const { return anchor == head; }
    bool operator==(const Selection&) const = default;
};

enum class LineEnding { Lf, CrLf };
struct IndentStyle {
    bool tabs = false;
    int width = 4; // spaces per level (also the tab display width)
};

// One contiguous replacement of lines [first, first+removed) by `added` lines, for caches.
struct LineChange {
    int first = 0, removed = 0, added = 0;
};

enum class Move { Left, Right, Up, Down, WordLeft, WordRight, LineStart, LineEnd, SmartHome, DocStart, DocEnd, PageUp, PageDown };

// Grapheme helpers (UTF-8). `next_boundary`/`prev_boundary` return byte offsets.
std::size_t next_boundary(std::string_view s, std::size_t off);
std::size_t prev_boundary(std::string_view s, std::size_t off);
// Terminal-style cell width of a grapheme cluster: 0 for none, 2 for wide (CJK, emoji), else 1.
int grapheme_cells(std::string_view cluster);
// Display column of byte `col` with tabs expanded to `tab` stops, and the inverse (nearest boundary at or before).
int display_col(std::string_view line, int col, int tab);
int col_at_display(std::string_view line, int dcol, int tab);
// UTF-16 code units in line[0, col).
int utf16_len(std::string_view s);

class TextBuffer {
public:
    TextBuffer() { lines_.emplace_back(); sels_.push_back({}); }

    // Replaces everything; detects line ending and indentation, resets undo and the saved marker.
    void set_text(std::string_view text);
    std::string text() const;           // "\n" separated
    std::string text_for_save() const;  // detected line ending, BOM kept if it had one
    int line_count() const { return static_cast<int>(lines_.size()); }
    const std::string& line(int i) const { return lines_[static_cast<std::size_t>(i)]; }
    LineEnding line_ending() const { return eol_; }
    void set_line_ending(LineEnding e) { eol_ = e; }
    const IndentStyle& indent() const { return indent_; }
    bool has_bom() const { return bom_; }
    std::uint64_t revision() const { return revision_; }

    // Positions and ranges.
    TextPos clamp(TextPos p) const;
    std::size_t offset_of(TextPos p) const;     // byte offset in text()
    TextPos pos_of(std::size_t offset) const;
    std::string text_in(TextRange r) const;
    TextPos end_pos() const { return {line_count() - 1, static_cast<int>(lines_.back().size())}; }
    // LSP positions (UTF-16 characters).
    int to_utf16(TextPos p) const;
    TextPos from_utf16(int line, int character) const;
    // Word at/around p (identifier characters); empty range when none.
    TextRange word_at(TextPos p) const;

    // Selections. The last one is the primary caret.
    const std::vector<Selection>& selections() const { return sels_; }
    const Selection& primary() const { return sels_.back(); }
    void set_selections(std::vector<Selection> s);
    void set_caret(TextPos p, bool extend = false);
    void add_caret(TextPos p);
    void select_all();
    void select_range(TextRange r) { set_selections({{r.start, r.end}}); }
    std::string selected_text() const; // multi-selection joined by "\n"
    bool has_selection() const;
    void move(Move m, bool extend, int page_lines = 20);

    // Edits. All of them run on every selection and form one undo step (or coalesce, see `typing`).
    // `now_ms` only matters for `typed`: consecutive typed inserts within 1000 ms and adjacent join one undo step.
    void insert(std::string_view text, bool typed = false, std::uint64_t now_ms = 0);
    // Typing with bracket and quote help: skips over a closing char that is already there, wraps a selection
    // in a pair, and auto-closes ( [ { and " when the next char is not part of a word. Anything else is a
    // plain insert. Returns true when the pairing logic (not the plain insert) handled the key.
    bool type(std::string_view text, std::uint64_t now_ms = 0);
    // The bracket pair next to the caret (char at the caret first, then the one before), or none. Strings
    // and // comments on the scanned lines do not count; block comments are not tracked.
    bool match_bracket(TextPos caret, TextPos& open, TextPos& close) const;
    void insert_newline();              // keeps the line's indent, one level deeper after '{'
    void backspace();
    void delete_forward();
    void delete_word_back();
    void delete_word_forward();
    void delete_line();
    void duplicate_line();
    void indent_lines(bool outdent);    // Tab / Shift+Tab with a multi-line selection or Shift+Tab alone
    void move_lines(int dir);           // Alt+Up / Alt+Down
    void toggle_comment();              // "// " on the selected lines
    // A replacement of one range (selection becomes a caret after it). Used by find/replace, rename, format.
    void replace(TextRange r, std::string_view text);
    // Several replacements as ONE undo step (ranges are in the current text, non-overlapping, any order).
    void replace_all(std::vector<std::pair<TextRange, std::string>> edits);

    // Undo and redo. `seal()` ends the current coalescing group (caret jumps call it).
    bool undo();
    bool redo();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    void seal() { sealed_ = true; }
    void mark_saved();
    bool dirty() const { return state_id_ != saved_id_; }

    // Line changes since the last call (for highlight and diagnostics caches).
    std::vector<LineChange> take_changes() { return std::move(changes_); }

    // Normalizes CRLF/CR to "\n" (clipboard and file text).
    static std::string normalize(std::string_view s);

private:
    struct Edit { TextPos at; std::string removed, inserted; };
    struct Group {
        std::vector<Edit> edits;
        std::vector<Selection> before, after;
        std::uint64_t id = 0;
        std::uint64_t last_ms = 0;
        bool typed = false;
    };

    struct Planned {
        TextRange r;
        std::string text;
        int caret = -1; // byte offset into `text` for the caret, -1 = after it
    };
    // Runs planned edits (ascending, non-overlapping) top-down in the open group; later ranges follow earlier edits.
    std::vector<TextPos> run(std::vector<Planned>& plan);
    // Applies one replacement without recording; returns the position after the inserted text.
    TextPos apply(TextRange r, std::string_view text, std::string* removed);
    void begin(bool typed, std::uint64_t now_ms, TextPos probe);
    void record(TextPos at, std::string removed, std::string inserted);
    void finish();
    template <class F> void for_each_sel_bottom_up(F&& f);
    void normalize_selections();
    void set_primary_only(TextPos p, bool extend);

    std::vector<std::string> lines_;
    std::vector<Selection> sels_;
    std::vector<Group> undo_, redo_;
    std::vector<LineChange> changes_;
    LineEnding eol_ = LineEnding::Lf;
    IndentStyle indent_;
    bool bom_ = false;
    std::uint64_t revision_ = 0, next_id_ = 1, state_id_ = 0, saved_id_ = 0;
    bool sealed_ = true;
    bool open_ = false; // a group is being built
    int goal_col_ = -1; // display column kept across Up/Down
};

} // namespace cuervo::editor

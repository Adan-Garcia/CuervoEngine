#pragma once
// Language assistance for the script editor (AUDIT.md section 15, S-2): the UI-free state behind the
// autocomplete popup, hover card and signature help. The LSP server does the analysis; this holds the
// popup state (anchor, selection, accept) and turns Markdown/signatures into small display pieces.

#include <string>
#include <string_view>
#include <vector>

#include "editor/script_workspace.hpp"

namespace cuervo::editor {

struct CompletionRow {
    std::string label, detail, doc, insert;
    char letter = 'f'; // kind badge: f field, m method, v variable, C class/type, k keyword, c constant, e enum, s signal
};

class CompletionState {
public:
    bool active() const { return !rows.empty(); }
    std::vector<CompletionRow> rows;
    int sel = 0;
    TextPos anchor; // start of the identifier prefix being completed

    // Asks the server at the caret. `min_prefix` = shortest identifier prefix that opens the list (0 after
    // a '.', where every member is wanted). Returns active(). The list stays closed when its only entry is
    // exactly what is typed.
    bool query(ScriptWorkspace& ws, ScriptDoc& d, int min_prefix);
    void move(int delta);
    void close() { rows.clear(); sel = 0; }
    // Replaces [anchor, caret) with the selected row's text (one undo step); closes. False when closed.
    bool accept(TextBuffer& buf);
    // False when the caret left the completed word (other line, before the anchor, or past a non-identifier).
    bool still_valid(const TextBuffer& buf) const;
};

// Hover contents: Markdown reduced to rml (fenced and `inline` code in <code>, **bold** in <b>, text escaped).
std::string markdown_to_rml(std::string_view md);
std::string escape_rml(std::string_view s);

struct SignatureView {
    std::string before, active, after; // the label split around the active parameter
    std::string doc;                   // documentation of the active parameter, else of the signature
    int index = 0, count = 0;          // overload position
    bool ok = false;
};
SignatureView signature_view(const pluma::lsp::SignatureHelp& h);

} // namespace cuervo::editor

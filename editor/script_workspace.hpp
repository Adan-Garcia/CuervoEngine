#pragma once
// The script editor's documents (AUDIT.md section 15), UI-free: open tabs with their TextBuffer and
// highlight cache, dirty/saved state, MRU tab order, the in-process pluma LSP server with a 150 ms
// diagnostics debounce, outline symbols and project script discovery. The RmlUi side only presents it.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "editor/script_syntax.hpp"
#include "editor/text_buffer.hpp"
#include "pluma/lsp.hpp"

namespace cuervo::editor {

enum class ProblemSeverity : std::uint8_t { Error, Warning, Note };

struct Problem {
    TextRange range;
    ProblemSeverity severity = ProblemSeverity::Error;
    std::string message;
    std::string code; // lint code ("unused-variable", ...), empty for compiler errors
};

struct OutlineItem {
    std::string name, detail;
    int kind = 0; // pluma::lsp::SymbolKind
    int depth = 0;
    TextPos pos;  // selection start
    TextRange range;
};

// One reference to a symbol: where it is and the line it sits on (for the list).
struct RefHit {
    std::filesystem::path path;
    std::string rel;
    TextRange range;
    std::string preview; // the source line, trimmed
    bool open = false;   // the file has a tab
};

// What a rename would do, for the preview line before it is applied.
struct RenamePlan {
    pluma::lsp::WorkspaceEdit edit;
    int edits = 0, files = 0, unsaved = 0; // unsaved: files whose tab has unsaved changes
    std::vector<std::string> rels;
};

struct ScriptDoc {
    int id = 0;
    std::filesystem::path path;   // absolute
    std::string rel;              // project relative, '/' separators
    TextBuffer buf;
    SyntaxCache syn;
    std::vector<Problem> problems;
    std::vector<OutlineItem> outline;
    std::uint64_t synced_rev = ~std::uint64_t{0}; // buffer revision the LSP server has
    std::uint64_t checked_rev = ~std::uint64_t{0}; // buffer revision the problems/outline are for
    std::uint64_t due_rev = ~std::uint64_t{0}; // revision the debounce below is for
    std::uint64_t due_ms = 0;     // diagnostics run at or after this time
    std::set<int>* bps = nullptr; // this file's breakpoints (1-based lines), owned by the workspace; follows edits
    std::int64_t disk_mtime = 0;
    int version = 0;
    bool missing = false;         // the file vanished from disk
    std::string name() const { return path.filename().string(); }
};

class ScriptWorkspace {
public:
    static constexpr std::uint64_t kDebounceMs = 150;

    // Starts over for a project (closes every tab). Empty dir = no project.
    void reset(const std::filesystem::path& project_dir);
    const std::filesystem::path& project_dir() const { return dir_; }

    // Tabs.
    int open(const std::filesystem::path& file, std::string& error); // id, or -1
    int new_script(const std::string& rel_path, const std::string& class_name, const std::string& base, std::string& error);
    int find(const std::filesystem::path& file) const; // id or -1
    ScriptDoc* doc(int id);
    const ScriptDoc* doc(int id) const;
    const std::vector<std::unique_ptr<ScriptDoc>>& tabs() const { return docs_; }
    int active() const { return active_; }
    void activate(int id);
    // Ctrl+Tab: next tab in most-recently-used order (`back` for Shift+Ctrl+Tab); activates it.
    int cycle(bool back);
    // Closes without asking; the caller checks `dirty` first. Activates the most recent remaining tab.
    void close(int id);
    int dirty_count() const;

    // Saving writes the file (format on save when enabled and the text is valid).
    bool save(int id, std::string& error);
    bool save_all(std::string& error);
    bool format_on_save = true; // setting; "Format Document" in the palette runs it on demand
    // Formats the document as one undo step; true when the text changed.
    bool format(int id);
    // Reloads a clean tab whose file changed on disk; true if any tab changed.
    bool refresh_from_disk();

    // Diagnostics: call every frame with a monotonic clock; recomputes after the debounce.
    // Returns true when any document's problems changed.
    bool tick(std::uint64_t now_ms);
    // Immediately syncs and recomputes (tests, save).
    void check_now(int id);
    int error_count(const ScriptDoc& d) const;
    int warning_count(const ScriptDoc& d) const;

    // Project scripts (relative paths, sorted).
    std::vector<std::string> project_scripts() const;

    // LSP access for completion, hover and navigation (syncs the document first).
    pluma::lsp::Server& server() { return *server_; }
    std::string uri(const ScriptDoc& d) const;
    pluma::lsp::Position to_lsp(const ScriptDoc& d, TextPos p) const;
    TextPos from_lsp(const ScriptDoc& d, pluma::lsp::Position p) const;
    TextRange from_lsp(const ScriptDoc& d, pluma::lsp::Range r) const;
    void sync(ScriptDoc& d);
    void sync_all();

    // Line breakpoints per project file (1-based). They move with their lines while a file is edited.
    using Breakpoints = std::map<std::string, std::set<int>>;
    const Breakpoints& breakpoints() const { return bps_; }
    void set_breakpoints(Breakpoints b);
    bool toggle_breakpoint(const std::string& rel, int line); // true: now set
    void clear_breakpoints();
    std::uint64_t breakpoint_revision() const { return bp_rev_; }
    static void shift_breakpoints(std::set<int>& lines, const LineChange& c);

    // Find references and rename work on the LSP server's view of every open tab (unsaved text included).
    std::vector<RefHit> references(int id, TextPos at);
    std::optional<TextRange> prepare_rename(int id, TextPos at);
    std::optional<RenamePlan> plan_rename(int id, TextPos at, const std::string& new_name);
    // Applies the edit to the buffers: one undo group per file. Files without a tab are opened (so the
    // change can be undone and is saved with Save all); nothing is written to disk. Returns the doc ids.
    std::vector<int> apply_edit(const pluma::lsp::WorkspaceEdit& edit, std::string& error);

private:
    void make_server();
    void recompute(ScriptDoc& d);
    void touch_mru(int id);

    std::filesystem::path dir_;
    std::unique_ptr<pluma::lsp::Server> server_;
    std::vector<std::unique_ptr<ScriptDoc>> docs_;
    Breakpoints bps_;
    std::uint64_t bp_rev_ = 1;
    std::vector<int> mru_; // most recent first
    int active_ = -1;
    int next_id_ = 1;
    int cycle_pos_ = -1;   // while Ctrl is held through repeated Ctrl+Tab
};

} // namespace cuervo::editor

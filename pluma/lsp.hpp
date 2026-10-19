#pragma once

// Language Server Protocol (LSP 3.17) server for Pluma (DESIGN.md 5.14).
//
// Standalone in `pluma/` (never depends on `cuervo/`). A host such as
// `cuervo-tools lsp` or `cuervo-editor` can supply its prelude sources
// (`cuervo/script/prelude/*.plm`) and registered `@native` function names via
// `ServerOptions`, giving completion, go-to-definition, hover, signature help,
// references, rename and diagnostics full knowledge of the engine API alongside
// the project's scripts.

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/diagnostic.hpp"
#include "pluma/source.hpp"

namespace pluma::lsp {

// 0-based line and character in UTF-16 code units (LSP Position, the
// protocol's default encoding).
struct Position {
    std::uint32_t line = 0;
    std::uint32_t character = 0;
    bool operator==(const Position&) const = default;
    bool operator<(const Position& o) const {
        return line != o.line ? line < o.line : character < o.character;
    }
};

struct Range {
    Position start;
    Position end;
    bool operator==(const Range&) const = default;
    bool contains(Position p) const { return !(p < start) && (p < end || p == end); }
    bool overlaps(const Range& o) const { return !(end < o.start) && !(o.end < start); }
};

struct Location {
    std::string uri;
    Range range;
    bool operator==(const Location&) const = default;
};

struct TextEdit {
    Range range;
    std::string new_text;
    bool operator==(const TextEdit&) const = default;
};

struct DiagnosticItem {
    Range range;
    Severity severity = Severity::Error;
    std::string message;
    std::optional<TextEdit> fix;
};

// Matches LSP CompletionItemKind values.
enum class CompletionKind : std::uint8_t {
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8, // trait
    Module = 9,
    Property = 10,
    Enum = 13,
    Keyword = 14,
    EnumMember = 20,
    Constant = 21,
    Struct = 22,
    Event = 23, // signal
    TypeParameter = 25,
};

struct CompletionItem {
    std::string label;
    CompletionKind kind = CompletionKind::Text;
    std::string detail;
    std::string documentation;
    std::string insert_text;
};

struct HoverInfo {
    std::string contents; // Markdown
    Range range;
};

struct ParameterInfo {
    std::string label;
    std::string documentation;
};

struct SignatureInfo {
    std::string label;
    std::string documentation;
    std::vector<ParameterInfo> parameters;
};

struct SignatureHelp {
    std::vector<SignatureInfo> signatures;
    std::uint32_t active_signature = 0;
    std::uint32_t active_parameter = 0;
};

// Matches LSP SymbolKind values.
enum class SymbolKind : std::uint8_t {
    Class = 5,
    Method = 6,
    Property = 7,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Interface = 11, // trait
    Function = 12,
    Variable = 13,
    Constant = 14,
    EnumMember = 22,
    Struct = 23,
    Event = 24, // signal
};

struct DocumentSymbol {
    std::string name;
    std::string detail;
    SymbolKind kind = SymbolKind::Variable;
    Range range;
    Range selection_range;
    std::vector<DocumentSymbol> children;
};

struct WorkspaceSymbol {
    std::string name;
    SymbolKind kind = SymbolKind::Variable;
    Location location;
    std::string container_name;
};

struct CodeAction {
    std::string title;
    std::string kind = "quickfix";
    std::string uri;
    std::vector<TextEdit> edits;
};

struct WorkspaceEdit {
    std::map<std::string, std::vector<TextEdit>> changes; // URI -> edits
};

struct ServerOptions {
    // Prelude files compiled before the workspace's own `.plm` files (e.g.
    // `cuervo/script/prelude/*.plm`).
    std::vector<SourceFile::Part> preludes;
    // Host function names available to `@native` declarations (e.g.
    // "Canvas.circle"). When empty, any `@native` declaration in a prelude is
    // automatically accepted.
    std::set<std::string, std::less<>> native_names;
    // Optional initial workspace directory path.
    std::string root_path;
    // Include naming style warnings (LANGUAGE.md 6) in published diagnostics.
    bool check_style = false;
};

// Converts between filesystem paths and `file://` URIs.
std::string path_to_uri(std::string_view path);
std::string uri_to_path(std::string_view uri);

class Server {
public:
    explicit Server(ServerOptions options = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void set_root_path(std::string path);
    void set_root_uri(std::string_view uri);
    const std::string& root_path() const { return options_.root_path; }

    // ---- document lifecycle
    void open_document(std::string_view uri, std::string text, int version = 1);
    void change_document(std::string_view uri, std::string text, int version = 2);
    void change_document_range(std::string_view uri, Range range, std::string_view text, int version = 2);
    void close_document(std::string_view uri);
    std::optional<std::string> document_text(std::string_view uri) const;

    // ---- language queries
    std::vector<DiagnosticItem> diagnostics(std::string_view uri);
    std::map<std::string, std::vector<DiagnosticItem>> all_diagnostics();
    std::vector<CompletionItem> complete(std::string_view uri, Position pos);
    std::optional<Location> definition(std::string_view uri, Position pos);
    std::optional<HoverInfo> hover(std::string_view uri, Position pos);
    std::optional<SignatureHelp> signature_help(std::string_view uri, Position pos);
    std::vector<DocumentSymbol> document_symbols(std::string_view uri);
    std::vector<WorkspaceSymbol> workspace_symbols(std::string_view query);
    std::vector<Location> references(std::string_view uri, Position pos, bool include_declaration = true);
    std::optional<Range> prepare_rename(std::string_view uri, Position pos);
    std::optional<WorkspaceEdit> rename(std::string_view uri, Position pos, std::string_view new_name);
    std::vector<CodeAction> code_actions(std::string_view uri, Range range);
    std::vector<TextEdit> format_document(std::string_view uri);

    // ---- JSON-RPC 2.0 protocol
    // Processes one JSON-RPC message and returns any outgoing JSON-RPC messages
    // (responses and `textDocument/publishDiagnostics` notifications).
    std::vector<std::string> handle_message(std::string_view json_payload);
    bool exit_requested() const { return exit_requested_; }
    int exit_code() const { return shutdown_received_ ? 0 : 1; }

    // Runs the standard `Content-Length` framed stdio loop until `exit` or EOF.
    int run_stdio(std::istream& in, std::ostream& out);

private:
    struct Impl;
    ServerOptions options_;
    bool shutdown_received_ = false;
    bool exit_requested_ = false;
    std::map<std::string, std::string> open_docs_; // normalized URI -> text
    std::map<std::string, int> doc_versions_;
    std::set<std::string> published_uris_;

    void ensure_root_from_uri(std::string_view uri);
    std::vector<std::string> publish_diagnostics_notifications();
};

} // namespace pluma::lsp

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pluma {

// Half-open byte range [begin, end) in a source file.
struct Span {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    static Span cover(Span a, Span b) { return {a.begin < b.begin ? a.begin : b.begin, a.end > b.end ? a.end : b.end}; }
    bool operator==(const Span&) const = default;
};

struct LineCol {
    std::uint32_t line = 1;   // 1-based
    std::uint32_t column = 1; // 1-based, in bytes
};

// A file position as reported to users: the file's own path and its line
// and column (for a combined source, relative to the segment).
struct Location {
    std::string_view path;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

// A source file's text plus a line index. ASTs and tokens point into `text`,
// so a SourceFile must outlive them and must not be moved after parsing.
//
// Several files can be combined into one SourceFile (`combine`), so a whole
// project is parsed and checked as one unit (DESIGN.md 5.11: names are
// global, files reference each other freely). `line_col` stays absolute
// (the lexer and parser use it); `location` maps an offset back to the
// original file for diagnostics and panics.
class SourceFile {
public:
    SourceFile(std::string path, std::string text);
    struct Part {
        std::string path;
        std::string text;
        // The package this file belongs to (LANGUAGE.md 9.26); empty for the
        // game's own files, whose names are the root namespace.
        std::string module = {};
        // A prelude file (the engine API, the std library): its root names
        // are visible to packages too.
        bool prelude = false;
    };
    // Joins `parts` with a newline after each. `path` names the whole.
    static std::unique_ptr<SourceFile> combine(std::string path, const std::vector<Part>& parts);
    SourceFile(const SourceFile&) = delete;
    SourceFile& operator=(const SourceFile&) = delete;

    const std::string& path() const { return path_; }
    std::string_view text() const { return text_; }
    std::string_view slice(Span span) const { return std::string_view(text_).substr(span.begin, span.end - span.begin); }

    LineCol line_col(std::uint32_t offset) const;
    // The text of a 1-based line, without its newline.
    std::string_view line_text(std::uint32_t line) const;
    std::uint32_t line_count() const { return static_cast<std::uint32_t>(line_starts_.size()); }

    // User-facing position of `offset` (segment-relative for combined files).
    Location location(std::uint32_t offset) const;
    // The text of the part named `path` (the whole text for a single file);
    // empty if there is none. Debuggers show sources that aren't on disk.
    std::string_view part_text(std::string_view path) const;
    // The package of the part containing `offset` (empty: the root
    // namespace), and whether that part is a prelude.
    std::string_view module_at(std::uint32_t offset) const;
    bool prelude_at(std::uint32_t offset) const;
    // Index of the part containing `offset` (0 for a single file); imports
    // are per part.
    std::size_t part_index(std::uint32_t offset) const;
    std::size_t part_count() const { return segments_.empty() ? 1 : segments_.size(); }
    std::string_view part_module(std::size_t index) const {
        return segments_.empty() ? std::string_view() : std::string_view(segments_[index].module);
    }
    // The text of the line containing `offset`.
    std::string_view line_text_at(std::uint32_t offset) const { return line_text(line_col(offset).line); }

private:
    struct Segment {
        std::uint32_t offset = 0;
        std::uint32_t first_line = 1; // absolute line of the segment's first line
        std::string path;
        std::string module;
        bool prelude = false;
    };
    std::vector<Segment> segments_; // empty: a single file
    std::string path_;
    std::string text_;
    std::vector<std::uint32_t> line_starts_;
};

} // namespace pluma

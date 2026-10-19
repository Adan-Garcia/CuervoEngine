#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pluma/source.hpp"

namespace pluma {

enum class Severity : std::uint8_t { Error, Warning, Note };

// A suggested edit: replace `span` with `replacement`.
struct FixIt {
    Span span;
    std::string replacement;
};

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string file;
    std::uint32_t line = 1;   // 1-based, of span.begin
    std::uint32_t column = 1; // 1-based, in bytes
    Span span;
    std::string message;
    std::optional<FixIt> fix;
};

// Collects diagnostics for one source file.
class DiagnosticSink {
public:
    explicit DiagnosticSink(const SourceFile& source) : source_(source) {}

    Diagnostic& report(Severity severity, Span span, std::string message);
    Diagnostic& error(Span span, std::string message) { return report(Severity::Error, span, std::move(message)); }

    const std::vector<Diagnostic>& all() const { return diagnostics_; }
    std::size_t error_count() const;
    bool has_errors() const { return error_count() > 0; }
    const SourceFile& source() const { return source_; }
    // Stable sort by position, so errors read top to bottom.
    void sort_by_position();

private:
    const SourceFile& source_;
    std::vector<Diagnostic> diagnostics_;
};

// Renders one diagnostic, clang style:
//
//   boss.plm:3:10: error: message
//       3 | var x = a?b:c
//         |          ^
//         = fix-it: replace 'a?b:c' with 'a ? b : c'
std::string render(const Diagnostic& diagnostic, const SourceFile& source);
std::string render_all(const std::vector<Diagnostic>& diagnostics, const SourceFile& source);

} // namespace pluma

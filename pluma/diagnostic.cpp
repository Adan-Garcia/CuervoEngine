#include "pluma/diagnostic.hpp"

#include <algorithm>
#include <string_view>

namespace pluma {

Diagnostic& DiagnosticSink::report(Severity severity, Span span, std::string message) {
    Location loc = source_.location(span.begin);
    diagnostics_.push_back(
        {severity, std::string(loc.path), loc.line, loc.column, span, std::move(message), std::nullopt});
    return diagnostics_.back();
}

std::size_t DiagnosticSink::error_count() const {
    return static_cast<std::size_t>(std::count_if(diagnostics_.begin(), diagnostics_.end(),
                                                  [](const Diagnostic& d) { return d.severity == Severity::Error; }));
}

void DiagnosticSink::sort_by_position() {
    std::stable_sort(diagnostics_.begin(), diagnostics_.end(),
                     [](const Diagnostic& a, const Diagnostic& b) { return a.span.begin < b.span.begin; });
}

std::string render(const Diagnostic& d, const SourceFile& source) {
    std::string out = d.file + ':' + std::to_string(d.line) + ':' + std::to_string(d.column) + ": ";
    switch (d.severity) {
        case Severity::Error: out += "error: "; break;
        case Severity::Warning: out += "warning: "; break;
        case Severity::Note: out += "note: "; break;
    }
    out += d.message;
    out += '\n';

    std::string_view line = source.line_text_at(d.span.begin);
    std::string number = std::to_string(d.line);
    std::string gutter(number.size(), ' ');
    out += ' ' + number + " | ";
    out += line;
    out += '\n';
    out += ' ' + gutter + " | ";
    // Keep tabs so the caret lines up with the source line.
    for (std::uint32_t i = 1; i < d.column && i - 1 < line.size(); ++i) {
        out += line[i - 1] == '\t' ? '\t' : ' ';
    }
    out += '^';
    // Underline the rest of the span on this line.
    std::uint32_t span_len = d.span.end > d.span.begin ? d.span.end - d.span.begin : 1;
    std::uint32_t room = line.size() >= d.column ? static_cast<std::uint32_t>(line.size()) - d.column + 1 : 1;
    for (std::uint32_t i = 1; i < std::min(span_len, room); ++i) {
        out += '~';
    }
    out += '\n';
    if (d.fix) {
        if (d.fix->span.begin == d.fix->span.end) {
            out += ' ' + gutter + " = fix-it: insert '" + d.fix->replacement + "'\n";
        } else {
            out += ' ' + gutter + " = fix-it: replace '";
            out += source.slice(d.fix->span);
            out += "' with '" + d.fix->replacement + "'\n";
        }
    }
    return out;
}

std::string render_all(const std::vector<Diagnostic>& diagnostics, const SourceFile& source) {
    std::string out;
    for (const Diagnostic& d : diagnostics) {
        out += render(d, source);
    }
    return out;
}

} // namespace pluma

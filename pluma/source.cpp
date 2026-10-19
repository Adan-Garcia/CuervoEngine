#include "pluma/source.hpp"

#include <algorithm>

namespace pluma {

SourceFile::SourceFile(std::string path, std::string text) : path_(std::move(path)), text_(std::move(text)) {
    line_starts_.push_back(0);
    for (std::uint32_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            line_starts_.push_back(i + 1);
        }
    }
}

std::unique_ptr<SourceFile> SourceFile::combine(std::string path, const std::vector<Part>& parts) {
    std::string text;
    std::vector<Segment> segments;
    std::uint32_t line = 1;
    for (const Part& p : parts) {
        segments.push_back(Segment{static_cast<std::uint32_t>(text.size()), line, p.path});
        text += p.text;
        text += '\n';
        for (char c : p.text) line += c == '\n' ? 1u : 0u;
        line += 1;
    }
    auto out = std::make_unique<SourceFile>(std::move(path), std::move(text));
    out->segments_ = std::move(segments);
    return out;
}

Location SourceFile::location(std::uint32_t offset) const {
    LineCol lc = line_col(offset);
    if (segments_.empty()) return {path_, lc.line, lc.column};
    auto it = std::upper_bound(segments_.begin(), segments_.end(), offset,
                               [](std::uint32_t o, const Segment& s) { return o < s.offset; });
    const Segment& seg = it == segments_.begin() ? segments_.front() : *(it - 1);
    return {seg.path, lc.line - seg.first_line + 1, lc.column};
}

std::string_view SourceFile::part_text(std::string_view path) const {
    if (segments_.empty()) return path == path_ ? std::string_view(text_) : std::string_view();
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        if (segments_[i].path != path) continue;
        const std::uint32_t begin = segments_[i].offset;
        // `combine` adds a newline after each part.
        const std::uint32_t end = i + 1 < segments_.size() ? segments_[i + 1].offset - 1
                                                           : static_cast<std::uint32_t>(text_.size()) - 1;
        return std::string_view(text_).substr(begin, end > begin ? end - begin : 0);
    }
    return {};
}

LineCol SourceFile::line_col(std::uint32_t offset) const {
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    auto line = static_cast<std::uint32_t>(it - line_starts_.begin()); // 1-based
    return {line, offset - line_starts_[line - 1] + 1};
}

std::string_view SourceFile::line_text(std::uint32_t line) const {
    if (line == 0 || line > line_starts_.size()) {
        return {};
    }
    std::uint32_t begin = line_starts_[line - 1];
    std::uint32_t end = line < line_starts_.size() ? line_starts_[line] - 1 : static_cast<std::uint32_t>(text_.size());
    std::string_view text = std::string_view(text_).substr(begin, end - begin);
    if (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace pluma

#include "cuervo/vector/xml.hpp"

#include <algorithm>
#include <cstdint>
#include <format>

namespace cuervo::vector::xml {

const std::string* Node::attribute(std::string_view key) const {
    for (const auto& [k, v] : attributes) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

namespace {

constexpr int max_depth = 256; // deeper nesting is surely hostile input

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_name_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == ':' || c == '-' || c == '.' || u >= 0x80;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool run(Document& out, std::string* error) {
        error_ = error;
        // UTF-8 byte order mark.
        if (s_.starts_with("\xEF\xBB\xBF")) {
            i_ = 3;
        }
        while (true) {
            skip_space();
            if (i_ >= s_.size()) {
                break;
            }
            if (s_.compare(i_, 4, "<!--") == 0) {
                if (!skip_past("-->")) return fail("unterminated comment");
            } else if (s_.compare(i_, 2, "<?") == 0) {
                if (!skip_past("?>")) return fail("unterminated processing instruction");
            } else if (s_.compare(i_, 9, "<!DOCTYPE") == 0) {
                if (!skip_doctype()) return fail("unterminated DOCTYPE");
            } else if (s_[i_] == '<') {
                if (out.root) return fail("more than one root element");
                out.root = std::make_unique<Node>();
                if (!element(*out.root, 0)) return false;
            } else {
                return fail("text outside the root element");
            }
        }
        if (!out.root) return fail("no root element");
        return true;
    }

private:
    bool fail(std::string_view message) {
        if (error_) {
            const auto line = 1 + std::count(s_.begin(), s_.begin() + static_cast<std::ptrdiff_t>(std::min(i_, s_.size())), '\n');
            *error_ = std::format("XML line {}: {}", line, message);
        }
        return false;
    }

    void skip_space() {
        while (i_ < s_.size() && is_space(s_[i_])) ++i_;
    }

    bool skip_past(std::string_view end) {
        const std::size_t at = s_.find(end, i_);
        if (at == std::string_view::npos) return false;
        i_ = at + end.size();
        return true;
    }

    bool skip_doctype() {
        int bracket = 0;
        for (; i_ < s_.size(); ++i_) {
            if (s_[i_] == '[') ++bracket;
            else if (s_[i_] == ']') --bracket;
            else if (s_[i_] == '>' && bracket <= 0) {
                ++i_;
                return true;
            }
        }
        return false;
    }

    std::string name() {
        const std::size_t start = i_;
        while (i_ < s_.size() && is_name_char(s_[i_])) ++i_;
        return std::string(s_.substr(start, i_ - start));
    }

    // Decodes entities in [begin, end).
    bool decode(std::string_view raw, std::string& out) {
        out.reserve(out.size() + raw.size());
        for (std::size_t k = 0; k < raw.size(); ++k) {
            if (raw[k] != '&') {
                out += raw[k];
                continue;
            }
            const std::size_t semi = raw.find(';', k);
            if (semi == std::string_view::npos || semi - k > 12) {
                out += '&'; // lenient: a stray ampersand
                continue;
            }
            const std::string_view entity = raw.substr(k + 1, semi - k - 1);
            if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "amp") out += '&';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (entity.starts_with("#x") || entity.starts_with("#X")) {
                append_utf8(out, static_cast<std::uint32_t>(std::strtoul(std::string(entity.substr(2)).c_str(), nullptr, 16)));
            } else if (entity.starts_with("#")) {
                append_utf8(out, static_cast<std::uint32_t>(std::strtoul(std::string(entity.substr(1)).c_str(), nullptr, 10)));
            } else {
                // Unknown (DTD-defined) entity: keep it verbatim.
                out += raw.substr(k, semi - k + 1);
            }
            k = semi;
        }
        return true;
    }

    bool element(Node& node, int depth) {
        if (depth > max_depth) return fail("elements nested too deeply");
        ++i_; // '<'
        node.name = name();
        if (node.name.empty()) return fail("expected an element name");
        if (node.name.starts_with("svg:")) node.name.erase(0, 4);

        // Attributes.
        while (true) {
            skip_space();
            if (i_ >= s_.size()) return fail("unterminated start tag");
            if (s_[i_] == '/') {
                if (s_.compare(i_, 2, "/>") != 0) return fail("expected '/>'");
                i_ += 2;
                return true; // empty element
            }
            if (s_[i_] == '>') {
                ++i_;
                break;
            }
            std::string key = name();
            if (key.empty()) return fail("expected an attribute name");
            skip_space();
            if (i_ >= s_.size() || s_[i_] != '=') return fail(std::format("attribute '{}' has no value", key));
            ++i_;
            skip_space();
            if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) return fail("expected a quoted attribute value");
            const char quote = s_[i_++];
            const std::size_t end = s_.find(quote, i_);
            if (end == std::string_view::npos) return fail("unterminated attribute value");
            std::string value;
            decode(s_.substr(i_, end - i_), value);
            i_ = end + 1;
            node.attributes.emplace_back(std::move(key), std::move(value));
        }

        // Content.
        while (true) {
            if (i_ >= s_.size()) return fail(std::format("<{}> is never closed", node.name));
            if (s_.compare(i_, 2, "</") == 0) {
                i_ += 2;
                std::string closing = name();
                if (closing.starts_with("svg:")) closing.erase(0, 4);
                if (closing != node.name) return fail(std::format("</{}> closes <{}>", closing, node.name));
                skip_space();
                if (i_ >= s_.size() || s_[i_] != '>') return fail("expected '>'");
                ++i_;
                return true;
            }
            if (s_.compare(i_, 4, "<!--") == 0) {
                if (!skip_past("-->")) return fail("unterminated comment");
            } else if (s_.compare(i_, 9, "<![CDATA[") == 0) {
                const std::size_t end = s_.find("]]>", i_ + 9);
                if (end == std::string_view::npos) return fail("unterminated CDATA");
                node.text.append(s_.substr(i_ + 9, end - i_ - 9));
                i_ = end + 3;
            } else if (s_.compare(i_, 2, "<?") == 0) {
                if (!skip_past("?>")) return fail("unterminated processing instruction");
            } else if (s_[i_] == '<') {
                auto child = std::make_unique<Node>();
                child->parent = &node;
                if (!element(*child, depth + 1)) return false;
                node.children.push_back(std::move(child));
            } else {
                const std::size_t end = std::min(s_.find('<', i_), s_.size());
                decode(s_.substr(i_, end - i_), node.text);
                i_ = end;
            }
        }
    }

    std::string_view s_;
    std::size_t i_ = 0;
    std::string* error_ = nullptr;
};

} // namespace

bool parse(std::string_view text, Document& out, std::string* error) {
    out.root.reset();
    Parser parser(text);
    if (!parser.run(out, error)) {
        out.root.reset();
        return false;
    }
    return true;
}

} // namespace cuervo::vector::xml

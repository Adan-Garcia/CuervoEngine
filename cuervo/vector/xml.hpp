#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Minimal non-validating XML parser for SVG import: elements, attributes,
// character data, CDATA, comments, processing instructions, DOCTYPE (with an
// internal subset, skipped) and the predefined + numeric entities. Namespace
// prefixes are kept verbatim ("xlink:href"); element names drop an "svg:"
// prefix. No external entities, so untrusted files can't make it fetch anything.
namespace cuervo::vector::xml {

struct Node {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::unique_ptr<Node>> children;
    std::string text; // concatenated character data directly inside this element
    Node* parent = nullptr;

    // Attribute value, or nullptr.
    const std::string* attribute(std::string_view key) const;
};

struct Document {
    std::unique_ptr<Node> root;
};

// Returns false (and sets `error` with a line number) on malformed input.
bool parse(std::string_view text, Document& out, std::string* error = nullptr);

} // namespace cuervo::vector::xml

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "pluma/value.hpp"
#include "pluma/vm.hpp"

// Reading a running game's node tree for the debugger panel (DESIGN.md
// 4.13): children, class, name and every field's value as text, read
// straight from the instances without running script code (VM::describe),
// so inspecting never changes the game.
namespace cuervo::editor {

struct LiveField {
    std::string name;
    std::string type;
    std::string value;
};

// The node's `children` (borrowed values), or none if it isn't a Node.
std::vector<pluma::Value> live_children(const pluma::Value& node);
std::string live_class(const pluma::Value& node);
std::string live_name(const pluma::Value& node);
// Every field but the tree links (parent, children), in declaration order.
std::vector<LiveField> live_fields(pluma::VM& vm, const pluma::Value& node, std::size_t max_length = 120);
// Nodes in the tree under (and including) `root`.
std::size_t live_count(const pluma::Value& root);

} // namespace cuervo::editor

#pragma once

#include <string>
#include <vector>

#include "cuervo/scene/scene_file.hpp"
#include "pluma/vm.hpp"

// Converts between scene files (cuervo/scene/scene_file.hpp) and live script
// node trees (cuervo/script/prelude/scene.plm), DESIGN.md 4.12.
//
// Saving walks `Node.children` from a root and records, per node, its class,
// its `name` and every `@export` field (LANGUAGE.md 9.13) except `name`.
// Nodes with `transient = true` (children a script creates itself, e.g. in
// `ready()`) are skipped with their subtrees.
//
// Instantiating creates each node with its field initializers, then runs
// `init()` if the class has one without arguments, then assigns the saved
// fields, then attaches it to its parent with `add_child` (so `ready()` runs
// only when the caller adds the root to the tree). Saved fields are matched
// by name like hot reload (DESIGN.md 5.13): a field that no longer exists,
// is no longer exported or whose declared type changed keeps its default,
// with a warning.
namespace cuervo::script {

scene::SceneDoc save_tree(pluma::VM& vm, const pluma::Value& root, std::vector<std::string>& warnings);

// Builds the tree; `out` gets the (owned) root. Unknown classes and classes
// that aren't Nodes are errors (a panic in the VM); field mismatches are
// warnings.
// `nodes` (optional) receives every node created, in document order (owned
// references: release them), so a caller can map file nodes to live objects
// (the editor's scene view picks and inspects through it).
pluma::RunResult instantiate_tree(pluma::VM& vm, const scene::SceneDoc& doc, pluma::Value* out,
                                 std::vector<std::string>& warnings, std::vector<pluma::Value>* nodes = nullptr);

} // namespace cuervo::script

#pragma once

// Acyclicity of types (DESIGN.md 5.10: "types the compiler proves acyclic are
// never traced"). Shared by the bytecode compiler and, later, the AOT-to-C
// emitter: both mark objects of acyclic types Untraced.
//
// The analysis runs on a graph whose nodes are the heap types: classes,
// structs (boxed values), payload enums, `List<T>` per element type, and ANY,
// which stands for closures, cells, tasks, signals, trait- and
// generic-typed values: they can hold anything, including the object that
// holds them. A node has an edge to every heap type a strong field (or
// payload, or list element) of it can hold; a field of class type `D` can
// hold any subclass of `D`. `weak var` fields hold no strong reference, and
// references to manual objects are not counted, so neither is an edge.
//
// A type is acyclic iff its node is on no cycle and cannot reach ANY. With
// `honor_promises` (release), an `@acyclic` class counts as acyclic and its
// outgoing edges are dropped; in development the promise is checked instead:
// the class is analyzed normally, and if it is not provably acyclic its
// instances are traced with objflag::Promised so that the collector reports
// one found in a garbage cycle.
//
// Types added later (another REPL input, a new subclass) can make an earlier
// verdict wrong for objects already created; they then leak rather than
// being collected, which is safe (an untraced object only ever keeps others
// alive).

#include <map>
#include <vector>

#include "pluma/sema.hpp"

namespace pluma {

class AcyclicAnalysis {
public:
    // Sets RecordInfo::acyclic and EnumInfo::acyclic for every type in
    // `program`.
    void run(Program& program, bool honor_promises);
    // Can an object holding values of type `t` (a List<t>, a cell of a
    // captured `t` variable) never be part of a cycle?
    bool holder_acyclic(const Type* t) const;
    // Development: an `@acyclic` class that is not provably acyclic.
    bool unproven_promise(const ClassInfo* c) const;

private:
    int node_of(const void* decl) const;
    void targets(const Type* t, std::vector<int>& out);
    int list_node(const Type* element);

    Program* program_ = nullptr;
    bool honor_ = false;
    std::vector<std::vector<int>> edges_;
    std::map<const void*, int> ids_;           // ClassInfo / StructInfo / EnumInfo -> node
    std::map<const Type*, int> lists_;         // element type -> List node
    std::vector<bool> acyclic_;                // by node
    std::map<const ClassInfo*, bool> unproven_;
};

} // namespace pluma

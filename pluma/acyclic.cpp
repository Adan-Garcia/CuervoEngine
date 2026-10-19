#include "pluma/acyclic.hpp"

#include <algorithm>
#include <functional>

namespace pluma {

namespace {
constexpr int any_node = 0;
} // namespace

int AcyclicAnalysis::node_of(const void* decl) const {
    auto it = ids_.find(decl);
    return it != ids_.end() ? it->second : any_node;
}

int AcyclicAnalysis::list_node(const Type* element) {
    auto it = lists_.find(element);
    if (it != lists_.end()) return it->second;
    int id = static_cast<int>(edges_.size());
    edges_.emplace_back();
    lists_.emplace(element, id);
    std::vector<int> t;
    targets(element, t);
    edges_[static_cast<std::size_t>(id)] = std::move(t);
    return id;
}

// The nodes a strong slot of type `t` can point at.
void AcyclicAnalysis::targets(const Type* t, std::vector<int>& out) {
    if (t == nullptr) return;
    switch (t->tag) {
        case TypeTag::Class: {
            const ClassInfo* d = t->cls();
            for (const auto& k : program_->classes) {
                // Manual instances are reached through uncounted references.
                if (!k->manual && k->is_subclass_of(d)) out.push_back(node_of(k.get()));
            }
            return;
        }
        case TypeTag::Struct: out.push_back(node_of(t->st())); return;
        case TypeTag::Enum:
            if (!t->en()->plain) out.push_back(node_of(t->en()));
            return;
        case TypeTag::List: out.push_back(list_node(t->args[0])); return;
        // The 9.2a containers hold their element types like a list does
        // (a Map both its keys and values); a PriorityQueue also holds its
        // ordering closure.
        case TypeTag::Map:
        case TypeTag::Set:
        case TypeTag::Deque:
        case TypeTag::Array:
        case TypeTag::Result:
            for (const Type* a : t->args) out.push_back(list_node(a));
            return;
        case TypeTag::PQueue:
            out.push_back(list_node(t->args[0]));
            out.push_back(any_node);
            return;
        case TypeTag::Nullable: targets(t->args[0], out); return;
        case TypeTag::Fn:
        case TypeTag::Trait:
        case TypeTag::Param:
        case TypeTag::Task:
        case TypeTag::Signal: out.push_back(any_node); return;
        default: return; // numbers, bool, str, name, vectors, plain enums: no references
    }
}

void AcyclicAnalysis::run(Program& program, bool honor_promises) {
    program_ = &program;
    honor_ = honor_promises;
    edges_.assign(1, {}); // ANY
    ids_.clear();
    lists_.clear();
    unproven_.clear();
    auto add = [&](const void* decl) {
        ids_[decl] = static_cast<int>(edges_.size());
        edges_.emplace_back();
    };
    for (const auto& c : program.classes) add(c.get());
    for (const auto& s : program.structs) add(s.get());
    for (const auto& e : program.enums) {
        if (!e->plain) add(e.get());
    }
    auto record_edges = [&](const RecordInfo* r) {
        std::vector<int> t;
        for (const FieldInfo* f : r->fields) {
            if (!f->weak) targets(f->type, t);
        }
        edges_[static_cast<std::size_t>(node_of(r))] = std::move(t);
    };
    for (const auto& c : program.classes) {
        if (c->manual || (honor_ && c->promised_acyclic)) continue;
        record_edges(c.get());
    }
    for (const auto& s : program.structs) record_edges(s.get());
    for (const auto& e : program.enums) {
        if (e->plain) continue;
        std::vector<int> t;
        for (const VariantInfo& v : e->variants) {
            for (const Type* ft : v.field_types) targets(ft, t);
        }
        edges_[static_cast<std::size_t>(node_of(e.get()))] = std::move(t);
    }

    // Nodes on a cycle: strongly connected components (Tarjan).
    const std::size_t n = edges_.size();
    std::vector<int> index(n, -1);
    std::vector<int> low(n, 0);
    std::vector<bool> on_stack(n, false);
    std::vector<int> stack;
    std::vector<bool> cyclic(n, false);
    int counter = 0;
    std::function<void(int)> strongconnect = [&](int v) {
        auto vi = static_cast<std::size_t>(v);
        index[vi] = low[vi] = counter++;
        stack.push_back(v);
        on_stack[vi] = true;
        for (int w : edges_[vi]) {
            auto wi = static_cast<std::size_t>(w);
            if (w == v) cyclic[vi] = true; // self edge
            if (index[wi] < 0) {
                strongconnect(w);
                low[vi] = std::min(low[vi], low[wi]);
            } else if (on_stack[wi]) {
                low[vi] = std::min(low[vi], index[wi]);
            }
        }
        if (low[vi] != index[vi]) return;
        std::vector<int> comp;
        int w = -1;
        do {
            w = stack.back();
            stack.pop_back();
            on_stack[static_cast<std::size_t>(w)] = false;
            comp.push_back(w);
        } while (w != v);
        if (comp.size() > 1) {
            for (int c : comp) cyclic[static_cast<std::size_t>(c)] = true;
        }
    };
    for (std::size_t v = 0; v < n; ++v) {
        if (index[v] < 0) strongconnect(static_cast<int>(v));
    }

    // Nodes that reach ANY (which can point back at anything).
    std::vector<std::vector<int>> rev(n);
    for (std::size_t v = 0; v < n; ++v) {
        for (int w : edges_[v]) rev[static_cast<std::size_t>(w)].push_back(static_cast<int>(v));
    }
    std::vector<bool> reach_any(n, false);
    std::vector<int> work{any_node};
    reach_any[any_node] = true;
    while (!work.empty()) {
        int v = work.back();
        work.pop_back();
        for (int u : rev[static_cast<std::size_t>(v)]) {
            if (!reach_any[static_cast<std::size_t>(u)]) {
                reach_any[static_cast<std::size_t>(u)] = true;
                work.push_back(u);
            }
        }
    }
    acyclic_.assign(n, false);
    for (std::size_t v = 1; v < n; ++v) acyclic_[v] = !cyclic[v] && !reach_any[v];

    for (const auto& c : program.classes) {
        bool proven = acyclic_[static_cast<std::size_t>(node_of(c.get()))];
        c->acyclic = proven || (honor_ && c->promised_acyclic);
        if (!honor_ && c->promised_acyclic && !proven) unproven_[c.get()] = true;
    }
    for (const auto& s : program.structs) s->acyclic = acyclic_[static_cast<std::size_t>(node_of(s.get()))];
    for (const auto& e : program.enums) {
        e->acyclic = e->plain || acyclic_[static_cast<std::size_t>(node_of(e.get()))];
    }
}

bool AcyclicAnalysis::holder_acyclic(const Type* t) const {
    if (auto it = lists_.find(t); it != lists_.end()) {
        // A container node made after run() (by an earlier query) has no
        // answer: stay traced.
        auto id = static_cast<std::size_t>(it->second);
        return id < acyclic_.size() && acyclic_[id];
    }
    // A holder no field can reach: it is on a cycle only through what it
    // holds, so it is acyclic if all of that is.
    std::vector<int> out;
    const_cast<AcyclicAnalysis*>(this)->targets(t, out);
    for (int v : out) {
        if (static_cast<std::size_t>(v) >= acyclic_.size() || !acyclic_[static_cast<std::size_t>(v)]) return false;
    }
    return true;
}

bool AcyclicAnalysis::unproven_promise(const ClassInfo* c) const { return unproven_.count(c) != 0; }

} // namespace pluma

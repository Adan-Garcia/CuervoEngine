#include "editor/actions.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <format>
#include <numeric>

#include "cuervo/math/dmath.hpp"

namespace cuervo::editor {

namespace {

constexpr int impossible = INT_MIN / 2;

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

// One word of the query against the text: best-scoring subsequence match.
bool match_word(std::string_view q, std::string_view t, int& score, std::vector<std::size_t>& positions) {
    const std::size_t m = q.size();
    const std::size_t n = t.size();
    if (m == 0) return true;
    if (m > n) return false;
    auto bonus = [&](std::size_t i) {
        int b = 16;
        if (i == 0) {
            b += 24;
        } else if (!alnum(t[i - 1])) {
            b += 20;
        } else if (std::islower(static_cast<unsigned char>(t[i - 1])) != 0 &&
                   std::isupper(static_cast<unsigned char>(t[i])) != 0) {
            b += 14;
        }
        return b;
    };
    std::vector<std::vector<int>> dp(m, std::vector<int>(n, impossible));
    std::vector<std::vector<int>> from(m, std::vector<int>(n, -1));
    for (std::size_t j = 0; j < m; ++j) {
        for (std::size_t i = j; i < n; ++i) {
            if (lower(t[i]) != lower(q[j])) continue;
            if (j == 0) {
                dp[j][i] = bonus(i) - static_cast<int>(std::min<std::size_t>(i, 12));
                continue;
            }
            int best = impossible;
            int best_k = -1;
            for (std::size_t k = j - 1; k < i; ++k) {
                if (dp[j - 1][k] == impossible) continue;
                const int gap = k + 1 == i ? 20 : -static_cast<int>(std::min<std::size_t>(i - k - 1, 8));
                if (dp[j - 1][k] + gap > best) {
                    best = dp[j - 1][k] + gap;
                    best_k = static_cast<int>(k);
                }
            }
            if (best_k >= 0) {
                dp[j][i] = best + bonus(i);
                from[j][i] = best_k;
            }
        }
    }
    int best = impossible;
    int at = -1;
    for (std::size_t i = m - 1; i < n; ++i) {
        if (dp[m - 1][i] > best) {
            best = dp[m - 1][i];
            at = static_cast<int>(i);
        }
    }
    if (at < 0) return false;
    score += best;
    for (std::size_t j = m; j-- > 0;) {
        positions.push_back(static_cast<std::size_t>(at));
        at = from[j][static_cast<std::size_t>(at)];
    }
    return true;
}

} // namespace

bool fuzzy_match(std::string_view query, std::string_view text, FuzzyMatch& out) {
    out = {};
    std::size_t pos = 0;
    bool any = false;
    while (pos < query.size()) {
        while (pos < query.size() && query[pos] == ' ') ++pos;
        const std::size_t end = std::min(query.find(' ', pos), query.size());
        if (end > pos) {
            any = true;
            if (!match_word(query.substr(pos, end - pos), text, out.score, out.positions)) return false;
        }
        pos = end;
    }
    (void)any;
    std::sort(out.positions.begin(), out.positions.end());
    out.positions.erase(std::unique(out.positions.begin(), out.positions.end()), out.positions.end());
    return true;
}

Action& ActionRegistry::add(Action action) {
    for (Action& a : actions_) {
        if (a.id == action.id) {
            a = std::move(action);
            return a;
        }
    }
    actions_.push_back(std::move(action));
    return actions_.back();
}

const Action* ActionRegistry::find(std::string_view id) const {
    for (const Action& a : actions_) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

std::string ActionRegistry::label(const Action& a) {
    return a.category.empty() ? a.title : std::format("{}: {}", a.category, a.title);
}

std::vector<ActionRegistry::Hit> ActionRegistry::search(std::string_view query) const {
    struct Ranked {
        Hit hit;
        std::size_t order;
        std::size_t recent;
        std::size_t length;
    };
    std::vector<Ranked> ranked;
    for (std::size_t i = 0; i < actions_.size(); ++i) {
        const Action& a = actions_[i];
        const std::string text = label(a);
        FuzzyMatch m;
        if (!fuzzy_match(query, text, m)) continue;
        const auto r = std::find(recent_.begin(), recent_.end(), a.id);
        ranked.push_back({{&a, m.score, enabled(a), std::move(m.positions)}, i,
                          r == recent_.end() ? recent_.size() : static_cast<std::size_t>(r - recent_.begin()),
                          text.size()});
    }
    const bool empty_query = query.find_first_not_of(' ') == std::string_view::npos;
    std::stable_sort(ranked.begin(), ranked.end(), [&](const Ranked& a, const Ranked& b) {
        if (a.hit.enabled != b.hit.enabled) return a.hit.enabled;
        if (!empty_query) {
            if (a.hit.score != b.hit.score) return a.hit.score > b.hit.score;
            if (a.length != b.length) return a.length < b.length;
        }
        if (a.recent != b.recent) return a.recent < b.recent;
        return a.order < b.order;
    });
    std::vector<Hit> out;
    out.reserve(ranked.size());
    for (Ranked& r : ranked) out.push_back(std::move(r.hit));
    return out;
}

void ActionRegistry::note_used(std::string_view id) {
    std::erase(recent_, std::string(id));
    recent_.insert(recent_.begin(), std::string(id));
    if (recent_.size() > 12) recent_.resize(12);
}

bool ActionRegistry::run(std::string_view id) {
    const Action* a = find(id);
    if (a == nullptr || !enabled(*a) || !a->run) return false;
    note_used(id);
    return a->run();
}

// ------------------------------------------------------------ the document's actions

void add_document_actions(ActionRegistry& registry, EditorDocument& doc,
                          std::function<std::vector<scene::Field>(std::string_view)> initial_fields) {
    EditorDocument* d = &doc;
    auto has_target = [d] { return !d->transform_targets().empty(); };
    auto add = [&](std::string id, std::string title, std::string category, std::string shortcut,
                   std::function<bool()> enabled, std::function<bool()> run) {
        registry.add({std::move(id), std::move(title), std::move(category), std::move(shortcut), std::move(enabled),
                      std::move(run)});
    };
    constexpr float degrees15 = 0.2617994f; // 15 degrees in radians
    add("edit.undo", "Undo", "Edit", "Ctrl+Z", [d] { return d->stack().can_undo(); }, [d] { return d->undo(); });
    add("edit.redo", "Redo", "Edit", "Ctrl+Shift+Z", [d] { return d->stack().can_redo(); }, [d] { return d->redo(); });
    add("edit.duplicate", "Duplicate Selection", "Edit", "Ctrl+D", has_target, [d] { return d->duplicate_selection(); });
    add("edit.delete", "Delete Selection", "Edit", "Del", has_target, [d] { return d->remove_selection(); });
    add("edit.group", "Group Selection", "Edit", "Ctrl+G", has_target, [d] { return d->group_selection() != 0; });
    add("select.root", "Select Root", "Select", "", {}, [d] {
        d->select(d->model().root());
        return true;
    });
    add("select.parent", "Select Parent", "Select", "",
        [d] {
            const ModelNode* n = d->model().get(d->selection());
            return n != nullptr && n->parent != 0;
        },
        [d] {
            d->select(d->model().get(d->selection())->parent);
            return true;
        });
    add("select.children", "Select Children", "Select", "",
        [d] {
            const ModelNode* n = d->model().get(d->selection());
            return n != nullptr && !n->children.empty();
        },
        [d] {
            d->select_set(d->model().get(d->selection())->children);
            return true;
        });
    add("select.siblings", "Select Siblings", "Select", "",
        [d] {
            const ModelNode* n = d->model().get(d->selection());
            return n != nullptr && n->parent != 0;
        },
        [d] {
            d->select_set(d->model().get(d->model().get(d->selection())->parent)->children);
            return true;
        });
    add("select.all", "Select All Nodes", "Select", "Ctrl+Shift+A", {}, [d] {
        std::vector<NodeId> all = d->model().preorder();
        d->select_set(all);
        return true;
    });
    add("select.none", "Clear Selection", "Select", "", {}, [d] {
        d->select_clear();
        return true;
    });
    for (int step : {-1, 1}) {
        const bool next = step > 0;
        auto target = [d, step]() -> NodeId {
            const ModelNode* n = d->model().get(d->selection());
            if (n == nullptr || n->parent == 0) return 0;
            const std::vector<NodeId>& kids = d->model().get(n->parent)->children;
            const std::size_t i = d->model().index_in_parent(n->id);
            if (step < 0) return i == 0 ? 0 : kids[i - 1];
            return i + 1 >= kids.size() ? 0 : kids[i + 1];
        };
        add(next ? "select.next_sibling" : "select.previous_sibling", next ? "Select Next Sibling" : "Select Previous Sibling",
            "Select", "", [target] { return target() != 0; }, [d, target] {
                d->select(target());
                return true;
            });
    }
    add("edit.move_up", "Move Up in Tree", "Edit", "",
        [d] {
            const ModelNode* n = d->model().get(d->selection());
            return n != nullptr && n->parent != 0 && d->model().index_in_parent(n->id) > 0;
        },
        [d] {
            const ModelNode* n = d->model().get(d->selection());
            return d->reparent(n->id, n->parent, d->model().index_in_parent(n->id) - 1);
        });
    add("edit.move_down", "Move Down in Tree", "Edit", "",
        [d] {
            const ModelNode* n = d->model().get(d->selection());
            return n != nullptr && n->parent != 0 &&
                   d->model().index_in_parent(n->id) + 1 < d->model().get(n->parent)->children.size();
        },
        [d] {
            const ModelNode* n = d->model().get(d->selection());
            return d->reparent(n->id, n->parent, d->model().index_in_parent(n->id) + 1);
        });
    struct Nudge {
        const char* id;
        const char* title;
        vec2 delta;
    };
    for (const Nudge& nudge : {Nudge{"transform.nudge_left", "Nudge Selection Left", {-1.0f, 0.0f}},
                               Nudge{"transform.nudge_right", "Nudge Selection Right", {1.0f, 0.0f}},
                               Nudge{"transform.nudge_up", "Nudge Selection Up", {0.0f, -1.0f}},
                               Nudge{"transform.nudge_down", "Nudge Selection Down", {0.0f, 1.0f}},
                               Nudge{"transform.nudge_left10", "Nudge Selection Left by 10", {-10.0f, 0.0f}},
                               Nudge{"transform.nudge_right10", "Nudge Selection Right by 10", {10.0f, 0.0f}},
                               Nudge{"transform.nudge_up10", "Nudge Selection Up by 10", {0.0f, -10.0f}},
                               Nudge{"transform.nudge_down10", "Nudge Selection Down by 10", {0.0f, 10.0f}}}) {
        const vec2 delta = nudge.delta;
        add(nudge.id, nudge.title, "Transform", "", has_target, [d, delta] {
            const bool ok = d->move_selection_by(delta, false);
            d->seal();
            return ok;
        });
    }
    add("transform.rotate_cw", "Rotate Selection 15 Degrees Clockwise", "Transform", "", has_target, [d] {
        const bool ok = d->rotate_selection_by(degrees15, std::nullopt, false);
        d->seal();
        return ok;
    });
    add("transform.rotate_ccw", "Rotate Selection 15 Degrees Counterclockwise", "Transform", "", has_target,
        [d] {
            const bool ok = d->rotate_selection_by(-degrees15, std::nullopt, false);
            d->seal();
            return ok;
        });
    add("transform.scale_up", "Scale Selection Up by 25 Percent", "Transform", "", has_target, [d] {
        const bool ok = d->scale_selection_by({1.25f, 1.25f}, std::nullopt, false);
        d->seal();
        return ok;
    });
    add("transform.scale_down", "Scale Selection Down by 20 Percent", "Transform", "", has_target, [d] {
        const bool ok = d->scale_selection_by({0.8f, 0.8f}, std::nullopt, false);
        d->seal();
        return ok;
    });
    add("transform.reset", "Reset Rotation and Scale of Selection", "Transform", "", has_target, [d] {
        bool ok = true;
        for (NodeId id : d->transform_targets()) {
            ok = d->reset_field(id, "rotation") && d->reset_field(id, "scale") && ok;
        }
        d->seal();
        return ok;
    });
    // "Add Node: <Class>" for every node class of the project and the prelude.
    if (d->registry() != nullptr) {
        for (const ClassSpec& c : d->registry()->classes()) {
            const std::string name = c.name;
            add("node.add." + name, name, "Add Node", "", {}, [d, name, initial_fields] {
                const NodeId parent = d->selection() != 0 ? d->selection() : d->model().root();
                return d->add_child(parent, name, {}, initial_fields ? initial_fields(name) : std::vector<scene::Field>{}) != 0;
            });
        }
    }
}

} // namespace cuervo::editor

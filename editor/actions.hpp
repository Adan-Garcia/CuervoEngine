#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/editor_document.hpp"

// The editor's command registry (DESIGN.md 4.16 "Clarified (Milestone 8.3)"):
// every thing the editor can do is an Action with a stable id, a title for the
// command palette (Ctrl+Shift+P) and the shortcut it shows. Menus, the toolbar
// and the palette all run actions, and `--script-commands` runs the same ones
// (`palette <query>`), so what the palette does is tested headless.
// (editor/commands.* is the other kind of command: undo steps of the scene.)
namespace cuervo::editor {

struct Action {
    std::string id;       // "edit.duplicate": stable, for tests and saved layouts
    std::string title;    // "Duplicate Selection"
    std::string category; // "Edit"; palette rows read "Edit: Duplicate Selection"
    std::string shortcut; // "Ctrl+D", display only
    std::function<bool()> enabled; // empty = always
    std::function<bool()> run; // false: it failed (the reason is the document's error())
};

// Fuzzy matching, case-insensitive. Every space-separated word of `query` must
// occur in `text` as a subsequence; matches at the start, after a separator, on a
// camelCase boundary and next to the previous match score higher, so "dup sel"
// finds "Duplicate Selection" and "ts" prefers "Toggle Snap" over "Scene Settings".
struct FuzzyMatch {
    int score = 0;
    std::vector<std::size_t> positions; // matched characters of `text`, ascending
};
bool fuzzy_match(std::string_view query, std::string_view text, FuzzyMatch& out);

class ActionRegistry {
public:
    struct Hit {
        const Action* action = nullptr;
        int score = 0;
        bool enabled = true;
        std::vector<std::size_t> positions; // in label(): "category: title"
    };

    // Adds an action (a repeated id replaces the earlier one).
    Action& add(Action action);
    // Drops every action but keeps the recently-used order (the app rebuilds them when scripts reload).
    void reset_actions() { actions_.clear(); }
    const Action* find(std::string_view id) const;
    const std::vector<Action>& all() const { return actions_; }
    static std::string label(const Action& action);
    bool enabled(const Action& action) const { return !action.enabled || action.enabled(); }

    // Actions matching `query`, best first (ties: shorter, then most recently used,
    // then registration order). An empty query lists everything, recently used
    // first. Disabled actions are listed after the enabled ones.
    std::vector<Hit> search(std::string_view query) const;
    // Runs an enabled action by id and remembers it as recently used; false when it
    // is unknown, disabled or failed.
    bool run(std::string_view id);
    void note_used(std::string_view id);

private:
    std::vector<Action> actions_;
    std::vector<std::string> recent_; // most recent first
};

// The scene-editing actions every host has (undo, duplicate, group, select,
// nudge, rotate, scale, "Add Node: <Class>" for each class of `classes`), working
// on `doc`. The app adds the window's own (files, play, view, panels).
// `initial_fields` (optional) gives a new node's starting fields (the app puts a
// Node2D in the middle of the scene view).
void add_document_actions(ActionRegistry& registry, EditorDocument& doc,
                          std::function<std::vector<scene::Field>(std::string_view)> initial_fields = {});

} // namespace cuervo::editor

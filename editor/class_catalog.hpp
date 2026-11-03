#pragma once
// The Add Child picker's data (DESIGN.md 4.13): every node class grouped by family, with a one-line
// description, fuzzy filtering, the user's favorites and recents. No UI here; the RmlUi shell renders
// the groups and the ImGui editor keeps its flat list.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/class_registry.hpp"

namespace cuervo::editor {

struct CatalogEntry {
    std::string name;
    std::string base;
    std::string description;
    bool favorite = false;
    int score = 0;
    std::vector<std::size_t> positions; // matched characters of `name`
};

struct CatalogGroup {
    std::string title;
    std::vector<CatalogEntry> items;
};

// ';'-separated lists, as stored in [rml_layout].
struct CatalogUser {
    std::vector<std::string> recents;   // newest first
    std::vector<std::string> favorites;
    bool is_favorite(std::string_view name) const;
    void toggle_favorite(std::string_view name);
    void note_used(std::string_view name); // puts it first; keeps 6
    static CatalogUser parse(std::string_view recents, std::string_view favorites);
    std::string recents_text() const;
    std::string favorites_text() const;
};

// The family a class belongs to: "Core", "Drawing", "Physics", "Lighting", "UI", "Audio and animation",
// "Navigation", or "Project scripts" (is_project is true for classes defined by the project).
std::string catalog_group_of(const ClassRegistry& reg, const ClassSpec& cls, bool is_project);
// A short description; known engine classes have one, the rest say what they extend.
std::string catalog_description(const ClassSpec& cls);

// With an empty query: Favorites, Recent, then the families in a fixed order, each sorted by name.
// With a query: the families only (best matches first), favorites starred.
std::vector<CatalogGroup> build_catalog(const ClassRegistry& reg, std::string_view query, const CatalogUser& user,
                                        const std::function<bool(const std::string&)>& is_project_class);
// The entries in display order (what Up/Down walks); a class under Favorites or Recent is walked there and again in its family.
std::vector<const CatalogEntry*> catalog_flat(const std::vector<CatalogGroup>& groups);

} // namespace cuervo::editor

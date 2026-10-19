#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Saved layouts (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): which panels are
// shown and how the docks are divided, kept per user in
// <user data dir>/editor-layouts.txt. The editor uses ImGui's docking branch
// (4.16): a layout is the dock tree as ImGui's own ini text, plus three
// fractions that size the default arrangement of 4.13 when there is no text,
// and the set of hidden panels.
namespace cuervo::editor {

struct Layout {
    float left = 0.19f;   // width of the left dock, fraction of the window
    float right = 0.24f;  // width of the right dock
    float bottom = 0.24f; // height of the bottom dock (between the side docks)
    std::vector<std::string> hidden; // panel titles that are not shown
    // ImGui's ini text (dock tree + window placement, docking branch); empty for the
    // built-in layouts, which are built from the fractions with the DockBuilder API.
    std::string dock;

    bool is_hidden(std::string_view title) const;
    void set_hidden(std::string_view title, bool hide);
    // Brings the fractions into a usable range (docks can't swallow the scene view).
    void clamp();
    friend bool operator==(const Layout&, const Layout&) = default;
};

class LayoutStore {
public:
    static constexpr const char* default_name = "Default";

    // Starts with the built-in layouts: Default, Debugging (big bottom dock) and Minimal.
    LayoutStore();

    const std::string& active() const { return active_; }
    // The active layout's name, or "Default" if it was removed.
    void set_active(std::string name);
    const Layout& layout(std::string_view name) const;
    bool contains(std::string_view name) const { return layouts_.count(std::string(name)) != 0; }
    std::vector<std::string> names() const; // built-ins first, then the user's, sorted
    // Saves (or replaces) a named layout. The built-in names can't be overwritten.
    bool save_as(std::string name, const Layout& layout, std::string& error);
    bool remove(std::string_view name, std::string& error);
    static bool is_builtin(std::string_view name);

    // The current layout the window was last left in (restored at the next start).
    const Layout& current() const { return current_; }
    void set_current(const Layout& layout) { current_ = layout; current_.clamp(); }

    std::string serialize() const;
    // Replaces everything but the built-ins; false (with `error`) on malformed text,
    // leaving the store unchanged.
    bool parse(std::string_view text, std::string& error);
    bool load(const std::filesystem::path& file, std::string& error);
    bool save(const std::filesystem::path& file, std::string& error) const;
    // <user data dir>/editor-layouts.txt (CUERVO_USER_DIR overrides the folder).
    static std::filesystem::path user_file();

private:
    std::map<std::string, Layout> layouts_;
    std::string active_ = default_name;
    Layout current_;
};

} // namespace cuervo::editor

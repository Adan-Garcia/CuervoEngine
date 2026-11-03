#include "editor/class_catalog.hpp"

#include <algorithm>
#include <map>

#include "editor/actions.hpp"

namespace cuervo::editor {

namespace {

constexpr std::size_t max_recents = 6;

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t end = text.find(';', at);
        if (end == std::string_view::npos) end = text.size();
        if (end > at) out.emplace_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : ";") + s;
    return out;
}

const std::map<std::string, std::string, std::less<>>& descriptions() {
    static const std::map<std::string, std::string, std::less<>> d = {
        {"Node", "Empty node; groups children and runs a script."},
        {"Node2D", "A node with a position, rotation and scale."},
        {"Marker2D", "A named point other nodes can read."},
        {"Timer", "Counts down and signals when it fires."},
        {"CanvasLayer", "Draws its children in screen space, above the world."},
        {"SceneInstance", "Places another scene file inside this one."},
        {"Camera2D", "Decides which part of the world the game shows."},
        {"Sprite", "Draws a vector or image asset."},
        {"Sprite2D", "Draws an image, optionally a frame of a sheet."},
        {"AnimatedSprite2D", "Plays frame animations from a sheet."},
        {"NinePatchRect", "Stretches an image by its borders."},
        {"Text", "Draws a line or block of text."},
        {"Line2D", "Draws a polyline with a width."},
        {"Polygon2D", "Draws a filled polygon."},
        {"Emitter2D", "Spawns particles from a preset."},
        {"TileMapLayer", "A grid of tiles painted from a tile set."},
        {"Path", "A curve other nodes can follow."},
        {"StaticBody2D", "Solid that never moves."},
        {"RigidBody2D", "Solid moved by forces and collisions."},
        {"CharacterBody2D", "Solid moved by script, with slide and floor tests."},
        {"AnimatableBody2D", "Solid moved by script that pushes other bodies."},
        {"Area2D", "Detects bodies that overlap it."},
        {"Area", "Detects bodies that overlap it."},
        {"CollisionShape2D", "Gives a body or area a collision shape."},
        {"CollisionPolygon2D", "A collision shape from a polygon."},
        {"RayCast2D", "Reports what a ray hits."},
        {"ShapeCast2D", "Reports what a swept shape hits."},
        {"PointLight2D", "A light with a position and radius."},
        {"DirectionalLight2D", "A light from one direction, everywhere."},
        {"AmbientLight2D", "Base light level for the whole scene."},
        {"LightOccluder2D", "Blocks light and casts shadows."},
        {"AudioPlayer", "Plays a sound, positioned in the scene."},
        {"AnimationPlayer", "Plays animation clips on its parent's properties."},
        {"AnimationTree", "Blends and switches between animation clips."},
        {"NavigationAgent2D", "Finds a path across a navigation region."},
        {"NavigationRegion2D", "The walkable area agents path across."},
        {"Control", "Base of every UI node: anchors and size."},
        {"Container", "Lays out its Control children."},
        {"VBox", "Stacks children top to bottom."},
        {"HBox", "Places children left to right."},
        {"GridContainer", "Places children on a grid."},
        {"Margin", "Pads its child."},
        {"Panel", "A filled background for UI."},
        {"Center", "Centers its child."},
        {"ScrollContainer", "Scrolls a child larger than itself."},
        {"Label", "Static UI text."},
        {"Button", "A clickable button."},
        {"Toggle", "A checkbox or switch."},
        {"Slider", "Picks a number by dragging."},
        {"ProgressBar", "Shows a value between a minimum and maximum."},
        {"LineEdit", "A one-line text field."},
        {"Image", "Shows an image in a UI layout."},
        {"TextureRect", "Shows an image stretched to its rectangle."},
        {"OptionButton", "A button that opens a list of choices."},
        {"TabBar", "A row of tabs."},
        {"TabContainer", "Shows one child per tab."},
    };
    return d;
}

} // namespace

bool CatalogUser::is_favorite(std::string_view name) const {
    return std::find(favorites.begin(), favorites.end(), name) != favorites.end();
}

void CatalogUser::toggle_favorite(std::string_view name) {
    const auto it = std::find(favorites.begin(), favorites.end(), name);
    if (it != favorites.end()) favorites.erase(it);
    else favorites.emplace_back(name);
}

void CatalogUser::note_used(std::string_view name) {
    std::erase(recents, name);
    recents.insert(recents.begin(), std::string(name));
    if (recents.size() > max_recents) recents.resize(max_recents);
}

CatalogUser CatalogUser::parse(std::string_view r, std::string_view f) {
    CatalogUser u;
    u.recents = split(r);
    u.favorites = split(f);
    return u;
}

std::string CatalogUser::recents_text() const { return join(recents); }
std::string CatalogUser::favorites_text() const { return join(favorites); }

std::string catalog_group_of(const ClassRegistry& reg, const ClassSpec& cls, bool is_project) {
    if (is_project) return "Project scripts";
    const std::string& n = cls.name;
    if (reg.is_a(n, "Control")) return "UI";
    if (reg.is_a(n, "PhysicsBody2D") || reg.is_a(n, "Joint2D") || reg.is_a(n, "Shape2D") || n == "Area" || n == "Area2D" ||
        n == "CollisionShape2D" || n == "CollisionPolygon2D" || n == "RayCast2D" || n == "ShapeCast2D" || n == "TileMapCollision2D")
        return "Physics";
    if (n.find("Light") != std::string::npos) return "Lighting";
    if (n == "AudioPlayer" || n == "AnimationPlayer" || n == "AnimationTree") return "Audio and animation";
    if (n.starts_with("Navigation")) return "Navigation";
    if (n == "Sprite" || n == "Sprite2D" || n == "AnimatedSprite2D" || n == "NinePatchRect" || n == "Text" || n == "Line2D" ||
        n == "Polygon2D" || n == "Emitter2D" || n == "TileMapLayer" || n == "Path")
        return "Drawing";
    return "Core";
}

std::string catalog_description(const ClassSpec& cls) {
    const auto it = descriptions().find(cls.name);
    if (it != descriptions().end()) return it->second;
    return cls.base.empty() ? "Base node." : "Extends " + cls.base + ".";
}

std::vector<CatalogGroup> build_catalog(const ClassRegistry& reg, std::string_view query, const CatalogUser& user,
                                        const std::function<bool(const std::string&)>& is_project_class) {
    static const char* const order[] = {"Core", "Drawing", "Physics", "Lighting", "Audio and animation", "Navigation", "UI", "Project scripts"};
    std::map<std::string, CatalogGroup, std::less<>> by;
    const auto make = [&](const ClassSpec& c) {
        CatalogEntry e;
        e.name = c.name;
        e.base = c.base;
        e.description = catalog_description(c);
        e.favorite = user.is_favorite(c.name);
        return e;
    };
    for (const ClassSpec& c : reg.classes()) {
        CatalogEntry e = make(c);
        if (!query.empty()) {
            FuzzyMatch m;
            if (!fuzzy_match(query, c.name, m)) continue;
            e.score = m.score;
            e.positions = m.positions;
        }
        const std::string g = catalog_group_of(reg, c, is_project_class && is_project_class(c.name));
        by[g].title = g;
        by[g].items.push_back(std::move(e));
    }
    std::vector<CatalogGroup> out;
    if (query.empty()) {
        CatalogGroup fav{"Favorites", {}}, rec{"Recent", {}};
        for (const std::string& n : user.favorites) if (const ClassSpec* c = reg.find(n)) fav.items.push_back(make(*c));
        for (const std::string& n : user.recents) if (const ClassSpec* c = reg.find(n)) rec.items.push_back(make(*c));
        if (!fav.items.empty()) out.push_back(std::move(fav));
        if (!rec.items.empty()) out.push_back(std::move(rec));
    }
    for (const char* title : order) {
        const auto it = by.find(title);
        if (it == by.end()) continue;
        CatalogGroup g = std::move(it->second);
        if (query.empty()) std::sort(g.items.begin(), g.items.end(), [](const CatalogEntry& a, const CatalogEntry& b) { return a.name < b.name; });
        else std::stable_sort(g.items.begin(), g.items.end(), [](const CatalogEntry& a, const CatalogEntry& b) { return a.score > b.score; });
        out.push_back(std::move(g));
    }
    if (!query.empty()) {
        // Best group first when searching.
        std::stable_sort(out.begin(), out.end(), [](const CatalogGroup& a, const CatalogGroup& b) { return a.items.front().score > b.items.front().score; });
    }
    return out;
}

std::vector<const CatalogEntry*> catalog_flat(const std::vector<CatalogGroup>& groups) {
    std::vector<const CatalogEntry*> out;
    for (const CatalogGroup& g : groups) for (const CatalogEntry& e : g.items) out.push_back(&e);
    return out;
}

} // namespace cuervo::editor

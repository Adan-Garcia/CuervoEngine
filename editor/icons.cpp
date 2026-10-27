#include "editor/icons.hpp"

#include <format>
#include <map>

namespace cuervo::editor {

namespace {

// A circle as two arcs (path data), center (cx, cy), radius r.
std::string circ(float cx, float cy, float r) {
    return std::format("M{} {}a{} {} 0 1 0 {} 0a{} {} 0 1 0 {} 0z", cx - r, cy, r, r, 2 * r, r, r, -2 * r);
}

struct IconSource {
    const char* name;
    std::string shapes; // shapes separated by '|'; a leading '!' fills the shape
};

// Line art on a 24x24 grid, 1.5 px strokes with round caps and joins when drawn
// at 16 px. Shapes of the node icons are simple on purpose: they are read at 14
// to 16 px next to a label.
const std::vector<IconSource>& sources() {
    static const std::vector<IconSource> list = {
        // ---- node classes
        {"node", circ(12, 12, 6) + "|!" + circ(12, 12, 2)},
        {"node2d", "M5 4v15h15|" + circ(9.5f, 14.5f, 2.5f) + "|M12 12l5-5|M13 7h4v4"},
        {"scene_instance", "M9 4h11v11h-5|M4 9h11v11H4z"},
        {"canvas_layer", "M12 4l8 4-8 4-8-4z|M4 12l8 4 8-4|M4 16l8 4 8-4"},
        {"control", "M4 5h16v14H4z|M8 10h8|M8 14h5"},
        {"container", "M4 4h16v16H4z|M4 12h16|M12 4v16"},
        {"vbox", "M4 4h16v16H4z|M4 9.3h16|M4 14.7h16"},
        {"hbox", "M4 4h16v16H4z|M9.3 4v16|M14.7 4v16"},
        {"margin", "M3 3h18v18H3z|M8 8h8v8H8z"},
        {"panel", "M4 4h16v16H4z|M4 9h16"},
        {"center", "M4 4h16v16H4z|!" + circ(12, 12, 1.8f)},
        {"label", "M6 6h12|M12 6v13|M9 19h6"},
        {"button", "M6 7h12a3 3 0 0 1 3 3v4a3 3 0 0 1-3 3H6a3 3 0 0 1-3-3v-4a3 3 0 0 1 3-3z|M9 12h6"},
        {"toggle", "M8 7h8a5 5 0 0 1 0 10H8a5 5 0 0 1 0-10z|!" + circ(16, 12, 2.4f)},
        {"slider", "M4 12h16|!" + circ(10, 12, 3)},
        {"shape", circ(9, 9, 5) + "|M11 11h9v9h-9z"},
        {"path", "M4 16.5C4 6 12 6 12 12S20 18 20 7.5|" + circ(4, 17.5f, 1.5f) + "|" + circ(20, 6.5f, 1.5f)},
        {"sprite", "M4 5h16v14H4z|M4 16l5-5 4 4 3-3 4 4|!" + circ(9, 9, 1.5f)},
        {"text", "M5 19l7-14 7 14|M8 14h8"},
        {"camera", "M3 7h11v10H3z|M14 11l6-3v8l-6-3z"},
        {"timer", circ(12, 13, 8) + "|M12 8v5l3 2|M9 2.5h6"},
        {"area", "M4 9V4h5|M15 4h5v5|M20 15v5h-5|M9 20H4v-5|!" + circ(12, 12, 1.8f)},
        {"emitter", "!" + circ(6, 12, 2) + "|M11 12h9|M11 12l7-5|M11 12l7 5"},
        {"audio", "M4 9.5h4l5-4v13l-5-4H4z|M16 9a4 4 0 0 1 0 6|M18.5 6.5a8 8 0 0 1 0 11"},
        // ---- toolbar and commands
        {"select", "M5 3l13 8-6 1.5L9 19z"},
        {"move", "M12 3v18|M3 12h18|M9 6l3-3 3 3|M9 18l3 3 3-3|M6 9l-3 3 3 3|M18 9l3 3-3 3"},
        {"rotate", "M19 12a7 7 0 1 1-2.1-5|M19 4v4h-4"},
        {"scale", "M5 19h8v-8H5z|M13 11l6-6|M14 5h5v5"},
        {"play", "!M7 4.5v15l12-7.5z"},
        {"pause", "!M7 5h3.5v14H7z|!M13.5 5H17v14h-3.5z"},
        {"stop", "!M6 6h12v12H6z"},
        {"step", "!M5 5l9 7-9 7z|M18 5v14"},
        {"record", "!" + circ(12, 12, 6)},
        {"save", "M5 4h12l3 3v13H5z|M8 4v5h7V4|M8 20v-6h8v6"},
        {"open", "M3 7a1 1 0 0 1 1-1h5l2 2h8a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z"},
        {"new", "M6 3h8l4 4v14H6z|M14 3v4h4|M12 11v6|M9 14h6"},
        {"undo", "M8 6L3 11l5 5|M3 11h11a5 5 0 0 1 0 10"},
        {"redo", "M16 6l5 5-5 5|M21 11H10a5 5 0 0 0 0 10"},
        {"plus", "M12 5v14|M5 12h14"},
        {"trash", "M4 7h16|M9 7V4h6v3|M6 7l1 13h10l1-13|M10 11v6|M14 11v6"},
        {"duplicate", "M8 8h12v12H8z|M16 8V4H4v12h4"},
        {"group", "M3 3h18v18H3z|M7 8h5v5H7z|M12 12h5v5h-5z"},
        {"search", circ(10, 10, 6) + "|M14.5 14.5L20 20"},
        {"frame", "M4 9V4h5|M15 4h5v5|M20 15v5h-5|M9 20H4v-5"},
        {"grid", "M4 4h16v16H4z|M4 9.3h16|M4 14.7h16|M9.3 4v16|M14.7 4v16"},
        {"folder", "M3 7a1 1 0 0 1 1-1h5l2 2h8a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z"},
        {"history", "M4 12a8 8 0 1 0 2.5-5.8|M4 4v4h4|M12 8v4l3 2"},
        {"warning", "M12 3l10 18H2z|M12 10v5|!" + circ(12, 17.8f, 1)},
        {"error", circ(12, 12, 9) + "|M9 9l6 6|M15 9l-6 6"},
        {"info", circ(12, 12, 9) + "|M12 11v6|!" + circ(12, 7.6f, 1)},
        {"check", "M5 12.5l4.5 4.5L19 7"},
        {"close", "M6 6l12 12|M18 6L6 18"},
        {"chevron_down", "M6 9l6 6 6-6"},
        {"chevron_right", "M9 6l6 6-6 6"},
        {"settings", "M4 7h8|M18 7h2|M4 17h2|M10 17h10|" + circ(15, 7, 2.5f) + "|" + circ(7.5f, 17, 2.5f)},
        {"eye", "M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z|" + circ(12, 12, 3)},
        {"layout", "M3 4h18v16H3z|M9 4v16|M9 14h12"},
        {"command", "M5 7l5 5-5 5|M12 17h7"},
        {"project", "M4 5h16v14H4z|M4 9h16|M7 7h.01"},
        {"cube", "M12 3l8 4.5v9L12 21l-8-4.5v-9z|M12 12l8-4.5|M12 12v9|M12 12L4 7.5"},
        {"paint", "M18.5 2.5a2.1 2.1 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z|M15 6l3 3"},
        {"script", "M6 3h8l4 4v14H6z|M14 3v4h4|M10 12l-2 2 2 2|M14 12l2 2-2 2"},
        {"rigid_body", "M4 4h16v16H4z|!" + circ(12, 12, 2.5f)},
        {"character_body", circ(12, 6, 2.5f) + "|M6 20v-3l6-3 6 3v3"},
        {"static_body", "M3 18h18|M4 22l4-4|M9 22l4-4|M14 22l4-4|M4 6h16v12H4z"},
        {"raycast", "M4 4l12 12|M11 16h5v-5|!" + circ(18, 18, 2)},
        {"joint", circ(7, 12, 3) + "|" + circ(17, 12, 3) + "|M10 12h4"},
        // ---- 2D lighting (milestone 9.7) and animation
        {"light", circ(12, 10, 4.5f) + "|M10 17h4|M10.5 20h3|M12 2.5v1.5|M4.5 10H3|M21 10h-1.5|M6.7 4.7l1 1|M17.3 4.7l-1 1"},
        {"sun", "!" + circ(12, 12, 3.5f) + "|M12 3v2.5|M12 18.5V21|M3 12h2.5|M18.5 12H21|M5.6 5.6l1.8 1.8|M16.6 16.6l1.8 1.8|M5.6 18.4l1.8-1.8|M16.6 7.4l1.8-1.8"},
        {"moon", "M15 4a8 8 0 1 0 5 13A7 7 0 0 1 15 4z"},
        {"occluder", "M5 5h14v14H5z|!M5 5h7v14H5z"},
        {"animation", "M4 6h16v12H4z|M4 9h16|M4 15h16|!M10 10.5v3l3-1.5z"},
    };
    return list;
}

struct Table {
    std::vector<std::string> order;
    std::map<std::string, std::vector<IconShape>, std::less<>> icons;
};

const Table& table() {
    static const Table t = [] {
        Table out;
        for (const IconSource& src : sources()) {
            std::vector<IconShape> shapes;
            std::size_t pos = 0;
            while (pos <= src.shapes.size()) {
                std::size_t end = src.shapes.find('|', pos);
                if (end == std::string::npos) end = src.shapes.size();
                std::string_view d = std::string_view(src.shapes).substr(pos, end - pos);
                pos = end + 1;
                IconShape shape;
                if (!d.empty() && d.front() == '!') {
                    shape.fill = true;
                    d.remove_prefix(1);
                }
                if (d.empty()) continue;
                vector::parse_path_data(d, shape.path);
                shapes.push_back(std::move(shape));
            }
            out.order.emplace_back(src.name);
            out.icons.emplace(src.name, std::move(shapes));
        }
        return out;
    }();
    return t;
}

// Engine and node-library classes that have their own icon. Subclasses inherit
// by walking up; a class missing here (a new node from the node library, say)
// gets its base class's icon until it is given one.
const std::map<std::string, std::string_view, std::less<>>& class_icons() {
    static const std::map<std::string, std::string_view, std::less<>> m = {
        {"Node", "node"},
        {"Node2D", "node2d"},
        {"SceneInstance", "scene_instance"},
        {"CanvasLayer", "canvas_layer"},
        {"Control", "control"},
        {"Container", "container"},
        {"BoxContainer", "container"},
        {"VBox", "vbox"},
        {"HBox", "hbox"},
        {"Margin", "margin"},
        {"Panel", "panel"},
        {"Center", "center"},
        {"Label", "label"},
        {"Button", "button"},
        {"Toggle", "toggle"},
        {"Slider", "slider"},
        {"Shape", "shape"},
        {"Path", "path"},
        {"Sprite", "sprite"},
        {"Text", "text"},
        {"Camera2D", "camera"},
        {"Timer", "timer"},
        {"Area", "area"},
        {"Emitter2D", "emitter"},
        {"AudioPlayer", "audio"},
        {"TileMapLayer", "grid"},
        {"NavigationRegion2D", "area"},
        {"NavigationAgent2D", "path"},
        {"CollisionShape2D", "shape"},
        {"CollisionPolygon2D", "shape"},
        {"PhysicsBody2D", "rigid_body"},
        {"StaticBody2D", "static_body"},
        {"AnimatableBody2D", "static_body"},
        {"RigidBody2D", "rigid_body"},
        {"CharacterBody2D", "character_body"},
        {"Area2D", "area"},
        {"RayCast2D", "raycast"},
        {"ShapeCast2D", "raycast"},
        {"Joint2D", "joint"},
        {"PinJoint2D", "joint"},
        {"RevoluteJoint2D", "joint"},
        {"DistanceJoint2D", "joint"},
        {"DampedSpringJoint2D", "joint"},
        {"PrismaticJoint2D", "joint"},
        {"SliderJoint2D", "joint"},
        {"GrooveJoint2D", "joint"},
        {"WeldJoint2D", "joint"},
        {"WheelJoint2D", "joint"},
        {"MotorJoint2D", "joint"},
        {"MoverJoint2D", "joint"},
        {"PogoJoint2D", "joint"},
        {"FilterJoint2D", "joint"},
        {"TileMapCollision2D", "static_body"},
        {"PointLight2D", "light"},
        {"DirectionalLight2D", "sun"},
        {"AmbientLight2D", "moon"},
        {"LightOccluder2D", "occluder"},
        {"AnimationPlayer", "animation"},
        {"AnimationTree", "animation"},
    };
    return m;
}

} // namespace

const std::vector<IconShape>* icon_shapes(std::string_view name) {
    const auto it = table().icons.find(name);
    return it == table().icons.end() ? nullptr : &it->second;
}

bool has_icon(std::string_view name) { return icon_shapes(name) != nullptr; }

std::vector<std::string> icon_names() { return table().order; }

std::string_view icon_for_class(const ClassRegistry* registry, std::string_view class_name) {
    std::string current(class_name);
    for (int depth = 0; depth < 32; ++depth) { // bounded: a cyclic registry can't hang the UI
        if (const auto it = class_icons().find(current); it != class_icons().end()) return it->second;
        const ClassSpec* spec = registry != nullptr ? registry->find(current) : nullptr;
        if (spec == nullptr || spec->base.empty()) break;
        current = spec->base;
    }
    return "node";
}

} // namespace cuervo::editor

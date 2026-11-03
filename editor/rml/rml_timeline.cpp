// The drawer's Animation tab (E2): clip chips, a tick ruler with one lane per track and diamond keys,
// a playhead, track tools and a key editor. Edits are Timeline operations (one rewrite of the player's
// `animations` field, a key drag merges into one undo step); "Pose" shows the clip at the playhead in
// the scene view through the same ProjectEnv::preview_animation the ImGui panel uses.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include <RmlUi/Core/Mesh.h>

#include "editor/icons.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"

namespace cuervo::editor {

namespace {

// <tlkey>: a keyframe diamond. The RmlRenderer ignores `transform`, so a rotated square is out; the
// element draws its own four-vertex polygon through Rml geometry, filled with its computed `color`
// (so hover and .on states come from the stylesheet).
class KeyDiamond final : public Rml::Element {
public:
    explicit KeyDiamond(const Rml::String& tag_name) : Rml::Element(tag_name) {}

protected:
    void OnRender() override {
        Rml::RenderManager* rm = GetRenderManager();
        if (rm == nullptr) return;
        const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Border);
        const Rml::Vector2f at = GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f c = at + size * 0.5f;
        const Rml::Colourb col = GetComputedValues().color();
        const float a = static_cast<float>(col.alpha) / 255.0f;
        const Rml::ColourbPremultiplied p(static_cast<Rml::byte>(col.red * a), static_cast<Rml::byte>(col.green * a), static_cast<Rml::byte>(col.blue * a), col.alpha);
        Rml::Mesh mesh;
        const float hx = size.x * 0.5f, hy = size.y * 0.5f;
        mesh.vertices = {{{c.x, c.y - hy}, p, {0, 0}}, {{c.x + hx, c.y}, p, {0, 0}}, {{c.x, c.y + hy}, p, {0, 0}}, {{c.x - hx, c.y}, p, {0, 0}}};
        mesh.indices = {0, 1, 2, 0, 2, 3};
        Rml::Geometry g = rm->MakeGeometry(std::move(mesh));
        g.Render({0.0f, 0.0f});
    }
};

} // namespace

void register_key_view() {
    static Rml::ElementInstancerGeneric<KeyDiamond> instancer;
    Rml::Factory::RegisterElementInstancer("tlkey", &instancer);
}


namespace {

Rml::Element* find_attr(Rml::Element* e, const char* attr) {
    for (; e != nullptr; e = e->GetParentNode()) if (e->HasAttribute(attr)) return e;
    return nullptr;
}

std::string pct(double v, double of) { return std::format("{:.3f}%", of > 0 ? v * 100.0 / of : 0.0); }

std::string next_of(const std::vector<std::string_view>& names, const std::string& cur) {
    for (std::size_t i = 0; i < names.size(); ++i) if (names[i] == cur) return std::string(names[(i + 1) % names.size()]);
    return std::string(names.front());
}

std::string field(const char* label, const char* id, const std::string& value, int width_dp) {
    return std::format("<span class=\"tlf\">{}</span><span class=\"tf tlin\" style=\"width: {}dp\"><input type=\"text\" id=\"{}\" value=\"{}\"/></span>", label, width_dp, id, w::esc(value));
}

std::string num_text(float v) {
    std::string s = std::format("{:.3f}", v);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

} // namespace

std::optional<Shell::AnimPose> Shell::anim_pose() const {
    if (!pose_on_ || tl_.clip() == nullptr || ctl_.play().active()) return std::nullopt;
    return AnimPose{tl_.player(), tl_.clip()->name, tl_.playhead};
}

void Shell::build_animation_drawer(const ModelNode& n) {
    Rml::Element* e = by_id("drawer-body");
    EditorDocument& doc = ctl_.doc();
    const NodeId player = find_animation_player(doc, ctl_.env().classes());
    if (player == 0) {
        set_drawer_head(std::format("<span class=\"dh-title\">Animation</span><span class=\"dh-note\">{}</span>", w::esc(n.name)));
        pose_on_ = false;
        e->SetInnerRML(field_group(n.type, {"animation", "frames", "playing", "speed", "fps", "loop", "frame"}));
        return;
    }
    tl_.follow(doc, player);
    const anim::Clip* clip = tl_.clip();
    set_drawer_head(std::format("<span class=\"dh-title\">Animation</span><span class=\"dh-note\">{}</span>", clip != nullptr ? std::format("{}, {} ticks, loop {}", w::esc(clip->name), clip->length, w::esc(clip->mode)) : std::string("no animation")));

    std::string out = "<div class=\"tl\"><div class=\"tlbar\">";
    for (std::size_t i = 0; i < tl_.clips().size(); ++i) {
        out += std::format("<div class=\"btn sm{}\" data-clip=\"{}\"><span>{}</span></div>", static_cast<int>(i) == tl_.clip_index() ? " on" : "", i, w::esc(tl_.clips()[i].name));
    }
    if (tl_new_open_) out += std::format("<span class=\"tf tlin\" style=\"width: 110dp\"><input type=\"text\" id=\"an-new\" value=\"{}\" placeholder=\"name, Enter\"/></span>", w::esc(tl_new_name_));
    else out += std::format("<div class=\"btn sm\" data-an=\"new\" title=\"Add an animation\">{}<span>New</span></div>", w::icon("plus", 12));
    if (clip != nullptr) {
        out += field("ticks", "an-len", std::to_string(clip->length), 48);
        out += std::format("<div class=\"btn sm\" data-an=\"loop\" title=\"Loop mode (click to change)\"><span>loop {}</span></div>", w::esc(clip->mode));
        out += std::format("<div class=\"btn sm{}\" data-an=\"pose\" title=\"Pose the scene view at the playhead\"><span>Pose</span></div>", pose_on_ ? " on" : "");
        out += std::format("<div class=\"btn sm{}\" data-an=\"play\" title=\"Play the clip in this timeline\">{}<span>{}</span></div>", timeline_play_ ? " on" : "", w::icon(timeline_play_ ? "pause" : "play", 12), timeline_play_ ? "Pause" : "Play");
        out += std::format("<div class=\"btn sm\" data-an=\"del-clip\" title=\"Delete this animation\">{}</div>", w::icon("trash", 12));
        out += "<span id=\"tl-tick\" class=\"mono tlf\"></span>";
    }
    out += "</div>";
    if (clip == nullptr) {
        out += "<div class=\"empty\">No animations yet. Press New and name one.</div></div>";
        e->SetInnerRML(out);
        return;
    }
    const double len = std::max(clip->length, 1);
    out += "<div class=\"tlbody scroll\"><div class=\"tlrow\"><div class=\"tllab tlhd\"><span>Track</span><span class=\"src\">" + std::to_string(clip->length) + " ticks</span></div><div class=\"tllane\" id=\"tl-ruler\">";
    int step = 1;
    while (static_cast<double>(step) / len * 600.0 < 12.0) step *= 2;
    for (int k = 0; k <= clip->length; k += step) {
        const bool major = (k / step) % 4 == 0;
        out += std::format("<div class=\"tlmark{}\" style=\"left: {}\">{}</div>", major ? " major" : "", pct(k, len), major ? std::to_string(k) : std::string());
    }
    out += "<div class=\"tlph\"></div></div></div>";
    for (std::size_t ti = 0; ti < clip->tracks.size(); ++ti) {
        const anim::Track& t = clip->tracks[ti];
        const std::string label = t.kind == "Property" ? t.path : std::format("{} ({})", t.path, t.kind);
        out += std::format("<div class=\"tlrow{}\" data-track=\"{}\"><div class=\"tllab\"><span>{}</span></div><div class=\"tllane\" data-lane=\"{}\">", static_cast<int>(ti) == tl_.track_index() ? " sel" : "", ti, w::esc(label.empty() ? std::string("(event)") : label), ti);
        for (std::size_t ki = 0; ki < t.keys.size(); ++ki) {
            const bool on = static_cast<int>(ti) == tl_.track_index() && static_cast<int>(ki) == tl_.key_index();
            out += std::format("<tlkey class=\"tlkey{}\" data-key=\"{}:{}\" style=\"left: {}\" title=\"tick {}, {} (right-click deletes)\"></tlkey>", on ? " on" : "", ti, ki, pct(t.keys[ki].time, len), t.keys[ki].time, w::esc(t.keys[ki].interp));
        }
        out += "<div class=\"tlph\"></div></div></div>";
    }
    if (clip->tracks.empty()) out += "<div class=\"hint\">No tracks. Add one below, for example \"arm:rotation\".</div>";
    out += "</div><div class=\"tlfoot\"><div class=\"tlbar\">";
    out += field("track", "an-path", tl_new_track_, 170);
    out += std::format("<div class=\"btn sm\" data-an=\"kind\" title=\"Track kind (click to change)\"><span>{}</span></div><div class=\"btn sm\" data-an=\"add-track\"><span>Add track</span></div>", w::esc(tl_new_kind_));
    if (tl_.track() != nullptr) {
        out += std::format("<div class=\"btn sm\" data-an=\"key\" title=\"Key the selected track's current value at the playhead\">{}<span>Key</span></div><div class=\"btn sm\" data-an=\"del-track\"><span>Delete track</span></div>", w::icon("plus", 12));
    }
    out += "</div>";
    if (const anim::Key* k = tl_.key()) {
        const anim::Track* t = tl_.track();
        out += "<div class=\"tlbar\">" + field("tick", "an-tick", std::to_string(k->time), 44);
        if (t->kind == "Property") {
            for (int i = 0; i < 4; ++i) out += field(std::array<const char*, 4>{"x", "y", "z", "w"}[static_cast<std::size_t>(i)], std::array<const char*, 4>{"an-v0", "an-v1", "an-v2", "an-v3"}[static_cast<std::size_t>(i)], num_text(k->value[static_cast<std::size_t>(i)]), 58);
            out += std::format("<div class=\"btn sm\" data-an=\"interp\" title=\"Interpolation to the next key (click to change)\"><span>{}</span></div>", w::esc(k->interp));
            if (k->interp == "EaseIn" || k->interp == "EaseOut" || k->interp == "EaseInOut") out += std::format("<div class=\"btn sm\" data-an=\"ease\"><span>{}</span></div>", w::esc(k->ease_kind));
            if (k->interp == "Curve") out += field("curve", "an-curve", num_text(k->curve), 50);
        } else {
            out += field(t->kind == "Event" ? "event" : "text", "an-text", k->text, 150);
        }
        out += std::format("<div class=\"btn sm\" data-an=\"del-key\">{}<span>Delete key</span></div></div>", w::icon("trash", 12));
    } else {
        out += "<div class=\"hint\">Click a lane to move the playhead, a diamond to edit its key, drag it to move; right-click deletes. Enter applies a typed value.</div>";
    }
    out += std::format("{}</div></div>", tl_error_.empty() ? std::string() : std::format("<div class=\"warn\">{}<span>{}</span></div>", w::icon("warning", 14), w::esc(tl_error_)));
    e->SetInnerRML(out);
    timeline_head();
}

// Playhead lines and tick readout, without rebuilding the rows.
void Shell::timeline_head() {
    const anim::Clip* clip = tl_.clip();
    Rml::Element* body = by_id("drawer-body");
    if (clip == nullptr || body == nullptr) return;
    Rml::ElementList list;
    body->GetElementsByClassName(list, "tlph");
    const std::string left = pct(tl_.playhead, std::max(clip->length, 1));
    for (Rml::Element* p : list) p->SetProperty("left", left);
    if (Rml::Element* t = by_id("tl-tick")) t->SetInnerRML(std::format("tick {}", tl_.playhead));
}

// Advances playback; called once per frame while the Animation tab shows.
void Shell::step_timeline() {
    const std::uint32_t now = SDL_GetTicks();
    const float dt = std::min(static_cast<float>(now - tl_ms_) / 1000.0f, 0.1f);
    tl_ms_ = now;
    const anim::Clip* clip = tl_.clip();
    if (!timeline_play_ || clip == nullptr) return;
    tl_acc_ += dt * 60.0f; // ticks at the engine's fixed rate
    const int whole = static_cast<int>(tl_acc_);
    if (whole <= 0) return;
    tl_acc_ -= static_cast<float>(whole);
    int p = tl_.playhead + whole;
    if (p > clip->length) {
        if (clip->mode == "None") { p = clip->length; timeline_play_ = false; build_node_drawer(); }
        else p = clip->length > 0 ? p % (clip->length + 1) : 0;
    }
    tl_.playhead = p;
    timeline_head();
}

int Shell::lane_tick(float x) const {
    Rml::Element* r = by_id("tl-ruler");
    const anim::Clip* clip = tl_.clip();
    if (r == nullptr || clip == nullptr) return 0;
    const float left = r->GetAbsoluteOffset(Rml::BoxArea::Content).x;
    const float width = std::max(r->GetBox().GetSize(Rml::BoxArea::Content).x, 1.0f);
    return std::clamp(static_cast<int>(std::lround((x - left) / width * static_cast<float>(clip->length))), 0, clip->length);
}

bool Shell::timeline_mousedown(Rml::Element* t, Rml::Vector2f at, int button) {
    if (node_tab_ != "Animation" || !layout_.drawer_open || tl_.clip() == nullptr) return false;
    bool in_body = false;
    for (Rml::Element* e = t; e != nullptr; e = e->GetParentNode()) if (e->GetId() == "drawer-body") in_body = true;
    if (!in_body) return false;
    EditorDocument& doc = ctl_.doc();
    if (Rml::Element* k = find_attr(t, "data-key")) {
        const std::string v = k->GetAttribute<Rml::String>("data-key", "0:0");
        const int ti = std::atoi(v.c_str());
        const int ki = std::atoi(v.substr(v.find(':') + 1).c_str());
        if (button == 1) {
            tl_error_ = tl_.delete_key(doc, ti, ki);
        } else {
            tl_.select_track(ti, ki);
            if (const anim::Key* key = tl_.key()) tl_.playhead = key->time;
            tl_drag_ = true;
        }
        build_node_drawer();
        return true;
    }
    if (Rml::Element* lane = find_attr(t, "data-lane"); lane != nullptr && button == 0) {
        tl_.select_track(lane->GetAttribute<int>("data-lane", -1));
        tl_.playhead = lane_tick(at.x);
        tl_scrub_ = true;
        build_node_drawer();
        return true;
    }
    if (Rml::Element* row = find_attr(t, "data-track"); row != nullptr && button == 0) {
        tl_.select_track(row->GetAttribute<int>("data-track", -1));
        build_node_drawer();
        return true;
    }
    if (dom::inside(t, "tl-ruler") && button == 0) {
        tl_.playhead = lane_tick(at.x);
        tl_scrub_ = true;
        timeline_head();
        return true;
    }
    return false;
}

void Shell::timeline_mousemove(Rml::Vector2f at) {
    if (tl_drag_) {
        tl_error_ = tl_.move_key(ctl_.doc(), lane_tick(at.x));
    } else if (tl_scrub_) {
        tl_.playhead = lane_tick(at.x);
        timeline_head();
    }
}

void Shell::timeline_mouseup() {
    if (tl_drag_) ctl_.doc().seal();
    if (tl_scrub_) build_node_drawer(); // key editor / pose follow the final tick
    tl_drag_ = tl_scrub_ = false;
}

bool Shell::timeline_click(Rml::Element* t) {
    if (node_tab_ != "Animation") return false;
    EditorDocument& doc = ctl_.doc();
    if (Rml::Element* c = find_attr(t, "data-clip")) {
        tl_.select_clip(c->GetAttribute<int>("data-clip", -1));
        build_node_drawer();
        return true;
    }
    Rml::Element* b = find_attr(t, "data-an");
    if (b == nullptr) return false;
    const std::string what = b->GetAttribute<Rml::String>("data-an", "");
    tl_error_.clear();
    const anim::Key* key = tl_.key();
    if (what == "new") { tl_new_open_ = true; }
    else if (what == "del-clip") tl_error_ = tl_.delete_clip(doc);
    else if (what == "loop" && tl_.clip() != nullptr) tl_error_ = tl_.set_mode(doc, next_of(anim::loop_names(), tl_.clip()->mode));
    else if (what == "pose") { pose_on_ = !pose_on_; pose_dirty_ = true; }
    else if (what == "play") { timeline_play_ = !timeline_play_; tl_acc_ = 0.0f; }
    else if (what == "kind") tl_new_kind_ = next_of(anim::track_kind_names(), tl_new_kind_);
    else if (what == "add-track") tl_error_ = tl_.add_track(doc, tl_new_track_, tl_new_kind_);
    else if (what == "del-track") tl_error_ = tl_.delete_track(doc);
    else if (what == "key") tl_error_ = tl_.key_at_playhead(doc);
    else if (what == "del-key") { tl_error_ = tl_.delete_key(doc, tl_.track_index(), tl_.key_index()); }
    else if (what == "interp" && key != nullptr) { anim::Key k = *key; k.interp = next_of(anim::interp_names(), k.interp); tl_error_ = tl_.edit_key(doc, k, false); }
    else if (what == "ease" && key != nullptr) { anim::Key k = *key; k.ease_kind = next_of(anim::ease_names(), k.ease_kind); tl_error_ = tl_.edit_key(doc, k, false); }
    else return false;
    build_node_drawer();
    return true;
}

bool Shell::timeline_change(Rml::Element* t, const std::string& value, bool enter) {
    const std::string id = t != nullptr ? t->GetId() : std::string();
    if (id.rfind("an-", 0) != 0) return false;
    EditorDocument& doc = ctl_.doc();
    if (id == "an-path") { tl_new_track_ = value; if (enter) { tl_error_ = tl_.add_track(doc, tl_new_track_, tl_new_kind_); build_node_drawer(); } return true; }
    if (id == "an-new") {
        tl_new_name_ = value;
        if (enter) {
            tl_error_ = tl_.new_clip(doc, tl_new_name_);
            if (tl_error_.empty()) { tl_new_name_.clear(); tl_new_open_ = false; }
            build_node_drawer();
        }
        return true;
    }
    if (!enter) return true; // typed values apply on Enter
    if (id == "an-len") { tl_error_ = tl_.set_length(doc, std::atoi(value.c_str()), false); build_node_drawer(); return true; }
    if (tl_.key() == nullptr) return true;
    anim::Key k = *tl_.key();
    const float f = static_cast<float>(std::atof(value.c_str()));
    if (id == "an-tick") { tl_error_ = tl_.move_key(doc, std::atoi(value.c_str())); doc.seal(); build_node_drawer(); return true; }
    if (id.size() == 5 && id.rfind("an-v", 0) == 0) k.value[static_cast<std::size_t>(id[4] - '0')] = f;
    else if (id == "an-curve") k.curve = f;
    else if (id == "an-text") k.text = value;
    else return true;
    tl_error_ = tl_.edit_key(doc, k, false);
    build_node_drawer();
    return true;
}

} // namespace cuervo::editor

#include "editor/timeline.hpp"

#include <algorithm>
#include <format>

namespace cuervo::editor {

namespace {
bool is_player(const ClassRegistry& classes, const std::string& type) {
    return type == "AnimationPlayer" || classes.is_a(type, "AnimationPlayer");
}
} // namespace

NodeId find_animation_player(const EditorDocument& doc, const ClassRegistry& classes) {
    const NodeId sel = doc.selection();
    const ModelNode* n = doc.model().get(sel);
    if (n == nullptr) return 0;
    if (is_player(classes, n->type)) return sel;
    for (NodeId c : n->children) {
        if (const ModelNode* cn = doc.model().get(c); cn != nullptr && is_player(classes, cn->type)) return c;
    }
    return 0;
}

std::optional<std::array<float, 4>> record_value(const EditorDocument& doc, NodeId player, const std::string& path) {
    std::string node_path, field;
    if (!anim::split_path(path, node_path, field)) return std::nullopt;
    const SceneModel& model = doc.model();
    const ModelNode* p = model.get(player);
    if (p == nullptr) return std::nullopt;
    // root_path: ".." (the default) is the parent, "." the player itself.
    const scene::Value root_path = doc.field_value(player, "root_path");
    NodeId at = root_path.kind == scene::ValueKind::Str && root_path.text == "." ? player : p->parent;
    std::size_t pos = 0;
    while (at != 0 && pos < node_path.size()) {
        std::size_t end = node_path.find('/', pos);
        if (end == std::string::npos) end = node_path.size();
        const std::string seg = node_path.substr(pos, end - pos);
        pos = end + 1;
        if (seg.empty() || seg == ".") continue;
        const ModelNode* n = model.get(at);
        if (seg == "..") {
            at = n->parent;
            continue;
        }
        NodeId next = 0;
        for (NodeId c : n->children) {
            if (model.get(c)->name == seg) {
                next = c;
                break;
            }
        }
        at = next;
    }
    if (at == 0) return std::nullopt;
    if (doc.field_type(at, field).empty()) return std::nullopt;
    return anim::pack(doc.field_value(at, field));
}

void Timeline::follow(const EditorDocument& doc, NodeId player) {
    if (player != player_) {
        player_ = player;
        clip_ = track_ = key_ = -1;
        playhead = 0;
    }
    clips_ = player == 0 ? std::vector<anim::Clip>{} : anim::read(doc.field_value(player, "animations"));
    if (clip_ >= static_cast<int>(clips_.size())) clip_ = -1;
    if (clip_ < 0 && !clips_.empty()) clip_ = 0;
    if (const anim::Clip* c = clip()) {
        if (track_ >= static_cast<int>(c->tracks.size())) track_ = -1;
        if (track_ < 0 || key_ >= static_cast<int>(c->tracks[static_cast<std::size_t>(track_)].keys.size())) key_ = -1;
        playhead = std::clamp(playhead, 0, std::max(c->length, 0));
    } else {
        track_ = key_ = -1;
    }
}

const anim::Track* Timeline::track() const {
    const anim::Clip* c = clip();
    return c != nullptr && track_ >= 0 && track_ < static_cast<int>(c->tracks.size()) ? &c->tracks[static_cast<std::size_t>(track_)] : nullptr;
}

const anim::Key* Timeline::key() const {
    const anim::Track* t = track();
    return t != nullptr && key_ >= 0 && key_ < static_cast<int>(t->keys.size()) ? &t->keys[static_cast<std::size_t>(key_)] : nullptr;
}

std::string Timeline::commit(EditorDocument& doc, bool merge) {
    if (!doc.set_field(player_, "animations", anim::write(clips_), merge)) return "could not write the animations";
    if (!merge) doc.seal();
    return {};
}

std::string Timeline::new_clip(EditorDocument& doc, const std::string& name) {
    if (player_ == 0 || name.empty()) return "name the animation first";
    for (const anim::Clip& c : clips_) if (c.name == name) return std::format("an animation named '{}' already exists", name);
    anim::Clip c;
    c.name = name;
    clips_.push_back(c);
    std::string e = commit(doc);
    if (e.empty()) select_clip(static_cast<int>(clips_.size()) - 1);
    return e;
}

std::string Timeline::delete_clip(EditorDocument& doc) {
    if (clip() == nullptr) return {};
    clips_.erase(clips_.begin() + clip_);
    select_clip(-1);
    return commit(doc);
}

std::string Timeline::set_length(EditorDocument& doc, int ticks, bool merge) {
    if (clip() == nullptr || ticks < 1) return {};
    clips_[static_cast<std::size_t>(clip_)].length = ticks;
    return commit(doc, merge);
}

std::string Timeline::set_mode(EditorDocument& doc, const std::string& mode) {
    if (clip() == nullptr) return {};
    clips_[static_cast<std::size_t>(clip_)].mode = mode;
    return commit(doc);
}

std::string Timeline::add_track(EditorDocument& doc, const std::string& path, const std::string& kind) {
    if (clip() == nullptr) return "add an animation first";
    anim::Clip& c = clips_[static_cast<std::size_t>(clip_)];
    std::string node_path, field;
    if (kind == "Property" && !anim::split_path(path, node_path, field)) return "a property track's path is \"Node/Path:field\" (\":field\" for the root)";
    if (anim::find_track(c, path, kind) >= 0) return std::format("the clip already has a {} track '{}'", kind, path);
    c.tracks.push_back({path, kind, {}});
    std::string e = commit(doc);
    if (e.empty()) select_track(static_cast<int>(c.tracks.size()) - 1);
    return e;
}

std::string Timeline::delete_track(EditorDocument& doc) {
    if (track() == nullptr) return {};
    anim::Clip& c = clips_[static_cast<std::size_t>(clip_)];
    c.tracks.erase(c.tracks.begin() + track_);
    select_track(-1);
    return commit(doc);
}

std::string Timeline::key_at_playhead(EditorDocument& doc) {
    if (track() == nullptr) return "select a track first";
    anim::Track& t = clips_[static_cast<std::size_t>(clip_)].tracks[static_cast<std::size_t>(track_)];
    anim::Key key;
    key.time = playhead;
    std::string note;
    if (t.kind == "Property") {
        if (auto v = record_value(doc, player_, t.path)) key.value = *v;
        else note = std::format("'{}' isn't a field a track can record; the key is zero", t.path);
        for (const anim::Key& k : t.keys) if (k.time < key.time) key.interp = k.interp; // hold the previous interpolation
    }
    key_ = static_cast<int>(anim::put_key(t, key));
    std::string e = commit(doc);
    return e.empty() ? note : e;
}

std::string Timeline::move_key(EditorDocument& doc, int tick) {
    if (key() == nullptr) return {};
    anim::Clip& c = clips_[static_cast<std::size_t>(clip_)];
    anim::Track& t = c.tracks[static_cast<std::size_t>(track_)];
    tick = std::clamp(tick, 0, c.length);
    if (tick == t.keys[static_cast<std::size_t>(key_)].time) return {};
    if (std::any_of(t.keys.begin(), t.keys.end(), [&](const anim::Key& k) { return k.time == tick; })) return {};
    anim::Key moved = t.keys[static_cast<std::size_t>(key_)];
    moved.time = tick;
    t.keys.erase(t.keys.begin() + key_);
    key_ = static_cast<int>(anim::put_key(t, moved));
    playhead = tick;
    return commit(doc, true);
}

std::string Timeline::edit_key(EditorDocument& doc, anim::Key key, bool merge) {
    if (this->key() == nullptr) return {};
    anim::Track& t = clips_[static_cast<std::size_t>(clip_)].tracks[static_cast<std::size_t>(track_)];
    t.keys.erase(t.keys.begin() + key_);
    key_ = static_cast<int>(anim::put_key(t, key));
    return commit(doc, merge);
}

std::string Timeline::delete_key(EditorDocument& doc, int track, int key) {
    anim::Clip* c = clip() != nullptr ? &clips_[static_cast<std::size_t>(clip_)] : nullptr;
    if (c == nullptr || track < 0 || track >= static_cast<int>(c->tracks.size())) return {};
    anim::Track& t = c->tracks[static_cast<std::size_t>(track)];
    if (key < 0 || key >= static_cast<int>(t.keys.size())) return {};
    t.keys.erase(t.keys.begin() + key);
    select_track(track);
    return commit(doc);
}

} // namespace cuervo::editor

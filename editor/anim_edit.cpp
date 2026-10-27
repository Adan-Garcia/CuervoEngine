#include "editor/anim_edit.hpp"

#include <algorithm>

namespace cuervo::editor::anim {

namespace {

using scene::Field;
using scene::NumKind;
using scene::Value;
using scene::ValueKind;

const Value* member(const Value& s, std::string_view name) {
    for (const Field& f : s.fields) {
        if (f.name == name) return &f.value;
    }
    return nullptr;
}

std::string str_of(const Value& s, std::string_view name, std::string fallback) {
    const Value* v = member(s, name);
    return v != nullptr && v->kind == ValueKind::Str ? v->text : fallback;
}

std::string enum_of(const Value& s, std::string_view name, std::string fallback) {
    const Value* v = member(s, name);
    return v != nullptr && v->kind == ValueKind::Enum ? v->text : fallback;
}

double num_of(const Value& s, std::string_view name, double fallback) {
    const Value* v = member(s, name);
    if (v == nullptr) return fallback;
    if (v->kind == ValueKind::Int) return static_cast<double>(v->i);
    if (v->kind == ValueKind::Float) return v->f;
    return fallback;
}

Value make_struct(std::string type, std::vector<Field> fields) {
    Value v;
    v.kind = ValueKind::Struct;
    v.type = std::move(type);
    v.fields = std::move(fields);
    return v;
}

Field field(std::string name, Value value) { return Field{std::move(name), {}, std::move(value)}; }

} // namespace

const std::vector<std::string_view>& interp_names() {
    static const std::vector<std::string_view> n = {"Step", "Linear", "EaseIn", "EaseOut", "EaseInOut", "Curve"};
    return n;
}

const std::vector<std::string_view>& loop_names() {
    static const std::vector<std::string_view> n = {"None", "Loop", "PingPong"};
    return n;
}

const std::vector<std::string_view>& track_kind_names() {
    static const std::vector<std::string_view> n = {"Property", "Method", "Event"};
    return n;
}

const std::vector<std::string_view>& ease_names() {
    static const std::vector<std::string_view> n = {"Sine", "Quad", "Cubic", "Quart", "Quint",
                                                    "Expo", "Circ", "Back", "Elastic", "Bounce"};
    return n;
}

std::vector<Clip> read(const Value& list) {
    std::vector<Clip> clips;
    if (list.kind != ValueKind::List) return clips;
    for (const Value& a : list.items) {
        if (a.kind != ValueKind::Struct) continue;
        Clip c;
        c.name = str_of(a, "name", "");
        c.length = static_cast<int>(num_of(a, "length", 60.0));
        c.mode = enum_of(a, "mode", "None");
        if (const Value* tracks = member(a, "tracks"); tracks != nullptr && tracks->kind == ValueKind::List) {
            for (const Value& t : tracks->items) {
                if (t.kind != ValueKind::Struct) continue;
                Track tr;
                tr.path = str_of(t, "path", "");
                tr.kind = enum_of(t, "kind", "Property");
                if (const Value* keys = member(t, "keys"); keys != nullptr && keys->kind == ValueKind::List) {
                    for (const Value& k : keys->items) {
                        if (k.kind != ValueKind::Struct) continue;
                        Key key;
                        key.time = static_cast<int>(num_of(k, "time", 0.0));
                        if (const Value* v = member(k, "value"); v != nullptr && v->kind == ValueKind::Vec) {
                            for (std::size_t i = 0; i < 4 && i < v->vec.size(); ++i) key.value[i] = v->vec[i];
                        }
                        key.text = str_of(k, "text", "");
                        key.interp = enum_of(k, "interp", "Linear");
                        key.ease_kind = enum_of(k, "ease_kind", "Sine");
                        key.curve = static_cast<float>(num_of(k, "curve", 1.0));
                        tr.keys.push_back(std::move(key));
                    }
                }
                c.tracks.push_back(std::move(tr));
            }
        }
        clips.push_back(std::move(c));
    }
    return clips;
}

Value write(const std::vector<Clip>& clips) {
    std::vector<Value> anims;
    for (const Clip& c : clips) {
        std::vector<Value> tracks;
        for (const Track& t : c.tracks) {
            std::vector<Value> keys;
            for (const Key& k : t.keys) {
                keys.push_back(make_struct("AnimKey", {
                    field("time", Value::integer(k.time)),
                    field("value", Value::vector({k.value[0], k.value[1], k.value[2], k.value[3]})),
                    field("text", Value::str(k.text)),
                    field("interp", Value::enum_value("AnimInterp", k.interp)),
                    field("ease_kind", Value::enum_value("Ease", k.ease_kind)),
                    field("curve", Value::number(k.curve)),
                }));
            }
            tracks.push_back(make_struct("AnimTrack", {
                field("path", Value::str(t.path)),
                field("kind", Value::enum_value("AnimTrackKind", t.kind)),
                field("keys", Value::list(std::move(keys))),
            }));
        }
        anims.push_back(make_struct("Animation", {
            field("name", Value::str(c.name)),
            field("length", Value::integer(c.length)),
            field("mode", Value::enum_value("AnimLoop", c.mode)),
            field("tracks", Value::list(std::move(tracks))),
        }));
    }
    return Value::list(std::move(anims));
}

void normalize(Track& track) {
    // Stable: of keys on one tick, the one placed last stays.
    std::stable_sort(track.keys.begin(), track.keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
    std::vector<Key> out;
    for (Key& k : track.keys) {
        if (!out.empty() && out.back().time == k.time) {
            out.back() = std::move(k);
        } else {
            out.push_back(std::move(k));
        }
    }
    track.keys = std::move(out);
}

std::size_t put_key(Track& track, const Key& key) {
    for (std::size_t i = 0; i < track.keys.size(); ++i) {
        if (track.keys[i].time == key.time) {
            track.keys[i] = key;
            return i;
        }
    }
    const auto at = std::find_if(track.keys.begin(), track.keys.end(), [&](const Key& k) { return k.time > key.time; });
    const auto index = static_cast<std::size_t>(at - track.keys.begin());
    track.keys.insert(at, key);
    return index;
}

int find_track(const Clip& clip, std::string_view path, std::string_view kind) {
    for (std::size_t i = 0; i < clip.tracks.size(); ++i) {
        if (clip.tracks[i].path == path && clip.tracks[i].kind == kind) return static_cast<int>(i);
    }
    return -1;
}

std::optional<std::array<float, 4>> pack(const Value& v) {
    switch (v.kind) {
        case ValueKind::Bool: return std::array<float, 4>{v.b ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        case ValueKind::Int: return std::array<float, 4>{static_cast<float>(v.i), 0.0f, 0.0f, 0.0f};
        case ValueKind::Float: return std::array<float, 4>{static_cast<float>(v.f), 0.0f, 0.0f, 0.0f};
        case ValueKind::Vec: {
            std::array<float, 4> out{0.0f, 0.0f, 0.0f, 0.0f};
            for (std::size_t i = 0; i < 4 && i < v.vec.size(); ++i) out[i] = v.vec[i];
            return out;
        }
        case ValueKind::Struct:
            if (v.type == "Color") {
                std::array<float, 4> out{0.0f, 0.0f, 0.0f, 1.0f};
                const char* names[4] = {"r", "g", "b", "a"};
                for (std::size_t i = 0; i < 4; ++i) out[i] = static_cast<float>(num_of(v, names[i], out[i]));
                return out;
            }
            return std::nullopt;
        default: return std::nullopt;
    }
}

bool split_path(std::string_view track_path, std::string& node_path, std::string& field_name) {
    const std::size_t colon = track_path.rfind(':');
    if (colon == std::string_view::npos) return false;
    node_path = std::string(track_path.substr(0, colon));
    field_name = std::string(track_path.substr(colon + 1));
    return !field_name.empty();
}

} // namespace cuervo::editor::anim

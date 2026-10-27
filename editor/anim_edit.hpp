#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/scene/scene_file.hpp"

// The data side of the editor's animation timeline (DESIGN.md 4.17 "Clarified
// (Milestone 9.8)"): an AnimationPlayer's `animations` field (List<Animation>
// of animation.plm) read from and written back to a scene value, plus the
// helpers the panel's edits need. Kept apart from ImGui so it can be tested.
namespace cuervo::editor::anim {

struct Key {
    int time = 0;
    std::array<float, 4> value{0.0f, 0.0f, 0.0f, 0.0f};
    std::string text;
    std::string interp = "Linear"; // AnimInterp variant
    std::string ease_kind = "Sine"; // Ease variant
    float curve = 1.0f;
};

struct Track {
    std::string path; // "Node/Path:field" relative to the player's root
    std::string kind = "Property"; // AnimTrackKind variant
    std::vector<Key> keys;         // sorted by time
};

struct Clip {
    std::string name;
    int length = 60;
    std::string mode = "None"; // AnimLoop variant
    std::vector<Track> tracks;
};

// The variants of the animation enums, in declaration order (combo boxes).
const std::vector<std::string_view>& interp_names();
const std::vector<std::string_view>& loop_names();
const std::vector<std::string_view>& track_kind_names();
const std::vector<std::string_view>& ease_names();

// A List<Animation> scene value as clips. Fields missing from a stored struct
// take animation.plm's defaults; anything that isn't an Animation list reads
// as no clips.
std::vector<Clip> read(const scene::Value& list);
// The clips as a List<Animation> scene value (every field written).
scene::Value write(const std::vector<Clip>& clips);

// Sorts a track's keys by time, keeping one key per tick (the last one
// placed there wins).
void normalize(Track& track);
// Inserts `key` (replacing a key on the same tick); returns its index.
std::size_t put_key(Track& track, const Key& key);
// Index of the track with `path` and `kind`, or -1.
int find_track(const Clip& clip, std::string_view path, std::string_view kind = "Property");

// A stored field value packed the way Anim.write unpacks it: float/int/bool
// in x, vectors in xy(zw), Color as rgba. nullopt for values a track can't
// drive (strings, lists, other structs).
std::optional<std::array<float, 4>> pack(const scene::Value& v);

// Splits "Torso/Head:rotation" into the node path ("Torso/Head", "" for the
// root itself) and the field ("rotation"). false without a ':'.
bool split_path(std::string_view track_path, std::string& node_path, std::string& field);

} // namespace cuervo::editor::anim

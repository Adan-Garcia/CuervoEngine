#pragma once
// Edit state and operations of the animation timeline (DESIGN.md 4.17, 9.8), UI-free: which player,
// clip, track and key are selected, the playhead, and every edit as a rewrite of the player's
// `animations` field through the document (one undo step; a drag or value scrub merges). The RmlUi
// drawer drives it; the ImGui Animation panel keeps its own selection state but shares find_player and
// record_value.
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "editor/anim_edit.hpp"
#include "editor/class_registry.hpp"
#include "editor/editor_document.hpp"

namespace cuervo::editor {

// The selected AnimationPlayer, or the first one among the selected node's children (0: none).
NodeId find_animation_player(const EditorDocument& doc, const ClassRegistry& classes);
// The value `path` ("Node/Path:field" from the player's root) has in the document, packed for a key.
std::optional<std::array<float, 4>> record_value(const EditorDocument& doc, NodeId player, const std::string& path);

class Timeline {
public:
    // Re-reads the clips for `player` and clamps the selection. Call before drawing and after edits.
    void follow(const EditorDocument& doc, NodeId player);
    NodeId player() const { return player_; }
    const std::vector<anim::Clip>& clips() const { return clips_; }
    const anim::Clip* clip() const { return clip_ >= 0 && clip_ < static_cast<int>(clips_.size()) ? &clips_[static_cast<std::size_t>(clip_)] : nullptr; }
    const anim::Track* track() const;
    const anim::Key* key() const;
    int clip_index() const { return clip_; }
    int track_index() const { return track_; }
    int key_index() const { return key_; }
    int playhead = 0;

    void select_clip(int i) { clip_ = i; track_ = key_ = -1; }
    void select_track(int t, int k = -1) { track_ = t; key_ = k; }

    // Each edit returns an error text, empty on success (the selection moves to what was made).
    std::string new_clip(EditorDocument& doc, const std::string& name);
    std::string delete_clip(EditorDocument& doc);
    std::string set_length(EditorDocument& doc, int ticks, bool merge);
    std::string set_mode(EditorDocument& doc, const std::string& mode);
    std::string add_track(EditorDocument& doc, const std::string& path, const std::string& kind);
    std::string delete_track(EditorDocument& doc);
    // Key at the playhead on the selected track, recording the field's current value.
    std::string key_at_playhead(EditorDocument& doc);
    // Moves the selected key to `tick` (refused when occupied); the playhead follows.
    std::string move_key(EditorDocument& doc, int tick);
    // Replaces the selected key; `merge` for scrubbed values.
    std::string edit_key(EditorDocument& doc, anim::Key key, bool merge);
    std::string delete_key(EditorDocument& doc, int track, int key);

private:
    std::string commit(EditorDocument& doc, bool merge = false);

    NodeId player_ = 0;
    std::vector<anim::Clip> clips_;
    int clip_ = -1, track_ = -1, key_ = -1;
};

} // namespace cuervo::editor

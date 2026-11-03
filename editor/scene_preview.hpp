#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "cuervo/script/game_session.hpp"
#include "pluma/value.hpp"

// The scene view's "ready() preview" (DESIGN.md 4.13, milestone 11.7a): the
// edited scene started on its own script::GameSession -- the same code Play
// runs, so field initializers, init() and ready() all run -- and then never
// ticked. It is isolated: its own ScriptHost, VM, RNG and particle pools, no
// audio, no process-wide job configuration, so previewing can't touch the
// edited document, the edit-time preview (ProjectEnv, which never runs
// ready()) or a game being played. Refreshing throws the session away and
// starts a fresh one from the document as it is now; the app does that after
// an edit settles and after scripts are recompiled (hot reload).
namespace cuervo::editor {

class ScenePreview {
public:
    ScenePreview();
    ~ScenePreview();
    ScenePreview(const ScenePreview&) = delete;
    ScenePreview& operator=(const ScenePreview&) = delete;

    // Starts a fresh session from `options` (project dir, font, and the scene
    // to preview as `options.scene`). Audio, jobs and debugger hooks in
    // `options` are overridden. False + `error` when the project can't load;
    // a panic in ready() is not a failure: panicked() tells, and the session
    // is kept so the view can show what ran before it.
    bool refresh(script::SessionOptions options, std::string& error);
    void clear();

    bool active() const { return session_ != nullptr; }
    bool panicked() const;
    std::string error() const;
    script::GameSession* session() { return session_.get(); }
    std::uint64_t tick() const;
    std::uint64_t refreshes() const { return refreshes_; }

    // The live node at `path` ("/" is the root, "/enemies/turret"), or nil.
    pluma::Value find(std::string_view path) const;
    // One field of a live node as the debugger shows it; false if missing.
    bool field_text(std::string_view path, const std::string& field, std::string& out);
    std::size_t node_count() const;

private:
    std::unique_ptr<script::GameSession> session_;
    std::uint64_t refreshes_ = 0;
};

} // namespace cuervo::editor

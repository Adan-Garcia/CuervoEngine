#include "editor/scene_preview.hpp"

#include <vector>

#include "cuervo/script/script_host.hpp"
#include "editor/live_tree.hpp"

namespace cuervo::editor {

ScenePreview::ScenePreview() = default;
ScenePreview::~ScenePreview() = default;

bool ScenePreview::refresh(script::SessionOptions options, std::string& error) {
    clear();
    options.audio_device = false;
    options.configure_jobs = false; // the editor's job system is not the preview's to reconfigure
    options.debug_hooks = nullptr;
    options.on_active_vm = nullptr;
    options.profile_scripts = false;
    auto session = std::make_unique<script::GameSession>();
    if (!session->init(options, error)) return false;
    session_ = std::move(session);
    ++refreshes_;
    return true;
}

void ScenePreview::clear() { session_.reset(); }

bool ScenePreview::panicked() const { return session_ && session_->host().panicked(); }
std::string ScenePreview::error() const { return session_ ? session_->host().error() : std::string(); }
std::uint64_t ScenePreview::tick() const { return session_ ? session_->host().tick_count() : 0; }

pluma::Value ScenePreview::find(std::string_view path) const {
    if (!session_) return {};
    pluma::Value node = session_->host().root();
    if (!node.is_obj()) return {};
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
        if (part.empty()) continue;
        bool found = false;
        for (const pluma::Value& c : live_children(node)) {
            if (live_name(c) == part) {
                node = c;
                found = true;
                break;
            }
        }
        if (!found) return {};
    }
    return node;
}

bool ScenePreview::field_text(std::string_view path, const std::string& field, std::string& out) {
    const pluma::Value node = find(path);
    if (!session_ || !node.is_obj()) return false;
    for (const LiveField& f : live_fields(session_->host().session()->vm(), node, 400)) {
        if (f.name == field) {
            out = f.value;
            return true;
        }
    }
    return false;
}

std::size_t ScenePreview::node_count() const {
    return session_ && session_->host().root().is_obj() ? live_count(session_->host().root()) : 0;
}

} // namespace cuervo::editor

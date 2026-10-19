// ScriptHost natives for scene files (DESIGN.md 4.12), the UI (4.10) and
// pausing: Scene.load / save / quit, UI.measure, Input.pointer and
// Canvas.new_layer. Kept apart from script_host.cpp so the scene/UI side
// stays in one place.

#include <format>
#include <system_error>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/render/text_layout.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/scene_bridge.hpp"
#include "cuervo/script/script_host.hpp"

namespace cuervo::script {

namespace {

namespace nv = pluma::native;
using pluma::NativeCall;
using pluma::Value;

// Scene paths are project-relative and may not leave the project.
bool resolve_path(std::string_view p, std::string& out, std::string& error) {
    if (!assets::Vfs::normalize(p, out, error)) {
        error = std::format("'{}': scene paths are relative to the project directory and can't leave it", p);
        return false;
    }
    return true;
}

int field_index(const pluma::RecordRT* rt, std::string_view name) {
    for (std::size_t i = 0; i < rt->field_names.size(); ++i) {
        if (rt->field_names[i] == name) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

bool ScriptHost::paused() const {
    if (session_ == nullptr) return false;
    Value p = session_->global("Scene.paused");
    return p.tag == pluma::Tag::Bool && p.b;
}

// During a pause a coroutine runs only if its owner is a node that processes
// while paused (it or an ancestor has `always_process`).
bool ScriptHost::frozen(const pluma::Object* owner) const {
    if (owner == nullptr || owner->kind != pluma::ObjKind::Instance) return true;
    const pluma::RecordRT* node_rt = session_->vm().find_class("Node");
    if (node_rt == nullptr) return true;
    const int always = field_index(node_rt, "always_process");
    const int parent = field_index(node_rt, "parent");
    if (always < 0 || parent < 0) return true;
    const auto* o = static_cast<const pluma::InstanceObj*>(owner);
    if (!o->type->is_subclass_of(node_rt)) return true;
    for (int guard = 0; o != nullptr && guard < 4096; ++guard) {
        const Value& a = o->fields[static_cast<std::size_t>(always)];
        if (a.tag == pluma::Tag::Bool && a.b) return false;
        const Value& p = o->fields[static_cast<std::size_t>(parent)];
        o = p.is_obj() ? static_cast<const pluma::InstanceObj*>(p.o) : nullptr;
    }
    return true;
}

std::optional<scene::SceneDoc> ScriptHost::load_scene(std::string_view path, std::string& error,
                                                      scene::LoadInfo* info) const {
    std::string rel;
    if (!resolve_path(path, rel, error)) return std::nullopt;
    const assets::Vfs& vfs = *options_.vfs;
    // A scene saved by the game (Scene.save) shadows the game's own copy. In
    // development both are the project directory.
    if (!vfs.user_dir().empty() && vfs.packed()) {
        const std::filesystem::path saved = vfs.user_dir() / platform::path_from_utf8(rel);
        std::error_code ec;
        if (std::filesystem::is_regular_file(saved, ec)) return scene::load_file(saved, error, info);
    }
    auto file = vfs.open(rel, error);
    if (!file) return std::nullopt;
    auto doc = scene::read_binary(file->bytes(), error, info);
    if (!doc) error = std::format("{}: {}", vfs.describe(rel), error);
    return doc;
}

void ScriptHost::bind_scene_and_ui() {
    pluma::Bindings& b = bindings_;
    auto host = this;

    // --- Scene files (DESIGN.md 4.12)
    b.add_native("Scene.load", [host](NativeCall& c) {
        std::string error;
        scene::LoadInfo info;
        auto doc = host->load_scene(nv::str(c, 0), error, &info);
        if (!doc) c.vm.panic("Scene.load: " + error);
        for (const std::string& m : info.migrated) LOG_INFO("Scene '{}': migrated ({})", nv::str(c, 0), m);
        std::vector<std::string> warnings;
        Value root;
        pluma::RunResult r = instantiate_tree(c.vm, *doc, &root, warnings);
        for (const std::string& w : warnings) LOG_WARN("Scene '{}': {}", nv::str(c, 0), w);
        if (!r.ok) c.vm.panic(std::format("Scene.load('{}'): {}", nv::str(c, 0), r.panic->message));
        return root; // owned
    });
    b.add_native("Scene.save", [host](NativeCall& c) {
        std::string rel;
        std::string error;
        if (!resolve_path(nv::str(c, 1), rel, error)) {
            LOG_ERROR("Scene.save: {}", error);
            return Value::boolean(false);
        }
        // Saves go to the real file system: the project in development, the
        // per-user directory in release (a pack is read-only).
        const std::filesystem::path& user_dir = host->options_.vfs->user_dir();
        if (user_dir.empty()) {
            LOG_ERROR("Scene.save('{}'): this game has no writable directory", rel);
            return Value::boolean(false);
        }
        const std::filesystem::path path = user_dir / platform::path_from_utf8(rel);
        if (!c.args[0].is_obj()) c.vm.panic("Scene.save: the node is null");
        std::vector<std::string> warnings;
        scene::SceneDoc doc = save_tree(c.vm, c.args[0], warnings);
        for (const std::string& w : warnings) LOG_WARN("Scene.save('{}'): {}", nv::str(c, 1), w);
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        if (!scene::save_file(path, doc, error)) {
            LOG_ERROR("Scene.save('{}'): {}", nv::str(c, 1), error);
            return Value::boolean(false);
        }
        return Value::boolean(true);
    });
    b.add_native("Scene.quit", [host](NativeCall&) {
        host->quit_requested_ = true;
        return Value{};
    });

    // --- UI (DESIGN.md 4.10)
#if CUERVO_FEATURE_UI
    b.add_native("UI.measure", [host](NativeCall& c) {
        std::string_view text = nv::str(c, 0);
        float size = nv::f32(c, 1);
        if (text.empty()) return nv::vec2_result(0.0f, 0.0f);
        if (host->options_.font != nullptr) {
            render::TextStyle style;
            style.size = size;
            vec2 s = render::layout_text(*host->options_.font, text, style).size();
            return nv::vec2_result(s.x(), s.y());
        }
        // No font (headless tools): a fixed estimate keeps layout working.
        float lines = 1.0f;
        float longest = 0.0f;
        float cur = 0.0f;
        for (std::size_t pos = 0; pos < text.size();) {
            std::uint32_t cp = render::decode_utf8(text, pos);
            if (cp == '\n') {
                lines += 1.0f;
                cur = 0.0f;
            } else {
                cur += 1.0f;
                longest = std::max(longest, cur);
            }
        }
        return nv::vec2_result(longest * size * 0.55f, lines * size * 1.2f);
    });
#endif
    b.add_native("Input.pointer", [host](NativeCall&) {
        const InputFrame& f = host->input_.frame();
        return nv::vec2_result(static_cast<float>(f.pointer[0]), static_cast<float>(f.pointer[1]));
    });
    b.add_native("Canvas.new_layer", [host](NativeCall&) {
        host->flush_draw(DrawFlush::Layer);
        return Value{};
    });
}

} // namespace cuervo::script

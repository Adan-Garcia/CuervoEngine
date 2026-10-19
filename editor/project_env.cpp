#include "editor/project_env.hpp"

#include <cmath>
#include <format>
#include <functional>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/script/scene_bridge.hpp"
#include "pluma/value.hpp"
#include "pluma/vm.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

namespace {

const pluma::Value* field_of(const pluma::Value& v, std::string_view name) {
    if (!v.is_obj() || v.o->kind != pluma::ObjKind::Instance) return nullptr;
    const auto* o = static_cast<const pluma::InstanceObj*>(v.o);
    for (std::size_t i = 0; i < o->type->field_names.size(); ++i) {
        if (o->type->field_names[i] == name) return &o->fields[i];
    }
    return nullptr;
}

bool is_instance_of(const pluma::Value& v, const pluma::RecordRT* rt) {
    return rt != nullptr && v.is_obj() && v.o->kind == pluma::ObjKind::Instance &&
           static_cast<const pluma::InstanceObj*>(v.o)->type->is_subclass_of(rt);
}

std::optional<vec2> vec2_field(const pluma::Value& v, std::string_view name) {
    const pluma::Value* f = field_of(v, name);
    if (f == nullptr || !pluma::is_vec(*f) || pluma::vec_size(*f) != 2) return std::nullopt;
    const float* d = pluma::vec_data(*f);
    return vec2{d[0], d[1]};
}

float float_field(const pluma::Value& v, std::string_view name, float fallback) {
    const pluma::Value* f = field_of(v, name);
    if (f == nullptr) return fallback;
    if (f->tag == pluma::Tag::F32) return f->f;
    if (f->tag == pluma::Tag::F64) return static_cast<float>(f->d);
    return fallback;
}

struct WorldXf {
    vec2 pos{0.0f, 0.0f};
    float rot = 0.0f;
    vec2 scale{1.0f, 1.0f};
};

// A Node2D's world transform, mirroring Node2D.global_* in the prelude
// (DESIGN.md 4.17): composed up the parent chain while the hierarchy is on,
// the node is not top_level and the parent is a (non-Control) Node2D.
WorldXf world_xf(const pluma::Value& v, bool hierarchy, const pluma::RecordRT* node2d, const pluma::RecordRT* control,
                 WorldXf* parent_out, int depth = 0) {
    WorldXf local;
    if (auto p = vec2_field(v, "position")) local.pos = *p;
    local.rot = float_field(v, "rotation", 0.0f);
    if (auto s = vec2_field(v, "scale")) local.scale = *s;
    if (parent_out != nullptr) *parent_out = WorldXf{};
    const pluma::Value* par = field_of(v, "parent");
    const pluma::Value* top = field_of(v, "top_level");
    const bool detached = top != nullptr && top->tag == pluma::Tag::Bool && top->b;
    if (!hierarchy || detached || depth >= 64 || is_instance_of(v, control) || par == nullptr ||
        !is_instance_of(*par, node2d) || is_instance_of(*par, control)) {
        return local;
    }
    const WorldXf p = world_xf(*par, hierarchy, node2d, control, nullptr, depth + 1);
    if (parent_out != nullptr) *parent_out = p;
    const float c = std::cos(p.rot);
    const float s = std::sin(p.rot);
    const vec2 scaled{local.pos.x() * p.scale.x(), local.pos.y() * p.scale.y()};
    WorldXf w;
    w.pos = {p.pos.x() + scaled.x() * c - scaled.y() * s, p.pos.y() + scaled.x() * s + scaled.y() * c};
    w.rot = p.rot + local.rot;
    w.scale = {p.scale.x() * local.scale.x(), p.scale.y() * local.scale.y()};
    return w;
}

} // namespace

ProjectEnv::ProjectEnv() = default;

ProjectEnv::~ProjectEnv() {
    release_preview();
    host_.reset();
}

bool ProjectEnv::compile(const std::vector<pluma::SourceFile::Part>& files, std::string& error) {
    if (!host_->load(files, "")) {
        error = host_->error();
        return false;
    }
    registry_ = ClassRegistry::from_vm(host_->session()->vm());
    return true;
}

bool ProjectEnv::open(const fs::path& dir, const render::FontAtlas* font, std::string& error) {
    auto project = script::load_project(dir, error);
    if (!project) return false;
    project_ = std::move(*project);
    input_map_ = InputMap{};
    if (!script::apply_input(project_, input_map_, error)) {
        error = "project.toml: " + error;
        return false;
    }
    std::vector<pluma::SourceFile::Part> files;
    if (!script::read_scripts(project_, files, nullptr, error)) return false;
    script::HostOptions options;
    options.seed = project_.seed;
    options.tick_rate = project_.tick_rate;
    options.transform_hierarchy = project_.transform_hierarchy;
    options.virtual_size = {static_cast<float>(project_.virtual_size.x()), static_cast<float>(project_.virtual_size.y())};
    options.project_dir = project_.dir;
    options.font = font;
    release_preview();
    host_ = std::make_unique<script::ScriptHost>(options, input_map_);
    has_project_ = true;
    return compile(files, error);
}

bool ProjectEnv::open_prelude_only(std::string& error) {
    project_ = script::Project{};
    project_.dir = fs::current_path();
    has_project_ = false;
    release_preview();
    host_ = std::make_unique<script::ScriptHost>(script::HostOptions{}, input_map_);
    return compile({}, error);
}

fs::path ProjectEnv::resolve(const std::string& path) const {
    fs::path p(path);
    if (p.is_absolute() || !has_project_) return p;
    return project_.dir / p;
}

std::string ProjectEnv::relative(const fs::path& path) const {
    if (!has_project_) return platform::utf8(path);
    std::error_code ec;
    const fs::path rel = fs::relative(path, project_.dir, ec);
    if (ec || rel.empty() || *rel.begin() == "..") return platform::utf8(path);
    return rel.generic_string();
}

vec2 ProjectEnv::virtual_size() const {
    return {static_cast<float>(project_.virtual_size.x()), static_cast<float>(project_.virtual_size.y())};
}

void ProjectEnv::release_preview() {
    for (const pluma::Value& v : preview_nodes_) pluma::release(v);
    preview_nodes_.clear();
    bounds_.clear();
    preview_ok_ = false;
    if (host_ && host_->session() != nullptr) host_->set_root(pluma::Value::null(), false);
}

void ProjectEnv::rebuild_preview(const scene::SceneDoc& doc) {
    release_preview();
    if (!host_ || host_->session() == nullptr || doc.nodes.empty()) return;
    host_->clear_error();
    pluma::VM& vm = host_->session()->vm();
    std::vector<std::string> warnings;
    pluma::Value root;
    pluma::RunResult r = script::instantiate_tree(vm, doc, &root, warnings, &preview_nodes_);
    // The view rebuilds on every edit (each frame of a drag): report a
    // problem when it changes, not sixty times a second.
    std::string report;
    for (const std::string& w : warnings) report += w + "\n";
    if (!r.ok) report += "scene view: " + (r.panic ? r.panic->render() : std::string("failed")) + "\n";

    if (r.ok) {
        // Instanced scenes (SceneInstance) load their scene when they enter
        // the tree, which the preview never does: load them here, nested
        // ones too, so the view shows them (DESIGN.md 4.13, Phase 6B).
        if (const pluma::RecordRT* instance_rt = vm.find_class("SceneInstance")) {
            std::function<void(const pluma::Value&, int)> expand = [&](const pluma::Value& v, int depth) {
                if (is_instance_of(v, instance_rt)) {
                    pluma::RunResult made = vm.call_method(v, "instantiate");
                    if (!made.ok && made.panic) report += "scene instance: " + made.panic->message + "\n";
                }
                if (depth >= 8) return;
                const pluma::Value* children = field_of(v, "children");
                if (children == nullptr || !children->is_obj() || children->o->kind != pluma::ObjKind::List) return;
                const std::vector<pluma::Value> items = static_cast<const pluma::ListObj*>(children->o)->items;
                for (const pluma::Value& c : items) expand(c, depth + 1);
            };
            expand(root, 0);
        }
        // One layout pass for UI roots (a Control whose parent isn't one), as
        // the first tick would do (DESIGN.md 4.10), so controls have rects.
        // The whole live tree is walked: instanced scenes have UI roots too.
        const pluma::RecordRT* control = vm.find_class("Control");
        const pluma::RecordRT* node2d = vm.find_class("Node2D");
        const pluma::Value screen = pluma::vec2_value(virtual_size().x(), virtual_size().y());
        std::function<void(const pluma::Value&, bool, int)> lay_out = [&](const pluma::Value& v, bool parent_is_control,
                                                                         int depth) {
            const bool is_control = is_instance_of(v, control);
            if (is_control && !parent_is_control) {
                const pluma::Value args[2] = {pluma::vec2_value(0.0f, 0.0f), screen};
                pluma::RunResult laid = vm.call_method(v, "layout_in", args);
                if (!laid.ok && laid.panic) report += "layout: " + laid.panic->render() + "\n";
            }
            const pluma::Value* children = field_of(v, "children");
            if (depth >= 64 || children == nullptr || !children->is_obj() || children->o->kind != pluma::ObjKind::List) {
                return;
            }
            const std::vector<pluma::Value> items = static_cast<const pluma::ListObj*>(children->o)->items;
            for (const pluma::Value& c : items) lay_out(c, is_control, depth + 1);
        };
        if (control != nullptr) lay_out(root, false, 0);
        bounds_.resize(preview_nodes_.size());
        for (std::size_t i = 0; i < preview_nodes_.size(); ++i) {
            NodeBounds& b = bounds_[i];
            if (auto p = vec2_field(preview_nodes_[i], "position")) {
                b.has_position = true;
                b.position = *p;
                // Gizmos and picking live in world space: with the hierarchy
                // on, a child's `position` is parent-relative (DESIGN.md 4.17).
                WorldXf parent_xf;
                const WorldXf w = world_xf(preview_nodes_[i], project_.transform_hierarchy, node2d, control, &parent_xf);
                b.position = w.pos;
                b.parent_rotation = parent_xf.rot;
                b.parent_scale = parent_xf.scale;
            }
            if (is_instance_of(preview_nodes_[i], control)) {
                if (auto s = vec2_field(preview_nodes_[i], "size")) {
                    b.has_rect = true;
                    b.rect_min = b.position;
                    b.rect_size = *s;
                }
            }
        }
        host_->set_root(root, false);
        preview_ok_ = true;
    }
    pluma::release(root);
    if (report != last_report_) {
        last_report_ = report;
        if (!report.empty()) {
            if (report.back() == '\n') report.pop_back();
            LOG_WARN("{}", report);
        }
    }
}

} // namespace cuervo::editor

#include "editor/command_script.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <memory>
#include <format>
#include <fstream>
#include <optional>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/input/replay.hpp"
#include "editor/actions.hpp"
#include "editor/asset_library.hpp"
#include "editor/editor_document.hpp"
#include "editor/input_map_doc.hpp"
#include "editor/play_controller.hpp"
#include "editor/project_env.hpp"

namespace cuervo::editor {

namespace {

class LineReader {
public:
    explicit LineReader(std::string_view line) : s_(line) {}

    // The next word: a run of non-space characters, or a "quoted string"
    // (with \" and \\ escapes). nullopt at the end of the line.
    std::optional<std::string> word() {
        skip();
        if (pos_ >= s_.size()) return std::nullopt;
        std::string out;
        if (s_[pos_] == '"') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] != '"') {
                if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) ++pos_;
                out += s_[pos_++];
            }
            ++pos_; // closing quote
            return out;
        }
        while (pos_ < s_.size() && s_[pos_] != ' ' && s_[pos_] != '\t' && s_[pos_] != '\r') out += s_[pos_++];
        return out;
    }
    // Everything left on the line, trimmed.
    std::string_view rest() {
        skip();
        std::string_view r = s_.substr(std::min(pos_, s_.size()));
        while (!r.empty() && (r.back() == ' ' || r.back() == '\t' || r.back() == '\r')) r.remove_suffix(1);
        pos_ = s_.size();
        return r;
    }
    bool done() {
        skip();
        return pos_ >= s_.size();
    }

private:
    void skip() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r')) ++pos_;
    }
    std::string_view s_;
    std::size_t pos_ = 0;
};

bool is_identifier(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// A value in the scene text syntax; a bare word (an enum variant, a hex
// color) is passed on as a name for coerce_value to interpret.
std::optional<scene::Value> parse_script_value(std::string_view text, std::string& error) {
    if (auto v = scene::parse_value(text, error)) return v;
    if (is_identifier(text)) return scene::Value::name(std::string(text));
    return std::nullopt;
}

bool parse_u64(std::string_view s, std::uint64_t& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

// A play session's options: the project, the game font, and either its main
// or the open scene as edited (unsaved changes included).
script::SessionOptions play_options(const CommandContext& ctx, bool current_scene) {
    script::SessionOptions o;
    o.dir = ctx.env.project_dir();
    o.font_path = ctx.font_path;
    o.jobs = ctx.jobs;
    if (current_scene) {
        o.scene = std::make_shared<const scene::SceneDoc>(ctx.doc.model().to_doc());
        o.scene_label = ctx.doc.has_path() ? ctx.env.relative(ctx.doc.path()) : std::string("(new scene)");
    }
    return o;
}

bool parse_float(std::string_view s, float& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

} // namespace

ScriptResult run_command_script(std::string_view text, EditorDocument& doc, const ProjectEnv& env) {
    CommandContext context{doc, env, nullptr, nullptr, nullptr, {}, 0};
    return run_command_script(text, context);
}

ScriptResult run_command_script(std::string_view text, CommandContext& ctx) {
    EditorDocument& doc = ctx.doc;
    const ProjectEnv& env = ctx.env;
    DeviceState no_devices; // headless play: input only from `hold`
    ActionRegistry actions;  // the command palette's actions (`palette`)
    add_document_actions(actions, doc);
    ScriptResult result;
    int line_no = 0;
    std::size_t pos = 0;
    auto fail = [&](std::string message) {
        result.ok = false;
        result.line = line_no;
        result.error = std::move(message);
        return result;
    };
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++line_no;
        LineReader in(line);
        auto cmd = in.word();
        if (!cmd || cmd->starts_with('#') || cmd->starts_with("//")) continue;

        // A node argument: a path from the root, or "." (the selection).
        auto node_arg = [&](std::optional<std::string> w, NodeId& out) -> bool {
            if (!w) return false;
            out = *w == "." ? doc.selection() : doc.model().find_path(*w);
            return out != 0;
        };
        auto need_node = [&](NodeId& out) -> std::optional<ScriptResult> {
            auto w = in.word();
            if (!w) return fail(std::format("'{}' needs a node path", *cmd));
            if (!node_arg(w, out)) return fail(std::format("no node at '{}'", *w));
            return std::nullopt;
        };
        auto check = [&](bool ok) -> std::optional<ScriptResult> {
            if (!ok) return fail(doc.error());
            return std::nullopt;
        };

        if (*cmd == "new") {
            auto cls = in.word();
            if (!cls) return fail("new needs the root's class, e.g. 'new Node2D level'");
            if (doc.registry() != nullptr && doc.registry()->find(*cls) == nullptr) {
                return fail(std::format("'{}' is not a node class of this project", *cls));
            }
            auto name = in.word();
            doc.new_scene(*cls, name.value_or("root"));
        } else if (*cmd == "open") {
            auto p = in.word();
            if (!p) return fail("open needs a scene path");
            std::string error;
            if (!doc.open(env.resolve(*p), error)) return fail(error);
        } else if (*cmd == "add") {
            NodeId parent = 0;
            if (auto r = need_node(parent)) return *r;
            auto cls = in.word();
            if (!cls) return fail("add needs a class: add <parent> <Class> [name]");
            auto name = in.word();
            if (doc.add_child(parent, *cls, name.value_or("")) == 0) return fail(doc.error());
        } else if (*cmd == "remove" || *cmd == "delete") {
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            if (auto r = check(doc.remove(id))) return *r;
        } else if (*cmd == "rename") {
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            auto name = in.word();
            if (!name) return fail("rename needs the new name");
            if (auto r = check(doc.rename(id, *name))) return *r;
        } else if (*cmd == "duplicate") {
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            if (doc.duplicate(id) == 0) return fail(doc.error());
        } else if (*cmd == "reparent") {
            NodeId id = 0;
            NodeId parent = 0;
            if (auto r = need_node(id)) return *r;
            if (auto r = need_node(parent)) return *r;
            std::optional<std::size_t> index;
            if (auto w = in.word()) {
                std::size_t i = 0;
                auto [p, ec] = std::from_chars(w->data(), w->data() + w->size(), i);
                if (ec != std::errc() || p != w->data() + w->size()) return fail("reparent index must be a number");
                index = i;
            }
            if (auto r = check(doc.reparent(id, parent, index))) return *r;
        } else if (*cmd == "select") {
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            doc.select(id);
        } else if (*cmd == "set" || *cmd == "expect") {
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            auto field = in.word();
            const std::string_view value_text = in.rest();
            if (!field || value_text.empty()) return fail(std::format("{} <node> <field> <value>", *cmd));
            std::string error;
            auto value = parse_script_value(value_text, error);
            if (!value) return fail(std::format("{}: {}", *field, error));
            if (*cmd == "set") {
                if (auto r = check(doc.set_field(id, *field, std::move(*value)))) return *r;
                doc.seal();
            } else {
                scene::Value actual;
                if (*field == "name") {
                    actual = scene::Value::str(doc.model().get(id)->name);
                } else {
                    actual = doc.field_value(id, *field);
                }
                if (!coerce_value(*value, actual, doc.registry(), error)) return fail(error);
                if (!(actual == *value)) {
                    return fail(std::format("expected {} = {}, got {}", *field, scene::format_value(*value),
                                            scene::format_value(actual)));
                }
                continue; // a check, not an edit
            }
        } else if (*cmd == "reset") {
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            auto field = in.word();
            if (!field) return fail("reset <node> <field>");
            if (auto r = check(doc.reset_field(id, *field))) return *r;
        } else if (*cmd == "move") {
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            auto dx = in.word();
            auto dy = in.word();
            float x = 0.0f;
            float y = 0.0f;
            if (!dx || !dy || !parse_float(*dx, x) || !parse_float(*dy, y)) return fail("move <node> <dx> <dy>");
            // What a gizmo drag does, in one step (exact sums for the file).
            if (auto r = check(doc.move_by(id, vec2{x, y}, true))) return *r;
            doc.seal();
        } else if (*cmd == "rotate" || *cmd == "scale") {
            // What the rotate and scale gizmos do to one node: a delta in radians, a factor.
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            doc.select(id);
            auto a = in.word();
            float x = 0.0f;
            if (!a || !parse_float(*a, x)) return fail(std::format("{} <node> {}", *cmd, *cmd == "rotate" ? "<radians>" : "<factor> [<factor y>]"));
            if (*cmd == "rotate") {
                if (auto r = check(doc.rotate_selection_by(x))) return *r;
            } else {
                float y = x;
                if (auto b = in.word(); b && !parse_float(*b, y)) return fail("scale <node> <factor> [<factor y>]");
                if (auto r = check(doc.scale_selection_by(vec2{x, y}))) return *r;
            }
            doc.seal();
        } else if (*cmd == "select+" || *cmd == "select-toggle") {
            NodeId id = 0;
            if (auto r = need_node(id)) return *r;
            if (*cmd == "select+") {
                doc.select_add(id);
            } else {
                doc.select_toggle(id);
            }
        } else if (*cmd == "select-clear") {
            doc.select_clear();
        } else if (*cmd == "move-selection" || *cmd == "rotate-selection" || *cmd == "scale-selection") {
            // The gizmos on the whole selection: one undo step for all of it.
            auto a = in.word();
            auto b = in.word();
            float x = 0.0f;
            float y = 0.0f;
            if (!a || !parse_float(*a, x) || (b && !parse_float(*b, y))) return fail(std::format("{} <a> [<b>]", *cmd));
            if (!b) y = x;
            bool ok = false;
            if (*cmd == "move-selection") {
                if (!b) return fail("move-selection <dx> <dy>");
                ok = doc.move_selection_by(vec2{x, y});
            } else if (*cmd == "rotate-selection") {
                ok = doc.rotate_selection_by(x);
            } else {
                ok = doc.scale_selection_by(vec2{x, y});
            }
            if (auto r = check(ok)) return *r;
            doc.seal();
        } else if (*cmd == "group") {
            auto cls = in.word();
            if (doc.group_selection(cls.value_or("Node2D")) == 0) return fail(doc.error());
        } else if (*cmd == "palette") {
            // Runs the best matching enabled palette action, as Ctrl+Shift+P then Enter does.
            const std::string query(in.rest());
            if (query.empty()) return fail("palette <query>");
            const auto hits = actions.search(query);
            const ActionRegistry::Hit* best = nullptr;
            for (const auto& h : hits) {
                if (h.enabled) {
                    best = &h;
                    break;
                }
            }
            if (best == nullptr) return fail(std::format("palette: no enabled command matches '{}'", query));
            doc.seal();
            if (!actions.run(best->action->id)) {
                return fail(std::format("palette: '{}' failed: {}", ActionRegistry::label(*best->action), doc.error()));
            }
            doc.seal();
        } else if (*cmd == "expect-palette") {
            auto query = in.word();
            auto title = in.word();
            if (!query || !title) return fail("expect-palette <query> <title>");
            const auto hits = actions.search(*query);
            if (hits.empty() || hits.front().action->title != *title) {
                return fail(std::format("palette '{}': expected '{}' first, got '{}'", *query, *title,
                                        hits.empty() ? std::string("nothing") : hits.front().action->title));
            }
            continue;
        } else if (*cmd == "expect-selected") {
            auto n = in.word();
            std::uint64_t want = 0;
            if (!n || !parse_u64(*n, want)) return fail("expect-selected <count>");
            if (doc.selected().size() != want) {
                return fail(std::format("expected {} selected node(s), got {}", want, doc.selected().size()));
            }
            continue;
        } else if (*cmd == "expect-history") {
            auto n = in.word();
            std::uint64_t want = 0;
            if (!n || !parse_u64(*n, want)) return fail("expect-history <steps>");
            if (doc.stack().size() != want) return fail(std::format("expected {} undo step(s), got {}", want, doc.stack().size()));
            continue;
        } else if (*cmd == "jump") {
            auto n = in.word();
            std::uint64_t to = 0;
            if (!n || !parse_u64(*n, to)) return fail("jump <steps applied>");
            if (auto r = check(doc.jump_to_history(to))) return *r;
        } else if (*cmd == "undo") {
            if (!doc.undo()) return fail("nothing to undo");
        } else if (*cmd == "redo") {
            if (!doc.redo()) return fail("nothing to redo");
        } else if (*cmd == "save") {
            std::string error;
            auto p = in.word();
            const bool ok = p ? doc.save_as(env.resolve(*p), error) : doc.save(error);
            if (!ok) return fail(error);
            LOG_INFO("Saved '{}'", env.relative(doc.path()));
            result.saved = true;
        } else if (*cmd == "instance") {
            NodeId parent = 0;
            if (auto r = need_node(parent)) return *r;
            auto scene = in.word();
            if (!scene) return fail("instance <parent> <scene> [x y]");
            std::optional<vec2> at;
            if (auto xs = in.word()) {
                auto ys = in.word();
                float x = 0.0f;
                float y = 0.0f;
                if (!ys || !parse_float(*xs, x) || !parse_float(*ys, y)) return fail("instance <parent> <scene> [x y]");
                at = vec2{x, y};
            }
            const std::string self = doc.has_path() ? env.relative(doc.path()) : std::string();
            if (doc.instance_scene(parent, *scene, at, self) == 0) return fail(doc.error());
        } else if (*cmd == "play" || *cmd == "play-record") {
            if (ctx.play == nullptr) return fail("play is not available here");
            auto what = in.word();
            if (what && *what != "scene") return fail("play [scene]");
            std::string error;
            if (!ctx.play->start(play_options(ctx, what.has_value()), error)) return fail("play: " + error);
            if (*cmd == "play-record") ctx.play->start_recording();
        } else if (*cmd == "pause" || *cmd == "resume" || *cmd == "stop") {
            if (ctx.play == nullptr) return fail(std::format("{} is not available here", *cmd));
            if (!ctx.play->active()) return fail(std::format("{}: nothing is playing", *cmd));
            if (*cmd == "pause") ctx.play->pause();
            if (*cmd == "resume") ctx.play->resume();
            if (*cmd == "stop") ctx.play->stop();
        } else if (*cmd == "step") {
            if (ctx.play == nullptr || !ctx.play->active()) return fail("step: nothing is playing");
            std::uint64_t n = 1;
            if (auto w = in.word()) {
                if (!parse_u64(*w, n)) return fail("step [ticks]");
            }
            ctx.play->step(no_devices, {0.0f, 0.0f}, n);
        } else if (*cmd == "hold" || *cmd == "release") {
            if (ctx.play == nullptr || !ctx.play->active()) return fail(std::format("{}: nothing is playing", *cmd));
            auto action = in.word();
            if (!action) return fail(std::format("{} <action>", *cmd));
            auto index = ctx.play->session()->input_map().action_index(Name(*action));
            if (!index) return fail(std::format("no input action '{}'", *action));
            ctx.play->hold(*index, *cmd == "hold");
        } else if (*cmd == "append-file") {
            // Edits a project file the way saving a script would (the hook that makes hot reload testable headless).
            auto path = in.word();
            if (!path) return fail("append-file <path> <text>");
            const std::string line_text(in.rest());
            std::ofstream f(env.resolve(*path), std::ios::app | std::ios::binary);
            if (!f || !(f << line_text << '\n')) return fail(std::format("append-file: can't write '{}'", *path));
        } else if (*cmd == "reload") {
            if (ctx.play == nullptr || !ctx.play->active()) return fail("reload: nothing is playing");
            using Reload = script::GameSession::Reload;
            const Reload r = ctx.play->reload(true);
            if (r != Reload::Reloaded) return fail("reload: the scripts did not reload (see the log)");
        } else if (*cmd == "expect-tick" || *cmd == "expect-hash") {
            if (ctx.play == nullptr || !ctx.play->active()) return fail(std::format("{}: nothing is playing", *cmd));
            auto w = in.word();
            std::uint64_t want = 0;
            const int base = *cmd == "expect-hash" ? 16 : 10;
            if (!w || std::from_chars(w->data(), w->data() + w->size(), want, base).ec != std::errc()) {
                return fail(std::format("{} <{}>", *cmd, base == 16 ? "hex hash" : "tick"));
            }
            const std::uint64_t got = base == 16 ? ctx.play->state_hash() : ctx.play->tick();
            if (got != want) {
                return fail(base == 16 ? std::format("expected state hash {:016x}, got {:016x} at tick {}", want, got,
                                                     ctx.play->tick())
                                       : std::format("expected tick {}, got {}", want, got));
            }
            continue;
        } else if (*cmd == "print-hash") {
            if (ctx.play == nullptr || !ctx.play->active()) return fail("print-hash: nothing is playing");
            // The runtime's wording, so a test can compare a play in the editor with an exported game's run.
            LOG_INFO("at tick {} (state hash {:016x})", ctx.play->tick(), ctx.play->state_hash());
            continue;
        } else if (*cmd == "expect-state") {
            if (ctx.play == nullptr) return fail("expect-state is not available here");
            auto w = in.word();
            if (!w || *w != ctx.play->state_name()) {
                return fail(std::format("expected the game to be {}, it is {}", w.value_or("?"), ctx.play->state_name()));
            }
            continue;
        } else if (*cmd == "expect-panic") {
            if (ctx.play == nullptr) return fail("expect-panic is not available here");
            const std::string_view want = in.rest();
            const auto& panic = ctx.play->panic();
            if (!panic) return fail("expected a panic, the game has none");
            if (panic->render_with_locals().find(want) == std::string::npos) {
                return fail(std::format("the panic doesn't mention '{}':\n{}", want, panic->render_with_locals()));
            }
            continue;
        } else if (*cmd == "expect-clean") {
            if (doc.dirty()) return fail("the scene has unsaved changes");
            continue;
        } else if (*cmd == "record") {
            if (ctx.play == nullptr) return fail("record is not available here");
            auto what = in.word();
            if (what == "start") {
                if (!ctx.play->active()) return fail("record start: nothing is playing");
                ctx.play->start_recording();
            } else if (what == "stop") {
                ctx.play->stop_recording();
            } else if (what == "save") {
                auto path = in.word();
                if (!path) return fail("record save <path>");
                if (ctx.play->recorded().frames.empty()) return fail("record save: nothing was recorded");
                std::string error;
                if (!save_replay(env.resolve(*path), ctx.play->recorded(), error)) return fail(error);
                LOG_INFO("Replay of {} ticks saved to '{}'", ctx.play->recorded().ticks(), *path);
            } else {
                return fail("record start | stop | save <path>");
            }
        } else if (*cmd == "replay") {
            if (ctx.play == nullptr) return fail("replay is not available here");
            auto what = in.word();
            std::string error;
            if (what == "load") {
                auto path = in.word();
                if (!path) return fail("replay load <path>");
                auto replay = load_replay(env.resolve(*path), error);
                if (!replay) return fail(error);
                if (!ctx.play->start_replay(play_options(ctx, false), std::move(*replay), error)) return fail(error);
            } else if (what == "seek") {
                std::uint64_t t = 0;
                auto w = in.word();
                if (!w || !parse_u64(*w, t)) return fail("replay seek <tick>");
                if (!ctx.play->seek(t, error)) return fail(error);
            } else if (what == "verify") {
                if (ctx.play->mode() != PlayMode::Replay || !ctx.play->active()) return fail("replay verify: no replay loaded");
                ctx.play->step(no_devices, {0.0f, 0.0f}, ctx.play->replay_length() - ctx.play->tick());
                if (auto m = ctx.play->first_mismatch()) {
                    return fail(std::format("replay mismatch at tick {}: recorded {:016x}, now {:016x}", *m,
                                            ctx.play->mismatch_expected(), ctx.play->mismatch_actual()));
                }
                LOG_INFO("Replay verified: {} ticks, {} hashes match", ctx.play->tick(), ctx.play->verified());
            } else if (what == "expect-mismatch") {
                std::uint64_t t = 0;
                auto w = in.word();
                if (!w || !parse_u64(*w, t)) return fail("replay expect-mismatch <tick>");
                ctx.play->step(no_devices, {0.0f, 0.0f}, ctx.play->replay_length() - ctx.play->tick());
                if (ctx.play->first_mismatch() != t) {
                    return fail(std::format("expected the first mismatch at tick {}, got {}", t,
                                            ctx.play->first_mismatch() ? std::to_string(*ctx.play->first_mismatch())
                                                                       : std::string("none")));
                }
                continue;
            } else {
                return fail("replay load <path> | seek <tick> | verify | expect-mismatch <tick>");
            }
        } else if (*cmd == "input") {
            if (ctx.input == nullptr) return fail("input is not available here");
            InputMapDoc& im = *ctx.input;
            auto what = in.word();
            auto a = in.word();
            auto b = in.word();
            auto need = [&](int n) { return (n < 1 || a.has_value()) && (n < 2 || b.has_value()); };
            bool ok = true;
            if (what == "add-action" && need(1)) {
                ok = im.add_action(*a);
            } else if (what == "remove-action" && need(1)) {
                ok = im.remove_action(*a);
            } else if (what == "rename-action" && need(2)) {
                ok = im.rename_action(*a, *b);
            } else if (what == "bind" && need(2)) {
                ok = im.bind(*a, *b);
            } else if (what == "unbind" && need(2)) {
                ok = im.unbind(*a, *b);
            } else if (what == "add-axis" && need(1)) {
                ok = im.add_axis(*a);
            } else if (what == "remove-axis" && need(1)) {
                ok = im.remove_axis(*a);
            } else if (what == "axis-source" && need(2)) {
                ok = im.add_axis_source(*a, *b);
            } else if (what == "remove-axis-source" && need(2)) {
                ok = im.remove_axis_source(*a, *b);
            } else if (what == "undo" && !a) {
                if (!im.undo()) return fail("input undo: nothing to undo");
            } else if (what == "redo" && !a) {
                if (!im.redo()) return fail("input redo: nothing to redo");
            } else if (what == "save" && !a) {
                std::string error;
                if (!im.save(error)) return fail(error);
                LOG_INFO("Input map saved to project.toml");
            } else if (what == "expect" && need(1)) {
                // input expect <action|axis> [binding...]: exactly these, in order.
                const script::InputEntry* e = im.action(*a);
                if (e == nullptr) e = im.axis(*a);
                if (e == nullptr) return fail(std::format("no action or axis '{}'", *a));
                std::vector<std::string> want;
                if (b) want.push_back(*b);
                while (auto w = in.word()) want.push_back(*w);
                if (e->bindings != want) {
                    std::string got;
                    for (const std::string& g : e->bindings) got += " " + g;
                    return fail(std::format("'{}' is bound to{}", *a, got.empty() ? " nothing" : got));
                }
                continue;
            } else {
                return fail("input add-action|remove-action|rename-action|bind|unbind|add-axis|remove-axis|"
                            "axis-source|remove-axis-source|undo|redo|save|expect ...");
            }
            if (!ok) return fail(im.error());
        } else if (*cmd == "asset") {
            if (ctx.assets == nullptr) return fail("asset is not available here");
            AssetLibrary& lib = *ctx.assets;
            auto what = in.word();
            auto path = in.word();
            if (!what || !path) return fail("asset mkdir|rename|delete|expect|expect-missing <path> ...");
            std::string error;
            if (*what == "mkdir") {
                const std::size_t slash = path->find_last_of('/');
                const std::string parent = slash == std::string::npos ? std::string() : path->substr(0, slash);
                const std::string name = slash == std::string::npos ? *path : path->substr(slash + 1);
                if (!lib.create_folder(parent, name, error)) return fail(error);
            } else if (*what == "rename") {
                auto name = in.word();
                if (!name) return fail("asset rename <path> <new name>");
                if (!lib.rename(*path, *name, error)) return fail(error);
            } else if (*what == "delete") {
                if (!lib.remove(*path, error)) return fail(error);
            } else if (*what == "expect" || *what == "expect-missing") {
                lib.poll();
                const bool exists = lib.find(*path) != nullptr;
                if (exists != (*what == "expect")) {
                    return fail(std::format("'{}' {}", *path, exists ? "exists" : "does not exist"));
                }
                continue;
            } else {
                return fail("asset mkdir|rename|delete|expect|expect-missing <path> ...");
            }
        } else {
            return fail(std::format("unknown command '{}'", *cmd));
        }
        if (!in.done()) return fail("unexpected text at the end of the line");
        ++result.commands;
    }
    return result;
}

} // namespace cuervo::editor

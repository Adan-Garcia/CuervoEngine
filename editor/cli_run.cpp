#include "editor/cli_run.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "cuervo/core/log.hpp"
#include "cuervo/input/replay.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/script/game_session.hpp"
#include "editor/command_script.hpp"
#include "editor/editor_controller.hpp"

namespace cuervo::editor {

int run_script_file(EditorController& ctl, const std::string& file, const std::string& font_path, unsigned jobs,
                    std::vector<UiStep>* ui_steps) {
    std::ifstream in(platform::path_from_utf8(file), std::ios::binary);
    if (!in) {
        LOG_ERROR("--script-commands: can't read '{}'", file);
        return EXIT_FAILURE;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    CommandContext context{ctl.doc(), ctl.env(), &ctl.play(), &ctl.input_map(), &ctl.assets(), font_path, jobs};
    context.ui_steps = ui_steps;
    const ScriptResult r = run_command_script(buffer.str(), context);
    if (!r.ok) {
        LOG_ERROR("{}:{}: {}", file, r.line, r.error);
        return EXIT_FAILURE;
    }
    if (ctl.doc().dirty()) {
        std::string error;
        if (!ctl.doc().save(error)) {
            LOG_ERROR("--script-commands: {}", error);
            return EXIT_FAILURE;
        }
        LOG_INFO("Saved '{}'", ctl.env().relative(ctl.doc().path()));
        ctl.assets().poll(); // our own write is not an external change
    }
    if (ctl.input_map().dirty()) LOG_WARN("--script-commands: the input map was edited but not saved ('input save')");
    LOG_INFO("--script-commands: {} command(s) applied, {} node(s) in '{}'", r.commands, ctl.doc().model().size(),
             ctl.env().relative(ctl.doc().path()));
    return EXIT_SUCCESS;
}

int run_replay_verify_file(const std::string& project, const std::string& replay_file, const std::string& font_path, unsigned jobs) {
    std::string error;
    auto replay = load_replay(platform::path_from_utf8(replay_file), error);
    if (!replay) {
        LOG_ERROR("--replay-verify: {}", error);
        return EXIT_FAILURE;
    }
    script::SessionOptions base;
    base.font_path = font_path;
    base.jobs = jobs;
    const script::ReplayCheck c = script::verify_replay(platform::path_from_utf8(project), *replay, base);
    if (!c.error.empty()) {
        LOG_ERROR("--replay-verify: {}", c.error);
        return EXIT_FAILURE;
    }
    if (c.first_mismatch) {
        LOG_ERROR("--replay-verify: state hash mismatch at tick {} of {}: recorded {:016x}, got {:016x}",
                  *c.first_mismatch, replay->ticks(), c.expected, c.actual);
        return EXIT_FAILURE;
    }
    LOG_INFO("--replay-verify: {} ticks replayed on {} thread(s), all {} state hashes match (final state hash {:016x})",
             c.ticks, jobs::default_jobs().thread_count(), c.compared, c.final_hash);
    return EXIT_SUCCESS;
}

void select_paths(EditorController& ctl, const std::vector<std::string>& paths) {
    std::vector<NodeId> ids;
    for (const std::string& path : paths) {
        if (const NodeId id = ctl.doc().model().find_path(path); id != 0) ids.push_back(id);
        else LOG_WARN("--select: no node at '{}'", path);
    }
    ctl.doc().select_set(ids);
}

} // namespace cuervo::editor

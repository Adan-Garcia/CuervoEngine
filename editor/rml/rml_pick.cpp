// Native file dialogs for the RmlUi shell: Import, Open project, Save As and Open scene each offer a
// "Browse..." button next to the typed path. SDL3 shows the dialog (portal, Cocoa or Win32 behind it)
// and calls back on another thread; the result waits in a mutex-guarded slot until refresh() takes it,
// so no shell state is touched off the main thread. Where no dialog implementation exists (headless),
// SDL reports an error: the shell says so and the typed path keeps working.
#include <SDL3/SDL.h>

#include <mutex>
#include <string>
#include <string_view>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"

namespace cuervo::editor {

namespace {

struct PickResult {
    std::mutex mutex;
    bool done = false;
    int kind = 0;
    std::string path;
    std::string error; // set when SDL could not show a dialog
    bool open = false; // a dialog is up: ignore further requests
};
PickResult g_pick;

void pick_callback(void* userdata, const char* const* list, int) {
    std::lock_guard lock(g_pick.mutex);
    g_pick.kind = static_cast<int>(reinterpret_cast<std::intptr_t>(userdata));
    if (list == nullptr) g_pick.error = SDL_GetError(); // no dialog available (or it failed)
    else if (list[0] != nullptr) g_pick.path = list[0];  // an empty list is "cancelled"
    g_pick.done = true;
}

} // namespace

void Shell::native_pick(PickKind kind) {
    {
        std::lock_guard lock(g_pick.mutex);
        if (g_pick.open) return;
        g_pick.open = true;
    }
    // Headless drivers have nobody to answer (a desktop portal could still pop a dialog on another
    // display), and CUERVO_NO_NATIVE_DIALOGS=1 turns the dialogs off for scripted runs.
    const char* driver = SDL_GetCurrentVideoDriver();
    if (platform::env_is("CUERVO_NO_NATIVE_DIALOGS", "1") || (driver != nullptr && (std::string_view(driver) == "offscreen" || std::string_view(driver) == "dummy"))) {
        std::lock_guard lock(g_pick.mutex);
        g_pick.done = true;
        g_pick.error = "disabled for this video driver";
        return;
    }
    // Start where the user last was: the project for files, the newest recent project for folders.
    std::string start = ctl_.env().has_project() ? platform::utf8(ctl_.env().project_dir()) : std::string();
    if (kind == PickKind::ProjectFolder) {
        const std::vector<RecentProject> list = recents_.list();
        start = list.empty() ? std::string() : list.front().path;
    }
    const char* where = start.empty() ? nullptr : start.c_str();
    void* tag = reinterpret_cast<void*>(static_cast<std::intptr_t>(kind));
    static const SDL_DialogFileFilter scenes[] = {{"Scenes", "scene"}, {"All files", "*"}};
    static const SDL_DialogFileFilter assets[] = {{"Images", "png;jpg;jpeg;qoi;svg;webp"}, {"Sounds", "wav;ogg;flac;mp3"}, {"Fonts", "ttf;otf"}, {"All files", "*"}};
    switch (kind) {
    case PickKind::ProjectFolder: SDL_ShowOpenFolderDialog(pick_callback, tag, host_.window, where, false); break;
    case PickKind::ImportFile: SDL_ShowOpenFileDialog(pick_callback, tag, host_.window, assets, 4, where, false); break;
    case PickKind::OpenScene: SDL_ShowOpenFileDialog(pick_callback, tag, host_.window, scenes, 2, where, false); break;
    case PickKind::SaveScene: SDL_ShowSaveFileDialog(pick_callback, tag, host_.window, scenes, 2, where); break;
    }
}

void Shell::poll_native_pick() {
    PickKind kind;
    std::string path, error;
    {
        std::lock_guard lock(g_pick.mutex);
        if (!g_pick.done) return;
        g_pick.done = g_pick.open = false;
        kind = static_cast<PickKind>(g_pick.kind);
        path = std::exchange(g_pick.path, {});
        error = std::exchange(g_pick.error, {});
    }
    if (!error.empty()) {
        LOG_WARN("native file dialog unavailable: {}", error);
        ctl_.notify_ok("No system file dialog here; type the path instead.");
        return;
    }
    if (path.empty()) return; // cancelled
    switch (kind) {
    case PickKind::ProjectFolder:
        if (dialog_ == Dialog::OpenProject) { project_text_ = path; open_project_confirm(path); }
        else welcome_open(path);
        break;
    case PickKind::ImportFile:
        if (dialog_ != Dialog::AssetImport) break;
        dialog_text_ = path; // the dialog's own confirm imports it and reports errors in place
        dialog_confirm();
        break;
    case PickKind::SaveScene:
        if (dialog_ != Dialog::SaveAs) break;
        dialog_text_ = ctl_.env().relative(platform::path_from_utf8(path)); // project-relative inside the project, absolute outside
        dialog_confirm();
        break;
    case PickKind::OpenScene:
        close_dialog();
        ctl_.request_open_scene(ctl_.env().relative(platform::path_from_utf8(path)));
        break;
    }
}

} // namespace cuervo::editor

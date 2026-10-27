#include "editor/launcher.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <filesystem>
#include <format>
#include <mutex>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "editor/imgui_rhi.hpp"
#include "editor/messages.hpp"
#include "editor/projects.hpp"
#include "editor/splash.hpp"
#include "editor/theme.hpp"
#include "editor/ui.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

namespace {

// SDL_ShowOpenFolderDialog answers from another thread (or later in the event loop).
struct FolderPick {
    std::mutex mutex;
    std::string path;
    bool done = false;
};

void folder_callback(void* userdata, const char* const* list, int) {
    auto* pick = static_cast<FolderPick*>(userdata);
    std::lock_guard lock(pick->mutex);
    if (list != nullptr && list[0] != nullptr) pick->path = list[0];
    pick->done = true;
}

fs::path default_projects_dir() {
    for (const char* var : {"HOME", "USERPROFILE"}) {
        if (auto home = platform::env(var); home && !home->empty()) return fs::path(*home) / "CuervoProjects";
    }
    return fs::path("CuervoProjects");
}

class Launcher {
public:
    explicit Launcher(const LauncherOptions& options) : options_(options) {}
    LauncherResult run();

private:
    bool open(std::string& error);
    void close();
    void frame(double now);
    void draw_welcome_screen();
    void begin_page(float max_width);
    void end_page();
    void open_project(const std::string& path);

    LauncherOptions options_;
    Window window_;
    std::unique_ptr<rhi::Device> device_;
    render::TextRenderer text_;
    Splash splash_;
    ImGuiRenderer renderer_;
    bool imgui_ready_ = false;

    LauncherResult result_;
    bool running_ = true;
    std::uint64_t frames_ = 0;
    ivec2 pixel_size_{1, 1};
    float lockup_center_y_ = 0.0f;
    float lockup_center_x_ = 0.0f;
    float lockup_width_ = 220.0f;

    MessageCenter messages_;
    double clock_ = 0.0;
    RecentProjects recents_;
    FolderPick pick_;
    std::string open_path_;
    std::size_t template_index_ = 0;
    std::string new_name_ = "My Game";
    std::string new_parent_;
};

bool Launcher::open(std::string& error) {
    auto try_open = [&](rhi::Backend backend) -> bool {
        if (!window_.create({.title = "Cuervo Engine",
                             .width = options_.width,
                             .height = options_.height,
                             .graphics = rhi::graphics_api(backend),
                             .gl_es = options_.gl_es})) {
            return false;
        }
        rhi::DeviceOptions o;
        o.vsync = options_.vsync;
        device_ = rhi::create_device(backend, window_, o);
        if (!device_) window_.destroy();
        return device_ != nullptr;
    };
    if (!try_open(options_.backend)) {
        if (!(options_.allow_fallback && options_.backend == rhi::Backend::Vulkan && try_open(rhi::Backend::OpenGL))) {
            error = "could not open a window and render device";
            return false;
        }
        LOG_WARN("Vulkan unavailable, falling back to OpenGL");
    }
    if (!text_.init(*device_)) {
        error = "could not create the text renderer";
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    theme::apply(options_.theme.value_or(theme::system_theme()));
    theme::load_fonts(options_.font_path);
    const std::string brand_dir = platform::utf8(platform::find_asset("brand", CUERVO_BRAND_DIR));
    const std::string display_font = platform::utf8(platform::find_asset("fonts/Outfit-Medium.ttf", CUERVO_FONT_DISPLAY));
    if (!splash_.init(*device_, text_, brand_dir, display_font)) {
        LOG_WARN("Welcome: the brand mark could not be loaded from '{}'", brand_dir);
    }
    if (!ImGui_ImplSDL3_InitForOther(window_.native())) {
        error = "ImGui SDL3 backend failed to start";
        return false;
    }
    imgui_ready_ = true;
    if (!renderer_.init(*device_)) {
        error = "ImGui renderer failed to start";
        return false;
    }
    return true;
}

void Launcher::close() {
    if (imgui_ready_) {
        renderer_.shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    splash_.shutdown();
    text_.shutdown();
    device_.reset();
    window_.destroy();
}

LauncherResult Launcher::run() {
    std::string error;
    if (!open(error)) {
        LOG_ERROR("{}", error);
        close();
        result_.outcome = LauncherOutcome::Failed;
        return result_;
    }
    recents_.load(RecentProjects::user_file());
    new_parent_ = platform::utf8(default_projects_dir());
    while (running_) {
        const double now = static_cast<double>(SDL_GetTicks()) / 1000.0;
        frame(now);
    }
    close();
    return result_;
}

void Launcher::begin_page(float max_width) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::im(theme::colors().bg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    const float w = std::min(max_width, vp->WorkSize.x - 2.0f * theme::space_4);
    lockup_center_x_ = vp->WorkSize.x * 0.5f;
    lockup_center_y_ = 52.0f;
    ImGui::SetCursorPos({(vp->WorkSize.x - w) * 0.5f, 100.0f});
    ImGui::BeginChild("##column", {w, vp->WorkSize.y - 100.0f - theme::space_4}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
}

void Launcher::end_page() {
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor();
}

void Launcher::open_project(const std::string& path) {
    const fs::path dir = platform::path_from_utf8(path);
    std::error_code ec;
    if (!fs::is_regular_file(dir / "project.toml", ec)) {
        messages_.push(message_project_failed(path, "There is no project.toml in that folder."), clock_);
        return;
    }
    result_.outcome = LauncherOutcome::OpenProject;
    result_.project = path;
    running_ = false;
}

void Launcher::draw_welcome_screen() {
    using namespace theme;
    const brand::ThemeColors& t = colors();
    begin_page(980.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(im(t.text_muted), "Open a project to start editing, or begin a new one.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0.0f, space_2});

    // Messages (bad folder, unsaved acceptance) sit above the columns.
    messages_.update(clock_);
    for (auto& item : messages_.items()) {
        ui::message_row(item, ImGui::GetContentRegionAvail().x);
        ImGui::Dummy({0.0f, space_1});
    }

    const float gap = space_4;
    const float total = ImGui::GetContentRegionAvail().x;
    const float left_w = std::floor((total - gap) * 0.55f);
    const float right_w = total - gap - left_w;
    const float height = ImGui::GetContentRegionAvail().y - 28.0f;

    // ---- recent projects and opening a folder
    ImGui::PushStyleColor(ImGuiCol_ChildBg, im(t.surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {space_3, space_3});
    ImGui::BeginChild("##recent", {left_w, height}, ImGuiChildFlags_Borders);
    ui::section_label("Recent projects");
    const std::vector<RecentProject> recent = recents_.list();
    const float list_h = ImGui::GetContentRegionAvail().y - 96.0f;
    if (ImGui::BeginChild("##recent_list", {0.0f, list_h}, ImGuiChildFlags_None)) {
        if (recent.empty()) {
            ImGui::TextColored(im(t.text_muted), "No projects yet. Open a folder or create one.");
        }
        std::string remove;
        for (std::size_t i = 0; i < recent.size(); ++i) {
            const RecentProject& r = recent[i];
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float row_w = ImGui::GetContentRegionAvail().x;
            const bool clicked = ImGui::Selectable("##row", false, r.exists ? 0 : ImGuiSelectableFlags_Disabled, {row_w, 44.0f});
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 name_color = im32(r.exists ? t.text : t.text_muted);
            ui::draw_icon(dl, "folder", {p.x + 16.0f, p.y + 22.0f}, 18.0f, im32(r.exists ? t.accent : t.text_muted));
            dl->AddText({p.x + 36.0f, p.y + 5.0f}, name_color, r.name.c_str());
            const std::string sub = r.exists ? r.path : r.path + "  (not found)";
            dl->PushClipRect({p.x + 36.0f, p.y}, {p.x + row_w - 8.0f, p.y + 44.0f}, true);
            dl->AddText({p.x + 36.0f, p.y + 23.0f}, im32(t.text_muted), sub.c_str());
            dl->PopClipRect();
            if (!r.exists) {
                ImGui::SameLine();
                ImGui::SetCursorScreenPos({p.x + row_w - 70.0f, p.y + 8.0f});
                if (ImGui::SmallButton("Remove")) remove = r.path;
            }
            ImGui::PopID();
            if (clicked) open_project(r.path);
        }
        if (!remove.empty()) recents_.remove(remove);
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::Dummy({0.0f, 2.0f});
    if (ui::icon_label_button("Open Folder...", "folder")) {
        const std::string start = recent.empty() ? std::string{} : recent.front().path;
        SDL_ShowOpenFolderDialog(folder_callback, &pick_, window_.native(), start.empty() ? nullptr : start.c_str(), false);
    }
    ImGui::SameLine();
    ImGui::TextColored(im(t.text_muted), "or type a path");
    ImGui::SetNextItemWidth(-72.0f);
    const bool enter = ImGui::InputTextWithHint("##open_path", "/path/to/project", &open_path_,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Open", {64.0f, 0.0f}) || enter) && !open_path_.empty()) open_project(open_path_);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, gap);

    // ---- new project from a template
    ImGui::PushStyleColor(ImGuiCol_ChildBg, im(t.surface));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {space_3, space_3});
    ImGui::BeginChild("##new", {right_w, height}, ImGuiChildFlags_Borders);
    ui::section_label("New project");
    const auto& templates = project_templates();
    for (std::size_t i = 0; i < templates.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float row_w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##template", template_index_ == i, 0, {row_w, 48.0f})) template_index_ = i;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText({p.x + 8.0f, p.y + 5.0f}, im32(t.text), templates[i].name.c_str());
        dl->PushClipRect({p.x + 8.0f, p.y}, {p.x + row_w - 8.0f, p.y + 48.0f}, true);
        dl->AddText({p.x + 8.0f, p.y + 25.0f}, im32(t.text_muted), templates[i].description.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    ImGui::Dummy({0.0f, space_1});
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##new_name", &new_name_);
    ImGui::TextUnformatted("Create in");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##new_parent", &new_parent_);
    const fs::path target = platform::path_from_utf8(new_parent_) / platform::path_from_utf8(new_name_);
    ImGui::TextColored(im(t.text_muted), "%s", platform::utf8(target).c_str());
    ImGui::Dummy({0.0f, space_1});
    const bool can_create = !new_name_.empty() && !new_parent_.empty() && !templates.empty();
    ImGui::BeginDisabled(!can_create);
    push_primary_button();
    if (ImGui::Button("Create project", {-1.0f, 30.0f})) {
        std::string error;
        if (create_project(templates[template_index_].id, target, new_name_, error)) {
            open_project(platform::utf8(target));
        } else {
            UserMessage m;
            m.severity = Severity::Danger;
            m.what = "The project was not created.";
            m.safety = "Nothing was changed outside the new folder.";
            m.action = "Pick another name or folder.";
            m.details = error;
            messages_.push(std::move(m), clock_);
        }
    }
    pop_primary_button();
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::TextColored(im(t.text_muted), "Cuervo Engine  ·  MIT License");
    end_page();

    std::lock_guard lock(pick_.mutex);
    if (pick_.done) {
        pick_.done = false;
        if (!pick_.path.empty()) {
            const std::string chosen = std::exchange(pick_.path, {});
            open_path_ = chosen;
            open_project(chosen);
        }
    }
}

void Launcher::frame(double now) {
    clock_ = now;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            result_.outcome = LauncherOutcome::Closed;
            running_ = false;
        }
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_welcome_screen();
    ImGui::Render();

    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window_.native(), &w, &h);
    pixel_size_ = {w, h};
    device_->begin_frame(pixel_size_);
    device_->clear(theme::colors().bg);
    renderer_.render(ImGui::GetDrawData());
    splash_.render_lockup(*device_, pixel_size_, theme::current(), {lockup_center_x_, lockup_center_y_}, lockup_width_);
    ++frames_;
    const bool capture = !options_.screenshot.empty() && frames_ >= static_cast<std::uint64_t>(options_.screenshot_frame);
    if (capture) device_->capture_frame(options_.screenshot);
    device_->end_frame();
    if (capture) {
        LOG_INFO("Screenshot written to '{}' ({} frames)", options_.screenshot, frames_);
        result_.outcome = LauncherOutcome::Closed;
        running_ = false;
    }
}

} // namespace

LauncherResult run_launcher(const LauncherOptions& options) {
    Launcher launcher(options);
    return launcher.run();
}

} // namespace cuervo::editor

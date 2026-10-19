// The profiler panel (bottom dock; DESIGN.md 5.14 "a profiler view",
// "Clarified (Phase 8 -- Profiler)"): the editor's frame times and the
// engine zones (cuervo/core/profiler.hpp) of the last frames, and while
// playing, the game's script functions (pluma/profiler.hpp). Recording is
// off until enabled here (or `cuervo-editor --profile`); profiling only
// observes, so the game's state hash is the same either way.

#include <imgui.h>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "cuervo/core/profiler.hpp"
#include "editor/app.hpp"
#include "editor/panels.hpp"

namespace cuervo::editor {

namespace {

void plot(const char* label, const std::vector<float>& values, float height) {
    float max = 1.0f;
    for (float v : values) max = std::max(max, v);
    const std::string overlay =
        values.empty() ? std::string("no frames yet") : std::format("{:.2f} ms (max {:.2f})", values.back(), max);
    ImGui::PlotLines(label, values.data(), static_cast<int>(values.size()), 0, overlay.c_str(), 0.0f, max * 1.1f,
                     ImVec2(-1.0f, height));
}

class ProfilerPanel final : public Panel {
public:
    const char* title() const override { return "Profiler"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        if constexpr (!profiler::compiled_in) {
            ImGui::TextDisabled("Engine zones are compiled out (CUERVO_PROFILING=OFF).");
        }
        PlayController& play = app.play();
        script::GameSession* session = play.session();
        script::ScriptHost* host = play.active() && session != nullptr ? &session->host() : nullptr;

        bool on = profiler::enabled();
        if (ImGui::Checkbox("Record", &on)) profiler::set_enabled(on);
        // Script profiling follows the toggle onto every new play session
        // (each Play makes a new host; hot reloads keep it, ScriptHost).
        if (host != nullptr && host->script_profiling() != on) host->set_script_profiling(on);
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            profiler::reset();
            if (host != nullptr && host->script_profiler() != nullptr) host->script_profiler()->reset();
        }
        ImGui::SameLine();
        const profiler::FrameStats frames = profiler::frame_stats();
        ImGui::TextDisabled("%s", std::format("frame {:.2f} ms avg, {:.2f} max over {} frames", frames.avg_ms,
                                              frames.max_ms, frames.frames)
                                      .c_str());
        if (!on && frames.frames == 0) {
            ImGui::TextWrapped("Record to see the frame times, the engine's zones (simulation, scripts, particles, "
                               "rendering) and, while playing, every script function's time.");
            return;
        }

        const std::vector<profiler::ZoneStats> zones = profiler::zone_stats();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float graph_w = std::max(220.0f, avail.x * 0.24f);
        const float zones_w = std::max(300.0f, avail.x * 0.32f);
        // ---- graphs: frame time and the simulation's share
        if (ImGui::BeginChild("##graphs", ImVec2(graph_w, 0.0f), ImGuiChildFlags_Borders)) {
            const float h = std::max(40.0f, (ImGui::GetContentRegionAvail().y - 2.0f * ImGui::GetTextLineHeightWithSpacing()) * 0.5f);
            ImGui::TextUnformatted("frame ms");
            plot("##frame", profiler::frame_history(), h);
            auto sim = std::find_if(zones.begin(), zones.end(), [](const auto& z) { return std::string_view(z.name) == "tick"; });
            ImGui::TextUnformatted("simulation ms (ticks)");
            plot("##sim", sim != zones.end() ? profiler::zone_history(sim->id) : std::vector<float>{}, h);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        // ---- engine zones
        if (ImGui::BeginChild("##zones", ImVec2(zones_w, 0.0f), ImGuiChildFlags_Borders)) {
            if (ImGui::BeginTable("##zone_table", 4,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("zone", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("avg ms", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("max ms", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("/frame", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (const profiler::ZoneStats& z : zones) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // Indent(0) would mean the default spacing, hence the epsilon.
                    ImGui::Indent(static_cast<float>(z.depth) * 10.0f + 0.001f);
                    ImGui::TextUnformatted(z.name);
                    ImGui::Unindent(static_cast<float>(z.depth) * 10.0f + 0.001f);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", z.avg_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", z.max_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", z.calls_per_frame);
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        // ---- script functions (cumulative since Play or Reset)
        if (ImGui::BeginChild("##scripts", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
            const pluma::ScriptProfiler* sp = host != nullptr ? host->script_profiler() : nullptr;
            if (sp == nullptr) {
                ImGui::TextDisabled(host == nullptr ? "Not playing: script functions show while the game runs."
                                                    : "Script profiling is off.");
            } else {
                const auto functions = sp->snapshot();
                const double total = static_cast<double>(sp->total_ns());
                ImGui::TextDisabled("%s", std::format("script time {:.1f} ms in {} functions (since Play or Reset)",
                                                      total / 1e6, functions.size())
                                              .c_str());
                if (ImGui::BeginTable("##script_table", 5,
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("function", ImGuiTableColumnFlags_WidthStretch, 4.0f);
                    ImGui::TableSetupColumn("calls", ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableSetupColumn("self ms", ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableSetupColumn("incl ms", ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableSetupColumn("% self", ImGuiTableColumnFlags_WidthFixed);
                    ImGui::TableHeadersRow();
                    for (const auto& f : functions) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(f.name.c_str());
                        if (!f.location.empty()) ImGui::SetItemTooltip("%s", f.location.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%llu", static_cast<unsigned long long>(f.calls));
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", f.self_ms());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", f.inclusive_ms());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", total > 0.0 ? 100.0 * static_cast<double>(f.self_ns) / total : 0.0);
                    }
                    ImGui::EndTable();
                }
            }
        }
        ImGui::EndChild();
    }
};

} // namespace

std::unique_ptr<Panel> make_profiler_panel() { return std::make_unique<ProfilerPanel>(); }

} // namespace cuervo::editor

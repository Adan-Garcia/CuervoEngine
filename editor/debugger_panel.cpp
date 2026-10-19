// The debugger panel (right dock, next to the inspector; DESIGN.md 4.13,
// 5.8): the running game's numbers, the panic that paused it (stack trace
// with every frame's locals, from pluma::PanicInfo::frames), and the live
// node tree with each node's field values. Read-only: values are read
// straight from the instances without running script code.

#include <imgui.h>

#include <format>

#include "editor/app.hpp"
#include "editor/theme.hpp"
#include "editor/live_tree.hpp"
#include "editor/panels.hpp"
#include "pluma/heap.hpp"

namespace cuervo::editor {

namespace {

class DebuggerPanel final : public Panel {
public:
    const char* title() const override { return "Debugger"; }
    Dock dock() const override { return Dock::Right; }

    void draw(EditorApp& app) override {
        PlayController& play = app.play();
        script::GameSession* session = play.session();
        if (!play.active() || session == nullptr) {
            ImGui::TextDisabled("Not playing.");
            ImGui::TextWrapped("F5 plays the project, F6 the open scene; this panel then shows the running game.");
            return;
        }
        script::ScriptHost& host = session->host();
        // ---- numbers
        if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_SizingStretchProp)) {
            auto row = [](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", k);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(v.c_str());
            };
            row("state", std::format("{}{}", play.mode() == PlayMode::Replay ? "replay, " : "", play.state_name()));
            row("tick", std::format("{}  ({:.2f} s at {} Hz)", play.tick(),
                                    static_cast<double>(play.tick()) / session->project().tick_rate,
                                    session->project().tick_rate));
            row("sim ms / tick", std::format("{:.3f} (last {:.3f})", play.average_tick_ms(), play.last_tick_ms()));
#if CUERVO_FEATURE_PARTICLES
            row("particles", std::format("{} in {} emitters", host.particles().particle_count(),
                                         host.particles().emitter_count()));
            row("colliders", std::format("{}", host.particles().collision().size()));
#endif
            row("nodes", std::format("{}", live_count(host.root())));
            row("state hash", std::format("{:016x}", host.state_hash()));
            row("hot reloads", std::format("{}", host.reloads()));
            // DESIGN.md 5.10: script heap and cycle collector.
            const pluma::HeapStats heap = pluma::heap_stats();
            row("script objects", std::format("{} live ({} manual), {} suspects", heap.live, heap.manual,
                                              heap.suspects));
            row("cycles collected", std::format("{} objects in {} sets", heap.collected, heap.sets));
            ImGui::EndTable();
        }
        // ---- panic (DESIGN.md 5.8: the game pauses; trace and locals)
        if (const auto& panic = play.panic()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::im(theme::colors().danger));
            ImGui::SeparatorText("Panic");
            ImGui::TextWrapped("%s", panic->message.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Fix the script and save: it hot-reloads and Play resumes. Or Stop.");
            for (std::size_t i = 0; i < panic->frames.size(); ++i) {
                const pluma::PanicFrame& f = panic->frames[i];
                ImGui::PushID(static_cast<int>(i));
                const std::string label = std::format("{}  {}:{}", f.function, f.file, f.line);
                const bool open = ImGui::TreeNodeEx("##frame", i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s",
                                                    label.c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    !f.file.starts_with("cuervo/")) {
                    app.open_location(f.file, f.line);
                }
                ImGui::SetItemTooltip("double-click: show the file (engine prelude frames are cuervo/*.plm)");
                if (open) {
                    if (f.locals.empty()) ImGui::TextDisabled("(no locals)");
                    if (!f.locals.empty() &&
                        ImGui::BeginTable("##locals", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        for (const pluma::PanicLocal& l : f.locals) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(l.name.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextDisabled("%s", l.type.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextWrapped("%s", l.value.c_str());
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        // ---- the live node tree
        ImGui::SeparatorText("Scene tree (live)");
        if (host.session() == nullptr || !host.root().is_obj()) {
            ImGui::TextDisabled("(no root)");
            return;
        }
        node(host.session()->vm(), host.root(), 0);
    }

private:
    void node(pluma::VM& vm, const pluma::Value& v, int depth) {
        if (depth > 64) return;
        const std::vector<pluma::Value> children = live_children(v);
        const std::string name = live_name(v);
        ImGui::PushID(v.o);
        const std::string label = (name.empty() ? live_class(v) : std::format("{}  ({})", name, live_class(v))) +
                                  (children.empty() ? std::string() : std::format("  [{}]", children.size()));
        const bool open = ImGui::TreeNodeEx("##n", depth == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s", label.c_str());
        if (open) {
            if (ImGui::TreeNodeEx("##fields", ImGuiTreeNodeFlags_None, "fields")) {
                if (ImGui::BeginTable("##f", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    for (const LiveField& f : live_fields(vm, v)) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(f.name.c_str());
                        ImGui::SetItemTooltip("%s", f.type.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", f.value.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
            for (const pluma::Value& c : children) node(vm, c, depth + 1);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
};

} // namespace

std::unique_ptr<Panel> make_debugger_panel() { return std::make_unique<DebuggerPanel>(); }

} // namespace cuervo::editor

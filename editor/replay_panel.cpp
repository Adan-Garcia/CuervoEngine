// Replays (bottom dock; DESIGN.md 4.1, 4.8, 4.13): record the inputs of a
// play session, save and load replay files (cuervo/input/replay.hpp), and
// play one back in the scene view with a timeline. Every replayed tick's
// state hash is compared with the recorded one; the first mismatching tick
// is flagged on the timeline. Seeking backwards re-simulates from the start
// (the VM has no snapshots), forward seeks simulate ahead.

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <format>

#include "cuervo/core/log.hpp"
#include "editor/app.hpp"
#include "editor/theme.hpp"
#include "editor/panels.hpp"

namespace cuervo::editor {

namespace {

class ReplayPanel final : public Panel {
public:
    const char* title() const override { return "Replay"; }
    Dock dock() const override { return Dock::Bottom; }

    void draw(EditorApp& app) override {
        PlayController& play = app.play();
        // ---- recording (hidden while a replay plays, to keep the timeline in view)
        const bool replaying = play.mode() == PlayMode::Replay && play.active();
        if (!replaying) ImGui::SeparatorText("Record");
        if (replaying) {
            // nothing: the playback section below
        } else if (play.recording()) {
            ImGui::TextColored(theme::im(theme::colors().danger), "REC");
            ImGui::SameLine();
            ImGui::Text("%llu ticks", static_cast<unsigned long long>(play.recorded().ticks()));
            ImGui::SameLine();
            if (ImGui::Button("Stop Recording")) play.stop_recording();
        } else {
            ImGui::BeginDisabled(play.active());
            if (ImGui::Button("Play and Record")) app.start_play(false, true);
            ImGui::SameLine();
            if (ImGui::Button("Play Scene and Record")) app.start_play(true, true);
            ImGui::EndDisabled();
            ImGui::SameLine();
            const std::uint64_t n = play.recorded().ticks();
            if (n > 0) {
                ImGui::Text("recorded: %llu ticks", static_cast<unsigned long long>(n));
            } else {
                ImGui::TextDisabled("nothing recorded");
            }
        }
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("##path", &path_);
        ImGui::SameLine();
        ImGui::BeginDisabled(play.recorded().frames.empty());
        if (ImGui::Button("Save Recording")) {
            std::string error;
            if (save_replay(app.env().resolve(path_), play.recorded(), error)) {
                LOG_INFO("Replay of {} ticks saved to '{}'", play.recorded().ticks(), path_);
                app.assets().poll();
            } else {
                LOG_ERROR("{}", error);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Load")) app.start_replay(path_);
        ImGui::SameLine();
        const std::vector<std::string> files = app.assets().files_of(AssetKind::Replay);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("##files", "project replays", ImGuiComboFlags_HeightLarge)) {
            if (files.empty()) ImGui::TextDisabled("(none)");
            for (const std::string& f : files) {
                if (ImGui::Selectable(f.c_str(), f == path_)) path_ = f;
            }
            ImGui::EndCombo();
        }

        // ---- playback
        if (!replaying) ImGui::SeparatorText("Playback");
        if (play.mode() != PlayMode::Replay || !play.active()) {
            ImGui::TextDisabled("Load a replay (or double-click one in Assets) to play it back here.");
            return;
        }
        const Replay& r = play.replay();
        const std::uint64_t length = play.replay_length();
        ImGui::Text("'%s', seed %llu, %llu ticks%s%s", r.project.c_str(), static_cast<unsigned long long>(r.seed),
                    static_cast<unsigned long long>(length), r.start_scene.empty() ? "" : ", from ",
                    r.start_scene.c_str());
        if (auto m = play.first_mismatch()) {
            ImGui::TextColored(theme::im(theme::colors().danger),
                               "State hash mismatch at tick %llu: recorded %016llx, replayed %016llx",
                               static_cast<unsigned long long>(*m),
                               static_cast<unsigned long long>(play.mismatch_expected()),
                               static_cast<unsigned long long>(play.mismatch_actual()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Seek there")) seek(play, *m);
        } else if (play.verified() > 0) {
            ImGui::TextColored(theme::im(theme::colors().success), "%llu / %llu ticks verified: every state hash matches",
                               static_cast<unsigned long long>(play.verified()),
                               static_cast<unsigned long long>(std::min<std::uint64_t>(length, r.hashes.size())));
        }
        const bool playing = play.state() == PlayState::Playing;
        if (ImGui::Button(playing ? "Pause" : "Play")) {
            if (playing) {
                play.pause();
            } else {
                play.resume();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Step")) {
            play.pause();
            DeviceState none;
            play.step(none, {0.0f, 0.0f}, 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Verify All")) seek(play, length);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        const float speeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
        if (ImGui::BeginCombo("speed", std::format("{}x", play.speed).c_str())) {
            for (float s : speeds) {
                if (ImGui::Selectable(std::format("{}x", s).c_str(), s == play.speed)) play.speed = s;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            app.stop_play();
            return;
        }

        // The timeline: drag to seek (applied on release), mismatch in red.
        std::uint64_t tick = scrubbing_ ? scrub_ : play.tick();
        const std::uint64_t lo = 0;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderScalar("##timeline", ImGuiDataType_U64, &tick, &lo, &length, "tick %llu")) {
            scrubbing_ = true;
            scrub_ = tick;
        }
        if (scrubbing_ && ImGui::IsItemDeactivated()) {
            scrubbing_ = false;
            seek(play, scrub_);
        }
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = b.x - a.x;
        auto x_at = [&](std::uint64_t t) {
            return a.x + w * static_cast<float>(t) / static_cast<float>(std::max<std::uint64_t>(length, 1));
        };
        // Verified range (green) under the grab; the first mismatch (red).
        dl->AddRectFilled({a.x, b.y - 3.0f}, {x_at(play.verified()), b.y}, theme::im32(brand::with_alpha(theme::colors().success, 0.8f)));
        if (auto m = play.first_mismatch()) {
            const float x = x_at(*m);
            dl->AddRectFilled({x - 1.5f, a.y}, {x + 1.5f, b.y}, theme::im32(theme::colors().danger));
        }

        // This tick's hashes.
        const std::uint64_t t = play.tick();
        if (t >= 1 && t <= r.hashes.size()) {
            const std::uint64_t now = play.state_hash();
            const std::uint64_t rec = r.hashes[t - 1];
            ImGui::Text("tick %llu  recorded %016llx  now %016llx", static_cast<unsigned long long>(t),
                        static_cast<unsigned long long>(rec), static_cast<unsigned long long>(now));
            ImGui::SameLine();
            if (rec == now) {
                ImGui::TextColored(theme::im(theme::colors().success), "match");
            } else {
                ImGui::TextColored(theme::im(theme::colors().danger), "MISMATCH");
            }
        }
        if (t < r.frames.size()) {
            ImGui::TextDisabled("next input: held %016llx  pointer (%d, %d)",
                                static_cast<unsigned long long>(r.frames[t].held), r.frames[t].pointer[0],
                                r.frames[t].pointer[1]);
        }
    }

private:
    static void seek(PlayController& play, std::uint64_t tick) {
        std::string error;
        if (!play.seek(tick, error)) LOG_ERROR("Replay: {}", error);
    }

    std::string path_ = "replays/last.replay";
    bool scrubbing_ = false;
    std::uint64_t scrub_ = 0;
};

} // namespace

std::unique_ptr<Panel> make_replay_panel() { return std::make_unique<ReplayPanel>(); }

} // namespace cuervo::editor

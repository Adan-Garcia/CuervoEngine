// The drawer's Profiler tab: Record / Reset / Trace controls, frame-time and simulation graphs (a custom
// <plot> element drawing bars through Rml geometry), the engine zone table and, while playing, the
// script function table. Same data and switches as the ImGui panel (cuervo/core/profiler.hpp,
// cuervo/profile/trace.hpp, pluma/profiler.hpp); profiling only observes, so state hashes are unchanged.
#include <RmlUi/Core.h>
#include <RmlUi/Core/Mesh.h>

#include <algorithm>
#include <filesystem>
#include <format>

#include "cuervo/core/profiler.hpp"
#include "cuervo/profile/trace.hpp"
#include "cuervo/platform/platform.hpp"
#include "editor/icons.hpp"
#include "editor/rml/rml_dom.hpp"
#include "editor/rml/rml_shell.hpp"
#include "editor/rml_widgets.hpp"
#include "pluma/profiler.hpp"

namespace cuervo::editor {

namespace {

// <plot>: bars for the values the shell bound (oldest left), scaled to the largest.
class PlotView final : public Rml::Element {
public:
    explicit PlotView(const Rml::String& tag_name) : Rml::Element(tag_name) {}
    void bind(const std::vector<float>* values) { values_ = values; }

protected:
    void OnRender() override {
        Rml::RenderManager* rm = GetRenderManager();
        if (rm == nullptr || values_ == nullptr || values_->empty()) return;
        const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
        const Rml::Vector2f origin = GetAbsoluteOffset(Rml::BoxArea::Content);
        float max = 1.0f;
        for (float v : *values_) max = std::max(max, v);
        const Rml::ColourbPremultiplied c(120, 150, 175, 255);
        Rml::Mesh mesh;
        const std::size_t n = values_->size();
        const float bw = size.x / static_cast<float>(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float h = std::max(1.0f, (*values_)[i] / (max * 1.1f) * size.y);
            const float x0 = origin.x + static_cast<float>(i) * bw, x1 = x0 + std::max(bw - 0.5f, 0.5f);
            const float y1 = origin.y + size.y, y0 = y1 - h;
            const int b = static_cast<int>(mesh.vertices.size());
            mesh.vertices.push_back({{x0, y0}, c, {0, 0}});
            mesh.vertices.push_back({{x1, y0}, c, {0, 0}});
            mesh.vertices.push_back({{x1, y1}, c, {0, 0}});
            mesh.vertices.push_back({{x0, y1}, c, {0, 0}});
            for (int k : {0, 1, 2, 0, 2, 3}) mesh.indices.push_back(b + k);
        }
        Rml::Geometry g = rm->MakeGeometry(std::move(mesh));
        g.Render({0.0f, 0.0f});
    }

private:
    const std::vector<float>* values_ = nullptr;
};

std::string cell(const std::string& v, const char* cls = "n") { return std::format("<span class=\"{}\">{}</span>", cls, w::esc(v)); }

} // namespace

void register_plot_view() {
    static Rml::ElementInstancerGeneric<PlotView> instancer;
    Rml::Factory::RegisterElementInstancer("plot", &instancer);
}

void Shell::build_profiler() {
    Rml::Element* e = by_id("drawer-body");
    if (e == nullptr) return;
    const std::uint32_t now = SDL_GetTicks();
    if (now - prof_ms_ < 200 && prof_built_) return; // 5 Hz is plenty for numbers
    prof_ms_ = now;
    PlayController& play = ctl_.play();
    script::GameSession* session = play.session();
    script::ScriptHost* host = play.active() && session != nullptr ? &session->host() : nullptr;
    const bool on = profiler::enabled();
    if (host != nullptr && host->script_profiling() != on) host->set_script_profiling(on); // follows the toggle onto each new play session
    const bool tracing = profile::capturing();
    if (tracing) {
        profile::end_frame();
        profile::add_counter("heap_live", profiler::clock_ns(), static_cast<double>(heap_.sample().live));
    }
    const profiler::FrameStats fs = profiler::frame_stats();
    set_drawer_head(std::format("<span class=\"dh-title\">Profiler</span><span class=\"dh-note mono\">{}</span>",
                                fs.frames == 0 ? std::string("no frames recorded") : std::format("frame {:.2f} ms avg, {:.2f} max, {} frames", fs.avg_ms, fs.max_ms, fs.frames)));

    std::string out = "<div class=\"tl\"><div class=\"tlbar\">";
    out += std::format("<div class=\"btn sm{}\" data-pf=\"record\" title=\"Record frame times, engine zones and script functions\"><span>Record</span></div>", on ? " on" : "");
    out += "<div class=\"btn sm\" data-pf=\"reset\" title=\"Clear the recorded frames\"><span>Reset</span></div>";
    out += std::format("<div class=\"btn sm{}\" data-pf=\"trace\" title=\"Capture a Chrome trace (zones, jobs, draw calls, heap)\"><span>Trace</span></div>", tracing ? " on" : "");
    if (!tracing && !profile::events().empty()) out += "<div class=\"btn sm\" data-pf=\"save\" title=\"Write cuervo_trace.json for ui.perfetto.dev\"><span>Save trace</span></div>";
    if (!prof_saved_.empty()) out += std::format("<span class=\"tlf\">{}</span>", w::esc(prof_saved_));
    out += "</div>";
    if constexpr (!profiler::compiled_in) out += "<div class=\"hint\">Engine zones are compiled out (CUERVO_PROFILING=OFF).</div>";
    if (!on && fs.frames == 0) {
        out += "<div class=\"hint\">Press Record to see frame times, the engine's zones (simulation, scripts, particles, rendering) and, while playing, every script function's time.</div></div>";
        e->SetInnerRML(out);
        e->SetClass("mono", false);
        prof_built_ = true;
        return;
    }
    const std::vector<profiler::ZoneStats> zones = profiler::zone_stats();
    prof_frame_ = profiler::frame_history();
    auto sim = std::find_if(zones.begin(), zones.end(), [](const auto& z) { return std::string_view(z.name) == "tick"; });
    prof_sim_ = sim != zones.end() ? profiler::zone_history(sim->id) : std::vector<float>{};
    float fmax = 0.0f, smax = 0.0f;
    for (float v : prof_frame_) fmax = std::max(fmax, v);
    for (float v : prof_sim_) smax = std::max(smax, v);

    out += "<div class=\"pf\"><div class=\"pfcol pfg\">";
    out += std::format("<div class=\"pfl\"><span>frame ms</span><span class=\"src\">{}</span></div><plot id=\"pf-frame\" class=\"pfplot\"></plot>", prof_frame_.empty() ? std::string("none") : std::format("{:.2f} (max {:.2f})", prof_frame_.back(), fmax));
    out += std::format("<div class=\"pfl\"><span>simulation ms (ticks)</span><span class=\"src\">{}</span></div><plot id=\"pf-sim\" class=\"pfplot\"></plot></div>", prof_sim_.empty() ? std::string("none") : std::format("{:.2f} (max {:.2f})", prof_sim_.back(), smax));
    out += "<div class=\"pfcol pfz scroll\"><div class=\"pfr hd\"><span class=\"nm\">zone</span><span class=\"n\">avg ms</span><span class=\"n\">max ms</span><span class=\"n\">/frame</span></div>";
    for (const profiler::ZoneStats& z : zones) {
        out += std::format("<div class=\"pfr\"><span class=\"nm\" style=\"padding-left: {}dp\">{}</span>{}{}{}</div>", z.depth * 10, w::esc(z.name), cell(std::format("{:.3f}", z.avg_ms)), cell(std::format("{:.3f}", z.max_ms)), cell(std::format("{:.2f}", z.calls_per_frame)));
    }
    out += "</div><div class=\"pfcol pfs scroll\">";
    const pluma::ScriptProfiler* sp = host != nullptr ? host->script_profiler() : nullptr;
    if (sp == nullptr) {
        out += std::format("<div class=\"hint\">{}</div>", host == nullptr ? "Not playing: script functions show while the game runs." : "Script profiling is off.");
    } else {
        const auto functions = sp->snapshot();
        const double total = static_cast<double>(sp->total_ns());
        out += std::format("<div class=\"hint\">script time {:.1f} ms in {} functions (since Play or Reset)</div>", total / 1e6, functions.size());
        out += "<div class=\"pfr hd\"><span class=\"nm\">function</span><span class=\"n\">calls</span><span class=\"n\">self ms</span><span class=\"n\">incl ms</span><span class=\"n\">% self</span></div>";
        std::size_t shown = 0;
        for (const auto& f : functions) {
            if (++shown > 120) break;
            out += std::format("<div class=\"pfr\" title=\"{}\"><span class=\"nm\">{}</span>{}{}{}{}</div>", w::esc(f.location), w::esc(f.name), cell(std::to_string(f.calls)), cell(std::format("{:.3f}", f.self_ms())), cell(std::format("{:.3f}", f.inclusive_ms())),
                               cell(std::format("{:.1f}", total > 0.0 ? 100.0 * static_cast<double>(f.self_ns) / total : 0.0)));
        }
    }
    out += "</div></div></div>";
    e->SetInnerRML(out);
    e->SetClass("mono", false);
    if (auto* p = dynamic_cast<PlotView*>(by_id("pf-frame"))) p->bind(&prof_frame_);
    if (auto* p = dynamic_cast<PlotView*>(by_id("pf-sim"))) p->bind(&prof_sim_);
    prof_built_ = true;
}

bool Shell::profiler_click(Rml::Element* t) {
    for (Rml::Element* e = t; e != nullptr; e = e->GetParentNode()) {
        if (!e->HasAttribute("data-pf")) continue;
        const std::string what = e->GetAttribute<Rml::String>("data-pf", "");
        PlayController& play = ctl_.play();
        script::GameSession* session = play.session();
        script::ScriptHost* host = play.active() && session != nullptr ? &session->host() : nullptr;
        if (what == "record") profiler::set_enabled(!profiler::enabled());
        else if (what == "reset") {
            profiler::reset();
            if (host != nullptr && host->script_profiler() != nullptr) host->script_profiler()->reset();
        } else if (what == "trace") {
            if (profile::capturing()) profile::stop_capture();
            else if (profiler::enabled()) { profile::start_capture(); prof_saved_.clear(); }
            else prof_saved_ = "Record first, then Trace";
        } else if (what == "save") {
            const std::filesystem::path path = std::filesystem::current_path() / "cuervo_trace.json";
            std::string error;
            prof_saved_ = profile::write_chrome_trace(path, &error) ? "saved " + platform::utf8(path) : error;
        }
        prof_built_ = false;
        build_profiler();
        return true;
    }
    return false;
}

} // namespace cuervo::editor

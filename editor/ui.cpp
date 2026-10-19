#include "editor/ui.hpp"


#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "cuervo/vector/flatten.hpp"
#include "editor/icons.hpp"
#include "editor/theme.hpp"

namespace cuervo::editor::ui {

namespace {

struct FlatShape {
    std::vector<std::vector<vec2>> lines; // in the icon's 24x24 box
    std::vector<bool> closed;
    bool fill = false;
};

const std::vector<FlatShape>& flat_icon(std::string_view name, float size) {
    // Curves are flattened for the pixel size they are drawn at (0.25 px tolerance, DESIGN.md 4.5).
    static std::map<std::pair<std::string, int>, std::vector<FlatShape>> cache;
    const int bucket = std::max(8, static_cast<int>(size + 0.5f));
    auto key = std::make_pair(std::string(name), bucket);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const auto* shapes = icon_shapes(name);
    if (shapes == nullptr) shapes = icon_shapes("node");
    std::vector<FlatShape> out;
    const float tolerance = vector::tolerance_for_scale(static_cast<float>(bucket) / 24.0f);
    for (const IconShape& shape : *shapes) {
        std::vector<vector::Polyline> lines;
        vector::flatten(shape.path, tolerance, lines);
        FlatShape flat;
        flat.fill = shape.fill;
        for (vector::Polyline& l : lines) {
            flat.lines.push_back(std::move(l.points));
            flat.closed.push_back(l.closed);
        }
        out.push_back(std::move(flat));
    }
    return cache.emplace(std::move(key), std::move(out)).first->second;
}

} // namespace

void draw_icon(ImDrawList* list, std::string_view name, ImVec2 center, float size, ImU32 color) {
    const std::vector<FlatShape>& shapes = flat_icon(name, size);
    const float k = size / 24.0f;
    const ImVec2 origin{center.x - size * 0.5f, center.y - size * 0.5f};
    const float thickness = std::max(1.3f, size * 0.09f);
    static thread_local std::vector<ImVec2> points;
    for (const FlatShape& shape : shapes) {
        for (std::size_t i = 0; i < shape.lines.size(); ++i) {
            points.clear();
            for (const vec2& p : shape.lines[i]) points.push_back({origin.x + p.x() * k, origin.y + p.y() * k});
            if (points.size() < 2) {
                if (points.size() == 1) list->AddCircleFilled(points[0], thickness * 0.5f, color);
                continue;
            }
            if (shape.fill) {
                list->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
            } else {
                list->AddPolyline(points.data(), static_cast<int>(points.size()), color,
                                  thickness,
                                  shape.closed[i] ? ImDrawFlags_Closed | ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersAll);
            }
        }
    }
}

void icon(std::string_view name, ImU32 color, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = std::max(size, ImGui::GetTextLineHeight());
    ImGui::Dummy({size, h});
    draw_icon(ImGui::GetWindowDrawList(), name, {p.x + size * 0.5f, p.y + h * 0.5f}, size, color);
}

bool icon_button(const char* id, std::string_view icon_name, const char* tooltip, bool selected, bool enabled,
                 float size) {
    const brand::ThemeColors& t = theme::colors();
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##b", {size, size});
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && enabled;
    const bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q{p.x + size, p.y + size};
    if (selected) {
        dl->AddRectFilled(p, q, theme::im32(t.accent_subtle), theme::radius_md);
        dl->AddRect(p, q, theme::im32(brand::with_alpha(t.accent, 0.55f)), theme::radius_md);
    } else if (held) {
        dl->AddRectFilled(p, q, theme::im32(theme::control_active()), theme::radius_md);
    } else if (hovered) {
        dl->AddRectFilled(p, q, theme::im32(theme::control_hover()), theme::radius_md);
    }
    rhi::Color color = selected ? t.accent : t.text_secondary;
    if (hovered && !selected) color = t.text;
    if (!enabled) color = brand::with_alpha(t.text_muted, 0.55f);
    draw_icon(dl, icon_name, {p.x + size * 0.5f, p.y + size * 0.5f}, theme::icon_size, theme::im32(color));
    if (tooltip != nullptr && tooltip[0] != '\0') ImGui::SetItemTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked && enabled;
}

bool icon_label_button(const char* label, std::string_view icon_name, bool enabled) {
    const brand::ThemeColors& t = theme::colors();
    ImGui::PushID(label);
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 text = ImGui::CalcTextSize(label);
    const float h = ImGui::GetFrameHeight();
    const float w = style.FramePadding.x * 2.0f + theme::icon_size + 6.0f + text.x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton("##b", {w, h});
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && enabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    rhi::Color fill = theme::control_fill();
    if (ImGui::IsItemActive()) {
        fill = theme::control_active();
    } else if (hovered) {
        fill = theme::control_hover();
    }
    dl->AddRectFilled(p, {p.x + w, p.y + h}, theme::im32(fill), theme::radius_md);
    dl->AddRect(p, {p.x + w, p.y + h}, theme::im32(t.border), theme::radius_md);
    const rhi::Color ink = enabled ? t.text : brand::with_alpha(t.text_muted, 0.6f);
    draw_icon(dl, icon_name, {p.x + style.FramePadding.x + theme::icon_size * 0.5f, p.y + h * 0.5f}, theme::icon_size,
              theme::im32(enabled ? t.text_secondary : ink));
    dl->AddText({p.x + style.FramePadding.x + theme::icon_size + 6.0f, p.y + (h - text.y) * 0.5f}, theme::im32(ink),
                label);
    ImGui::PopID();
    return clicked && enabled;
}

void toolbar_separator() {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 20.0f;
    ImGui::Dummy({theme::space_2 + 1.0f, h});
    const float x = p.x + theme::space_1;
    ImGui::GetWindowDrawList()->AddLine({x, p.y + 4.0f}, {x, p.y + 4.0f + h}, theme::im32(theme::colors().border));
}

void section_label(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::im(theme::colors().text_muted));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

bool message_row(MessageCenter::Item& item, float width) {
    const brand::ThemeColors& t = theme::colors();
    const UserMessage& m = item.message;
    const rhi::Color accent = theme::severity_color(m.severity);
    bool dismissed = false;
    ImGui::PushID(&item);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::im(theme::severity_subtle(m.severity)));
    ImGui::PushStyleColor(ImGuiCol_Border, theme::im(brand::with_alpha(accent, 0.45f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::radius_md);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::space_3 + 4.0f, theme::space_2});
    const ImVec2 start = ImGui::GetCursorScreenPos();
    if (ImGui::BeginChild("##msg", {width, 0.0f},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar)) {
        const char* name = m.severity == Severity::Danger    ? "error"
                           : m.severity == Severity::Warning ? "warning"
                           : m.severity == Severity::Success ? "check"
                                                             : "info";
        icon(name, theme::im32(accent));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - 100.0f);
        ImGui::TextUnformatted(m.what.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, theme::im(t.text_secondary));
        if (!m.safety.empty()) ImGui::TextUnformatted(m.safety.c_str());
        if (!m.action.empty()) ImGui::TextUnformatted(m.action.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        if (m.has_details()) {
            if (ImGui::TextLink(item.details_open ? "Hide details" : "Details")) item.details_open = !item.details_open;
            if (item.details_open) {
                ImGui::PushFont(theme::mono_font(), theme::mono_font() != nullptr ? theme::mono_font_size : 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::im(t.text_secondary));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - 100.0f);
                ImGui::TextUnformatted(m.details.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::PopFont();
                if (ImGui::SmallButton("Copy details")) ImGui::SetClipboardText(m.details.c_str());
            }
        }
        ImGui::EndGroup();
        ImGui::SetCursorPos({width - 34.0f, theme::space_2});
        if (icon_button("dismiss", "close", "Dismiss", false, true, 24.0f)) dismissed = true;
    }
    ImGui::EndChild();
    // The severity bar on the left edge.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end = ImGui::GetItemRectMax();
    dl->AddRectFilled(start, {start.x + 3.0f, end.y}, theme::im32(accent), theme::radius_md, ImDrawFlags_RoundCornersLeft);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    return dismissed;
}

void highlighted_text(ImDrawList* list, ImVec2 pos, std::string_view text, const std::vector<std::size_t>& positions,
                      ImU32 color, ImU32 highlight) {
    std::size_t next = 0; // index into positions
    std::size_t run_start = 0;
    float x = pos.x;
    auto flush = [&](std::size_t end, bool marked) {
        if (end <= run_start) return;
        const char* b = text.data() + run_start;
        const char* e = text.data() + end;
        list->AddText({x, pos.y}, marked ? highlight : color, b, e);
        x += ImGui::CalcTextSize(b, e).x;
        run_start = end;
    };
    bool marked = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool m = next < positions.size() && positions[next] == i;
        if (m) ++next;
        if (m != marked) {
            flush(i, marked);
            marked = m;
        }
    }
    flush(text.size(), marked);
}

} // namespace cuervo::editor::ui

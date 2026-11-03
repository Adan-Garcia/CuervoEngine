#include "editor/rml_widgets.hpp"

#include <cctype>
#include <format>

namespace cuervo::editor::w {

std::string esc(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (const char c : v) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

std::string icon(std::string_view name, int size, std::string_view cls) {
    return std::format("<img class=\"ic {}\" src=\"icon:{}:{}\"/>", cls, name, size);
}

Tip split_tip(std::string_view title) {
    Tip t{std::string(title), {}};
    if (!title.empty() && title.back() == ')') {
        const std::size_t open = title.rfind(" (");
        if (open != std::string_view::npos && open + 2 < title.size() - 1) {
            t.text = std::string(title.substr(0, open));
            t.keys = std::string(title.substr(open + 2, title.size() - open - 3));
        }
    }
    return t;
}

std::string tip_body(const Tip& t) {
    std::string out = std::format("<span>{}</span>", esc(t.text));
    if (!t.keys.empty()) out += std::format("<span class=\"kbd\">{}</span>", esc(t.keys));
    return out;
}

std::string row(std::string_view data, std::string_view label, std::string_view secondary, const RowOpts& o, std::string_view icon_name, std::string_view lead) {
    std::string out = std::format("<div class=\"row{}{}\" {} style=\"padding-left: {}dp{}\">", o.selected ? " selected" : "", o.disabled ? " disabled" : "", data, 6 + o.indent,
                                  o.top >= 0 ? std::format("; position: absolute; left: 0dp; right: 0dp; top: {}dp", o.top) : std::string());
    out += lead.empty() ? std::string("<span class=\"chev\"></span>") : std::string(lead);
    if (!icon_name.empty()) out += icon(icon_name, 16);
    out += std::format("<span class=\"name\">{}</span>", esc(label));
    if (!secondary.empty()) out += std::format("<span class=\"type\">{}</span>", esc(secondary));
    return out + "</div>";
}

std::string section(std::string_view key, std::string_view title, bool open, std::string_view count, std::string_view tip) {
    return std::format("<div class=\"sect{}\" data-sect=\"{}\"{}>{}<span class=\"st\">{}</span>{}</div>", open ? " open" : "", esc(key),
                       tip.empty() ? std::string() : std::format(" title=\"{}\"", esc(tip)), icon(open ? "chevron_down" : "chevron_right", 12, "chev"), esc(title),
                       count.empty() ? std::string() : std::format("<span class=\"count\">{}</span>", esc(count)));
}

std::string toggle(std::string_view key, bool on, bool disabled) {
    return std::format("<div class=\"tog{}{}\" data-tog=\"{}\"><div class=\"knob\"></div></div>", on ? " on" : "", disabled ? " disabled" : "", esc(key));
}

std::string scrub(std::string_view key, std::string_view text, std::string_view axis, bool modified, bool disabled) {
    std::string tag;
    if (!axis.empty()) tag = std::format("<span class=\"axis ax-{}\">{}</span>", axis, std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(axis[0])))));
    return std::format("<div class=\"scrub{}{}\" data-scrub=\"{}\">{}<span class=\"sv\">{}</span></div>", modified ? " mod" : "", disabled ? " disabled" : "", esc(key), tag, esc(text));
}

std::string text_field(std::string_view key, std::string_view value, std::string_view placeholder, bool clearable, bool mono) {
    return std::format("<div class=\"tf{}\"><input type=\"text\" data-field=\"{}\" value=\"{}\" placeholder=\"{}\"/>{}</div>", mono ? "" : " ui", esc(key), esc(value), esc(placeholder),
                       clearable ? std::format("<div class=\"clear{}\" data-clear=\"{}\" title=\"Clear\">{}</div>", value.empty() ? " hidden" : "", esc(key), icon("close", 12)) : std::string());
}

std::string dropdown(std::string_view key, std::string_view value, bool disabled) {
    return std::format("<div class=\"dd{}\" data-dd=\"{}\"><span class=\"sv\">{}</span>{}</div>", disabled ? " disabled" : "", esc(key), esc(value), icon("chevron_down", 12, "chev"));
}

std::string segmented(std::string_view group, const std::vector<Seg>& items, std::string_view active) {
    std::string out = std::format("<div class=\"seg\" data-seg=\"{}\">", esc(group));
    for (const Seg& s : items) out += std::format("<div class=\"segi{}\" data-segi=\"{}\">{}</div>", s.key == active ? " active" : "", esc(s.key), esc(s.label));
    return out + "</div>";
}

std::string badge(std::string_view text, std::string_view kind) {
    return std::format("<span class=\"tag{}{}\">{}</span>", kind.empty() ? "" : " ", kind, esc(text));
}

std::string popup_menu(std::string_view data_attr, const std::vector<MenuItem>& items) {
    std::string out;
    for (const MenuItem& m : items) {
        if (m.key == "-") { out += "<div class=\"msep\"></div>"; continue; }
        out += std::format("<div class=\"mi{}\" {}=\"{}\"><span class=\"chk\">{}</span><span class=\"lbl\">{}</span><span class=\"kbd\">{}</span></div>", m.disabled ? " off" : "", data_attr,
                           esc(m.key), m.checked ? icon("check", 12) : std::string(), esc(m.label), esc(m.keys));
    }
    return out;
}

std::string prop(std::string_view key, std::string_view label, std::string_view control, const PropOpts& o) {
    return std::format("<div class=\"prop{}{}{}\" data-prop=\"{}\"><label{}><span class=\"dot\"></span><span class=\"lt\">{}</span></label><div class=\"ctl\">{}</div>{}</div>", o.modified ? " mod" : "", o.dim ? " dim" : "",
                       o.disabled ? " disabled" : "", esc(key), o.tip.empty() ? std::string() : std::format(" title=\"{}\"", esc(o.tip)), esc(label), control,
                       o.modified && o.reset ? std::format("<div class=\"rst\" data-reset=\"{}\" title=\"Reset to the class default\">{}</div>", esc(key), icon("reset", 12)) : std::string("<div class=\"rst none\"></div>"));
}

} // namespace cuervo::editor::w

#include "editor/messages.hpp"

#include <algorithm>
#include <format>

namespace cuervo::editor {

std::string UserMessage::sentence() const {
    std::string s = what;
    for (const std::string* part : {&safety, &action}) {
        if (!part->empty()) s += (s.empty() ? "" : " ") + *part;
    }
    return s;
}

UserMessage message_edit_rejected(std::string_view raw) {
    UserMessage m;
    m.severity = Severity::Warning;
    m.what = "That change was not applied.";
    m.safety = "Your scene is unchanged.";
    m.action = "Check the value or the selection and try again.";
    m.details = std::string(raw);
    return m;
}

UserMessage message_save_failed(std::string_view path, std::string_view raw) {
    UserMessage m;
    m.severity = Severity::Danger;
    m.what = std::format("'{}' could not be saved.", path);
    m.safety = "Your changes are still open in the editor and nothing was lost.";
    m.action = "Check that the folder is writable and has space, then save again, or use Save As to pick another place.";
    m.details = std::string(raw);
    return m;
}

UserMessage message_open_failed(std::string_view path, std::string_view raw, bool scene_unchanged) {
    UserMessage m;
    m.severity = Severity::Danger;
    m.what = std::format("'{}' could not be opened.", path);
    m.safety = scene_unchanged ? "The scene you had open is unchanged." : "Nothing was changed.";
    m.action = "The file may be damaged or from a newer Cuervo Engine. Open a backup, or open another file.";
    m.details = std::string(raw);
    return m;
}

UserMessage message_project_failed(std::string_view path, std::string_view raw) {
    UserMessage m;
    m.severity = Severity::Danger;
    m.what = std::format("The project in '{}' could not be opened.", path);
    m.safety = "Nothing in it was changed.";
    m.action = "Pick a folder that has a project.toml, or fix the script error listed under Details and open it again.";
    m.details = std::string(raw);
    return m;
}

UserMessage message_from_log(std::string_view level, std::string_view text) {
    UserMessage m;
    m.severity = level == "error" ? Severity::Danger : level == "warn" ? Severity::Warning : Severity::Info;
    m.details = std::string(text);
    if (m.severity == Severity::Danger) {
        m.what = "Something went wrong.";
        m.safety = "Your open scene is unchanged.";
        m.action = "See Details; the Console panel has the full log.";
    } else {
        m.what = "The editor noticed a problem.";
        m.action = "See Details for what it was.";
    }
    return m;
}

void MessageCenter::push(UserMessage message, double now) {
    // The same message twice in a row is one item (a drag that keeps failing).
    if (!items_.empty() && !items_.back().dismissed && items_.back().message.what == message.what &&
        items_.back().message.details == message.details) {
        items_.back().created = now;
        return;
    }
    items_.push_back({std::move(message), now, false, false});
    if (items_.size() > 6) items_.erase(items_.begin());
}

void MessageCenter::update(double now) {
    std::erase_if(items_, [&](const Item& i) {
        if (i.dismissed) return true;
        const bool transient = i.message.severity == Severity::Info || i.message.severity == Severity::Success;
        return transient && !i.details_open && now - i.created > toast_seconds;
    });
}

} // namespace cuervo::editor

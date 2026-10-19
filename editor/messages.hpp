#pragma once

#include <string>
#include <string_view>
#include <vector>

// User-facing messages in the brand kit's formula (docs/06_Product_UI_Guidelines.md,
// "Error message formula": what happened, whether the work is safe, what to do;
// never a raw error without a sentence, the raw text goes in "Details"). The
// editor's banners and toasts show these; the console keeps the raw log line.
namespace cuervo::editor {

enum class Severity { Info, Success, Warning, Danger };

struct UserMessage {
    Severity severity = Severity::Info;
    std::string what;    // what happened, one sentence
    std::string safety;  // whether work is lost ("Your scene is unchanged.")
    std::string action;  // what to do next ("Pick another name and try again.")
    std::string details; // the raw error text, shown one click away

    // "what safety action", for logs and tests.
    std::string sentence() const;
    bool has_details() const { return !details.empty(); }
};

// A rejected edit (EditorDocument::error()): the scene is untouched.
UserMessage message_edit_rejected(std::string_view raw);
// A file that could not be saved: `unsaved` says the edits are still open in the editor.
UserMessage message_save_failed(std::string_view path, std::string_view raw);
UserMessage message_open_failed(std::string_view path, std::string_view raw, bool scene_unchanged);
UserMessage message_project_failed(std::string_view path, std::string_view raw);
// Anything else: classify the raw text from a log line into a message.
UserMessage message_from_log(std::string_view level, std::string_view text);

// The banners and toasts the window shows, newest last. Success toasts and info
// expire after a few seconds; warnings and danger stay until dismissed (kit 4).
class MessageCenter {
public:
    struct Item {
        UserMessage message;
        double created = 0.0; // seconds, the app's clock
        bool dismissed = false;
        bool details_open = false;
    };
    static constexpr double toast_seconds = 4.0;

    void push(UserMessage message, double now);
    // Drops expired and dismissed items.
    void update(double now);
    std::vector<Item>& items() { return items_; }
    const std::vector<Item>& items() const { return items_; }
    bool empty() const { return items_.empty(); }

private:
    std::vector<Item> items_;
};

} // namespace cuervo::editor

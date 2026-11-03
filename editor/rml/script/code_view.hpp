#pragma once
// <code-view>: the script editor's text surface (AUDIT.md section 15). One Rml element that owns
// scrolling, caret, selection, the line-number gutter and keyboard/mouse editing of a ScriptDoc's
// TextBuffer, and draws only the visible lines through Rml geometry (so Vulkan and GL share one path
// with the rest of the UI). Per-line text geometry is cached by the highlight stamp; no per-line DOM.
// Metrics are integer cell math from the monospace face the element's `font-family` selects.

#include <RmlUi/Core.h>

#include <functional>
#include <unordered_map>
#include <vector>

#include "editor/script_workspace.hpp"

namespace cuervo::editor {

// Registers the <code-view> element; call once after Rml::Initialise.
void register_code_view();

class CodeView final : public Rml::Element, public Rml::EventListener {
public:
    explicit CodeView(const Rml::String& tag_name);
    ~CodeView() override;

    // Outward events; the panel wires these.
    struct Hooks {
        std::function<bool(Rml::Input::KeyIdentifier, bool ctrl, bool shift, bool alt)> key; // first look at every key; true = handled
        std::function<void(TextPos)> ctrl_click;
        std::function<void(TextPos, Rml::Vector2f at)> hover;   // pointer rested over a position (at = absolute px), or left (pos.line < 0)
        std::function<void(const std::string&)> typed;           // text just inserted by typing
        std::function<void()> changed;                           // the buffer changed through this view
        std::function<void()> caret_moved;
        std::function<void(int line)> gutter_click;
    };
    Hooks hooks;

    void set_doc(ScriptWorkspace* ws, int doc_id);
    int doc_id() const { return doc_id_; }
    ScriptDoc* doc() const { return ws_ != nullptr ? ws_->doc(doc_id_) : nullptr; }

    // Scrolls so `p` is visible (center = put it in the middle when it was off screen).
    void reveal(TextPos p, bool center = false);
    void goto_pos(TextPos p, bool center = true) { if (ScriptDoc* d = doc()) { d->buf.set_caret(p); reveal(p, center); blink_reset(); } }
    // The caret's rectangle in absolute px (for popups), and the cell metrics.
    Rml::Rectanglef caret_rect() const;
    Rml::Rectanglef rect_of(TextPos p) const; // one cell at p
    TextPos pos_at(Rml::Vector2f abs_px) const;
    int cell_w() const { return cw_; }
    int line_h() const { return lh_; }
    int first_visible() const { return first_vis_; }
    int last_visible() const { return last_vis_; }
    float scroll_y() const { return scroll_y_; }
    void set_scroll_y(float y) { scroll_y_ = y; clamp_scroll(); }
    bool focused() const { return focused_; }

    // Find highlights (all matches and the current one).
    // The line the debugger stopped on in document `doc_id` (0-based), or -1: drawn as a fill and a gutter arrow.
    void set_paused(int doc_id, int line) { paused_doc_ = doc_id; paused_line_ = line; }
    void set_matches(std::vector<TextRange> m, int current) { matches_ = std::move(m); match_cur_ = current; }
    // True while the caret blinks: the app keeps drawing.
    bool wants_frames() const { return focused_; }

    // Editing entry points the panel also uses (menus, shortcuts).
    void cut();
    void copy();
    void paste();
    void edit_done(); // after a buffer change: scroll to caret, notify

    void ProcessEvent(Rml::Event& e) override;
protected:
    void OnRender() override;
    void ProcessDefaultAction(Rml::Event& e) override;
    void OnResize() override;

private:
    struct LineGeo {
        std::vector<std::pair<Rml::Geometry, Rml::Texture>> parts;
        std::uint64_t used = 0;
    };
    struct Colors {
        Rml::ColourbPremultiplied text, kw, ty, str, num, cm, err, warn, note, gutter, gutter_cur, sel, cur_line, caret, gutter_bg, match, match_cur, fn, cst;
        bool operator==(const Colors&) const = default;
    };

    void measure();
    void read_colors();
    void clamp_scroll();
    void blink_reset();
    int gutter_w() const;
    float text_x() const; // absolute px of column 0
    Rml::Vector2f origin() const;
    double now() const;
    std::uint64_t now_ms() const { return static_cast<std::uint64_t>(now() * 1000.0); }
    Rml::ColourbPremultiplied color_of(Syn k) const;
    LineGeo& line_geo(ScriptDoc& d, int line);
    LineGeo& number_geo(int line, bool current);
    bool key_down(Rml::Event& e);
    void mouse_down(Rml::Event& e);
    void drag_to(Rml::Event& e);
    void do_move(Move m, bool extend);

    ScriptWorkspace* ws_ = nullptr;
    int doc_id_ = -1;
    std::uint64_t seen_rev_ = ~std::uint64_t{0};
    std::vector<TextRange> matches_;
    int match_cur_ = -1;
    int paused_doc_ = -1, paused_line_ = -1;

    Rml::FontFaceHandle face_ = 0;
    int face_version_ = -1;
    int cw_ = 8, lh_ = 20, ascent_ = 15;
    float ratio_ = 1.0f;
    float scroll_x_ = 0, scroll_y_ = 0;
    int first_vis_ = 0, last_vis_ = 0;
    bool focused_ = false;
    double blink_t0_ = 0;
    Colors colors_{};
    bool have_colors_ = false;
    std::unordered_map<std::uint64_t, LineGeo> geo_;
    std::uint64_t frame_ = 0;
    // Mouse state.
    double last_click_t_ = -10;
    Rml::Vector2f last_click_at_{};
    int click_count_ = 0;
    bool dragging_ = false;
    TextPos drag_anchor_;
    int drag_mode_ = 0; // 0 chars, 1 words, 2 lines
    bool line_clip_ = false; // the clipboard holds a copied line (no selection)
    std::string line_clip_text_;
};

} // namespace cuervo::editor

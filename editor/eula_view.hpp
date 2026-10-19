#pragma once

// The license agreement as a view (DESIGN.md 4.16 "EULA agreement"): the plain-English
// summary on top, the scrollable agreement and THIRD_PARTY_NOTICES.md below, and an
// unchecked "I have read and agree" box with Accept and Decline. Shared by the
// first-launch screen (interactive) and Help > License Agreement (read-only).
namespace cuervo::editor {

struct EulaView {
    bool agreed = false;   // the check box; never starts checked
    bool notices = false;  // showing THIRD_PARTY_NOTICES.md instead of the agreement
};

enum class EulaChoice { Pending, Accepted, Declined };

// Draws into the current window, filling its content region. `interactive` adds the
// check box and the buttons; Accept stays disabled until the box is checked, and the
// two buttons are the same size and weight (the user must be able to decline).
EulaChoice draw_eula(EulaView& view, bool interactive);

} // namespace cuervo::editor

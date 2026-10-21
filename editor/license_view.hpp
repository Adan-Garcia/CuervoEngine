#pragma once

namespace cuervo::editor {

struct LicenseView {
    bool notices = false; // showing THIRD_PARTY_NOTICES.md instead of LICENSE.md
};

// Draws the license dialog (tabs for License and Third-party notices).
void draw_license_dialog(LicenseView& view);

} // namespace cuervo::editor

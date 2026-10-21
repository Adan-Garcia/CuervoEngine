#pragma once

#include <string_view>

namespace cuervo::license {

// LICENSE.md and THIRD_PARTY_NOTICES.md, embedded.
std::string_view license_text();
std::string_view notices_text();

} // namespace cuervo::license

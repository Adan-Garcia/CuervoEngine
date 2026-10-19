#include <cstddef>

#include "cuervo/platform/eula.hpp"

// Kept apart from eula.cpp so programs that never show the agreement (the web
// player, exported games) don't link ~45 KB of text.
namespace cuervo::eula {

namespace {
constexpr unsigned char license_bytes[] = {
#include "cuervo/platform/eula_license.inc"
};
constexpr unsigned char notices_bytes[] = {
#include "cuervo/platform/eula_notices.inc"
};
} // namespace

std::string_view license_text() {
    return {reinterpret_cast<const char*>(license_bytes), sizeof(license_bytes) - 1};
}

std::string_view notices_text() {
    return {reinterpret_cast<const char*>(notices_bytes), sizeof(notices_bytes) - 1};
}

} // namespace cuervo::eula

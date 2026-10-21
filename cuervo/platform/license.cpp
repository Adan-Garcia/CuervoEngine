#include "cuervo/platform/license.hpp"

#include <cstddef>

namespace cuervo::license {

namespace {
constexpr unsigned char license_bytes[] = {
#include "cuervo/platform/license_text.inc"
};
constexpr unsigned char notices_bytes[] = {
#include "cuervo/platform/notices_text.inc"
};
} // namespace

std::string_view license_text() {
    return {reinterpret_cast<const char*>(license_bytes), sizeof(license_bytes) - 1};
}

std::string_view notices_text() {
    return {reinterpret_cast<const char*>(notices_bytes), sizeof(notices_bytes) - 1};
}

} // namespace cuervo::license

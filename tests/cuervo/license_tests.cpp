#include <doctest.h>

#include <string_view>

#include "cuervo/platform/license.hpp"

using namespace cuervo;

TEST_CASE("the license text comes from LICENSE.md") {
    CHECK(license::license_text().find("MIT License") != std::string_view::npos);
    CHECK(license::license_text().find("Copyright (c) 2026 Adan Garcia Morataya") != std::string_view::npos);
    CHECK(license::notices_text().find("Third") != std::string_view::npos);
}

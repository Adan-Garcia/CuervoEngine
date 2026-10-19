#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

#include "cuervo/math/dmath.hpp"

using namespace cuervo;

TEST_CASE("dmath sin/cos stay close to libm") {
    float max_error = 0.0f;
    for (int i = -20000; i <= 20000; ++i) {
        float x = static_cast<float>(i) * 0.0025f; // [-50, 50]
        max_error = std::max(max_error, std::fabs(dmath::sin(x) - std::sin(x)));
        max_error = std::max(max_error, std::fabs(dmath::cos(x) - std::cos(x)));
    }
    CHECK(max_error < 2e-5f);
}

TEST_CASE("dmath exact values") {
    CHECK(dmath::sin(0.0f) == 0.0f);
    CHECK(dmath::cos(0.0f) == 1.0f);
    CHECK(dmath::sqrt(16.0f) == 4.0f);
    CHECK(dmath::sin(dmath::half_pi) == doctest::Approx(1.0f).epsilon(1e-6));
    CHECK(dmath::cos(dmath::pi) == doctest::Approx(-1.0f).epsilon(1e-6));
}

TEST_CASE("dmath atan2 stays close to libm in every quadrant") {
    float max_error = 0.0f;
    for (int i = 0; i < 3600; ++i) {
        const float angle = static_cast<float>(i) * (dmath::tau / 3600.0f) - dmath::pi;
        for (float radius : {0.001f, 1.0f, 250.0f}) {
            const float y = std::sin(angle) * radius;
            const float x = std::cos(angle) * radius;
            max_error = std::max(max_error, std::fabs(dmath::atan2(y, x) - std::atan2(y, x)));
        }
    }
    CHECK(max_error < 1e-6f);
    CHECK(dmath::atan2(0.0f, 0.0f) == 0.0f);
    CHECK(dmath::atan2(0.0f, 1.0f) == 0.0f);
    CHECK(dmath::atan2(1.0f, 0.0f) == doctest::Approx(dmath::half_pi).epsilon(1e-7));
    CHECK(dmath::atan2(-1.0f, 0.0f) == doctest::Approx(-dmath::half_pi).epsilon(1e-7));
    CHECK(dmath::atan2(0.0f, -1.0f) == doctest::Approx(dmath::pi).epsilon(1e-7));
}

TEST_CASE("dmath tan stays close to libm away from asymptotes") {
    float max_error = 0.0f;
    for (int i = -1400; i <= 1400; ++i) {
        float x = static_cast<float>(i) * 0.001f; // [-1.4, 1.4], away from +/- pi/2
        max_error = std::max(max_error, std::fabs(dmath::tan(x) - std::tan(x)));
    }
    CHECK(max_error < 5e-5f);
    CHECK(dmath::tan(0.0f) == 0.0f);
    CHECK(dmath::tan(dmath::pi * 0.25f) == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(dmath::tan(-dmath::pi * 0.25f) == doctest::Approx(-1.0f).epsilon(1e-5));
}

TEST_CASE("dmath asin and acos stay close to libm on [-1, 1]") {
    float max_asin_err = 0.0f;
    float max_acos_err = 0.0f;
    for (int i = -999; i <= 999; ++i) {
        float x = static_cast<float>(i) * 0.001f;
        max_asin_err = std::max(max_asin_err, std::fabs(dmath::asin(x) - std::asin(x)));
        max_acos_err = std::max(max_acos_err, std::fabs(dmath::acos(x) - std::acos(x)));
    }
    CHECK(max_asin_err < 1e-6f);
    CHECK(max_acos_err < 1e-6f);

    CHECK(dmath::asin(0.0f) == 0.0f);
    CHECK(dmath::asin(1.0f) == doctest::Approx(dmath::half_pi).epsilon(1e-7));
    CHECK(dmath::asin(-1.0f) == doctest::Approx(-dmath::half_pi).epsilon(1e-7));

    CHECK(dmath::acos(1.0f) == 0.0f);
    CHECK(dmath::acos(0.0f) == doctest::Approx(dmath::half_pi).epsilon(1e-7));
    CHECK(dmath::acos(-1.0f) == doctest::Approx(dmath::pi).epsilon(1e-7));

    CHECK(std::isnan(dmath::asin(1.01f)));
    CHECK(std::isnan(dmath::asin(-1.5f)));
    CHECK(std::isnan(dmath::acos(2.0f)));
    CHECK(std::isnan(dmath::acos(-1.001f)));
}

TEST_CASE("dmath atan stays close to libm") {
    float max_err = 0.0f;
    for (int i = -1000; i <= 1000; ++i) {
        float x = static_cast<float>(i) * 0.05f; // [-50, 50]
        max_err = std::max(max_err, std::fabs(dmath::atan(x) - std::atan(x)));
    }
    CHECK(max_err < 1e-6f);
    CHECK(dmath::atan(0.0f) == 0.0f);
    CHECK(dmath::atan(1.0f) == doctest::Approx(dmath::pi * 0.25f).epsilon(1e-7));
    CHECK(dmath::atan(-1.0f) == doctest::Approx(-dmath::pi * 0.25f).epsilon(1e-7));
}

TEST_CASE("dmath exp stays close to libm") {
    float max_rel_err = 0.0f;
    for (int i = -400; i <= 400; ++i) {
        float x = static_cast<float>(i) * 0.1f; // [-40, 40]
        float expected = std::exp(x);
        float actual = dmath::exp(x);
        max_rel_err = std::max(max_rel_err, std::fabs(actual - expected) / expected);
    }
    CHECK(max_rel_err < 5e-6f);
    CHECK(dmath::exp(0.0f) == 1.0f);
    CHECK(dmath::exp(1.0f) == doctest::Approx(dmath::e).epsilon(1e-6));
    CHECK(dmath::exp(100.0f) == std::numeric_limits<float>::infinity());
    CHECK(dmath::exp(-110.0f) == 0.0f);
}

TEST_CASE("dmath log, log2, log10 stay close to libm") {
    float max_log_rel_err = 0.0f;
    float max_log2_rel_err = 0.0f;
    float max_log10_rel_err = 0.0f;
    for (int i = -100; i <= 100; ++i) {
        float x = std::exp(static_cast<float>(i) * 0.5f);
        if (std::fabs(std::log(x)) > 1e-3f) {
            max_log_rel_err = std::max(max_log_rel_err, std::fabs(dmath::log(x) - std::log(x)) / std::fabs(std::log(x)));
            max_log2_rel_err = std::max(max_log2_rel_err, std::fabs(dmath::log2(x) - std::log2(x)) / std::fabs(std::log2(x)));
            max_log10_rel_err = std::max(max_log10_rel_err, std::fabs(dmath::log10(x) - std::log10(x)) / std::fabs(std::log10(x)));
        }
    }
    CHECK(max_log_rel_err < 5e-6f);
    CHECK(max_log2_rel_err < 5e-6f);
    CHECK(max_log10_rel_err < 5e-6f);

    CHECK(dmath::log(1.0f) == 0.0f);
    CHECK(dmath::log(dmath::e) == doctest::Approx(1.0f).epsilon(1e-6));
    CHECK(dmath::log2(1.0f) == 0.0f);
    CHECK(dmath::log2(4.0f) == doctest::Approx(2.0f).epsilon(1e-6));
    CHECK(dmath::log2(1024.0f) == doctest::Approx(10.0f).epsilon(1e-6));
    CHECK(dmath::log10(1.0f) == 0.0f);
    CHECK(dmath::log10(1000.0f) == doctest::Approx(3.0f).epsilon(1e-6));

    CHECK(std::isnan(dmath::log(-1.0f)));
    CHECK(std::isinf(dmath::log(0.0f)));
}

TEST_CASE("dmath pow stays close to libm") {
    CHECK(dmath::pow(2.0f, 3.0f) == doctest::Approx(8.0f).epsilon(1e-5));
    CHECK(dmath::pow(10.0f, 2.0f) == doctest::Approx(100.0f).epsilon(1e-5));
    CHECK(dmath::pow(5.0f, 0.0f) == 1.0f);
    CHECK(dmath::pow(0.0f, 0.0f) == 1.0f);
    CHECK(dmath::pow(0.0f, 3.0f) == 0.0f);
    CHECK(dmath::pow(1.0f, 100.0f) == 1.0f);
    CHECK(dmath::pow(4.0f, 0.5f) == doctest::Approx(2.0f).epsilon(1e-5));

    // Negative base with integer exponent
    CHECK(dmath::pow(-2.0f, 3.0f) == doctest::Approx(-8.0f).epsilon(1e-5));
    CHECK(dmath::pow(-2.0f, 4.0f) == doctest::Approx(16.0f).epsilon(1e-5));
    // Huge integer exponents: even (no out-of-range int conversion), so |base| < 1 -> +0.
    CHECK(dmath::pow(-0.5f, 1.0e30f) == 0.0f);
    CHECK(dmath::pow(-0.5f, -1.0e30f) == std::numeric_limits<float>::infinity());
    CHECK(dmath::pow(-1.5f, 3.0f) == doctest::Approx(-3.375f).epsilon(1e-5));
    // Negative base with non-integer exponent -> NaN
    CHECK(std::isnan(dmath::pow(-2.0f, 0.5f)));
}

TEST_CASE("dmath scalar utilities") {
    CHECK(dmath::sign(5.5f) == 1.0f);
    CHECK(dmath::sign(-4.2f) == -1.0f);
    CHECK(dmath::sign(0.0f) == 0.0f);

    CHECK(dmath::clamp(5.0f, 0.0f, 2.0f) == 2.0f);
    CHECK(dmath::clamp(-1.0f, 0.0f, 2.0f) == 0.0f);
    CHECK(dmath::clamp(1.5f, 0.0f, 2.0f) == 1.5f);

    CHECK(dmath::min(3.0f, 7.0f) == 3.0f);
    CHECK(dmath::max(3.0f, 7.0f) == 7.0f);

    CHECK(dmath::ceil(2.3f) == 3.0f);
    CHECK(dmath::round(2.5f) == 3.0f);
    CHECK(dmath::trunc(-2.7f) == -2.0f);

    CHECK(dmath::lerp(10.0f, 20.0f, 0.5f) == 15.0f);
    CHECK(dmath::inverse_lerp(10.0f, 20.0f, 15.0f) == 0.5f);
    CHECK(dmath::inverse_lerp(10.0f, 10.0f, 10.0f) == 0.0f);
    CHECK(dmath::remap(0.0f, 10.0f, 100.0f, 200.0f, 2.5f) == 125.0f);

    CHECK(dmath::step(5.0f, 3.0f) == 0.0f);
    CHECK(dmath::step(5.0f, 5.0f) == 1.0f);
    CHECK(dmath::step(5.0f, 6.0f) == 1.0f);

    CHECK(dmath::smoothstep(0.0f, 10.0f, 0.0f) == 0.0f);
    CHECK(dmath::smoothstep(0.0f, 10.0f, 10.0f) == 1.0f);
    CHECK(dmath::smoothstep(0.0f, 10.0f, 5.0f) == 0.5f);

    CHECK(dmath::move_toward(0.0f, 10.0f, 3.0f) == 3.0f);
    CHECK(dmath::move_toward(5.0f, 6.0f, 2.0f) == 6.0f);
    CHECK(dmath::move_toward(10.0f, 0.0f, 4.0f) == 6.0f);

    CHECK(dmath::rad_to_deg(dmath::pi) == doctest::Approx(180.0f).epsilon(1e-6));
    CHECK(dmath::deg_to_rad(180.0f) == doctest::Approx(dmath::pi).epsilon(1e-6));

    CHECK(dmath::wrap(7.0f, 0.0f, 5.0f) == doctest::Approx(2.0f).epsilon(1e-6));
    CHECK(dmath::wrap(-1.0f, 0.0f, 5.0f) == doctest::Approx(4.0f).epsilon(1e-6));
    CHECK(dmath::wrap_angle(dmath::tau + 0.5f) == doctest::Approx(0.5f).epsilon(1e-6));
}

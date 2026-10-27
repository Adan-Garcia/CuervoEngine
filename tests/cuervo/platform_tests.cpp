// Platform checks that differ by compiler and OS (docs/WINDOWS.md checklist):
// no FMA contraction in compiled arithmetic, and on Windows the manifest's
// UTF-8 code page and DPI awareness plus the pointer mapping at the desktop's
// real scale.

#if defined(_WIN32) && (!defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // the DPI-awareness context API (Windows 10); MinGW-w64 defaults lower
#endif

#include <doctest.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "cuervo/input/devices.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/platform/window.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX // MinGW-w64's libstdc++ defines it already
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace cuervo;

TEST_CASE("platform: a * b + c rounds twice (no FMA contraction)") {
    // DESIGN.md 4.1: sim arithmetic must round each operation, so every
    // compiler must keep a*b+c as a multiply then an add (-ffp-contract=off,
    // MSVC /fp:precise without /fp:contract). With a = 1 + 2^-28 and
    // b = 1 - 2^-28 the exact product is 1 - 2^-56, which rounds to 1.0; a
    // fused multiply-add keeps the 2^-56 and gives -2^-56 instead of 0 (floats:
    // 2^-13, a 2^-26 error below float precision).
    volatile double a = 1.0 + std::ldexp(1.0, -28);
    volatile double b = 1.0 - std::ldexp(1.0, -28);
    volatile double c = -1.0;
    const double r = a * b + c;
    CHECK(r == 0.0);
    volatile float af = 1.0f + std::ldexp(1.0f, -13);
    volatile float bf = 1.0f - std::ldexp(1.0f, -13);
    volatile float cf = -1.0f;
    const float rf = af * bf + cf;
    CHECK(rf == 0.0f);
}

#if defined(_WIN32)
TEST_CASE("platform: Windows programs run with the UTF-8 code page and per-monitor DPI awareness") {
    // cmake/windows/cuervo.manifest, embedded by cuervo_windows_app.
    CHECK(GetACP() == CP_UTF8);
    CHECK(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) == DPI_AWARENESS_PER_MONITOR_AWARE);

    // So a narrow (UTF-8) file name reaches the file system intact.
    const std::string name = "cuervo_platform_\xC3\xB1\x61nd\xC3\xBA.txt"; // "ñandú"
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string narrow = platform::utf8(dir) + "/" + name;
    {
        std::ofstream f(narrow.c_str(), std::ios::binary); // the char* overload: the active code page
        REQUIRE(f.good());
        f << "ok";
    }
    CHECK(std::filesystem::exists(dir / platform::path_from_utf8(name)));
    std::filesystem::remove(dir / platform::path_from_utf8(name));
}

TEST_CASE("platform: pointer positions are framebuffer pixels at the desktop's scale") {
    // docs/WINDOWS.md 7: at 125 % / 150 % scaling the pointer the game sees
    // must land on the pixel under the cursor. Opens a real window and moves
    // the cursor (put back afterwards).
    Window window;
    if (!window.create({.title = "platform_tests", .width = 640, .height = 360, .brand_icon = false})) {
        MESSAGE("no window: skipped");
        return;
    }
    SDL_Window* w = window.native();
    int points_w = 0, points_h = 0;
    SDL_GetWindowSize(w, &points_w, &points_h);
    const ivec2 pixels = window.pixel_size();
    const float density = SDL_GetWindowPixelDensity(w);
    MESSAGE("display scale ", SDL_GetWindowDisplayScale(w), ", pixel density ", density, ", ", points_w, "x",
            points_h, " points, ", pixels.x(), "x", pixels.y(), " pixels");
    CHECK(pixels.x() == doctest::Approx(points_w * density).epsilon(0.01));
    CHECK(pixels.y() == doctest::Approx(points_h * density).epsilon(0.01));

    POINT saved{};
    GetCursorPos(&saved);
    SDL_RaiseWindow(w);
    DeviceState devices;
    window.poll_events(devices);
    const float px = static_cast<float>(points_w) * 0.75f, py = static_cast<float>(points_h) * 0.25f;
    SDL_WarpMouseInWindow(w, px, py);
    for (int i = 0; i < 20; ++i) {
        SDL_Delay(5);
        window.poll_events(devices);
    }
    // The OS's own answer in physical pixels (the process is DPI-aware).
    POINT cursor{};
    GetCursorPos(&cursor);
    HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    REQUIRE(hwnd != nullptr);
    ScreenToClient(hwnd, &cursor);
    SetCursorPos(saved.x, saved.y);

    CHECK(std::abs(devices.mouse_position.x() - static_cast<float>(cursor.x)) <= 1.0f);
    CHECK(std::abs(devices.mouse_position.y() - static_cast<float>(cursor.y)) <= 1.0f);
    // Where the warp put it, unless someone moved the mouse meanwhile (a real desktop).
    if (std::abs(static_cast<float>(cursor.x) - px * density) <= 1.0f &&
        std::abs(static_cast<float>(cursor.y) - py * density) <= 1.0f) {
        CHECK(std::abs(devices.mouse_position.x() - px * density) <= 1.0f);
        CHECK(std::abs(devices.mouse_position.y() - py * density) <= 1.0f);
    } else {
        MESSAGE("the cursor moved during the test; compared with the OS position only");
    }
}
#endif

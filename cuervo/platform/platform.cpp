#include "cuervo/platform/platform.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <system_error>
#include <thread>

namespace cuervo::platform {

std::string_view os_name() {
    switch (os) {
        case Os::Linux:   return "Linux";
        case Os::Windows: return "Windows";
        case Os::Web:     return "Web";
        case Os::Other:   return "other";
    }
    return "other";
}

std::optional<std::string> env(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read at startup only
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

bool env_is(const char* name, std::string_view value) {
    const std::optional<std::string> v = env(name);
    return v && *v == value;
}

unsigned hardware_threads() {
    if constexpr (!has_threads) {
        return 1;
    } else {
        return std::max(1u, std::thread::hardware_concurrency());
    }
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::filesystem::path executable_dir() {
    const char* base = SDL_GetBasePath(); // owned by SDL (SDL3)
    if (base == nullptr) return {};
    auto p = path_from_utf8(base);
    while (p.has_relative_path() && p.filename().empty()) {
        p = p.parent_path();
    }
    return p;
}

std::filesystem::path find_asset(std::string_view subpath, std::string_view fallback) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // 1. Environment variable override
    if (auto dir = env("CUERVO_ASSETS_DIR"); dir && !dir->empty()) {
        auto p = path_from_utf8(*dir) / path_from_utf8(subpath);
        if (fs::exists(p, ec)) return p;
    }
    // 2. Compiled fallback if it exists (e.g. running from repo during development)
    if (!fallback.empty()) {
        auto p = path_from_utf8(fallback);
        if (fs::exists(p, ec)) return p;
    }
    // 3. Search upwards from the executable directory: <dir>/assets/<subpath>
    const auto exe = executable_dir();
    if (!exe.empty()) {
        for (fs::path cur = exe; !cur.empty(); ) {
            auto p = cur / "assets" / path_from_utf8(subpath);
            if (fs::exists(p, ec)) return p;
            auto parent = cur.parent_path();
            if (parent == cur) break;
            cur = std::move(parent);
        }

        // Standard install prefix: <exe_dir>/../share/cuervo/assets/<subpath>
        auto p_share = exe.parent_path() / "share" / "cuervo" / "assets" / path_from_utf8(subpath);
        if (fs::exists(p_share, ec)) return p_share;
    }
    // 4. Search upwards from current working directory: <dir>/assets/<subpath>
    for (fs::path cur = fs::current_path(ec); !ec && !cur.empty(); ) {
        auto p = cur / "assets" / path_from_utf8(subpath);
        if (fs::exists(p, ec)) return p;
        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = std::move(parent);
    }

    if (!fallback.empty()) return path_from_utf8(fallback);
    if (!exe.empty()) return exe.parent_path() / "assets" / path_from_utf8(subpath);
    return path_from_utf8(subpath);
}

std::filesystem::path user_data_dir(std::string_view game) {
    if (auto dir = env("CUERVO_USER_DIR"); dir && !dir->empty()) {
        std::filesystem::path p = path_from_utf8(*dir);
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }
    // Keep the folder name portable: letters, digits, ' ', '-', '_' and '.'.
    std::string app;
    for (char c : game) {
        const auto u = static_cast<unsigned char>(c);
        app += (std::isalnum(u) != 0 || c == ' ' || c == '-' || c == '_' || c == '.' || u >= 0x80) ? c : '_';
    }
    if (app.empty()) app = "Game";
    char* pref = SDL_GetPrefPath("Cuervo", app.c_str());
    if (pref == nullptr) return {};
    std::filesystem::path p = path_from_utf8(pref);
    SDL_free(pref);
    return p;
}

} // namespace cuervo::platform

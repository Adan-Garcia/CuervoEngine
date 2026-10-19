#include "eula_gate.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <iostream>

#if defined(_WIN32)
#include <io.h>
#define PLM_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define PLM_ISATTY(fd) isatty(fd)
#endif

namespace plm_eula {

namespace {

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    return std::string(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
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
    const char* v = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (v == nullptr) return std::nullopt;
    return std::string(v);
#endif
}

std::string today() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return std::format("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

bool write(const std::filesystem::path& file, std::string_view version) {
    if (file.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << "# Cuervo Engine License Agreement (LICENSE.md): accepted by this user.\n"
        << "version=" << version << "\naccepted=" << today() << "\n";
    out.flush();
    return static_cast<bool>(out);
}

} // namespace

std::filesystem::path acceptance_file() {
    // CUERVO_USER_DIR, else SDL_GetPrefPath("Cuervo", "Cuervo Engine") by hand.
    std::filesystem::path dir;
    if (auto d = env("CUERVO_USER_DIR"); d && !d->empty()) {
        dir = std::filesystem::path(std::u8string(d->begin(), d->end()));
    } else {
#if defined(_WIN32)
        if (auto base = env("APPDATA")) dir = std::filesystem::path(std::u8string(base->begin(), base->end())) / "Cuervo" / "Cuervo Engine";
#elif defined(__APPLE__)
        if (auto home = env("HOME")) dir = std::filesystem::path(*home) / "Library/Application Support/Cuervo/Cuervo Engine";
#else
        if (auto xdg = env("XDG_DATA_HOME"); xdg && !xdg->empty()) {
            dir = std::filesystem::path(*xdg) / "Cuervo" / "Cuervo Engine";
        } else if (auto home = env("HOME")) {
            dir = std::filesystem::path(*home) / ".local/share/Cuervo/Cuervo Engine";
        }
#endif
    }
    return dir.empty() ? dir : dir / "eula-accepted.txt";
}

bool accepted(const std::filesystem::path& file, std::string_view version) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos && trim(line.substr(0, eq)) == "version") return trim(line.substr(eq + 1)) == version;
    }
    return false;
}

Result decide(const Input& g, std::string& message) {
    message.clear();
    if (accepted(g.file, g.version)) return Result::Accepted;
    if (g.env_version) {
        if (*g.env_version == g.version) return Result::Accepted;
        message = std::format("plm: CUERVO_ACCEPT_EULA={} does not match the license version {}.\n"
                              "Nothing was run and no work was lost.\n"
                              "Read LICENSE.md, then set CUERVO_ACCEPT_EULA={} if you agree to it.",
                              *g.env_version, g.version, g.version);
        return Result::Refused;
    }
    if (g.accept_flag) {
        if (!write(g.file, g.version)) message = "plm: could not remember your acceptance. It applies to this run only.";
        return Result::Accepted;
    }
    if (g.interactive && g.in != nullptr && g.out != nullptr) {
        *g.out << "\nCuervo Engine License Agreement, version " << g.version
               << "\nUsing plm, the Pluma compiler of Cuervo Engine, requires accepting the agreement in LICENSE.md.\n"
                  "Its plain-English summary is at the top of that file. Type 'yes' to accept it, anything else to decline.\n"
                  "Accept the license agreement? [yes/no] "
               << std::flush;
        std::string answer;
        if (std::getline(*g.in, answer)) {
            answer = trim(answer);
            for (char& c : answer) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (answer == "yes" || answer == "y") {
                if (!write(g.file, g.version)) message = "plm: could not remember your acceptance. It applies to this run only.";
                return Result::Accepted;
            }
        }
        message = "plm: the license agreement was not accepted, so nothing was run. No work was lost.\n"
                  "To use plm, run it again and answer 'yes'.";
        return Result::Declined;
    }
    message = std::format("plm: the Cuervo Engine License Agreement (version {}) has not been accepted by this user, "
                          "and there is no terminal to ask on.\n"
                          "Nothing was run and no work was lost.\n"
                          "To accept it: read LICENSE.md, then run 'plm' once in a terminal, or pass --accept-eula, "
                          "or set CUERVO_ACCEPT_EULA={}.",
                          g.version, g.version);
    return Result::Refused;
}

bool require_acceptance(int argc, char** argv) {
    Input g;
    g.version = CUERVO_EULA_VERSION;
    g.file = acceptance_file();
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--accept-eula") g.accept_flag = true;
    }
    g.env_version = env("CUERVO_ACCEPT_EULA");
    if (g.env_version && g.env_version->empty()) g.env_version.reset();
    g.interactive = PLM_ISATTY(0) != 0 && PLM_ISATTY(2) != 0;
    g.in = &std::cin;
    g.out = &std::cerr;
    std::string message;
    const Result r = decide(g, message);
    if (!message.empty()) std::fprintf(stderr, "%s\n", message.c_str());
    return r == Result::Accepted;
}

} // namespace plm_eula

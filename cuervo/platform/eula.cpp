#include "cuervo/platform/eula.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>

#include "cuervo/platform/platform.hpp"

#if defined(_WIN32)
#include <io.h>
#define CUERVO_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define CUERVO_ISATTY(fd) isatty(fd)
#endif

namespace cuervo::eula {

namespace {

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

// Markdown emphasis out of a line, for the terminal.
std::string plain(std::string_view line) {
    std::string out;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '*' || line[i] == '`') continue;
        out += line[i];
    }
    return out;
}

} // namespace

std::string_view version() { return CUERVO_EULA_VERSION; }

std::string_view summary() {
    const std::string_view text = license_text();
    const std::size_t head = text.find("## A word before the legal text");
    if (head == std::string_view::npos) return {};
    const std::size_t start = text.find('\n', head);
    if (start == std::string_view::npos) return {};
    const std::size_t end = text.find("\n---", start);
    return text.substr(start + 1, end == std::string_view::npos ? std::string_view::npos : end - start - 1);
}

std::filesystem::path acceptance_file() {
    const std::filesystem::path dir = platform::user_data_dir("Cuervo Engine");
    return dir.empty() ? std::filesystem::path{} : dir / "eula-accepted.txt";
}

std::optional<Acceptance> read_acceptance(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    Acceptance a;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(std::string_view(line).substr(0, eq));
        const std::string value = trim(std::string_view(line).substr(eq + 1));
        if (key == "version") a.version = value;
        if (key == "accepted") a.date = value;
    }
    if (a.version.empty()) return std::nullopt;
    return a;
}

bool write_acceptance(const std::filesystem::path& file, const Acceptance& acceptance, std::string& error) {
    if (file.empty()) {
        error = "there is no per-user folder to store it in";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << "# Cuervo Engine License Agreement (LICENSE.md): accepted by this user.\n"
        << "version=" << acceptance.version << "\naccepted=" << acceptance.date << "\n";
    out.flush();
    if (!out) {
        error = std::format("could not write '{}'", platform::utf8(file));
        return false;
    }
    return true;
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

bool accepted(const std::filesystem::path& file, std::string_view current) {
    const auto a = read_acceptance(file);
    return a && a->version == current;
}

bool accepted() { return accepted(acceptance_file(), version()); }

bool record_acceptance(std::string& error) {
    return write_acceptance(acceptance_file(), {std::string(version()), today()}, error);
}

GateResult run_gate(const GateInput& g, std::string& message) {
    const std::string prog(g.program);
    message.clear();
    if (accepted(g.file, g.version)) return GateResult::Accepted;
    if (g.env_version) {
        if (*g.env_version == g.version) return GateResult::Accepted; // CI: this run only
        message = std::format("{}: CUERVO_ACCEPT_EULA={} does not match the license version {}.\n"
                              "Nothing was run and no work was lost.\n"
                              "Read LICENSE.md, then set CUERVO_ACCEPT_EULA={} if you agree to it.",
                              prog, *g.env_version, g.version, g.version);
        return GateResult::Refused;
    }
    auto store = [&]() -> bool {
        std::string error;
        if (write_acceptance(g.file, {std::string(g.version), today()}, error)) return true;
        message = std::format("{}: could not remember your acceptance ({}). It applies to this run only.", prog, error);
        return true; // still accepted for this run
    };
    if (g.accept_flag) {
        store();
        return GateResult::Accepted;
    }
    if (g.interactive && g.in != nullptr && g.out != nullptr) {
        std::ostream& out = *g.out;
        out << "\nCuervo Engine License Agreement, version " << g.version << "\n\n";
        std::istringstream lines{std::string(summary())};
        std::string line;
        while (std::getline(lines, line)) out << plain(line) << '\n';
        out << "\nThe full agreement is LICENSE.md. Type 'view' to read it here, 'yes' to accept it, or anything\n"
               "else to decline (the engine then exits; nothing is changed).\n";
        for (;;) {
            out << "Accept the license agreement? [view/yes/no] " << std::flush;
            std::string answer;
            if (!std::getline(*g.in, answer)) break;
            answer = trim(answer);
            for (char& c : answer) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (answer == "view") {
                out << '\n' << license_text() << '\n';
                continue;
            }
            if (answer == "yes" || answer == "y") {
                store();
                return GateResult::Accepted;
            }
            break;
        }
        message = std::format("{}: the license agreement was not accepted, so nothing was run. No work was lost.\n"
                              "To use the engine, run it again and answer 'yes'.",
                              prog);
        return GateResult::Declined;
    }
    message = std::format("{}: the Cuervo Engine License Agreement (version {}) has not been accepted by this user, "
                          "and there is no terminal to ask on.\n"
                          "Nothing was run and no work was lost.\n"
                          "To accept it: read LICENSE.md, then run '{}' once in a terminal, or pass --accept-eula, "
                          "or set CUERVO_ACCEPT_EULA={}.",
                          prog, g.version, prog, g.version);
    return GateResult::Refused;
}

bool require_acceptance(int argc, char** argv, std::string_view program) {
    GateInput g;
    g.program = program;
    g.version = version();
    g.file = acceptance_file();
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--accept-eula") g.accept_flag = true;
    }
    g.env_version = platform::env("CUERVO_ACCEPT_EULA");
    if (g.env_version && g.env_version->empty()) g.env_version.reset();
    g.interactive = CUERVO_ISATTY(0) != 0 && CUERVO_ISATTY(2) != 0;
    g.in = &std::cin;
    g.out = &std::cerr;
    std::string message;
    const GateResult r = run_gate(g, message);
    if (!message.empty()) std::fprintf(stderr, "%s\n", message.c_str());
    return r == GateResult::Accepted;
}

void strip_accept_flag(int& argc, char** argv) {
    int out = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) != "--accept-eula") argv[out++] = argv[i];
    }
    argc = out;
    argv[argc] = nullptr;
}

} // namespace cuervo::eula

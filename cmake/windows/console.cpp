// Linked into every Cuervo executable on Windows (cmake/Windows.cmake), so it
// runs before main without each program calling anything (docs/WINDOWS.md 4).
//
// Log lines and messages are UTF-8; the console shows them right only with its
// output code page set to UTF-8. The previous code page is restored at exit,
// since the setting outlives the process in the user's console window.
//
// A GUI-subsystem player (CUERVO_GUI_SUBSYSTEM, docs/WINDOWS.md 5) gets no
// console. Its output still goes wherever the parent redirected it (a pipe or
// a file, as the tests and `cuervo-tools export` verification do); started
// from a console without redirection, it attaches to that console so logs
// stay visible.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX // MinGW-w64's libstdc++ defines it already
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace {

UINT g_previous_cp = 0;

void restore_code_page() {
    if (g_previous_cp != 0) SetConsoleOutputCP(g_previous_cp);
}

#if defined(CUERVO_GUI_SUBSYSTEM)
bool handle_usable(DWORD which) {
    HANDLE h = GetStdHandle(which);
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

void reopen(const char* device, const char* mode, std::FILE* stream) {
    std::FILE* f = nullptr;
    if (freopen_s(&f, device, mode, stream) == 0) setvbuf(stream, nullptr, _IONBF, 0);
}
#endif

const bool g_console_ready = [] {
#if defined(CUERVO_GUI_SUBSYSTEM)
    const bool out_ok = handle_usable(STD_OUTPUT_HANDLE);
    const bool err_ok = handle_usable(STD_ERROR_HANDLE);
    if ((!out_ok || !err_ok) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!out_ok) reopen("CONOUT$", "w", stdout);
        if (!err_ok) reopen("CONOUT$", "w", stderr);
    }
#endif
    if (GetConsoleWindow() != nullptr) {
        const UINT current = GetConsoleOutputCP();
        if (current != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) {
            g_previous_cp = current;
            std::atexit(restore_code_page);
        }
    }
    return true;
}();

} // namespace

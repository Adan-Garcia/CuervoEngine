#pragma once

// Test fixture: tools/net/http_test_server.py on an ephemeral port (no fixed ports), stopped when
// the fixture goes out of scope. ok() is false when python3 or popen is unavailable (callers skip).
#if !defined(_WIN32)
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

struct HttpServer {
    FILE* pipe = nullptr; // write end = the server's stdin; closing it stops the server
    int port = 0;
    std::filesystem::path port_file;
    explicit HttpServer(const std::string& extra_args = "") {
        if (std::system("python3 --version >/dev/null 2>&1") != 0) return;
        static int counter = 0;
        port_file = std::filesystem::temp_directory_path() /
                    ("cuervo_http_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".port");
        std::filesystem::remove(port_file);
        const std::string cmd = "python3 -I \"" CUERVO_SOURCE_DIR "/tools/net/http_test_server.py\" --port 0 --port-file \"" +
                                port_file.string() + "\" --exit-on-stdin-eof " + extra_args + " >/dev/null 2>&1";
        pipe = popen(cmd.c_str(), "w");
        if (!pipe) return;
        for (int i = 0; i < 500 && port == 0; ++i) {
            std::ifstream f(port_file);
            if (!(f >> port)) port = 0;
            if (port == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~HttpServer() {
        if (pipe) pclose(pipe);
        std::error_code ec;
        if (!port_file.empty()) std::filesystem::remove(port_file, ec);
    }
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    bool ok() const { return port != 0; }
    std::string url(const std::string& path, bool tls = false) const {
        return std::string(tls ? "https" : "http") + "://127.0.0.1:" + std::to_string(port) + path;
    }
};
#endif

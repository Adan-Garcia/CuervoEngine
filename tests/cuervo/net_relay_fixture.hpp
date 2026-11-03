#pragma once

// Test fixture: tools/net/relay.py on an ephemeral port (no fixed ports), stopped when the
// fixture goes out of scope. ok() is false when python3 or popen is unavailable (callers skip).
#if !defined(_WIN32)
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

struct Relay {
    FILE* pipe = nullptr; // write end = the relay's stdin; closing it stops the relay
    int port = 0;
    std::filesystem::path port_file;
    explicit Relay(const std::string& extra_args = "") {
        if (std::system("python3 --version >/dev/null 2>&1") != 0) return;
        static int counter = 0;
        port_file = std::filesystem::temp_directory_path() /
                    ("cuervo_relay_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".port");
        std::filesystem::remove(port_file);
        const std::string cmd = "python3 -I \"" CUERVO_SOURCE_DIR "/tools/net/relay.py\" --port 0 --port-file \"" +
                                port_file.string() + "\" --exit-on-stdin-eof " + extra_args + " >/dev/null";
        pipe = popen(cmd.c_str(), "w");
        if (!pipe) return;
        for (int i = 0; i < 500 && port == 0; ++i) {
            std::ifstream f(port_file);
            if (!(f >> port)) port = 0;
            if (port == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~Relay() {
        if (pipe) pclose(pipe);
        std::error_code ec;
        if (!port_file.empty()) std::filesystem::remove(port_file, ec);
    }
    bool ok() const { return port != 0; }
};

// A self-signed certificate for localhost / 127.0.0.1 made with the openssl CLI in a unique temp dir
// (removed on destruction). ok() is false when openssl is unavailable (callers skip).
struct TestCert {
    std::filesystem::path dir;
    bool made = false;
    TestCert() {
        if (std::system("openssl version >/dev/null 2>&1") != 0) return;
        static int counter = 0;
        dir = std::filesystem::temp_directory_path() /
              ("cuervo_tls_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        const std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=localhost "
                                "-addext subjectAltName=DNS:localhost,IP:127.0.0.1 -keyout \"" + key() +
                                "\" -out \"" + cert() + "\" >/dev/null 2>&1";
        made = std::system(cmd.c_str()) == 0 && std::filesystem::exists(cert());
    }
    ~TestCert() {
        std::error_code ec;
        if (!dir.empty()) std::filesystem::remove_all(dir, ec);
    }
    std::string cert() const { return (dir / "cert.pem").string(); }
    std::string key() const { return (dir / "key.pem").string(); }
    bool ok() const { return made; }
};
#endif // !_WIN32: these fixtures need popen, getpid and the openssl CLI

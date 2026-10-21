#include "dap_cli.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/script/game_runner.hpp"
#include "pluma/dap.hpp"
#include "pluma/json.hpp"

namespace {

namespace fs = std::filesystem;
using namespace cuervo;

// DAP's base protocol on stdio: `Content-Length: N\r\n\r\n` + N bytes of
// JSON. A reader thread queues requests so the game loop can poll them, and
// flags `pause` at once (a script stuck in a loop never polls).
class StdioTransport final : public pluma::dap::Transport {
public:
    ~StdioTransport() override {
        // The reader may be blocked reading stdin: let it go with the process.
        if (reader_.joinable()) reader_.detach();
    }

    void start(pluma::dap::Debugger& debugger) {
        reader_ = std::thread([this, &debugger] { read_loop(debugger); });
    }

    void send(const std::string& message) override {
        std::lock_guard lock(write_mutex_);
        std::fprintf(stdout, "Content-Length: %zu\r\n\r\n", message.size());
        std::fwrite(message.data(), 1, message.size(), stdout);
        std::fflush(stdout);
    }

    std::optional<std::string> receive(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(queue_mutex_);
        if (queue_.empty() && !closed_ && timeout.count() > 0) {
            ready_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; });
        }
        if (queue_.empty()) return std::nullopt;
        std::string m = std::move(queue_.front());
        queue_.pop_front();
        return m;
    }

    bool closed() const override {
        std::lock_guard lock(queue_mutex_);
        return closed_ && queue_.empty();
    }

private:
    // Raw reads from fd 0, not stdio: a thread blocked in fgetc(stdin) holds
    // the FILE lock, and exit() would deadlock flushing stdio at shutdown.
    bool next_byte(char& c) {
        if (pos_ == len_) {
#if defined(_WIN32)
            const int n = _read(0, buffer_, sizeof(buffer_));
#else
            const ssize_t n = ::read(0, buffer_, sizeof(buffer_));
#endif
            if (n <= 0) return false;
            pos_ = 0;
            len_ = static_cast<std::size_t>(n);
        }
        c = buffer_[pos_++];
        return true;
    }

    // One header line without its CRLF; false at end of input.
    bool read_line(std::string& line) {
        line.clear();
        for (char c = 0; next_byte(c);) {
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            line += c;
        }
        return false;
    }

    bool read_exact(std::string& out) {
        for (char& c : out) {
            if (!next_byte(c)) return false;
        }
        return true;
    }

    void read_loop(pluma::dap::Debugger& debugger) {
        std::string line;
        for (;;) {
            std::size_t length = 0;
            bool header = false;
            while (read_line(line)) {
                if (line.empty()) {
                    if (header) break;
                    continue;
                }
                constexpr std::string_view key = "Content-Length:";
                if (line.starts_with(key)) {
                    length = static_cast<std::size_t>(std::strtoull(line.c_str() + key.size(), nullptr, 10));
                    header = true;
                }
            }
            if (!header) break; // end of input
            std::string body(length, '\0');
            if (!read_exact(body)) break;
            if (auto msg = pluma::json::Parser(body).parse(); msg && msg->str_or("command") == "pause") {
                debugger.request_pause();
            }
            {
                std::lock_guard lock(queue_mutex_);
                queue_.push_back(std::move(body));
            }
            ready_.notify_one();
        }
        {
            std::lock_guard lock(queue_mutex_);
            closed_ = true;
        }
        ready_.notify_one();
    }

    std::thread reader_;
    char buffer_[4096];
    std::size_t pos_ = 0;
    std::size_t len_ = 0;
    std::mutex write_mutex_;
    mutable std::mutex queue_mutex_;
    std::condition_variable ready_;
    std::deque<std::string> queue_;
    bool closed_ = false;
};

// Engine log lines and script `print`s become `output` events.
void forward_log(void* user, log::Level level, std::string_view, int, std::string_view message) {
    auto* debugger = static_cast<pluma::dap::Debugger*>(user);
    constexpr std::string_view script_prefix = "[script] "; // ScriptHost's print
    if (message.starts_with(script_prefix)) {
        message.remove_prefix(script_prefix.size());
        debugger->output(std::string(message) + "\n", "stdout");
        return;
    }
    debugger->output(std::string(message) + "\n", level >= log::Level::Warn ? "stderr" : "console");
}

} // namespace

int run_dap(int /*argc*/, char** /*argv*/) {
    log::set_stdout_free(true); // stdout carries DAP only
    StdioTransport transport;
    pluma::dap::Debugger debugger(transport);
    transport.start(debugger);

    std::optional<pluma::json::Value> args = debugger.wait_for_start([](const pluma::json::Value& a) {
        const std::string_view project = a.str_or("project");
        if (project.empty()) return std::string("launch: 'project' (the game directory) is required");
        std::error_code ec;
        if (!fs::is_regular_file(platform::path_from_utf8(project) / "project.toml", ec)) {
            return std::format("launch: no project.toml in '{}'", project);
        }
        const std::string_view backend = a.str_or("backend");
        if (!backend.empty() && backend != "vulkan" && backend != "gl") {
            return std::format("launch: unknown backend '{}' (vulkan or gl)", backend);
        }
        return std::string();
    });
    if (!args) return EXIT_FAILURE;

    log::set_sink(&forward_log, &debugger);
    script::GameOptions o;
    o.dir = std::string(args->str_or("project"));
    const std::string_view backend = args->str_or("backend");
    // Like `cuervo --game`: Vulkan when built in (falling back to GL), unless asked.
    o.backend = backend == "gl" || !rhi::backend_compiled_in(rhi::Backend::Vulkan) ? rhi::Backend::OpenGL
                                                                                    : rhi::Backend::Vulkan;
    o.allow_fallback = backend.empty();
    o.font_path = platform::utf8(platform::find_asset("fonts/LiberationSans-Regular.ttf", CUERVO_DEFAULT_FONT));
    if (!args->bool_or("noDebug", false)) {
        o.debugger = &debugger;
        debugger.set_source_root(fs::absolute(platform::path_from_utf8(o.dir)));
    }
    const int code = script::run_game(o);
    log::set_sink(nullptr, nullptr);
    debugger.exited(code);
    return code;
}

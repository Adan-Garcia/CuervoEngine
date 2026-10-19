#pragma once

// Debug Adapter Protocol server for Pluma (DESIGN.md 5.14 "a debugger over
// DAP"): line breakpoints, stepping, pause, stop on panic, stack frames,
// scopes, variables and `evaluate` of variable paths, for the bytecode VM
// (AOT-native code has no hooks). Engine-independent: the host owns the
// transport (e.g. Content-Length framed stdio), starts the program after the
// handshake and attaches the debugger to every VM it creates (hot reload
// makes new ones). Everything runs on the VM's thread; only request_pause()
// may be called from elsewhere.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/debug.hpp"
#include "pluma/json.hpp"

namespace pluma {
class SourceFile;
class VM;
} // namespace pluma

namespace pluma::dap {

// The client connection. The debugger calls these on the VM thread;
// implementations usually read on another thread and queue messages.
class Transport {
public:
    virtual ~Transport() = default;
    // Sends one message (JSON text; the transport frames it).
    virtual void send(const std::string& message) = 0;
    // The next message, waiting at most `timeout`; nullopt if none came (or
    // the client is gone, see closed()).
    virtual std::optional<std::string> receive(std::chrono::milliseconds timeout) = 0;
    virtual bool closed() const = 0;
};

class Debugger final : public DebugHooks {
public:
    explicit Debugger(Transport& transport) : transport_(transport) {}
    ~Debugger() override;
    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    // The handshake: serves initialize, launch, breakpoints and exception
    // filters until configurationDone, then returns the launch request's
    // arguments (the host starts the program). `validate` may refuse the
    // launch with an error message. nullopt: the client left first.
    std::optional<json::Value> wait_for_start(const std::function<std::string(const json::Value&)>& validate);

    // VM source paths (part paths such as "scripts/game.plm") are relative to
    // this directory; the client sends absolute paths.
    // Call it before the program starts; breakpoints the client set during the
    // handshake are mapped then.
    void set_source_root(std::filesystem::path root);
    // Called about every 20 ms while stopped, so the host stays responsive
    // (window events). It must not run script code.
    void set_idle(std::function<void()> idle) { idle_ = std::move(idle); }

    // Installs the debugger on `vm` (and on each new VM after a hot reload),
    // checking the breakpoints against its code. detach() removes it.
    void attach(VM& vm);
    void detach();

    // Requests that arrived while the program runs (breakpoint changes,
    // pause, disconnect). Call once per frame.
    void poll();
    // Program output, as an `output` event ("stdout", "stderr", "console").
    void output(std::string_view text, std::string_view category = "stdout");
    // The program ended: `exited` and `terminated` events.
    void exited(int exit_code);
    // The client asked to end the program (disconnect / terminate).
    bool terminate_requested() const { return terminate_; }
    // Times the program stopped so far: a host with a frame clock restarts it
    // after a stop instead of catching up on the time spent stopped.
    std::uint64_t stops() const { return stops_; }
    // Thread-safe: stop at the next line (the transport's reader calls this
    // when it sees a `pause` request, which a busy script loop never polls).
    void request_pause() { pause_.store(true); }

    // DebugHooks
    std::uint32_t events() const override { return lines | panics; }
    void on_line(VM& vm) override;
    void on_panic(VM& vm, std::string_view message) override;
    void on_code_loaded(VM& vm) override;

private:
    enum class Step : std::uint8_t { None, In, Over, Out };
    enum class Phase : std::uint8_t { Starting, Running, Stopped };

    struct Breakpoint {
        std::int64_t id = 0;
        std::uint32_t requested = 0; // the line the client asked for
        std::uint32_t line = 0;      // where it stops (the next line with code)
        bool verified = false;
    };
    // A variables reference (1-based index into refs_): a frame's locals,
    // the globals, or a value's children. Values are borrowed: nothing runs
    // while stopped, and the table is cleared on resume.
    struct Ref {
        enum class Kind : std::uint8_t { Locals, Globals, Children } kind = Kind::Children;
        std::size_t frame = 0;
        Value value;
    };
    struct SourceRef {
        const SourceFile* file = nullptr;
        std::string path;
    };

    // Dispatches one client message; returns true when the program should
    // resume (only while stopped).
    bool handle(const std::string& text);
    void stop(VM& vm, std::string_view reason, std::string_view text, const std::vector<std::int64_t>& hit);
    void resume();

    void send_response(const json::Value& request, json::Value body = json::Value::object());
    void send_error(const json::Value& request, std::string_view message);
    void send_event(std::string_view event, json::Value body = json::Value::object());

    std::string relative_path(const json::Value* source) const;
    std::string relativize(std::string_view path) const;
    json::Value source_json(const SourceFile* file, std::string_view path);
    // Moves breakpoints to lines with code in `vm` (null: no code yet, keep them).
    void verify(const std::string& file, std::vector<Breakpoint>& bps, const VM* vm) const;
    // Re-checks every breakpoint against `vm`'s code and tells the client.
    void reverify(const VM& vm);
    void rebuild_lines();
    std::int64_t add_ref(Ref ref);
    json::Value variable_json(std::string name, const Value& value);
    std::optional<Value> evaluate_path(std::string_view expression, std::size_t frame) const;

    void on_set_breakpoints(const json::Value& request);
    void on_stack_trace(const json::Value& request);
    void on_scopes(const json::Value& request);
    void on_variables(const json::Value& request);
    void on_evaluate(const json::Value& request);
    void on_source(const json::Value& request);

    Transport& transport_;
    std::atomic<std::int64_t> seq_{1}; // output() may come from a logging thread
    Phase phase_ = Phase::Starting;
    VM* vm_ = nullptr;         // the active VM (breakpoint checks while running)
    VM* stopped_vm_ = nullptr; // the VM stopped in, while stopped (inspection)
    std::filesystem::path root_;
    std::function<void()> idle_;
    std::optional<json::Value> launch_args_;
    std::string launch_error_;
    bool configured_ = false;

    std::map<std::string, std::vector<Breakpoint>, std::less<>> breakpoints_; // by relative path
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> lines_;   // stop lines by path
    std::int64_t next_breakpoint_id_ = 1;
    bool stop_on_panic_ = true;
    bool stop_on_entry_ = false;

    std::atomic<bool> pause_{false};
    Step step_ = Step::None;
    std::size_t step_depth_ = 0;
    bool terminate_ = false;
    std::uint64_t stops_ = 0;

    std::vector<Ref> refs_;
    std::vector<SourceRef> sources_; // sourceReference - 1
};

} // namespace pluma::dap

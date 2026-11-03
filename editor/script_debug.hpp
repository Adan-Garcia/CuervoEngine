#pragma once
// The editor's side of the in-process debugger (docs: DESIGN.md 5.14, pluma/dap.hpp). UI-free: owns the
// LocalTransport and the pluma::dap::Debugger, turns DAP events and responses into plain state the
// Debugger drawer and the code view read. Everything runs on one thread: the Debugger blocks inside the
// VM while stopped and calls the host's idle hook, which pumps the UI and this class; requests sent
// while stopped are answered during the next idle slice (about every 20 ms).
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pluma/dap.hpp"
#include "pluma/json.hpp"

namespace cuervo::editor {

struct DebugFrame {
    int id = 0;
    std::string name;
    std::string rel; // project relative, empty for built-in code
    int line = 0;    // 1-based
};

struct DebugVar {
    std::string name, value, type;
    int ref = 0;     // children are fetched through it
    int depth = 0;
    bool open = false;
};

struct WatchItem {
    std::string expr, result;
    bool ok = false, pending = false;
};

class DebugSession {
public:
    using Breakpoints = std::map<std::string, std::set<int>>; // project relative file -> 1-based lines

    DebugSession();
    DebugSession(const DebugSession&) = delete;
    DebugSession& operator=(const DebugSession&) = delete;

    // The handshake with the breakpoints the project has. False: the client left before it finished.
    bool start(const std::filesystem::path& project, const Breakpoints& bps);
    pluma::dap::Debugger& debugger() { return dbg_; }

    // Sends a file's full line list (the DAP contract); applies at once while stopped, at the next
    // Debugger::poll() while running.
    void set_breakpoints(const std::string& rel, const std::set<int>& lines);

    // Reads what the debugger sent since the last call. Cheap when there is nothing.
    void pump();

    bool stopped() const { return stopped_; }
    const std::string& stop_reason() const { return reason_; }
    std::uint64_t stop_serial() const { return stop_serial_; } // grows with every stop
    const std::vector<DebugFrame>& frames() const { return frames_; }
    int frame() const { return frame_; }               // selected index into frames()
    void select_frame(int index);
    const std::vector<DebugVar>& locals() const { return vars_; }
    void toggle_var(int index);                        // open or close a value's children
    const std::vector<WatchItem>& watches() const { return watches_; }
    void add_watch(const std::string& expr);
    void remove_watch(int index);
    const std::vector<std::string>& output() const { return output_; }
    bool terminated() const { return terminated_; }

    void resume();
    void step_over();
    void step_in();
    void step_out();
    void pause();
    void disconnect();

private:
    void on_message(const pluma::json::Value& v);
    void request(std::string_view command, std::string args, std::function<void(const pluma::json::Value&)> done = {});
    void leave_stop();
    void fetch_frame_state();
    void eval_watch(std::size_t i);
    void fetch_children(int var_index);

    pluma::dap::LocalTransport transport_;
    pluma::dap::Debugger dbg_;
    std::filesystem::path root_;
    std::map<std::int64_t, std::function<void(const pluma::json::Value&)>> pending_;
    bool stopped_ = false, terminated_ = false;
    std::string reason_;
    std::uint64_t stop_serial_ = 0;
    std::vector<DebugFrame> frames_;
    int frame_ = 0;
    std::vector<DebugVar> vars_;
    std::vector<WatchItem> watches_;
    std::vector<std::string> output_;
};

} // namespace cuervo::editor

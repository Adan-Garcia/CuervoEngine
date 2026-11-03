#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cuervo/input/input_map.hpp"
#include "cuervo/net/http.hpp"
#include "cuervo/net/lockstep.hpp"
#include "cuervo/net/rollback.hpp"
#include "cuervo/net/transport.hpp"
#include "pluma/bindings.hpp"

// Natives of cuervo/script/prelude/net.plm and http.plm (DESIGN.md 4.19). One network session per ScriptHost,
// owned here beside the VM, never inside it: sockets, peers and timings are not simulation state
// and never reach the state hash. The only thing the network gives the sim is one recorded
// InputFrame per player per tick (ScriptHost::net_tick -> tick_players), so a networked match
// replays offline from those frames exactly like local input.
namespace cuervo::script {

class ScriptHost;

class NetContext {
public:
    enum class Mode { Lockstep, Rollback };

    // Steps without a packet from a peer before it counts as gone (virtual time: host steps,
    // not the wall clock).
    static constexpr std::uint32_t peer_timeout_steps = 600;

    NetContext();
    ~NetContext();
    NetContext(const NetContext&) = delete;
    NetContext& operator=(const NetContext&) = delete;

    // ---- session setup (the natives call these; tests call attach() with loopback endpoints)
    bool host(std::uint16_t port, std::string& error);                       // UDP, two players, we are player 0
    bool join(const std::string& address, std::string& error);               // UDP, two players, we are player 1
    // UDP, 2 to 4 players: `addresses` lists every player's "host:port" in player order (comma separated),
    // `self` is our index; we bind our own port and exchange with all the others directly.
    bool mesh(const std::string& addresses, std::uint32_t self, std::string& error);
    bool relay(const std::string& url, const std::string& room, std::uint32_t players, std::string& error);
    // Uses an existing transport (not owned; it must outlive the session). Player numbers are its local_id().
    void attach(cuervo::net::Transport& transport, std::uint32_t players);
    void stop();

    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }
    void set_input_delay(std::uint32_t ticks) { delay_ = ticks; }
    void set_max_rollback(std::uint32_t ticks) { max_rollback_ = ticks; }

    // ---- per host step (ScriptHost::net_tick): true when one sim tick ran
    bool step(ScriptHost& host, const InputFrame& local);

    // ---- queries
    bool active() const { return transport_ != nullptr; }
    std::uint32_t player() const;
    std::uint32_t players() const { return players_; }
    std::uint32_t peers() const;       // remote players heard from recently
    bool connected() const;            // every other player has been heard from
    bool desynced() const;
    bool consume_peer_joined();        // true once per newly seen peer; clears
    bool consume_peer_left();
    // The simulation's hashes after `tick` ticks as this session ran them (the runner records them in a
    // replay for ticks both peers confirmed). Kept for the last `hash_keep` ticks; nullopt when pruned.
    static constexpr std::uint32_t hash_keep = 256;
    std::optional<std::uint64_t> state_hash_at(std::uint32_t tick) const;
    std::optional<std::uint64_t> snapshot_hash_at(std::uint32_t tick) const;
    // Ticks every player's input has been delivered for (the replay-able prefix).
    std::uint32_t confirmed_tick() const;
    // The delivered frame of `player` for `tick`, if still retained.
    const InputFrame* confirmed_frame(std::uint32_t player, std::uint32_t tick) const;
    const cuervo::net::LockstepSession* lockstep() const { return lockstep_.get(); }
    const cuervo::net::RollbackSession* rollback() const { return rollback_.get(); }

    // ---- Http.* (cuervo/script/prelude/http.plm): requests outside the sim, polled by handle
    static constexpr std::size_t max_http_handles = 32;
    // Starts a request; 0 with `error` set when refused. Handles are 1, 2, 3... (never reused).
    int http_start(const std::string& method, const std::string& url, const std::string& body,
                   const std::string& header_lines, std::string& error);
    std::shared_ptr<cuervo::net::HttpRequest> http_find(int handle) const;
    void http_close(int handle);
    int http_timeout_ms = 10000;
    std::string http_ca_file;

    // Runs `fn` now, or after the current step when a script called it from inside a tick
    // (the session must not be replaced while it is simulating).
    void request(std::function<void()> fn);
    // The same for a setup call: returns its result, or true when it was deferred (errors are logged).
    bool request_bool(const char* what, std::function<bool(std::string&)> fn);

private:
    class PlayerTransport; // maps UDP peer ids to player numbers and tracks who we hear from
    class Host;            // RollbackHost over a ScriptHost
    void build_session(std::unique_ptr<PlayerTransport> t, std::uint32_t players);

    Mode mode_ = Mode::Lockstep;
    std::uint32_t delay_ = 2;
    std::uint32_t max_rollback_ = 6;
    std::uint32_t players_ = 2;
    std::unique_ptr<PlayerTransport> transport_;
    std::unique_ptr<cuervo::net::LockstepSession> lockstep_;
    std::unique_ptr<cuervo::net::RollbackSession> rollback_;
    std::unique_ptr<Host> rollback_host_;
    void record_hashes(ScriptHost& host, std::uint32_t tick, std::optional<std::uint64_t> snapshot);
    std::map<std::uint32_t, std::uint64_t> state_hashes_, snapshot_hashes_;
    bool in_step_ = false;
    bool closed_logged_ = false;
    std::vector<std::function<void()>> deferred_;
    std::set<std::uint32_t> alive_; // remote players heard from within peer_timeout_steps
    std::uint32_t joined_pending_ = 0;
    std::uint32_t left_pending_ = 0;
    std::map<int, std::shared_ptr<cuervo::net::HttpRequest>> http_;
    int next_http_ = 1;
};

void bind_net(pluma::Bindings& bindings, NetContext& context);

} // namespace cuervo::script

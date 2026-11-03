#include "cuervo/script/net_bindings.hpp"

#include <algorithm>
#include <format>
#include <map>

#include "cuervo/core/log.hpp"
#include "cuervo/net/udp.hpp"
#include "cuervo/net/websocket.hpp"
#include "cuervo/script/script_host.hpp"

namespace cuervo::script {

namespace nv = pluma::native;
using pluma::NativeCall;
using pluma::Value;
using namespace cuervo::net;

// Presents a raw transport to the sessions as "PeerId == player number" and records which
// players we hear from. UDP has no shared numbering (both sides see themselves as 0 and the
// other as 1), so the joiner swaps 0 and 1; relay ids are already player numbers.
class NetContext::PlayerTransport final : public Transport {
public:
    PlayerTransport(Transport& inner, std::unique_ptr<Transport> owned, bool swap01)
        : inner_(inner), owned_(std::move(owned)), swap_(swap01) {}
    // A UDP mesh: the transport's peers 1.. are the players other than `self`, in player order.
    PlayerTransport(Transport& inner, std::unique_ptr<Transport> owned, std::uint32_t self, std::uint32_t players)
        : inner_(inner), owned_(std::move(owned)), swap_(false), mesh_(true), self_(self) {
        inner_of_player_.assign(players, 0);
        player_of_inner_.assign(players, 0);
        PeerId inner_id = 1;
        for (std::uint32_t p = 0; p < players; ++p) {
            if (p == self) continue;
            inner_of_player_[p] = inner_id;
            player_of_inner_[inner_id++] = p;
        }
    }

    PeerId local_id() const override { return mesh_ ? self_ : swap_ ? 1 : inner_.local_id(); }
    bool send(PeerId to, std::span<const std::byte> data) override {
        if (mesh_) return to < inner_of_player_.size() && to != self_ && inner_.send(inner_of_player_[to], data);
        return inner_.send(map(to), data);
    }
    bool recv(Packet& out) override {
        while (inner_.recv(out)) {
            if (mesh_) {
                // Datagrams from addresses outside the agreed list are not players.
                if (out.from == 0 || out.from >= player_of_inner_.size()) continue;
                out.from = player_of_inner_[out.from];
            } else {
                out.from = map(out.from);
            }
            last_heard_[out.from] = steps_;
            return true;
        }
        return false;
    }
    void tick() { ++steps_; }
    bool alive(PeerId p) const {
        const auto it = last_heard_.find(p);
        return it != last_heard_.end() && steps_ - it->second <= NetContext::peer_timeout_steps;
    }
    Transport& inner() { return inner_; }
    // A browser WebSocket gets its player id from the relay asynchronously (open() cannot block there).
    bool ready() const {
        const auto* ws = dynamic_cast<const WebSocketTransport*>(&inner_);
        return ws == nullptr || ws->ready();
    }

private:
    PeerId map(PeerId p) const { return swap_ && p <= 1 ? 1 - p : p; }
    Transport& inner_;
    std::unique_ptr<Transport> owned_;
    bool swap_;
    bool mesh_ = false;
    std::uint32_t self_ = 0;
    std::vector<PeerId> inner_of_player_, player_of_inner_;
    std::uint64_t steps_ = 0;
    std::map<PeerId, std::uint64_t> last_heard_;
};

class NetContext::Host final : public RollbackHost {
public:
    explicit Host(ScriptHost& h, std::uint32_t slots) : host_(h), slots_(slots) {}
    void step(std::span<const InputFrame> f) override {
        host_.tick_players(f);
        // Overwritten when a rollback re-simulates the tick, so a confirmed tick keeps its final hash.
        if (on_tick) on_tick(static_cast<std::uint32_t>(host_.tick_count()));
    }
    std::function<void(std::uint32_t)> on_tick;
    void save(std::uint32_t slot, std::uint32_t) override {
        std::string err;
        if (!host_.save(slots_[slot], err)) LOG_ERROR("Net: snapshot failed: {}", err);
    }
    void restore(std::uint32_t slot, std::uint32_t) override {
        std::string err;
        if (!host_.restore(slots_[slot], err)) LOG_ERROR("Net: rollback failed: {}", err);
    }
    std::uint64_t hash() override { return host_.snapshot_hash(); }
    ScriptHost* target() const { return &host_; }

private:
    ScriptHost& host_;
    std::vector<SimSnapshot> slots_;
};

std::uint32_t NetContext::player() const { return transport_ ? transport_->local_id() : 0; }

NetContext::NetContext() = default;
NetContext::~NetContext() = default;

void NetContext::build_session(std::unique_ptr<PlayerTransport> t, std::uint32_t players) {
    stop();
    transport_ = std::move(t);
    players_ = players;
    alive_.clear();
    // Sessions are created lazily in step(): the rollback one needs the ScriptHost.
}

void NetContext::stop() {
    lockstep_.reset();
    rollback_.reset();
    rollback_host_.reset();
    transport_.reset();
    state_hashes_.clear();
    snapshot_hashes_.clear();
    alive_.clear();
    joined_pending_ = left_pending_ = 0;
}

void NetContext::attach(Transport& transport, std::uint32_t players) {
    build_session(std::make_unique<PlayerTransport>(transport, nullptr, false), players);
}

bool NetContext::host(std::uint16_t port, std::string& error) {
    auto udp = UdpTransport::open("0.0.0.0:" + std::to_string(port), error);
    if (!udp) return false;
    Transport& ref = *udp;
    build_session(std::make_unique<PlayerTransport>(ref, std::move(udp), false), 2);
    return true;
}

bool NetContext::join(const std::string& address, std::string& error) {
    auto udp = UdpTransport::open("0.0.0.0:0", error);
    if (!udp) return false;
    if (udp->add_peer(address) != 1) {
        error = "cannot resolve '" + address + "' (expected host:port)";
        return false;
    }
    Transport& ref = *udp;
    build_session(std::make_unique<PlayerTransport>(ref, std::move(udp), true), 2);
    return true;
}

bool NetContext::mesh(const std::string& addresses, std::uint32_t self, std::string& error) {
    std::vector<std::string> list;
    for (std::size_t at = 0; at <= addresses.size();) {
        std::size_t comma = addresses.find(',', at);
        if (comma == std::string::npos) comma = addresses.size();
        list.push_back(addresses.substr(at, comma - at));
        at = comma + 1;
    }
    if (list.size() < 2 || list.size() > 4) {
        error = "a mesh needs 2 to 4 comma-separated host:port addresses";
        return false;
    }
    if (self >= list.size()) {
        error = std::format("player {} is not in a mesh of {}", self, list.size());
        return false;
    }
    const std::size_t colon = list[self].rfind(':');
    if (colon == std::string::npos) {
        error = "cannot read the port of '" + list[self] + "' (expected host:port)";
        return false;
    }
    auto udp = UdpTransport::open("0.0.0.0:" + list[self].substr(colon + 1), error);
    if (!udp) return false;
    for (std::size_t p = 0; p < list.size(); ++p) {
        if (p == self) continue;
        if (udp->add_peer(list[p]) == 0) {
            error = "cannot resolve '" + list[p] + "' (expected host:port)";
            return false;
        }
    }
    Transport& ref = *udp;
    const auto players = static_cast<std::uint32_t>(list.size());
    build_session(std::make_unique<PlayerTransport>(ref, std::move(udp), self, players), players);
    return true;
}

bool NetContext::relay(const std::string& url, const std::string& room, std::uint32_t players, std::string& error) {
    if (players < 2 || players > 4) {
        error = "players must be 2 to 4";
        return false;
    }
    auto ws = open_websocket_transport({url, room}, error);
    if (!ws) return false;
    if (ws->local_id() >= players) {
        error = std::format("the room already has {} players", players);
        return false;
    }
    Transport& ref = *ws;
    build_session(std::make_unique<PlayerTransport>(ref, std::move(ws), false), players);
    return true;
}

int NetContext::http_start(const std::string& method, const std::string& url, const std::string& body,
                           const std::string& header_lines, std::string& error) {
    // Finished requests the script never closed would pin their memory; refuse instead of growing.
    if (http_.size() >= max_http_handles) {
        error = std::format("too many open requests ({}); Http.close the finished ones", max_http_handles);
        return 0;
    }
    HttpOptions o;
    o.method = method;
    o.body = body;
    o.timeout_ms = http_timeout_ms;
    o.ca_file = http_ca_file;
    std::size_t pos = 0;
    while (pos < header_lines.size()) {
        std::size_t eol = header_lines.find('\n', pos);
        if (eol == std::string::npos) eol = header_lines.size();
        std::string line = header_lines.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            error = "malformed header line '" + line + "' (expected 'Name: value')";
            return 0;
        }
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        o.headers.emplace_back(line.substr(0, colon), std::move(value));
    }
    auto req = cuervo::net::http_start(url, std::move(o), error);
    if (!req) return 0;
    const int id = next_http_++;
    http_.emplace(id, std::move(req));
    return id;
}

std::shared_ptr<HttpRequest> NetContext::http_find(int handle) const {
    const auto it = http_.find(handle);
    return it == http_.end() ? nullptr : it->second;
}

void NetContext::http_close(int handle) { http_.erase(handle); } // ~HttpRequest cancels and joins

std::uint32_t NetContext::peers() const {
    std::uint32_t n = 0;
    for (std::uint32_t p = 0; p < players_ && transport_; ++p)
        if (p != transport_->local_id() && transport_->alive(p)) ++n;
    return n;
}

bool NetContext::connected() const { return active() && peers() + 1 == players_; }

bool NetContext::desynced() const {
    if (lockstep_) return lockstep_->desync().has_value();
    if (rollback_) return rollback_->desync().has_value();
    return false;
}

bool NetContext::consume_peer_joined() {
    if (joined_pending_ == 0) return false;
    --joined_pending_;
    return true;
}

bool NetContext::consume_peer_left() {
    if (left_pending_ == 0) return false;
    --left_pending_;
    return true;
}

void NetContext::request(std::function<void()> fn) {
    if (in_step_) deferred_.push_back(std::move(fn));
    else fn();
}

bool NetContext::request_bool(const char* what, std::function<bool(std::string&)> fn) {
    auto run = [what, fn = std::move(fn)] {
        std::string error;
        const bool ok = fn(error);
        if (!ok) LOG_WARN("{}: {}", what, error);
        return ok;
    };
    if (!in_step_) return run();
    deferred_.push_back([run] { run(); });
    return true; // accepted; the result is only known after the tick (see Net.connected)
}

void NetContext::record_hashes(ScriptHost& host, std::uint32_t tick, std::optional<std::uint64_t> snapshot) {
    state_hashes_[tick] = host.state_hash();
    if (snapshot) snapshot_hashes_[tick] = *snapshot;
    if (tick > hash_keep) {
        std::erase_if(state_hashes_, [&](const auto& kv) { return kv.first + hash_keep < tick; });
        std::erase_if(snapshot_hashes_, [&](const auto& kv) { return kv.first + hash_keep < tick; });
    }
}

std::optional<std::uint64_t> NetContext::state_hash_at(std::uint32_t tick) const {
    const auto it = state_hashes_.find(tick);
    return it == state_hashes_.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
}

std::optional<std::uint64_t> NetContext::snapshot_hash_at(std::uint32_t tick) const {
    if (rollback_) return rollback_->hash_at(tick);
    const auto it = snapshot_hashes_.find(tick);
    return it == snapshot_hashes_.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
}

std::uint32_t NetContext::confirmed_tick() const {
    if (rollback_) return rollback_->confirmed_tick();
    return lockstep_ ? lockstep_->sim_tick() : 0;
}

const InputFrame* NetContext::confirmed_frame(std::uint32_t player, std::uint32_t tick) const {
    if (rollback_) return rollback_->frame(player, tick);
    return lockstep_ ? lockstep_->frame(player, tick) : nullptr;
}

bool NetContext::step(ScriptHost& host, const InputFrame& local) {
    if (!transport_) return false;
    in_step_ = true;
    transport_->tick();
    bool ticked = false;
    if (!transport_->ready()) {
        if (const auto* ws = dynamic_cast<const WebSocketTransport*>(&transport_->inner()); ws != nullptr && ws->closed()) {
            if (!closed_logged_) LOG_ERROR("Net: the relay connection is closed; no more input can arrive");
            closed_logged_ = true;
        }
        in_step_ = false;
        return false; // waiting for the relay to assign our player number
    }
    if (transport_->local_id() >= players_) {
        LOG_ERROR("Net: the room already has {} players", players_);
        in_step_ = false;
        stop();
        return false;
    }
    if (mode_ == Mode::Rollback) {
        if (!rollback_) {
            rollback_host_ = std::make_unique<Host>(host, max_rollback_ + 1);
            rollback_host_->on_tick = [this, &host](std::uint32_t tick) { record_hashes(host, tick, std::nullopt); };
            rollback_ = std::make_unique<RollbackSession>(*transport_, *rollback_host_, players_, max_rollback_, 0);
        }
        ticked = rollback_->update(local);
    } else {
        if (!lockstep_) lockstep_ = std::make_unique<LockstepSession>(*transport_, players_, delay_);
        lockstep_->pump();
        if (lockstep_->can_submit()) lockstep_->submit_local(local);
        std::vector<InputFrame> frames;
        if (lockstep_->advance(frames)) {
            host.tick_players(frames);
            const std::uint64_t snapshot = host.snapshot_hash();
            lockstep_->report_hash(snapshot);
            record_hashes(host, lockstep_->sim_tick(), snapshot);
            ticked = true;
        }
    }
    // Peer presence events, from virtual time only.
    const std::uint32_t me = transport_->local_id();
    for (std::uint32_t p = 0; p < players_; ++p) {
        if (p == me) continue;
        const bool now = transport_->alive(p);
        const bool was = alive_.contains(p);
        if (now && !was) {
            alive_.insert(p);
            ++joined_pending_;
        } else if (!now && was) {
            alive_.erase(p);
            ++left_pending_;
        }
    }
    in_step_ = false;
    std::vector<std::function<void()>> run;
    run.swap(deferred_);
    for (auto& fn : run) fn();
    return ticked;
}

namespace {

Value i32(std::int64_t v) { return Value::integer(pluma::NumKind::I32, v); }

} // namespace

void bind_net(pluma::Bindings& b, NetContext& ctx) {
    NetContext* n = &ctx;

    b.add_native("Net.host", [n](NativeCall& c) {
        const std::int64_t port = nv::integer(c, 0);
        if (port < 0 || port > 65535) c.vm.panic("Net.host: port must be 0..65535");
        return Value::boolean(n->request_bool("Net.host", [n, port](std::string& error) {
            return n->host(static_cast<std::uint16_t>(port), error);
        }));
    });
    b.add_native("Net.join", [n](NativeCall& c) {
        const std::string address(nv::str(c, 0));
        return Value::boolean(
            n->request_bool("Net.join", [n, address](std::string& error) { return n->join(address, error); }));
    });
    b.add_native("Net.mesh", [n](NativeCall& c) {
        const std::string addresses(nv::str(c, 0));
        const auto self = static_cast<std::uint32_t>(std::max<std::int64_t>(nv::integer(c, 1), 0));
        return Value::boolean(n->request_bool("Net.mesh", [n, addresses, self](std::string& error) {
            return n->mesh(addresses, self, error);
        }));
    });
    b.add_native("Net.relay", [n](NativeCall& c) {
        const std::string url(nv::str(c, 0)), room(nv::str(c, 1));
        const auto players = static_cast<std::uint32_t>(std::max<std::int64_t>(nv::integer(c, 2), 0));
        return Value::boolean(n->request_bool("Net.relay", [n, url, room, players](std::string& error) {
            return n->relay(url, room, players, error);
        }));
    });
    b.add_native("Net.stop", [n](NativeCall&) {
        n->request([n] { n->stop(); });
        return Value{};
    });
    b.add_native("Net.set_mode", [n](NativeCall& c) {
        const std::string_view m = nv::name(c, 0);
        if (m == "lockstep") n->set_mode(NetContext::Mode::Lockstep);
        else if (m == "rollback") n->set_mode(NetContext::Mode::Rollback);
        else c.vm.panic(std::format("Net.set_mode: unknown mode #{} (#lockstep, #rollback)", m));
        return Value{};
    });
    b.add_native("Net.is_rollback", [n](NativeCall&) { return Value::boolean(n->mode() == NetContext::Mode::Rollback); });
    b.add_native("Net.set_input_delay", [n](NativeCall& c) {
        n->set_input_delay(static_cast<std::uint32_t>(std::clamp<std::int64_t>(nv::integer(c, 0), 0, 30)));
        return Value{};
    });
    b.add_native("Net.player", [n](NativeCall&) { return i32(n->player()); });
    b.add_native("Net.players", [n](NativeCall&) { return i32(n->active() ? n->players() : 1); });
    b.add_native("Net.peers", [n](NativeCall&) { return i32(n->peers()); });
    b.add_native("Net.connected", [n](NativeCall&) { return Value::boolean(n->connected()); });
    b.add_native("Net.desynced", [n](NativeCall&) { return Value::boolean(n->desynced()); });
    b.add_native("Net.peer_joined", [n](NativeCall&) { return Value::boolean(n->consume_peer_joined()); });
    // ---- Http.*: outside the sim; an unknown handle is a script bug, not a panic (reads as pending/empty)
    auto start = [n](const char* what, const char* method, NativeCall& c, std::size_t body_arg, std::size_t hdr_arg) {
        std::string error;
        const std::string url(nv::str(c, 0));
        const std::string body = body_arg == 0 ? std::string() : std::string(nv::str(c, body_arg));
        const std::string hdrs(nv::str(c, hdr_arg));
        const int id = n->http_start(method, url, body, hdrs, error);
        if (id == 0) LOG_WARN("{}: {}", what, error);
        return i32(id);
    };
    b.add_native("Http.get", [start](NativeCall& c) { return start("Http.get", "GET", c, 0, 1); });
    b.add_native("Http.post", [start](NativeCall& c) { return start("Http.post", "POST", c, 1, 2); });
    b.add_native("Http.done", [n](NativeCall& c) {
        const auto r = n->http_find(static_cast<int>(nv::integer(c, 0)));
        return Value::boolean(r && r->done());
    });
    b.add_native("Http.status", [n](NativeCall& c) {
        const auto r = n->http_find(static_cast<int>(nv::integer(c, 0)));
        return i32(r ? r->status() : 0);
    });
    b.add_native("Http.body", [n](NativeCall& c) {
        const auto r = n->http_find(static_cast<int>(nv::integer(c, 0)));
        return nv::str_result(r ? r->body() : std::string());
    });
    b.add_native("Http.error", [n](NativeCall& c) {
        const auto r = n->http_find(static_cast<int>(nv::integer(c, 0)));
        return nv::str_result(r ? r->error() : std::string("unknown request handle"));
    });
    b.add_native("Http.header", [n](NativeCall& c) {
        const auto r = n->http_find(static_cast<int>(nv::integer(c, 0)));
        return nv::str_result(r ? r->header(std::string(nv::str(c, 1))) : std::string());
    });
    b.add_native("Http.close", [n](NativeCall& c) {
        n->http_close(static_cast<int>(nv::integer(c, 0)));
        return Value{};
    });
    b.add_native("Http.set_timeout", [n](NativeCall& c) {
        n->http_timeout_ms = static_cast<int>(std::clamp<std::int64_t>(nv::integer(c, 0), 100, 120000));
        return Value{};
    });
    b.add_native("Http.set_ca_file", [n](NativeCall& c) {
        n->http_ca_file = std::string(nv::str(c, 0));
        return Value{};
    });
    b.add_native("Net.peer_left", [n](NativeCall&) { return Value::boolean(n->consume_peer_left()); });
}

} // namespace cuervo::script

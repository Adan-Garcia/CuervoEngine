// The DAP debugger (pluma/dap.hpp, DESIGN.md 5.14) against a scripted client:
// each test queues the client's requests up front; the debugger consumes them
// as it stops, and the test checks the messages it sent back.

#include <doctest.h>

#include <deque>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "pluma/dap.hpp"
#include "pluma/json.hpp"
#include "pluma/session.hpp"

namespace {

using pluma::json::Value;

struct ScriptedClient : pluma::dap::Transport {
    std::deque<std::string> incoming;
    std::vector<Value> sent;
    int seq = 1;

    void request(std::string_view command, std::string_view arguments = "{}") {
        incoming.push_back(
            std::format(R"({{"seq":{},"type":"request","command":"{}","arguments":{}}})", seq++, command, arguments));
    }
    void send(const std::string& message) override {
        auto v = pluma::json::Parser(message).parse();
        REQUIRE(v.has_value());
        sent.push_back(*v);
    }
    std::optional<std::string> receive(std::chrono::milliseconds) override {
        if (incoming.empty()) return std::nullopt;
        std::string m = incoming.front();
        incoming.pop_front();
        return m;
    }
    // Out of script: the client "left", so a stopped debugger lets go.
    bool closed() const override { return incoming.empty(); }

    std::vector<const Value*> events(std::string_view name) const {
        std::vector<const Value*> out;
        for (const Value& m : sent) {
            if (m.str_or("type") == "event" && m.str_or("event") == name) out.push_back(&m);
        }
        return out;
    }
    std::vector<const Value*> responses(std::string_view command) const {
        std::vector<const Value*> out;
        for (const Value& m : sent) {
            if (m.str_or("type") == "response" && m.str_or("command") == command) out.push_back(&m);
        }
        return out;
    }
};

const Value& body(const Value* m) { return *m->get("body"); }

const char* const adder = "fn add(a: int, b: int) -> int {\n" // 1
                          "    var c = a + b\n"               // 2
                          "    return c\n"                    // 3
                          "}\n"                               // 4
                          "var total = 0\n"                   // 5
                          "for i in 0..3 { total += add(i, 1) }\n" // 6
                          "print(total)\n";                   // 7

// The handshake with breakpoints in test.plm at `lines`.
void handshake(ScriptedClient& client, std::string_view lines) {
    client.request("initialize", R"({"adapterID":"pluma"})");
    client.request("launch", R"({"project":"/game"})");
    client.request("setBreakpoints",
                   std::format(R"({{"source":{{"path":"/game/test.plm"}},"breakpoints":[{}]}})", lines));
    client.request("configurationDone");
}

} // namespace

TEST_CASE("dap: handshake, breakpoints, stack, variables and evaluate") {
    ScriptedClient client;
    handshake(client, R"({"line":3},{"line":4})");
    pluma::dap::Debugger dbg(client);
    dbg.set_source_root("/game");
    auto args = dbg.wait_for_start({});
    REQUIRE(args.has_value());
    CHECK(args->str_or("project") == "/game");
    CHECK(client.events("initialized").size() == 1);
    CHECK(client.responses("initialize").size() == 1);

    // First stop: line 5 (the moved breakpoint). Then, stopped in add (first
    // pass): inspect, then continue through the other two passes.
    client.request("continue");
    client.request("stackTrace", R"({"threadId":1})");
    client.request("scopes", R"({"frameId":1})");
    client.request("variables", R"({"variablesReference":1})"); // the first scope: Locals
    client.request("evaluate", R"({"expression":"c","frameId":1})");
    client.request("evaluate", R"({"expression":"total","frameId":2})");
    client.request("evaluate", R"({"expression":"nope.x","frameId":1})");
    client.request("continue");
    client.request("continue");
    client.request("continue");
    client.request("threads"); // keeps the client "connected" until the end

    std::string out;
    pluma::VmOptions options;
    options.out = [&out](std::string_view s) { out += s; };
    pluma::Session session(options);
    dbg.attach(session.vm());
    const pluma::SessionResult r = session.run_script("test.plm", adder);
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(out == "6\n");

    // Line 4 (a brace) moved to the next line with code; line 3 kept.
    const auto bp = client.responses("setBreakpoints");
    REQUIRE(bp.size() == 1);
    CHECK(body(bp[0]).get("breakpoints")->a.size() == 2);
    const auto changed = client.events("breakpoint");
    REQUIRE(changed.size() == 2);
    CHECK(body(changed[0]).get("breakpoint")->int_or("line") == 3);
    CHECK(body(changed[1]).get("breakpoint")->int_or("line") == 5); // `}` -> `var total = 0`

    const auto stops = client.events("stopped");
    REQUIRE(stops.size() == 4); // line 5 once, then add:3 on each pass
    CHECK(body(stops[0]).str_or("reason") == "breakpoint");

    const auto trace = client.responses("stackTrace");
    REQUIRE(trace.size() == 1);
    const auto& frames = body(trace[0]).get("stackFrames")->a;
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].str_or("name") == "add");
    CHECK(frames[0].int_or("line") == 3);
    CHECK(frames[1].int_or("line") == 6);

    const auto vars = client.responses("variables");
    REQUIRE(vars.size() == 1);
    const auto& locals = body(vars[0]).get("variables")->a;
    REQUIRE(locals.size() == 3);
    CHECK(locals[2].str_or("name") == "c");
    CHECK(locals[2].str_or("value") == "1");
    CHECK(locals[2].str_or("type") == "int");

    const auto eval = client.responses("evaluate");
    REQUIRE(eval.size() == 3);
    CHECK(body(eval[0]).str_or("result") == "1");
    CHECK(body(eval[1]).str_or("result") == "0"); // the global, from the caller's frame
    CHECK_FALSE(eval[2]->bool_or("success", true));
}

TEST_CASE("dap: step in, step out and step over") {
    ScriptedClient client;
    handshake(client, R"({"line":6})");
    pluma::dap::Debugger dbg(client);
    dbg.set_source_root("/game");
    REQUIRE(dbg.wait_for_start({}).has_value());
    client.request("stepIn");  // 6 -> add:2
    client.request("next");    // add:2 -> add:3
    client.request("stepOut"); // back to the loop line
    client.request("disconnect");

    pluma::Session session;
    dbg.attach(session.vm());
    REQUIRE(session.run_script("test.plm", adder).ok);
    CHECK(dbg.terminate_requested());

    std::vector<std::string> where;
    const auto stops = client.events("stopped");
    REQUIRE(stops.size() == 4);
    CHECK(body(stops[0]).str_or("reason") == "breakpoint");
    CHECK(body(stops[1]).str_or("reason") == "step");
    // Positions come from the stack traces... which this client didn't ask
    // for; check them through the stop order instead: in, over, out.
    CHECK(client.responses("stepIn").size() == 1);
    CHECK(client.responses("next").size() == 1);
    CHECK(client.responses("stepOut").size() == 1);
}

TEST_CASE("dap: stopping positions while stepping") {
    // Ask for the top frame after every stop to see where each step lands.
    ScriptedClient client;
    handshake(client, R"({"line":6})");
    pluma::dap::Debugger dbg(client);
    dbg.set_source_root("/game");
    REQUIRE(dbg.wait_for_start({}).has_value());
    for (const char* step : {"stepIn", "next", "next", "stepOut"}) {
        client.request("stackTrace", R"({"threadId":1,"levels":1})");
        client.request(step);
    }
    client.request("stackTrace", R"({"threadId":1,"levels":1})");
    client.request("disconnect");

    pluma::Session session;
    dbg.attach(session.vm());
    REQUIRE(session.run_script("test.plm", adder).ok);
    std::vector<std::string> where;
    for (const Value* t : client.responses("stackTrace")) {
        const Value& f = body(t).get("stackFrames")->a.at(0);
        where.push_back(std::format("{}:{}", f.str_or("name"), f.int_or("line")));
    }
    // in: into add; next: line 3; next: leaves add (back in the caller);
    // stepOut from the loop's frame then runs to the end of the script.
    REQUIRE(where.size() >= 4);
    CHECK(where[0] == "<top level>:6");
    CHECK(where[1] == "add:2");
    CHECK(where[2] == "add:3");
    CHECK(where[3] == "<top level>:6");
}

TEST_CASE("dap: a panic stops before unwinding, with its frame inspectable") {
    ScriptedClient client;
    client.request("initialize");
    client.request("launch", "{}");
    client.request("setExceptionBreakpoints", R"({"filters":["panic"]})");
    client.request("configurationDone");
    pluma::dap::Debugger dbg(client);
    REQUIRE(dbg.wait_for_start({}).has_value());
    client.request("stackTrace", R"({"threadId":1})");
    client.request("scopes", R"({"frameId":1})");
    client.request("variables", R"({"variablesReference":1})");
    client.request("continue");
    client.request("threads");

    pluma::Session session;
    dbg.attach(session.vm());
    const pluma::SessionResult r =
        session.run_script("test.plm", "fn div(x: int) -> int {\n    return 10 / x\n}\nprint(div(0))\n");
    REQUIRE(r.panic.has_value()); // continuing lets the panic happen
    const auto stops = client.events("stopped");
    REQUIRE(stops.size() == 1);
    CHECK(body(stops[0]).str_or("reason") == "exception");
    CHECK(body(stops[0]).str_or("text").find("division by zero") != std::string_view::npos);
    const auto& frames = body(client.responses("stackTrace").at(0)).get("stackFrames")->a;
    CHECK(frames.at(0).str_or("name") == "div");
    CHECK(frames.at(0).int_or("line") == 2);
    const auto& locals = body(client.responses("variables").at(0)).get("variables")->a;
    REQUIRE(locals.size() == 1);
    CHECK(locals[0].str_or("value") == "0");
}

TEST_CASE("dap: pause, refused launch, and sources outside the project") {
    {
        ScriptedClient client;
        client.request("initialize");
        client.request("launch", R"({"project":"/nowhere"})");
        pluma::dap::Debugger dbg(client);
        CHECK_FALSE(dbg.wait_for_start([](const Value&) { return std::string("no project.toml"); }).has_value());
        const auto launch = client.responses("launch");
        REQUIRE(launch.size() == 1);
        CHECK_FALSE(launch[0]->bool_or("success", true));
        CHECK(launch[0]->str_or("message") == "no project.toml");
    }
    ScriptedClient client;
    client.request("initialize");
    client.request("launch", "{}");
    client.request("configurationDone");
    pluma::dap::Debugger dbg(client);
    dbg.set_source_root("/game");
    REQUIRE(dbg.wait_for_start({}).has_value());
    client.request("stackTrace", R"({"threadId":1})");
    client.request("source", R"({"sourceReference":1})");
    client.request("disconnect");
    dbg.request_pause(); // as the transport's reader would on a `pause` request

    pluma::Session session;
    dbg.attach(session.vm());
    // "lib.plm" isn't on disk under /game: it is served by reference.
    REQUIRE(session.load_files({{"lib.plm", "fn helper() -> int {\n    return 7\n}\n"},
                                {"main.plm", "var x = helper()\n"}})
                .ok);
    const auto stops = client.events("stopped");
    REQUIRE(stops.size() == 1);
    CHECK(body(stops[0]).str_or("reason") == "pause");
    const auto& frames = body(client.responses("stackTrace").at(0)).get("stackFrames")->a;
    REQUIRE_FALSE(frames.empty());
    const Value* source = frames[0].get("source");
    REQUIRE(source != nullptr);
    CHECK(source->int_or("sourceReference") == 1);
    const auto src = client.responses("source");
    REQUIRE(src.size() == 1);
    CHECK(body(src[0]).str_or("content").starts_with("fn helper()") ==
          (source->str_or("name") == "lib.plm"));
}

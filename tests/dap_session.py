#!/usr/bin/env python3
"""End-to-end test of `cuervo --dap` (runtime/dap_cli.hpp, DESIGN.md 5.14).

Plays a DAP client against the real runtime on tests/dap_game: breakpoint,
stack, scopes, variables, evaluate, step over, continue, script output and
disconnect. Usage: dap_session.py <cuervo executable> <tests/dap_game dir>
"""

import json
import os
import subprocess
import sys
import threading
import queue

BREAK_LINE = 10  # `count += 1` in Main.update


class Client:
    def __init__(self, runtime, env):
        self.proc = subprocess.Popen([runtime, "--dap"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, env=env)
        self.seq = 1
        self.messages = queue.Queue()
        self.backlog = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        out = self.proc.stdout
        while True:
            length = None
            while True:
                line = out.readline()
                if not line:
                    self.messages.put(None)
                    return
                line = line.strip()
                if not line:
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            self.messages.put(json.loads(out.read(length)))

    def request(self, command, arguments=None):
        msg = {"seq": self.seq, "type": "request", "command": command, "arguments": arguments or {}}
        self.seq += 1
        body = json.dumps(msg).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()
        return msg["seq"]

    def wait(self, predicate, what, timeout=60):
        # Messages that arrived while waiting for something else come first.
        for i, m in enumerate(self.backlog):
            if predicate(m):
                return self.backlog.pop(i)
        seen = []
        while True:
            try:
                m = self.messages.get(timeout=timeout)
            except queue.Empty:
                fail(f"timed out waiting for {what}; got {[x.get('event') or x.get('command') for x in seen]}")
            if m is None:
                fail(f"adapter closed stdout while waiting for {what}")
            seen.append(m)
            if m.get("type") == "event" and m.get("event") == "output":
                outputs.append(m["body"]["output"])
            if predicate(m):
                return m
            self.backlog.append(m)

    def response(self, seq, what):
        m = self.wait(lambda m: m.get("type") == "response" and m.get("request_seq") == seq, what)
        if not m.get("success"):
            fail(f"{what} failed: {m.get('message')}")
        return m.get("body", {})

    def call(self, command, arguments=None):
        return self.response(self.request(command, arguments), command)

    def stopped(self, what):
        return self.wait(lambda m: m.get("type") == "event" and m.get("event") == "stopped", what)["body"]


outputs = []


def fail(message):
    print("FAIL:", message)
    print("output events:", "".join(outputs[-20:]))
    sys.exit(1)


def top_frame(client):
    frames = client.call("stackTrace", {"threadId": 1})["stackFrames"]
    return frames[0]


def main():
    runtime, game = sys.argv[1], os.path.abspath(sys.argv[2])
    env = dict(os.environ, CUERVO_AUDIO="null")
    if sys.platform != "win32":
        env["SDL_VIDEO_DRIVER"] = "offscreen"
    c = Client(runtime, env)

    caps = c.call("initialize", {"adapterID": "pluma", "linesStartAt1": True})
    if not caps.get("supportsConfigurationDoneRequest"):
        fail("capabilities")
    c.wait(lambda m: m.get("event") == "initialized", "initialized")
    launch = c.request("launch", {"project": game, "backend": "gl"})
    bp = c.call("setBreakpoints", {"source": {"path": os.path.join(game, "scripts", "main.plm")},
                                    "breakpoints": [{"line": BREAK_LINE}]})
    if not bp["breakpoints"][0]["verified"]:
        fail(f"breakpoint not verified: {bp}")
    c.call("configurationDone")
    c.response(launch, "launch")

    stop = c.stopped("the breakpoint")
    if stop["reason"] != "breakpoint":
        fail(f"stop reason {stop}")
    frame = top_frame(c)
    if frame["line"] != BREAK_LINE or "update" not in frame["name"]:
        fail(f"top frame {frame}")
    if not frame["source"]["path"].replace("\\", "/").endswith("scripts/main.plm"):
        fail(f"source {frame['source']}")

    scopes = c.call("scopes", {"frameId": frame["id"]})["scopes"]
    local_vars = c.call("variables", {"variablesReference": scopes[0]["variablesReference"]})["variables"]
    names = [v["name"] for v in local_vars]
    if "self" not in names or "dt" not in names:
        fail(f"locals {names}")
    count = c.call("evaluate", {"expression": "self.count", "frameId": frame["id"]})
    if count["result"] != "0":
        fail(f"self.count = {count}")

    c.call("next")
    c.stopped("step over")
    if top_frame(c)["line"] != BREAK_LINE + 1:
        fail("step over didn't land on the next line")
    count = c.call("evaluate", {"expression": "self.count", "frameId": 1})
    if count["result"] != "1":
        fail(f"after one step self.count = {count}")

    c.call("continue")
    c.stopped("the breakpoint on the next tick")
    hist = c.call("evaluate", {"expression": "self.history[0]", "frameId": 1})
    if hist["result"] != "2":
        fail(f"self.history[0] = {hist}")

    c.call("setBreakpoints", {"source": {"path": os.path.join(game, "scripts", "main.plm")}, "breakpoints": []})
    c.call("continue")
    c.call("disconnect", {"terminateDebuggee": True})
    c.wait(lambda m: m.get("event") == "terminated", "terminated")
    try:
        code = c.proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        c.proc.kill()
        fail("the runtime didn't exit after disconnect")
    if not any("dap game ready" in o for o in outputs):
        fail(f"script print not forwarded: {outputs[:10]}")
    print(f"ok: dap session (exit code {code})")


main()

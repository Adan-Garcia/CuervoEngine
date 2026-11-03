#!/usr/bin/env python3
"""Reference WebSocket datagram relay for Cuervo networking (DESIGN.md 4.19).

Clients connect to ws://host:port/<room>. The relay assigns ids 0,1,2... per room in join
order and sends each client one binary message holding its id (u32 little endian). After
that a client sends [to:u32 LE][payload] and the target receives [from:u32 LE][payload];
to = 0xFFFFFFFF broadcasts to the rest of the room. Datagram semantics: nothing is queued
for absent peers. Plain ws:// by default; --tls-cert/--tls-key serve wss:// directly (tests, small deployments).

Usage: relay.py [--port N] [--host H]   (port 0 = ephemeral; the chosen port is printed
as "listening <port>" on stdout once ready).
"""
import argparse, base64, hashlib, os, socket, ssl, struct, sys, threading

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_MSG = 1 << 16
rooms = {}  # room -> {id: client}
lock = threading.Lock()


class Client:
    def __init__(self, sock):
        self.sock, self.wlock = sock, threading.Lock()

    def send(self, payload):
        n = len(payload)
        head = bytes([0x82]) + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n))
        with self.wlock:
            try:
                self.sock.sendall(head + payload)
            except OSError:
                pass


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def read_message(sock):
    """Returns (opcode, payload) of the next complete message; handles masking/fragments."""
    data, op0 = b"", None
    while True:
        b0, b1 = read_exact(sock, 2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", read_exact(sock, 2))[0]
        elif n == 127:
            n = struct.unpack(">Q", read_exact(sock, 8))[0]
        if n > MAX_MSG:
            raise EOFError
        mask = read_exact(sock, 4) if b1 & 0x80 else None
        p = read_exact(sock, n)
        if mask:
            p = bytes(c ^ mask[i & 3] for i, c in enumerate(p))
        op = b0 & 0x0F
        if op >= 8:
            return op, p
        op0 = op if op0 is None else op0
        data += p
        if b0 & 0x80:
            return op0, data


def serve(sock, tls=None):
    room = cid = None
    try:
        if tls is not None:
            sock = tls.wrap_socket(sock, server_side=True)
        c = Client(sock)
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = sock.recv(4096)
            if not chunk or len(req) > 8192:
                return
            req += chunk
        lines = req.decode("latin1").split("\r\n")
        parts = lines[0].split(" ")
        path = parts[1] if len(parts) > 1 else "/"
        hdr = {l.split(":", 1)[0].strip().lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        key = hdr.get("sec-websocket-key")
        if not key:
            sock.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            return
        acc = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
        sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
        room = path.lstrip("/")
        with lock:
            members = rooms.setdefault(room, {})
            cid = next(i for i in range(len(members) + 1) if i not in members)
            members[cid] = c
        c.send(struct.pack("<I", cid))
        while True:
            op, p = read_message(sock)
            if op == 8:
                return
            if op == 9:  # ping -> pong
                with c.wlock:
                    sock.sendall(bytes([0x8A, len(p)]) + p)
            elif op == 2 and len(p) >= 4:
                to = struct.unpack("<I", p[:4])[0]
                out = struct.pack("<I", cid) + p[4:]
                with lock:
                    targets = [m for i, m in rooms[room].items() if i != cid and to in (i, 0xFFFFFFFF)]
                for t in targets:
                    t.send(out)
    except (EOFError, OSError, ValueError):
        pass
    finally:
        with lock:
            if room is not None and cid is not None:
                rooms.get(room, {}).pop(cid, None)
        sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--port-file", help="write the bound port here once listening")
    ap.add_argument("--tls-cert", help="PEM certificate: serve wss:// instead of ws://")
    ap.add_argument("--tls-key", help="PEM private key for --tls-cert")
    ap.add_argument("--exit-on-stdin-eof", action="store_true", help="exit when stdin closes (test harnesses)")
    a = ap.parse_args()
    tls = None
    if a.tls_cert:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(a.tls_cert, a.tls_key)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.host, a.port))
    srv.listen(16)
    print("listening", srv.getsockname()[1], flush=True)
    if a.port_file:
        with open(a.port_file + ".tmp", "w") as f:
            f.write(str(srv.getsockname()[1]))
        os.replace(a.port_file + ".tmp", a.port_file)
    if a.exit_on_stdin_eof:
        def watch():
            sys.stdin.read()
            os._exit(0)
        threading.Thread(target=watch, daemon=True).start()
    while True:
        s, _ = srv.accept()
        threading.Thread(target=serve, args=(s, tls), daemon=True).start()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Standalone PSP ad-hoc lobby server, wire-compatible with PPSSPP's "PRO ad-hoc server".

Clients (PPSSPP, Pataport) keep one TCP connection (default port 27312) open and
exchange packed packets: one opcode byte, then fixed-size fields. Multi-byte
integers are little-endian; the IP is the raw IPv4 address in network order.

Client -> server:
  0 PING        [0]                                   1   refreshes idle timer
  1 LOGIN       [1][mac 6][nickname 128][product 9]   144 must be first; no reply
  2 CONNECT     [2][group 8]                          9   join/create a group
  3 DISCONNECT  [3]                                   1   leave the group
  4 SCAN        [4]                                   1   list groups of this game
  7 CHAT        [7][message 64]                       65  relay to group
Server -> client:
  2 CONNECT       [2][nickname 128][mac 6][ip 4]      139 this peer is in your group
  3 DISCONNECT    [3][ip 4]                           5   peer with this IP left
  4 SCAN          [4][group 8][bssid 6]               15  one per group
  5 SCAN_COMPLETE [5]                                 1   end of scan results
  6 CONNECT_BSSID [6][mac 6]                          7   you joined; group BSSID
  7 CHAT          [7][message 64][nickname 128]       193 relayed chat

Rules: LOGIN needs a unicast non-zero MAC and a product code matching [A-Z0-9]{9},
else the connection is dropped (as is any other packet before LOGIN, a second
LOGIN, or an unknown opcode). A new login with an already-used MAC drops the old
session. Players and groups are partitioned by product code. A group's BSSID is
its creator's MAC. CONNECT while already in a group is ignored. SCAN is answered
whether or not the client is in a group. Leaving (DISCONNECT, socket close, idle
timeout) notifies the remaining members; empty groups are deleted. The IP sent to
peers is the TCP peer address as seen by this server.
"""
import argparse
import re
import selectors
import socket
import sys
import time

PING, LOGIN, CONNECT, DISCONNECT, SCAN, SCAN_COMPLETE, CONNECT_BSSID, CHAT = range(8)
C2S_LEN = {PING: 1, LOGIN: 144, CONNECT: 9, DISCONNECT: 1, SCAN: 1, CHAT: 65}
PRODUCT_RE = re.compile(rb"[A-Z0-9]{9}")
MAX_OUT = 256 * 1024  # unsent bytes per client before it's dropped as stuck
R, W = selectors.EVENT_READ, selectors.EVENT_WRITE


class Client:
    def __init__(self, sock, addr):
        self.sock, self.ip, self.addr = sock, socket.inet_aton(addr[0]), "%s:%d" % addr[:2]
        self.buf, self.out, self.writing = bytearray(), bytearray(), False
        self.mac = self.nick = self.product = self.group = None
        self.last = time.monotonic()

    def __str__(self):
        name = self.nick.rstrip(b"\0").decode("utf-8", "replace") if self.nick else "?"
        mac = self.mac.hex(":") if self.mac else "-"
        return f"{self.addr} {mac} {name!r}" + (f" [{self.product.decode()}]" if self.product else "")


class Group:
    def __init__(self, bssid):
        self.bssid, self.members = bssid, []


class Server:
    def __init__(self, host="0.0.0.0", port=27312, timeout=15.0, verbose=False):
        self.timeout, self.verbose, self.running = timeout, verbose, True
        self.lsock = socket.create_server((host, port), backlog=128)
        self.lsock.setblocking(False)
        self.port = self.lsock.getsockname()[1]
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.lsock, R)
        self.clients = {}  # socket -> Client
        self.by_mac = {}   # mac -> logged-in Client
        self.groups = {}   # (product, group name) -> Group
        self.dead = {}     # Client -> reason; dropped in reap() to avoid re-entrancy

    def log(self, msg):
        if self.verbose:
            print(time.strftime("%H:%M:%S"), msg, file=sys.stderr, flush=True)

    def serve_forever(self):
        try:
            while self.running:
                for key, ev in self.sel.select(min(1.0, self.timeout / 4)):
                    if key.fileobj is self.lsock:
                        self.accept()
                        continue
                    c = key.data
                    if ev & W and c not in self.dead:
                        self.flush(c)
                    if ev & R and c not in self.dead:
                        self.read(c)
                    self.reap()
                now = time.monotonic()
                for c in list(self.clients.values()):
                    if now - c.last > self.timeout:
                        self.kill(c, "timeout")
                self.reap()
        finally:
            for c in list(self.clients.values()):
                self.drop(c, "shutdown")
            self.sel.close()
            self.lsock.close()

    def stop(self):
        self.running = False

    def accept(self):
        try:
            sock, addr = self.lsock.accept()
        except OSError:
            return
        sock.setblocking(False)
        c = self.clients[sock] = Client(sock, addr)
        self.sel.register(sock, R, c)

    def kill(self, c, why):
        self.dead.setdefault(c, why)

    def reap(self):
        while self.dead:
            c, why = self.dead.popitem()
            self.drop(c, why)

    def drop(self, c, why):
        if self.clients.pop(c.sock, None) is None:
            return
        self.sel.unregister(c.sock)
        c.sock.close()
        if self.by_mac.get(c.mac) is c:
            del self.by_mac[c.mac]
        self.leave(c)
        self.log(f"drop {c}: {why}")

    def send(self, c, data):
        if c in self.dead or c.sock not in self.clients:
            return
        c.out += data
        if len(c.out) > MAX_OUT:
            return self.kill(c, "send buffer overflow")
        self.flush(c)

    def flush(self, c):
        try:
            del c.out[:c.sock.send(c.out)]
        except BlockingIOError:
            pass
        except OSError as e:
            return self.kill(c, f"send error: {e}")
        if bool(c.out) != c.writing:
            c.writing = bool(c.out)
            self.sel.modify(c.sock, R | W if c.writing else R, c)

    def read(self, c):
        try:
            data = c.sock.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if not data:
            return self.kill(c, "connection closed")
        c.last = time.monotonic()
        c.buf += data
        while c.buf and c not in self.dead:
            n = C2S_LEN.get(c.buf[0])
            if n is None:
                return self.kill(c, f"unknown opcode {c.buf[0]}")
            if len(c.buf) < n:
                return
            pkt = bytes(c.buf[:n])
            del c.buf[:n]
            self.handle(c, pkt)

    def handle(self, c, pkt):
        op = pkt[0]
        if (c.mac is None) != (op == LOGIN):
            return self.kill(c, "LOGIN expected" if c.mac is None else "repeated LOGIN")
        if op == LOGIN:
            self.login(c, pkt[1:7], pkt[7:135], pkt[135:144])
        elif op == CONNECT:
            self.join(c, pkt[1:9])
        elif op == DISCONNECT:
            self.leave(c)
        elif op == SCAN:
            for (product, name), g in self.groups.items():
                if product == c.product:
                    self.send(c, bytes([SCAN]) + name + g.bssid)
            self.send(c, bytes([SCAN_COMPLETE]))
        elif op == CHAT and c.group:
            for m in self.groups[c.group].members:
                if m is not c:
                    self.send(m, bytes([CHAT]) + pkt[1:65] + c.nick)

    def login(self, c, mac, nick, product):
        if mac == bytes(6) or mac[0] & 1 or not PRODUCT_RE.fullmatch(product):
            return self.kill(c, f"bad login mac={mac.hex(':')} product={product!r}")
        old = self.by_mac.get(mac)
        if old is not None:
            self.drop(old, "replaced by new login")
        c.mac, c.nick, c.product = mac, nick, product
        self.by_mac[mac] = c
        self.log(f"login {c}")

    def join(self, c, name):
        if c.group:
            return  # already in a group: ignored, client must DISCONNECT first
        c.group = key = (c.product, name)
        g = self.groups.get(key) or self.groups.setdefault(key, Group(c.mac))
        for m in g.members:
            self.send(c, bytes([CONNECT]) + m.nick + m.mac + m.ip)
            self.send(m, bytes([CONNECT]) + c.nick + c.mac + c.ip)
        g.members.append(c)
        self.send(c, bytes([CONNECT_BSSID]) + g.bssid)
        self.log(f"join {c} -> {name.rstrip(b'\0')!r} ({len(g.members)} members)")

    def leave(self, c):
        if not c.group:
            return
        key, c.group = c.group, None
        g = self.groups[key]
        g.members.remove(c)
        for m in g.members:
            self.send(m, bytes([DISCONNECT]) + c.ip)
        if not g.members:
            del self.groups[key]
        self.log(f"leave {c} <- {key[1].rstrip(b'\0')!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=27312)
    ap.add_argument("--timeout", type=float, default=15.0, help="idle timeout in seconds")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    srv = Server(args.host, args.port, args.timeout, args.verbose)
    srv.log(f"listening on {args.host}:{srv.port}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

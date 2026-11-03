# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for ``tools/adhoc_server.py``: an in-thread server on a free localhost port, raw sockets."""
import socket
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import adhoc_server  # noqa: E402

LOCAL_IP = socket.inet_aton("127.0.0.1")
MAC_A, MAC_B, MAC_C = bytes.fromhex("020000000001"), bytes.fromhex("020000000002"), bytes.fromhex("020000000003")
GAME, OTHER_GAME = b"UCES01421", b"UCUS98751"


def nick(name):
    return name.ljust(128, b"\0")


def login(mac, name, product=GAME):
    return b"\x01" + mac + nick(name) + product


def connect(group):
    return b"\x02" + group.ljust(8, b"\0")


def connect_s2c(name, mac):
    return b"\x02" + nick(name) + mac + LOCAL_IP


def bssid(mac):
    return b"\x06" + mac


class ServerCase(unittest.TestCase):
    timeout = 5.0

    def setUp(self):
        self.srv = adhoc_server.Server("127.0.0.1", 0, timeout=self.timeout)
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()
        self.socks = []

    def tearDown(self):
        for s in self.socks:
            s.close()
        self.srv.stop()
        try:  # wake select() so the loop sees stop() now; it may already have exited
            socket.create_connection(("127.0.0.1", self.srv.port)).close()
        except ConnectionError:
            pass
        self.thread.join(2)
        self.assertFalse(self.thread.is_alive())

    def client(self, *chunks):
        s = socket.create_connection(("127.0.0.1", self.srv.port), timeout=2)
        self.socks.append(s)
        for chunk in chunks:
            s.sendall(chunk)
        return s

    def expect(self, s, data):
        got = b""
        while len(got) < len(data):
            part = s.recv(len(data) - len(got))
            self.assertTrue(part, f"connection closed, got {got!r}")
            got += part
        self.assertEqual(got, data)

    def assert_silent(self, s, wait=0.15):
        s.settimeout(wait)
        with self.assertRaises(TimeoutError):
            s.recv(1)
        s.settimeout(2)

    def assert_closed(self, s):
        try:
            self.assertEqual(s.recv(1), b"")
        except ConnectionResetError:
            pass

    def make_group(self):
        """A creates G1, B joins it; returns (a, b) with all join traffic consumed."""
        a = self.client(login(MAC_A, b"Alice"), connect(b"G1"))
        self.expect(a, bssid(MAC_A))
        b = self.client(login(MAC_B, b"Bob"), connect(b"G1"))
        self.expect(b, connect_s2c(b"Alice", MAC_A) + bssid(MAC_A))
        self.expect(a, connect_s2c(b"Bob", MAC_B))
        return a, b


class LobbyTest(ServerCase):
    def test_join_group(self):
        a, b = self.make_group()
        self.assert_silent(a)
        self.assert_silent(b)

    def test_scan_is_partitioned_by_product(self):
        self.make_group()
        other = self.client(login(MAC_C, b"Carol", OTHER_GAME), b"\x04")
        self.expect(other, b"\x05")
        same = self.client(login(bytes.fromhex("020000000004"), b"Dave"), b"\x04")
        self.expect(same, b"\x04" + b"G1".ljust(8, b"\0") + MAC_A + b"\x05")
        # Same group name in another game is a separate group with its own BSSID.
        other.sendall(connect(b"G1"))
        self.expect(other, bssid(MAC_C))

    def test_disconnect_opcode(self):
        a, b = self.make_group()
        b.sendall(b"\x03")
        self.expect(a, b"\x03" + LOCAL_IP)
        b.sendall(connect(b"G2"))  # B is out of G1, so it can create G2
        self.expect(b, bssid(MAC_B))

    def test_socket_close(self):
        a, b = self.make_group()
        b.close()
        self.expect(a, b"\x03" + LOCAL_IP)

    def test_last_member_leaving_deletes_group(self):
        a, b = self.make_group()
        b.close()
        self.expect(a, b"\x03" + LOCAL_IP)
        a.close()
        scanner = self.client(login(MAC_C, b"Carol"), b"\x04")
        self.expect(scanner, b"\x05")

    def test_split_and_coalesced_packets(self):
        pkt = login(MAC_A, b"Alice")
        a = self.client(pkt[:1], pkt[1:50])
        time.sleep(0.05)
        a.sendall(pkt[50:] + connect(b"G1")[:4])
        time.sleep(0.05)
        a.sendall(connect(b"G1")[4:])
        self.expect(a, bssid(MAC_A))
        b = self.client(login(MAC_B, b"Bob") + connect(b"G1") + b"\x00\x04")
        self.expect(b, connect_s2c(b"Alice", MAC_A) + bssid(MAC_A) + b"\x04" + b"G1".ljust(8, b"\0") + MAC_A + b"\x05")

    def test_chat_relay(self):
        a, b = self.make_group()
        msg = b"hello".ljust(64, b"\0")
        a.sendall(b"\x07" + msg)
        self.expect(b, b"\x07" + msg + nick(b"Alice"))
        self.assert_silent(a)

    def test_relogin_replaces_old_session(self):
        a, b = self.make_group()
        a2 = self.client(login(MAC_A, b"Alice2"))
        self.assert_closed(a)
        self.expect(b, b"\x03" + LOCAL_IP)
        a2.sendall(connect(b"G1"))
        # The group keeps its creator's MAC as BSSID even after the creator left.
        self.expect(a2, connect_s2c(b"Bob", MAC_B) + bssid(MAC_A))

    def test_bad_logins_close(self):
        for first in (
            login(b"\xff" * 6, b"x"),                 # broadcast
            login(bytes.fromhex("010000000001"), b"x"),  # multicast
            login(bytes(6), b"x"),                    # all-zero
            login(MAC_A, b"x", b"uces01421"),         # bad product code
            login(MAC_A, b"x", b"UCES0142\0"),
            b"\x04",                                  # SCAN before LOGIN
            b"\x00",                                  # PING before LOGIN
            b"\x09",                                  # unknown opcode
        ):
            with self.subTest(first=first[:8]):
                self.assert_closed(self.client(first))

    def test_unknown_opcode_and_double_login_after_login_close(self):
        self.assert_closed(self.client(login(MAC_A, b"x"), b"\x08"))
        self.assert_closed(self.client(login(MAC_B, b"x"), login(MAC_B, b"x")))


class TimeoutTest(ServerCase):
    timeout = 0.4

    def test_idle_client_dropped_and_group_notified(self):
        a, b = self.make_group()
        deadline = time.monotonic() + 3
        got = b""
        b.settimeout(0.1)
        while len(got) < 5 and time.monotonic() < deadline:
            b.sendall(b"\x00")  # B keeps pinging, A stays idle
            try:
                got += b.recv(5 - len(got))
            except TimeoutError:
                pass
        self.assertEqual(got, b"\x03" + LOCAL_IP)
        a.settimeout(2)
        self.assert_closed(a)

    def test_silent_connection_dropped_before_login(self):
        self.assert_closed(self.client())


if __name__ == "__main__":
    unittest.main()

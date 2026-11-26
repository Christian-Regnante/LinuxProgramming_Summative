#!/usr/bin/env python3
"""Automated protocol-level tests for the event registration server.

Starts ./server on a free port with a temporary state/log directory and talks
to it over raw TCP sockets, checking every requirement with assertions:
authentication, event listing, registration rules, concurrency, unexpected
disconnections, timeouts, server-full handling, TCP framing, persistence and
graceful shutdown.

    python3 tests/protocol_test.py          (run from Project5/, after `make`)
"""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SERVER = os.environ.get("SERVER", os.path.join(ROOT, "server"))
TMP = tempfile.mkdtemp(prefix="p5test-")
STATE, LOG = os.path.join(TMP, "registrations.csv"), os.path.join(TMP, "server.log")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """Context manager that runs ./server and stops it with SIGINT."""

    def __init__(self, *extra, fresh=True):
        self.port = free_port()
        args = [SERVER, "-p", str(self.port), "--state", STATE, "--log", LOG,
                "--students", os.path.join(ROOT, "students.txt"), *extra]
        if fresh:
            args.append("--fresh")
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(100):                                   # wait until listening
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        time.sleep(0.05)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
        out, _ = self.proc.communicate(timeout=10)
        return out

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.output = self.stop()


class Conn:
    """A line-oriented client connection."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.welcome = self.line()

    def send(self, text):
        self.sock.sendall(text.encode())

    def cmd(self, text):
        self.send(text + "\n")
        return self.line()

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                return None                                    # EOF
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode()

    def block(self, first):
        """Read a multi-line reply (EVENTS/MYEVENTS ... END)."""
        lines = [first]
        while lines[-1] != "END":
            lines.append(self.line())
        return lines

    def login(self, sid):
        reply = self.cmd(f"AUTH {sid}")
        assert reply.startswith("OK AUTH"), reply
        return self

    def close(self):
        self.sock.close()


def event_counts(port):
    c = Conn(port).login("S1020")
    rows = c.block(c.cmd("LIST"))[1:-1]
    c.cmd("QUIT")
    c.close()
    return {int(r.split()[1].split("|")[0]): tuple(map(int, r.split("|")[2:4])) for r in rows}


class TestRegistrationServer(unittest.TestCase):

    def test_01_welcome_and_unauthenticated_access(self):
        with Server() as srv:
            c = Conn(srv.port)
            self.assertTrue(c.welcome.startswith("WELCOME"))
            for command in ("LIST", "REGISTER 101", "MYEVENTS"):
                self.assertTrue(c.cmd(command).startswith("ERR NOT_AUTHENTICATED"), command)
            self.assertTrue(c.cmd("HELLO").startswith("ERR NOT_AUTHENTICATED"))
            self.assertEqual(c.cmd("QUIT"), "BYE guest")
            c.close()

    def test_02_authentication(self):
        with Server() as srv:
            c = Conn(srv.port)
            self.assertTrue(c.cmd("AUTH S1001").startswith("OK AUTH S1001 Amina Uwase"))
            self.assertTrue(c.cmd("AUTH S1001").startswith("ERR BAD_REQUEST"))   # already authenticated
            self.assertTrue(c.cmd("FOO").startswith("ERR BAD_COMMAND"))
            # the same student cannot hold two sessions at once
            d = Conn(srv.port)
            self.assertTrue(d.cmd("AUTH S1001").startswith("ERR ALREADY_LOGGED_IN"))
            c.cmd("QUIT"); c.close()
            time.sleep(0.1)
            self.assertTrue(d.cmd("AUTH S1001").startswith("OK AUTH"))           # free again
            d.close()

    def test_03_three_failed_attempts_disconnect(self):
        with Server() as srv:
            c = Conn(srv.port)
            self.assertIn("attempt 1 of 3", c.cmd("AUTH X1"))
            self.assertIn("attempt 2 of 3", c.cmd("AUTH bad-id!"))
            self.assertIn("too many failed attempts", c.cmd("AUTH S9999"))
            self.assertIsNone(c.line())                                          # server closed
            c.close()

    def test_04_list_register_duplicate_full_unknown(self):
        with Server() as srv:
            c = Conn(srv.port).login("S1001")
            listing = c.block(c.cmd("LIST"))
            self.assertEqual(listing[0], "EVENTS 3")
            self.assertIn("EVENT 101|AI Workshop|0|5", listing)
            self.assertIn("EVENT 102|Cybersecurity Seminar|0|4", listing)
            self.assertIn("EVENT 103|Research Conference|0|6", listing)
            self.assertEqual(c.cmd("REGISTER 101"), "OK REGISTERED 101 1 5 AI Workshop")
            self.assertTrue(c.cmd("REGISTER 101").startswith("ERR DUPLICATE"))
            self.assertTrue(c.cmd("REGISTER 999").startswith("ERR NO_SUCH_EVENT"))
            self.assertTrue(c.cmd("REGISTER abc").startswith("ERR BAD_REQUEST"))
            self.assertTrue(c.cmd("REGISTER").startswith("ERR BAD_REQUEST"))
            self.assertEqual(c.block(c.cmd("MYEVENTS")), ["MYEVENTS 1", "EVENT 101|AI Workshop", "END"])
            # fill event 102 (capacity 4), then a fifth student is refused
            for sid in ("S1002", "S1003", "S1004", "S1005"):
                d = Conn(srv.port).login(sid)
                self.assertTrue(d.cmd("REGISTER 102").startswith("OK REGISTERED 102"))
                d.cmd("QUIT"); d.close()
            self.assertTrue(c.cmd("REGISTER 102").startswith("ERR EVENT_FULL"))
            # a student already registered for a full event gets DUPLICATE, not FULL
            d = Conn(srv.port).login("S1002")
            self.assertTrue(d.cmd("REGISTER 102").startswith("ERR DUPLICATE"))
            d.close(); c.close()

    def test_05_tcp_framing(self):
        with Server() as srv:
            c = Conn(srv.port)
            c.send("AU"); time.sleep(0.1); c.send("TH S10"); time.sleep(0.1); c.send("07\r\n")  # split + CRLF
            self.assertTrue(c.line().startswith("OK AUTH S1007"))
            c.send("MYEVENTS\nLIST\n")                                           # two commands, one segment
            self.assertEqual(c.block(c.line())[0], "MYEVENTS 0")
            self.assertEqual(c.block(c.line())[0], "EVENTS 3")
            c.close()

    def test_06_unexpected_disconnection(self):
        with Server() as srv:
            a = Conn(srv.port).login("S1008")
            a.cmd("REGISTER 103")
            # abort the connection with a TCP RST (SO_LINGER timeout 0), mid-session
            a.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
            a.close()
            b = Conn(srv.port).login("S1009")                                    # others keep working
            self.assertTrue(b.cmd("REGISTER 103").startswith("OK REGISTERED 103 2 6"))
            b.close()
            time.sleep(0.2)
            self.assertEqual(event_counts(srv.port)[103], (2, 6))               # S1008's seat kept
            again = Conn(srv.port)
            self.assertTrue(again.cmd("AUTH S1008").startswith("OK AUTH"))      # slot released
            again.close()
        self.assertIn("DISCONNECTED UNEXPECTEDLY", srv.output)

    def test_07_idle_timeout(self):
        with Server("--timeout", "2") as srv:
            c = Conn(srv.port).login("S1010")
            time.sleep(2.5)
            self.assertTrue(c.line().startswith("ERR TIMEOUT"))
            self.assertIsNone(c.line())
            c.close()
        self.assertIn("idle timeout", srv.output)

    def test_08_server_full(self):
        with Server("--max-clients", "5") as srv:
            conns = [Conn(srv.port) for _ in range(5)]
            extra = Conn(srv.port)
            self.assertTrue(extra.welcome.startswith("ERR SERVER_FULL"))
            extra.close()
            for c in conns:
                c.close()

    def test_09_concurrent_last_seats(self):
        """All 20 authorised students race for the 6 seats of event 103 at the same instant."""
        with Server("--race-delay-ms", "20", "--max-clients", "64") as srv:
            students = [f"S10{n:02d}" for n in range(1, 21)]
            conns = [Conn(srv.port).login(s) for s in students]
            barrier, results = threading.Barrier(len(conns)), []

            def attempt(c):
                barrier.wait()
                results.append(c.cmd("REGISTER 103"))

            threads = [threading.Thread(target=attempt, args=(c,)) for c in conns]
            for t in threads: t.start()
            for t in threads: t.join()
            ok = [r for r in results if r.startswith("OK REGISTERED")]
            full = [r for r in results if r.startswith("ERR EVENT_FULL")]
            self.assertEqual(len(ok), 6)
            self.assertEqual(len(full), len(students) - 6)
            self.assertEqual(sorted(int(r.split()[3]) for r in ok), [1, 2, 3, 4, 5, 6])   # distinct seats
            for c in conns: c.close()
            time.sleep(0.2)
            self.assertEqual(event_counts(srv.port)[103], (6, 6))

    def test_10_persistence_across_restart(self):
        with Server() as srv:
            first = Conn(srv.port).login("S1011")
            first.cmd("REGISTER 101")
            first.close()
        with Server(fresh=False) as srv:
            self.assertEqual(event_counts(srv.port)[101], (1, 5))
            c = Conn(srv.port).login("S1011")
            self.assertTrue(c.cmd("REGISTER 101").startswith("ERR DUPLICATE"))
            c.close()
        self.assertIn("1 registration(s) restored", srv.output)

    def test_11_graceful_shutdown_notifies_clients(self):
        srv = Server()
        c = Conn(srv.port).login("S1012")
        out = srv.stop()
        self.assertTrue(c.line().startswith("ERR SERVER_SHUTDOWN"))
        self.assertIsNone(c.line())
        c.close()
        self.assertIn("shutdown requested - closing 1 active session(s)", out)
        self.assertIn("final registrations:", out)
        self.assertEqual(srv.proc.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)

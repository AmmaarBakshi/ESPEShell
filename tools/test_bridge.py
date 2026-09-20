#!/usr/bin/env python3
"""End-to-end test of the host bridge, standing in for the ESP32.

Runs espehost.py as a real subprocess against a real socket, so it exercises
the parts that unit-testing the ops would miss: the connect handshake, line
framing, id matching, the error path, and the size cap that keeps a reply
inside the firmware's line buffer.

    python tools/test_bridge.py            # observe-only agent
    python tools/test_bridge.py -v         # show every line on the wire

Exits non-zero on the first failure.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from typing import Optional

HERE = os.path.dirname(os.path.abspath(__file__))
AGENT = os.path.join(HERE, "espehost.py")

# Must match HOST_BRIDGE_LINE_MAX in config.h - the whole point of the cap.
LINE_MAX = 8192

VERBOSE = "-v" in sys.argv
failures = 0


def log(*parts: object) -> None:
    if VERBOSE:
        print("   ", *parts)


def check(name: str, ok: bool, detail: str = "") -> None:
    global failures
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))
    if not ok:
        failures += 1


class FakeEsp:
    """The ESP32 side: listen, accept the agent, exchange lines."""

    def __init__(self) -> None:
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.conn: Optional[socket.socket] = None
        self.buf = b""
        self.next_id = 1

    def accept(self, timeout: float = 15.0) -> None:
        self.srv.settimeout(timeout)
        self.conn, _ = self.srv.accept()
        self.conn.settimeout(timeout)

    def read_line(self, timeout: float = 30.0) -> dict:
        assert self.conn
        self.conn.settimeout(timeout)
        while b"\n" not in self.buf:
            chunk = self.conn.recv(65536)
            if not chunk:
                raise AssertionError("agent closed the connection")
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        log("<-", raw[:160].decode("utf-8", "replace"))
        # The firmware refuses to buffer more than this, so the agent must
        # never emit a longer line.
        assert len(raw) + 1 <= LINE_MAX, f"line of {len(raw)+1} bytes exceeds {LINE_MAX}"
        return json.loads(raw.decode("utf-8"))

    def ask(self, op: str, args: str = "", timeout: float = 30.0) -> dict:
        assert self.conn
        req = {"id": self.next_id, "op": op}
        if args:
            req["args"] = args
        self.next_id += 1
        line = json.dumps(req, separators=(",", ":")) + "\n"
        log("->", line.strip())
        self.conn.sendall(line.encode())

        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self.read_line(timeout=max(1.0, deadline - time.time()))
            if msg.get("id") == req["id"]:
                return msg
            log("(event, ignored)", msg.get("ev"))
        raise AssertionError(f"no reply to {op} within {timeout}s")

    def close(self) -> None:
        for s in (self.conn, self.srv):
            if s:
                try:
                    s.close()
                except OSError:
                    pass


def main() -> int:
    esp = FakeEsp()
    print(f"fake ESP32 listening on 127.0.0.1:{esp.port}")

    proc = subprocess.Popen(
        [sys.executable, AGENT, "127.0.0.1", "-p", str(esp.port), "-r", "0"],
        stdout=subprocess.DEVNULL if not VERBOSE else None,
        stderr=subprocess.STDOUT if VERBOSE else subprocess.DEVNULL,
    )
    try:
        esp.accept()
        print("agent connected\n")

        hello = esp.read_line()
        check("hello is sent on connect", hello.get("ev") == "hello", str(hello)[:80])
        check("hello advertises caps", bool(hello.get("caps")), hello.get("caps", "")[:60])
        caps = hello.get("caps", "").split()

        # Observation ops are on by default; control ops are not.
        for name in ("sys", "cpu", "mem", "disk", "ping", "caps"):
            check(f"{name} is advertised", name in caps)
        for name in ("exec", "type", "key", "clip"):
            check(f"{name} is NOT advertised without its flag", name not in caps)

        print()
        reply = esp.ask("ping")
        check("ping replies ok", reply.get("ok") is True, str(reply)[:80])
        check("ping carries a scalar", reply.get("val") == 1)

        reply = esp.ask("sys")
        check("sys returns text", bool(reply.get("text")))
        check("sys text is multi-line", "\n" in reply.get("text", ""))

        reply = esp.ask("nosuchop")
        check("unknown op fails cleanly", reply.get("ok") is False, reply.get("err", "")[:60])
        check("unknown op explains itself", "unknown op" in reply.get("err", ""))

        # An op that raises must not take the link down with it.
        reply = esp.ask("cpu")
        check("link survives after an error", reply.get("ok") is True)

        # Ids must be matched, not assumed in order.
        r1, r2 = esp.ask("ping"), esp.ask("sys")
        check("replies carry their request id", r1["id"] != r2["id"])

        print()
        # The size cap: a reply the firmware could not buffer must be trimmed
        # by the agent rather than truncated mid-line on the wire.
        reply = esp.ask("io")
        if reply.get("ok"):
            encoded = len(json.dumps(reply, separators=(",", ":"), ensure_ascii=True)) + 1
            check("io reply fits the ESP32 line buffer", encoded <= LINE_MAX,
                  f"{encoded} bytes")
        else:
            check("io reports a reason when unavailable", bool(reply.get("err")),
                  reply.get("err", "")[:60])

    except AssertionError as exc:
        print(f"  FAIL  {exc}")
        globals()["failures"] += 1
    finally:
        esp.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print()
    if failures:
        print(f"{failures} check(s) failed")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

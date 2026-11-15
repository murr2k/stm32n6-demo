#!/usr/bin/env python3
"""Send remote-control commands to the board (see app/src/platform/ctl.h).

Usage: ctl.py [--host IP] "screen 2" "stats" "swipe 600 240 200 240 200"

As a module, send() keeps one connection per host open and reuses it. That
matters: the board serves one client at a time and has 16 TCP connection
slots that linger after a close, so a script that opened a connection per
command (an injected drag is 80 commands a second) used them up and the
board then looked dead for tens of seconds.
"""
from __future__ import annotations

import argparse
import os
import socket
import sys
import time

DEFAULT_HOST = os.environ.get("N6_HOST", "192.168.254.86")
IDLE_LIMIT = 3.0  # the board drops a client that has been silent for 5 s

_open: dict[tuple[str, int], tuple[socket.socket, object, float]] = {}


def close(host: str, port: int = 5556) -> None:
    conn = _open.pop((host, port), None)
    if conn:
        try:
            conn[0].close()
        except OSError:
            pass


def _connection(host: str, port: int, timeout: float):
    conn = _open.get((host, port))
    if conn and time.monotonic() - conn[2] < IDLE_LIMIT:
        conn[0].settimeout(timeout)
        return conn[1]
    close(host, port)
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    f = s.makefile("rw", newline="\n")
    _open[(host, port)] = (s, f, time.monotonic())
    return f


def _ask(host: str, port: int, timeout: float, command: str) -> str:
    f = _connection(host, port, timeout)
    f.write(command + "\n")
    f.flush()
    reply = f.readline()
    if not reply:
        raise ConnectionResetError("the board closed the connection")
    s, _, _ = _open[(host, port)]
    _open[(host, port)] = (s, f, time.monotonic())
    return reply.strip()


def send(host: str, commands: list[str], port: int = 5556, timeout: float = 5.0) -> list[str]:
    replies = []
    for c in commands:
        if c.startswith("sleep "):
            time.sleep(float(c.split()[1]))
            continue
        try:
            replies.append(_ask(host, port, timeout, c))
        except OSError:
            # the kept connection had gone stale: once more on a fresh one
            close(host, port)
            try:
                replies.append(_ask(host, port, timeout, c))
            except OSError:
                close(host, port)
                raise
    return replies


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("commands", nargs="+")
    a = ap.parse_args()
    for c, r in zip([c for c in a.commands if not c.startswith("sleep ")], send(a.host, a.commands)):
        print(f"{c} -> {r}", file=sys.stdout)


if __name__ == "__main__":
    main()

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
import re
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


def margins(line: str) -> dict | None:
    """The board's "margins" reply (ctl.h) as numbers: frames on a deadline, near
    misses and their line (floor_us), the bins of time left at the hand-over (bin 0
    counts the misses), and per screen its first frame's least time left, how often
    it was entered and the least left by the frames after the first one while it
    faded in (None if none had a deadline). None if the reply is not one."""
    kv = dict(re.findall(r"(\w+)=(\S+)", line))
    if "hist" not in kv:
        return None
    first = {}
    for item in ([] if kv.get("first", "-") == "-" else kv["first"].split(",")):
        s, us, n, nxt = item.split(":")
        first[int(s)] = (int(us), int(n), None if nxt == "-" else int(nxt))
    return {"frames": int(kv["frames"]), "near": int(kv["near"]), "floor_us": int(kv["floor_us"]),
            "bin_us": int(kv["bin_us"]), "hist": [int(x) for x in kv["hist"].split(",")], "first": first}


def margin_report(m: dict, name=str) -> list[str]:
    """Report lines: how little time the tightest 0.1 %, 1 % and half of the frames
    left, the misses and near misses, and each screen's tightest first frame (the
    frame that builds it after a switch) and the tightest of the frames after it
    under the fade (they draw what the first one leaves out), tightest first."""
    h, n, b = m["hist"], m["frames"], m["bin_us"]
    if n == 0:
        return ["margins: no frame on a deadline"]

    def under(p: float) -> str:
        need, cum = n * p / 100, 0
        for k, c in enumerate(h):
            cum += c
            if cum >= need:
                return "a miss" if k == 0 else (f"{k * b / 1000:.1f} ms" if k < len(h) - 1 else
                                               f"{(k - 1) * b / 1000:.0f}+ ms")
        return "?"

    floor = m["floor_us"]
    out = [f"margins: {n} frames on a deadline, {h[0]} missed, {m['near']} near misses (under "
           f"{floor / 1000:.1f} ms left); time left: the tightest 0.1 % under {under(0.1)}, 1 % under "
           f"{under(1)}, half under {under(50)}"]
    def ms(us: int) -> str:
        return f"{us / 1000:.2f} ms{' MISSED' if us < 0 else ' NEAR' if us < floor else ''}"

    def least(v: tuple) -> int:
        return v[0] if v[2] is None else min(v[0], v[2])

    firsts = sorted(m["first"].items(), key=lambda kv: least(kv[1]))
    if firsts:
        out.append("entering a screen, least time left by its first frame / the next ones under the fade: " +
                   ", ".join(f"{name(s)} {ms(us)} / {'-' if nxt is None else ms(nxt)} ({k}x)"
                             for s, (us, k, nxt) in firsts))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("commands", nargs="+")
    a = ap.parse_args()
    for c, r in zip([c for c in a.commands if not c.startswith("sleep ")], send(a.host, a.commands)):
        print(f"{c} -> {r}", file=sys.stdout)


if __name__ == "__main__":
    main()

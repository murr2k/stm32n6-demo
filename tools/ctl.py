#!/usr/bin/env python3
"""Send remote-control commands to the board (see app/src/platform/ctl.h).

Usage: ctl.py [--host IP] "screen 2" "stats" "swipe 600 240 200 240 200"
"""
from __future__ import annotations

import argparse
import os
import socket
import sys
import time

DEFAULT_HOST = os.environ.get("N6_HOST", "192.168.254.86")


def send(host: str, commands: list[str], port: int = 5556, timeout: float = 5.0) -> list[str]:
    replies = []
    with socket.create_connection((host, port), timeout=timeout) as s:
        f = s.makefile("rw", newline="\n")
        for c in commands:
            if c.startswith("sleep "):
                time.sleep(float(c.split()[1]))
                continue
            f.write(c + "\n")
            f.flush()
            replies.append(f.readline().strip())
    return replies


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("commands", nargs="+")
    a = ap.parse_args()
    for c, r in zip([c for c in a.commands if not c.startswith("sleep ")], send(a.host, a.commands)):
        print(f"{c} -> {r}", file=sys.stdout)


if __name__ == "__main__":
    main()

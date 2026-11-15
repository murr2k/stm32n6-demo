#!/usr/bin/env python3
"""Host companion for the Host Link screen.

Sends this PC's load to the board ten times a second over UDP and times the
board's reply to each datagram. Only aggregate numbers leave the machine:
per-core CPU load, memory in use, network throughput. No process names, no
host name.

It opens no listening port: replies come back on the socket it sends from.

Usage: companion.py [--host 192.168.254.86] [--rate 10]
Protocol: app/src/platform/hostlink.h
"""
from __future__ import annotations

import argparse
import os
import socket
import struct
import sys
import time

try:
    import psutil
except ImportError:
    sys.exit("psutil is missing: .venv\\Scripts\\python.exe -m pip install psutil")

PORT = 5557
MAX_CORES = 32


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("N6_HOST", "192.168.254.86"))
    ap.add_argument("--rate", type=float, default=10.0, help="datagrams per second")
    ap.add_argument("--secs", type=float, help="stop after this long (default: run until Ctrl+C)")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.05)
    period = 1.0 / a.rate
    psutil.cpu_percent(percpu=True)  # prime the counters
    net0, t_net = psutil.net_io_counters(), time.perf_counter()
    seq, rtt_us, replies = 0, 0, 0
    t_end = time.time() + a.secs if a.secs else None
    next_t = time.perf_counter()
    print(f"sending to {a.host}:{PORT} at {a.rate:g}/s, Ctrl+C to stop")
    try:
        while t_end is None or time.time() < t_end:
            next_t += period
            time.sleep(max(0.0, next_t - time.perf_counter()))

            cores = psutil.cpu_percent(percpu=True)[:MAX_CORES]
            mem = psutil.virtual_memory().percent
            net1, t1 = psutil.net_io_counters(), time.perf_counter()
            dt = max(t1 - t_net, 1e-3)
            rx = int((net1.bytes_recv - net0.bytes_recv) / dt)
            tx = int((net1.bytes_sent - net0.bytes_sent) / dt)
            net0, t_net = net1, t1

            seq += 1
            pkt = struct.pack("<4sIIIIHBB", b"N6HL", seq, rtt_us, rx & 0xFFFFFFFF, tx & 0xFFFFFFFF,
                              int(sum(cores) / len(cores) * 10), int(mem), len(cores))
            pkt += bytes(min(100, int(c + 0.5)) for c in cores)
            t_send = time.perf_counter()
            s.sendto(pkt, (a.host, PORT))
            try:
                while True:
                    data, _ = s.recvfrom(64)
                    if len(data) == 8 and data[:4] == b"N6HR" and struct.unpack_from("<I", data, 4)[0] == seq:
                        rtt_us = int((time.perf_counter() - t_send) * 1e6)
                        replies += 1
                        break
            except (socket.timeout, ConnectionResetError):
                pass  # board not there (yet): keep sending
            if seq % int(a.rate * 5) == 0:
                print(f"sent {seq}, answered {replies}, last round trip {rtt_us} us")
    except KeyboardInterrupt:
        pass
    print(f"sent {seq}, answered {replies}")


if __name__ == "__main__":
    main()

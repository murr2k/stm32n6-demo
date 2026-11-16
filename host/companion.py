#!/usr/bin/env python3
"""Host companion for the Host Link screen.

Sends this PC's load to the board ten times a second over UDP and times the
board's reply to each datagram. Only aggregate numbers leave the machine:
per-core CPU load, memory in use, network throughput, and the CPU's core
layout (which logical processors are P-cores, E-cores, SMT siblings). No
process names, no host name.

It opens no listening port: replies come back on the socket it sends from.
Without --host it finds boards itself: it broadcasts a query on the board's
port and sends to every board that answers (asked again every 10 s, so a
board that appears later is picked up). The answer to a broadcast comes back
to the same socket, which Windows' firewall lets through without a prompt.

--cores N replaces the real load with a synthetic pattern on N processors,
to test how the screens cope with a large CPU. The board cannot tell it
apart from a real host: use it for tests only.

Usage: companion.py [--host IP] [--rate 10] [--cores N]
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
MAX_CORES = 255  # the datagram's core count is a byte; the board keeps the first 128
PLAIN, P_CORE, E_CORE, LPE_CORE = 0, 1, 2, 3


def topology(n: int) -> bytes:
    """The chip's layout for the Core Map screen: for each of the first n logical
    processors a kind byte (core type, SMT thread << 2), then for each the index
    of its physical core. Empty if it cannot be read (not Windows, or an error);
    the board then draws a plain grid.

    Types follow the efficiency class Windows reports: the highest class is a
    P-core; of the rest, a core that shares the L3 cache is an E-core and one
    that does not is a low-power E-core. A CPU with one class is not hybrid.
    """
    if sys.platform != "win32":
        return b""
    import ctypes

    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        size = ctypes.c_ulong(0)
        k32.GetLogicalProcessorInformationEx(0xFFFF, None, ctypes.byref(size))  # RelationAll
        buf = ctypes.create_string_buffer(size.value)
        if not k32.GetLogicalProcessorInformationEx(0xFFFF, buf, ctypes.byref(size)):
            return b""
        raw = buf.raw[:size.value]
    except (OSError, AttributeError):
        return b""

    cores: list[tuple[int, int]] = []  # (efficiency class, affinity mask), processor group 0 only
    l3_mask, off = 0, 0
    while off + 8 <= len(raw):
        rel, length = struct.unpack_from("<II", raw, off)
        if length == 0:
            break
        if rel == 0:  # RelationProcessorCore: EfficiencyClass at 9, GroupMask at 32
            mask, group = struct.unpack_from("<QH", raw, off + 32)
            if group == 0:
                cores.append((raw[off + 9], mask))
        elif rel == 2:  # RelationCache: Level at 8, Type at 16, GroupMask at 40
            (ctype,) = struct.unpack_from("<I", raw, off + 16)
            mask, group = struct.unpack_from("<QH", raw, off + 40)
            if group == 0 and raw[off + 8] == 3 and ctype in (0, 2):  # unified or data
                l3_mask |= mask
        off += length
    if not cores:
        return b""
    top = max(e for e, _ in cores)
    hybrid = top != min(e for e, _ in cores)
    kind, core_of = bytearray(n), bytearray(n)
    for index, (eff, mask) in enumerate(cores):
        ctype = PLAIN if not hybrid else P_CORE if eff == top else E_CORE if mask & l3_mask else LPE_CORE
        thread = 0
        for lp in range(64):
            if mask >> lp & 1:
                if lp < n:
                    kind[lp] = ctype | min(thread, 3) << 2
                    core_of[lp] = min(index, 255)
                thread += 1
    return bytes(kind) + bytes(core_of)


def discover(s: socket.socket, wait: float = 0.6) -> dict[str, str]:
    """Boards on the local network: {ip: their answer line}."""
    found: dict[str, str] = {}
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        s.sendto(b"N6HQ", ("255.255.255.255", PORT))
    except OSError:
        return found
    t_end = time.perf_counter() + wait
    while time.perf_counter() < t_end:
        try:
            data, addr = s.recvfrom(512)
        except (socket.timeout, ConnectionResetError):
            continue
        if data[:4] == b"N6HA":
            found[addr[0]] = data[5:].decode("ascii", "replace")
    return found


def synthetic(n: int, t: float) -> list[float]:
    """A moving pattern for --cores tests: waves that sweep across the processors."""
    import math

    return [max(0.0, min(100.0, 50 + 45 * math.sin(t * 1.3 + i * 0.37) * math.sin(t * 0.21 + i * 0.05)))
            for i in range(n)]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("N6_HOST"), help="board address (default: find boards by broadcast)")
    ap.add_argument("--cores", type=int, help="TEST ONLY: send a synthetic load on this many processors")
    ap.add_argument("--rate", type=float, default=10.0, help="datagrams per second")
    ap.add_argument("--secs", type=float, help="stop after this long (default: run until Ctrl+C)")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.05)
    period = 1.0 / a.rate
    n_cores = min(len(psutil.cpu_percent(percpu=True)), MAX_CORES)  # also primes the counters
    topo = topology(n_cores)
    if a.cores:
        n_cores, topo = min(a.cores, MAX_CORES), b""
    boards: list[str] = [a.host] if a.host else []
    t_discover = 0.0
    net0, t_net = psutil.net_io_counters(), time.perf_counter()
    seq, rtt_us, replies = 0, 0, 0
    t_end = time.time() + a.secs if a.secs else None
    next_t = time.perf_counter()
    print(f"sending to {a.host}:{PORT}" if a.host else "looking for boards (broadcast on udp/5557)",
          f"at {a.rate:g}/s, Ctrl+C to stop")
    try:
        while t_end is None or time.time() < t_end:
            next_t += period
            time.sleep(max(0.0, next_t - time.perf_counter()))

            if not a.host and time.perf_counter() - t_discover > (10.0 if boards else 2.0):
                t_discover = time.perf_counter()
                found = discover(s)
                for ip, line in found.items():
                    if ip not in boards:
                        print(f"found {ip}: {line}")
                boards = list(found) or boards  # keep sending to the last ones if a query goes unanswered
                next_t = time.perf_counter()
            if not boards:
                continue
            cores = psutil.cpu_percent(percpu=True)[:MAX_CORES]
            if a.cores:
                cores = synthetic(n_cores, time.perf_counter())
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
            if len(cores) == n_cores:
                pkt += topo
            t_send = time.perf_counter()
            for ip in boards:
                s.sendto(pkt, (ip, PORT))
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

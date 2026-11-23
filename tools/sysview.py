#!/usr/bin/env python3
"""SEGGER SystemView from the command line, for the demo's SystemView build.

  sysview.py record [--secs 10] [--name NAME] [--speed 20000] [--quit]
  sysview.py report NAME        report again from captures/sysview/NAME_*.csv
  sysview.py shot [FILE]        a picture of SystemView's window

record checks that the board runs the SystemView build (the build stamp it
announces must be in build-sysview's ELF; app/sysview.conf says how to build
and flash it), takes the RTT control block's address from that build's map,
asks the J-Link for its serial number (never stored) and starts SystemView
(-single -port 8785, or uses the one already listening there). Once the
window title shows a recording of the target it waits SECS seconds, then
tells SystemView's command server to stop, save NAME.SVDat and export the
contexts and the events, and reports: CPU per thread and interrupt (the
interrupts named from the chip's header), how late each thread ran after it
was made ready and what ran meanwhile, the interrupts' periods, the busiest
mutexes and semaphores by name, and lost (overflow) events. SystemView stays
open with the recording unless --quit.

SWD at 20 MHz by default: at 4 MHz the probe drained the RTT buffer too
slowly and events were lost. SystemView reads RTT while the core runs: this
tool never halts, resets or flashes anything.
"""
from __future__ import annotations

import argparse
import csv
import re
import socket
import statistics
import subprocess
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import segger  # noqa: E402

ROOT = segger.ROOT
BUILD = ROOT / "build-sysview" / "app" / "zephyr"
OUT = ROOT / "captures" / "sysview"
PORT = 8785                      # ~/.claude/port-registry.md
DEVICE = "STM32N657X0"
HEADER = ROOT / "modules/hal/stm32/stm32cube/stm32n6xx/soc/stm32n657xx.h"
# what the demo uses some interrupts for (the header only says which block)
ROLES = {
    "GPDMA1_Channel2": "audio out DMA",
    "GPDMA1_Channel6": "microphone DMA",
    "SysTick": "kernel tick",
    "SDMMC2": "microSD card",
    "ETH1": "Ethernet",
}


# ---------------------------------------------------------------- record

def rtt_address() -> int:
    m = re.search(r"^\s+0x([0-9a-f]+)\s+_SEGGER_RTT\s*$",
                  (BUILD / "zephyr.map").read_text(errors="replace"), re.M)
    if not m:
        raise SystemExit("_SEGGER_RTT not in build-sysview's map: is it the SystemView build?")
    return int(m.group(1), 16)


def check_board(host: str) -> None:
    elf = BUILD / "zephyr.elf"
    if not elf.exists():
        raise SystemExit("no build-sysview: see app/sysview.conf for how to build it")
    stamp = segger.board_build(host)
    if stamp is None:
        raise SystemExit(f"the board at {host} does not answer the host link's discovery")
    if not segger.elf_has_build(elf, stamp):
        raise SystemExit(f"the board runs build {stamp!r}, not build-sysview's: "
                         "n6.cmd -d build-sysview go --app-only --no-zprof -- '-Dapp_EXTRA_CONF_FILE=sysview.conf' "
                         "(n6.cmd go --app-only puts the release back)")


def sysview_pid() -> int | None:
    p = segger.pids("SystemView.exe")
    return p[0] if p else None


def title_has(pid: int, *words: str) -> str | None:
    for t in segger.window_titles(pid):
        if all(w in t for w in words):
            return t
    return None


class Server:
    """SystemView's command server: a line per command, no answers."""

    def __init__(self) -> None:
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.s.settimeout(1.0)
        try:
            self.s.recv(256)            # "SEGGER SystemView ... Command Server"
        except socket.timeout:
            pass

    def send(self, cmd: str) -> None:
        self.s.sendall((cmd + "\r\n").encode())

    def close(self) -> None:
        self.s.close()


def wait_file(path: Path, timeout: float = 240.0) -> None:
    end, last, stable = time.time() + timeout, -1, 0.0
    while time.time() < end:
        if path.exists():
            size = path.stat().st_size
            if size == last and size > 0:
                if time.time() - stable > 1.5:
                    return
            else:
                last, stable = size, time.time()
        time.sleep(0.25)
    raise SystemExit(f"SystemView did not write {path.name} (a dialog in its window? sysview.py shot)")


def record(args) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    name = args.name or time.strftime("%Y%m%d-%H%M%S")
    base = OUT / name
    if base.with_suffix(".SVDat").exists():
        raise SystemExit(f"{base}.SVDat exists: pick another --name")
    check_board(args.host)
    rtt = rtt_address()

    pid = sysview_pid()
    if pid and not segger.port_open(PORT):
        raise SystemExit("SystemView is open but not as this tool's instance (no command server "
                         f"on {PORT}): close it, then run this again")
    if pid:
        print(f"using the open SystemView (command server on {PORT})")
        srv = Server()
        srv.send("start")
    else:
        sn = segger.probe_serial()
        cmd = [str(segger.sysview_exe()), "-single", "-port", str(PORT), "-recorder", "J-Link",
               "-device", DEVICE, "-usb", sn, "-if", "SWD", "-speed", str(args.speed),
               "-rttcbaddr", f"0x{rtt:08X}", "-start"]
        subprocess.Popen(cmd, cwd=str(OUT))
        print(f"SystemView started: SWD {args.speed} kHz, RTT control block 0x{rtt:08X}")
        for _ in range(100):
            pid = sysview_pid()
            if pid and segger.port_open(PORT):
                break
            time.sleep(0.3)
        else:
            raise SystemExit("SystemView did not open its command server")
        srv = Server()

    # recording has begun once the title names a new recording of the target
    for _ in range(150):
        t = title_has(pid, "New Recording", "[")
        if t:
            break
        time.sleep(0.2)
    else:
        shot = OUT / f"{name}_window.png"
        print(segger.window_shot("SystemView", shot))
        raise SystemExit(f"SystemView did not start recording; its window: {shot}")
    t0 = time.time()
    print(f"recording {args.secs:g} s ...")
    time.sleep(args.secs)
    srv.send("stop")
    print(f"stopped after {time.time() - t0:.1f} s")
    time.sleep(1.0)
    srv.send(f"save {base}.SVDat")
    wait_file(base.with_suffix(".SVDat"))
    srv.send(f"export-contexts {base}_contexts.csv")
    wait_file(Path(f"{base}_contexts.csv"))
    srv.send(f"export {base}_events.csv")
    wait_file(Path(f"{base}_events.csv"))
    if args.quit:
        srv.send("quit")
    srv.close()
    print(report(base))


# ---------------------------------------------------------------- report

def irq_names() -> dict[int, str]:
    """SystemView's interrupt ids (exception numbers, IRQ + 16) to names."""
    names = {}
    for m in re.finditer(r"^\s*(\w+)_IRQn\s*=\s*(-?\d+)\s*,", HEADER.read_text(errors="replace"), re.M):
        n = m.group(1)
        names[int(m.group(2)) + 16] = f"{n} ({ROLES[n]})" if n in ROLES else n
    return names


def us(text: str) -> float | None:
    """'2.622 ms' / '0.365 us' / '7.258 048 128' (s) to microseconds."""
    m = re.search(r"(\d[\d ]*(?:\.[\d ]*)?)\s*(us|ms|s)?\s*$", text.strip())
    if not m:
        return None
    v = float(m.group(1).replace(" ", ""))
    return v * {"us": 1.0, "ms": 1e3, "s": 1e6, None: 1e6}[m.group(2)]


def label(ctx: str, irqs: dict[int, str], isr: int | None = None) -> str:
    """A context's name; an interrupt's with its number and what it is. With the
    symbol table SystemView names an interrupt by its handler, and two handlers
    can share a name (static functions in different files): the number tells
    them apart."""
    m = re.fullmatch(r"ISR (\d+)", ctx)
    if m:
        return f"{ctx} {irqs.get(int(m.group(1)), '?')}"
    if isr is None:
        return ctx
    what = irqs.get(isr, "?")
    role = re.search(r"\((.+)\)$", what)       # "GPDMA1_Channel2 (audio out DMA)": the role will do
    return f"{ctx} #{isr} {role.group(1) if role else what}"


def isr_of(contextint: str) -> int | None:
    """The interrupt number in an event row's context id (high word 1: an interrupt)."""
    try:
        v = int(contextint, 16)
    except ValueError:
        return None
    return v & 0xFFFFFFFF if v >> 32 == 1 else None


def pct(sorted_vals: list[float], p: float) -> float:
    return sorted_vals[min(len(sorted_vals) - 1, int(len(sorted_vals) * p))]


def report(base: Path) -> str:
    irqs = irq_names()
    syms = segger.elf_symbols(BUILD / "zephyr.elf")
    rows: list[tuple[float, str, str, str]] = []
    with open(f"{base}_events.csv", encoding="utf-8", errors="replace", newline="") as f:
        for r in csv.DictReader(f):
            ctx = label(r["context"], irqs, isr_of(r.get("contextint", "")))
            rows.append((us(r["timestamp"] + " s") or 0.0, ctx, r["event"], r["detail"][:120]))
    lines: list[str] = []
    out = lines.append
    span = (rows[-1][0] - rows[0][0]) / 1e6 if rows else 0.0
    lost = sum(1 for _, _, e, _ in rows if "overflow" in e.lower())
    out(f"{base.name}: {span:.2f} s, {len(rows)} events ({len(rows) / max(span, 1e-9):.0f}/s), "
        f"overflow events {lost}" + ("  <- events were lost" if lost else ""))

    # CPU per context (SystemView's own figures)
    ctxs = list(csv.DictReader(open(f"{base}_contexts.csv", encoding="utf-8", errors="replace")))

    def load(r):
        try:
            return float(r["CPU Load"].replace("%", "").strip())
        except ValueError:
            return 0.0
    ctxs = [r for r in ctxs if r["Name"] != "Idle"]
    ctxs.sort(key=load, reverse=True)
    out("\nCPU by context (Idle left out)")
    out(f"  {'context':44} {'CPU':>7} {'runs/s':>8} {'longest run':>12}")
    for r in ctxs[:18]:
        if load(r) <= 0 and r["Activations"].strip() in ("", "0"):
            continue
        act = int(r["Activations"].replace(" ", "") or 0)
        mx = us(r["Max Run Time"]) or 0.0
        t = re.fullmatch(r"#(\d+)", r.get("Type", "").strip())
        name = label(r["Name"], irqs, int(t.group(1)) if t else None)
        out(f"  {name[:44]:44} {load(r):6.2f}% {act / max(span, 1e-9):8.0f} {mx:9.1f} us")

    # how late threads ran after being made ready, and what ran meanwhile
    lat: dict[str, list[tuple[float, int]]] = defaultdict(list)
    waiting: dict[str, int] = {}
    for i, (t, c, e, d) in enumerate(rows):
        if e == "Task Ready":
            m = re.match(r"(.+?), runs after (.+)$", d)
            if m:
                waiting[m.group(1)] = i
                v = us(m.group(2))
                if v is not None:
                    lat[m.group(1)].append((v, i))
    out("\nready to running, per thread (us)")
    out(f"  {'thread':16} {'n':>6} {'median':>8} {'p99':>8} {'worst':>8}  ran in between, worst case")
    for th, vals in sorted(lat.items(), key=lambda kv: -max(v for v, _ in kv[1])):
        s = sorted(v for v, _ in vals)
        worst_v, worst_i = max(vals)
        between: Counter = Counter()        # time each context ran meanwhile, us
        for k in range(worst_i, len(rows) - 1):
            t, c, e, d = rows[k]
            if k > worst_i and c == th and e == "Task Run":
                break
            if c not in ("Idle", "Scheduler", th):
                between[label(c, irqs)] += rows[k + 1][0] - t
        who = ", ".join(f"{n} {v / 1000:.1f} ms" if v >= 1000 else f"{n} {v:.0f} us"
                        for n, v in between.most_common(3))
        out(f"  {th[:16]:16} {len(s):6} {statistics.median(s):8.1f} {pct(s, 0.99):8.1f} {worst_v:8.1f}  {who}")
        if len(lines) > 80:
            break

    # interrupt periods
    enters: dict[str, list[float]] = defaultdict(list)
    for t, c, e, d in rows:
        if e == "ISR Enter":
            enters[c].append(t)
    out("\ninterrupt periods (us)")
    for c, ts in sorted(enters.items(), key=lambda kv: -len(kv[1])):
        if len(ts) < 10:
            continue
        p = [b - a for a, b in zip(ts, ts[1:])]
        out(f"  {label(c, irqs)[:44]:44} n {len(ts):6}  mean {statistics.mean(p):9.1f}  "
            f"sd {statistics.pstdev(p):7.2f}  min {min(p):9.1f}  max {max(p):9.1f}")

    # busiest kernel objects
    objs = Counter()
    for t, c, e, d in rows:
        m = re.search(r"\b(mutex|sem|msgq|queue|fifo|lifo|slab)=0x([0-9A-Fa-f]+)", d)
        if m and e in ("k_mutex_lock", "k_sem_take", "k_sem_give", "k_msgq_get", "k_msgq_put"):
            a = int(m.group(2), 16)
            objs[(e, syms.get(a, f"0x{a:08X}"), c)] += 1
    out("\nbusiest kernel objects (calls/s)")
    for (e, obj, c), n in objs.most_common(8):
        out(f"  {e:13} {obj[:28]:28} by {c[:16]:16} {n / max(span, 1e-9):8.0f}")
    text = "\n".join(lines)
    Path(f"{base}_report.txt").write_text(text + "\n", encoding="utf-8")
    return text


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("--secs", type=float, default=10.0)
    r.add_argument("--name")
    r.add_argument("--speed", type=int, default=20000, help="SWD kHz (4000 lost events)")
    r.add_argument("--host", default=segger.DEFAULT_HOST)
    r.add_argument("--quit", action="store_true", help="close SystemView afterwards")
    r.set_defaults(fn=record)
    p = sub.add_parser("report")
    p.add_argument("name")
    p.set_defaults(fn=lambda a: print(report(OUT / a.name)))
    s = sub.add_parser("shot")
    s.add_argument("file", nargs="?", default=str(OUT / "window.png"))
    s.set_defaults(fn=lambda a: print(segger.window_shot("SystemView", Path(a.file))))
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()

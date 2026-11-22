#!/usr/bin/env python3
"""SEGGER Ozone on this board, through Ozone's automation socket. Attach only.

  ozone.py start            Ozone with tools/n6demo.jdebug, socket on 8784, log in captures/ozone/
  ozone.py attach           open the ELF of the build the board runs, attach, make sure it runs on
  ozone.py detach           stop debugging (the program runs on)
  ozone.py cmd "Debug.IsHalted();" "Target.ReadU32(0xE000EDF0);" ...
                            console commands, each printed with Ozone's reply
  ozone.py read ADDR [N]    N 32-bit words of the target's memory (while it runs)
  ozone.py threads          the Zephyr threads from Ozone's RTOS window: only while the
                            program is halted (a breakpoint); for a running board use
                            ctl.py stacks or sysview.py
  ozone.py shot [FILE]      a picture of Ozone's window
  ozone.py quit

Measured (2026-11-20, SWD 20 MHz): an attach halts the core for about 0.7 s
(the project's OnStartupComplete lets it run on). Ozone runs console commands
one at a time, so each command here waits for Ozone's echo of it before the
next is sent: commands sent without waiting queued up behind slow ones and
kept the core halted 15 s. Commands that would reset the chip or write the
program or the flash are refused; tools/n6.py is the only way to flash and
start this board (tools/n6demo.jdebug says why).
"""
from __future__ import annotations

import argparse
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import segger  # noqa: E402

ROOT = segger.ROOT
PORT = 8784                       # ~/.claude/port-registry.md
PROJECT = ROOT / "tools" / "n6demo.jdebug"
OUT = ROOT / "captures" / "ozone"
ELFS = [ROOT / "build" / "app" / "zephyr" / "zephyr.elf",
        ROOT / "build-sysview" / "app" / "zephyr" / "zephyr.elf"]
REFUSED = re.compile(r"\b(Exec\.(Reset|Download)|Debug\.(Reset|Download|SetConnectMode|Connect)"
                     r"|Target\.(LoadMemory|Erase\w*|Program\w*|Fill\w*)|File\.(Load|Download)\w*"
                     r"|Project\.SetJLinkScript)\b")


class Console:
    """Ozone's automation socket: console commands in, console text out."""

    def __init__(self) -> None:
        if not segger.port_open(PORT):
            raise SystemExit(f"Ozone is not listening on {PORT}: ozone.py start")
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.read(0.8)                       # welcome line

    def read(self, secs: float) -> str:
        self.s.settimeout(0.05)
        out, end = b"", time.time() + secs
        while time.time() < end:
            try:
                b = self.s.recv(65536)
                if not b:
                    break
                out += b
            except socket.timeout:
                pass
        return out.decode("utf-8", "replace")

    def run(self, cmd: str, timeout: float = 10.0, until: str | None = None) -> str:
        """Send CMD; return once Ozone has echoed it (it ran) and UNTIL, if given, appeared."""
        if REFUSED.search(cmd):
            raise SystemExit(f"refused: {cmd} (reset, download and flash writes break booting here)")
        if not cmd.rstrip().endswith(";"):
            cmd += ";"
        name = cmd.split("(")[0].strip()
        echo = re.compile(re.escape(name) + r"\s*\(")
        self.s.sendall((cmd + "\n").encode())
        text, end = "", time.time() + timeout
        while time.time() < end:
            text += self.read(0.05)
            if echo.search(text) and (until is None or re.search(until, text)):
                text += self.read(0.1)        # the rest of its output
                return text
        raise SystemExit(f"Ozone did not finish {cmd} within {timeout:g} s:\n{text.strip()[-400:]}")

    def halted(self) -> int | None:
        m = re.search(r"returns (0x[0-9A-Fa-f]+)", self.run("Debug.IsHalted();", 5.0, r"returns"))
        return int(m.group(1), 16) if m else None

    def close(self) -> None:
        self.s.close()


def board_elf() -> Path | None:
    """The ELF of the build the board runs (its announced build stamp is in it)."""
    stamp = segger.board_build()
    for elf in ELFS:
        if stamp and elf.exists() and segger.elf_has_build(elf, stamp):
            return elf
    return None


def start(_a) -> None:
    if segger.port_open(PORT):
        print(f"Ozone is already running (socket on {PORT})")
        return
    if segger.pids("Ozone.exe"):
        raise SystemExit("an Ozone without this tool's socket is open: close it first")
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.Popen([str(segger.ozone_exe()), str(PROJECT), "-port", str(PORT),
                      "-logfile", str(OUT / "ozone.log"), "-loginterval", "0"])
    for _ in range(120):
        if segger.port_open(PORT):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("Ozone did not open its socket")
    time.sleep(4.0)                           # the project loads the ELF (about 3 s)
    print(f"Ozone started with {PROJECT.name}; log in {OUT / 'ozone.log'}")


def attach(a) -> None:
    elf = board_elf()
    if elf is None and not a.any_build:
        raise SystemExit("the board runs a build in neither build/ nor build-sysview/: Ozone's "
                         "symbols would be wrong (--any-build to attach anyway)")
    c = Console()
    c.run("Debug.Stop();", 5.0)               # no session yet: harmless
    if elf is not None:                       # the symbols of the build that runs
        c.run(f'File.Open("{elf.as_posix()}");', 30.0, r"File.Open: completed|rror")
        print(f"symbols from {elf.relative_to(ROOT)}")
    t0 = time.time()
    text = c.run("Debug.Start();", 20.0, r"Startup complete \(PC=0x[0-9A-Fa-f]+\)|rror")
    m = re.search(r"Startup complete \(PC=(0x[0-9A-Fa-f]+)\)", text)
    if not m:
        print(text.strip()[-600:])
        raise SystemExit("attach did not complete")
    for _ in range(40):                       # the project's OnStartupComplete lets it run
        if c.halted() == 0:
            break
        time.sleep(0.25)
    else:
        c.run("Debug.Continue();", 5.0)
        print("the program was still halted: resumed it")
    print(f"attached (halted at {m.group(1)}), running again {time.time() - t0:.1f} s after the request")
    c.close()


def detach(_a) -> None:
    c = Console()
    print(c.run("Debug.Stop();", 10.0).strip()[-200:])
    c.close()


def cmd(a) -> None:
    c = Console()
    for x in a.commands:
        print(c.run(x, a.timeout).strip())
    c.close()


def read(a) -> None:
    c = Console()
    base = int(a.addr, 0)
    for i in range(a.n):
        addr = base + 4 * i
        text = c.run(f"Target.ReadU32(0x{addr:08X});", 5.0, r"data is|rror")
        m = re.search(r"data is (0x[0-9A-Fa-f]+)", text)   # "returns" is the byte count
        print(f"0x{addr:08X}: " + (f"0x{int(m.group(1), 16):08X}" if m else "? " + text.strip()[-120:]))
    c.close()


def threads(_a) -> None:
    c = Console()
    if c.halted() != 1:
        c.close()
        raise SystemExit("the program runs: Ozone lists threads only while it is halted (a "
                         "breakpoint). For a running board: ctl.py stacks, or sysview.py record")
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "threads.csv"
    c.run('Window.Show("Zephyr");', 5.0)      # Ozone's RTOS window is called "Zephyr" here
    for _ in range(10):                       # the plugin fills it a moment after a halt
        path.unlink(missing_ok=True)
        c.run(f'Window.Export("Zephyr", "{path.as_posix()}");', 10.0)
        if path.exists() and len(path.read_text(errors="replace").splitlines()) > 2:
            print(path.read_text(errors="replace"))
            break
        time.sleep(0.3)
    else:
        print("Ozone exported no threads (ozone.py shot shows its window)")
    c.close()


def quit_(_a) -> None:
    c = Console()
    c.run("Debug.Stop();", 10.0)
    c.s.sendall(b"File.Exit();\n")
    c.close()
    print("Ozone closed")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="op", required=True)
    sub.add_parser("start").set_defaults(fn=start)
    p = sub.add_parser("attach")
    p.add_argument("--any-build", action="store_true", help="attach although no ELF here matches the board")
    p.set_defaults(fn=attach)
    sub.add_parser("detach").set_defaults(fn=detach)
    p = sub.add_parser("cmd")
    p.add_argument("commands", nargs="+")
    p.add_argument("--timeout", type=float, default=10.0)
    p.set_defaults(fn=cmd)
    p = sub.add_parser("read")
    p.add_argument("addr")
    p.add_argument("n", type=int, nargs="?", default=1)
    p.set_defaults(fn=read)
    sub.add_parser("threads").set_defaults(fn=threads)
    p = sub.add_parser("shot")
    p.add_argument("file", nargs="?", default=str(OUT / "window.png"))
    p.set_defaults(fn=lambda a: print(segger.window_shot("Ozone", Path(a.file))))
    sub.add_parser("quit").set_defaults(fn=quit_)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()

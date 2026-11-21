#!/usr/bin/env python3
"""Build, flash and run helper for the STM32N6570-DK demo.

The board stays in development boot (BOOT1 = H, BOOT0 = L) for the whole
development loop, so nobody has to touch the switches:

  flash  write MCUboot and/or the app to XSPI NOR over SWD (ST external loader)
  run    power-cycle the target through the ST-LINK, load MCUboot into RAM,
         start it; MCUboot then chain-loads the app from XSPI flash
  go     build + flash + run

Why the power cycle: ST's external loader leaves the NOR flash in octal DTR
mode. A pin reset does not clear that, and the Zephyr XSPI driver then stalls
on its first SPI-mode command ("XSPI reset enable (SPI/STR) failed"). Only
removing power returns the flash to SPI mode.

In flash boot (BOOT1 = L) none of this is needed: the boot ROM loads MCUboot
from flash on every power-up.
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CUBE_BIN = Path(r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin")
CLI = CUBE_BIN / "STM32_Programmer_CLI.exe"
LOADER = CUBE_BIN / "ExternalLoader" / "MX66UW1G45G_STM32N6570-DK.stldr"
WEST = ROOT / ".venv" / "Scripts" / "west.exe"

BOARD = "stm32n6570_dk"
BUILD = ROOT / "build"
FLASH_BASE = 0x70000000
MCUBOOT_RAM_ADDR = 0x34180400
DEFAULT_PORT = os.environ.get("N6_PORT", "COM18")

# No "ap=" option: with ap=1 this probe/chip revision refuses to connect.
CONNECT = ["-c", "port=swd", "mode=HOTPLUG"]


def cli(args: list[str], quiet: bool = True, check: bool = True) -> str:
    """Run STM32_Programmer_CLI and return its (progress-bar-free) output."""
    proc = subprocess.run([str(CLI), *args], capture_output=True, text=True, errors="replace")
    out = "\n".join(
        ln for ln in (proc.stdout + proc.stderr).splitlines()
        if ln.strip() and not re.search(r"[\u2588\u2591\ufffd]|^\s*\d+%", ln)
    )
    if not quiet or proc.returncode != 0:
        print(out)
    if check and proc.returncode != 0:
        sys.exit(f"[n6] STM32_Programmer_CLI failed ({proc.returncode})")
    return out


def app_dir(build: Path) -> Path:
    """The application image directory inside a sysbuild tree."""
    for d in sorted(build.iterdir()):
        if d.is_dir() and d.name not in ("mcuboot", "_sysbuild") and (d / "zephyr" / "zephyr.signed.bin").exists():
            return d
    sys.exit(f"[n6] no application image found under {build}")


def slot0_offset(build: Path) -> int:
    """Read the slot0 partition offset out of the generated devicetree."""
    dts = (app_dir(build) / "zephyr" / "zephyr.dts").read_text(errors="replace")
    m = re.search(r"slot0_partition:\s*partition@([0-9a-fA-F]+)", dts)
    if not m:
        sys.exit("[n6] slot0_partition not found in zephyr.dts")
    return int(m.group(1), 16)


def _symbols(elf: Path) -> list[tuple[int, int, str]] | None:
    """(address, size, name) of the sized symbols of ELF, by address, via the Zephyr SDK's nm."""
    roots = [os.environ.get("ZEPHYR_SDK_INSTALL_DIR", "")] + glob.glob(str(Path.home() / "zephyr-sdk-*"))
    nm = next((p for r in roots if r for p in [Path(r) / "gnu" / "arm-zephyr-eabi" / "bin" / "arm-zephyr-eabi-nm.exe"]
               if p.exists()), None)
    if nm is None:
        return None
    out = subprocess.run([str(nm), "-S", "-n", str(elf)], capture_output=True, text=True).stdout
    return [(int(f[0], 16), int(f[1], 16), f[3]) for f in (ln.split() for ln in out.splitlines()) if len(f) == 4]


def check_isr_args(build: Path) -> None:
    """Every interrupt handler and argument in isr_tables.c must still be what it was.

    Zephyr generates isr_tables.c from the first link, with literal addresses,
    and the final link can move things: CONFIG_SYMTAB fills in a section before
    rodata only in the final link, and an argument that pointed into rodata then
    pointed at other data (the board hung at boot in the EXTI3 interrupt,
    2026-11-20; app/CMakeLists.txt). A build where that happens fails here.
    """
    z = app_dir(build) / "zephyr"
    pre = sorted(z.glob("zephyr_pre*.elf"))
    tables = z / "isr_tables.c"
    if not pre or not tables.exists():
        return
    first, final = _symbols(pre[-1]), _symbols(z / "zephyr.elf")
    if first is None or final is None:
        print("[n6] warning: no Zephyr SDK nm: interrupt arguments not checked")
        return
    where: dict[str, set[int]] = {}
    for addr, _, name in final:
        where.setdefault(name, set()).add(addr)  # a static's name can occur more than once
    bad = []
    for arg in sorted({int(x, 16) for x in re.findall(r"\{\(const void \*\)(0x[0-9a-f]+), \(ISR\)(?:0x[0-9a-f]+)\}",
                                                       tables.read_text())} - {0}):
        hit = next(((addr, name) for addr, size, name in first if addr <= arg < addr + size), None)
        if hit and hit[0] not in where.get(hit[1], {hit[0]}):
            now = ", ".join(f"0x{x:08x}" for x in sorted(where[hit[1]]))
            bad.append(f"0x{arg:08x} ({hit[1]}+{arg - hit[0]}, in the final image at {now})")
    if bad:
        sys.exit("[n6] interrupt arguments moved between the first and the final link (isr_tables.c "
                 "is wrong; see app/CMakeLists.txt, CONFIG_SYMTAB):\n  " + "\n  ".join(bad))


def cmd_build(a: argparse.Namespace) -> None:
    env = dict(os.environ)
    env["PATH"] = env["PATH"] + os.pathsep + str(CUBE_BIN)  # STM32_SigningTool_CLI
    cmd = [str(WEST), "build", "--sysbuild", "-b", BOARD, "-d", str(a.build), str(a.app)]
    if a.pristine:
        cmd[2:2] = ["-p", "always"]
    if a.cmake:
        cmd += ["--", *a.cmake]
    t0 = time.time()
    proc = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, errors="replace")
    text = proc.stdout + proc.stderr
    if proc.returncode != 0:
        # Show the tail that contains the first compiler/linker error.
        lines = text.splitlines()
        # Compiler diagnostics first (deduplicated), then the first failure block.
        diags = [ln for ln in lines if re.search(r": (fatal )?error:|undefined reference|: warning: implicit", ln)]
        if diags:
            print("\n".join(dict.fromkeys(diags[:40])))
        else:
            hits = [i for i, ln in enumerate(lines) if re.search(r"FAILED|Error", ln)]
            start = max(0, (hits[0] if hits else len(lines)) - 6)
            print("\n".join(lines[start:start + 40]))
        sys.exit(f"[n6] build failed ({proc.returncode})")
    for ln in text.splitlines():
        if re.search(r"^\s+(FLASH|RAM|PSRAM|AXISRAM\w*|\w*_REGION):|warning:", ln):
            print(ln)
    check_isr_args(a.build)
    print(f"[n6] build ok in {time.time() - t0:.0f} s")


def cmd_flash(a: argparse.Namespace) -> None:
    args = [*CONNECT, "-el", str(LOADER)]
    items = []
    if not a.app_only:
        items.append((a.build / "mcuboot" / "zephyr" / "zephyr.signed.bin", FLASH_BASE))
    if not a.boot_only:
        items.append((app_dir(a.build) / "zephyr" / "zephyr.signed.bin", FLASH_BASE + slot0_offset(a.build)))
    for spec in a.file or []:
        path, addr = spec.rsplit("@", 1)
        items.append((Path(path), int(addr, 0)))
    for path, addr in items:
        if not path.exists():
            sys.exit(f"[n6] missing {path}")
        args += ["-d", str(path), hex(addr)]
        print(f"[n6] flash {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path} "
              f"({path.stat().st_size / 1024:.0f} KB) @ {addr:#x}")
    t0 = time.time()
    # The loader only works against the boot ROM's idle state. If firmware is
    # running (XSPI memory-mapped) the sector erase fails, so start clean.
    power_cycle()
    out = cli(args)
    if out.count("File download complete") != len(items):
        print(out)
        sys.exit("[n6] flash did not complete")
    print(f"[n6] flash ok in {time.time() - t0:.1f} s")


def power_cycle() -> None:
    cli(["-c", "port=swd", "mode=UR", "--power", "off", "index=0", "--power", "on", "index=0"], check=False)
    time.sleep(1.0)


def cmd_run(a: argparse.Namespace) -> None:
    import serial  # pyserial, project venv

    elf = a.build / "mcuboot" / "zephyr" / "zephyr.elf"
    if not elf.exists():
        sys.exit(f"[n6] missing {elf}")
    ser = serial.Serial(a.port, 115200, timeout=0.1)
    try:
        if not a.no_power_cycle:
            power_cycle()
        ser.reset_input_buffer()
        out = cli([*CONNECT, "-halt", "-d", str(elf), "-s", hex(MCUBOOT_RAM_ADDR)])
        if "Start operation achieved successfully" not in out:
            print(out)
            sys.exit("[n6] could not start MCUboot from RAM")
        print(f"[n6] MCUboot started from RAM, console for {a.secs:g} s:")
        console(ser, a.secs, a.until, a.log)
    finally:
        ser.close()


def console(ser, secs: float, until: str | None, log: Path | None) -> None:
    deadline = time.time() + secs
    buf = b""
    sink = open(log, "wb") if log else None
    try:
        while time.time() < deadline:
            chunk = ser.read(4096)
            if not chunk:
                continue
            if sink:
                sink.write(chunk)
                sink.flush()  # the log has to survive this process being killed
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for ln in lines:
                text = ln.decode("utf-8", "replace").rstrip()
                print(text)
                if until and re.search(until, text):
                    return
        if buf:
            print(buf.decode("utf-8", "replace").rstrip())
    finally:
        if sink:
            sink.close()


def cmd_console(a: argparse.Namespace) -> None:
    import serial

    with serial.Serial(a.port, 115200, timeout=0.1) as ser:
        console(ser, a.secs, a.until, a.log)


def cmd_go(a: argparse.Namespace) -> None:
    cmd_build(a)
    cmd_flash(a)
    cmd_run(a)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-d", "--build", type=Path, default=BUILD, help="build directory (default: build)")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_build(sp):
        sp.add_argument("--app", type=Path, default=ROOT / "app")
        sp.add_argument("-p", "--pristine", action="store_true")
        sp.add_argument("cmake", nargs="*", help="extra CMake args, e.g. -DCONFIG_FOO=y")

    def add_flash(sp):
        sp.add_argument("--app-only", action="store_true", help="skip MCUboot")
        sp.add_argument("--boot-only", action="store_true", help="skip the application")
        sp.add_argument("--file", action="append", metavar="PATH@ADDR", help="extra binary to write")

    def add_console(sp, secs):
        sp.add_argument("--port", default=DEFAULT_PORT)
        sp.add_argument("--secs", type=float, default=secs)
        sp.add_argument("--until", help="stop when a console line matches this regex")
        sp.add_argument("--log", type=Path, help="also write raw console bytes here")

    sp = sub.add_parser("build"); add_build(sp); sp.set_defaults(fn=cmd_build)
    sp = sub.add_parser("flash"); add_flash(sp); sp.set_defaults(fn=cmd_flash)
    sp = sub.add_parser("run"); add_console(sp, 8)
    sp.add_argument("--no-power-cycle", action="store_true"); sp.set_defaults(fn=cmd_run)
    sp = sub.add_parser("console"); add_console(sp, 10); sp.set_defaults(fn=cmd_console)
    sp = sub.add_parser("go"); add_build(sp); add_flash(sp); add_console(sp, 8)
    sp.add_argument("--no-power-cycle", action="store_true"); sp.set_defaults(fn=cmd_go)

    a = p.parse_args()
    a.build = a.build if a.build.is_absolute() else ROOT / a.build
    a.fn(a)


if __name__ == "__main__":
    main()

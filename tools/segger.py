"""Shared helpers for the SEGGER tools on Windows (tools/sysview.py, tools/ozone.py).

Nothing here touches the board: the installed programs, the probe's serial
number (asked of the probe each time, never stored: it stays out of the
repository), the titles of a process's windows, a picture of one
application's window (tools/winshot.ps1), the build stamp the board
announces, and symbols from an ELF.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import glob
import os
import re
import socket
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEGGER = Path(os.environ.get("SEGGER_DIR", r"C:\Program Files\SEGGER"))
DEFAULT_HOST = os.environ.get("N6_HOST", "192.168.254.86")


def _newest(pattern: str) -> Path:
    hits = sorted(glob.glob(str(SEGGER / pattern)), key=os.path.getmtime)
    if not hits:
        raise SystemExit(f"not installed: {SEGGER / pattern}")
    return Path(hits[-1])


def jlink_exe() -> Path:
    return _newest("JLink*/JLink.exe")


def ozone_exe() -> Path:
    return _newest("Ozone*/Ozone.exe")


def sysview_exe() -> Path:
    return _newest("SystemView*/SystemView.exe")


def probe_serial() -> str:
    """The J-Link's serial number: N6_JLINK_SN, or asked of the one probe on USB."""
    sn = os.environ.get("N6_JLINK_SN")
    if sn:
        return sn
    fd, path = tempfile.mkstemp(suffix=".jlink")
    with os.fdopen(fd, "w") as f:
        f.write("ShowEmuList\nexit\n")
    try:
        out = subprocess.run([str(jlink_exe()), "-NoGui", "1", "-CommandFile", path],
                             capture_output=True, text=True, timeout=60).stdout
    finally:
        os.unlink(path)
    sns = sorted(set(re.findall(r"Serial number:\s*(\d+)", out)))
    if len(sns) != 1:
        raise SystemExit(f"{len(sns)} J-Links found on USB; set N6_JLINK_SN to the one to use")
    return sns[0]


def port_open(port: int) -> bool:
    with socket.socket() as s:
        s.settimeout(0.3)
        return s.connect_ex(("127.0.0.1", port)) == 0


_user32 = ctypes.windll.user32 if os.name == "nt" else None


def window_titles(pid: int) -> list[str]:
    """Titles of the visible top-level windows of process PID."""
    titles: list[str] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def each(hwnd, _lp):
        owner = wt.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _user32.IsWindowVisible(hwnd):
            n = _user32.GetWindowTextLengthW(hwnd)
            if n:
                buf = ctypes.create_unicode_buffer(n + 1)
                _user32.GetWindowTextW(hwnd, buf, n + 1)
                titles.append(buf.value)
        return True

    _user32.EnumWindows(each, 0)
    return titles


def pids(image: str) -> list[int]:
    """Process ids of a running program, e.g. "SystemView.exe"."""
    out = subprocess.run(["tasklist", "/fi", f"imagename eq {image}", "/fo", "csv", "/nh"],
                         capture_output=True, text=True).stdout
    return [int(m.group(1)) for m in re.finditer(r'^"[^"]+","(\d+)"', out, re.M)]


def window_shot(process: str, out: Path) -> str:
    """A PNG of one application's main window (not the desktop)."""
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        str(ROOT / "tools" / "winshot.ps1"), "-Process", process, "-Out", str(out)],
                       capture_output=True, text=True, timeout=60)
    return (r.stdout + r.stderr).strip()


def board_build(host: str = DEFAULT_HOST, timeout: float = 2.0) -> str | None:
    """The build stamp the board announces (fw=..., hostlink.c's __DATE__ __TIME__)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(b"N6HQ", (host, 5557))
        try:
            data, _ = s.recvfrom(512)
        except socket.timeout:
            return None
    m = re.search(r"fw=(.+)$", data.decode(errors="replace").strip())
    return m.group(1) if m else None


def elf_has_build(elf: Path, stamp: str) -> bool:
    """Whether STAMP ("Nov 20 2026 20:12:34") was compiled into ELF."""
    blob = elf.read_bytes()
    parts = stamp.rsplit(" ", 1)  # __DATE__ and __TIME__ are separate strings
    return all(p.encode() + b"\0" in blob for p in parts)


def _sdk_tool(name: str) -> Path | None:
    roots = [os.environ.get("ZEPHYR_SDK_INSTALL_DIR", "")] + glob.glob(str(Path.home() / "zephyr-sdk-*"))
    for r in roots:
        if r:
            p = Path(r) / "gnu" / "arm-zephyr-eabi" / "bin" / f"arm-zephyr-eabi-{name}.exe"
            if p.exists():
                return p
    return None


def elf_symbols(elf: Path) -> dict[int, str]:
    """Data and code symbols of ELF by address (static ones too), via the SDK's nm."""
    nm = _sdk_tool("nm")
    if nm is None or not elf.exists():
        return {}
    out = subprocess.run([str(nm), "-S", str(elf)], capture_output=True, text=True).stdout
    syms: dict[int, str] = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 3 and len(f[-2]) == 1 and f[-2].lower() in "tdbr":
            try:
                addr = int(f[0], 16)
            except ValueError:
                continue
            if len(f) == 4:               # has a size: a real object, not a linker marker
                syms[addr] = f[-1]
            else:
                syms.setdefault(addr, f[-1])
    return syms

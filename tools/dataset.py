#!/usr/bin/env python3
"""Explorer's data set: move it from PSRAM into the flash.

The firmware uses the data set (a million samples on four channels and their
min/max pyramid, 14 MB) in place from the flash at 0x77000000 when a valid
copy is there. When there is none it generates the data into PSRAM at boot,
as it always did. This tool reads that generated block out of the running
board, and writes it to the flash; from the next start the board uses the
copy and the 14 MB of PSRAM stay free.

  dataset.py info                    where the running board has its data from
  dataset.py pull -o dataset.bin     read the block out of the board (about two minutes)
  dataset.py flash dataset.bin       write it to the flash over SWD (stops the board; n6.cmd run afterwards)
  dataset.py erase                   blank the header: the board generates into PSRAM again

Usage: dataset.py [--host IP] info | pull -o FILE | flash FILE | erase
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
import tempfile
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctl import DEFAULT_HOST, send  # noqa: E402

FLASH_ADDR = 0x77000000
MAGIC, VERSION = 0x5344364E, 1  # "N6DS"
N, CH, LEVELS = 1 << 20, 4, 5


def info(host: str) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=(\S+)", send(host, ["ds"])[0]))


def cmd_info(a: argparse.Namespace) -> None:
    i = info(a.host)
    print(f"ready {i['ready']}, source {i['source']}, at {i['addr']}, {int(i['bytes']) / 1e6:.1f} MB, "
          f"checksum {i['crc']}, {'checked' if i['source'] == 'flash' else 'built'} in {i['ms']} ms")


def cmd_pull(a: argparse.Namespace) -> None:
    i = info(a.host)
    if i["ready"] != "1":
        sys.exit("the board is still preparing its data set")
    addr, size, crc = int(i["addr"], 16), int(i["bytes"]), int(i["crc"], 16)
    data = bytearray()
    t0 = time.time()
    for at in range(0, size, 256):  # 64 words a command
        n = min(256, size - at)
        hexs = send(a.host, [f"dump 0x{addr + at:x} {(n + 3) // 4}"])[0]
        data += b"".join(struct.pack("<I", int(hexs[k:k + 8], 16)) for k in range(0, len(hexs), 8))[:n]
        if at % (1 << 20) == 0:
            print(f"\r{at / 1e6:5.1f} of {size / 1e6:.1f} MB", end="", flush=True)
    print(f"\r{size / 1e6:.1f} MB in {time.time() - t0:.0f} s          ")
    got = zlib.crc32(bytes(data))
    if got != crc:
        sys.exit(f"checksum {got:08x}, the board says {crc:08x}: not written")
    header = struct.pack("<8I", MAGIC, VERSION, N, CH, LEVELS, size, crc, 0)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(header + bytes(data))
    print(f"{a.output}: checksum {crc:08x} verified (the data came from: {i['source']})")


def write_flash(path: Path) -> None:
    import n6

    n6.power_cycle()
    out = n6.cli([*n6.CONNECT, "-el", str(n6.LOADER), "-d", str(path), hex(FLASH_ADDR)])
    if "File download complete" not in out:
        print(out)
        sys.exit("[dataset] flash failed")
    print("[dataset] done; start the board with: n6.cmd run")


def cmd_flash(a: argparse.Namespace) -> None:
    blob = a.image.read_bytes()
    magic, version, n, ch, levels, size, crc, _ = struct.unpack_from("<8I", blob)
    if (magic, version, n, ch, levels) != (MAGIC, VERSION, N, CH, LEVELS) or len(blob) != 32 + size:
        sys.exit(f"{a.image} is not a data set image for this firmware")
    if zlib.crc32(blob[32:]) != crc:
        sys.exit(f"{a.image}: checksum does not match its header")
    print(f"[dataset] writing {len(blob) / 1e6:.1f} MB to {FLASH_ADDR:#x} (slow over SWD, about a minute and a half)")
    write_flash(a.image.resolve())


def cmd_erase(a: argparse.Namespace) -> None:
    with tempfile.TemporaryDirectory() as d:
        blank = Path(d) / "blank.bin"
        blank.write_bytes(b"\xff" * 4096)
        print(f"[dataset] blanking the header at {FLASH_ADDR:#x}")
        write_flash(blank)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info").set_defaults(fn=cmd_info)
    sp = sub.add_parser("pull")
    sp.add_argument("-o", "--output", type=Path, required=True)
    sp.set_defaults(fn=cmd_pull)
    sp = sub.add_parser("flash")
    sp.add_argument("image", type=Path)
    sp.set_defaults(fn=cmd_flash)
    sub.add_parser("erase").set_defaults(fn=cmd_erase)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()

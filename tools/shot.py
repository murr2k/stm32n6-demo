#!/usr/bin/env python3
"""Pull the board's front frame buffer over TCP and save it as a PNG.

Usage: shot.py [--host n6demo | IP] [-o captures/shot.png]
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def grab(host: str, port: int = 5555, timeout: float = 10.0) -> Image.Image:
    with socket.create_connection((host, port), timeout=timeout) as s:
        buf = bytearray()
        while True:
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
    magic, w, h, bpp = struct.unpack_from("<4sIII", buf, 0)
    if magic != b"N6FB":
        raise SystemExit(f"bad magic {magic!r}")
    need = w * h * bpp // 8
    data = bytes(buf[16:])
    if len(data) != need:
        raise SystemExit(f"short frame: {len(data)} of {need} bytes")
    if bpp == 32:  # little-endian ARGB8888 -> bytes B, G, R, A
        px = np.frombuffer(data, np.uint8).reshape(h, w, 4)
        return Image.fromarray(px[:, :, [2, 1, 0]].copy(), "RGB")
    if bpp == 16:
        v = np.frombuffer(data, "<u2").reshape(h, w)
        r = ((v >> 11) & 31) * 255 // 31
        g = ((v >> 5) & 63) * 255 // 63
        b = (v & 31) * 255 // 31
        return Image.fromarray(np.dstack([r, g, b]).astype(np.uint8), "RGB")
    raise SystemExit(f"unsupported bpp {bpp}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="n6demo")
    ap.add_argument("-o", "--out", type=Path, default=ROOT / "captures" / "shot.png")
    a = ap.parse_args()
    t0 = time.time()
    img = grab(a.host)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    img.save(a.out)
    print(f"{a.out} {img.size[0]}x{img.size[1]} in {time.time() - t0:.2f} s", file=sys.stderr)


if __name__ == "__main__":
    main()

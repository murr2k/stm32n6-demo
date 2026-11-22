#!/usr/bin/env python3
"""A still from the board's camera, for a camera pointed at the board's own panel.

  camsnap.py [--uncap] [-o captures/camsnap/NAME.png] [--step 2] [--frames 45]

The board starts the camera if no screen has it (the Camera screen does not
open), waits FRAMES frames for the exposure to settle, keeps every STEP-th
pixel of the newest frame and stops the camera again ("camsnap" on the
control port); this reads the picture with the control port's dump command,
writes a PNG and prints its brightness: the mean and the 99th percentile of
the luma (0..255), and the share of pixels brighter than 40. Those figures
cannot tell a black panel from a lit one: the picture takes in the board and
the table around the panel, and the camera's exposure control lifts a dark
scene. Look at the PNG (a black panel with its backlight on is an even grey).

The board refuses while its lens cap is on, as it is after every reset:
"ctl.py 'camsnap on'" lifts it until a reset or "camsnap off"; --uncap lifts
it for this one still and puts it back on afterwards.

Camera pictures may show people: they stay in captures/ (ignored), never in
the repository.
"""
from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctl import DEFAULT_HOST, send  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "captures" / "camsnap"


def kv(line: str) -> dict[str, str]:
    return dict(p.split("=", 1) for p in line.split() if "=" in p)


def read(host: str, addr: int, size: int) -> bytes:
    cmds, at = [], addr
    while at < addr + size:
        words = min(64, (addr + size - at + 3) // 4)
        cmds.append(f"dump {at:#x} {words}")
        at += words * 4
    data = bytearray()
    for r in send(host, cmds):
        h = r.strip()
        data += b"".join(struct.pack("<I", int(h[i:i + 8], 16)) for i in range(0, len(h), 8))
    return bytes(data[:size])


def snap(host: str, step: int, frames: int, out: Path, uncap: bool = False) -> dict[str, float]:
    import numpy as np
    from PIL import Image

    cmds = ["camsnap on"] if uncap else []
    cmds.append(f"camsnap {step} {frames}")
    if uncap:
        cmds.append("camsnap off")
    r = send(host, cmds, timeout=20.0)[1 if uncap else 0].strip()
    if r.startswith("err"):
        raise SystemExit(r)
    k = kv(r)
    w, h, addr = int(k["w"]), int(k["h"]), int(k["addr"], 16)
    px = np.frombuffer(read(host, addr, w * h * 2), dtype="<u2").reshape(h, w)
    rgb = np.empty((h, w, 3), np.uint8)
    rgb[..., 0] = ((px >> 11) & 0x1F) * 255 // 31
    rgb[..., 1] = ((px >> 5) & 0x3F) * 255 // 63
    rgb[..., 2] = (px & 0x1F) * 255 // 31
    out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgb).save(out)
    y = rgb @ np.array([0.299, 0.587, 0.114])
    return {"mean": float(y.mean()), "p99": float(np.percentile(y, 99)),
            "bright": float((y > 40).mean() * 100)}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", help="PNG (default captures/camsnap/<time>.png)")
    ap.add_argument("--step", type=int, default=2, help="keep every STEP-th pixel (1 to 8)")
    ap.add_argument("--frames", type=int, default=45, help="frames for the exposure to settle")
    ap.add_argument("--uncap", action="store_true", help="lift the lens cap for this still, then put it back")
    ap.add_argument("--host", default=DEFAULT_HOST)
    a = ap.parse_args()
    out = Path(a.out) if a.out else OUT / (time.strftime("%Y%m%d-%H%M%S") + ".png")
    t0 = time.time()
    s = snap(a.host, a.step, a.frames, out, a.uncap)
    print(f"{out}  luma mean {s['mean']:.1f}  p99 {s['p99']:.0f}  bright {s['bright']:.1f} %"
          f"  ({time.time() - t0:.1f} s)")


if __name__ == "__main__":
    main()

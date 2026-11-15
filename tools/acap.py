#!/usr/bin/env python3
"""Record what the board hands to its DAC, as a WAV file (48 kHz stereo).

The firmware copies the next seconds of its output into PSRAM ("acap" on the
control port); this reads them back with "dump". Control commands given after
the options run while the recording is going, e.g. an injected drag on the
Cinema slider:

  acap.py --secs 2.5 -o scrub.wav "swipe 120 444 600 444 1500"

With --clicks it also counts discontinuities: steps from one sample to the
next that are far larger than the steps in the 10 ms on either side. Plain
playback of music gives none; a cut or a jump without a fade gives one each.

Usage: acap.py [--host IP] [--secs 2] -o out.wav [--clicks] [command ...]
"""
from __future__ import annotations

import argparse
import math
import re
import struct
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctl import DEFAULT_HOST, send  # noqa: E402

RATE = 48000


def capture(host: str, secs: float, commands: list[str]) -> list[tuple[int, int]]:
    """Stereo sample pairs of the next secs (3 at most), with commands run meanwhile."""
    r = send(host, [f"acap {int(secs * 1000)}"])[0]
    if r.startswith("err"):
        sys.exit(f"board: {r}")
    t0 = time.time()
    send(host, commands)
    while True:
        g = dict(re.findall(r"(\w+)=(\w+)", send(host, ["acap"])[0]))
        if int(g["filled"]) >= int(g["frames"]):
            break
        if time.time() - t0 > secs + 5:
            sys.exit(f"capture did not finish: {g}")
        time.sleep(0.1)
    addr, frames = int(g["addr"], 16), int(g["frames"])
    out: list[tuple[int, int]] = []
    for at in range(0, frames, 64):  # one word is one stereo frame
        n = min(64, frames - at)
        hexs = send(host, [f"dump 0x{addr + at * 4:x} {n}"])[0]
        for i in range(n):
            w = int(hexs[i * 8:i * 8 + 8], 16)
            out.append(struct.unpack("<hh", struct.pack("<I", w)))
    return out


def clicks(pcm: list[tuple[int, int]], k: float = 12.0, floor: int = 400, half: int = 480) -> list[int]:
    """Sample positions of discontinuities in the left channel: a step above
    floor that is more than k times the RMS step of the half samples on either
    side (itself and its two neighbours left out). Hits within 1 ms count once."""
    x = [s[0] for s in pcm]
    d = [x[i + 1] - x[i] for i in range(len(x) - 1)]
    sq = [0]
    for v in d:
        sq.append(sq[-1] + v * v)
    n, events = len(d), []
    for i, v in enumerate(d):
        if abs(v) <= floor:
            continue
        lo, hi, a, b = max(i - half, 0), min(i + half + 1, n), max(i - 1, 0), min(i + 2, n)
        rms = math.sqrt(max(sq[hi] - sq[lo] - (sq[b] - sq[a]), 0) / max((hi - lo) - (b - a), 1))
        if abs(v) > k * max(rms, 20.0) and (not events or i - events[-1] > 48):
            events.append(i)
    return events


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--secs", type=float, default=2.0)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--clicks", action="store_true", help="count discontinuities")
    ap.add_argument("commands", nargs="*")
    a = ap.parse_args()

    pcm = capture(a.host, a.secs, a.commands)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(a.out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<hh", l, r) for l, r in pcm))
    print(f"{a.out}: {len(pcm) / RATE:.2f} s")
    if a.clicks:
        ev = clicks(pcm)
        print(f"discontinuities: {len(ev)}" + (" at " + ", ".join(f"{p / RATE:.3f} s" for p in ev[:8]) if ev else ""))


if __name__ == "__main__":
    main()

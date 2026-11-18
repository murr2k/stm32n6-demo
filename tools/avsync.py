#!/usr/bin/env python3
"""Sound and picture in step, measured by the board itself: play the A/V
sync check clip in Cinema while the board records its screen and sound
(tools/record.py), then find each flash in the pictures and each beep in the
sound and print how far apart they are. Both are measured in DAC frames
(each picture carries the frame at the DAC as it appeared), so the result is
what the panel and the headphones did, not what a PC player does.

Usage: avsync.py [--host IP] [--fps 30|60] [--clip 2] [--name NAME]
"""
from __future__ import annotations

import argparse
import csv
import subprocess
import sys
import time
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctl import DEFAULT_HOST, close, send  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--fps", type=int, choices=(30, 60), default=60)
    ap.add_argument("--clip", type=int, default=2, help="the A/V sync check clip's index in Cinema")
    ap.add_argument("--name", default="avsync")
    a = ap.parse_args()
    import av  # PyAV, in the project venv

    send(a.host, ["screen 7", "sleep 1.5", f"clip {a.clip}"])
    close(a.host)
    rec = subprocess.Popen([sys.executable, str(ROOT / "tools" / "record.py"), "--host", a.host, "--secs", "9",
                            "--fps", str(a.fps), "--name", a.name],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.0)
    send(a.host, [f"play {a.clip}"])
    close(a.host)
    out = rec.communicate(timeout=120)[0]
    if rec.returncode:
        sys.exit(out)
    base = ROOT / "captures" / "rec" / a.name
    rows = list(csv.DictReader(open(base.with_suffix(".csv"), newline="")))
    apos = np.array([int(r["audio_pos"]) for r in rows])
    ref = np.array([int(r["refresh"]) for r in rows])
    ok = apos > 0
    b, c0 = np.polyfit(ref[ok], apos[ok], 1)
    at_dac = c0 + b * ref  # the DAC frame each picture appeared at, jitter taken out
    with av.open(str(base.with_suffix(".h264")), format="h264") as c:
        luma = np.array([f.to_ndarray(format="gray").mean() for f in c.decode(video=0)])
    thr = (np.percentile(luma, 5) + np.percentile(luma, 95)) / 2
    flashes = [i for i in range(1, len(luma)) if luma[i - 1] < thr <= luma[i]]
    with wave.open(str(base.with_suffix(".wav"))) as w:
        rate = w.getframerate()
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 2).mean(axis=1) / 32768
    win = 48
    n = len(pcm) // win
    env = np.sqrt((pcm[: n * win].reshape(n, win) ** 2).mean(axis=1))
    et = env.max() * 0.25
    beeps = np.array([i * win for i in range(1, n) if env[i - 1] < et <= env[i]])
    if not flashes or beeps.size == 0:
        sys.exit(f"found {len(flashes)} flashes and {beeps.size} beeps: is the sync clip index right?")
    offs = [(at_dac[i] - beeps[np.argmin(abs(beeps - at_dac[i]))]) * 1000 / rate for i in flashes]
    print(f"{a.fps} fps recording: flash minus beep, ms: " + " ".join(f"{o:+.0f}" for o in offs))
    print(f"mean {np.mean(offs):+.1f} ms over {len(offs)} flashes (a refresh is 16.7 ms; "
          f"positive: the picture after the sound)")


if __name__ == "__main__":
    main()

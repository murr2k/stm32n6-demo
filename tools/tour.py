#!/usr/bin/env python3
"""Unattended review of the running demo.

Visits every screen, lets it run, records frame statistics and a screenshot,
then exercises the multi-touch paths with injected contacts. Output goes to
captures/tour/.

Usage: tour.py [--host IP] [--screens home,deck,...]
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctl import DEFAULT_HOST, send  # noqa: E402
from shot import grab  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "captures" / "tour"
SCREENS = ["home", "deck", "siglab", "explorer", "touchlab", "cinema", "specimen"]


def stats(host: str) -> dict[str, float]:
    line = send(host, ["stats"])[0]
    return {k: float(v) for k, v in re.findall(r"(\w+)=([\d.]+)", line)}


def visit(host: str, index: int, name: str, settle: float, run: float) -> None:
    send(host, [f"screen {index}"])
    time.sleep(settle)
    before = stats(host)
    send(host, ["reset"])
    time.sleep(run)
    s = stats(host)
    grab(host).save(OUT / f"{index}-{name}.png")
    print(f"{name:9s} fps {s['fps']:5.1f}  frame avg {s['render_avg_us'] / 1000:5.2f} ms  "
          f"worst {s['render_worst_us'] / 1000:5.2f} ms  worst period {s['period_worst_us'] / 1000:5.2f} ms  "
          f"dropped {int(s['dropped'] - before['dropped'])}")


def touch_tests(host: str) -> None:
    # Touch Lab: three contacts held, one of them dragged.
    send(host, [f"screen {SCREENS.index('touchlab')}"])
    time.sleep(1.0)
    send(host, ["touch 0 200 200 1", "touch 1 520 300 1", "touch 2 640 140 1"])
    for i in range(40):
        send(host, [f"touch 0 {200 + i * 4} {200 + (i * i) // 20} 1"])
        time.sleep(0.012)
    time.sleep(0.2)
    grab(host).save(OUT / "t1-touchlab-3-contacts.png")
    send(host, ["touch 0 0 0 0", "touch 1 0 0 0", "touch 2 0 0 0"])

    # Explorer: pinch out around x = 400 (zoom in), then a pan with a flick.
    send(host, [f"screen {SCREENS.index('explorer')}"])
    time.sleep(1.0)
    send(host, ["reset"])
    before = stats(host)
    send(host, ["touch 0 380 250 1", "touch 1 420 250 1"])
    for i in range(60):
        send(host, [f"touch 0 {380 - i * 5} 250 1", f"touch 1 {420 + i * 5} 250 1"])
        time.sleep(0.012)
    send(host, ["touch 0 0 0 0", "touch 1 0 0 0"])
    time.sleep(0.3)
    grab(host).save(OUT / "t2-explorer-pinched.png")
    send(host, ["swipe 600 250 200 250 180"])
    time.sleep(1.2)
    send(host, ["tap 400 200"])
    time.sleep(0.4)
    s = stats(host)
    grab(host).save(OUT / "t3-explorer-panned-cursor.png")
    print(f"explorer gestures: frame worst {s['render_worst_us'] / 1000:.2f} ms, "
          f"dropped {int(s['dropped'] - before['dropped'])}")

    # Edge swipe: from the right edge inward goes to the next screen.
    send(host, ["screen 1", "sleep 0.5", "swipe 795 240 560 240 160"])
    time.sleep(0.6)
    grab(host).save(OUT / "t4-after-edge-swipe.png")
    send(host, ["screen 0"])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--screens", default=",".join(SCREENS))
    ap.add_argument("--no-touch", action="store_true")
    ap.add_argument("--run", type=float, default=5.0, help="seconds measured per screen")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    for name in a.screens.split(","):
        visit(a.host, SCREENS.index(name), name, settle=2.0, run=a.run)
    if not a.no_touch:
        touch_tests(a.host)


if __name__ == "__main__":
    main()

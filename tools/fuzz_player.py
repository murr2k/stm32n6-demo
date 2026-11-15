#!/usr/bin/env python3
"""Random abuse of the Cinema player: scrubs with a wandering finger (some starting
at a screen edge, some with a second contact), play and pause, back,
re-opening clips, all through injected touches.

Stops at the first action after which the board no longer answers and prints
the last actions, so a crash can be replayed with --seed. Run the console in
another terminal (n6.cmd console) to see the fault message.

Usage: fuzz_player.py [--host IP] [--secs 120] [--seed N]
"""
from __future__ import annotations

import argparse
import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctl import DEFAULT_HOST, send  # noqa: E402
from tour import SCREENS  # noqa: E402

BAR_Y = 444            # middle of the scrub bar
BAR_X = (78, 638)
CARDS = (140, 380, 620)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--secs", type=float, default=120.0)
    ap.add_argument("--seed", type=int, default=int(time.time()) & 0xFFFF)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    log: list[str] = []
    print(f"seed {a.seed}")

    def do(name: str, cmds: list[str], gap: float = 0.0) -> None:
        log.append(name)
        for c in cmds:
            send(a.host, [c])
            if gap:
                time.sleep(gap)

    def scrub() -> None:
        x, y = rnd.choice((rnd.randint(60, 660), rnd.randint(60, 660), rnd.randint(0, 27), rnd.randint(772, 799))), BAR_Y + rnd.randint(-10, 10)
        steps = rnd.randint(5, 60)
        wander = rnd.choice((0, 0, 6, 25))  # how far the finger strays off the bar
        second = rnd.random() < 0.25
        pts = []
        for _ in range(steps):
            x = max(0, min(799, x + rnd.randint(-70, 70)))
            y = max(380, min(479, y + rnd.randint(-wander, wander)))
            pts.append(f"touch 0 {x} {y} 1")
            if second and rnd.random() < 0.3:  # a second finger or the palm, somewhere else
                pts.append(f"touch 1 {rnd.randint(0, 799)} {rnd.randint(60, 479)} {rnd.choice((0, 1, 1))}")
        do(f"scrub {steps} steps, wander {wander}, ends at {x},{y}", pts + ["touch 0 0 0 0", "touch 1 0 0 0"], 0.010)

    actions = [
        (8, scrub),
        (2, lambda: do("play/pause", ["tap 40 444"])),
        (1, lambda: do("toggle controls", ["tap 400 240"])),
        (1, lambda: do("back", ["tap 40 26"])),
        (2, lambda: do("open a clip", [f"tap {rnd.choice(CARDS)} 200"])),
        (1, lambda: do("wait", [], 0) or time.sleep(rnd.uniform(0.2, 2.5))),
    ]
    weights = [w for w, _ in actions]
    send(a.host, [f"screen {SCREENS.index('cinema')}"])
    time.sleep(1.0)
    t0, n = time.time(), 0
    try:
        while time.time() - t0 < a.secs:
            rnd.choices(actions, weights)[0][1]()
            n += 1
            time.sleep(rnd.uniform(0.02, 0.4))
            send(a.host, ["video"])  # still alive?
    except OSError as e:
        print(f"board stopped answering after {n} actions, {time.time() - t0:.1f} s ({e}). Last actions:")
        for line in log[-12:]:
            print("  ", line)
        sys.exit(1)
    print(f"{n} actions in {time.time() - t0:.0f} s, still alive: {send(a.host, ['video', 'audio'])}")
    send(a.host, ["tap 40 26", "screen 0"])


if __name__ == "__main__":
    main()

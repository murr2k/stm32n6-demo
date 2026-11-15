#!/usr/bin/env python3
"""Two short companion clips.

  flow   16 s generative study: particles advected through a slowly changing
         field, leaving trails. Fine detail and motion everywhere, so it is
         the hard case for the decoder.
  sync   8 s A/V sync check: a flash and a 1 kHz beep on every second.

Usage: extras.py [flow] [sync]
"""
from __future__ import annotations

import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import skia

sys.path.insert(0, str(Path(__file__).resolve().parent))
import score  # noqa: E402
from kit import *  # noqa: E402,F403

SR = score.SR


def encode(name: str, title: str, q: int) -> None:
    subprocess.run([sys.executable, str(ROOT / "tools" / "n6v.py"), "encode", str(OUT / f"{name}.mp4"), "-o",
                    str(OUT / f"{name}.n6v"), "--title", title, "--q", str(q)], check=True)


# ---- flow ---------------------------------------------------------------------

def flow_audio(total: float) -> np.ndarray:
    mix = np.zeros((int(total * SR) + SR, 2))
    for i, n in enumerate((38, 45, 50, 57, 62, 65)):  # an open D minor 9 voicing
        score.add(mix, score.pad_note(score.hz(n), total - 1.0), 0.0, 0.07, pan=-0.7 + 0.28 * i)
    rng = np.random.default_rng(11)
    for k in range(26):  # sparse high plucks, like drops
        score.add(mix, score.pluck(score.hz(rng.choice([74, 77, 81, 84, 86, 89]))), 1.0 + k * 0.55 + rng.uniform(0, 0.2),
                  0.07, pan=rng.uniform(-0.8, 0.8))
    mix = mix[: int(total * SR)]
    t = np.arange(mix.shape[0]) / SR
    env = np.minimum(t / 1.5, 1.0) * np.minimum((total - t) / 2.5, 1.0)
    return np.tanh(mix * 1.4) * 0.8 * env[:, None]


def make_flow() -> None:
    total, n = 16.0, 1400
    rng = np.random.default_rng(3)
    px = rng.uniform(0, W, n)
    py = rng.uniform(0, H, n)
    group = rng.integers(0, 4, n)
    colors = (AZURE, AMBER, VIOLET, MINT)
    trail = new_surface()
    trail.getCanvas().clear(col(BG0))
    out = new_surface()
    wav = OUT / "flow.wav"
    write_wav(wav, flow_audio(total))
    w = Writer(OUT / "flow.mp4", wav)

    for f in range(int(total * FPS)):
        t = f / FPS
        c = trail.getCanvas()
        # fade the old trails towards the background a little every frame
        c.drawRect(skia.Rect.MakeWH(W, H), paint(BG0, 22))
        # the field: an angle built from a few moving sine waves
        ang = (2.2 * np.sin(px * 0.0085 + 0.31 * t) + 2.0 * np.cos(py * 0.011 - 0.23 * t)
               + 1.3 * np.sin((px + py) * 0.006 + 0.4 * t))
        speed = 2.4 + 0.9 * np.sin(0.5 * t + group)
        nx = px + np.cos(ang) * speed
        ny = py + np.sin(ang) * speed
        for g in range(4):
            path = skia.Path()
            for i in np.nonzero(group == g)[0]:
                path.moveTo(float(px[i]), float(py[i]))
                path.lineTo(float(nx[i]), float(ny[i]))
            c.drawPath(path, paint(colors[g], 150, stroke=1.4, plus=True))
        px, py = nx, ny
        lost = (px < -10) | (px > W + 10) | (py < -10) | (py > H + 10) | (rng.random(n) < 0.004)
        px[lost] = rng.uniform(0, W, int(lost.sum()))
        py[lost] = rng.uniform(0, H, int(lost.sum()))

        oc = out.getCanvas()
        put(oc, trail.makeImageSnapshot())
        chyron(oc, t, total, "Flow", "GENERATIVE STUDY  1400 PARTICLES", start=1.5, hold_out=1.6, accent=VIOLET)
        oc.drawRect(skia.Rect.MakeWH(W, H),
                    paint((0, 0, 0), 255 * max(1 - ramp(t, 0, 1.2), ease_io(ramp(t, total - 1.6, 1.5)))))
        w.write(out)
    w.close()
    encode("flow", "Flow", 5)


# ---- sync ---------------------------------------------------------------------

def make_sync() -> None:
    total = 8.0
    mix = np.zeros((int(total * SR), 2))
    beep = np.sin(2 * np.pi * 1000 * score.tt(0.06)) * np.minimum(score.tt(0.06) / 0.002, 1.0) * 0.5
    for s in range(int(total)):
        score.add(mix, beep, float(s))
    wav = OUT / "sync.wav"
    write_wav(wav, mix)
    out = new_surface()
    w = Writer(OUT / "sync.mp4", wav)
    for f in range(int(total * FPS)):
        t = f / FPS
        frac = t - math.floor(t)
        c = out.getCanvas()
        c.clear(col((255, 255, 255)) if f % FPS < 2 else col(BG0))  # flash for the first two frames of a second
        ink = BG0 if f % FPS < 2 else TEXT
        text(c, "A/V SYNC", 48, 70, font("mono", 16), TEXT3 if f % FPS >= 2 else BG1, 255, spacing=4)
        text(c, f"{int(t)}", W / 2, 300, font("num", 240), ink, 255, "center")
        # a sweep that reaches the right edge exactly on the beep
        c.drawRect(skia.Rect.MakeXYWH(48, 400, 704, 6), paint(LINE))
        c.drawRect(skia.Rect.MakeXYWH(48, 400, 704 * frac, 6), paint(AZURE))
        c.drawCircle(48 + 704 * frac, 403, 9, paint(ink))
        text(c, "FLASH AND 1 KHZ BEEP ON EVERY SECOND", 48, 446, font("mono", 12.5), TEXT2 if f % FPS >= 2 else BG1,
             255, spacing=2.5)
        w.write(out)
    w.close()
    encode("sync", "A/V sync check", 5)


if __name__ == "__main__":
    which = sys.argv[1:] or ["flow", "sync"]
    if "flow" in which:
        make_flow()
    if "sync" in which:
        make_sync()

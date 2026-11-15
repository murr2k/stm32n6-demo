#!/usr/bin/env python3
"""The demo film: 48 seconds of original motion graphics about the demo
itself, in the demo's own design language. Rendered with Skia, scored by
score.py, written as an H.264 master and as an N6V clip for the board.

Usage: n6_film.py [--preview T ...]   (--preview writes PNG stills instead)
"""
from __future__ import annotations

import argparse
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
import skia

sys.path.insert(0, str(Path(__file__).resolve().parent))
import score  # noqa: E402
from kit import *  # noqa: E402,F403

BAR = score.BAR
# scene boundaries, on bars
T = [0.0, 2 * BAR, 5 * BAR, 8 * BAR, 11 * BAR, 14 * BAR, 16 * BAR, 18 * BAR, 20 * BAR]
TOTAL = T[-1]


def hashf(x: float) -> float:
    return math.modf(math.sin(x * 127.1) * 43758.5453)[0]


# ---- scene 0: cold open --------------------------------------------------------

def s_open(c, t, d):
    c.clear(col((3, 5, 8)))
    p = ease_out(t / 1.6)
    amp = 46 * ease_io(ramp(t, 1.5, 2.2)) * (1 + 1.6 * ease_in(ramp(t, d - 0.9, 0.9)))
    pts = []
    for i in range(0, 321):
        x = 80 + i * 2
        if x > 80 + p * 640:
            break
        u = (x - 80) / 640
        win = math.sin(math.pi * u) ** 2
        y = 240 + amp * win * (math.sin(0.045 * x - 5.5 * t) * 0.7 + 0.3 * math.sin(0.11 * x + 3.1 * t))
        pts.append((x, y))
    if len(pts) > 1:
        polyline(c, pts, paint(AZURE, 120, stroke=7, blur=5, plus=True))
        polyline(c, pts, paint((210, 236, 255), 255, stroke=2))
        c.drawCircle(*pts[-1], 3.5, paint((255, 255, 255)))
    a = 255 * ramp(t, 2.3, 0.8) * (1 - ramp(t, d - 0.8, 0.5))
    text(c, "A MICROCONTROLLER, MEASURED", W / 2, 352, font("mono", 15), TEXT2, a, "center", spacing=5)


# ---- scene 1: title ------------------------------------------------------------

def s_title(c, t, d):
    backdrop(c, t, 0.0)
    f = font("head", 250)
    k = ease_out(t / 0.7)
    c.save()
    c.clipRect(skia.Rect.MakeLTRB(0, 0, W, 240))
    c.translate(0, (1 - k) * 210)
    text(c, "N6", 62, 234, f, TEXT)
    c.restore()
    w = 150 * ease_out(ramp(t, 0.5, 0.6))
    c.drawRect(skia.Rect.MakeXYWH(70, 258, w, 5), paint(AZURE))
    a = ramp(t, 0.9, 0.6)
    text(c, "Instrument-grade graphics", 70, 310 + 16 * (1 - ease_out(a)), font("text", 40), TEXT, 255 * a)
    a = ramp(t, 1.5, 0.6)
    text(c, "ON A MICROCONTROLLER", 72, 342, font("mono", 15), TEXT2, 255 * a, spacing=4)
    # a slow scope trace on the right, the film's motif
    pts = [(430 + i * 3, 150 + 44 * math.sin(0.09 * i - 2.4 * t) * math.sin(math.pi * i / 110) ** 2) for i in range(111)]
    polyline(c, pts, paint(AZURE, 90 * ramp(t, 0.6, 1.0), stroke=6, blur=4, plus=True))
    polyline(c, pts, paint(AZURE, 255 * ramp(t, 0.6, 1.0), stroke=2))
    chyron(c, t, d, "STM32N6570-DK", "CORTEX-M55  800 MHZ  HELIUM", start=3.0)


# ---- scene 2: frame rate -------------------------------------------------------

def s_fps(c, t, d):
    backdrop(c, t, 1.7)
    c.save()
    z = 1 + 0.05 * t / d
    c.translate(W / 2, H / 2)
    c.scale(z, z)
    c.translate(-W / 2, -H / 2)
    text(c, "PANEL REFRESH", 62, 92, font("mono", 14), TEXT3, 255 * ramp(t, 0.2, 0.5), spacing=3)
    v = 59.98 * ease_out(t / 1.3)
    w = text(c, f"{v:5.2f}", 54, 250, font("num", 190), TEXT)
    text(c, "Hz", 54 + w + 14, 250, font("head", 54), TEXT2, 255 * ramp(t, 0.6, 0.5))
    # frame-time bars scrolling under the number
    x0, y0, bw = 62, 352, 5.6
    n = 120
    c.drawLine(x0, y0 - 62, x0 + n * bw, y0 - 62, paint(TEXT3, 150, stroke=1))
    text(c, "16.67 MS BUDGET", x0 + n * bw - 2, y0 - 68, font("mono", 11), TEXT3, 255, "right", spacing=1.5)
    shift = t * 60
    for i in range(n):
        idx = int(i + shift)
        hgt = 9 + 7 * hashf(idx) + (16 if idx % 15 == 0 else 0)
        grow = ease_out(ramp(t, 0.4 + i * 0.006, 0.5))
        c.drawRect(skia.Rect.MakeXYWH(x0 + i * bw, y0 - hgt * grow, bw - 2, hgt * grow), paint(AZURE))
    c.drawLine(x0, y0 + 1, x0 + n * bw, y0 + 1, paint(LINE, 255, stroke=1))
    c.restore()
    chyron(c, t, d, "Locked to the panel", "EVERY FRAME MEASURED  ZERO DROPPED", start=2.6)


# ---- scene 3: spectrum ridgelines ---------------------------------------------

def spectrum(u: float, tm: float) -> float:
    tri = abs(((tm / 3.0) % 2.0) - 1.0)
    us = 0.12 + 0.72 * tri
    v = (0.55 * math.exp(-((u - 0.30) / 0.035) ** 2) + 0.30 * math.exp(-((u - 0.50) / 0.020) ** 2)
         + 0.20 * math.exp(-((u - 0.63) / 0.014) ** 2) + 0.62 * math.exp(-((u - us) / 0.028) ** 2))
    return v + 0.035 * (0.5 + 0.5 * math.sin(u * 310 + tm * 23)) * (0.4 + 0.6 * hashf(u * 91 + math.floor(tm * 30)))


def s_spectrum(c, t, d):
    backdrop(c, t, 3.1, grid=False)
    c.save()
    c.translate(-30 * t / d, 0)
    n, m = 30, 150
    for i in range(n - 1, -1, -1):
        z = i / n
        tm = t - z * 2.4
        xl, xr = 90 + z * 150, 760 - z * 150
        base = 418 - z * 270
        hs = 150 * (1 - 0.45 * z)
        pts = [(xl + (xr - xl) * j / m, base - hs * spectrum(j / m, tm)) for j in range(m + 1)]
        fill = skia.Path()
        fill.moveTo(xl, base + 6)
        for q in pts:
            fill.lineTo(*q)
        fill.lineTo(xr, base + 6)
        fill.close()
        c.drawPath(fill, paint(BG0, 236))
        a = 255 * (1 - z) ** 1.3 * ramp(t, 0.1 + z * 0.9, 0.5)
        polyline(c, pts, paint(AZURE, a, stroke=2.2 - 1.2 * z))
        hot = [q for q in pts if base - q[1] > hs * 0.44]
        if len(hot) > 1 and i < n * 0.6:
            polyline(c, hot, paint(AMBER, a, stroke=2.6 - 1.2 * z))
    c.restore()
    text(c, "1024", 60, 110, font("num", 86), TEXT, 255 * ramp(t, 0.3, 0.5))
    text(c, "POINT FFT, EVERY FRAME", 64, 138, font("mono", 13), TEXT2, 255 * ramp(t, 0.6, 0.5), spacing=3)
    chyron(c, t, d, "Signal Lab", "SCOPE  SPECTRUM  WATERFALL  220 MICROSECONDS", start=2.6)


# ---- scene 4: explorer zoom ----------------------------------------------------

SPAN0, SPAN1, XC = 104.86, 0.030, 37.4135


def lane_a(x0, x1):
    xm = 0.5 * (x0 + x1)
    amp = 0.38 + 0.28 * math.sin(0.13 * xm) + 0.16 * math.sin(0.031 * xm + 1.0)
    f = 50.0
    if (x1 - x0) * f > 0.5:
        return -amp, amp
    v = [amp * (math.sin(2 * math.pi * f * x) + 0.08 * math.sin(10 * math.pi * f * x)) for x in (x0, xm, x1)]
    return min(v), max(v)


def lane_b(x0, x1):
    per, tau, f = 1.7, 0.11, 420.0

    def env(x):
        k = math.floor(x / per)
        return (0.35 + 0.6 * hashf(k)) * math.exp(-(x - k * per) / tau)

    if math.floor(x0 / per) != math.floor(x1 / per):
        e = 0.35 + 0.6 * hashf(math.floor(x1 / per))
    else:
        e = env(x0)
    if (x1 - x0) * f > 0.5:
        return -e, e
    v = [env(x) * math.sin(2 * math.pi * f * (x - math.floor(x / per) * per)) for x in (x0, 0.5 * (x0 + x1), x1)]
    return min(v), max(v)


def s_explorer(c, t, d):
    backdrop(c, t, 4.4, grid=False)
    z = ease_io(ramp(t, 0.9, d - 2.2))
    span = SPAN0 * (SPAN1 / SPAN0) ** z
    left = XC - span * (0.5 if z > 0 else XC / SPAN0)
    left = max(0.0, min(left, SPAN0 - span))
    px0, pw = 40, 720
    for lane, (fn, rgb, name) in enumerate(((lane_a, AZURE, "PHASE CURRENT"), (lane_b, AMBER, "VIBRATION"))):
        mid, amp = 208 + lane * 120, 50
        up, dn = [], []
        for i in range(0, pw + 1, 1):
            x0 = left + span * i / pw
            lo, hi = fn(x0, x0 + span / pw)
            up.append((px0 + i, mid - hi * amp))
            dn.append((px0 + i, mid - lo * amp))
        band = skia.Path()
        band.moveTo(*up[0])
        for q in up[1:]:
            band.lineTo(*q)
        for q in reversed(dn):
            band.lineTo(*q)
        band.close()
        c.drawPath(band, paint(rgb, 70))
        polyline(c, up, paint(rgb, 255, stroke=1.6))
        polyline(c, dn, paint(rgb, 255, stroke=1.6))
        c.drawLine(px0, mid + 60, px0 + pw, mid + 60, paint(LINE, 255, stroke=1))
        text(c, name, px0 + 4, mid - 46, font("mono", 11.5), rgb, 255, spacing=1.5)
    text(c, "1,048,576", 40, 96, font("num", 76), TEXT, 255 * ramp(t, 0.2, 0.5))
    text(c, "SAMPLES, FOUR CHANNELS", 44, 122, font("mono", 13), TEXT2, 255 * ramp(t, 0.5, 0.5), spacing=3)
    lab = f"{span:6.2f} s" if span >= 1 else f"{span * 1000:6.1f} ms"
    text(c, "SPAN", 760, 70, font("mono", 12), TEXT3, 255, "right", spacing=2)
    text(c, lab, 760, 104, font("mono", 30), TEXT, 255, "right")
    chyron(c, t, d, "Explorer", "PINCH FROM 105 SECONDS TO 30 MILLISECONDS", start=2.6)


# ---- scene 5: touch -----------------------------------------------------------

def s_touch(c, t, d):
    backdrop(c, t, 5.9)
    cols = (AZURE, AMBER, VIOLET, MINT, CORAL)
    for i, rgb in enumerate(cols):
        def pos(tt):
            return (W / 2 + 300 * math.sin((0.55 + 0.11 * i) * tt + i * 1.3),
                    230 + 150 * math.sin((0.83 + 0.07 * i) * tt + i * 0.7))

        grow = ease_out(ramp(t, 0.15 * i, 0.5))
        if grow <= 0:
            continue
        x, y = pos(t)
        trail = [pos(t - k / 40) for k in range(34)]
        for k in range(len(trail) - 1):
            c.drawLine(*trail[k], *trail[k + 1], paint(rgb, 210 * (1 - k / 33) * grow, stroke=4.5 * (1 - k / 40)))
        c.drawLine(0, y, W, y, paint(rgb, 46 * grow, stroke=1))
        c.drawLine(x, 0, x, H, paint(rgb, 46 * grow, stroke=1))
        c.drawCircle(x, y, 34 * grow, paint(rgb, 255, stroke=3))
        c.drawCircle(x, y, 9 * grow, paint(rgb, 235))
        rip = (t * 0.9 + i * 0.37) % 1.0
        c.drawCircle(x, y, 34 + 60 * ease_out(rip), paint(rgb, 150 * (1 - rip) * grow, stroke=2))
        text(c, f"{i + 1}  x {int(x):3d}  y {int(y):3d}", x + 44, y - 30, font("mono", 11.5), rgb, 255 * grow)
    chyron(c, t, d, "Touch Lab", "FIVE POINTS  MEASURED LATENCY", start=1.4, accent=MINT)


# ---- scene 6: video -----------------------------------------------------------

def s_video(c, t, d):
    backdrop(c, t, 7.3, grid=False)
    fw, fh, gap = 208, 125, 22
    scroll = t * 74
    pairs = ((AZURE, VIOLET), (AMBER, CORAL), (MINT, AZURE), (VIOLET, AMBER))
    for i in range(-1, 6):
        x = i * (fw + gap) - (scroll % (fw + gap)) + 40
        idx = i + int(scroll // (fw + gap))
        a, b = pairs[idx % 4]
        r = skia.Rect.MakeXYWH(x, 218, fw, fh)
        p = skia.Paint(AntiAlias=True)
        p.setShader(skia.GradientShader.MakeLinear([(x, 218), (x + fw, 218 + fh)], [col(a, 150), col(b, 60)]))
        c.drawRoundRect(r, 8, 8, paint(BG1))
        c.drawRoundRect(r, 8, 8, p)
        near = 1 - min(abs(x + fw / 2 - W / 2) / 180, 1)
        c.drawRoundRect(r, 8, 8, paint(TEXT if near > 0.5 else LINE, 110 + 145 * near, stroke=1.5 + near))
        text(c, f"{idx * 30 + 1:04d}", x + 12, 218 + fh - 12, font("mono", 12), TEXT, 200)
        pts = [(x + 14 + j * 3, 262 + 22 * math.sin(0.2 * j + idx) * math.sin(math.pi * j / 60)) for j in range(61)]
        polyline(c, pts, paint((255, 255, 255), 170, stroke=1.5))
    text(c, "Decoded in hardware", 44, 112, font("head", 58), TEXT, 255 * ramp(t, 0.2, 0.5))
    text(c, "MOTION JPEG  8 MS A FRAME  COLOUR CONVERSION BY DMA2D", 48, 144, font("mono", 12.5), TEXT2,
         255 * ramp(t, 0.6, 0.5), spacing=2.5)
    chyron(c, t, d, "Cinema", "800 X 480  30 FRAMES PER SECOND  STEREO", start=1.6, accent=AMBER)


# ---- scene 7: end card --------------------------------------------------------

def s_end(c, t, d):
    backdrop(c, t, 9.0)
    k = ease_out(t / 0.8)
    text(c, "N6", W / 2, 228 + 14 * (1 - k), font("head", 170), TEXT, 255 * k, "center")
    c.drawRect(skia.Rect.MakeXYWH(W / 2 - 60 * k, 252, 120 * k, 4), paint(AZURE))
    words, f = ("Zephyr", "LVGL", "Helium"), font("text", 30)
    widths = [f.measureText(w) for w in words]
    total = sum(widths) + 2 * 56
    x = W / 2 - total / 2
    for i, w in enumerate(words):
        a = ramp(t, 0.9 + 0.35 * i, 0.5)
        text(c, w, x, 316 + 10 * (1 - ease_out(a)), f, TEXT, 255 * a)
        x += widths[i]
        if i < 2:
            c.drawCircle(x + 28, 306, 3, paint(AZURE, 255 * a))
            x += 56
    text(c, "EVERY NUMBER ON SCREEN WAS MEASURED ON THE BOARD", W / 2, 366, font("mono", 12.5), TEXT2,
         255 * ramp(t, 2.2, 0.6), "center", spacing=3)
    c.drawRect(skia.Rect.MakeWH(W, H), paint((0, 0, 0), 255 * ease_io(ramp(t, d - 1.5, 1.4))))


SCENES = [s_open, s_title, s_fps, s_spectrum, s_explorer, s_touch, s_video, s_end]
# transition into scene i+1 at T[i+1]: (function, duration)
TRANS = [(tr_flash, 0.5), (tr_wipe_diag, 0.8), (tr_push, 0.7), (tr_iris, 0.9), (tr_blinds, 0.9),
         (tr_wipe_diag, 0.8), (tr_fade, 0.9)]


def draw_scene(surface, i: int, t: float) -> None:
    c = surface.getCanvas()
    c.save()
    SCENES[i](c, max(t - T[i], 0.0), T[i + 1] - T[i])
    c.restore()


def render_frame(t: float, out, sa, sb) -> None:
    i = max(k for k in range(len(SCENES)) if T[k] <= t)
    for j, (fn, dur) in enumerate(TRANS):
        cut = T[j + 1]
        if cut - dur / 2 <= t < cut + dur / 2:
            # both scenes keep running through the transition
            draw_scene(sa, j, t)
            draw_scene(sb, j + 1, t)
            fn(out.getCanvas(), sa.makeImageSnapshot(), sb.makeImageSnapshot(), (t - (cut - dur / 2)) / dur)
            return
    draw_scene(out, i, t)

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", type=float, nargs="*", help="write PNG stills at these times and exit")
    a = ap.parse_args()
    out, sa, sb = new_surface(), new_surface(), new_surface()

    if a.preview is not None:
        times = a.preview or [1.5 + i * 3.0 for i in range(16)]
        (OUT / "stills").mkdir(parents=True, exist_ok=True)
        for t in times:
            render_frame(t, out, sa, sb)
            img = out.makeImageSnapshot().resize(W, H, skia.SamplingOptions(skia.FilterMode.kLinear, skia.MipmapMode.kLinear))
            img.save(str(OUT / "stills" / f"film_{t:05.2f}.png"), skia.kPNG)
        print(f"{len(times)} stills in {OUT / 'stills'}")
        return

    wav = OUT / "n6_film.wav"
    write_wav(wav, score.render(TOTAL, T[2:-1], T[1]))
    w = Writer(OUT / "n6_film.mp4", wav)
    n = int(TOTAL * FPS)
    for f in range(n):
        render_frame(f / FPS, out, sa, sb)
        w.write(out)
        if f % 150 == 0:
            print(f"frame {f}/{n}", flush=True)
    w.close()
    subprocess.run([sys.executable, str(ROOT / "tools" / "n6v.py"), "encode", str(OUT / "n6_film.mp4"), "-o",
                    str(OUT / "n6_film.n6v"), "--title", "N6: the film", "--q", "4"], check=True)


if __name__ == "__main__":
    main()

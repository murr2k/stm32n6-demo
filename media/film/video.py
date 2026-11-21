#!/usr/bin/env python3
"""The YouTube video (docs/plan.html, V4): one attract lap recorded by the
board itself, framed in 4K at 60 fps: a cold open, three cards, the lap with
a caption per screen above the panel (nothing is drawn over the board's own
pixels) and a frame-time strip drawn from the board's own numbers, six
diagrams of how it is built, a card on taking it into a project, and the
lap's end titles set sharp in 4K.

The lap is the board's recording (tools/record.py --attract --fps 60): its
pictures scaled 4 times (3200 x 1920, every board pixel a 4 x 4 block) in a
3840 x 2160 frame, on the sound's clock. Two things the recording cannot
hold are re-created from its frame log: the panel's fade and settle at
every screen change (the display controller fades on the way to the panel,
after the frame buffer the encoder reads: the recording cuts hard) and the
frame time of each picture (the CSV's frame_us, the UI thread's time for the
frame in it). The sound under the cold open and the lap is the board's own
(the DAC feed in the recording, taken from the DAC's 47.7 kHz to 48 kHz at
the same speed); under the cards, the diagrams and the end titles it is the
film's score (score.py), rendered here. Everything else on screen is drawn
here with the film kit.

The recording holds camera footage (the lap's four gesture bars, the user's
hands): the take stays in captures/, and what this script writes goes to
media/out/; neither is committed.

Usage: video.py [--plan]               the timeline and the YouTube chapters
       video.py --still V [--out PNG]  the video's frame at V seconds, 4K
       video.py --preview [--out MP4]  1080p30: the cold open, the cards, two moments
                                       of the lap and everything after it
       video.py --render [--out MP4]   the whole video, 4K60, H.264 and AAC
"""
from __future__ import annotations

import argparse
import csv
import math
import multiprocessing as mp
import subprocess
import sys
import time
import wave
from bisect import bisect_right
from pathlib import Path

import numpy as np
import skia

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / "tools"))
import attract  # noqa: E402
import score  # noqa: E402
from kit import (AMBER, AZURE, BG1, CORAL, LINE, MINT, OUT, ROOT, TEXT, TEXT2, TEXT3, VIOLET, clamp, col,  # noqa: E402
                 ease_in, ease_io, ease_out, ffmpeg_exe, font, glow, paint, polyline, ramp, text, write_wav)
from record import picture_times  # noqa: E402

VW, VH, FPS = 3840, 2160, 60
SCALE = 4                                  # board pixels to video pixels
BX, BY = (VW - 800 * SCALE) // 2, (VH - 480 * SCALE) // 2   # the panel picture: 320, 120
BG0 = (8, 12, 18)                          # the board's C_BG0, also the layer's background in a fade
REFRESH_HZ = 59.977
TAKE = ROOT / "captures" / "rec" / "video-lap-20261118"
TAKE_TITLES_BAR = 56                       # the take's lap (56 bars, with Under the Hood): its end titles' bar;
                                           # attract.lap() is today's lap (52 bars since 2026-11-20)
CLIP_WAV = OUT / "n6_attract.wav"          # the lap clip's sound (attract.py): places the clip on the take's clock
                                           # by its first sound, which every lap clip shares (the film's music)
NAMES = ["Home", "Flight Deck", "Signal Lab", "Explorer", "Touch Lab", "Host Link", "Core Map", "Cinema",
         "Specimen", "Under the Hood", "Settings", "Camera"]
# the shell's screen change (app/src/ui/shell.c): fade out, dark hold, fade in while settling 10 px
FADE_OUT, FADE_HOLD, FADE_IN, SETTLE_PX = 0.110, 0.034, 0.200, 10

# the video's parts (seconds); the music's bar is the film's, 2.4 s
BAR = score.BAR
COLD_S, CARD_S = 10.0, 10.0
INTRO_S = COLD_S + 3 * CARD_S              # the lap starts here
COLD_LEAD = 0.35                           # into Signal Lab's stay: past the whoosh of its arrival
HOW_BARS, DIAG_BARS, TAKEIT_BARS = 1, 4, 8
TAIL_S = 1.0                               # black after the end titles

# captions, the approved script: the screen's own name and one measured fact
CAPTION = {
    "film": ("N6: the film", "DRAWN AND SCORED BY CODE, PLAYED FROM THE MICROSD CARD"),
    "Home": ("Home", "EVERY TILE A LIVE THUMBNAIL  ·  6.9 MS A FRAME"),
    "Flight Deck": ("Flight Deck", "2.5 MS A FRAME AT 60 FPS"),
    "Signal Lab": ("Signal Lab", "A THEREMIN, SYNTHESIZED LIVE  ·  TOUCH TO SOUND 3.5 MS"),
    "gestures": ("Hand tracking", "ON THE NPU, TWO HANDS AT 15 FPS  ·  RECORDED ON THIS BOARD"),
    "Explorer": ("Explorer", "4 × 1 M SAMPLES READ FROM FLASH IN PLACE  ·  7.4 MS A FRAME"),
    "Host Link": ("Host Link", "THE PC'S 22 CORES OVER UDP  ·  ROUND TRIP 0.64 MS"),
    "Core Map": ("Core Map", "A POST PER CORE, DRAWN IN 3.5 MS"),
    "Under the Hood": ("Under the Hood", "EVERY THREAD'S CPU TIME, LIVE  ·  8.3 MS A FRAME"),
}
ACCENT = {"film": AMBER, "gestures": MINT, "Signal Lab": VIOLET}


def rect(x, y, w, h):
    return skia.Rect.MakeXYWH(x, y, w, h)


def veil(fr, a: float, rgb=(0, 0, 0)) -> None:
    """the whole frame faded towards a colour (in numpy: a full-frame fill is slow in Skia here)"""
    if a >= 1:
        fr.arr[..., :3] = rgb
    elif a > 0:
        px = fr.arr[..., :3].astype(np.float32)
        fr.arr[..., :3] = (px + (np.array(rgb, np.float32) - px) * a + 0.5).astype(np.uint8)


# ---- the take -------------------------------------------------------------------------------

class Take:
    """The board's recording of one lap: its frame log on the sound's clock."""

    def __init__(self, base: Path):
        self.rows = list(csv.DictReader(open(base.with_suffix(".csv"), newline="")))
        wa = [r for r in self.rows if int(r["audio_pos"]) > 0]
        span = (int(wa[-1]["t_us"]) - int(wa[0]["t_us"])) & 0xFFFFFFFF
        self.rate = (int(wa[-1]["audio_pos"]) - int(wa[0]["audio_pos"])) * 1e6 / span
        self.times = picture_times(self.rows, REFRESH_HZ, self.rate)
        self.mkv, self.wav = base.with_suffix(".mkv"), base.with_suffix(".wav")
        self.lap0 = next(i for i, r in enumerate(self.rows) if NAMES[int(r["screen"])] == "Cinema")
        # stays on screens: (start time, screen name), a Cinema stay named by what it shows
        self.stays = []
        for i, r in enumerate(self.rows):
            s = NAMES[int(r["screen"])]
            if not self.stays or self.stays[-1][2] != int(r["screen"]):
                self.stays.append([self.times[i], s, int(r["screen"])])
        cinema = [k for k, st in enumerate(self.stays) if st[1] == "Cinema"]
        if len(cinema) >= 2:
            self.stays[cinema[0]][1] = "film"
            self.stays[cinema[1]][1] = "gestures"
        if len(cinema) >= 3:
            self.stays[cinema[2]][1] = "coda"
        self.switches = [st[0] for st in self.stays[1:]]

    def index_at(self, t: float) -> int:
        """the picture on screen at t (seconds on the sound's clock)"""
        return max(0, bisect_right(self.times, t) - 1)

    def stay_at(self, t: float):
        k = max(0, bisect_right([s[0] for s in self.stays], t) - 1)
        end = self.stays[k + 1][0] if k + 1 < len(self.stays) else self.times[-1]
        return self.stays[k][1], self.stays[k][0], end

    def stay(self, name: str) -> tuple[float, float]:
        k = next(k for k, s in enumerate(self.stays) if s[1] == name and k > 0)
        end = self.stays[k + 1][0] if k + 1 < len(self.stays) else self.times[-1]
        return self.stays[k][0], end

    def fade(self, t: float) -> tuple[float, float]:
        """the panel's layer alpha (0..1) and settle offset (board pixels) at t"""
        a, off = 1.0, 0.0
        for s in self.switches:
            if s - FADE_OUT <= t < s:
                a = 1 - ease_in((t - (s - FADE_OUT)) / FADE_OUT)
            elif s <= t < s + FADE_HOLD:
                a, off = 0.0, SETTLE_PX
            elif s + FADE_HOLD <= t < s + FADE_HOLD + FADE_IN:
                p = ease_out((t - s - FADE_HOLD) / FADE_IN)
                a, off = p, SETTLE_PX * (1 - p)
        return a, off


class Pictures:
    """The take's pictures, decoded in order (RGBA); going back starts the file again."""

    def __init__(self, mkv: Path):
        self.mkv = mkv
        self._open()

    def _open(self):
        import av
        self.box = av.open(str(self.mkv))
        self.frames = self.box.decode(video=0)
        self.k, self.frame, self.arr_k, self.arr = -1, None, -1, None

    def get(self, i: int) -> np.ndarray:
        if i < self.k:
            self.box.close()
            self._open()
        while self.k < i:
            self.frame = next(self.frames)
            self.k += 1
        if self.arr_k != i:
            self.arr = np.ascontiguousarray(self.frame.to_ndarray(format="rgba"))
            self.arr_k = i
        return self.arr


def onset(path: Path, lsb: int = 8) -> float:
    """seconds to the first sample above `lsb`"""
    with wave.open(str(path), "rb") as w:
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2)
        return int(np.argmax(np.abs(x).max(axis=1) > lsb)) / w.getframerate()


class Timeline:
    """Where each part of the video starts, and where the lap sits in the take."""

    def __init__(self, take: Take):
        self.take = take
        sl0, sl1 = take.stay("Signal Lab")
        self.cold_t0, self.cold_t1 = sl0 + COLD_LEAD, sl1      # the cold open: Signal Lab's flight
        self.cold_len = self.cold_t1 - self.cold_t0
        # the clip on the take's clock: its first sound, and the DAC's real rate (clip time is 48 kHz)
        self.speed = score.SR / take.rate
        self.clip0 = onset(take.wav) - onset(CLIP_WAV) * self.speed
        self.t_a = take.times[take.lap0]                       # the film's first picture
        self.t_b = self.clip0 + TAKE_TITLES_BAR * BAR * self.speed   # the coda's end
        self.how_v = INTRO_S + self.t_b - self.t_a
        self.takeit_v = self.how_v + (HOW_BARS + 6 * DIAG_BARS) * BAR
        self.end_v = self.takeit_v + TAKEIT_BARS * BAR
        self.total = self.end_v + attract.TITLES_S + TAIL_S

    def lap_v(self, t: float) -> float:
        return INTRO_S + t - self.t_a

    def first(self, name: str) -> float:
        return self.lap_v(self.take.stay(name)[0])

    def chapters(self) -> list[tuple[float, str]]:
        return [(0.0, "Cold open"), (COLD_S, "What it is"), (INTRO_S, "N6: the film"),
                (self.first("Home"), "Home and Flight Deck"),
                (self.first("Signal Lab"), "Signal Lab and hand tracking"),
                (self.first("Explorer"), "Explorer"), (self.first("Host Link"), "Host Link and Core Map"),
                (self.first("Under the Hood"), "Under the Hood"), (self.how_v, "How it's built"),
                (self.takeit_v, "Take it into your project"), (self.end_v, "Credits")]

    def preview_parts(self) -> list[tuple[float, float]]:
        cm = self.first("Core Map")
        return [(0.0, INTRO_S + 6.0), (cm - 4.0, cm + 5.0), (self.how_v - 4.0, self.total)]


# ---- the lap --------------------------------------------------------------------------------

def caption(c, name: str, t: float, t0: float, t1: float) -> None:
    """The screen's name and one measured fact in the margin above the panel, for the whole stay:
    out with the panel's fade, in after its dark hold, rising a little as the panel settles."""
    if name not in CAPTION:
        return
    title, sub = CAPTION[name]
    k_in = ease_out(ramp(t, t0 + FADE_HOLD + 0.06, 0.4))
    k_out = ease_in(ramp(t, t1 - FADE_OUT, FADE_OUT))
    a = k_in * (1 - k_out)
    if a <= 0:
        return
    tf, sf = font("text", 56), font("mono", 36)
    y = 92 + 12 * (1 - k_in)
    c.drawRect(rect(BX, y - 44, 6, 56), paint(ACCENT.get(name, AZURE), 255 * a))
    text(c, title, BX + 30, y, tf, TEXT, 255 * a)
    text(c, sub, BX + 30 + tf.measureText(title) + 44, y, sf, TEXT2, 255 * a, spacing=3)


def frame_strip(c, take: Take, i: int) -> None:
    """The last 4 s of the board's frames under the panel: each new frame's UI time against the
    refresh period, and the display's own count of dropped frames since the lap began."""
    period_us = 1e6 / REFRESH_HZ
    y_base, h_full = VH - 22, 74                 # 74 px for one refresh period
    n = 240
    first = max(take.lap0 + 1, i - n + 1)
    bw = 800 * SCALE / n
    p, idle = paint(AZURE, 230), paint(TEXT3, 170)
    for k in range(first, i + 1):
        r, q = take.rows[k], take.rows[k - 1]
        x = BX + (k - (i - n + 1)) * bw
        if r["frames"] == q["frames"]:
            # a refresh with nothing new to draw (the screen was still): a tick on the floor, so
            # the strip has a mark for every refresh and a quiet screen never looks like a gap
            c.drawRect(rect(x, y_base - 4, max(bw - 4, 2), 4), idle)
            continue
        us = int(r["frame_us"])
        hgt = min(us / period_us, 1.3) * h_full
        c.drawRect(rect(x, y_base - hgt, max(bw - 4, 2), hgt), p if us < period_us else paint(CORAL))
    yl = y_base - h_full
    dash = paint(TEXT3, 200, stroke=2)
    for x in np.arange(BX, BX + 800 * SCALE, 24):
        c.drawLine(x, yl, x + 12, yl, dash)
    mf = font("mono", 28)
    text(c, "FRAME TIME", BX - 36, y_base - 46, mf, TEXT2, 255, "right", spacing=2)
    text(c, "LINE: 16.7 MS", BX - 36, y_base - 10, mf, TEXT3, 255, "right", spacing=2)
    dropped = int(take.rows[i]["dropped"]) - int(take.rows[take.lap0]["dropped"])
    text(c, "DROPPED", BX + 800 * SCALE + 36, y_base - 46, mf, TEXT2, 255, spacing=2)
    text(c, str(dropped), BX + 800 * SCALE + 36, y_base - 4, font("num", 44), TEXT if dropped == 0 else CORAL)


def panel(fr: Frame, c, take: Take, t: float, img: np.ndarray, alpha: float = 1.0, border: float = 1.0) -> None:
    """The board's picture at 4x, nearest neighbour (every board pixel a 4 x 4 block), with the
    panel's fade and settle; written into the frame's pixels (Skia took a third of a second)."""
    a, off = take.fade(t)
    a *= alpha
    k = int(round(SCALE * fr.s))                          # 4, or 2 in the preview
    x0, y0, w, h = int(BX * fr.s), int(BY * fr.s), 800 * k, 480 * k
    if a > 0:
        dst = fr.arr[y0:y0 + h, x0:x0 + w]
        oy = int(round(off * k))
        if a >= 1 and oy == 0:
            dst.reshape(480, k, 800, k, 4)[:] = img[:, None, :, None, :]
        else:                                             # a fade: over the background, settling
            up = np.repeat(np.repeat(img, k, 0), k, 1)[:h - oy, :, :3].astype(np.float32)
            under = dst[oy:, :, :3].astype(np.float32)
            dst[oy:, :, :3] = (under + (up - under) * a + 0.5).astype(np.uint8)
    if border > 0:
        c.drawRect(rect(BX - 2, BY - 2, 800 * SCALE + 4, 480 * SCALE + 4), paint(LINE, 255 * border, stroke=2))


def lap_layer(fr: Frame, c, take: Take, t: float, img: np.ndarray, chrome: float = 1.0,
              alpha: float = 1.0) -> None:
    """The lap at t on the take's clock: the panel, its caption, the strip, faded in the margins by
    `chrome` (the panel is never covered)."""
    panel(fr, c, take, t, img, alpha)
    name, t0, t1 = take.stay_at(t)
    caption(c, name, t, t0, t1)
    frame_strip(c, take, take.index_at(t))
    text(c, "STM32N6570-DK  ·  RECORDED BY THE BOARD ITSELF AT 60 FPS", BX + 800 * SCALE, 92,
         font("mono", 26), TEXT3, 255, "right", spacing=3)
    if chrome < 1:
        p = paint(BG0, 255 * (1 - chrome))
        pw, ph = 800 * SCALE, 480 * SCALE
        for r in (rect(0, 0, VW, BY), rect(0, BY + ph, VW, VH - BY - ph), rect(0, BY, BX, ph),
                  rect(BX + pw, BY, VW - BX - pw, ph)):
            c.drawRect(r, p)


# ---- cards ----------------------------------------------------------------------------------
# drawn in 1920 x 1080 units (the canvas scaled 2x)

CARDS = [
    ("THE BOARD", "STM32N6570-DK",
     "CORTEX-M55 AT 800 MHZ WITH HELIUM  ·  NEURAL-ART NPU  ·  800 × 480 TOUCH PANEL"),
    ("THE SOFTWARE", "Zephyr RTOS  ·  LVGL 9.5  ·  Apache-2.0",
     "60 FRAMES A SECOND, EVERY FRAME TIMED ON THE BOARD"),
    ("WHAT FOLLOWS", "One attract lap, recorded frame for frame",
     "BY THE BOARD'S OWN H.264 ENCODER, WITH ITS OWN SOUND"),
]


def glows(t: float, seed: float):
    """the ground's three lights: centre, radius (1920 x 1080 units), colour, alpha"""
    return [(430 + 140 * math.sin(0.21 * t + seed), 300 + 90 * math.cos(0.17 * t + seed), 900, AZURE, 46),
            (1580 + 120 * math.cos(0.19 * t + 1.3 + seed), 930 + 70 * math.sin(0.23 * t + seed), 760, AMBER, 30),
            (1340 + 100 * math.sin(0.13 * t + 2.1 + seed), 140 + 70 * math.cos(0.11 * t + seed), 620, VIOLET, 26)]


def ground_skia(c, t: float, seed: float = 0.0) -> None:
    """The films' ground (kit.backdrop) at 16:9 in Skia, 1920 x 1080 units: dark field, drifting
    colour, faint grid, vignette. The reference for Ground, which draws the same in numpy."""
    W, H = 1920, 1080
    c.clear(col(BG0))
    for x, y, r, rgb, a in glows(t, seed):
        glow(c, x, y, r, rgb, a)
    p = paint((255, 255, 255), 9, stroke=1.5)
    off = (t * 14) % 96
    for x in np.arange(-96 + off, W + 96, 96):
        c.drawLine(x, 0, x, H, p)
    for y in np.arange(0, H + 96, 96):
        c.drawLine(0, y, W, y, p)
    v = skia.Paint()
    v.setShader(skia.GradientShader.MakeRadial((W / 2, H / 2), 1250, [col((0, 0, 0), 0), col((0, 0, 0), 170)],
                                               [0.55, 1.0]))
    c.drawRect(rect(0, 0, W, H), v)


class Ground:
    """ground_skia's picture written straight into the frame's pixels. Skia's raster path in this
    build takes 1.3 s for a 4K frame of it (each large gradient 0.4 to 0.5 s); this, 50 ms.
    The lights and the vignette are smooth (under half an LSB from one 4 x 4 block to the next):
    computed per block, written as whole pixels; the grid's lines at full resolution, each its own
    columns or rows, in the order Skia draws them."""

    F = 4                                                # device pixels per block, each way

    def __init__(self, w: int, h: int):
        self.w, self.h, self.u = w, h, w / 1920          # device pixels per unit
        F = self.F
        self.xu = ((np.arange(w // F) + 0.5) * F / self.u).astype(np.float32)
        self.yu = ((np.arange(h // F) + 0.5) * F / self.u).astype(np.float32)

        def keep(x, y):                                  # 1 - the vignette's alpha
            d = np.hypot(x[None, :] - 960, y[:, None] - 540) / 1250
            return (1 - np.clip((d - 0.55) / 0.45, 0, 1) * 170 / 255).astype(np.float32)

        self.keep_block = keep(self.xu, self.yu)[..., None]
        self.keep_full = keep((np.arange(w) + 0.5) / self.u, (np.arange(h) + 0.5) / self.u)
        self.rows = [self.cover(y, h) for y in np.arange(0, 1080 + 96, 96)]

    def cover(self, pos: float, n: int):
        """pixels under a 1.5-unit line at `pos` and how much of each it covers"""
        c, hw = pos * self.u, 0.75 * self.u
        out = []
        for px in range(int(math.floor(c - hw)), int(math.ceil(c + hw)) + 1):
            cov = min(px + 1, c + hw) - max(px, c - hw)
            if 0 <= px < n and cov > 0:
                out.append((px, cov * 9 / 255))
        return out

    def fill(self, arr: np.ndarray, t: float, seed: float) -> None:
        f = np.empty((len(self.yu), len(self.xu), 3), np.float32)
        f[:] = BG0
        for cx, cy, r, rgb, a in glows(t, seed):
            x0, x1 = np.searchsorted(self.xu, [cx - r, cx + r])
            y0, y1 = np.searchsorted(self.yu, [cy - r, cy + r])
            d = np.sqrt(((self.xu[x0:x1] - cx) ** 2)[None, :] + ((self.yu[y0:y1] - cy) ** 2)[:, None]) / r
            f[y0:y1, x0:x1] += (np.clip(1 - d, 0, None) * (a / 255))[..., None] * np.array(rgb, np.float32)
        np.minimum(f, 255, out=f)
        f *= self.keep_block
        q = (f + 0.5).astype(np.uint32)
        px = q[..., 0] | (q[..., 1] << 8) | (q[..., 2] << 16) | np.uint32(255 << 24)   # RGBA in memory
        arr.view(np.uint32).reshape(len(self.yu), self.F, len(self.xu), self.F)[:] = px[:, None, :, None]

        def line(sl, keep, g):
            px = arr[sl][..., :3].astype(np.float32)
            arr[sl + (slice(0, 3),)] = (px + (255 * keep[..., None] - px) * g + 0.5).astype(np.uint8)

        for x in np.arange(-96 + (t * 14) % 96, 1920 + 96, 96):
            for px, g in self.cover(x, self.w):
                line((slice(None), px), self.keep_full[:, px], g)
        for row in self.rows:
            for px, g in row:
                line((px, slice(None)), self.keep_full[px, :], g)


class Frame:
    """A frame's pixels (RGBA, what ffmpeg is fed) and a Skia canvas drawing into them."""

    def __init__(self, w: int = VW, h: int = VH):
        self.arr = np.zeros((h, w, 4), np.uint8)
        self.surface = skia.Surface(self.arr, colorType=skia.kRGBA_8888_ColorType,
                                    alphaType=skia.kPremul_AlphaType)
        self.s = w / VW                                  # device pixels per video pixel
        self.bg = Ground(w, h)


def ground(fr: Frame, t: float, seed: float = 0.0) -> None:
    fr.bg.fill(fr.arr, t, seed)


def card_text(c, k: int, t: float, dur: float) -> None:
    eyebrow, title, sub = CARDS[k]
    a = ease_out(ramp(t, 0.3, 0.7)) * (1 - ease_in(ramp(t, dur - 0.6, 0.5)))
    text(c, eyebrow, 960, 432, font("mono", 22), TEXT2, 255 * a, "center", spacing=6)
    text(c, title, 960, 530 + 14 * (1 - a), font("head", 76), TEXT, 255 * a, "center")
    b = ease_out(ramp(t, 0.8, 0.6)) * (1 - ease_in(ramp(t, dur - 0.6, 0.5)))
    c.drawRect(rect(960 - 90 * b, 566, 180 * b, 5), paint(AZURE, 255 * b))
    text(c, sub, 960, 632, font("mono", 22), TEXT2, 255 * b, "center", spacing=4)


def cold_title(c, w: float, v: float) -> None:
    a = ease_out(ramp(w, 0.45, 0.6)) * (1 - ease_in(ramp(v, COLD_S - 0.75, 0.5)))
    text(c, "Recorded by the board itself.", 960, 560 + 10 * (1 - a), font("head", 76), TEXT, 255 * a, "center")


# ---- how it's built: drawing kit --------------------------------------------------------------

def rbox(c, x, y, w, h, a, edge=LINE, fill=BG1, r=12, ew=2.0) -> None:
    if a <= 0:
        return
    rr = skia.RRect.MakeRectXY(rect(x, y, w, h), r, r)
    c.drawRRect(rr, paint(fill, 235 * a))
    c.drawRRect(rr, paint(edge, 255 * a, stroke=ew))


def small(c, s, x, y, rgb, a, align="left", size=18) -> float:
    return text(c, s, x, y, font("mono", size), rgb, 255 * a, align, spacing=2)


def block(c, x, y, w, h, title, sub, a, accent=AZURE, edge=LINE, ew=2.0) -> None:
    rbox(c, x, y, w, h, a, edge=edge, ew=ew)
    c.drawRect(rect(x, y + 16, 5, h - 32), paint(accent, 255 * a))
    text(c, title, x + 24, y + 46, font("text", 26), TEXT, 255 * a)
    if sub:
        small(c, sub, x + 24, y + 80, TEXT2, a, size=17)


def arrow(c, x0, y0, x1, y1, rgb, a, w=3.0, head=13.0) -> None:
    if a <= 0:
        return
    c.drawLine(x0, y0, x1, y1, paint(rgb, 255 * a, stroke=w))
    ang = math.atan2(y1 - y0, x1 - x0)
    p = skia.Path()
    p.moveTo(x1, y1)
    p.lineTo(x1 - head * math.cos(ang - 0.45), y1 - head * math.sin(ang - 0.45))
    p.lineTo(x1 - head * math.cos(ang + 0.45), y1 - head * math.sin(ang + 0.45))
    p.close()
    c.drawPath(p, paint(rgb, 255 * a))


def along(pts, s):
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        seg = math.hypot(x1 - x0, y1 - y0)
        if s <= seg:
            return x0 + (x1 - x0) * s / seg, y0 + (y1 - y0) * s / seg
        s -= seg
    return pts[-1]


def dots(c, pts, offset, gap, rgb, a, r=6.0) -> None:
    """packets moving along a polyline"""
    if a <= 0:
        return
    total = sum(math.hypot(x1 - x0, y1 - y0) for (x0, y0), (x1, y1) in zip(pts, pts[1:]))
    s = offset % gap
    p = paint(rgb, 255 * a)
    while s < total:
        x, y = along(pts, s)
        c.drawCircle(x, y, r, p)
        s += gap


def stall(u, period=0.8, step=120.0):
    """a position that moves only in the last 40 % of every period: traffic held off"""
    n = math.floor(u / period)
    return step * (n + ease_in(clamp((u / period - n - 0.6) / 0.4)))


# ---- how it's built: the six diagrams ---------------------------------------------------------

def d_vblank(c, u, a):
    """Slow motion, one refresh per bar: the panel scanned line by line from the buffer on it while
    LVGL draws the next frame into the other; the swap in the blanking, on the bar line."""
    blank = 21 / 501                          # lines 492..500 and 0..11 of 501
    r = int(u // BAR)
    q = u / BAR - r                           # 0: the blanking begins, the swap
    colors = (AZURE, AMBER)

    def picture(x, y, w, h, n, alpha):
        for i in range(6):
            ln = 0.25 + 0.65 * (0.5 + 0.5 * math.sin(0.9 * n + 1.4 * i))
            c.drawRect(rect(x + 0.07 * w, y + h * (0.12 + 0.135 * i), 0.86 * w * ln, h * 0.08),
                       paint(colors[n % 2], alpha))

    px, py, pw, ph = 160, 318, 667, 400
    scanning = q >= blank
    rbox(c, px - 12, py - 12, pw + 24, ph + 24, a, fill=(4, 7, 11), edge=LINE if scanning else VIOLET,
         ew=2 if scanning else 4)
    line = 12 + (q - blank) / (1 - blank) * 480 if scanning else 12
    sy = py + ph * (line - 12) / 480
    picture(px, py, pw, ph, r - 1, 255 * a)   # the panel holds the last refresh's frame,
    if sy > py:                               # rewritten line by line with this one's
        c.save()
        c.clipRect(rect(px, py, pw, sy - py))
        c.drawRect(rect(px, py, pw, ph), paint((4, 7, 11), 255 * a))
        picture(px, py, pw, ph, r, 255 * a)
        c.restore()
    if scanning:
        c.drawRect(rect(px, sy - 16, pw, 16), paint(colors[r % 2], 60 * a))
        c.drawLine(px, sy, px + pw, sy, paint(TEXT, 255 * a, stroke=3))
        small(c, f"LINE {int(line)}", px + pw + 22, sy + 6, TEXT2, a)
    small(c, "THE PANEL, SCANNED LINE BY LINE", px - 12, py + ph + 54, TEXT2, a)

    bx, bw, bh = 1000, 760, 190
    for j in range(2):
        by = 318 + j * 222
        front = r % 2 == j
        rgb = colors[j]
        rbox(c, bx, by, bw, bh, a, edge=rgb if front else LINE, ew=3 if front else 2)
        text(c, f"Frame buffer {'AB'[j]}", bx + 26, by + 50, font("text", 28), TEXT, 255 * a)
        small(c, "1.5 MB, INTERNAL SRAM", bx + 26, by + 84, TEXT3, a)
        tx, ty, tw, th = bx + bw - 26 - 230, by + 24, 230, 138
        rbox(c, tx, ty, tw, th, a, fill=(4, 7, 11), r=6)
        if front:
            picture(tx, ty, tw, th, r, 255 * a)
            small(c, f"FRAME {1000 + r} ON THE PANEL", bx + 26, by + 150, rgb, a)
        else:
            done = clamp((q - 0.04) / 0.42)
            if done > 0:
                c.save()
                c.clipRect(rect(tx, ty, tw, th * done))
                picture(tx, ty, tw, th, r + 1, 255 * a)
                c.restore()
            small(c, f"LVGL DRAWS FRAME {1001 + r}" if done < 1 else f"FRAME {1001 + r} READY",
                  bx + 26, by + 150, TEXT2, a)
    k = 1 - ramp(q, 0.0, 0.4)
    if k > 0:
        rbox(c, bx + bw / 2 - 70, 511, 140, 26, a * k, edge=VIOLET, fill=VIOLET, r=13)
        small(c, "SWAP", bx + bw / 2, 530, BG1, a * k, "center", 17)

    tx0, tw_, ty = 160, 1600, 852
    c.drawRect(rect(tx0, ty, tw_ * blank, 24), paint(VIOLET, 230 * a))
    c.drawRect(rect(tx0 + tw_ * blank + 3, ty, tw_ * (1 - blank) - 3, 24), paint(AZURE, 70 * a))
    small(c, "ACTIVE AREA: LINES 12 TO 491", tx0 + tw_ * blank + 16, ty - 16, TEXT2, a)
    small(c, "ONE REFRESH: 16.7 MS, SHOWN 144 TIMES SLOWER", tx0 + tw_, ty - 16, TEXT3, a, "right")
    small(c, "BLANKING: THE SWAP", tx0, ty + 56, VIOLET, a)
    x = tx0 + tw_ * q
    c.drawLine(x, ty - 8, x, ty + 32, paint(TEXT, 255 * a, stroke=3))


CAPS = ("50", "200", "1k", "5k", "20k", "-60 dB", "0 dB")


def d_handlers(c, u, a):
    """Signal Lab's spectrum with its axis captions as label objects (each an invalid area every
    frame, because the pane under it changed) and drawn by the pane's own handler."""
    tick = int(u / 0.4) % 2
    f18 = font("mono", 18)
    for j in range(2):
        x0, y0, w, h = 160 + j * 840, 350, 760, 360
        rbox(c, x0, y0, w, h, a, fill=(4, 7, 11))
        qx, qy, qw, qh = x0 + 28, y0 + 28, w - 56, h - 100
        pts = []
        for i in range(90):
            f = i / 89
            v = (0.22 + 0.5 * math.exp(-((f - 0.22 - 0.05 * math.sin(0.7 * u)) / 0.06) ** 2)
                 + 0.3 * math.exp(-((f - 0.55) / 0.04) ** 2) * (0.6 + 0.4 * math.sin(1.9 * u))
                 + 0.07 * math.sin(23 * f + 3.1 * u) + 0.05 * math.sin(41 * f - 5.3 * u))
            pts.append((qx + f * qw, qy + qh - clamp(v, 0.02, 0.98) * qh))
        polyline(c, pts, paint(VIOLET, 255 * a, stroke=3))
        spots = [(qx + qw * f, qy + qh + 44) for f in (0.04, 0.22, 0.44, 0.66, 0.86)] + \
                [(qx + 10, qy + qh - 14), (qx + 10, qy + 30)]
        n_inv = 1
        for (sx, sy), s in zip(spots, CAPS):
            if j == 0:
                rr = skia.RRect.MakeRectXY(rect(sx - 10, sy - 24, f18.measureText(s) + len(s) * 2 + 20, 34), 6, 6)
                c.drawRRect(rr, paint(BG1, 255 * a))
                c.drawRRect(rr, paint(CORAL, (230 if tick else 110) * a, stroke=2))
                n_inv += 1
            small(c, s, sx, sy, TEXT2, a)
        edge = CORAL if j == 0 else MINT
        c.drawRect(rect(qx - 5, qy - 5, qw + 10, qh + 10), paint(edge, (230 if tick or j else 110) * a, stroke=2))
        small(c, ("LVGL OBJECTS", "ONE DRAW HANDLER")[j], x0, y0 - 22, TEXT, a, size=20)
        small(c, f"INVALID AREAS A FRAME: {n_inv}", x0 + w, y0 - 22, edge, a, "right")
        ms = (8.3, 4.6)[j]
        g = ease_out(ramp(u, 1.2 + 0.5 * j, 0.8))
        by = y0 + h + 40
        rbox(c, x0, by, w, 22, a, fill=(4, 7, 11), r=4)
        c.drawRect(rect(x0, by, w * ms / 16.7 * g, 22), paint(edge, 230 * a))
        small(c, f"{ms} MS A FRAME", x0, by + 58, TEXT, a * g, size=20)
        small(c, "OF 16.7", x0 + w, by + 58, TEXT3, a, "right")
        small(c, ("SIGNAL LAB, ITS 7 AXIS CAPTIONS AS OBJECTS", "THE CAPTIONS DRAWN BY THE PANE'S HANDLER")[j],
              x0, by + 94, TEXT2, a * g)


def d_dma(c, u, a):
    """The audio buffer in AXISRAM: the DMA's reads wait behind the CPU's fills on the AXI and the
    SAI's FIFO runs dry; moved to AHBSRAM1, off the AXI, it never does."""
    move = ease_io(ramp(u, 4.3, 1.0))
    now = u >= 4.8
    small(c, "NOW: THE BUFFER IN AHBSRAM1, OFF THE AXI" if now else "BEFORE: THE AUDIO BUFFER IN AXISRAM",
          1760, 292, MINT if now else CORAL, a, "right", 20)
    by = 500
    rbox(c, 160, by - 22, 1600, 44, a, fill=(20, 28, 40), r=22)
    small(c, "AXI INTERCONNECT", 180, by + 7, TEXT3, a)
    block(c, 200, 318, 380, 110, "CPU", "LVGL FILLS LARGE AREAS", a)
    block(c, 1180, 318, 560, 110, "AXISRAM", "FRAME BUFFERS A AND B", a)
    for x in (390, 1320):
        c.drawLine(x, 428, x, by - 22, paint(LINE, 255 * a, stroke=3))
    c.drawLine(1638, 428, 1638, by - 22, paint(LINE, 255 * a * (1 - move), stroke=3))
    block(c, 640, 600, 320, 120, "GPDMA1", "CIRCULAR TRANSFER", a, MINT)
    block(c, 1040, 600, 340, 130, "SAI", "FIFO: 8 SAMPLES", a, MINT)
    block(c, 1460, 600, 300, 120, "WM8904", "TO THE HEADPHONES", a, MINT)
    arrow(c, 960, 650, 1036, 650, LINE, a)
    arrow(c, 1380, 650, 1456, 650, LINE, a)
    c.drawLine(812, by + 22, 812, 600, paint(LINE, 255 * a * (1 - move), stroke=3))
    if move > 0:
        block(c, 200, 770, 400, 110, "AHBSRAM1", "OFF THE AXI", a * move, MINT)
        polyline(c, [(600, 825), (812, 825), (812, 720)], paint(MINT, 160 * a * move, stroke=3))
    # traffic: the CPU's writes always; the DMA's reads held off before, steady now
    dots(c, [(390, 428), (390, by), (1320, by), (1320, 428)], u * 420, 30, AZURE, a, 5)
    held = stall(u, 0.8, 120)
    dots(c, [(1638, 428), (1638, by + 12), (812, by + 12), (812, 600)], held, 70, MINT, a * (1 - move), 6)
    dots(c, [(600, 825), (812, 825), (812, 720)], u * 260, 40, MINT, a * move, 6)
    flow = u * 260 if now else held
    dots(c, [(964, 650), (1036, 650)], flow, 36, MINT, a, 5)
    dots(c, [(1384, 650), (1456, 650)], flow, 36, MINT, a, 5)
    # the buffer itself
    cx, cy = 1556 + (420 - 1556) * move, 346 + (800 - 346) * move
    rbox(c, cx, cy, 164, 40, a, edge=MINT, fill=(20, 60, 48), r=8)
    small(c, "AUDIO BUFFER", cx + 82, cy + 27, TEXT, a, "center", 15)
    # the FIFO
    if now:
        level, under = 7.3 + 0.7 * math.sin(7 * u), False
    else:
        ph = (u / 0.8) % 1
        level, under = 8 * clamp(1 - ph / 0.7), ph > 0.7
    for i in range(8):
        r_ = rect(1064 + i * 38, 698, 28, 18)
        if i < level:
            c.drawRect(r_, paint(MINT, 230 * a))
        else:
            c.drawRect(r_, paint(CORAL if under else LINE, 255 * a, stroke=2))
    if under:
        small(c, "UNDERRUN", 1210, 584, CORAL, a, "center", 20)
    small(c, "UNDERRUNS IN THE TOUR: 0" if now else "UNDERRUNS: ABOUT 60 A SECOND", 1040, 790,
          MINT if now else CORAL, a, size=20)


def d_synth(c, u, a):
    """Blocks of 128 frames on the audio thread: a touch is read at the next block's start and
    heard from the block after it."""
    x0, w, span = 160, 1600, 16.0

    def X(ms):
        return x0 + w * ms / span

    blk = 128 / 48.0
    b1, b2 = blk, 2 * blk
    t_touch = b2 - 3.5                        # touch to sound 3.5 ms, as measured
    cur = span * clamp((u - 0.4) / 5.6)
    small(c, "THE PANEL", x0 + w, 340, TEXT3, a, "right")
    small(c, "THE AUDIO THREAD: BLOCKS OF 128 FRAMES", x0 + w, 428, TEXT3, a, "right")
    small(c, "AT THE DAC", x0 + w, 556, TEXT3, a, "right")
    c.drawLine(x0, 360, x0 + w, 360, paint(LINE, 255 * a, stroke=2))
    for k in range(6):
        bx = X(k * blk)
        rbox(c, bx + 3, 440, w * blk / span - 6, 56, a, r=8)
        small(c, "2.7 MS", bx + w * blk / span / 2, 476, TEXT3, a, "center", 16)
        if cur >= k * blk:
            c.drawRect(rect(bx + 3, 440, max(w * 0.23 / span, 6), 56), paint(MINT, 220 * a))
        c.drawLine(bx, 572, bx, 628, paint(LINE, 255 * a, stroke=2))
    if cur >= t_touch:
        g = clamp((cur - t_touch) / 2.0)
        c.drawCircle(X(t_touch), 360, 12 + 36 * g, paint(AZURE, 200 * a * (1 - g), stroke=3))
        c.drawCircle(X(t_touch), 360, 10, paint(AZURE, 255 * a))
        small(c, "A TOUCH", X(t_touch) + 24, 352, AZURE, a)
    if cur >= b1:
        arrow(c, X(t_touch) + 4, 372, X(b1) + 10, 436, AZURE, a)
        small(c, "READ AT THE BLOCK'S START; FIVE VOICES TAKE 0.1 TO 0.23 MS OF IT", X(b1) + 30, 528,
              TEXT2, a)
    pts = []
    for x in np.arange(x0, X(min(cur, span)), 3.0):
        ms = (x - x0) / w * span
        y = 0.0 if ms < b2 else 26 * math.sin(2 * math.pi * (ms - b2) / 0.9) * min((ms - b2) / 0.8, 1.0)
        pts.append((x, 600 - y))
    if len(pts) > 1:
        polyline(c, pts, paint(MINT, 255 * a, stroke=3))
    if cur >= b2:
        y = 664
        c.drawLine(X(t_touch), y, X(b2), y, paint(TEXT, 255 * a, stroke=2))
        for xx in (X(t_touch), X(b2)):
            c.drawLine(xx, y - 10, xx, y + 10, paint(TEXT, 255 * a, stroke=2))
        small(c, "TOUCH TO SOUND: 3.5 MS", (X(t_touch) + X(b2)) / 2, y + 36, TEXT, a, "center", 20)
    if cur < span:
        c.drawLine(X(cur), 318, X(cur), 640, paint(TEXT, 110 * a, stroke=2))
    for ms in range(0, 17, 4):
        small(c, f"{ms} MS" if ms == 16 else str(ms), X(ms), 736, TEXT3, a, "center", 16)
    g = ease_out(ramp(u, 6.0, 0.8))
    small(c, "TOUCH TO SOUND", 700, 776, TEXT3, a * g)

    def XC(ms):
        return 700 + 1000 * ms / 24

    for y, label, lo, hi, rgb, val in ((800, "512-FRAME BLOCKS, BEFORE", 11, 21, CORAL, "11 TO 21 MS"),
                                       (856, "128-FRAME BLOCKS, NOW", 3.5, 4.6, MINT, "3.5 MS, WORST 4.6")):
        small(c, label, x0, y + 16, TEXT2, a * g, size=20)
        c.drawRect(rect(XC(lo), y, max((XC(hi) - XC(lo)) * g, 4), 20), paint(rgb, 230 * a * g))
        small(c, val, XC(hi) + 16, y + 17, rgb, a * g, size=20)


def d_npu(c, u, a):
    """Double-buffered on every screen; on the Camera screen buffer B goes to the NPU as its working
    memory and the display runs from A alone."""
    lend = ease_io(ramp(u, 3.0, 1.2))
    lent = u >= 3.6
    small(c, "THE CAMERA SCREEN: B IS LENT TO THE NPU" if lent else "EVERY OTHER SCREEN: DOUBLE-BUFFERED",
          1760, 292, MINT if lent else TEXT2, a, "right", 20)
    front = 0 if lend > 0 else int(u / 0.6) % 2
    block(c, 760, 330, 340, 150, "LTDC", "TO THE PANEL", a)
    block(c, 1200, 520, 560, 260, "Neural-ART NPU", "", a, VIOLET)
    block(c, 160, 330, 440, 150, "Frame buffer A", "1.5 MB, INTERNAL SRAM", a, AZURE,
          edge=AZURE if front == 0 else LINE, ew=3 if front == 0 else 2)
    arrow(c, 604, 405, 756, 405, AZURE, a * (1.0 if front == 0 else 0.3))
    arrow(c, 604, 600, 900, 486, AMBER, a * (1 - lend) * (1.0 if front == 1 else 0.3))
    bx, by = 160 + (1240 - 160) * lend, 540 + (600 - 540) * lend
    block(c, bx, by, 440, 150, "Frame buffer B", "THE NETWORKS' WORKING MEMORY" if lent else "1.5 MB, INTERNAL SRAM", a,
          AMBER, edge=AMBER if (front == 1 and lend == 0) or lent else LINE, ew=3)
    k = ramp(u, 4.2, 0.5)
    small(c, "THE DISPLAY RUNS SINGLE-BUFFERED FROM A,", 160, 548, TEXT2, a * k)
    small(c, "EACH FRAME STARTED AT THE TOP OF THE REFRESH", 160, 580, TEXT2, a * k)
    g = ease_out(ramp(u, 4.8, 1.0))
    X0 = 560
    for name, slow, fast, y in (("PALM DETECTOR", 938, 65, 812), ("HAND LANDMARKS", 350, 22, 880)):
        small(c, name, 160, y + 28, TEXT2, a * g, size=20)
        c.drawRect(rect(X0, y, slow * g, 16), paint(CORAL, 220 * a * g))
        small(c, f"{slow} MS, ALL IN PSRAM", X0 + slow * g + 14, y + 15, CORAL, a * g, size=17)
        c.drawRect(rect(X0, y + 24, max(fast * g, 3), 16), paint(MINT, 220 * a * g))
        small(c, f"{fast} MS IN THE LENT BUFFER", X0 + fast * g + 14, y + 39, MINT, a * g, size=17)


def d_recorder(c, u, a):
    """The VENC encodes the buffer on screen every refresh; pictures and the DAC's own feed go to
    the PC in one stream, each picture with the numbers this video's strip is drawn from."""
    row1 = [(160, 300, "Frame buffer", "ON SCREEN, XRGB8888"), (540, 290, "VENC", "H.264, ABOUT 7 MS"),
            (910, 300, "12 × 256 KB", "IN PSRAM, TO BE SENT"), (1290, 200, "Ethernet", "TCP"),
            (1570, 190, "The PC", "record.py")]
    for x, w, t, s in row1:
        block(c, x, 330, w, 120, t, s, a)
    for (x, w, _, _), (nx, _, _, _) in zip(row1, row1[1:]):
        arrow(c, x + w + 4, 390, nx - 4, 390, LINE, a)
        for k in range(2):
            px = x + w + 8 + ((u * 0.9 + k / 2) % 1) * (nx - x - w - 24)
            c.drawRect(rect(px, 382, 16, 16), paint(AZURE, 255 * a))
    block(c, 160, 560, 300, 110, "DAC feed", "EVERY BLOCK PLAYED", a, MINT)
    block(c, 540, 560, 290, 110, "Audio tap", "A RING IN PSRAM", a, MINT)
    arrow(c, 464, 615, 536, 615, LINE, a)
    path = [(834, 615), (1390, 615), (1390, 454)]
    polyline(c, path[:2], paint(LINE, 255 * a, stroke=3))
    arrow(c, 1390, 615, 1390, 454, LINE, a)
    dots(c, path, u * 300, 60, MINT, a, 6)
    small(c, "EACH PICTURE ALSO CARRIES", 160, 742, TEXT3, a)
    chips = [("The DAC frame playing", "PICTURES PLACED ON THE SOUND'S CLOCK"),
             ("The UI's time for its frame", "THE FRAME-TIME STRIP UNDER THE LAP"),
             ("The dropped-frame count", "THE COUNT BESIDE THE STRIP")]
    for i, (t, s) in enumerate(chips):
        k = a * ease_out(ramp(u, 2.5 + 0.6 * i, 0.6))
        x = 160 + i * 546
        rbox(c, x, 764, 508, 100, k)
        c.drawRect(rect(x, 780, 5, 68), paint((MINT, AZURE, AZURE)[i], 255 * k))
        text(c, t, x + 24, 806, font("text", 24), TEXT, 255 * k)
        small(c, s, x + 24, 842, TEXT2, k, size=16)


HOW = [
    ("Swapping frames in the vertical blanking", d_vblank,
     "present() SWAPS THE LAYER'S ADDRESS IN THE BLANKING: NO COPY, NO TEARING"),
    ("Draw handlers instead of widgets for dense pictures", d_handlers,
     "LVGL KEEPS 32 INVALID AREAS A FRAME: ONE MORE AND IT REDRAWS THE WHOLE SCREEN, ABOUT 10 MS"),
    ("Audio DMA kept in AHB SRAM: 0 underruns", d_dma,
     "THE CPU'S FILLS HELD OFF THE DMA'S READS FOR LONGER THAN THE SAI'S 8-SAMPLE FIFO LASTS"),
    ("A synthesizer on the audio thread: 2.7 ms blocks", d_synth,
     "THE TOUCH TABLE IS READ AT THE START OF EVERY 128-FRAME BLOCK, ON THE AUDIO THREAD"),
    ("The NPU borrowing the second frame buffer", d_npu,
     "THE WEIGHTS ARE READ IN PLACE FROM THE FLASH; B GOES BACK WHEN THE CAMERA SCREEN CLOSES"),
    ("The recorder that made this video", d_recorder,
     "THIS LAP: 9,511 PICTURES IN 158.6 S, NONE MISSED, 0 DROPPED FRAMES"),
]


def how_frame(c, u: float) -> None:
    if u < HOW_BARS * BAR:
        a = ease_out(ramp(u, 0.3, 0.6)) * (1 - ease_in(ramp(u, BAR - 0.5, 0.4)))
        text(c, "How it's built", 960, 560 + 12 * (1 - a), font("head", 96), TEXT, 255 * a, "center")
        b = ease_out(ramp(u, 0.6, 0.6)) * (1 - ease_in(ramp(u, BAR - 0.5, 0.4)))
        c.drawRect(rect(960 - 90 * b, 598, 180 * b, 5), paint(AZURE, 255 * b))
        return
    k = min(int((u - HOW_BARS * BAR) // (DIAG_BARS * BAR)), len(HOW) - 1)
    t = u - (HOW_BARS + k * DIAG_BARS) * BAR
    dur = DIAG_BARS * BAR
    title, fn, fact = HOW[k]
    a = ease_out(ramp(t, 0.15, 0.6)) * (1 - ease_in(ramp(t, dur - 0.55, 0.45)))
    if a <= 0:
        return
    text(c, f"HOW IT'S BUILT  ·  {k + 1} OF {len(HOW)}", 160, 150, font("mono", 22), TEXT2, 255 * a, spacing=6)
    text(c, title, 160, 228 + 10 * (1 - a), font("head", 60), TEXT, 255 * a)
    fn(c, t, a)
    text(c, fact, 160, 990, font("mono", 22), TEXT2, 255 * a * ramp(t, 0.9, 0.5), spacing=3)


# ---- take it into your project, and the end titles ----------------------------------------------

TAKEIT_CMDS = [("git clone https://github.com/murr2k/stm32n6-demo.git", ""),
               ("west init -l app", ""),
               ("west update", "# Zephyr 4.4.0 and its modules"),
               ("n6.cmd go", "# build, flash, power-cycle, start")]


def takeit_frame(fr: Frame, c, u: float) -> None:
    dur = TAKEIT_BARS * BAR
    a = ease_out(ramp(u, 0.2, 0.7))
    text(c, "TAKE IT INTO YOUR PROJECT", 960, 236, font("mono", 22), TEXT2, 255 * a, "center", spacing=6)
    text(c, "github.com/murr2k/stm32n6-demo", 960, 330 + 10 * (1 - a), font("mono", 56), TEXT, 255 * a, "center")
    b = ease_out(ramp(u, 0.6, 0.6))
    c.drawRect(rect(960 - 90 * b, 366, 180 * b, 5), paint(AZURE, 255 * b))
    rbox(c, 360, 420, 1200, 270, a, fill=(4, 7, 11))
    mf = font("mono", 28)
    for i, (cmd, note) in enumerate(TAKEIT_CMDS):
        t0 = 1.6 + 1.7 * i
        n = int(clamp((u - t0) * 34, 0, len(cmd)))
        if u < t0:
            break
        y = 482 + 54 * i
        text(c, ">", 400, y, mf, AZURE, 255 * a)
        w = text(c, cmd[:n], 440, y, mf, TEXT, 255 * a)
        typing = n < len(cmd) or (i == len(TAKEIT_CMDS) - 1)
        if typing and int(u * 2.5) % 2 == 0:
            c.drawRect(rect(444 + w, y - 24, 14, 30), paint(TEXT2, 200 * a))
        if n == len(cmd) and note:
            text(c, note, 440 + w + 30, y, mf, TEXT3, 255 * a * ramp(u, t0 + len(cmd) / 34, 0.4))
    k = ease_out(ramp(u, 9.0, 0.7))
    text(c, "APACHE-2.0, LIKE ZEPHYR", 960, 776, font("mono", 22), TEXT, 255 * k, "center", spacing=4)
    k = ease_out(ramp(u, 9.8, 0.7))
    text(c, "ST'S CAMERA ISP AND NPU RUNTIME COME FROM ST: FETCHED OR INSTALLED, NEVER COMMITTED", 960, 822,
         font("mono", 20), TEXT2, 255 * k, "center", spacing=2)
    veil(fr, ease_io(ramp(u, dur - 0.9, 0.8)))


def end_frame(fr: Frame, c, v: float, u: float) -> None:
    """The lap's end titles (attract.py's words and timings) on the 16:9 ground."""
    if u >= attract.TITLES_S:
        veil(fr, 1.0)
        return
    ground(fr, v, 11.0)
    c.save()
    c.translate((1920 - 800 * 2.25) / 2, 0)
    c.scale(2.25, 2.25)
    attract.end_title_text(c, u)
    c.restore()
    veil(fr, max(1 - ease_io(ramp(u, 0.0, 0.8)),
                 ease_io(ramp(u, attract.CREDITS_S + attract.THANKS_S, attract.FADE_S))))


# ---- one frame ----------------------------------------------------------------------------------

def draw_frame(fr: Frame, v: float, tl: Timeline, pics: Pictures) -> None:
    """The video's frame at v seconds, into the frame's pixels (drawn in 3840 x 2160 units)."""
    take = tl.take
    c = fr.surface.getCanvas()
    c.save()
    c.scale(fr.s, fr.s)
    c.clear(col(BG0))
    if v < tl.cold_len:                       # the cold open: Signal Lab's ghost, the board's own sound
        t = tl.cold_t0 + v
        env = ease_out(ramp(v, 0.0, 0.5))
        panel(fr, c, take, t, pics.get(take.index_at(t)), env,
              env * (1 - ease_in(ramp(v, tl.cold_len - FADE_OUT, FADE_OUT))))
    elif INTRO_S <= v < tl.how_v:             # the lap
        t = tl.t_a + v - INTRO_S
        chrome = ease_out(ramp(v, INTRO_S, 0.6)) * (1 - ease_in(ramp(t, tl.t_b - 0.6, 0.6)))
        lap_layer(fr, c, take, t, pics.get(take.index_at(t)), chrome, 1 - ramp(t, tl.t_b - 0.3, 0.3))
    else:
        c.scale(2, 2)                         # the titles' 1920 x 1080 units
        if v < INTRO_S:
            ground(fr, v, 3.0)
            if v < COLD_S:
                cold_title(c, v - tl.cold_len, v)
            else:
                k = int((v - COLD_S) // CARD_S)
                card_text(c, k, v - COLD_S - k * CARD_S, CARD_S)
            veil(fr, 1 - ease_io(ramp(v, tl.cold_len + 0.05, 0.6)), BG0)
            veil(fr, ease_io(ramp(v, INTRO_S - 0.65, 0.6)), BG0)
        elif v < tl.takeit_v:
            ground(fr, v, 5.0)
            how_frame(c, v - tl.how_v)
            veil(fr, 1 - ease_io(ramp(v, tl.how_v, 0.6)), BG0)
        elif v < tl.end_v:
            ground(fr, v, 5.0)
            takeit_frame(fr, c, v - tl.takeit_v)
        else:
            end_frame(fr, c, v, v - tl.end_v)
    c.restore()


# ---- sound ----------------------------------------------------------------------------------

def fades(x: np.ndarray, fin: float, fout: float, sr: int = score.SR) -> np.ndarray:
    for n, rev in ((int(fin * sr), False), (int(fout * sr), True)):
        n = min(n, len(x))
        if n > 0:
            g = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
            if rev:
                x[-n:] *= g[::-1, None]
            else:
                x[:n] *= g[:, None]
    return x


def soundtrack(tl: Timeline) -> np.ndarray:
    """The board's sound under the cold open and the lap, the film's score under the rest."""
    sr = score.SR
    with wave.open(str(tl.take.wav), "rb") as w:
        raw = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2).astype(np.float64) / 32768
    # the DAC's samples, played at its real rate (take.rate), to 48 kHz at the same speed: band-limited
    n_out = int(round(len(raw) * sr / tl.take.rate))
    spec = np.fft.rfft(raw, axis=0)
    sp2 = np.zeros((n_out // 2 + 1, 2), complex)
    m = min(len(sp2), len(spec))
    sp2[:m] = spec[:m]
    board = np.fft.irfft(sp2, n_out, axis=0) * (n_out / len(raw))
    del spec, sp2, raw

    def seg(t0, t1, fin, fout):
        return fades(board[int(round(t0 * sr)):int(round(t1 * sr))].copy(), fin, fout)

    n_total = int(round(tl.total * sr))
    mix = np.zeros((n_total + sr * 30, 2))

    def put(v, x):
        i = int(round(v * sr))
        mix[i:i + len(x)] += x

    put(0.0, seg(tl.cold_t0, tl.cold_t1 - 0.02, 0.12, 0.45))
    bed_v = tl.cold_len + 0.05                # pads under the cold open's title and the cards
    bed = INTRO_S - bed_v
    put(bed_v, score.render(bed, [COLD_S - bed_v + k * CARD_S for k in range(3)], bed + 2 * BAR))
    put(INTRO_S, seg(tl.t_a, tl.t_b + 1.5, 0.0, 1.5))
    how = (HOW_BARS + 6 * DIAG_BARS + TAKEIT_BARS) * BAR + attract.TITLES_S
    cuts = [(HOW_BARS + DIAG_BARS * k) * BAR for k in range(1, len(HOW))] + [tl.takeit_v - tl.how_v]
    put(tl.how_v, score.render(how, cuts, HOW_BARS * BAR, tail=attract.TITLES_S))
    mix = mix[:n_total]
    peak = float(np.abs(mix).max())
    print(f"soundtrack {tl.total:.2f} s, peak {20 * math.log10(peak):.2f} dBFS")
    if peak > 0.98:
        mix *= 0.98 / peak
    return mix


# ---- rendering ----------------------------------------------------------------------------------

def encoder(path: Path, w: int, h: int, fps: int, preview: bool) -> list[str]:
    q = (["-preset", "veryfast", "-crf", "25", "-maxrate", "1300k", "-bufsize", "2600k", "-g", "60"] if preview
         # three of these at once: with 8 threads and a 30-frame lookahead each held 2.1 GB at 4K
         else ["-preset", "medium", "-crf", "16", "-g", "120", "-x264-params", "rc-lookahead=20", "-threads", "5"])
    return [ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba",
            "-s", f"{w}x{h}", "-r", str(fps), "-i", "-",
            "-vf", "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p", "-c:v", "libx264", *q,
            "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv",
            str(path)]


def render_chunk(job) -> str:
    times, path, preview, wk = job
    take = Take(TAKE)
    tl, pics = Timeline(take), Pictures(take.mkv)
    w, h, fps = (1920, 1080, 30) if preview else (VW, VH, FPS)
    fr = Frame(w, h)
    proc = subprocess.Popen(encoder(path, w, h, fps, preview), stdin=subprocess.PIPE)
    t0 = time.time()
    for n, v in enumerate(times):
        draw_frame(fr, v, tl, pics)
        proc.stdin.write(fr.arr.data)
        if n % 300 == 0:
            print(f"  part {wk}: {n}/{len(times)} frames, {time.time() - t0:.0f} s", flush=True)
    proc.stdin.close()
    if proc.wait() != 0:
        raise RuntimeError(f"ffmpeg failed on {path}")
    return str(path)


def render(tl: Timeline, out: Path, preview: bool, workers: int) -> None:
    fps = 30 if preview else FPS
    parts = tl.preview_parts() if preview else [(0.0, tl.total)]
    times = [v0 + i / fps for v0, v1 in parts for i in range(int(round((v1 - v0) * fps)))]
    tmp = OUT / ("video_parts_preview" if preview else "video_parts")
    tmp.mkdir(parents=True, exist_ok=True)
    n = len(times)
    bounds = [round(n * k / workers) for k in range(workers + 1)]
    jobs = [(times[bounds[k]:bounds[k + 1]], tmp / f"part{k:02d}.mp4", preview, k) for k in range(workers)]
    print(f"{n} frames at {fps} fps in {workers} parts")
    t0 = time.time()
    with mp.get_context("spawn").Pool(workers) as pool:
        files = pool.map(render_chunk, jobs)
    print(f"pictures done in {time.time() - t0:.0f} s")

    sound = soundtrack(tl)
    if preview:                               # the same parts of the sound, joined with short fades
        sr = score.SR
        sound = np.concatenate([fades(sound[int(round(v0 * sr)):int(round(v1 * sr))].copy(), 0.03, 0.03)
                                for v0, v1 in parts])
    wav = tmp / "sound.wav"
    write_wav(wav, sound, score.SR)
    lst = tmp / "parts.txt"
    lst.write_text("".join(f"file '{Path(f).name}'\n" for f in files))
    subprocess.run([ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0",
                    "-i", str(lst), "-i", str(wav), "-map", "0:v", "-map", "1:a", "-c:v", "copy",
                    # the encoder's options left the stream's primaries and transfer "unknown": say
                    # BT.709 in the stream itself (no re-encoding) and in the container
                    "-bsf:v", "h264_metadata=video_full_range_flag=0:colour_primaries=1:"
                              "transfer_characteristics=1:matrix_coefficients=1",
                    "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709", "-c:a", "aac",
                    "-b:a", "160k" if preview else "384k", "-ar", "48000", "-movflags", "+faststart", "-shortest",
                    str(out)], check=True)
    print(out, f"{out.stat().st_size / 1e6:.1f} MB")


def mmss(v: float) -> str:
    return f"{int(v) // 60}:{int(v) % 60:02d}"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plan", action="store_true", help="the timeline and the chapters (the default)")
    ap.add_argument("--still", type=float, help="the video's frame at this time, in seconds")
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--render", action="store_true")
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    take = Take(TAKE)
    tl = Timeline(take)
    if a.still is not None:
        out = a.out or OUT / "video_still.png"
        fr = Frame()
        draw_frame(fr, a.still, tl, Pictures(take.mkv))
        fr.surface.makeImageSnapshot().save(str(out), skia.kPNG)
        print(out)
    elif a.preview or a.render:
        out = a.out or OUT / ("n6_video_preview.mp4" if a.preview else "n6_video_4k60.mp4")
        render(tl, out, a.preview, a.workers)
    else:
        print(f"cold open: the take {tl.cold_t0:.3f} to {tl.cold_t1:.3f} s; the lap: {tl.t_a:.3f} to "
              f"{tl.t_b:.3f} s (the clip's start at {tl.clip0:.4f} s, speed {tl.speed:.6f})")
        print(f"how it's built {tl.how_v:.2f} s, take it {tl.takeit_v:.2f} s, credits {tl.end_v:.2f} s, "
              f"total {tl.total:.2f} s ({mmss(tl.total)})")
        for v, name in tl.chapters():
            print(f"{mmss(v)} {name}")


if __name__ == "__main__":
    main()

"""Shared drawing kit for the demo films: palette, type, easing, lower thirds,
transitions and the ffmpeg writer.

Everything is drawn in 800 x 480 units on a canvas scaled 2x, then scaled down
by ffmpeg with Lanczos, so edges and small type are supersampled.
"""
from __future__ import annotations

import glob
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
import skia

ROOT = Path(__file__).resolve().parents[2]
FONTS = ROOT / "assets" / "fonts" / "src"
OUT = ROOT / "media" / "out"
W, H, SS, FPS = 800, 480, 2, 30

BG0 = (8, 12, 18)
BG1 = (15, 21, 30)
LINE = (34, 45, 60)
TEXT = (233, 238, 245)
TEXT2 = (152, 166, 184)
TEXT3 = (95, 109, 128)
AZURE = (76, 181, 255)
AMBER = (255, 177, 61)
VIOLET = (181, 140, 255)
MINT = (61, 220, 160)
CORAL = (255, 107, 122)


def col(rgb, a=255):
    return skia.Color(int(rgb[0]), int(rgb[1]), int(rgb[2]), int(max(0, min(255, a))))


def _typeface(name: str):
    path = str(FONTS / name)
    try:
        return skia.Typeface.MakeFromFile(path)
    except AttributeError:
        return skia.FontMgr.New_Custom_Empty().makeFromFile(path)


_TF = {}


def font(kind: str, size: float) -> skia.Font:
    files = {"head": "IBMPlexSansCondensed-SemiBold.ttf", "num": "IBMPlexSansCondensed-Medium.ttf",
             "text": "IBMPlexSans-Medium.ttf", "reg": "IBMPlexSans-Regular.ttf", "mono": "IBMPlexMono-Medium.ttf"}
    if kind not in _TF:
        _TF[kind] = _typeface(files[kind])
    f = skia.Font(_TF[kind], size)
    f.setSubpixel(True)
    f.setEdging(skia.Font.Edging.kAntiAlias)
    return f


# ---- easing -----------------------------------------------------------------

def clamp(x, lo=0.0, hi=1.0):
    return lo if x < lo else hi if x > hi else x


def ease_out(t):
    t = clamp(t)
    return 1 - (1 - t) ** 3


def ease_in(t):
    t = clamp(t)
    return t ** 3


def ease_io(t):
    t = clamp(t)
    return 4 * t * t * t if t < 0.5 else 1 - (-2 * t + 2) ** 3 / 2


def ramp(t, start, dur):
    """0 before start, 1 after start + dur."""
    return clamp((t - start) / dur) if dur > 0 else float(t >= start)


# ---- primitives ---------------------------------------------------------------

def paint(rgb, a=255, stroke=None, blur=None, plus=False):
    p = skia.Paint(AntiAlias=True, Color=col(rgb, a))
    if stroke is not None:
        p.setStyle(skia.Paint.kStroke_Style)
        p.setStrokeWidth(stroke)
        p.setStrokeCap(skia.Paint.kRound_Cap)
        p.setStrokeJoin(skia.Paint.kRound_Join)
    if blur:
        p.setMaskFilter(skia.MaskFilter.MakeBlur(skia.kNormal_BlurStyle, blur))
    if plus:
        p.setBlendMode(skia.BlendMode.kPlus)
    return p


def text(c, s, x, y, f, rgb, a=255, align="left", spacing=0.0):
    """Draw a string; returns its width. `spacing` adds tracking in px per glyph."""
    if spacing:
        widths = [f.measureText(ch) + spacing for ch in s]
        total = sum(widths) - spacing
    else:
        total = f.measureText(s)
    if align == "center":
        x -= total / 2
    elif align == "right":
        x -= total
    p = paint(rgb, a)
    if spacing:
        for ch, w in zip(s, widths):
            c.drawString(ch, x, y, f, p)
            x += w
    else:
        c.drawString(s, x, y, f, p)
    return total


def glow(c, x, y, r, rgb, a):
    """Soft additive light: a radial gradient, cheap compared with a blur."""
    p = skia.Paint(AntiAlias=True)
    p.setShader(skia.GradientShader.MakeRadial((x, y), r, [col(rgb, a), col(rgb, 0)]))
    p.setBlendMode(skia.BlendMode.kPlus)
    c.drawCircle(x, y, r, p)


def backdrop(c, t, seed=0.0, grid=True):
    """The films' common ground: dark field, drifting colour, faint grid, vignette."""
    c.clear(col(BG0))
    glow(c, 180 + 60 * math.sin(0.21 * t + seed), 130 + 40 * math.cos(0.17 * t + seed), 380, AZURE, 46)
    glow(c, 660 + 50 * math.cos(0.19 * t + 1.3 + seed), 390 + 30 * math.sin(0.23 * t + seed), 320, AMBER, 30)
    glow(c, 560 + 40 * math.sin(0.13 * t + 2.1 + seed), 60 + 30 * math.cos(0.11 * t + seed), 260, VIOLET, 26)
    if grid:
        p = paint((255, 255, 255), 9, stroke=1)
        off = (t * 6) % 40
        for x in np.arange(-40 + off, W + 40, 40):
            c.drawLine(x, 0, x, H, p)
        for y in np.arange(0, H + 40, 40):
            c.drawLine(0, y, W, y, p)
    v = skia.Paint()
    v.setShader(skia.GradientShader.MakeRadial((W / 2, H / 2), 520, [col((0, 0, 0), 0), col((0, 0, 0), 170)],
                                               [0.55, 1.0]))
    c.drawRect(skia.Rect.MakeWH(W, H), v)


def polyline(c, pts, p):
    path = skia.Path()
    path.moveTo(*pts[0])
    for q in pts[1:]:
        path.lineTo(*q)
    c.drawPath(path, p)


def chyron(c, t, dur, title, sub, start=2.2, hold_out=0.5, accent=AZURE):
    """Lower third: accent bar wipes in, plate follows, title and caption reveal.
    Leaves the same way before the scene ends.
    """
    t_in = ramp(t, start, 0.5)
    t_out = ramp(t, dur - hold_out - 0.45, 0.45)
    if t_in <= 0 or t_out >= 1:
        return
    k = ease_out(t_in) * (1 - ease_in(t_out))
    x0, y0, h = 48, 372, 62
    tf, sf = font("text", 24), font("mono", 12.5)
    wplate = max(tf.measureText(title), sf.measureText(sub) + len(sub) * 1.2) + 44
    c.save()
    c.clipRect(skia.Rect.MakeXYWH(x0, y0 - 4, (wplate + 8) * k, h + 8))
    c.drawRect(skia.Rect.MakeXYWH(x0, y0, wplate, h), paint((4, 7, 11), 190))
    c.drawRect(skia.Rect.MakeXYWH(x0, y0, 4, h), paint(accent))
    a = 255 * ramp(t, start + 0.18, 0.35) * (1 - t_out)
    text(c, title, x0 + 20, y0 + 28, tf, TEXT, a)
    text(c, sub, x0 + 20, y0 + 49, sf, TEXT2, a, spacing=1.2)
    c.restore()


# ---- transitions: draw image `b` over `a` with progress p in 0..1 ---------------

def put(c, image, x=0.0, y=0.0, alpha=1.0):
    """Draw a full-frame scene image (rendered at device resolution) at x, y in 800x480 units."""
    c.drawImageRect(image, skia.Rect.MakeXYWH(x, y, W, H), skia.SamplingOptions(skia.FilterMode.kLinear),
                    skia.Paint(Alphaf=alpha))


def tr_fade(c, a, b, p):
    put(c, a)
    put(c, b, alpha=ease_io(p))


def tr_wipe_diag(c, a, b, p):
    """A slanted edge sweeps left to right with a bright leading line."""
    put(c, a)
    e = ease_io(p)
    x = -260 + e * (W + 520)
    path = skia.Path()
    path.moveTo(-10, -10)
    path.lineTo(x + 130, -10)
    path.lineTo(x - 130, H + 10)
    path.lineTo(-10, H + 10)
    path.close()
    c.save()
    c.clipPath(path, True)
    put(c, b)
    c.restore()
    c.drawLine(x + 130, -10, x - 130, H + 10, paint(AZURE, 230 * math.sin(math.pi * p), stroke=3))


def tr_push(c, a, b, p):
    """The new scene pushes the old one off to the left."""
    e = ease_io(p)
    put(c, a, -W * e)
    put(c, b, W * (1 - e))


def tr_iris(c, a, b, p):
    put(c, a)
    r = ease_io(p) * 560
    path = skia.Path()
    path.addCircle(W / 2, H / 2, r)
    c.save()
    c.clipPath(path, True)
    put(c, b)
    c.restore()
    c.drawCircle(W / 2, H / 2, r, paint(AMBER, 220 * math.sin(math.pi * p), stroke=3))


def tr_blinds(c, a, b, p):
    """Vertical blinds, each slat opening slightly after its neighbour."""
    put(c, a)
    n = 10
    sw = W / n
    for i in range(n):
        k = ease_io(clamp(p * 1.6 - i * 0.06))
        if k <= 0:
            continue
        c.save()
        c.clipRect(skia.Rect.MakeXYWH(i * sw, 0, sw * k, H))
        put(c, b)
        c.restore()


def tr_flash(c, a, b, p):
    """Hard cut through a white flash."""
    put(c, a if p < 0.35 else b)
    c.drawRect(skia.Rect.MakeWH(W, H), paint((255, 255, 255), 255 * (1 - abs(p - 0.35) / 0.65) ** 2))


# ---- output -----------------------------------------------------------------

def ffmpeg_exe() -> str:
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    hits = glob.glob(os.path.expandvars(
        r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg_*\ffmpeg-*\bin\ffmpeg.exe"))
    if not hits:
        sys.exit("ffmpeg not found")
    return hits[0]


def new_surface():
    s = skia.Surface(W * SS, H * SS)
    s.getCanvas().scale(SS, SS)
    return s


class Writer:
    """Pipes supersampled RGBA frames to ffmpeg, which scales them and muxes the WAV."""

    def __init__(self, path: Path, wav: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.proc = subprocess.Popen(
            [ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-y",
             "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{W * SS}x{H * SS}", "-r", str(FPS), "-i", "-",
             "-i", str(wav), "-vf", f"scale={W}:{H}:flags=lanczos", "-c:v", "libx264", "-crf", "10",
             "-preset", "medium", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "256k", "-shortest", str(path)],
            stdin=subprocess.PIPE)

    def write(self, surface) -> None:
        arr = surface.makeImageSnapshot().toarray(colorType=skia.kRGBA_8888_ColorType)
        self.proc.stdin.write(arr.tobytes())

    def close(self) -> None:
        self.proc.stdin.close()
        if self.proc.wait() != 0:
            sys.exit("ffmpeg failed")


def remux_audio(mp4: Path, wav: Path) -> None:
    """Give an existing master a new soundtrack without rendering the picture again."""
    tmp = mp4.with_name(mp4.stem + ".remux.mp4")
    subprocess.run([ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-y", "-i", str(mp4), "-i", str(wav),
                    "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac", "-b:a", "256k", "-shortest",
                    str(tmp)], check=True)
    tmp.replace(mp4)


def write_wav(path: Path, stereo: np.ndarray, rate: int = 48000) -> None:
    import wave

    pcm = (np.clip(stereo, -1, 1) * 32767).astype("<i2")
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())

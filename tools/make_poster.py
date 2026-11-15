#!/usr/bin/env python3
"""Render the Cinema poster frame and save it as a baseline 4:2:0 JPEG.

The image is original, generated here: soft colour fields on the theme's
dark ground with the title set in IBM Plex. It exercises what JPEG finds
hard (smooth gradients, fine lines, saturated edges), so it doubles as a
decode-quality check.

Output: assets/media/poster.jpg (800x480, a multiple of the 16x16 MCU).
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONTS = ROOT / "assets" / "fonts" / "src"
OUT = ROOT / "assets" / "media" / "poster.jpg"
W, H = 800, 480


def glow(cx: float, cy: float, radius: float, rgb: tuple[int, int, int], gain: float) -> np.ndarray:
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    d2 = ((x - cx) ** 2 + (y - cy) ** 2) / (radius * radius)
    return np.exp(-d2)[..., None] * np.array(rgb, np.float32) * gain


def main() -> None:
    img = np.zeros((H, W, 3), np.float32)
    img += np.array([8, 12, 18], np.float32)
    img += glow(170, 120, 300, (76, 181, 255), 0.55)    # azure
    img += glow(690, 400, 260, (255, 177, 61), 0.50)    # amber
    img += glow(560, 60, 200, (181, 140, 255), 0.35)    # violet
    # fine diagonal lines: detail that survives or smears
    y, x = np.mgrid[0:H, 0:W]
    lines = (((x + y) % 24) == 0).astype(np.float32)[..., None]
    img += lines * 14.0
    # vignette
    v = 1.0 - 0.55 * (((x - W / 2) / (W / 2)) ** 2 + ((y - H / 2) / (H / 2)) ** 2)[..., None]
    img = np.clip(img * np.clip(v, 0.25, 1.0), 0, 255).astype(np.uint8)

    im = Image.fromarray(img, "RGB")
    d = ImageDraw.Draw(im)
    big = ImageFont.truetype(str(FONTS / "IBMPlexSansCondensed-SemiBold.ttf"), 150)
    mid = ImageFont.truetype(str(FONTS / "IBMPlexSans-Medium.ttf"), 30)
    mono = ImageFont.truetype(str(FONTS / "IBMPlexMono-Medium.ttf"), 16)
    d.text((56, 120), "Cinema", font=big, fill=(233, 238, 245))
    d.text((62, 300), "Hardware JPEG decode, colour conversion by DMA2D", font=mid, fill=(152, 166, 184))
    d.text((62, 410), "800 x 480   4:2:0   STM32N657", font=mono, fill=(95, 109, 128))
    d.rectangle((62, 286, 62 + 96, 289), fill=(76, 181, 255))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    # Baseline, 4:2:0, standard Huffman tables: what the hardware codec accepts.
    im.save(OUT, "JPEG", quality=90, subsampling=2, optimize=False, progressive=False)
    print(f"{OUT} {OUT.stat().st_size / 1024:.1f} KB")


if __name__ == "__main__":
    main()

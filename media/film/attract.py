#!/usr/bin/env python3
"""The attract lap's clip: the film up to its last scene, the demos, the
film's coda, under one piece of music.

Attract mode plays this clip from the microSD card (/SD:/clips/_attract.n6v;
a leading underscore keeps a file out of Cinema's list) and changes screens
on its bar lines. LAP in app/src/main.c lists the same screens and bars, and
the firmware checks the clip's length against it.

The picture is the film's up to bar 18 (0:43.2), black while the demos show
(nobody sees it: the clip only plays their sound), then the film's last two
bars. After Signal Lab, Cinema shows four bars of it: the Camera screen
tracking two hands, recorded by the board itself (tools/record.py --camera)
and cut to 288 pictures, with a plate saying so (the lap does not open the
camera; a recording shown as if live would be fake data). That recording,
media/out/gestures.mkv, is camera footage: the user's hands, checked by the
user for faces, never committed. The sound is the film's score at lap length: the film's own music to
0:43.2 (checked here, sample for sample), its groove carried on under the
demos with a whoosh at every screen change and a break of pad and bass under
the theremin, then the film's coda on the film's last two bars. The demos
take a whole number of four-bar chord cycles, so the coda lands on the
film's own chords.

Usage: attract.py     (needs media/out/n6_film.mp4 and .wav: run n6_film.py once;
                       and media/out/gestures.mkv, 288 pictures of the Camera screen)
       then: tools/n6v.py upload media/out/_attract.n6v
"""
from __future__ import annotations

import subprocess
import sys
import wave
from pathlib import Path

import numpy as np
import skia

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / "tools"))
import score  # noqa: E402
from kit import AZURE, FPS, H, OUT, ROOT, TEXT, W, font, new_surface, paint, text, write_wav  # noqa: E402
from n6_film import T, TOTAL  # noqa: E402
from n6v import ffmpeg_exe  # noqa: E402

BAR = score.BAR
FILM_BARS = 18      # the film up to its last scene: 0:43.2
CODA_BARS = 2       # the film's last scene and the score's coda
RECORDED = "Gestures"  # not a screen: Cinema shows the clip's picture, a recording of the Camera screen
DEMOS = [("Home", 2), ("Flight Deck", 4), ("Signal Lab", 3), (RECORDED, 4), ("Explorer", 8),
         ("Host Link", 4), ("Core Map", 6), ("Under the Hood", 5)]
BREAK_UNDER = "Signal Lab"  # pad and bass only: the ghost plays the theremin over it
GESTURES = OUT / "gestures.mkv"  # camera footage (the user's hands): never committed
CAPTION = "RECORDED ON THIS BOARD"
VIEW_W, VIEW_BOTTOM = 592, 480  # the Camera screen's picture: x 0..592, down to the panel's bottom


def caption_png(path: Path) -> None:
    """The plate over the recording's quietest corner (bottom right of the camera view, measured:
    the least motion there), in the films' lower-third style."""
    s = new_surface()
    c = s.getCanvas()
    c.clear(skia.Color(0, 0, 0, 0))
    f = font("mono", 12.5)
    wplate = f.measureText(CAPTION) + len(CAPTION) * 1.2 + 36
    h = 30
    x0, y0 = VIEW_W - 12 - wplate, VIEW_BOTTOM - 12 - h
    c.drawRect(skia.Rect.MakeXYWH(x0, y0, wplate, h), paint((4, 7, 11), 190))
    c.drawRect(skia.Rect.MakeXYWH(x0, y0, 4, h), paint(AZURE))
    text(c, CAPTION, x0 + 20, y0 + 19.5, f, TEXT, spacing=1.2)
    s.makeImageSnapshot().save(str(path), skia.kPNG)


def lap() -> tuple[float, list[float], tuple[int, ...], list[tuple[str, int]]]:
    """Length, cut times, break bars and each demo's first bar."""
    demo_bars = sum(n for _, n in DEMOS)
    if demo_bars % 4:
        sys.exit(f"the demos take {demo_bars} bars: make it a multiple of 4, or the coda leaves the film's chords")
    starts, bar = [], FILM_BARS
    for name, n in DEMOS:
        starts.append((name, bar))
        bar += n
    coda = bar
    total = (coda + CODA_BARS) * BAR
    # the film's own cuts (its last one, 0:43.2, is the cut to the first demo), every demo change, the coda
    cuts = list(T[2:-1]) + [b * BAR for _, b in starts[1:]] + [coda * BAR]
    first = dict(starts)[BREAK_UNDER]
    breaks = tuple(range(first, first + dict(DEMOS)[BREAK_UNDER]))
    return total, cuts, breaks, starts + [("coda", coda)]


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        return np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2)


def main() -> None:
    film_mp4, film_wav = OUT / "n6_film.mp4", OUT / "n6_film.wav"
    if not film_mp4.exists() or not film_wav.exists():
        sys.exit(f"{film_mp4} or {film_wav} missing: run media/film/n6_film.py once")
    if not GESTURES.exists():
        sys.exit(f"{GESTURES} missing: the Camera screen recorded by the board (record.py --camera), "
                 f"cut to {dict(DEMOS)[RECORDED] * 72} pictures")
    total, cuts, breaks, starts = lap()
    for name, b in starts:
        print(f"  {b * BAR:6.1f} s  bar {b:2d}  {name}")
    print(f"lap {total:.1f} s ({total / BAR:.0f} bars), break under the theremin: bars {breaks[0]}..{breaks[-1]}")

    wav = OUT / "n6_attract.wav"
    write_wav(wav, score.render(total, cuts, T[1], breaks))
    lap_pcm, film_pcm = read_wav(wav), read_wav(film_wav)
    n = int(FILM_BARS * BAR * score.SR)
    diff = int(np.abs(lap_pcm[:n].astype(np.int32) - film_pcm[:n]).max())
    print(f"the lap's first {FILM_BARS * BAR:.1f} s against the film's soundtrack: largest difference {diff} LSB")
    if diff:
        sys.exit("the film's part of the lap is not the film's music")

    # the picture: the film to bar 18, black under the demos but the recorded gestures with their
    # plate, the film's last two bars; counted in frames (a bar is 72)
    bar_f = round(BAR * FPS)
    f_cut, f_end = FILM_BARS * bar_f, round(TOTAL * FPS)
    g_at, g_n = dict(starts)[RECORDED] * bar_f, dict(DEMOS)[RECORDED] * bar_f
    n_g = int(subprocess.run([str(Path(ffmpeg_exe()).with_name("ffprobe" + Path(ffmpeg_exe()).suffix)), "-v",
                              "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                              "stream=nb_read_frames", "-of", "csv=p=0", str(GESTURES)],
                             capture_output=True, text=True, check=True).stdout.strip())
    if n_g != g_n:
        sys.exit(f"{GESTURES} has {n_g} pictures, the lap's {RECORDED} bars take {g_n}")
    black1 = g_at - f_cut
    black2 = round(total * FPS) - (f_end - f_cut) - f_cut - black1 - g_n
    cap = OUT / "gestures_caption.png"
    caption_png(cap)
    black = f"color=c=black:s={W}x{H}:r={FPS},format=yuv420p,setsar=1"
    graph = (f"[0:v]split[v1][v2];"
             f"[v1]trim=end_frame={f_cut},setpts=PTS-STARTPTS,format=yuv420p,setsar=1[a];"
             f"{black},trim=end_frame={black1}[b1];"
             f"[2:v]scale={W}:{H}:flags=lanczos,format=rgba[cap];"
             f"[1:v]setpts=PTS-STARTPTS[g0];[g0][cap]overlay=0:0:shortest=1,format=yuv420p,setsar=1[g];"
             f"{black},trim=end_frame={black2}[b2];"
             f"[v2]trim=start_frame={f_cut}:end_frame={f_end},setpts=PTS-STARTPTS,format=yuv420p,setsar=1[c];"
             f"[a][b1][g][b2][c]concat=n=5:v=1:a=0[v]")
    mkv = OUT / "n6_attract.mkv"
    subprocess.run([ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-y", "-i", str(film_mp4),
                    "-i", str(GESTURES), "-loop", "1", "-i", str(cap),
                    "-filter_complex", graph, "-map", "[v]", "-r", str(FPS), "-c:v", "libx264", "-qp", "0",
                    "-preset", "ultrafast", str(mkv)], check=True)
    print(f"picture: film {f_cut}, black {black1}, recorded gestures {g_n} (from bar {g_at // bar_f}), "
          f"black {black2}, coda {f_end - f_cut} frames")
    clip = OUT / "_attract.n6v"
    subprocess.run([sys.executable, str(ROOT / "tools" / "n6v.py"), "encode", str(mkv), "--audio", str(wav),
                    "-o", str(clip), "--title", "N6 attract lap", "--q", "4"], check=True)
    mkv.unlink()


if __name__ == "__main__":
    main()

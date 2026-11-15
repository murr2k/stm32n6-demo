#!/usr/bin/env python3
"""Unattended review of the running demo.

Visits every screen, lets it run, records frame statistics and a screenshot,
then exercises the multi-touch paths with injected contacts (Touch Lab,
Explorer, Core Map), checks that touch releases never change screen, runs a
set of screen transitions, audio and video playback from flash, an upload to
the microSD card with playback and seeks from it, and one lap of attract
mode. host/companion.py is started for the run so Host Link and Core Map
show live data. Output goes to captures/tour/. Takes about seven minutes.

Usage: tour.py [--host IP] [--screens home,deck,...] [--no-touch] [--no-media] [--no-attract]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from acap import capture, clicks  # noqa: E402
from ctl import DEFAULT_HOST, send  # noqa: E402
from shot import grab  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "captures" / "tour"
SCREENS = ["home", "deck", "siglab", "explorer", "touchlab", "hostlink", "coremap", "cinema", "specimen"]


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


def where(host: str) -> tuple[str, bool]:
    """The screen that is up and whether attract mode is running."""
    v = {k: int(x) for k, x in re.findall(r"(\w+)=(-?\d+)", send(host, ["where"])[0])}
    return SCREENS[v["screen"]], bool(v["attract"])


def kv(host: str, cmd: str) -> dict[str, float]:
    return {k: float(v) for k, v in re.findall(r"(\w+)=([\d.]+)", send(host, [cmd])[0])}


def use_card(host: str, on: bool) -> None:
    """List the card's clips or only the flash ones, and wait until the gallery has followed."""
    send(host, [f"card {int(on)}"])
    for _ in range(40):
        time.sleep(0.25)
        if (media(host)["card_clips"] > 0) == on:
            break
    time.sleep(1.0)  # thumbnails


def video_test(host: str) -> None:
    send(host, [f"screen {SCREENS.index('cinema')}"])
    time.sleep(1.0)
    send(host, ["tap 140 200"])  # first clip card
    time.sleep(2.0)
    send(host, ["reset"])
    v0, g0, t0 = kv(host, "video"), stats(host), time.time()
    time.sleep(6.0)
    v1, g1, t1 = kv(host, "video"), stats(host), time.time()
    source = "card" if v1.get("card") else "flash"
    grab(host).save(OUT / f"v1-cinema-playing-{source}.png")
    if v1.get("open"):
        print(f"video from {source}: {(v1['frame'] - v0['frame']) / (t1 - t0):.2f} fps over {t1 - t0:.1f} s, "
              f"decode {v1['decode_us'] / 1000:.1f} ms, convert {v1['convert_us'] / 1000:.1f} ms, "
              f"{v1['kbps'] / 1000:.1f} Mbit/s, player dropped {int(v1['dropped'] - v0['dropped'])}, "
              f"UI dropped {int(g1['dropped'] - g0['dropped'])}, stalls {int(v1.get('stalls', 0))}, "
              f"clock {'audio' if v1['audio_clock'] else 'system'}")
    else:
        print("video: no clip opened (is the media partition flashed, or a clip on the card?)")
    send(host, ["tap 400 240", "sleep 0.4", "tap 40 26"])  # show the overlay, then Back
    time.sleep(0.8)


def media_tests(host: str) -> None:
    """Audio sample clock and video playback rate, measured against the host clock."""
    a0, t0 = kv(host, "audio"), time.time()
    time.sleep(4.0)
    a1, t1 = kv(host, "audio"), time.time()
    if a1.get("ok"):
        rate = (a1["blocks"] - a0["blocks"]) * 512 / (t1 - t0)
        print(f"audio: DAC clock {rate / 1000:.2f} kHz (nominal 48.00), underruns {int(a1['errors'])}")
    else:
        print("audio: not running")

    # The first clip, from flash and then, if the card holds a copy of it (same
    # title), from the card: "card 0" takes the card's clips out of the list.
    on_card = media(host).get("card_clips", 0) > 0
    try:
        if on_card:
            use_card(host, False)
        video_test(host)
        if on_card:
            use_card(host, True)
            if media(host)["first_card"] == 0:
                video_test(host)
    finally:
        if on_card:
            send(host, ["card 1"])

    # Opening a clip and leaving it again (Back while a frame is being decoded)
    # must not cost the UI a frame either.
    send(host, ["reset"])
    g0 = stats(host)
    for _ in range(3):
        send(host, ["tap 140 200", "sleep 1.3", "tap 40 26", "sleep 0.7"])
    g1 = stats(host)
    print(f"open and close a clip, 3 times: frame worst {g1['render_worst_us'] / 1000:.2f} ms, "
          f"dropped {int(g1['dropped'] - g0['dropped'])}, now on {where(host)[0]}")
    scrub_sound(host)


def scrub_sound(host: str) -> None:
    """What the DAC gets during a drag on the slider, a pause and a resume, read
    back from the board: every jump has to be a fade, not a step."""
    send(host, ["tap 140 200", "sleep 1.3"])  # the controls are up for 3 s after opening
    drag = [c for i in range(40) for c in (f"touch 0 {200 + 6 * i} 444 1", "sleep 0.03")]
    send(host, ["reset"])
    g0, v0 = stats(host), kv(host, "video")
    pcm = capture(host, 2.6, drag + ["touch 0 440 444 0", "sleep 0.3", "tap 40 444", "sleep 0.3", "tap 40 444"])
    g1, v1 = stats(host), kv(host, "video")
    send(host, ["screen 0", "sleep 0.6", f"screen {SCREENS.index('cinema')}", "sleep 0.8"])
    loud = sum(1 for l, _ in pcm if abs(l) > 200) * 100 // max(len(pcm), 1)
    print(f"scrub sound: {len(pcm) / 48000:.1f} s recorded over a drag, a pause and a resume: "
          f"discontinuities {len(clicks(pcm))}, signal in {loud} % of it, frame worst "
          f"{g1['render_worst_us'] / 1000:.2f} ms, UI dropped {int(g1['dropped'] - g0['dropped'])}, "
          f"stalls {int(v1.get('stalls', 0) - v0.get('stalls', 0))}")


def media(host: str) -> dict[str, int]:
    return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", send(host, ["media"])[0])}


def card_tests(host: str) -> None:
    """Clips on the microSD card: an upload, playback through the read-ahead window, seeks."""
    import n6v

    m = media(host)
    if not m.get("mounted"):
        print("card: no microSD card mounted, skipped")
        return
    clip = OUT / "card-test.n6v"
    if not clip.exists():
        # an 8 s test pattern with a tone, made once: nothing in the repository is needed
        subprocess.run([sys.executable, str(ROOT / "tools" / "n6v.py"), "encode", "testsrc2=size=800x480:rate=30",
                        "--lavfi", "--duration", "8", "--audio-lavfi", "sine=frequency=440:sample_rate=48000",
                        "--title", "Card test", "-o", str(clip)], capture_output=True)
    if clip.exists():
        # Uploaded while Signal Lab runs: the upload itself must not cost the UI a frame.
        # The gallery rebuild that follows decodes the new clip's thumbnail on the
        # UI thread and is allowed to; it is counted separately.
        send(host, [f"screen {SCREENS.index('siglab')}"])
        time.sleep(1.5)
        send(host, ["reset"])
        g0, a0 = stats(host), kv(host, "audio")
        name, size, secs = n6v.upload(host, [str(clip)], quiet=True)[0]
        g1 = stats(host)
        time.sleep(2.0)
        g2, a1 = stats(host), kv(host, "audio")
        print(f"card upload: {name}, {size / 1e6:.1f} MB at {size / 1e6 / secs:.2f} MB/s, checksum verified; "
              f"UI dropped {int(g1['dropped'] - g0['dropped'])} while receiving, "
              f"{int(g2['dropped'] - g1['dropped'])} in the gallery rebuild, "
              f"audio underruns {int(a1.get('errors', 0) - a0.get('errors', 0))}")
        m = media(host)
    else:
        print("card upload: could not make the test clip (is ffmpeg installed?), skipped")
    if m["first_card"] < 0:
        print("card: no clips on it, playback skipped")
        return
    # play the test clip if it is there, else the first clip that is read from the card
    index = m["first_card"]
    for i in range(m["clips"]):
        if send(host, [f"clip {i}"])[0].split()[0] == f"file={n6v.card_name(clip)}":
            index = i
    try:
        card_playback(host, m, index)
    finally:
        if clip.exists():
            # leave the card as it was: the test clip does not belong in the gallery
            card = n6v.Card(host)
            r = card.ask(f"del {n6v.card_name(clip)}")[1]
            card.close()
            time.sleep(1.5)
            print(f"card cleanup: test clip removed ({r}), {media(host)['clips']} clips listed")


def card_playback(host: str, m: dict[str, int], index: int) -> None:
    # the test clip is 8 s long: measure and start seeking before it ends
    send(host, [f"play {index}"])
    time.sleep(1.5)
    first_us = kv(host, "video").get("seek_us", 0)
    send(host, ["reset"])
    v0, g0, t0 = kv(host, "video"), stats(host), time.time()
    time.sleep(4.0)
    v1, g1, t1 = kv(host, "video"), stats(host), time.time()
    grab(host).save(OUT / "v2-cinema-card-playing.png")
    if not (v1.get("open") and v1.get("card")):
        print("card video: the clip did not open from the card")
        return
    print(f"card video: {(v1['frame'] - v0['frame']) / (t1 - t0):.2f} fps over {t1 - t0:.1f} s, "
          f"{v1['kbps'] / 1000:.1f} Mbit/s, player dropped {int(v1['dropped'] - v0['dropped'])}, "
          f"UI dropped {int(g1['dropped'] - g0['dropped'])}, stalls {int(v1['stalls'])}, "
          f"read ahead {v1['ahead_kb'] / 1024:.1f} MB, open to first picture {first_us / 1000:.0f} ms")

    # Seeks: the overlay is hidden by now, so one tap shows it, then taps on the slider.
    # Each lands outside what is buffered or inside it; either way the picture must come.
    n = int(v1["n_frames"])
    send(host, ["reset", "tap 400 240", "sleep 0.4"])
    g0 = stats(host)
    times = []
    for x in (520, 110, 380, 560, 90):
        send(host, [f"tap {x} 444", "sleep 0.5"])
        v = kv(host, "video")
        want = (x - 86) * (n - 1) // 544
        ok = want - 2 <= v["frame"] <= want + 40 and not v["buffering"]
        times.append(f"{v['seek_us'] / 1000:.0f}{'' if ok else '!'}")
    time.sleep(1.0)
    v2, g2 = kv(host, "video"), stats(host)
    print(f"card seeks: picture after {', '.join(times)} ms (a ! marks a seek that did not arrive), "
          f"stalls {int(v2['stalls'])}, UI dropped {int(g2['dropped'] - g0['dropped'])}, "
          f"{'playing' if v2['playing'] else 'NOT playing'} afterwards")
    send(host, ["tap 40 26"])  # Back (the overlay is up after the seeks)
    time.sleep(0.5)

    if m["clips"] > 3:
        send(host, ["reset"])
        g0 = stats(host)
        send(host, ["swipe 600 180 250 180 250"])
        time.sleep(1.2)
        g1 = stats(host)
        grab(host).save(OUT / "v3-cinema-gallery-scrolled.png")
        send(host, ["swipe 250 180 600 180 250"])
        time.sleep(1.0)
        print(f"gallery scroll: {m['clips']} clips, frame worst {g1['render_worst_us'] / 1000:.2f} ms, "
              f"dropped {int(g1['dropped'] - g0['dropped'])}, still on {where(host)[0]}")


def transition_test(host: str) -> None:
    """Screen changes: the fade is done by the LTDC, so none may cost a frame."""
    send(host, ["screen 0"])
    time.sleep(1.5)
    send(host, ["reset"])
    before = stats(host)
    order = [1, 2, 3, 5, 6, 0, 7, 4, 1, 0]
    for i in order:
        send(host, [f"screen {i}"])
        time.sleep(0.9)
    s = stats(host)
    print(f"transitions: {len(order)} screen changes, frame worst {s['render_worst_us'] / 1000:.2f} ms, "
          f"dropped {int(s['dropped'] - before['dropped'])}")


def attract_test(host: str, secs: float) -> None:
    """Attract mode, one lap: every screen it shows, Explorer's zoom tour and the film.

    No screenshots here: reading the frame buffer back costs the UI one frame
    (an 18 ms copy to PSRAM), which would be counted as a drop.
    """
    send(host, ["screen 0"])
    time.sleep(1.5)
    send(host, ["reset"])
    before, a0 = stats(host), kv(host, "audio")
    send(host, ["attract"])
    time.sleep(secs)
    s, a1 = stats(host), kv(host, "audio")
    send(host, ["screen 0"])  # counts as input: ends attract mode
    print(f"attract mode: {secs:.0f} s, frame worst {s['render_worst_us'] / 1000:.2f} ms, "
          f"dropped {int(s['dropped'] - before['dropped'])}, "
          f"audio underruns {int(a1.get('errors', 0) - a0.get('errors', 0))}")


def home_pan_test(host: str) -> None:
    """Home with more tiles than fit on the screen (five placeholders after the
    real ones): the field pans under a finger, rests on a column, a tile that
    has moved still opens its own screen, and a drag from the screen edge is
    still a change of screen."""
    def home() -> dict[str, int]:
        return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", send(host, ["home"])[0])}

    col = 195  # one tile and its gap
    try:
        send(host, ["screen 0", "sleep 0.8", "hometiles 5", "sleep 0.6", "reset"])
        g0, h0 = stats(host), home()
        send(host, ["swipe 560 240 300 240 400"])  # a slow drag of 260 px
        time.sleep(1.2)
        h1 = home()
        send(host, ["swipe 600 240 250 240 120"])  # a flick: carries on to the end
        time.sleep(1.5)
        h2, g1 = home(), stats(host)
        grab(host).save(OUT / "h1-home-panned.png")
        first = h2["scroll"] // col                # the column now at the left edge
        expect = SCREENS[first + 1] if first < 4 else "home"  # its top tile; a placeholder opens nothing
        send(host, ["tap 100 112"])
        time.sleep(1.0)
        tapped = where(host)[0]
        send(host, ["screen 0", "sleep 0.8"])
        h3 = home()
        send(host, ["swipe 795 240 560 240 160"])  # from the edge
        time.sleep(0.8)
        edge = where(host)[0]
        send(host, ["screen 0", "sleep 0.8"])
        h4 = home()
    finally:
        send(host, ["hometiles 0", "sleep 0.5"])
    h5 = home()
    rests = "on a column" if h1["scroll"] % col == 0 and h2["scroll"] % col == 0 else "NOT on a column"
    print(f"home panning: {h0['tiles']} tiles, range {h0['max']} px; a 260 px drag rests at {h1['scroll']}, "
          f"a flick at {h2['scroll']} ({rests}); the tile now at the left opens {tapped} (expected {expect}); "
          f"back on Home the field is at {h3['scroll']}; a drag from the edge goes to {edge} "
          f"(expected {SCREENS[1]}) and leaves the field at {h4['scroll']}; frame worst "
          f"{g1['render_worst_us'] / 1000:.2f} ms, dropped {int(g1['dropped'] - g0['dropped'])}; "
          f"without placeholders: {h5['tiles']} tiles, range {h5['max']} px")


def touch_tests(host: str) -> None:
    home_pan_test(host)

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
    print(f"edge swipe from Flight Deck: now on {where(host)[0]} (expected {SCREENS[2]})")

    # Core Map: orbit with one finger, pinch to zoom, tap a post, double tap to reset.
    send(host, [f"screen {SCREENS.index('coremap')}"])
    time.sleep(1.5)
    send(host, ["reset"])
    before = stats(host)
    send(host, ["touch 0 250 250 1"])
    for i in range(80):
        send(host, [f"touch 0 {250 + i * 4} {250 + (i % 30) - 15} 1"])
        time.sleep(0.012)
    send(host, ["touch 0 0 0 0"])
    time.sleep(0.8)
    send(host, ["touch 0 380 300 1", "touch 1 420 300 1"])
    for i in range(50):
        send(host, [f"touch 0 {380 - i * 5} 300 1", f"touch 1 {420 + i * 5} 300 1"])
        time.sleep(0.012)
    send(host, ["touch 0 0 0 0", "touch 1 0 0 0"])
    time.sleep(0.4)
    s = stats(host)
    grab(host).save(OUT / "t5-coremap-orbited-zoomed.png")
    send(host, ["tap 700 150", "sleep 0.1", "tap 700 150", "sleep 1.5", "tap 420 300", "sleep 0.5"])
    grab(host).save(OUT / "t6-coremap-reset-selected.png")
    print(f"core map gestures: frame worst {s['render_worst_us'] / 1000:.2f} ms, "
          f"dropped {int(s['dropped'] - before['dropped'])}, still on {where(host)[0]}")

    # A touch release must never change the screen or start attract mode (it
    # did, about one release in five, when the idle timer wrapped).
    home = SCREENS.index("touchlab")
    send(host, [f"screen {home}"])
    time.sleep(1.0)
    stray = 0
    for i in range(80):
        send(host, [f"tap {150 + (i * 37) % 500} {150 + (i * 53) % 250}", "sleep 0.05"])
        name, attract = where(host)
        if name != "touchlab" or attract:
            stray += 1
            send(host, [f"screen {home}", "sleep 0.8"])
    print(f"touch releases: 80, stray screen changes or attract starts {stray}")
    send(host, ["screen 0"])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--screens", default=",".join(SCREENS))
    ap.add_argument("--no-touch", action="store_true")
    ap.add_argument("--no-media", action="store_true")
    ap.add_argument("--no-attract", action="store_true")
    ap.add_argument("--no-companion", action="store_true", help="do not start host/companion.py for the run")
    ap.add_argument("--run", type=float, default=5.0, help="seconds measured per screen")
    ap.add_argument("--attract", type=float, default=150.0, help="seconds of attract mode (one lap is about 140 with the companion running)")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    companion = None
    if not a.no_companion:
        # Host Link and Core Map are measured with live data, not their waiting state
        companion = subprocess.Popen([sys.executable, str(ROOT / "host" / "companion.py"), "--host", a.host],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.5)
    try:
        run(a)
    finally:
        if companion:
            companion.terminate()


def run(a: argparse.Namespace) -> None:
    for name in a.screens.split(","):
        visit(a.host, SCREENS.index(name), name, settle=2.0, run=a.run)
    if not a.no_touch:
        touch_tests(a.host)
    transition_test(a.host)
    if not a.no_media:
        media_tests(a.host)
        card_tests(a.host)
    if not a.no_attract:
        attract_test(a.host, a.attract)
    send(a.host, ["screen 0"])


if __name__ == "__main__":
    main()

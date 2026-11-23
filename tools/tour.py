#!/usr/bin/env python3
"""Unattended review of the running demo.

Visits every screen, lets it run, records frame statistics and a screenshot,
then exercises the multi-touch paths with injected contacts (Touch Lab,
Explorer, Core Map), plays Signal Lab's theremin and records its sound,
drags the microphone monitor's equalizer bands,
checks that touch releases never change screen, runs a
set of screen transitions, drives the web remote (http/80: its page, its
state, its actions and its guard), audio and video playback from flash, an upload to
the microSD card with playback and seeks from it, the card taken out and put
back (acted out: idle, playing, during the attract lap and an upload), and
one lap of attract mode. host/companion.py is started for the run so Host Link and Core Map
show live data. Output goes to captures/tour/. Takes about seven minutes.

The board knows the tour runs ("tour begin", a "tour step" per part, "tour
end"), and its web remote outranks it: anything picked on the remote stops
the tour (the board refuses its next command and it ends here, removing its
test clip from the card if it got that far). The remote can also start a
tour: host/companion.py runs this script with --no-companion.

Usage: tour.py [--host IP] [--screens home,deck,...] [--no-touch] [--no-media] [--no-attract]
"""
from __future__ import annotations

import argparse
import gzip
import json
import math
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ctl  # noqa: E402
from acap import capture, clicks  # noqa: E402
from ctl import DEFAULT_HOST  # noqa: E402
from shot import grab  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "captures" / "tour"
TEST_CLIP = "card-test.n6v"  # on the card while card_tests runs


class Stopped(Exception):
    """The board's web remote took the board back: from then on the board
    refuses the tour's commands ("err stopped from the web remote")."""


def send(host: str, commands: list[str]) -> list[str]:
    """ctl.send, with a stop from the web remote turned into Stopped."""
    replies = ctl.send(host, commands)
    if any(r.startswith("err stopped") for r in replies):
        raise Stopped()
    return replies


def step(host: str, name: str) -> None:
    """Tell the board what the tour does now (the web remote shows it)."""
    send(host, [f"tour step {name}"])


def wait(host: str, secs: float) -> None:
    """A long wait that keeps saying "tour alive": the board gives up a tour
    whose control port is silent for 30 s, and a stopped one finds out here."""
    end = time.monotonic() + secs
    while (left := end - time.monotonic()) > 0:
        time.sleep(min(5.0, left))
        send(host, ["tour alive"])
SCREENS = ["home", "deck", "siglab", "explorer", "touchlab", "hostlink", "coremap", "cinema", "specimen", "settings", "camera",
           "power"]  # Home Power: on Home and in the swipes only while the energy monitor answers


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
        rate = (a1["blocks"] - a0["blocks"]) * a1.get("block_frames", 512) / (t1 - t0)
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


def theremin_test(host: str) -> None:
    """Signal Lab's theremin played with injected touches: what the DAC gets
    through a glide across the field, a two-finger chord and the lifts (every
    change has to be a ramp), the pitch and level a held touch plays against
    its column and height, touch to sound, a finger that leaves the field
    (it keeps its level until it lifts) or comes down off it (it does not
    play), the SOUND switch, and the frames meanwhile."""
    def thm() -> dict[str, str]:
        return {k: v for k, v in re.findall(r"(\w+)=([-\w/]+)", send(host, ["thm"])[0])}

    def voice(st: dict[str, str]) -> tuple[int, ...]:
        return tuple(int(x) for x in st["v0"].split("/"))  # on, held, hz, db

    sound_was = thm()["sound"]  # the SOUND switch is the user's: put back afterwards
    send(host, [f"screen {SCREENS.index('siglab')}", "sleep 1.2", "sigsrc 2", "sleep 1.2", "thmwave 1", "thmsnap 0",
                "thmsound 1"])
    play = [c for i in range(40) for c in (f"touch 0 {100 + 15 * i} {200 + i} 1", "sleep 0.03")]
    play += ["touch 1 400 300 1", "sleep 0.5", "touch 0 700 239 0", "sleep 0.3", "touch 1 400 300 0", "sleep 0.6",
             "touch 0 300 180 1", "sleep 0.15", "touch 0 300 180 0"]
    send(host, ["reset"])
    g0 = stats(host)
    pcm = capture(host, 3.0, play)
    # a held touch: column 400 (x 416), y 204 (the spectrum pane is 252 px tall from y 144)
    send(host, ["touch 0 416 204 1", "sleep 0.5"])
    st = thm()
    grab(host).save(OUT / f"{SCREENS.index('siglab')}-siglab-theremin.png")
    # the same finger off the field's top (over the scope), then past its left end
    send(host, ["touch 0 416 110 1", "sleep 0.3"])
    above = voice(thm())
    send(host, ["touch 0 5 110 1", "sleep 0.3"])
    past = voice(thm())
    send(host, ["touch 0 5 110 0", "sleep 0.6"])
    # a finger that comes down over the scope and slides onto the field
    send(host, ["touch 0 416 110 1", "sleep 0.2", "touch 0 416 204 1", "sleep 0.3"])
    stray = voice(thm())
    send(host, ["touch 0 416 204 0", "sleep 0.6"])
    # SOUND off: the voice plays on screen and in the analysis, the DAC gets silence
    send(host, ["thmsound 0", "sleep 0.1"])
    muted = capture(host, 1.0, ["touch 0 416 204 1", "sleep 0.6"])
    shown = voice(thm())
    send(host, ["touch 0 416 204 0", "sleep 0.6"])
    g1 = stats(host)
    send(host, ["sigsrc 0", f"thmsound {sound_was}"])
    _, _, hz, db = voice(st)
    want_hz = 20 * 1000 ** (400 / 767)
    want_db = -6 + 10 * math.log2((394 - 204) / 249)  # the loudness taper: 10 dB a halving of the height
    loud = sum(1 for l, _ in pcm if abs(l) > 200) * 100 // max(len(pcm), 1)
    print(f"theremin: a glide, a chord and lifts: discontinuities {len(clicks(pcm))}, signal in {loud} % of it; "
          f"a touch at {want_hz:.0f} Hz, {want_db:.0f} dB played {hz} Hz, {db} dB; touch to sound last "
          f"{int(st['lat_last_us']) / 1000:.1f} ms, worst {int(st['lat_worst_us']) / 1000:.1f} ms; frame worst "
          f"{g1['render_worst_us'] / 1000:.2f} ms, dropped {int(g1['dropped'] - g0['dropped'])}")
    print(f"theremin: off the top {above[2]} Hz, {above[3]} dB (held {above[1]}); past the left end {past[2]} Hz, "
          f"{past[3]} dB; down off the field, slid on: held {stray[1]} (0 expected); SOUND off: playing "
          f"{shown[1]} at {shown[3]} dB, DAC peak {max((abs(l) for l, _ in muted), default=0)} (0 expected)")


def monitor_test(host: str) -> None:
    """Signal Lab's microphone monitor and its equalizer, with nothing to hear
    (no headphones needed): with MICROPHONE the monitor takes the audio output
    and keeps its read distance (no resync); a handle dragged by an injected
    finger moves its band (up is gain, across is frequency); a second finger
    beside it sets the width; a finger that comes down off the field moves
    nothing; the curve and handles cost no frame; leaving the screen gives the
    output back with LISTEN off."""
    def mon() -> dict[str, str]:
        return {k: v for k, v in re.findall(r"(\w+)=([-\w/]+)", send(host, ["mon"])[0])}

    def band(st: dict[str, str], b: int) -> tuple[int, ...]:
        return tuple(int(x) for x in st[f"b{b}"].split("/"))  # Hz, dB x10, Q x100

    def x_of(hz: float) -> int:  # panel x of a frequency: the pane starts at x 16, 768 columns, 20 Hz..20 kHz
        return round(16 + math.log(hz / 20) / math.log(1000) * 767)

    siglab = SCREENS.index("siglab")
    send(host, [f"screen {siglab}", "sleep 1.2", "sigsrc 1", "sleep 1.0", "monreset", "moneq 1", "monlisten 0",
                "mongain 30", "sleep 0.3"])
    # handle 3 (PEAK 3, 1500 Hz, 0 dB): the EQ's 0 dB line is the middle of the 252 px spectrum from y 144;
    # 110 px is 15 dB
    x0, y0 = x_of(1500), 144 + 126
    drag = [c for i in range(1, 31) for c in (f"touch 0 {x0 + 2 * i} {y0 - round(55 * i / 30)} 1", "sleep 0.03")]
    send(host, ["reset"])
    g0 = stats(host)
    send(host, [f"touch 0 {x0} {y0} 1", "sleep 0.05"] + drag + ["sleep 0.2"])
    dragged = band(mon(), 2)
    # a second finger 40 px to the right of the held handle, spread to 80 px: half the Q
    send(host, [f"touch 1 {x0 + 100} {y0 - 55} 1", "sleep 0.1", f"touch 1 {x0 + 120} {y0 - 55} 1", "sleep 0.05",
                f"touch 1 {x0 + 140} {y0 - 55} 1", "sleep 0.2"])
    widened = band(mon(), 2)
    send(host, [f"touch 1 {x0 + 140} {y0 - 55} 0", "sleep 0.05", f"touch 0 {x0 + 60} {y0 - 55} 0", "sleep 0.3"])
    # a finger that comes down over the scope and slides onto the handle
    send(host, [f"touch 0 {x0 + 60} 110 1", "sleep 0.1", f"touch 0 {x0 + 60} {y0 - 55} 1", "sleep 0.1",
                f"touch 0 {x0 + 60} {y0 - 90} 1", "sleep 0.2"])
    stray = band(mon(), 2)
    send(host, [f"touch 0 {x0 + 60} {y0 - 90} 0", "sleep 0.3"])
    g1 = stats(host)
    m = mon()
    grab(host).save(OUT / f"{siglab}-siglab-monitor.png")
    # leave with LISTEN on (gain 0 dB: no feedback through headphones lying by the board)
    send(host, ["mongain 0", "monlisten 1", "sleep 0.3", "screen 0", "sleep 0.6"])
    left = mon()
    send(host, ["mongain 30", "monreset", f"screen {siglab}", "sleep 1.0", "sigsrc 0", "sleep 0.3"])
    hz, db, q = dragged
    want_hz = 1500 * 1000 ** (60 / 767)
    print(f"monitor: read distance {m['lag']} samples (min {m['lag_min']}, max {m['lag_max']}), resyncs {m['resyncs']}, "
          f"delay {int(m['delay_us']) / 1000:.1f} ms, DSP worst {m['dsp_worst_us']} us a block; a dragged handle: "
          f"{hz} Hz, {db / 10:+.1f} dB (expected {want_hz:.0f} Hz, +7.5 dB); a second finger: Q {widened[2] / 100:.2f} "
          f"(expected 0.50); a finger from off the field: {'moved nothing' if stray == widened else f'moved it to {stray}'}; "
          f"frame worst {g1['render_worst_us'] / 1000:.2f} ms, dropped {int(g1['dropped'] - g0['dropped'])}; after "
          f"leaving: output taken {left['enabled']} (0 expected), LISTEN {left['listen']} (0 expected)")


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
    card_swap_test(host, clip)


def card_swap_test(host: str, clip: Path) -> None:
    """The card taken out and put back, acted out with ctl "sdcard out|in" (the
    firmware's card watcher behaves as if the detect switch had moved; no hands
    at the board): idle, with a clip from the card playing, during the attract
    lap, and in the middle of an upload. Each time the card must be let go of
    and come back (at High Speed, its clips listed again), the open clip must
    close, an interrupted upload must leave nothing on the card, and no frame
    or audio block may be lost. A card that came back only after a restart was
    the bug this guards against."""
    def sd() -> dict[str, str]:
        return dict(re.findall(r"(\w+)=(\S+)", send(host, ["sdcard"])[0]))

    def wait(cond, secs: float) -> float | None:
        t0 = time.time()
        while time.time() - t0 < secs:
            if cond():
                return time.time() - t0
            time.sleep(0.05)
        return None

    def t(x: float | None) -> str:
        return "NEVER" if x is None else f"{x:.2f} s"

    def card_files() -> list[str]:
        out = subprocess.run([sys.executable, str(ROOT / "tools" / "n6v.py"), "ls", "--host", host],
                             capture_output=True, text=True).stdout
        return [ln.split()[-1] for ln in out.splitlines() if " MB " in ln]

    n_card = -1
    for _ in range(20):  # a change just made to the card (the test clip deleted) must have reached the list
        n, n_card = n_card, media(host)["card_clips"]
        if n == n_card:
            break
        time.sleep(0.5)
    results = []

    def out_in(what: str, closed=None) -> None:
        send(host, ["sdcard out"])
        t_closed = wait(closed, 5.0) if closed else None
        t_out = wait(lambda: sd()["mounted"] == "0", 5.0)
        t_list = wait(lambda: media(host)["card_clips"] == 0, 5.0)
        send(host, ["sdcard in"])
        t_in = wait(lambda: sd()["mounted"] == "1", 10.0)
        t_back = wait(lambda: media(host)["card_clips"] == n_card, 10.0)
        s = sd()
        results.append(f"{what}: " + (f"closed in {t(t_closed)}, " if closed else "") +
                       f"let go of in {t(t_out)} (clips off the list {t(t_list)}), back in {t(t_in)}"
                       f"{'' if s.get('high_speed') == '1' else ' NOT at High Speed'}, "
                       f"its {n_card} clips listed {'again' if t_back is not None else 'NOT again'}")

    send(host, ["screen 0"])
    time.sleep(1.0)
    g0, a0 = stats(host), kv(host, "audio")
    try:
        out_in("idle")

        m = media(host)
        send(host, [f"play {m['first_card']}"])
        time.sleep(2.0)
        out_in("a clip from the card playing", lambda: kv(host, "video").get("open") == 0)
        send(host, ["screen 0"])
        time.sleep(1.0)

        send(host, ["attract"])
        time.sleep(4.0)
        out_in("the attract lap", lambda: send(host, ["where"])[0].find("lap_ms=-1") >= 0)
        send(host, ["screen 0"])
        time.sleep(1.0)

        if clip.exists():
            up = subprocess.Popen([sys.executable, str(ROOT / "tools" / "n6v.py"), "upload", "--host", host,
                                   str(clip)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            # taken out only once the board is receiving: earlier, the upload is refused, not cut off
            under_way = wait(lambda: media(host)["receiving"] == 1, 10.0)
            send(host, ["sdcard out"])
            try:
                text = up.communicate(timeout=30)[0]
            except subprocess.TimeoutExpired:
                up.kill()
                text = "did not end"
            stopped = under_way is not None and "checksum verified" not in text and "did not end" not in text
            wait(lambda: sd()["mounted"] == "0", 5.0)  # let go of first: else the old mount is the one listed
            send(host, ["sdcard in"])
            wait(lambda: sd()["mounted"] == "1", 10.0)
            wait(lambda: media(host)["card_clips"] == n_card, 10.0)
            left = [f for f in card_files() if f in ("upload.tmp", n6v_card_name(clip)) or f.endswith(".old")]
            last = ([ln.strip() for ln in re.split(r"[\r\n]", text) if ln.strip()] or ["no output"])[-1]
            results.append(f"an upload: " + (f"stopped ({last.split(': ', 1)[-1]})" if stopped else
                                             f"NOT stopped (under way: {under_way is not None}; {last})") +
                           f", {'nothing left on the card' if not left else 'LEFT ' + ', '.join(left)}")
    finally:
        send(host, ["sdcard auto", "screen 0"])
    time.sleep(1.0)
    g1, a1 = stats(host), kv(host, "audio")
    print("card taken out and put back (acted out): " + "; ".join(results) +
          f"; UI dropped {int(g1['dropped'] - g0['dropped'])}, "
          f"audio underruns {int(a1.get('errors', 0) - a0.get('errors', 0))}")


def n6v_card_name(clip: Path) -> str:
    import n6v

    return n6v.card_name(clip)


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


def entry_frames(host: str) -> list[dict[str, int]] | None:
    """The frame that built the screen after the last switch and the one after it,
    which draws what the first one left out (shell_entering() in ui/shell.h), from
    ctl swtrace (gfx.h): time left before its deadline, LVGL, the switch tail, the
    main loop, other threads, us; None if the trace has no such frames. Time left
    is None when no deadline applied ("u": the slot before presented nothing, as
    under a film's 30 fps, so the frame could not drop and its figure means
    nothing)."""
    d = dict(re.findall(r"(\w+)=(\S+)", send(host, ["swtrace"])[0]))
    out = []
    try:
        at = int(d["at_switch"])
        for raw in d["f"].split(",")[at:at + 2]:
            _, tail, pre, lv, away, left, _ = (int(x) for x in raw.rstrip("u").split("/"))
            out.append({"left": None if raw.endswith("u") else left, "lv": lv, "tail": tail, "pre": pre,
                        "away": away})
    except (KeyError, IndexError, ValueError):
        return None
    return out or None


def margins(host: str, label: str, line: str | None = None) -> None:
    """The board's frame margins since the last "margins reset" (ctl.margin_report),
    from its reply now or one read earlier."""
    m = ctl.margins(line or send(host, ["margins"])[0])
    if m:
        for line in ctl.margin_report(m, lambda i: SCREENS[i] if 0 <= i < len(SCREENS) else str(i)):
            print(f"  {label} {line}")


def transition_test(host: str, film: int = 6, homes: int = 3) -> None:
    """Screen changes: the fade is done by the LTDC, so none may cost a frame.

    Every screen is entered at least once (the Camera screen lends and takes
    back a frame buffer on the way), and the two tightest first frames of the
    attract lap straight from a playing film: the switch itself (closing the
    player) runs after a present, outside the next frame's render time but
    inside its budget. Explorer opens at the view its tour left it at, which an
    attract lap moves on by one of six stops: its frame at one of them dropped
    in the hunt of 2026-11-23 (once in 3.7 h, again in 4.5 h; every sixth lap
    came within about 2 ms), so it is entered at each (ctl exview). Home's
    first frame after the film was within 2.5 ms on every lap of that hunt.
    Until 2026-11-20 Under the Hood (removed) held this place: its first frame
    after a film dropped a frame on about one lap in three.
    """
    cinema, explorer = SCREENS.index("cinema"), SCREENS.index("explorer")
    send(host, ["screen 0"])
    time.sleep(1.5)
    send(host, ["reset", "margins reset"])
    before = stats(host)
    order = [SCREENS.index(n) for n in ("deck", "siglab", "explorer", "hostlink", "coremap", "home", "cinema",
                                        "touchlab", "deck", "home", "specimen", "settings", "camera", "home")]
    for i in order:
        send(host, [f"screen {i}"])
        time.sleep(0.9)
    entries: list[tuple[str, list[dict[str, int]] | None]] = []
    for k in [i % 6 for i in range(film)] + [-1] * homes:
        send(host, [f"screen {cinema}"])
        time.sleep(1.0)
        if k >= 0:
            send(host, [f"exview {k}"])   # Explorer is hidden: its next opening view
        send(host, ["play 0"])
        time.sleep(3.0)
        send(host, [f"screen {explorer if k >= 0 else 0}"])
        time.sleep(1.0)
        entries.append((f"stop {k}" if k >= 0 else "home", entry_frames(host)))
    s = stats(host)
    print(f"transitions: {len(order) + 2 * (film + homes)} screen changes ({film} from a playing film into "
          f"Explorer, {homes} into Home), frame worst {s['render_worst_us'] / 1000:.2f} ms, switch tail worst "
          f"{s.get('tail_worst_us', 0) / 1000:.2f} ms, dropped {int(s['dropped'] - before['dropped'])}")

    def fmt(f: dict[str, int]) -> str:
        left = "no deadline" if f["left"] is None else f"{f['left'] / 1000:.2f} ms"
        return f"{left} (LVGL {f['lv'] / 1000:.1f}, loop {f['pre'] / 1000:.1f}, tail {f['tail'] / 1000:.1f})"

    print("  after the film, time left by the first frame / the next: " +
          ", ".join(f"{n} {' / '.join(fmt(f) for f in fs) if fs else '?'}" for n, fs in entries))
    margins(host, "transitions:")


def remote_test(host: str) -> None:
    """The web remote on http/80, as a phone uses it: the page, the state it
    polls, screen changes, the demo started and stopped, and the guard (a POST
    without the header X-N6 is refused, so another site's page cannot drive
    the board). None of it may cost the UI a frame."""
    base = f"http://{host}"

    def get(path: str) -> bytes:
        with urllib.request.urlopen(base + path, timeout=5) as r:
            return r.read()

    def post(path: str, guard: bool = True) -> int:
        # X-N6-Tour: the tour's own test of the remote, which must not stop the tour
        rq = urllib.request.Request(base + path, method="POST", headers={"X-N6": "1", "X-N6-Tour": "1"} if guard else {})
        try:
            with urllib.request.urlopen(rq, timeout=5) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code

    send(host, ["screen 0", "sleep 1.0", "reset"])
    before = stats(host)
    page = gzip.decompress(get("/"))
    state = json.loads(get("/remote.json"))
    refused = post(f"/screen/{SCREENS.index('siglab')}", guard=False)
    landed = []
    for name in ("siglab", "coremap", "home"):
        post(f"/screen/{SCREENS.index(name)}")
        time.sleep(1.0)
        landed.append(where(host)[0] == name)
    post("/demo/start")
    time.sleep(2.0)
    started = where(host)[1]
    post("/demo/stop")
    time.sleep(1.0)
    stopped = not where(host)[1]
    s = stats(host)
    send(host, ["screen 0"])
    print(f"web remote: page {len(page)} B, {len(state['screens'])} screens and {len(state['clips'])} clips listed; "
          f"a POST without its header {refused} (403 expected); screen changes landed {sum(landed)} of {len(landed)}; "
          f"demo {'started' if started else 'NOT started'} and {'stopped' if stopped else 'NOT stopped'}; "
          f"frame worst {s['render_worst_us'] / 1000:.2f} ms, dropped {int(s['dropped'] - before['dropped'])}")


def attract_test(host: str, secs: float) -> None:
    """Attract mode, one lap: every screen it shows, Explorer's zoom tour and the film.

    No screenshots here: reading the frame buffer back costs the UI one frame
    (an 18 ms copy to PSRAM), which would be counted as a drop.
    """
    send(host, ["screen 0"])
    time.sleep(1.5)
    send(host, ["reset", "margins reset"])
    before, a0 = stats(host), kv(host, "audio")
    send(host, ["attract"])
    wait(host, secs)
    s, a1 = stats(host), kv(host, "audio")
    m = send(host, ["margins"])[0]  # before the switch below
    send(host, ["screen 0"])  # counts as input: ends attract mode
    print(f"attract mode: {secs:.0f} s, frame worst {s['render_worst_us'] / 1000:.2f} ms, "
          f"dropped {int(s['dropped'] - before['dropped'])}, "
          f"audio underruns {int(a1.get('errors', 0) - a0.get('errors', 0))}")
    margins(host, "attract:", m)


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
        top = first if first < 4 else 8 + (first - 4) * 2  # the index of its top tile
        real = h0.get("real", len(SCREENS) - 1)     # tiles that open a screen now; the rest are placeholders
        expect = SCREENS[top + 1] if top < real else "home"  # a placeholder opens nothing
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
    ap.add_argument("--attract", type=float, default=170.0, help="seconds of attract mode (one lap, end titles and pause included, is about 159)")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    # The board knows the tour runs (the web remote shows it, and can stop it).
    began = ctl.send(a.host, ["tour begin"])[0]
    if began.startswith("err"):
        sys.exit(f"tour: the board says {began[4:]}")
    companion, end = None, "failed"
    try:
        if not a.no_companion:
            # Host Link and Core Map are measured with live data, not their waiting state
            # (--no-tour: this one never starts a tour of its own)
            companion = subprocess.Popen([sys.executable, str(ROOT / "host" / "companion.py"), "--host", a.host,
                                          "--no-tour"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.5)
        run(a)
        end = "finished"
    except Stopped:
        end = "stopped"
    except KeyboardInterrupt:
        end = "interrupted"
    except Exception:
        # a command refused inside a helper (acap, a reply that no longer parses)
        if "state=stopping" not in ctl.send(a.host, ["tour"])[0]:
            raise
        end = "stopped"
    finally:
        if end == "stopped":
            print("tour: stopped from the board's web remote")
            remove_test_clip(a.host)
        try:
            ctl.send(a.host, [f"tour end {end}"])
        except OSError:
            pass
        if companion:
            companion.terminate()


def remove_test_clip(host: str) -> None:
    """A tour stopped mid-upload leaves the test clip on the card (the upload
    service, unlike the control port, still serves it)."""
    import n6v

    try:
        card = n6v.Card(host)
        card.ask(f"del {TEST_CLIP}")
        card.close()
    except (OSError, SystemExit):
        pass


def run(a: argparse.Namespace) -> None:
    step(a.host, "screens")
    for name in a.screens.split(","):
        visit(a.host, SCREENS.index(name), name, settle=2.0, run=a.run)
    if not a.no_touch:
        step(a.host, "touch")
        touch_tests(a.host)
        step(a.host, "theremin")
        theremin_test(a.host)
        step(a.host, "monitor")
        monitor_test(a.host)
    step(a.host, "transitions")
    transition_test(a.host)
    step(a.host, "remote")
    remote_test(a.host)
    step(a.host, "fault")
    fault_test(a.host)
    if not a.no_media:
        step(a.host, "media")
        media_tests(a.host)
        step(a.host, "card")
        card_tests(a.host)
    if not a.no_attract:
        step(a.host, "attract")
        attract_test(a.host, a.attract)
    ds = dict(re.findall(r"(\w+)=(\S+)", send(a.host, ["ds"])[0]))
    print(f"memory: PSRAM in use {media(a.host)['psram_kb'] / 1000:.1f} of 28 MB; Explorer's data set from "
          f"{ds['source']}" + ("" if ds["source"] == "flash" else " (14 MB of PSRAM: tools/dataset.py puts it in the flash)"))
    trace_port(a.host)
    send(a.host, ["screen 0"])


def fault_test(host: str) -> None:
    """zprof's fault capture as the demo uses it: a test thread faults, the board
    runs on with no frame dropped, the alarm names the thread (the control port and
    /status.json), and an acknowledgement clears it, so a tour leaves no alarm."""
    def faults() -> dict[str, str]:
        line = send(host, ["faults"])[0]
        return {k: v.strip('"') for k, v in re.findall(r'(\w+)=("[^"]*"|\S+)', line)} if "count=" in line else {}

    def status() -> dict:
        with urllib.request.urlopen(f"http://{host}/status.json", timeout=5) as r:
            return json.loads(r.read())["faults"]

    f0 = faults()
    if not f0:
        print("fault capture: NOT in this build (no zprof fault handler)")
        return
    before = stats(host)
    r = send(host, ["crash 1"])[0]
    if not r.startswith("crash 1"):
        print(f"fault capture: NOT tested ({r.strip()}; CONFIG_APP_CRASH_TEST off?)")
        return
    time.sleep(2.0)
    after = stats(host)
    f1, js = faults(), status()
    raised = (int(f1["count"]) == int(f0["count"]) + 1 and f1["last"] == "crash_test"
              and js["count"] == int(f1["count"]) and js["count"] > js["acknowledged"])
    send(host, ["faults ack"])
    js2 = status()
    cleared = js2["acknowledged"] == js2["count"]
    print(f"fault capture: a test thread's {f1.get('reason', '?')} caught, the board ran on "
          f"(dropped {int(after['dropped'] - before['dropped'])}); alarm raised "
          f"{'yes' if raised else 'NO'}, cleared by faults ack {'yes' if cleared else 'NO'}")


def trace_port(host: str) -> None:
    """zprof, the tracer, is in every build: the board's discovery answer lists its trace
    port (prof=5560). Asked of the answer, not of the port: a bridge streaming during the
    tour holds the port's one session."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    try:
        for _ in range(3):
            s.sendto(b"N6HQ", (host, 5557))
            try:
                data, _ = s.recvfrom(512)
            except (socket.timeout, ConnectionResetError):
                continue
            if data[:4] == b"N6HA":
                m = re.search(rb"\bprof=(\d+)", data)
                print(f"zprof: trace port {m.group(1).decode()} listed" if m else
                      "zprof: NOT in this build (no prof= in the discovery answer)")
                return
    finally:
        s.close()
    print("zprof: NOT checked (the board did not answer discovery)")


if __name__ == "__main__":
    main()

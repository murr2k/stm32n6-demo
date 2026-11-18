#!/usr/bin/env python3
"""Record the board's screen: the board encodes it itself with its hardware
H.264 encoder and streams it on tcp/5559 (app/src/media/recorder.h).

Connecting starts the recording, closing the connection stops it. Writes
captures/rec/<name>.h264 (the stream as it came), <name>.csv (one line per
picture: the panel refresh it was taken at, the board's time, the encoder's
time, size, IDR or P, the screen, whether the display was single-buffered),
<name>.mkv (each picture at the time of its refresh, no re-encoding; needs
PyAV, `pip install av` in the project venv) and <name>.mp4 (the same).

Usage: record.py [--host IP] [--secs 10] [--name NAME] [--fps 30|60] [--attract]
  --fps      a picture every second refresh (30, the default) or every one (60)
  --attract  start attract mode first and record one lap: until the lap
             is back at the screen it started from (or --secs, if sooner)
"""
from __future__ import annotations

import argparse
import csv
import socket
import statistics
import struct
import subprocess
import sys
import time
from datetime import datetime
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctl import DEFAULT_HOST, close, send  # noqa: E402
from n6v import ffmpeg_exe  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "captures" / "rec"
PORT = 5559
HDR = struct.Struct("<4sBBBBIIIIII")  # struct rec_hdr
REC_HEADERS, REC_PICTURE, REC_AUDIO = 1, 2, 3
FLAG_SINGLE = 0x01


def read_exact(s: socket.socket, n: int) -> bytes:
    b = bytearray()
    while len(b) < n:
        chunk = s.recv(n - len(b))
        if not chunk:
            raise ConnectionError("the board closed the connection")
        b += chunk
    return bytes(b)


def picture_times(rows: list[dict], refresh_hz: float, rate: float) -> list[float]:
    """When each picture shows, in seconds on the sound's clock. A picture's
    audio_pos is the DAC frame playing as it appeared, estimated between the
    DAC's blocks; a straight line through all of them against the refresh
    number takes out that estimate's jitter and covers the first pictures,
    taken before the tap had a block. Without sound: the refresh times."""
    pts = [(int(r["refresh"]), int(r["audio_pos"])) for r in rows if int(r["audio_pos"]) > 0]
    if rate <= 0 or len(pts) < 10:
        r0 = int(rows[0]["refresh"])
        return [(int(r["refresh"]) - r0) / refresh_hz for r in rows]
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    b = sum((x - mx) * (y - my) for x, y in pts) / sum((x - mx) ** 2 for x, _ in pts)
    a = my - b * mx
    return [(a + b * int(r["refresh"])) / rate for r in rows]


def mux(h264: Path, times: list[float], mkv: Path) -> int:
    """The stream in an MKV with every picture at its time, so a slot the
    board missed holds the picture before it longer instead of shifting
    everything after it. No re-encoding. Returns the pictures placed."""
    import av  # PyAV, in the project venv

    tb = Fraction(1, 1000)
    with av.open(str(h264), format="h264") as inp, av.open(str(mkv), "w") as out:
        src = inp.streams.video[0]
        dst = out.add_stream_from_template(src)
        dst.time_base = tb
        n = 0
        last = -1
        for pkt in inp.demux(src):
            if pkt.size == 0 or n >= len(times):
                continue
            pts = max(round(times[n] * 1000), last + 1)
            pkt.pts = pkt.dts = pts
            last = pts
            pkt.time_base = tb
            pkt.stream = dst
            out.mux(pkt)
            n += 1
    return n


def write_wav(path: Path, pcm: bytes, rate: int) -> None:
    import wave

    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def where(host: str) -> tuple[int, bool]:
    reply = send(host, ["where"])[0]
    v = dict(x.split("=") for x in reply.split() if "=" in x)
    return int(v["screen"]), v["attract"] == "1"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--secs", type=float, default=10.0)
    ap.add_argument("--name", default=datetime.now().strftime("rec-%Y%m%d-%H%M%S"))
    ap.add_argument("--attract", action="store_true")
    ap.add_argument("--fps", type=int, choices=(30, 60), default=30)
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    h264, table, mkv, mp4 = (OUT / f"{a.name}{x}" for x in (".h264", ".csv", ".mkv", ".mp4"))

    refresh_hz = int(dict(x.split("=") for x in send(a.host, ["stats"])[0].split() if "=" in x)["refresh_mhz"]) / 1000
    start_screen = None
    if a.attract:
        start_screen = where(a.host)[0]
        send(a.host, ["attract"])
    # the control port serves one client at a time: let others (tour.py) in while this records
    close(a.host)

    s = socket.create_connection((a.host, PORT), timeout=10)
    s.sendall(f"fps {a.fps}\n".encode())
    pictures, sizes, enc_us, kinds = 0, [], [], {0: 0, 1: 0}
    missed = singles = 0
    audio = bytearray()
    left_start = False
    t0 = time.time()
    with open(h264, "wb") as fh, open(table, "w", newline="") as ft:
        w = csv.writer(ft)
        w.writerow(["refresh", "t_us", "encode_us", "bytes", "coding", "screen", "single", "missed", "audio_pos"])
        try:
            while time.time() - t0 < a.secs:
                magic, kind, coding, screen, flags, refresh, t_us, e_us, apos, size, miss = HDR.unpack(
                    read_exact(s, HDR.size))
                if magic != b"N6RV":
                    sys.exit(f"bad header {magic!r}")
                data = read_exact(s, size)
                if kind == REC_AUDIO:
                    at = apos * 4
                    if len(audio) < at:
                        audio.extend(bytes(at - len(audio)))  # a gap: silence
                    audio[at:at + len(data)] = data
                    continue
                fh.write(data)
                if kind != REC_PICTURE:
                    continue
                pictures += 1
                sizes.append(size)
                enc_us.append(e_us)
                kinds[coding] = kinds.get(coding, 0) + 1
                missed = miss
                singles += bool(flags & FLAG_SINGLE)
                w.writerow([refresh, t_us, e_us, size, "IDR" if coding == 0 else "P", screen,
                            int(bool(flags & FLAG_SINGLE)), miss, apos])
                if start_screen is not None:
                    if screen != start_screen:
                        left_start = True
                    elif left_start:
                        break  # back where the lap began
        finally:
            s.close()
    secs = time.time() - t0
    if a.attract:
        send(a.host, ["screen %d" % (start_screen or 0)])
        close(a.host)

    fps = refresh_hz / (1 if a.fps == 60 else 2)
    rows = list(csv.DictReader(open(table, newline="")))
    # the sound's real rate, from the board's own clock: DAC frames per microsecond between the
    # first and the last picture (the DAC runs from its own PLL, 47.7 kHz, not 48)
    rate = 0.0
    with_audio = [r for r in rows if int(r["audio_pos"]) > 0]
    if audio and len(with_audio) > 10:
        f, l = with_audio[0], with_audio[-1]
        span_us = (int(l["t_us"]) - int(f["t_us"])) & 0xFFFFFFFF
        if span_us > 1_000_000:
            rate = (int(l["audio_pos"]) - int(f["audio_pos"])) * 1e6 / span_us
    times = picture_times(rows, refresh_hz, rate)
    video = mkv.with_name(mkv.stem + "-video.mkv")
    placed = mux(h264, times, video)
    ff = [ffmpeg_exe(), "-y", "-v", "error"]
    if rate > 0:
        wav = mkv.with_suffix(".wav")
        write_wav(wav, bytes(audio), round(rate))
        subprocess.run(ff + ["-i", str(video), "-i", str(wav), "-map", "0:v", "-map", "1:a", "-c:v", "copy",
                             "-c:a", "pcm_s16le", str(mkv)], check=True)
        subprocess.run(ff + ["-i", str(mkv), "-c:v", "copy", "-c:a", "aac", "-b:a", "320k", str(mp4)], check=True)
        print(f"sound: {len(audio) // 4 / rate:.1f} s at {rate:.1f} Hz (the DAC's measured rate)")
    else:
        video.replace(mkv)
        subprocess.run(ff + ["-i", str(mkv), "-c", "copy", str(mp4)], check=True)
    if placed != pictures:
        print(f"warning: {pictures} pictures received, {placed} found in the stream")
    mbit = sum(sizes) * 8 / max(secs, 1e-3) / 1e6
    print(f"{pictures} pictures in {secs:.1f} s ({pictures / max(secs, 1e-3):.2f} a second, {fps:.3f} due), "
          f"{missed} missed, {kinds.get(0, 0)} IDR / {kinds.get(1, 0)} P, {singles} single-buffered")
    if enc_us:
        print(f"encode {statistics.mean(enc_us) / 1000:.2f} ms mean, {max(enc_us) / 1000:.2f} ms worst; "
              f"{mbit:.1f} Mbit/s; largest picture {max(sizes) // 1024} KB")
    print(f"{h264}\n{table}\n{mkv}\n{mp4}")


if __name__ == "__main__":
    main()

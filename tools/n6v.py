#!/usr/bin/env python3
"""N6V clips: Motion JPEG + PCM for the STM32N6570-DK demo player.

  encode IN -o OUT.n6v --title T   transcode any video ffmpeg can read
  info FILE.n6v                    print a clip's header and bit rate
  pack A.n6v B.n6v -o media.bin    build the media partition image
  flash media.bin                  write it to the board's XSPI flash

  upload A.n6v B.n6v               copy clips to the board's microSD card over Ethernet
  ls | rm NAME.n6v | df            list or delete clips on the card, free space
  mount                            mount a card that was put in after the board started

Clips on the card show up in Cinema as soon as the upload ends; one with the
same title as a clip in flash replaces it in the list. The format is
documented in app/src/media/player.h, the upload protocol in
app/src/media/sdcard.h.
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ctl import DEFAULT_HOST  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MEDIA_PORT = 5558
# Bytes of an upload per credit from the board (SD_PIECE in sdcard.h, where
# the reason is: Zephyr's delayed ACK makes a plain bulk send crawl).
PIECE = 8192
W, H = 800, 480
JPEG_PAD = 8
MEDIA_FLASH_ADDR = 0x70420000
MEDIA_MAX = 0x78000000 - MEDIA_FLASH_ADDR  # to the end of the 128 MB flash
HEADER = struct.Struct("<4sHHHHIIHHII32s")
assert HEADER.size == 64


def ffmpeg_exe() -> str:
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    hits = glob.glob(os.path.expandvars(
        r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg_*\ffmpeg-*\bin\ffmpeg.exe"))
    if hits:
        return hits[0]
    sys.exit("ffmpeg not found")


def pad4(b: bytes) -> bytes:
    return b + b"\0" * (-len(b) % 4)


def build(frames: list[bytes], audio: bytes, fps: tuple[int, int], rate: int, channels: int,
          title: str) -> bytes:
    """Assemble a clip from JPEG frames and interleaved s16le audio."""
    bytes_per_frame_audio = rate * channels * 2 * fps[1] / fps[0]
    chunks, offsets = [], []
    pos = HEADER.size
    for i, jpg in enumerate(frames):
        a0 = int(i * bytes_per_frame_audio) // (2 * channels) * (2 * channels)
        a1 = int((i + 1) * bytes_per_frame_audio) // (2 * channels) * (2 * channels)
        pcm = audio[a0:a1]
        body = struct.pack("<II", len(jpg), len(pcm)) + pad4(jpg + b"\0" * JPEG_PAD) + pad4(pcm)
        offsets.append(pos)
        chunks.append(body)
        pos += len(body)
    index = struct.pack(f"<{len(offsets)}I", *offsets)
    size = pos + len(index)
    head = HEADER.pack(b"N6V1", W, H, fps[0], fps[1], len(frames), rate, channels, 16, pos, size,
                       title.encode("ascii", "replace")[:31])
    return head + b"".join(chunks) + index


def cmd_encode(a: argparse.Namespace) -> None:
    ff = ffmpeg_exe()
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        vf = (f"scale={W}:{H}:force_original_aspect_ratio=decrease:flags=lanczos,"
              f"pad={W}:{H}:(ow-iw)/2:(oh-ih)/2,fps={a.fps}")
        base = [ff, "-hide_banner", "-loglevel", "error", "-y"]
        src = (["-f", "lavfi"] if a.lavfi else []) + ["-i", a.input]
        cut = (["-ss", str(a.start)] if a.start else []) + (["-t", str(a.duration)] if a.duration else [])
        subprocess.run([*base, *src, *cut, "-map", "0:v:0", "-vf", vf, "-c:v", "mjpeg", "-pix_fmt", "yuvj420p",
                        "-q:v", str(a.q), "-an", str(t / "%06d.jpg")], check=True)
        if a.audio_lavfi:
            asrc = ["-f", "lavfi", "-i", a.audio_lavfi]
        elif a.audio:
            asrc = ["-i", a.audio]
        else:
            asrc = [*src]
        r = subprocess.run([*base, *asrc, *cut, "-vn", "-ac", "2", "-ar", "48000", "-f", "s16le",
                            str(t / "audio.raw")], capture_output=True)
        frames = [p.read_bytes() for p in sorted(t.glob("*.jpg"))]
        audio = (t / "audio.raw").read_bytes() if r.returncode == 0 and (t / "audio.raw").exists() else b""
    if not frames:
        sys.exit("no frames produced")
    need = int(len(frames) / a.fps * 48000) * 4
    audio = audio[:need] + b"\0" * max(0, need - len(audio))  # silence if the source has no audio
    clip = build(frames, audio, (a.fps, 1), 48000, 2, a.title or Path(a.output).stem)
    Path(a.output).parent.mkdir(parents=True, exist_ok=True)
    Path(a.output).write_bytes(clip)
    describe(Path(a.output))


def describe(path: Path) -> None:
    data = path.read_bytes()
    magic, w, h, fn, fd, n, rate, ch, bits, index_off, size, title = HEADER.unpack_from(data)
    if magic != b"N6V1":
        sys.exit(f"{path}: not an N6V clip")
    secs = n * fd / fn
    video = index_off - HEADER.size - secs * rate * ch * bits // 8
    print(f"{path.name}: \"{title.rstrip(bytes(1)).decode()}\" {w}x{h} {fn}/{fd} fps, {n} frames, {secs:.1f} s, "
          f"{size / 1e6:.1f} MB, video {video / n / 1024:.1f} KB/frame = {video * 8 / secs / 1e6:.1f} Mbit/s, "
          f"audio {rate} Hz x{ch}")


def cmd_info(a: argparse.Namespace) -> None:
    for f in a.files:
        describe(Path(f))


def cmd_pack(a: argparse.Namespace) -> None:
    blobs = [Path(f).read_bytes() for f in a.files]
    entries, pos = [], 4096
    for b in blobs:
        entries.append((pos, len(b)))
        pos += (len(b) + 4095) // 4096 * 4096
    if pos > MEDIA_MAX:
        sys.exit(f"media image is {pos / 1e6:.1f} MB, the partition holds {MEDIA_MAX / 1e6:.1f} MB")
    head = struct.pack("<4sI", b"N6MD", len(blobs)) + b"".join(struct.pack("<II", *e) for e in entries)
    image = bytearray(head.ljust(4096, b"\xff"))
    for (off, _), b in zip(entries, blobs):
        image += b.ljust((len(b) + 4095) // 4096 * 4096, b"\xff")
    Path(a.output).parent.mkdir(parents=True, exist_ok=True)
    Path(a.output).write_bytes(image)
    print(f"{a.output}: {len(blobs)} clip(s), {len(image) / 1e6:.1f} MB of {MEDIA_MAX / 1e6:.1f} MB")


def cmd_flash(a: argparse.Namespace) -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import n6  # noqa: E402

    path = Path(a.image).resolve()
    print(f"[n6v] writing {path.stat().st_size / 1e6:.1f} MB to {MEDIA_FLASH_ADDR:#x} (this is slow over SWD)")
    n6.power_cycle()
    out = n6.cli([*n6.CONNECT, "-el", str(n6.LOADER), "-d", str(path), hex(MEDIA_FLASH_ADDR)])
    if "File download complete" not in out:
        print(out)
        sys.exit("[n6v] flash failed")
    print("[n6v] done; restart the board with: n6.cmd run")


class Card:
    """One connection to the board's upload service (tcp/5558)."""

    def __init__(self, host: str, timeout: float = 30.0) -> None:
        self.s = socket.create_connection((host, MEDIA_PORT), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.f = self.s.makefile("rb")

    def reply(self) -> tuple[list[str], str]:
        """Data lines, then the "ok ..." or "err ..." line that ends every reply."""
        lines: list[str] = []
        while True:
            raw = self.f.readline()
            if not raw:
                raise ConnectionError("the board closed the connection")
            line = raw.decode("ascii", "replace").rstrip("\n")
            if line == "ok" or line.startswith(("ok ", "err ")):
                return lines, line
            lines.append(line)

    def ask(self, line: str) -> tuple[list[str], str]:
        self.s.sendall((line + "\n").encode("ascii"))
        return self.reply()

    def close(self) -> None:
        self.s.close()


def card_name(path: Path) -> str:
    """The name a clip gets on the card: the board accepts letters, digits, . _ - only."""
    name = re.sub(r"[^A-Za-z0-9._-]", "_", path.stem)[:32].lstrip(".") or "clip"
    return name + ".n6v"


def upload(host: str, files: list[str], quiet: bool = False) -> list[tuple[str, int, float]]:
    """Copy clips to the card. Returns (name, bytes, seconds) per file."""
    card = Card(host)
    done = []
    try:
        for f in files:
            data = Path(f).read_bytes()
            if data[:4] != b"N6V1":
                sys.exit(f"{f}: not an N6V clip")
            name = card_name(Path(f))
            _, r = card.ask(f"put {name} {len(data)} {zlib.crc32(data):08x}")
            if r != "ok go":
                sys.exit(f"{name}: {r}")
            t0 = time.time()
            view, shown = memoryview(data), -1
            for pos in range(0, len(data), PIECE):
                card.s.sendall(view[pos:pos + PIECE])
                if pos + PIECE <= len(data) and card.f.read(1) != b".":
                    sys.exit(f"\n{name}: the board stopped the upload: {card.f.readline().decode('ascii', 'replace').strip()}")
                pct = 100 * min(pos + PIECE, len(data)) // len(data)
                if not quiet and pct != shown:
                    shown = pct
                    print(f"\r{name}: {pct:3d} %", end="", flush=True)
            _, r = card.reply()
            secs = time.time() - t0
            if not r.startswith("ok "):
                sys.exit(f"\n{name}: {r}")
            if not quiet:
                part = r.split()
                split = f" (board: {int(part[3]) / 1000:.1f} s receiving, {int(part[4]) / 1000:.1f} s writing)" if len(part) >= 5 else ""
                print(f"\r{name}: {len(data) / 1e6:.1f} MB in {secs:.1f} s, {len(data) / 1e6 / secs:.2f} MB/s, checksum verified{split}")
            done.append((name, len(data), secs))
    finally:
        card.close()
    return done


def cmd_upload(a: argparse.Namespace) -> None:
    upload(a.host, a.files)


def cmd_ls(a: argparse.Namespace) -> None:
    card = Card(a.host)
    lines, r = card.ask("list")
    card.close()
    if not r.startswith("ok"):
        sys.exit(r)
    for line in lines:
        _, name, size = line.split()
        print(f"{int(size) / 1e6:8.1f} MB  {name}")
    print(f"{len(lines)} file(s) in /clips on the card")


def cmd_rm(a: argparse.Namespace) -> None:
    card = Card(a.host)
    for name in a.names:
        print(f"{name}: {card.ask(f'del {name}')[1]}")
    card.close()


def cmd_df(a: argparse.Namespace) -> None:
    card = Card(a.host, timeout=120.0)  # a FAT32 card may have to count its free clusters
    print(card.ask("df")[1])
    card.close()


def cmd_mount(a: argparse.Namespace) -> None:
    card = Card(a.host)
    print(card.ask("mount")[1])
    card.close()


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, fn in (("upload", cmd_upload), ("ls", cmd_ls), ("rm", cmd_rm), ("df", cmd_df), ("mount", cmd_mount)):
        c = sub.add_parser(name)
        c.add_argument("--host", default=DEFAULT_HOST)
        if name == "upload":
            c.add_argument("files", nargs="+")
        elif name == "rm":
            c.add_argument("names", nargs="+")
        c.set_defaults(fn=fn)
    e = sub.add_parser("encode")
    e.add_argument("input")
    e.add_argument("-o", "--output", required=True)
    e.add_argument("--title")
    e.add_argument("--fps", type=int, default=30)
    e.add_argument("--q", type=int, default=4, help="ffmpeg MJPEG quantiser, 2 (best) to 31")
    e.add_argument("--start", type=float)
    e.add_argument("--duration", type=float)
    e.add_argument("--lavfi", action="store_true", help="input is an ffmpeg lavfi graph")
    e.add_argument("--audio", help="take audio from this file instead (a WAV keeps it lossless)")
    e.add_argument("--audio-lavfi", help="take audio from this lavfi source instead")
    e.set_defaults(fn=cmd_encode)
    i = sub.add_parser("info"); i.add_argument("files", nargs="+"); i.set_defaults(fn=cmd_info)
    k = sub.add_parser("pack"); k.add_argument("files", nargs="+"); k.add_argument("-o", "--output", required=True)
    k.set_defaults(fn=cmd_pack)
    f = sub.add_parser("flash"); f.add_argument("image"); f.set_defaults(fn=cmd_flash)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()

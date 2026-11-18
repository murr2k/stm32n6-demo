#!/usr/bin/env python3
"""Soak test: the board left running for hours, measured all the time.

Cycles of an hour by default (9 of them): 40 minutes of attract mode (every
screen, the transitions, the film, the theremin's ghost, the camera with the
NPU looking for hands; the host companion runs, so Host Link and Core Map
have live data), then 20 minutes on the Camera screen with the NPU tracking
the built-in test picture twice (`nn hands 3`: both networks every frame, a
palm search every 500 ms, camera and display running: the combination ST's
forum reports bus faults under). The tracked hands keep attract mode off.

Every second: where, stats, reset. Each second's worst frame, refresh period
and switch tail, the dropped-frame count and the least margin before a swap
deadline are written with the screen that was up; a change of screen is a
switch, its tail credited to from -> to. A dropped frame is described from
the board's own record of it (ctl droptrace): how far over it went, where
its budget went, and whether the panel was dark (a transition's black hold,
where nobody can see it), fading or steady.
Every minute: mem (kernel heap, LVGL pool, PSRAM, least stack left), audio
and microphone errors, the NPU's counters, /status.json over HTTP; every ten
minutes every thread's stack. The console is logged throughout.

The run stops early and leaves the board as it is when it stops answering
for two minutes, its uptime goes back (a reset), or the console prints a
fatal error. Do not reset it then: read the console log and the CPU state
(CLAUDE.md, "State that a guard reads").

Pass: no reset or fault; heap, LVGL pool and PSRAM the same at the end as
after the first hour; frame times per screen the same in the last hour as
in the first; every dropped frame accounted for.

Output: captures/soak/<start>/ seconds.csv, minutes.csv, stacks.log,
events.log, console.log, summary.txt. Progress and events also go to stdout.
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time
import urllib.request
from collections import defaultdict
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from ctl import DEFAULT_HOST, close, send  # noqa: E402

CAMERA = 11
UNREACHABLE_LIMIT = 120.0
FATAL = re.compile(r"FATAL|\*{3,}.*FAULT|Booting (Zephyr|MCUboot)|Halting system|Kernel panic")
NOTE = re.compile(r"<err>|<wrn>")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def kv(line: str) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=([-\w./:]+)", line))


def num(d: dict[str, str], k: str, default: float = 0) -> float:
    try:
        return float(d[k])
    except (KeyError, ValueError):
        return default


def drop_detail(host: str) -> str:
    """The board's own record of the last dropped frame (ctl droptrace): where its
    budget went and whether anyone could see it (the layer's fade)."""
    try:
        d = kv(send(host, ["droptrace"])[0])
        late = d["f"].split(",")[-1].rstrip("u").split("/")
        wake, tail, pre, lv, away, left, alpha = (int(x) for x in late)
    except (OSError, KeyError, ValueError, IndexError):
        return "no drop trace"
    seen = "panel dark, not visible" if alpha == 0 else "steady" if alpha == 255 else f"fading, alpha {alpha}"
    return (f"late frame: {-left / 1000:.2f} ms over, {seen}; tail {tail / 1000:.2f} + main loop {pre / 1000:.2f}"
            f" + LVGL {lv / 1000:.2f} ms, other threads {away / 1000:.2f} ms")


def keep_awake() -> None:
    """The PC must not sleep under the run (Windows: for as long as this process lives)."""
    if sys.platform == "win32":
        import ctypes

        ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000001)  # continuous, system required


class Events:
    def __init__(self, path: Path, t0: float):
        self.f = open(path, "a", encoding="utf-8")
        self.t0 = t0
        self.lock = threading.Lock()

    def note(self, text: str) -> None:
        el = time.monotonic() - self.t0
        line = f"{datetime.now():%H:%M:%S} +{el / 3600:5.2f} h  {text}"
        with self.lock:
            self.f.write(line + "\n")
            self.f.flush()
            print(line, flush=True)


class Console(threading.Thread):
    def __init__(self, port: str, path: Path, events: Events):
        super().__init__(daemon=True)
        self.port, self.path, self.events = port, path, events
        self.stop = threading.Event()
        self.fatal: str | None = None
        self.notes = 0

    def run(self) -> None:
        try:
            import serial

            ser = serial.Serial(self.port, 115200, timeout=0.2)
        except Exception as e:  # noqa: BLE001 - any failure: run without the console
            self.events.note(f"console: cannot open {self.port} ({e}); running without it")
            return
        with ser, open(self.path, "wb") as sink:
            buf = b""
            while not self.stop.is_set():
                chunk = ser.read(4096)
                if not chunk:
                    continue
                sink.write(chunk)
                sink.flush()  # the log has to survive this process being killed
                buf += chunk
                *lines, buf = buf.split(b"\n")
                for ln in lines:
                    text = ANSI.sub("", ln.decode("utf-8", "replace")).strip()
                    if FATAL.search(text):
                        self.fatal = self.fatal or text
                        self.events.note(f"CONSOLE FATAL: {text}")
                    elif NOTE.search(text):
                        self.notes += 1
                        if self.notes <= 200:
                            self.events.note(f"console: {text}")


def status_json(host: str) -> tuple[bool, float]:
    t = time.monotonic()
    try:
        with urllib.request.urlopen(f"http://{host}/status.json", timeout=5) as r:
            json.loads(r.read())
        return True, (time.monotonic() - t) * 1000
    except Exception:  # noqa: BLE001
        return False, 0.0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", default=os.environ.get("N6_PORT", "COM18"), help="the board's console (ST-LINK VCP)")
    ap.add_argument("--hours", type=float, default=9.0)
    ap.add_argument("--cycle", type=float, default=60.0, help="minutes per cycle")
    ap.add_argument("--camera", type=float, default=20.0, help="minutes of each cycle on the Camera screen")
    ap.add_argument("--no-companion", action="store_true")
    ap.add_argument("--sound", type=int, choices=(0, 1), help="set the theremin's SOUND switch for the run "
                    "(an unattended board: 0 keeps the attract ghost silent; the synthesizer runs either way)")
    a = ap.parse_args()

    keep_awake()
    t0 = time.monotonic()
    out = ROOT / "captures" / "soak" / datetime.now().strftime("%Y%m%d-%H%M")
    out.mkdir(parents=True, exist_ok=True)
    ev = Events(out / "events.log", t0)
    con = Console(a.port, out / "console.log", ev)
    con.start()
    companion = None
    if not a.no_companion:
        companion = subprocess.Popen([sys.executable, str(ROOT / "host" / "companion.py"), "--host", a.host],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    names = [str(i) for i in range(12)]
    try:
        names = json.loads(urllib.request.urlopen(f"http://{a.host}/status.json", timeout=5).read())["screens"]
    except Exception:  # noqa: BLE001
        pass

    def name(i: int) -> str:
        return names[i] if 0 <= i < len(names) else str(i)

    if a.sound is not None:
        send(a.host, [f"thmsound {a.sound}"])
    send(a.host, ["reset"])  # the worst-case figures from before the run (a boot's first frames) do not count

    sec_f = open(out / "seconds.csv", "w", newline="")
    sec = csv.writer(sec_f)
    sec.writerow(["t_s", "time", "phase", "screen", "attract", "frames", "fps", "render_avg_us", "render_worst_us",
                  "period_worst_us", "tail_worst_us", "dropped", "new_drops", "margin_min_us", "late_swaps"])
    min_f = open(out / "minutes.csv", "w", newline="")
    mins = csv.writer(min_f)
    min_cols = ["t_s", "time", "phase", "screen", "uptime_s", "heap_used", "heap_peak", "ui_used", "ui_peak", "psram_kb",
                "stack_min_free", "stack_min_thread", "audio_errors", "mic_errors", "nn_hands", "nn_searches",
                "nn_palms", "nn_confirmed", "nn_lost", "nn_palm_us", "nn_hand_us", "nn_step_us", "http_ok", "http_ms"]
    mins.writerow(min_cols)
    stacks_f = open(out / "stacks.log", "w", encoding="utf-8")

    total_s = a.hours * 3600
    cycle_s, cam_s = a.cycle * 60, a.camera * 60
    ev.note(f"soak start: {a.hours:g} h, cycles of {a.cycle:g} min ({a.cycle - a.camera:g} attract, "
            f"{a.camera:g} Camera with nn hands 3), output {out}")

    phase = None
    prev_screen = prev_frames = prev_dropped = prev_uptime = dropped0 = None
    last_ok = time.monotonic()
    unreachable_since = None
    pending_switch = None   # (t, from, to, tail so far)
    switches: list[tuple[float, int, int, int]] = []
    drops: list[str] = []
    render = defaultdict(lambda: defaultdict(list))  # hour -> (screen, phase) -> render_avg samples
    worst = defaultdict(lambda: defaultdict(int))    # hour -> (screen, phase) -> worst frame
    mem_rows: list[dict[str, float]] = []
    first_stacks = last_stacks = ""
    fatal = None
    next_tick = time.monotonic()
    next_min = next_tick
    next_stacks = next_tick
    next_fix = 0.0
    hour_reported = 0

    try:
        while True:
            now = time.monotonic()
            el = now - t0
            if el >= total_s:
                break
            if con.fatal:
                fatal = f"console: {con.fatal}"
                break
            want = "attract" if el % cycle_s < cycle_s - cam_s else "camera"
            try:
                if want != phase:
                    if want == "camera":
                        # end attract mode with a touch on the title bar, then hold the Camera screen
                        send(a.host, ["touch 4 400 4 1", "sleep 0.05", "touch 4 400 4 0", "sleep 0.5",
                                      f"screen {CAMERA}", "sleep 2", "nn hands 3"])
                    else:
                        send(a.host, ["nn hands 1", "sleep 0.5", "attract"])
                    ev.note(f"phase: {want}")
                    phase = want
                    next_fix = now + 30
                r = send(a.host, ["where", "stats", "reset"])
                w, s = kv(r[0]), kv(r[1])
                last_ok = now
                if unreachable_since is not None:
                    ev.note(f"board answers again after {now - unreachable_since:.0f} s")
                    unreachable_since = None
            except OSError as e:
                close(a.host)
                if unreachable_since is None:
                    unreachable_since = now
                    ev.note(f"board does not answer ({e})")
                if now - last_ok > UNREACHABLE_LIMIT:
                    fatal = f"no answer for {now - last_ok:.0f} s"
                    break
                next_tick += 1.0
                time.sleep(max(0.0, next_tick - time.monotonic()))
                continue

            screen, attract = int(num(w, "screen", -1)), int(num(w, "attract"))
            frames, dropped = int(num(s, "frames")), int(num(s, "dropped"))
            tail, rw, pw = int(num(s, "tail_worst_us")), int(num(s, "render_worst_us")), int(num(s, "period_worst_us"))
            if prev_frames is not None and frames < prev_frames:
                fatal = f"frame counter went back ({prev_frames} -> {frames}): the board restarted"
                break
            if dropped0 is None:
                dropped0 = dropped
            new = dropped - prev_dropped if prev_dropped is not None else 0
            sec.writerow([f"{el:.1f}", datetime.now().strftime("%H:%M:%S"), phase, screen, attract, frames,
                          s.get("fps", ""), s.get("render_avg_us", ""), rw, pw, tail, dropped, new,
                          s.get("margin_min_us", ""), s.get("late_swaps", "")])
            hour = int(el // 3600)
            key = (name(screen), phase if screen == CAMERA else "")
            render[hour][key].append(num(s, "render_avg_us"))
            worst[hour][key] = max(worst[hour][key], rw)

            # a switch's tail lands in the second it is seen or the next
            if pending_switch:
                t_sw, f_sw, to_sw, tail_sw = pending_switch
                switches.append((t_sw, f_sw, to_sw, max(tail_sw, tail)))
                pending_switch = None
            switched = prev_screen is not None and screen != prev_screen
            if switched:
                pending_switch = (el, prev_screen, screen, tail)
            if new > 0:
                where = f"at the switch {name(prev_screen)} -> {name(screen)}" if switched else f"on {name(screen)}"
                text = (f"DROPPED {new} {where} ({'attract' if attract else 'held'}, {phase} phase): "
                        f"{drop_detail(a.host)}")
                drops.append(text)
                ev.note(text)
            # keep the phase what it should be
            if now >= next_fix:
                try:
                    if phase == "camera" and screen != CAMERA:
                        ev.note(f"camera phase but on {name(screen)} (attract={attract}): back to Camera")
                        next_fix = now + 60
                        send(a.host, ["touch 4 400 4 1", "sleep 0.05", "touch 4 400 4 0", "sleep 0.5",
                                      f"screen {CAMERA}", "sleep 2", "nn hands 3"])
                    elif phase == "attract" and not attract:
                        ev.note(f"attract phase but attract mode is off (on {name(screen)}): starting it")
                        next_fix = now + 60
                        send(a.host, ["nn hands 1", "attract"])
                except OSError:
                    close(a.host)
            prev_screen, prev_frames, prev_dropped = screen, frames, dropped

            if now >= next_min:
                next_min += 60
                try:
                    m, au, mi, nn = (kv(x) for x in send(a.host, ["mem", "audio", "mic", "nn hands"]))
                except OSError:
                    m = au = mi = nn = {}
                ok, ms = status_json(a.host)
                up = num(m, "uptime_s", -1)
                if prev_uptime is not None and 0 <= up < prev_uptime:
                    fatal = f"uptime went back ({prev_uptime:.0f} -> {up:.0f} s): the board restarted"
                    break
                prev_uptime = up if up >= 0 else prev_uptime
                row = {"uptime_s": up, "heap_used": num(m, "heap_used"), "heap_peak": num(m, "heap_peak"),
                       "ui_used": num(m, "ui_used"), "ui_peak": num(m, "ui_peak"), "psram_kb": num(m, "psram_kb"),
                       "stack_min_free": num(m, "stack_min_free"), "audio_errors": num(au, "errors"),
                       "mic_errors": num(mi, "errors"), "t": el}
                mem_rows.append(row)
                mins.writerow([f"{el:.0f}", datetime.now().strftime("%H:%M:%S"), phase, screen, up,
                               m.get("heap_used"), m.get("heap_peak"), m.get("ui_used"), m.get("ui_peak"),
                               m.get("psram_kb"), m.get("stack_min_free"), m.get("stack_min_thread"),
                               au.get("errors"), mi.get("errors"), nn.get("hands"), nn.get("searches"),
                               nn.get("palms"), nn.get("confirmed"), nn.get("lost"), nn.get("palm_us"),
                               nn.get("hand_us"), nn.get("step_us"), int(ok), f"{ms:.0f}"])
                min_f.flush()
                sec_f.flush()
                if not ok:
                    ev.note("status.json did not answer")
                if len(mem_rows) > 1 and row["audio_errors"] > mem_rows[-2]["audio_errors"]:
                    ev.note(f"audio underruns: {row['audio_errors'] - mem_rows[-2]['audio_errors']:.0f} more")
                if companion and companion.poll() is not None:
                    ev.note("the host companion exited: restarting it")
                    companion = subprocess.Popen([sys.executable, str(ROOT / "host" / "companion.py"), "--host",
                                                  a.host], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if now >= next_stacks:
                next_stacks += 600
                try:
                    last_stacks = send(a.host, ["stacks"])[0]
                    first_stacks = first_stacks or last_stacks
                    stacks_f.write(f"{el:.0f} {last_stacks}\n")
                    stacks_f.flush()
                except OSError:
                    pass
            if el >= (hour_reported + 1) * 3600:
                hour_reported += 1
                last = mem_rows[-1] if mem_rows else {}
                ev.note(f"hour {hour_reported} done: dropped so far {prev_dropped - dropped0} in {len(drops)} events, "
                        f"{len(switches)} switches, LVGL pool {last.get('ui_used', 0) / 1024:.1f} KB "
                        f"(peak {last.get('ui_peak', 0) / 1024:.1f}), heap {last.get('heap_used', 0):.0f} B, "
                        f"PSRAM {last.get('psram_kb', 0):.0f} KB, least stack {last.get('stack_min_free', 0):.0f} B, "
                        f"audio underruns {last.get('audio_errors', 0):.0f}, uptime {last.get('uptime_s', 0) / 3600:.2f} h")

            next_tick += 1.0
            time.sleep(max(0.0, next_tick - time.monotonic()))
    except KeyboardInterrupt:
        fatal = "stopped by hand"
    finally:
        con.stop.set()
        if companion:
            companion.terminate()
        sec_f.close()
        min_f.close()
        stacks_f.close()
        summary = summarize(a, el_of(t0), fatal, switches, drops, render, worst, mem_rows, first_stacks,
                            last_stacks, con, name)
        (out / "summary.txt").write_text(summary, encoding="utf-8")
        print(summary, flush=True)
    return 1 if fatal and fatal != "stopped by hand" else 0


def el_of(t0: float) -> float:
    return time.monotonic() - t0


def summarize(a, elapsed, fatal, switches, drops, render, worst, mem_rows, first_stacks, last_stacks, con,
              name) -> str:
    L = [f"Soak test, {datetime.now():%Y-%m-%d %H:%M}: {elapsed / 3600:.2f} h of {a.hours:g} h planned",
         f"Result: {'STOPPED: ' + fatal if fatal else 'ran to the end, no reset, no fault, the board answered throughout'}",
         f"Console: {con.notes} warning or error lines" + (f"; fatal: {con.fatal}" if con.fatal else ""), ""]

    n_dropped = sum(int(re.search(r"DROPPED (\d+)", d).group(1)) for d in drops)
    L.append(f"Dropped frames: {n_dropped} in {len(drops)} events")
    L += [f"  {d}" for d in drops[:60]]
    if len(drops) > 60:
        L.append(f"  ... and {len(drops) - 60} more (events.log)")
    L.append("")

    by_pair: dict[tuple[int, int], list[int]] = defaultdict(list)
    for _, f, t, tail in switches:
        by_pair[(f, t)].append(tail)
    L.append(f"Screen switches: {len(switches)}; the costliest (switch tail, the UI thread's time after a present):")
    for (f, t), tails in sorted(by_pair.items(), key=lambda kv_: -max(kv_[1]))[:12]:
        L.append(f"  {name(f):>14} -> {name(t):<14} {len(tails):4d} x, worst {max(tails) / 1000:5.2f} ms, "
                 f"median {statistics.median(tails) / 1000:5.2f} ms")
    L.append("")

    if mem_rows:
        def med(rows, k):
            return statistics.median(r[k] for r in rows) if rows else float("nan")
        h1 = [r for r in mem_rows if 3600 <= r["t"] < 7200] or mem_rows[: max(1, len(mem_rows) // 4)]
        hl = [r for r in mem_rows if r["t"] >= mem_rows[-1]["t"] - 3600]
        L.append("Memory, median of the second hour against the last hour (the first hour still fills caches):")
        for k, unit, div in (("ui_used", "KB", 1024), ("ui_peak", "KB", 1024), ("heap_used", "B", 1),
                             ("heap_peak", "B", 1), ("psram_kb", "KB", 1), ("stack_min_free", "B", 1)):
            L.append(f"  {k:15s} {med(h1, k) / div:10.1f} -> {med(hl, k) / div:10.1f} {unit}"
                     f"   (min {min(r[k] for r in mem_rows) / div:.1f}, max {max(r[k] for r in mem_rows) / div:.1f})")
        L.append(f"  audio underruns {mem_rows[-1]['audio_errors'] - mem_rows[0]['audio_errors']:.0f}, "
                 f"microphone errors {mem_rows[-1]['mic_errors'] - mem_rows[0]['mic_errors']:.0f}")
        L.append("")
    if first_stacks:
        f = dict(x.rsplit(":", 1) for x in first_stacks.split() if ":" in x)
        l_ = dict(x.rsplit(":", 1) for x in last_stacks.split() if ":" in x)
        L.append("Stack never used, bytes, first -> last scan (a fall means a new high-water mark):")
        for k in sorted(f, key=lambda k: int(l_.get(k, f[k]))):
            L.append(f"  {k:16s} {f[k]:>6} -> {l_.get(k, '?'):>6}")
        L.append("")

    hours = sorted(render)
    if hours:
        h_first, h_last = hours[0], hours[-1]
        L.append(f"Frame time by screen, hour {h_first + 1} against hour {h_last + 1} "
                 "(mean of the 1 s averages; worst frame):")
        keys = sorted(set(render[h_first]) | set(render[h_last]))
        for k in keys:
            s0, s1 = render[h_first].get(k, []), render[h_last].get(k, [])
            label = k[0] + (f" ({k[1]})" if k[1] else "")
            m0 = f"{statistics.mean(s0) / 1000:5.2f}" if s0 else "    -"
            m1 = f"{statistics.mean(s1) / 1000:5.2f}" if s1 else "    -"
            L.append(f"  {label:26s} {m0} -> {m1} ms   worst {worst[h_first].get(k, 0) / 1000:5.2f} -> "
                     f"{worst[h_last].get(k, 0) / 1000:5.2f} ms   ({len(s0)} / {len(s1)} s)")
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    sys.exit(main())
